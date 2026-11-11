"""Guarded, unelevated single-video publication with verified previous backup."""
import hashlib
import os
import ctypes
import shutil
from pathlib import Path, PureWindowsPath
import time
import uuid
from .ipvf_validate import inspect_file
from .publish import _plain, _hash, _copy_verified, _same_target, _volume_guard, inspect_target, _exclusive, _write_receipt, _write_verified, _finish_backups
from .video_catalog import encode as _encode_catalog, read_file as _read_catalog_file
from .host_state import file_stamp
MAX_FILE=0x7fffffff


def video_filename(title, extension=".ipvf"):
    """A Windows/FAT-safe name; collisions are resolved against the actual folder."""
    name = Path(str(title).replace('\\', '/')).stem
    name = ''.join(c if ord(c) >= 32 and c not in '<>:"/\\|?*' else '_' for c in name)
    name = name.encode('utf-16-le')[:320].decode('utf-16-le', errors='ignore').strip(' .') or 'Video'
    if PureWindowsPath(name).is_reserved():
        name = '_' + name
    return name + extension


def _catalog_title(value):
    label = ''.join(c if ord(c) >= 32 else ' ' for c in str(value)).strip() or 'Video'
    return label.encode('utf-8')[:127].decode('utf-8', errors='ignore')


def device_video_path(source, root):
    buffer = ctypes.create_unicode_buffer(1024)
    if not ctypes.windll.kernel32.GetShortPathNameW(str(source), buffer, len(buffer)):
        raise OSError('Could not resolve the video FAT short path')
    path = '/' + Path(buffer.value).relative_to(root).as_posix()
    _encode_catalog([(bytes(32), path, 'Video')])
    return path


def _media_profile(root):
    from tools.inspect.retail_device import detect
    from tools.studio.targets import media_profile
    return media_profile(detect(root)["target_id"])


def _target_check(candidate, root, profile=None):
    profile = profile or _media_profile(root)
    if not profile["video_enabled"]:
        raise ValueError("This iPod target does not support video playback")
    native = profile["video_backend"] in ("bcm-mp4", "native-mp4")
    if candidate.get("format") == "MP4":
        if not native:
            raise ValueError("This iPod target does not support native MP4")
        if candidate.get("extension", ".mp4") not in profile["video_extensions"]:
            raise ValueError("This video container is unsupported by the selected iPod target")
        return
    if native:
        raise ValueError("This iPod target requires native MP4 video")
    if (candidate.get("width"), candidate.get("height")) != (profile["video_width"], profile["video_height"]):
        raise ValueError("video geometry does not match the selected iPod target")
    if not 1 <= candidate["max_record_sectors"] <= profile["max_read_sectors"]:
        raise ValueError("video record size exceeds the selected iPod target")


def publish_video(package, *, disk_number, expected_disk_serial,
                   expected_volume_serial, expected_drive=None, library_key=None, title=None,
                   ffprobe=None, source_to_preserve=None, readback_proof=None):
    """Validate all local bytes, preserve the previous package, publish last.

    A failure after replacement is an uncertain publication; inspect the host
    receipt and retained package/backup instead of automatically replaying it.
    The iPod must remain exclusively in Disk Mode throughout the operation.
    """
    package = Path(os.path.abspath(package))
    _plain(package, Path(package.anchor))
    package = package.resolve(strict=True)
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    root = Path(identity["root"])
    protected = None
    if source_to_preserve is not None:
        protected = Path(os.path.abspath(source_to_preserve))
        _plain(protected, root)
        protected = protected.resolve(strict=True)
        if not protected.is_file() or not protected.is_relative_to(root):
            raise ValueError('Video source to preserve is outside this iPod')
    profile = _media_profile(root)
    digest = _hash(package)
    if package.suffix.lower() in ('.mp4', '.m4v', '.mov'):
        from .native_video import inspect_file as inspect_native
        candidate = inspect_native(package, profile=profile, ffprobe=ffprobe)
    else:
        candidate = inspect_file(package); candidate['format'] = 'IPVF'
    candidate['sha256'] = digest
    candidate['bytes']=candidate['file_bytes']
    if _hash(package, candidate['bytes']) != digest:
        raise ValueError('video package changed during reference validation')
    _target_check(candidate, root, profile)
    if package.drive.casefold() == root.drive.casefold():
        raise ValueError('video package must be staged on a different volume from the iPod')
    with _exclusive(identity):
        from .publish import _recover_pending_groups
        _recover_pending_groups(root, identity)
        return _publish(package, candidate, identity, root, disk_number,
                        expected_disk_serial, expected_volume_serial, expected_drive,
                        library_key, title, protected, readback_proof)


