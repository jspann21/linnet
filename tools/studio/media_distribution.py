"""Package the Windows Media Manager with its Python runtime and dependencies."""
import os
import hashlib
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import zipfile

from tools.studio.targets import ROOT
from tools.studio.media_identity import APP_USER_MODEL_ID, ICON_PATH


def _publish(source, output):
    """Stage beside the destination so publication works across volumes."""
    with tempfile.TemporaryDirectory(prefix='.linnet-package-', dir=output.parent) as temporary:
        staged = Path(temporary) / output.name
        shutil.copyfile(source, staged)
        with source.open('rb') as original, staged.open('rb') as copied:
            if hashlib.file_digest(original, 'sha256').digest() != hashlib.file_digest(copied, 'sha256').digest():
                raise OSError('Package copy failed verification.')
        # Windows rename refuses to replace an output created by another build.
        staged.rename(output)


def create(output, installer_output=None):
    if os.name != "nt":
        raise ValueError("Build the Windows Media Manager on Windows.")
    output = Path(output).resolve()
    if output.exists():
        raise ValueError("Media Manager output already exists")
    if output.suffix.lower() != '.zip':
        raise ValueError('The portable Media Manager output must be a ZIP.')
    version = tomllib.loads((ROOT / 'config/system.toml').read_text(encoding='utf-8'))['system']['version']
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+][A-Za-z0-9.-]+)?', version) if isinstance(version, str) else None
    if match is None or any(int(part) > 65535 for part in match.groups()):
        raise ValueError('The system version is not a supported Windows application version.')
    windows_version = tuple(int(part) for part in match.groups()) + (0,)
    if not all(path.is_file() for path in (ICON_PATH,
            ICON_PATH.with_name('linnet-icon.png'), ICON_PATH.with_name('linnet-setup.png'))):
        raise ValueError('Generate the app icon with: python -m tools.studio.media_icon')
    iscc = None
    if installer_output is not None:
        installer_output = Path(installer_output).resolve()
        if installer_output.suffix.lower() != '.exe' or installer_output.exists():
            raise ValueError('Choose a new EXE path for the Windows installer.')
        from tools.studio.inno_setup import compiler
        iscc = compiler()
        installer_output.parent.mkdir(parents=True, exist_ok=True)
    try:
        import PyInstaller  # noqa: F401
    except ImportError as error:
        raise RuntimeError("Install packaging support with: python -m pip install pyinstaller==6.22.3") from error
    output.parent.mkdir(parents=True, exist_ok=True)
    (ROOT / "out").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="media-package-", dir=ROOT / "out") as temporary:
        work = Path(temporary)
        version_resource = work / 'version.txt'
        strings = dict(CompanyName='Linnet contributors', FileDescription='Linnet Media Manager',
            FileVersion=version, InternalName='Linnet Media',
            LegalCopyright='Copyright (C) 2026 jspann21 and Linnet contributors.',
            OriginalFilename='Linnet Media.exe', ProductName='Linnet', ProductVersion=version)
        version_resource.write_text(
            'VSVersionInfo(ffi=FixedFileInfo(filevers=' + repr(windows_version) +
            ', prodvers=' + repr(windows_version) + ', mask=0x3f, flags=0, OS=0x40004, '
            'fileType=0x1, subtype=0, date=(0, 0)), kids=[StringFileInfo([StringTable('
            "'040904B0', [" + ', '.join('StringStruct(' + repr(key) + ', ' + repr(value) + ')'
                for key, value in strings.items()) + "])]), VarFileInfo([VarStruct('Translation', [1033, 1200])])])",
            encoding='utf-8')
        command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
                   "--onedir", "--name", "Linnet Media", "--paths", str(ROOT),
                   "--distpath", str(work / "dist"), "--workpath", str(work / "build"),
                   "--specpath", str(work), "--collect-all", "lz4", "--icon", str(ICON_PATH),
                   '--version-file', str(version_resource)]
        for package in ("mutagen", "Pillow", "pyserial", "lz4", "PySide6",
                        "PySide6_Essentials", "PySide6_Addons", "shiboken6"):
            command += ["--copy-metadata", package]
        from tools.content.apple_font import PACKAGE_DIRECTORIES, check_packaged_assets
        check_packaged_assets(ROOT)
        for name in PACKAGE_DIRECTORIES:
            command += ["--add-data", str(ROOT / name) + os.pathsep + name]
        from tools.studio.game_build import build_library,library_files
        from tools.studio.targets import TARGETS
        prepared_games=work/'prepared-games'
        for target,spec in TARGETS.items():
            if not spec['capabilities'].get('games',False):continue
            built=ROOT/'out/game-packages'/target;build_library(target,built)
            destination=prepared_games/target;destination.mkdir(parents=True)
            for path in library_files(built,target):
                shutil.copy2(path,destination/path.name)
        command += ['--add-data',str(prepared_games)+os.pathsep+'resources/games']
        command += ["--add-data", str(ROOT / "third_party/ipod_models") + os.pathsep + "third_party/ipod_models"]
        # Photo cache recipes inspect their rendering functions' source.
        command += ["--add-data", str(ROOT / "tools/content/photos.py") + os.pathsep + "tools/content",
                    "--add-data", str(ROOT / "tools/studio/media_desktop.qss") + os.pathsep + "tools/studio",
                    str(ROOT / "tools/studio/media_entry.py")]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                                creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise RuntimeError("Media Manager packaging failed:\n" + result.stdout + result.stderr)
        # Qt on Windows imports the OS's unsuffixed ICU API. PyInstaller can
        # pick up an unrelated ICU build from PATH, whose versioned exports
        # break QtCore at startup. Let Windows supply its own ICU libraries.
        for library in (work / "dist/Linnet Media/_internal").glob("icu*.dll"):
            library.unlink()
        from tools.studio.apple_tools import distribution
        distribution(work / "dist/Linnet Media/_internal/apple-tools")
        from tools.studio.conversion_tools import distribution as conversion_distribution
        conversion_distribution(work / 'dist/Linnet Media/_internal/conversion-tools')
        from tools.content.aac_index import executable as aac_executable
        shutil.copy2(aac_executable(), work / 'dist/Linnet Media/_internal/aac-index.exe')
        from tools.content.mp3_index import executable as mp3_executable
        shutil.copy2(mp3_executable(), work / 'dist/Linnet Media/_internal/mp3-index.exe')
        from tools.content.ipvf_host import library_path as ipvf_host_library
        shutil.copy2(ipvf_host_library(), work / 'dist/Linnet Media/_internal/ipvf-host.dll')
        app = work / "dist/Linnet Media"
        shutil.copy2(ROOT / "docs/releases.md", app / "README.md")
        # Retain codec notices beside the native AAC/MP3 preparation helpers.
        # The matching complete source is distributed as a separate release asset.
        for path in (ROOT / "third_party").rglob("*"):
            if path.is_file() and (path.name.startswith(("LICENSE", "COPYING", "NOTICE", "PROVENANCE"))
                                   or "licenses" in path.parts or path.name.startswith("README")):
                destination = app / "notices" / path.relative_to(ROOT)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
        if (ROOT / "LICENSE").is_file():
            shutil.copy2(ROOT / "LICENSE", app / "LICENSE")
        shutil.copy2(ROOT / 'NOTICE.txt', app / 'NOTICE.txt')
        installer = None
        if installer_output is not None:
            installer_dir = work / 'installer'
            command = [str(iscc), '/Qp', '/DPackageDir=' + str(app),
                '/DOutputDir=' + str(installer_dir), '/DOutputName=' + installer_output.stem,
                '/DAppVersion=' + version, '/DAppUserModelID=' + APP_USER_MODEL_ID,
                '/DAppVersionNumeric=' + '.'.join(str(part) for part in windows_version),
                str(ROOT / 'tools/studio/linnet_setup.iss')]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                encoding='utf-8', timeout=300, creationflags=subprocess.CREATE_NO_WINDOW)
            installer = installer_dir / installer_output.name
            if result.returncode or not installer.is_file():
                raise RuntimeError('Windows installer compilation failed:\n' + result.stdout + result.stderr)
        archive = work / "media.zip"
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted((work / "dist").rglob("*")):
                if path.is_file():
                    bundle.write(path, path.relative_to(work / "dist").as_posix())
        _publish(archive, output)
        if installer is not None:
            _publish(installer, installer_output)
    return dict(archive=str(output), executable="Linnet Media/Linnet Media.exe",
                **(dict(installer=str(installer_output)) if installer_output is not None else {}))
