"""Prepare the pinned Inno compiler in portable mode for Windows packaging."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.request

from tools.studio.targets import ROOT


def compiler():
    if os.name != 'nt':
        raise ValueError('Build the Windows installer on Windows.')
    spec = json.loads((ROOT / 'config/inno-setup.json').read_text(encoding='utf-8'))
    parent = ROOT / 'out/toolchains'
    destination = parent / ('inno-setup-' + spec['version'])
    if destination.exists():
        try:
            files = json.loads((destination / 'files.json').read_text(encoding='utf-8'))
            valid = isinstance(files, dict) and 'ISCC.exe' in files and all(
                not Path(name).is_absolute() and not Path(name).drive and '..' not in Path(name).parts
                and (destination / name).is_file()
                and hashlib.sha256((destination / name).read_bytes()).hexdigest() == digest
                for name, digest in files.items())
        except (OSError, ValueError):
            valid = False
        if not valid:
            raise RuntimeError('Cached Inno compiler was modified; remove its out/toolchains folder and rebuild.')
        return destination / 'ISCC.exe'
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='inno-', dir=parent) as temporary:
        work = Path(temporary)
        download = work / 'inno-setup.exe'
        digest = hashlib.sha256()
        size = 0
        with urllib.request.urlopen(spec['url'], timeout=30) as source, download.open('xb') as writer:
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                if size > spec['bytes']:
                    raise ValueError('Inno compiler download exceeds its expected size.')
                digest.update(chunk); writer.write(chunk)
        if size != spec['bytes'] or digest.hexdigest() != spec['sha256']:
            raise ValueError('Inno compiler download checksum mismatch.')
        prepared = work / 'compiler'
        result = subprocess.run([str(download), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
            '/SP-', '/CURRENTUSER', '/PORTABLE=1', '/NOICONS', '/TASKS=', '/DIR=' + str(prepared)],
            capture_output=True, text=True, timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode or not (prepared / 'ISCC.exe').is_file():
            raise RuntimeError(f'Inno compiler setup failed ({result.returncode}).')
        files = {path.relative_to(prepared).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in prepared.rglob('*') if path.is_file()}
        (prepared / 'files.json').write_text(json.dumps(files, indent=2) + '\n', encoding='utf-8')
        try:
            prepared.rename(destination)
        except OSError:
            if not destination.is_dir():
                raise
            return compiler()
    return destination / 'ISCC.exe'
