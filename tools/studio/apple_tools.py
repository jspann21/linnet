"""Build and locate the native, staged Apple database writers."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SPEC = ROOT / "config/apple-tools.json"
SOURCES = ROOT / "tools/apple"
KINDS = ("music", "photos")


def _run(command, **kwargs):
    environment = dict(kwargs.pop("env", os.environ), PYTHONIOENCODING="utf-8")
    result = subprocess.run([str(arg) for arg in command], capture_output=True,
                            text=True, encoding="utf-8", errors="replace",
                            env=environment,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                            **kwargs)
    if result.returncode:
        raise RuntimeError("Apple database tool failed:\n" + (result.stderr or result.stdout).strip())
    return result.stdout


def _digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _download(url, sha256):
    cache = ROOT / "out/downloads/apple-tools"
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / url.rsplit("/", 1)[-1]
    if target.is_file() and _digest(target) == sha256:
        return target
    with tempfile.TemporaryDirectory(dir=cache) as directory:
        temporary = Path(directory) / target.name
        with urllib.request.urlopen(url, timeout=60) as source, temporary.open("wb") as output:
            size = 0
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                if size > 128 * 1024 * 1024:
                    raise ValueError("Apple tool download exceeds its size limit")
                output.write(chunk)
        if _digest(temporary) != sha256:
            raise ValueError(f"Apple tool download checksum mismatch: {target.name}")
        temporary.replace(target)
    return target


def _extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    # Windows includes bsdtar, including Zstandard support. Using it here also
    # keeps setup compatible with Python versions before tarfile gained Zstd.
    # Inbox tar uses narrow argv on some Windows versions. Feed the archive
    # through stdin and use the Unicode process cwd instead of path arguments.
    with archive.open("rb") as stream:
        names = _run(["tar.exe", "-tf", "-"], stdin=stream).splitlines()
    if any(not (destination / name).resolve().is_relative_to(destination.resolve())
           for name in names):
        raise ValueError("Apple tool archive contains an unsafe path")
    with archive.open("rb") as stream:
        _run(["tar.exe", "-xf", "-"], stdin=stream, cwd=destination)


def _compiler():
    spec = tomllib.loads((ROOT / "config/toolchain.toml").read_text(encoding="utf-8"))["toolchain"]
    directory = Path(os.environ.get("LINNET_LLVM_BIN") or
                     ROOT / "out/toolchains" / spec["windows_archive"] / "bin")
    compiler = directory / "clang.exe"
    if not compiler.is_file():
        raise RuntimeError("Install the Windows compiler first: python linnet setup")
    return compiler


def _port_libgpod(source):
    # libgpod 0.8.3's Unix stat layout and fopen are unsuitable for Windows
    # GLib: use its ABI-correct buffer and UTF-8 filesystem entry points.
    for path in (source / "src").glob("*.c"):
        text = path.read_text(encoding="utf-8")
        text = re.sub(r"\bstruct stat\b", "GStatBuf", text)
        for name in ("fopen", "stat", "unlink", "rename"):
            text = re.sub(r"\b" + name + r"\s*\(", "g_" + name + " (", text)
        text = text.replace("g_memmove (", "memmove (")
        # The casts match Debian's GCC 14 compatibility fix.
        text = text.replace("return g_object_ref (G_OBJECT (pixbuf));",
                            "return (GdkPixbuf *)g_object_ref (G_OBJECT (pixbuf));")
        text = text.replace("pixbuf = g_object_ref (G_OBJECT (thumb_pixbuf->pixbuf));",
                            "pixbuf = (GdkPixbuf *)g_object_ref (G_OBJECT (thumb_pixbuf->pixbuf));")
        if path.name == "ithumb-writer.c":
            # UCRT opens an append stream at position zero until it is explicitly
            # sought/written. libgpod mistakes that for an empty existing ithmb
            # and deletes it, or assigns newly appended thumbnails offset zero.
            text = text.replace(
                "\twriter->cur_offset = ftell (writer->f);",
                "\tif (_fseeki64 (writer->f, 0, SEEK_END) != 0) return FALSE;\n"
                "\twriter->cur_offset = _ftelli64 (writer->f);\n"
                "\tif (writer->cur_offset < 0) return FALSE;")
            # Repacking thumbnails uses binary bytes and UTF-8 host paths.
            # Existing FAT32 thumbnail files may exceed Windows' 32-bit off_t.
            text = text.replace("off_t cur_offset;", "gint64 cur_offset;")
            text = text.replace("lseek (fd,", "_lseeki64 (fd,")
            text = text.replace("ftruncate (fd, offset) == -1", "_chsize_s (fd, offset) != 0")
            text = text.replace("open (filename, O_RDWR, 0)",
                                "g_open (filename, O_RDWR | O_BINARY, 0)")
        if path.name == "itdb_artwork.c":
            text = text.replace('g_fopen (filename, "r")', 'g_fopen (filename, "rb")')
        if path.name == "itdb_device.c":
            text = text.replace(
                "    if (GetDiskFreeSpaceEx(device->mountpoint, &u_free, &u_capacity, NULL) == 0) {",
                "    gunichar2 *wide = g_utf8_to_utf16(device->mountpoint, -1, NULL, NULL, NULL);\n"
                "    if (!wide) return FALSE;\n"
                "    BOOL ok = GetDiskFreeSpaceExW(wide, &u_free, &u_capacity, NULL);\n"
                "    g_free(wide);\n"
                "    if (!ok) {")
        path.write_text("/* Linnet: Windows GLib/UTF-8 port; see apple_tools.py. */\n" + text,
                        encoding="utf-8")


def _build_pixbuf(compiler, sdk, source, work, output):
    build = work / "pixbuf-build"
    include = build / "gdk-pixbuf"
    include.mkdir(parents=True)
    (build / "config.h").write_text(
        '#define HAVE_UNISTD_H 1\n#define HAVE_STDINT_H 1\n#define HAVE_ROUND 1\n'
        '#define HAVE_LRINT 1\n#define DLL_EXPORT 1\n'
        '#define _GDK_PIXBUF_EXTERN __declspec(dllexport) extern\n'
        '#define GETTEXT_PACKAGE "gdk-pixbuf"\n', encoding="utf-8")
    for name in ("gdk-pixbuf-features.h", "gdk-pixbuf-enum-types.h", "gdk-pixbuf-marshal.h"):
        shutil.copy2(sdk / "include/gdk-pixbuf-2.0/gdk-pixbuf" / name, include / name)
    headers = ("gdk-pixbuf.h", "gdk-pixbuf-animation.h", "gdk-pixbuf-core.h", "gdk-pixbuf-io.h",
               "gdk-pixbuf-loader.h", "gdk-pixbuf-macros.h", "gdk-pixbuf-simple-anim.h",
               "gdk-pixbuf-transform.h")
    code = _run([sys.executable, sdk / "bin/glib-mkenums-script.py", "--template",
                 source / "gdk-pixbuf/gdk-pixbuf-enum-types.c.template",
                 *(source / "gdk-pixbuf" / name for name in headers)])
    (include / "gdk-pixbuf-enum-types.c").write_text(code, encoding="utf-8")
    code = _run([sys.executable, sdk / "bin/glib-genmarshal-script.py", "--body",
                 "--prefix=_gdk_pixbuf_marshal", source / "gdk-pixbuf/gdk-pixbuf-marshal.list"])
    (include / "gdk-pixbuf-marshal.c").write_text(code, encoding="utf-8")
    names = ("gdk-pixbuf", "gdk-pixbuf-animation", "gdk-pixbuf-data", "gdk-pixbuf-io",
             "gdk-pixbuf-loader", "gdk-pixbuf-scale", "gdk-pixbuf-simple-anim",
             "gdk-pixbuf-scaled-anim", "gdk-pixbuf-util", "gdk-pixdata", "io-png", "pixops/pixops")
    # Pillow normalizes input to PNG. Compile only the PNG loader, built in:
    # no plugin cache, global installation, or extra TIFF/JPEG dependencies.
    command = [compiler, "-shared", "-O2", "-std=gnu17", "-DHAVE_CONFIG_H",
               "-DGDK_PIXBUF_COMPILATION", "-DGDK_PIXBUF_ENABLE_BACKEND", "-DINCLUDE_png",
               '-DGDK_PIXBUF_PREFIX="."', '-DGDK_PIXBUF_LOCALEDIR="."',
               '-DGDK_PIXBUF_LIBDIR="."', '-DGDK_PIXBUF_BINARY_VERSION="2.10.0"',
               '-DPIXBUF_LIBDIR="."', '-DBUILT_MODULES_DIR="."', "-Wno-deprecated-declarations"]
    command += ["-I" + str(path) for path in (build, include, source, source / "gdk-pixbuf",
                source / "gdk-pixbuf/pixops", sdk / "include", sdk / "include/glib-2.0",
                sdk / "lib/glib-2.0/include")]
    command += [source / "gdk-pixbuf" / (name + ".c") for name in names]
    command += [include / "gdk-pixbuf-enum-types.c", include / "gdk-pixbuf-marshal.c",
                "-L" + str(sdk / "lib"), "-lglib-2.0", "-lgobject-2.0", "-lgio-2.0",
                "-lintl", "-lpng16", "-o", output / "libgdk_pixbuf-2.0-0.dll"]
    _run(command, timeout=300)


def _windows_build(work, output):
    compiler = _compiler()
    spec = json.loads(SPEC.read_text(encoding="utf-8"))
    with ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(lambda row: _download(row["url"], row["sha256"]), spec["packages"]))
    sdk = work / "sdk/ucrt64"
    for archive in archives:
        _extract(archive, sdk.parent)
    _extract(_download(**spec["libgpod"]), work)
    source = work / "libgpod-0.8.3"
    _port_libgpod(source)
    row = next(row for row in spec["packages"] if row["name"] == "gdk-pixbuf2")
    _extract(_download(row["source_url"], row["source_sha256"]), work)
    _extract(work / "mingw-w64-gdk-pixbuf2/gdk-pixbuf-2.44.7.tar.xz", work)
    _build_pixbuf(compiler, sdk, work / "gdk-pixbuf-2.44.7", work, output)
    build = work / "libgpod-build"
    (build / "gpod").mkdir(parents=True)
    shutil.copy2(source / "src/itdb.h", build / "gpod/itdb.h")
    macros = ("HAVE_CONFIG_H", "HAVE_UNISTD_H", "HAVE_GDKPIXBUF", "HAVE_LIBXML", "HAVE_ZLIB",
              "HAVE_G_INT64_EQUAL", "HAVE_G_INT64_HASH", "HAVE_G_MAPPED_FILE_UNREF",
              "HAVE_G_CHECKSUM_RESET", "WIN32")
    (build / "config.h").write_text("".join(f"#define {name} 1\n" for name in macros) +
        '#define GETTEXT_PACKAGE "libgpod"\n#define LIBGPOD_BLOB_DIR "disabled"\n', encoding="utf-8")
    flags = ["-O2", "-std=gnu17", "-DHAVE_CONFIG_H", "-Wno-deprecated-declarations",
             "-include", "glib/gstdio.h"]
    flags += ["-I" + str(path) for path in (build, source / "src", sdk / "include",
              sdk / "include/glib-2.0", sdk / "lib/glib-2.0/include",
              sdk / "include/gdk-pixbuf-2.0", sdk / "include/libxml2")]
    _run([compiler, "-shared", *flags, *sorted((source / "src").glob("*.c")),
          "-L" + str(sdk / "lib"), *["-l" + name for name in
              ("glib-2.0", "gobject-2.0", "gmodule-2.0", "gdk_pixbuf-2.0", "xml2",
               "plist-2.0", "sqlite3", "z", "intl")], "-o", output / "libgpod.dll",
          "-Wl,--out-implib," + str(build / "libgpod.dll.a")], timeout=300)
    for kind in KINDS:
        _run([compiler, *flags, SOURCES / f"managed_{kind}.c", "-L" + str(build),
              "-L" + str(sdk / "lib"), "-lgpod", "-lglib-2.0", "-o",
              output / f"managed-apple-{kind}.exe"], timeout=180)
    # Resolve the complete import closure, never relying on the developer's PATH.
    known = {path.name.lower(): path for path in (sdk / "bin").glob("*.dll")}
    known.update({path.name.lower(): path for path in output.glob("*.dll")})
    pending = list(output.glob("*.exe"))
    seen = set()
    system = Path(os.environ["SystemRoot"]) / "System32"
    while pending:
        imports = _run([compiler.parent / "llvm-objdump.exe", "-p", pending.pop()])
        for name in re.findall(r"DLL Name: (.+)", imports):
            name = name.lower()
            if name in seen:
                continue
            seen.add(name)
            if name in known:
                path = known[name]
                pending.append(path)
                if path.parent != output:
                    shutil.copy2(path, output / path.name)
            elif not (name.startswith("api-ms-") or (system / name).is_file()):
                raise RuntimeError(f"Missing Apple database runtime library: {name}")
    shutil.copytree(sdk / "share/licenses", output / "licenses")
    shutil.copy2(source / "COPYING", output / "licenses/libgpod-COPYING")
    # Include the modified LGPL source and actual generated build configuration.
    with tarfile.open(output / "libgpod-source.tar.gz", "w:gz") as archive:
        archive.add(source, arcname=source.name)
        archive.add(build / "config.h", arcname="libgpod-windows-config.h")


def _fingerprint():
    digest = hashlib.sha256()
    for path in (Path(__file__), SPEC, ROOT / "config/toolchain.toml",
                 SOURCES / "managed_io.h", *(SOURCES / f"managed_{kind}.c" for kind in KINDS)):
        digest.update(path.read_bytes())
    return digest.hexdigest()[:24]


def setup():
    """Build both writers once; published bundles already contain these files."""
    if os.name != "nt":
        raise ValueError("Windows Apple tools must be built on Windows.")
    parent = ROOT / "out/apple-tools"
    output = parent / _fingerprint()
    receipt = output / "files.json"
    if output.exists() or output.is_symlink():
        try:
            files = json.loads(receipt.read_text(encoding="utf-8"))
            required = {f"managed-apple-{kind}.exe" for kind in KINDS} | {
                "libgpod.dll", "libgdk_pixbuf-2.0-0.dll",
                "libgpod-source.tar.gz", "licenses/libgpod-COPYING"}
            valid = (isinstance(files, dict) and required <= files.keys() and
                     all(isinstance(digest, str) and not Path(name).is_absolute() and
                         not Path(name).drive and ".." not in Path(name).parts and
                         (output / name).is_file() and _digest(output / name) == digest
                         for name, digest in files.items()))
        except (OSError, ValueError):
            valid = False
        if valid:
            return output
        raise RuntimeError("Cached Apple tools were modified; remove that out/apple-tools folder and run setup again.")
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="build-", dir=parent) as temporary:
        work = Path(temporary)
        prepared = work / "runtime"
        prepared.mkdir()
        _windows_build(work, prepared)
        files = {path.relative_to(prepared).as_posix(): _digest(path)
                 for path in prepared.rglob("*") if path.is_file()}
        (prepared / "files.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")
        try:
            prepared.rename(output)
        except OSError:
            # Windows can report a competing directory publication as access denied.
            if not output.exists() and not output.is_symlink():
                raise
            return setup()
    return output


def writer(kind, cache):
    if kind not in KINDS:
        raise ValueError("Unknown Apple database writer")
    if getattr(sys, "frozen", False):
        output = ROOT / "apple-tools" / f"managed-apple-{kind}.exe"
        if not output.is_file():
            raise RuntimeError("Apple database tools are missing. Extract the complete Media Manager release.")
        return output
    if os.name == "nt":
        return setup() / f"managed-apple-{kind}.exe"
    output = cache / f"managed-apple-{kind}"
    source = SOURCES / f"managed_{kind}.c"
    if not output.is_file() or output.stat().st_mtime_ns < max(
            source.stat().st_mtime_ns, (SOURCES / "managed_io.h").stat().st_mtime_ns):
        flags = shlex.split(_run(["pkg-config", "--cflags", "--libs", "libgpod-1.0"]))
        with tempfile.TemporaryDirectory(dir=cache) as directory:
            temporary = Path(directory) / output.name
            _run([os.environ.get("CC", "cc"), source, *flags, "-o", temporary], timeout=180)
            temporary.replace(output)
    return output


def distribution(destination):
    """Include replaceable DLLs, license notices and corresponding sources."""
    shutil.copytree(setup(), destination)
    source_dir = destination / "sources"
    source_dir.mkdir()
    spec = json.loads(SPEC.read_text(encoding="utf-8"))
    with ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(lambda row: _download(row["source_url"], row["source_sha256"]),
                                 spec["packages"]))
    archives.append(_download(**spec["libgpod"]))
    for archive in archives:
        shutil.copy2(archive, source_dir / archive.name)
    for path in (Path(__file__), SPEC, ROOT / "config/toolchain.toml", SOURCES / "managed_io.h",
                 *(SOURCES / f"managed_{kind}.c" for kind in KINDS)):
        target = source_dir / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


if __name__ == "__main__":
    print(json.dumps({"apple_tools": str(setup())}))