def _publish(package, candidate, identity, root, number, serial, volume, drive,
             library_key=None, title=None, protected=None, readback_proof=None):
    def guard():
        _volume_guard(identity)
    linnet, metadata = root/'.LINNET', root/'.LINNET'/'VIDEOS'
    native = candidate.get('format') == 'MP4'
    extension = candidate.get('extension', '.mp4') if native else '.ipvf'
    if extension not in ('.mp4', '.m4v', '.mov', '.ipvf'):
        raise ValueError('Unsupported video container extension')
    directory = root/'Videos'/('Native' if native else 'IPVF')
    key = hashlib.sha256(str(library_key if library_key is not None else package.stem).encode('utf-8')).digest()
    filename = video_filename(title or package.stem, extension)
    active, backups = directory/filename, metadata/'BACKUP'
    catalog = metadata/'INDEX.VID'
    catalog_before = None
    _plain(catalog, root)
    entries = []
    if catalog.exists():
        catalog_before, entries = _read_catalog_file(catalog)
    if native:
        entries = [entry for entry in entries if Path(entry[1]).suffix.lower() in ('.mp4', '.m4v', '.mov')]
    existing = next((root / entry[1].lstrip('/') for entry in entries if entry[0] == key), None)
    if (existing is not None and existing.is_file() and existing.parent == directory and
            (protected is None or existing.resolve() != protected)):
        active = existing.resolve()
    elif active.exists():
        active = directory / (Path(filename).stem + ' [' + key.hex()[:8] + ']' + extension)
        if active.exists():
            raise ValueError('Video filename already exists; rename the source first')
    path = ('/VIDEOS/NATIVE/' if native else '/VIDEOS/IPVF/') + key.hex()[:8].upper() + (extension.upper() if native else '.IPV')
    for entry in entries:
        if entry[1].casefold() == path.casefold() and entry[0] != key:
            raise ValueError('video library filename collision; use a different library key')
    entries = [entry for entry in entries if entry[0] != key]
    label = _catalog_title(title or package.stem)
    entries.append((key, path, label))
    if len(entries)>256:
        raise ValueError('video library supports up to 256 videos')
    entries.sort(key=lambda entry: (entry[2].casefold(), entry[1]))
    catalog_bytes = _encode_catalog(entries)
    guard()
    for path in (linnet, metadata, directory.parent, directory, active, backups):
        _plain(path, root)
    previous = None
    if active.exists():
        # Preserve exact previous bytes even if its content is corrupt; replacing
        # a damaged package must not require deleting recovery evidence first.
        size = active.stat().st_size
        if not 0 <= size <= MAX_FILE:
            raise ValueError('existing video package exceeds the supported backup size')
        previous = dict(bytes=size, sha256=_hash(active, size))
    reuse_package = previous is not None and previous['bytes'] == candidate['bytes'] and previous['sha256'] == candidate['sha256']
    # Photo IPVF is prepared on this computer; keep its recovery bytes here too.
    backup_on_host = not native
    needed = (0 if reuse_package else candidate['bytes'] + (previous['bytes'] if previous and not backup_on_host else 0)) + 3*65536 + 1024*1024
    if identity['free_bytes'] < needed:
        raise ValueError('insufficient free space for new video package and retained backup')
    from .host_state import receipt_directory
    receipts = receipt_directory(identity)
    host_root = receipts.parent
    if backup_on_host and previous and not reuse_package and shutil.disk_usage(host_root).free < previous['bytes'] + 1024*1024:
        raise ValueError('insufficient computer space for previous video package')
    _same_target(identity, inspect_target(number, serial, volume, drive))
    receipt_path = receipts/('publish-videos-'+uuid.uuid4().hex+'.json')
    receipt = dict(operation='publish-videos', passed=False, phase='preflight-complete',
                   identity=identity, candidate_sha256=candidate['sha256'], candidate_bytes=candidate['bytes'],
                   previous=previous, started_at=time.time(),
                   elevation=False, receipt=str(receipt_path),
                   durability='Publication pending; FAT rename is not a power-loss journal')

    def record():
        _write_receipt(receipt_path, receipt)

    record()
    recovery_paths = []
    host_recovery_paths = []
    def finish_backups():
        _finish_backups(receipt, recovery_paths, root, guard)
        if host_recovery_paths:
            _finish_backups(receipt, host_recovery_paths, root, guard, host_root=host_root)
            receipt['backups_retained'] = [str(path) for path in recovery_paths + host_recovery_paths if path.exists()]
    try:
        for path in (linnet, metadata, directory.parent, directory):
            guard()
            _plain(path, root)
            path.mkdir(exist_ok=True)
            _plain(path, root)
        if previous and not reuse_package:
            receipt['phase'] = 'backing-up-previous'
            record()
            if backup_on_host:
                backup = host_root/'recovery'/(uuid.uuid4().hex+'-'+previous['sha256'][:8].upper()+'.IPV')
                _plain(backup.parent, host_root); backup.parent.mkdir(exist_ok=True)
                host_recovery_paths.append(backup)
                receipt['backup'] = str(backup); record()
            else:
                backups.mkdir(exist_ok=True)
                backup = backups/(previous['sha256'][:8].upper()+active.suffix.upper())
            _copy_verified(active, backup, previous, root, guard,
                           destination_root=host_root if backup_on_host else None)
            receipt['backup'] = str(backup)
            if not backup_on_host:
                recovery_paths.append(backup)
        if not reuse_package:
            temporary = directory/('P'+uuid.uuid4().hex[:7].upper()+'.TMP')
            receipt.update(phase='copying-candidate', temporary=str(temporary))
            record()
            _plain(package, Path(package.anchor))
            _copy_verified(package, temporary, candidate, root, guard)
        receipt['phase'] = 'candidate-verified'
        record()
        _plain(package, Path(package.anchor))
        if _hash(package, candidate['bytes']) != candidate['sha256']:
            raise RuntimeError('video source package changed during publication')
        _same_target(identity, inspect_target(number, serial, volume, drive))
        guard()
        _plain(active, root)
        readback_stamp = None
        hash_started_ns = None
        if previous:
            before_readback = file_stamp(active.stat()) if reuse_package else None
            if reuse_package:
                hash_started_ns = time.time_ns()
            if _hash(active, previous['bytes']) != previous['sha256']:
                raise RuntimeError('active video package changed during publication')
            if reuse_package:
                readback_stamp = file_stamp(active.stat())
                if before_readback != readback_stamp:
                    raise RuntimeError('active video package changed during readback')
        elif active.exists():
            raise RuntimeError('another video package appeared during publication')
        receipt['phase'] = 'reusing-package' if reuse_package else 'replacing-package'
        record()
        if not reuse_package:
            os.replace(temporary, active)
            guard()
            with active.open('rb+') as writer:
                writer.flush()
                os.fsync(writer.fileno())
        if not reuse_package:
            before_readback = file_stamp(active.stat())
            hash_started_ns = time.time_ns()
            if _hash(active, candidate['bytes']) != candidate['sha256']:
                raise OSError('published video package readback mismatch')
            readback_stamp = file_stamp(active.stat())
            if before_readback != readback_stamp:
                raise OSError('published video package changed during readback')
        path = device_video_path(active, root)
        entries = [(k, path if k == key else p, t) for k, p, t in entries]
        catalog_bytes = _encode_catalog(entries)
        receipt['phase'] = 'publishing-catalog'
        record()
        _plain(catalog, root)
        if (catalog.read_bytes() if catalog.exists() else None) != catalog_before:
            raise RuntimeError('video catalog changed during publication')
        if catalog_bytes == catalog_before:
            receipt['catalog_reused'] = True
            receipt['package_reused'] = reuse_package
            receipt['durability'] = (
                'Existing package and catalog verified; neither rewritten'
                if reuse_package else
                'Published package fsynced and read back; existing catalog verified; '
                'FAT rename is not a power-loss journal'
            )
            finish_backups()
            receipt.update(passed=True, phase='published', finished_at=time.time(), package=str(active),
                           catalog=str(catalog), frames=candidate['frames'])
            record()
            if readback_proof is not None:
                readback_proof.update(path=str(active), sha256=candidate['sha256'],
                                      stamp=readback_stamp, hash_started_ns=hash_started_ns)
            return receipt
        if catalog_before is not None:
            backups.mkdir(exist_ok=True)
            saved = backups/(hashlib.sha256(catalog_before).hexdigest()[:8].upper()+'.VID')
            _plain(saved, root)
            if saved.exists():
                if saved.read_bytes() != catalog_before:
                    raise RuntimeError('video catalog backup collision')
            else:
                _write_verified(saved, catalog_before, 'video catalog backup readback mismatch')
            recovery_paths.append(saved)
            receipt['catalog_backup'] = str(saved)
            record()
        temp_index = metadata/('I'+uuid.uuid4().hex[:7].upper()+'.TMP')
        guard(); _plain(temp_index, root)
        _write_verified(temp_index, catalog_bytes, 'video catalog readback mismatch')
        guard(); _plain(catalog, root)
        os.replace(temp_index, catalog)
        if catalog.read_bytes()!=catalog_bytes:
            raise OSError('published video catalog readback mismatch')
        receipt['catalog'] = str(catalog)
        finish_backups()
        if reuse_package:
            receipt['durability'] = (
                'Existing package verified; catalog fsynced and read back; '
                'FAT rename is not a power-loss journal'
            )
        else:
            receipt['durability'] = (
                'Package and catalog fsynced and read back; '
                'FAT rename is not a power-loss journal'
            )
        receipt.update(passed=True, phase='published', finished_at=time.time(), package=str(active),
                       frames=candidate['frames'], package_reused=reuse_package, catalog_reused=False)
        record()
        if readback_proof is not None:
            readback_proof.update(path=str(active), sha256=candidate['sha256'],
                                  stamp=readback_stamp, hash_started_ns=hash_started_ns)
        return receipt
    except Exception as exc:
        receipt.update(error=str(exc), finished_at=time.time())
        if host_recovery_paths:
            receipt['backups_retained'] = [str(path) for path in recovery_paths + host_recovery_paths if path.exists()]
        try:
            record()
        except OSError:
            pass
        raise RuntimeError(f"Video publication stopped in {receipt['phase']}: {exc}. Inspect {receipt_path}; do not replay automatically") from exc


