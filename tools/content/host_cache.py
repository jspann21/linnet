"""Manage regenerable media data without moving or deleting recovery records."""
from contextlib import contextmanager
from functools import lru_cache
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import threading
import time
import uuid

from .host_state import file_stamp, load_json_object, media_state_root, save_json, storage_root


_MARKER = '.linnet-media-cache.json'
_OWNER = {'format': 'linnet-media-cache', 'version': 1}
_lock = threading.RLock()
_local = threading.local()


def default_base():
    return storage_root() / 'Cache' / 'Media'


def _config_path():
    return storage_root() / 'media-cache.json'


def _plain(path):
    from .guard import guarded_path
    path = Path(os.path.abspath(path))
    guarded_path(path, Path(path.anchor))
    return path


def _location(path, device_roots=()):
    path = _plain(path)
    protected_roots = (media_state_root(), storage_root() / 'Installer', storage_root() / 'Firmware')
    for protected in (*protected_roots, *(Path(p) for p in device_roots)):
        if path.is_relative_to(protected) or protected.is_relative_to(path):
            raise ValueError('Cache storage must be separate from iPods, firmware and recovery folders.')
    if path == Path(path.anchor):
        raise ValueError('Choose a dedicated cache folder, not the root of a drive.')
    if os.name == 'nt':
        import ctypes
        if ctypes.windll.kernel32.GetDriveTypeW(str(path.anchor)) != 3:
            raise ValueError('Choose a local computer drive for the cache.')
        if any((Path(path.anchor) / name).exists() for name in ('iPod_Control', '.LINNET')):
            raise ValueError('The cache cannot be stored on an iPod.')
    return path


def _owned(path):
    path = _location(path)
    marker = _plain(path / _MARKER)
    if path != default_base() and load_json_object(marker) != _OWNER:
        raise ValueError(f'This is not a Linnet cache folder: {path}')
    return path


@lru_cache(maxsize=8)
def _configured_base(config_path, stamp):
    config = load_json_object(Path(config_path))
    value = config.get('location')
    if value is not None and (not isinstance(value, str) or not Path(value).is_absolute()):
        raise ValueError('The saved media cache location is invalid. Choose a new location in Settings.')
    return _owned(Path(value)) if value else _plain(default_base())


def cache_base():
    path = _config_path()
    try:
        stamp = file_stamp(path.stat())
    except FileNotFoundError:
        stamp = None
    return _configured_base(str(path), stamp)


@contextmanager
def activity():
    """Serialize maintenance with Media Manager operations across processes."""
    if not _lock.acquire(blocking=False):
        raise RuntimeError('Another media operation is using computer storage. Try again when it finishes.')
    try:
        if getattr(_local, 'active', False):
            yield
            return
        path = _plain(storage_root() / 'media-cache.lock')
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a+b') as handle:
            if not path.stat().st_size:
                handle.write(b'\0'); handle.flush()
            handle.seek(0)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise RuntimeError('Another Media Manager operation is using computer storage. '
                                   'Try again when it finishes.') from error
            _local.active = True
            try:
                yield
            finally:
                _local.active = False
                # Closing the private handle releases its OS lock, including
                # on failure. Nested calls keep using the outer handle.
    finally:
        _lock.release()


# Only producer-owned reusable preview and pack data qualify.
# Unknown files, journals, originals and backup directories never match.
_DEVICE, _APPLE, _HASH, _SHORT = r'[0-9a-f]{24}', r'apple-os/[0-9a-f]{32}', r'[0-9a-f]{64}', r'[0-9A-F]{8}'
_DISPOSABLE = re.compile('|'.join((
    rf'album-art/{_HASH}\.(png|json)',
    rf'retail/tracks-{_DEVICE}\.json',
    rf'retail/images-v2-{_HASH}/{_HASH}\.png',
    rf'{_APPLE}/apple-(library-verified|photo-verified|video-probes)\.json',
    rf'{_APPLE}/(artwork-sources|apple-photo-normalized)/(index\.json|{_HASH}\.(blob|png))',
    rf'{_DEVICE}/music-metadata\.sqlite3(-wal|-shm|-journal)?',
    rf'{_DEVICE}/(seek-checked\.json|verified-outputs\.json|photos\.(pimg|pgr)|notes\.pnot|files\.fil)',
    rf'{_DEVICE}/videos/{_SHORT}-[0-9a-f]{{16}}\.(mp4|ipvf)',
    rf'{_DEVICE}/photo-cache/(source-hashes\.json|{_HASH}\.bin)',
    rf'{_DEVICE}/file-previews/{_HASH}\.(FIM|TXT)',
    rf'{_DEVICE}/file-previews/packs/(layout\.json|{_HASH}\.FPK)',
    rf'{_DEVICE}/PACKS/{_SHORT}/{_SHORT}\.PAK',
    rf'{_DEVICE}/music/(LIBRARY\.PDB|BROWSE\.BIN|COVERS\.CFL|music-manifest\.json|ART/{_SHORT}\.RGB)',
)))


def _disposable(relative):
    return _DISPOSABLE.fullmatch(relative.as_posix()) is not None


def _roots(*, errors=None):
    config = load_json_object(_config_path())
    previous = config.get('previous', [])
    if not isinstance(previous, list) or any(not isinstance(p, str) or not Path(p).is_absolute() for p in previous):
        raise ValueError('The saved cache folder history is invalid.')
    roots = []
    try:
        roots.insert(0, _owned(cache_base()))
    except (OSError, ValueError) as error:
        if errors is None:
            raise
        errors.append(str(error))
    for value in previous:
        try:
            roots.append(_owned(Path(value)))
        except (OSError, ValueError) as error:
            if errors is not None:
                errors.append(str(error))
            # A retired cache may have been removed by the user. Never follow
            # a replacement directory that no longer has our ownership marker.
    return list(dict.fromkeys(roots))


