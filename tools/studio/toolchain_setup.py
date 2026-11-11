"""Install the pinned, portable Windows compiler without administrator access."""
import hashlib
import os
from pathlib import Path
import tempfile
import tomllib
import urllib.request
import zipfile

from tools.studio.targets import ROOT


def tool_directory(spec):
    override = os.environ.get("LINNET_LLVM_BIN")
    if override:
        return Path(override).resolve()
    if os.name == "nt":
        directory = ROOT / "out/toolchains" / spec["windows_archive"] / "bin"
        if directory.is_dir():
            return directory
    return None


def setup():
    spec = tomllib.loads((ROOT / "config/toolchain.toml").read_text(encoding="utf-8"))["toolchain"]
    if os.name != "nt":
        raise ValueError(f"Install LLVM {spec['version']} through your OS, then run python3 linnet doctor.")
    parent = ROOT / "out/toolchains"
    destination = parent / spec["windows_archive"]
    if not destination.exists():
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="download-", dir=parent) as temporary:
            staging = Path(temporary)
            archive = staging / "compiler.zip"
            digest = hashlib.sha256()
            with urllib.request.urlopen(spec["windows_url"], timeout=60) as source, archive.open("wb") as output:
                size = 0
                while chunk := source.read(1024 * 1024):
                    size += len(chunk)
                    if size > 200 * 1024 * 1024:
                        raise ValueError("compiler download exceeds its size limit")
                    digest.update(chunk)
                    output.write(chunk)
            if digest.hexdigest() != spec["windows_sha256"]:
                raise ValueError("compiler download checksum mismatch")
            with zipfile.ZipFile(archive) as bundle:
                for item in bundle.infolist():
                    path = (staging / item.filename).resolve()
                    if not path.is_relative_to(staging / spec["windows_archive"]):
                        raise ValueError("compiler archive contains an unexpected path")
                bundle.extractall(staging)
            (staging / spec["windows_archive"]).rename(destination)
    from tools.studio.build import toolchain
    from tools.studio.apple_tools import setup as setup_apple_tools
    return dict(directory=str(destination), toolchain=toolchain(),
                apple_tools=str(setup_apple_tools()))