def index_video(source, *, disk_number, expected_disk_serial,
               expected_volume_serial, expected_drive=None, library_key=None, title=None, ffprobe=None):
    """Validate an existing iPod video and publish only its bounded 8.3 catalog path."""
    source = Path(os.path.abspath(source))
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    root = Path(identity['root']).resolve(strict=True)
    _plain(source, root)
    source = source.resolve(strict=True)
    if not source.is_relative_to(root) or source.suffix.lower() not in ('.ipvf', '.mp4', '.m4v', '.mov'):
        raise ValueError('Native video source must already be on this iPod')
    profile = _media_profile(root)
    before = source.stat()
    if source.suffix.lower() in ('.mp4', '.m4v', '.mov'):
        from .native_video import inspect_file as inspect_native
        candidate = inspect_native(source, profile=profile, ffprobe=ffprobe)
    else:
        candidate = inspect_file(source)
    _target_check(candidate, root, profile)
    after = source.stat()
    if file_stamp(before) != file_stamp(after):
        raise ValueError('Video changed during validation')
    path = device_video_path(source, root)
    key = hashlib.sha256(str(library_key or source.relative_to(root)).replace('\\', '/').encode('utf-8')).digest()
    label = _catalog_title(title or source.stem)
    catalog = root/'.LINNET'/'VIDEOS'/'INDEX.VID'
    backups = root/'.LINNET'/'VIDEOS'/'BACKUP'
    from .host_state import receipt_directory
    receipt_path = receipt_directory(identity)/('index-video-'+uuid.uuid4().hex+'.json')
    receipt = dict(operation='index-video', passed=False, phase='validated', identity=identity,
                   source=str(source), device_path=path, frames=candidate['frames'], media_id=candidate.get('media_id'),
                   started_at=time.time(), receipt=str(receipt_path))
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    def record():
        _write_receipt(receipt_path, receipt)
    record()
    with _exclusive(identity):
        from .publish import _recover_pending_groups
        _recover_pending_groups(root, identity)
        try:
            _volume_guard(identity); _plain(catalog, root); _plain(backups, root)
            previous, entries = _read_catalog_file(catalog) if catalog.exists() else (None, [])
            if candidate.get('format') == 'MP4':
                entries = [entry for entry in entries if Path(entry[1]).suffix.lower() in ('.mp4', '.m4v', '.mov')]
            # An in-place recheck retains the existing library identity.
            existing = next((entry for entry in entries if entry[1].casefold() == path.casefold()), None)
            if existing is not None:
                key = existing[0]
            entries = [entry for entry in entries if entry[0] != key]
            entries.append((key, path, label)); entries.sort(key=lambda entry: (entry[2].casefold(), entry[1]))
            if len(entries) > 256: raise ValueError('video library supports up to 256 videos')
            data = _encode_catalog(entries)
            if data == previous:
                receipt.update(passed=True, phase='unchanged', finished_at=time.time(), catalog=str(catalog))
                record()
                return receipt
            _same_target(identity, inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive))
            catalog.parent.mkdir(parents=True, exist_ok=True)
            if previous is not None:
                backups.mkdir(exist_ok=True)
                saved = backups/(hashlib.sha256(previous).hexdigest()[:8].upper()+'.VID')
                if saved.exists() and saved.read_bytes() != previous: raise RuntimeError('video catalog backup collision')
                if not saved.exists():
                    _write_verified(saved, previous, 'video catalog backup readback mismatch')
                receipt['catalog_backup'] = str(saved)
                record()
            temporary = catalog.parent/('I'+uuid.uuid4().hex[:7].upper()+'.TMP')
            _write_verified(temporary, data, 'video catalog staging readback mismatch')
            _volume_guard(identity); os.replace(temporary, catalog)
            if catalog.read_bytes() != data: raise OSError('video catalog readback mismatch')
            _finish_backups(receipt, [saved] if previous is not None else [], root, lambda: _volume_guard(identity))
            receipt.update(passed=True, phase='published', finished_at=time.time(), catalog=str(catalog)); record(); return receipt
        except Exception as exc:
            receipt.update(error=str(exc), finished_at=time.time()); record()
            raise RuntimeError(f"Video indexing stopped in {receipt['phase']}: {exc}. Inspect {receipt_path}; do not replay automatically") from exc