def _inventory(root, errors, *, include_identity=True):
    root = _plain(root)
    pending = [root]
    while pending:
        folder = pending.pop()
        try:
            _plain(folder)
            with os.scandir(folder) as entries:
                for entry in entries:
                    # Size-only scans reuse directory metadata. Mutations need
                    # complete file identities to detect replacements later.
                    path = Path(entry.path)
                    info = path.lstat() if include_identity else entry.stat(follow_symlinks=False)
                    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                        errors.append(f'Link retained: {entry.path}')
                    elif stat.S_ISDIR(info.st_mode):
                        pending.append(path)
                    elif stat.S_ISREG(info.st_mode):
                        yield path, info
        except FileNotFoundError:
            continue
        except OSError as error:
            errors.append(f'{folder}: {error}')


def overview():
    with activity():
        errors, reusable, retained, startup, files = [], 0, 0, 0, 0
        for root in _roots(errors=errors):
            for path, info in _inventory(root, errors, include_identity=False):
                if _disposable(path.relative_to(root)):
                    reusable += info.st_size
                    files += 1
                elif path.name != _MARKER:
                    retained += info.st_size
        for _, info in _inventory(media_state_root(), errors, include_identity=False):
            retained += info.st_size
        installer = storage_root() / 'Installer'
        for _, info in _inventory(installer, errors, include_identity=False):
            startup += info.st_size
        return dict(location=str(cache_base()), bytes=reusable, files=files,
                    retained_bytes=retained, startup_bytes=startup,
                    recovery_location=str(media_state_root()), startup_location=str(installer), errors=errors)


def _unchanged(path, info):
    return file_stamp(_plain(path).lstat()) == file_stamp(info)


def _prune(path, root):
    while path != root and path.is_relative_to(root):
        try:
            _plain(path).rmdir()
        except OSError:
            break
        path = path.parent


def clear():
    with activity():
        errors, freed = [], 0
        for root in _roots():
            for path, info in _inventory(root, errors):
                if not _disposable(path.relative_to(root)):
                    continue
                try:
                    if not _unchanged(path, info):
                        raise OSError('File changed during cleanup; retained')
                    path.unlink()
                    freed += info.st_size
                    _prune(path.parent, root)
                except (OSError, ValueError) as error:
                    errors.append(f'{path}: {error}')
        result = overview()
        result.update(freed_bytes=freed, errors=list(dict.fromkeys(errors + result['errors'])))
        return result


def _destination(path, device_roots=(), *, roots=()):
    path = _location(path, device_roots)
    for root in roots:
        if path != root and (path.is_relative_to(root) or root.is_relative_to(path)):
            raise ValueError('Choose a cache folder outside the existing cache folders.')
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        _owned(path)
    path.mkdir(parents=True, exist_ok=True)
    save_json(path / _MARKER, _OWNER)
    return path


def _copy(source, destination, info):
    _plain(destination)
    if destination.exists():
        return False
    _plain(destination.parent).mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name('.' + uuid.uuid4().hex + '.tmp')
    digest = hashlib.sha256()
    try:
        with _plain(source).open('rb') as reader, temporary.open('xb') as writer:
            while data := reader.read(1024 * 1024):
                digest.update(data); writer.write(data)
            # Read back every copy, but avoid a disk flush for every thumbnail.
            # These bytes are regenerable; the settings switch is fsynced.
            writer.flush()
        if not _unchanged(source, info) or temporary.stat().st_size != info.st_size:
            raise OSError('Cache file changed while copying; original retained')
        with temporary.open('rb') as reader:
            if hashlib.file_digest(reader, 'sha256').digest() != digest.digest():
                raise OSError('Cache copy failed verification; original retained')
        shutil.copystat(source, temporary)
        _plain(destination)
        if destination.exists():
            # Prefer a cache already produced at the destination. Neither
            # receipt paths nor unknown contents are overwritten.
            return False
        temporary.replace(destination)
        return True
    finally:
        temporary.unlink(missing_ok=True)


def relocate(destination, *, device_roots=(), progress=lambda message: None):
    with activity():
        errors, copied = [], []
        roots = _roots(errors=errors)
        destination = _destination(destination, device_roots, roots=roots)
        total = sum(info.st_size for root in roots if root != destination
                    for path, info in _inventory(root, errors) if _disposable(path.relative_to(root)))
        if shutil.disk_usage(destination).free < total + 16 * 1024 ** 2:
            raise ValueError('Not enough computer space to verify the cache at the new location.')
        last_progress = 0
        for root in roots:
            if root == destination:
                continue
            for path, info in _inventory(root, errors):
                relative = path.relative_to(root)
                if not _disposable(relative):
                    continue
                if time.monotonic() - last_progress >= 0.2:
                    progress(f'Moving reusable cache files: {relative}')
                    last_progress = time.monotonic()
                if _copy(path, destination / relative, info):
                    copied.append((path, info, root))
        previous = [str(p) for p in roots if p not in (destination, media_state_root())]
        save_json(_config_path(), dict(location=str(destination), previous=previous))
        for path, info, root in copied:
            try:
                if not _unchanged(path, info):
                    raise OSError('Cache file changed after copying; original retained')
                path.unlink()
                _prune(path.parent, root)
            except (OSError, ValueError) as error:
                errors.append(f'{path}: {error}')
        result = overview()
        result.update(errors=list(dict.fromkeys(errors + result['errors'])))
        return result
