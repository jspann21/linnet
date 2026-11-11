"""Build the pinned, self-contained Windows FFmpeg and FFprobe distribution."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import time
import tomllib
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = ROOT / 'config/conversion-tools.json'


def _digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _download(spec):
    cache = ROOT / 'out/downloads/conversion-tools'
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / spec['url'].rsplit('/', 1)[-1]
    if target.is_file() and target.stat().st_size == spec['bytes'] and _digest(target) == spec['sha256']:
        return target
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        path = Path(temporary) / target.name
        with urllib.request.urlopen(spec['url'], timeout=30) as source, path.open('xb') as output:
            size = 0
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                if size > spec['bytes']:
                    raise ValueError('Conversion tool download exceeds its expected size.')
                output.write(chunk)
        if size != spec['bytes'] or _digest(path) != spec['sha256']:
            raise ValueError('Conversion tool download checksum mismatch: ' + target.name)
        path.replace(target)
    return target


def _extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as bundle:
            if any(not (destination / name).resolve().is_relative_to(destination.resolve())
                   for name in bundle.namelist()):
                raise ValueError('Conversion tool archive contains an unsafe path.')
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            bundle.extractall(destination, filter='data')


def _bash():
    git = shutil.which('git')
    if git:
        for parent in Path(git).resolve().parents:
            shell = parent / 'usr/bin/bash.exe'
            if shell.is_file():
                return shell
    raise RuntimeError('Install Git for Windows to build the included conversion tools.')


def _run(command, directory, environment, log):
    result = subprocess.run([str(part) for part in command], cwd=directory, env=environment,
                            capture_output=True, text=True, encoding='utf-8', errors='replace',
                            timeout=1200, creationflags=subprocess.CREATE_NO_WINDOW)
    with log.open('a', encoding='utf-8') as output:
        output.write(shlex.join(map(str, command)) + '\n' + result.stdout + result.stderr + '\n')
    if result.returncode:
        raise RuntimeError('Conversion tool build failed; see ' + str(log) + '\n' +
                           (result.stdout + result.stderr)[-3000:])
    return result.stdout


def setup():
    if os.name != 'nt':
        raise ValueError('Build the included conversion tools on Windows.')
    inputs = (SPEC, Path(__file__), ROOT / 'config/toolchain.toml')
    key = hashlib.sha256(b''.join(path.read_bytes() for path in inputs)).hexdigest()[:20]
    parent = ROOT / 'out/conversion-tools'
    destination = parent / key
    if destination.exists():
        try:
            files = json.loads((destination / 'files.json').read_text(encoding='utf-8'))
            valid = isinstance(files, dict) and all(name in files for name in ('ffmpeg.exe', 'ffprobe.exe')) and all(
                not Path(name).is_absolute() and not Path(name).drive and '..' not in Path(name).parts
                and (destination / name).is_file() and _digest(destination / name) == digest
                for name, digest in files.items())
        except (OSError, ValueError, TypeError):
            valid = False
        if not valid:
            raise RuntimeError('Cached conversion tools changed. Remove their out/conversion-tools folder and rebuild.')
        return destination
    spec = json.loads(SPEC.read_text(encoding='utf-8'))
    archives = {name: _download(item) for name, item in spec.items()}
    compiler_spec = tomllib.loads((ROOT / 'config/toolchain.toml').read_text(encoding='utf-8'))['toolchain']
    compiler = Path(os.environ.get('LINNET_LLVM_BIN') or
                    ROOT / 'out/toolchains' / compiler_spec['windows_archive'] / 'bin').resolve()
    missing = [name for name in ('clang.exe', 'mingw32-make.exe') if not (compiler / name).is_file()]
    if missing:
        raise RuntimeError('Conversion-tool compiler is missing ' + ', '.join(missing) +
                           '. Run python linnet setup or check LINNET_LLVM_BIN.')
    shell = _bash()
    parent.mkdir(parents=True, exist_ok=True)
    log = parent / (key + '.log')
    log.write_text('', encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='build-', dir=parent) as temporary:
        work = Path(temporary)
        for name, archive in archives.items():
            _extract(archive, work / 'pkgconf' if name == 'pkgconf' else work)
        prefix = work / 'prefix'
        environment = dict(os.environ)
        (work / 'temp').mkdir()
        environment.update(TEMP=str(work / 'temp'), TMP=str(work / 'temp'))
        environment['PATH'] = os.pathsep.join([str(compiler), str(work / spec['nasm']['directory']),
            str(work / spec['pkgconf']['directory']), str(shell.parent), environment.get('PATH', '')])
        environment.update(CC='clang', AR='llvm-ar', RANLIB='llvm-ranlib', STRIP='llvm-strip',
                           PKG_CONFIG_PATH=str(prefix / 'lib/pkgconfig'))
        make = [compiler / 'mingw32-make.exe', '-j' + str(min(8, os.cpu_count() or 1)),
                'SHELL=' + shell.with_name('sh.exe').as_posix()]
        x264 = work / spec['x264']['directory']
        x264_flags = ['--host=x86_64-w64-mingw32', '--enable-static', '--disable-cli',
                      '--disable-opencl', '--bit-depth=8', '--chroma-format=420', '--prefix=' + prefix.as_posix()]
        _run([shell, '-c', shlex.join(['./configure', *x264_flags])], x264, environment, log)
        _run(make, x264, environment, log)
        _run([*make, 'install'], x264, environment, log)
        ffmpeg = work / spec['ffmpeg']['directory']
        flags = ['--target-os=mingw32', '--arch=x86_64', '--cc=clang', '--cxx=clang++',
            '--ar=llvm-ar', '--ranlib=llvm-ranlib', '--nm=llvm-nm', '--strip=llvm-strip',
            '--disable-autodetect', '--enable-gpl', '--enable-version3', '--enable-libx264',
            '--disable-shared', '--enable-static', '--disable-doc', '--disable-debug',
            '--disable-ffplay', '--disable-network', '--disable-avdevice', '--disable-hwaccels',
            '--pkg-config=pkgconf', '--prefix=' + prefix.as_posix()]
        _run([shell, '-c', shlex.join(['./configure', *flags])], ffmpeg, environment, log)
        _run([*make, 'ffmpeg.exe', 'ffprobe.exe'], ffmpeg, environment, log)
        prepared = work / 'distribution'
        prepared.mkdir()
        versions = []
        for name in ('ffmpeg', 'ffprobe'):
            executable = prepared / (name + '.exe')
            shutil.copy2(ffmpeg / executable.name, executable)
            versions.append(_run([executable, '-version'], prepared, environment, log))
        if any('--enable-nonfree' in version or '--enable-libx264' not in version for version in versions):
            raise RuntimeError('The conversion tool configuration does not match the distributable build.')
        (prepared / 'BUILD.txt').write_text('\n'.join(versions), encoding='utf-8')
        sources = prepared / 'sources'
        sources.mkdir()
        for name in ('ffmpeg', 'x264'):
            shutil.copy2(archives[name], sources / archives[name].name)
        for path in inputs:
            target = sources / path.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        for path in ffmpeg.glob('COPYING*'):
            shutil.copy2(path, prepared / path.name)
        shutil.copy2(ffmpeg / 'LICENSE.md', prepared / 'FFmpeg-LICENSE.md')
        shutil.copy2(x264 / 'COPYING', prepared / 'x264-COPYING')
        (prepared / 'README.txt').write_text(
            'FFmpeg ' + spec['ffmpeg']['version'] + ' and x264 ' + spec['x264']['version'] + '\n'
            'https://ffmpeg.org/  https://www.videolan.org/developers/x264.html\n'
            'Built from unmodified sources under GNU GPL version 3 or later.\n'
            'Copyright the FFmpeg developers and x264 contributors; see the included licenses.\n'
            'Includes software developed by the Independent JPEG Group.\n'
            'Complete corresponding source and build instructions are in sources/.\n'
            'To rebuild on Windows, use Python 3.14 and Git for Windows. Work in sources/.\n'
            'Extract the compiler archive specified by config/toolchain.toml into\n'
            'out/toolchains/<windows_archive>. Copy the included source archives into\n'
            'out/downloads/conversion-tools/, then run python tools/studio/conversion_tools.py.\n'
            'The pinned source archives, configuration and build script are included; no source patches are applied.\n'
            'The separate executables can be replaced with compatible modified builds.\n', encoding='utf-8')
        files = {path.relative_to(prepared).as_posix(): _digest(path)
                 for path in prepared.rglob('*') if path.is_file()}
        (prepared / 'files.json').write_text(json.dumps(files, indent=2) + '\n', encoding='utf-8')
        for attempt in range(7):
            try:
                prepared.rename(destination)
                break
            except OSError as error:
                if destination.is_dir():
                    return setup()
                # Windows scanners can retain a just-checked executable briefly.
                if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 6:
                    raise
                time.sleep(min(0.1 * 2 ** attempt, 1))
    return destination


def distribution(destination):
    shutil.copytree(setup(), destination)


if __name__ == '__main__':
    print(setup())
