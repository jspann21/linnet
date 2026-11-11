"""Guarded recovery and permanent deletion of removed media."""
import json
import os
from pathlib import Path, PurePosixPath
import struct
import uuid
from datetime import datetime

from . import media_manager as media, media_library as library
from .host_state import cache_root, save_json
from .inventory import file_records
from .guard import guarded_path as _plain, guard_volume as _volume_guard, hash_file
from .publish import _exclusive, _write_verified
from .video_catalog import encode as _encode_catalog, read_file as _read_catalog_file


_MANIFEST = 'RECOVERY.JSON'
_MAX_MANIFEST = 16 * 1024 * 1024
_BACKUPS = {'INDEX.VID.before', 'LIBRARY.PDB.before', 'COVERS.CFL.before'}


def _recovery_temporary(relative):
    name = relative.as_posix()
    return (len(name) == 12 and name[0] in 'RI' and name[8:] == '.TMP' and
            all(c in '0123456789ABCDEF' for c in name[1:8]))


def _relative(value):
    """Accept only a canonical path below the device root."""
    if (not isinstance(value, str) or not value or value.startswith('/') or '\\' in value or
            ':' in value or any(part in ('', '.', '..') for part in value.split('/'))):
        raise ValueError('Unsafe Trash recovery path')
    return value


def _normal_location(category, kind, relative):
    """Check that a portable receipt describes a location this library can create."""
    parts = tuple(part.casefold() for part in relative.split('/'))
    suffix = PurePosixPath(relative).suffix.lower()
    apple_music = (len(parts) >= 3 and parts[:2] in
                   (('ipod_control', 'music'), ('ipod_c~1', 'music')))
    if category == 'original':
        return (suffix in media.extensions(kind) and
                ((len(parts) >= 2 and parts[0] == kind.casefold()) or
                 (kind in ('Music', 'Videos') and apple_music)))
    if category == 'library':
        locations = {('Music', '.linnet/library.pdb'),
                     ('Photos', '.linnet/photos/library.pim'),
                     ('Photos', '.linnet/photos/grid.pgr'),
                     ('Notes', '.linnet/notes/library.nnt')}
        return (kind, relative.casefold()) in locations
    if category == 'published':
        return (kind == 'Videos' and suffix in ('.ipv', '.ipvf', '.mp4', '.m4v', '.mov') and
                ((len(parts) >= 2 and parts[0] in ('videos', '.linnet')) or apple_music))
    return False


def _video_location(relative):
    """Conservatively identify paths that can carry restored Video media."""
    parts = tuple(part.casefold() for part in relative.split('/'))
    video_folder = (parts[0] == 'videos' or
                    len(parts) >= 2 and parts[:2] in
                    (('.linnet', 'videos'), ('ipod_control', 'music'), ('ipod_c~1', 'music')))
    suffix = PurePosixPath(relative).suffix.lower()
    return video_folder and (suffix == '.ipv' or suffix in media.extensions('Videos'))


def _portable(root, token, report):
    selected = []
    for row in report['selected']:
        if 'relative' in row:
            relative = _relative(row['relative'])
        else:
            path = Path(row['path'])
            _plain(path, root)
            relative = _relative(path.relative_to(root).as_posix())
        selected.append(dict(category=row['category'], kind=row['kind'],
                             title=str(row['title'])[:512], key=row.get('key'),
                             relative=relative,
                             bytes=row['bytes'], mtime_ns=row['mtime_ns'],
                             sha256=row.get('sha256')))
    return dict(version=2, token=token, started=report['started'],
                complete=bool(report.get('complete')), restored=bool(report.get('restored')),
                trash_emptied=bool(report.get('trash_emptied')),
                selected=selected, requires_update=report.get('requires_update', []),
                catalog_paths=report.get('catalog_paths', {}),
                dependencies=report.get('dependencies', []))


def _save_device(identity, root, trash, report):
    """Publish a small portable receipt on the identified device before mutation."""
    token = trash.name
    manifest = trash / _MANIFEST
    temporary = trash / ('R' + uuid.uuid4().hex[:7].upper() + '.TMP')
    _plain(manifest, root); _plain(temporary, root)
    data = json.dumps(_portable(root, token, report), ensure_ascii=False,
                      separators=(',', ':')).encode('utf-8')
    if len(data) > _MAX_MANIFEST:
        raise ValueError('Too many selected items for portable Trash recovery')
    owned = []
    try:
        _volume_guard(identity)
        _write_verified(temporary, data, 'Trash recovery metadata verification failed', owned=owned)
        _volume_guard(identity)
        os.replace(temporary, manifest)
        if manifest.read_bytes() != data:
            raise OSError('Portable Trash recovery metadata verification failed')
    finally:
        if temporary in owned and temporary.exists():
            _volume_guard(identity); _plain(temporary, root); temporary.unlink()


def _validate_manifest(value, token):
    if (not isinstance(value, dict) or value.get('version') != 2 or value.get('token') != token or
            type(value.get('started')) not in (int, float) or
            not isinstance(value.get('selected'), list) or
            not isinstance(value.get('catalog_paths'), dict) or
            not isinstance(value.get('dependencies'), list)):
        raise ValueError('Invalid portable Trash metadata')
    datetime.fromtimestamp(value['started'])
    if len(value['selected']) > 100000 or len(value['dependencies']) > 100000:
        raise ValueError('Oversized Trash metadata')
    if len({item.get('relative', '').casefold() for item in value['dependencies']
            if isinstance(item, dict) and isinstance(item.get('relative'), str)}) != len(value['dependencies']):
        raise ValueError('Duplicate photo recovery dependency')
    for row in value['selected']:
        if (not isinstance(row, dict) or row.get('category') not in ('original', 'library', 'published', 'supporting') or
                row.get('kind') not in media.KINDS or not isinstance(row.get('title'), str) or
                type(row.get('bytes')) is not int or row['bytes'] < 0 or
                row.get('mtime_ns') is not None and type(row['mtime_ns']) is not int):
            raise ValueError('Invalid selected Trash item')
        relative = _relative(row.get('relative'))
        if row['category'] == 'supporting':
            digest = row.get('sha256')
            try:
                library._supporting_parts(row['kind'], relative)
            except ValueError as error:
                raise ValueError('Invalid supporting-file recovery location') from error
            if (not isinstance(digest, str) or len(digest) != 64 or
                    any(c not in '0123456789abcdef' for c in digest)):
                raise ValueError('Invalid supporting-file recovery record')
        elif not _normal_location(row['category'], row['kind'], relative):
            raise ValueError('Selected Trash item does not match its recovery location')
        key = row.get('key')
        if key is not None and (not isinstance(key, str) or len(key) != 64 or
                                any(c not in '0123456789abcdef' for c in key)):
            raise ValueError('Invalid video recovery key')
    for key, path in value['catalog_paths'].items():
        if not isinstance(key, str) or len(key) != 64 or any(c not in '0123456789abcdef' for c in key):
            raise ValueError('Invalid catalog recovery key')
        _relative(path)
    for item in value['dependencies']:
        if (not isinstance(item, dict) or type(item.get('bytes')) is not int or item['bytes'] < 0 or
                not isinstance(item.get('sha256'), str) or len(item['sha256']) != 64 or
                any(c not in '0123456789abcdef' for c in item['sha256'])):
            raise ValueError('Invalid photo dependency')
        path = _relative(item.get('relative'))
        kind = item.get('kind', 'pack')
        if (kind == 'pack' and path.startswith('.LINNET/PHOTOS/PACKS/') and path.endswith('.PAK')):
            continue
        if kind == 'jpeg':
            from .photos import FAT_PATH
            if (FAT_PATH.fullmatch('/' + path) and path.endswith(('.JPG', '.JPEG')) and
                    0 < item['bytes'] <= 262144):
                continue
        if kind not in ('pack', 'jpeg'):
            raise ValueError('Unknown photo dependency type')
        else:
            raise ValueError('Unsupported photo dependency')
    return value


def _photo_pack_requirements(package):
    """Read the pack identities bound into a previously validated PIMG."""
    from .photos import HEADER, PACK_COUNT, PHOTO, MAX_PHOTOS, MAX_STRINGS, FAT_PATH, _crc
    with package.open('rb') as reader:
        header = reader.read(HEADER)
    if (len(header) != HEADER or header[:4] != b'LPIC' or
            _crc(header[:508]) != struct.unpack_from('<I', header, 508)[0]):
        raise ValueError('Photo package changed before Trash recovery.')
    size = package.stat().st_size
    count, photo_at = struct.unpack_from('<I', header, 20)[0], struct.unpack_from('<I', header, 36)[0]
    pool_at, pool_bytes = struct.unpack_from('<II', header, 48)
    if (not 1 <= count <= MAX_PHOTOS or not 1 <= pool_bytes <= MAX_STRINGS or
            photo_at + count * PHOTO > size or pool_at + pool_bytes > size):
        raise ValueError('Photo package has invalid fallback bounds.')
    version = struct.unpack_from('<H', header, 4)[0]
    with package.open('rb') as reader:
        reader.seek(pool_at)
        pool = reader.read(pool_bytes)
        if len(pool) != pool_bytes or pool[0] != 0 or pool[-1] != 0:
            raise ValueError('Invalid photo fallback string pool.')
        reader.seek(photo_at)
        distinct = {}
        for _ in range(count):
            photo = reader.read(PHOTO)
            if len(photo) != PHOTO or _crc(photo[:-4]) != struct.unpack_from('<I', photo, PHOTO - 4)[0]:
                raise ValueError('Invalid photo recovery record CRC.')
            start, length = struct.unpack_from('<II', photo, 12)
            if not length:
                continue
            if (length > 127 or start >= pool_bytes or length >= pool_bytes - start or
                    pool[start + length] != 0 or (start and pool[start - 1] != 0) or
                    b'\0' in pool[start:start + length]):
                raise ValueError('Invalid photo fallback string range.')
            try:
                name = pool[start:start + length].decode('ascii')
            except UnicodeError as error:
                raise ValueError('Invalid photo fallback path encoding.') from error
            fallback_bytes, crc = struct.unpack_from('<II', photo, 44)
            if (not FAT_PATH.fullmatch(name) or not name.endswith(('.JPG', '.JPEG')) or
                    not 0 < fallback_bytes <= 262144):
                raise ValueError('Invalid photo fallback reference in Trash.')
            item = dict(path=name, bytes=fallback_bytes, crc32=crc)
            if name in distinct and distinct[name] != item:
                raise ValueError('Conflicting photo fallback references.')
            distinct[name] = item
    fallbacks = list(distinct.values())
    if version == 0:
        return {}, fallbacks
    if version != 1:
        raise ValueError('Unsupported photo package in Trash.')
    result = {}
    for bucket in range(PACK_COUNT):
        identity, size = struct.unpack_from('<8sI', header, 72 + bucket * 12)
        if not size:
            continue
        prefix = identity.hex().upper()
        result[f'.LINNET/PHOTOS/PACKS/{prefix[:8]}/{prefix[8:]}.PAK'] = (prefix.lower(), size)
    return result, fallbacks


def _read_manifest(root, token):
    """Read recovery metadata without traversing or opening removed media."""
    if len(token) != 32 or any(c not in '0123456789abcdef' for c in token):
        raise ValueError('Invalid Trash entry')
    trash = root / '.LINNET' / 'TRASH' / token
    _plain(trash, root)
    if not trash.is_dir():
        raise ValueError('Missing Trash entry')
    manifest = trash / _MANIFEST
    _plain(manifest, root)
    with manifest.open('rb') as reader:
        raw = reader.read(_MAX_MANIFEST + 1)
    if len(raw) > _MAX_MANIFEST:
        raise ValueError('Oversized Trash metadata')
    return _validate_manifest(json.loads(raw.decode('utf-8')), token)


def summary(identity):
    """Count recoverable removal groups, excluding their internal backup files."""
    count, unreadable = 0, 0
    try:
        root = Path(identity['root']).resolve(strict=True)
        trash = root / '.LINNET' / 'TRASH'
        _plain(trash, root)
        if trash.exists() and not trash.is_dir():
            raise ValueError('Trash is not a directory')
        if trash.is_dir():
            for entry in trash.iterdir():
                try:
                    _read_manifest(root, entry.name)
                    count += 1
                except (OSError, ValueError, TypeError, KeyError, OverflowError):
                    unreadable += 1
    except (OSError, ValueError):
        unreadable += 1
    return dict(count=count, unreadable=unreadable)


def _removal(identity, root, token):
    receipt = cache_root(identity) / f'remove-{token}.json'
    try:
        trash = root / '.LINNET' / 'TRASH' / token
        report = _read_manifest(root, token)
        title = ', '.join(row['title'] for row in report['selected'][:3])
        files = file_records(trash)
        return (dict(id=token, receipt=str(receipt), path=str(trash), bytes=sum(i.st_size for _, i in files),
                     title=title, started=report['started']), report)
    except (OSError, ValueError, TypeError, KeyError, OverflowError):
        return None


def removals(identity):
    root = Path(identity['root']).resolve(strict=True)
    found = []
    tokens = set()
    trash_root = root / '.LINNET' / 'TRASH'
    _plain(trash_root, root)
    if trash_root.is_dir():
        tokens.update(path.name for path in trash_root.iterdir() if path.is_dir())
    for token in tokens:
        removal = _removal(identity, root, token)
        if removal is not None:
            found.append(removal[0])
    return sorted(found, key=lambda r: r['started'], reverse=True)


def inventory(identity):
    rows = removals(identity)
    status = summary(identity)
    status['unreadable'] += max(0, status['count'] - len(rows))
    status['count'] = len(rows)
    return dict(rows=rows, summary=status)


def recover(identity, token, *, erase=False):
    identity = media.refresh(identity)
    with _exclusive(identity):
        root = Path(identity['root']).resolve(strict=True)
        found = _removal(identity, root, token)
        if found is None:
            raise ValueError('This Trash removal changed. Refresh before continuing.')
        removal, report = found
        trash = Path(removal['path'])
        receipt = Path(removal['receipt'])
        manifest = trash / _MANIFEST
        if erase:
            # Enumerate guarded ordinary files before deleting any of them.
            files = file_records(trash)
            for path, info in files:
                if path == manifest:
                    continue
                _volume_guard(identity)
                _plain(path, trash)
                current = path.stat()
                if (current.st_size, current.st_mtime_ns) != (info.st_size, info.st_mtime_ns):
                    raise ValueError('Trash changed during removal; refresh to continue.')
                path.unlink()
            directories = sorted((p for p in trash.rglob('*') if p.is_dir()), key=lambda p: len(p.parts), reverse=True)
            for path in directories:
                _volume_guard(identity)
                _plain(path, trash)
                path.rmdir()
            _volume_guard(identity); _plain(manifest, trash); manifest.unlink()
            _volume_guard(identity)
            trash.rmdir()
            report['trash_emptied'] = True
            save_json(receipt, report)
            return dict(receipt=str(receipt), summary='Trash removal permanently deleted. Its storage is now available.')
        moves = []
        dependencies = {item['relative'].casefold(): item for item in report.get('dependencies', [])}
        selected_paths = {row['relative'].casefold() for row in report['selected']}
        supporting_rows = {row['relative'].casefold(): row for row in report['selected']
                           if row['category'] == 'supporting'}
        dependency_sources = []
        recovery_temporaries = []
        # Derive destinations from the guarded removal tree; never trust absolute receipt paths.
        for source, info in file_records(trash):
            relative = source.relative_to(trash)
            name = relative.as_posix()
            if name in _BACKUPS or name == _MANIFEST:
                continue
            if _recovery_temporary(relative):
                recovery_temporaries.append(source)
                continue
            if name.casefold() in dependencies:
                dependency_sources.append((source, root / relative, info, dependencies[name.casefold()]))
                continue
            allowed = relative.parts[0].casefold() in {kind.casefold() for kind in media.KINDS} or tuple(part.casefold() for part in relative.parts[:2]) == ('ipod_control', 'music') or name.casefold() in ('.linnet/library.pdb', '.linnet/photos/library.pim', '.linnet/photos/grid.pgr', '.linnet/notes/library.nnt') or (relative.parent.as_posix().casefold() == '.linnet/videos' and relative.suffix.lower() in ('.ipv', '.ipvf', '.mp4', '.m4v', '.mov'))
            if not allowed:
                raise ValueError('Unexpected file in Trash; inspect the report before restoring.')
            if name.casefold() not in selected_paths:
                raise ValueError('Trash contains a file outside its portable recovery manifest.')
            supporting = supporting_rows.get(name.casefold())
            if supporting is not None and hash_file(source, info.st_size) != supporting['sha256']:
                raise ValueError('Supporting file changed in Trash.')
            destination = root / relative
            _plain(destination, root)
            if destination.exists():
                raise ValueError(f'Cannot restore over an existing file: {relative}. Move that file first.')
            moves.append((source, destination, info))
        moved_names = {p.relative_to(trash).as_posix().casefold() for p, _, _ in moves}
        for row in report['selected']:
            name = row['relative']
            if name.casefold() in moved_names or row['mtime_ns'] is None:
                continue
            active = root / name
            _plain(active, root)
            if not active.is_file():
                raise ValueError('A selected Trash file is missing from both recovery locations.')
            info = active.stat()
            if (info.st_size, info.st_mtime_ns) != (row['bytes'], row['mtime_ns']):
                raise ValueError('A selected file changed since removal.')
            if row['category'] == 'supporting' and hash_file(active, info.st_size) != row['sha256']:
                raise ValueError('A supporting file changed since removal.')
        photo_name = '.LINNET/PHOTOS/LIBRARY.PIM'
        if photo_name.casefold() in selected_paths:
            package = (trash / photo_name) if photo_name.casefold() in moved_names else (root / photo_name)
            _plain(package, trash if photo_name.casefold() in moved_names else root)
            expected_packs, fallbacks = _photo_pack_requirements(package)
            expected_names = {name.casefold() for name in expected_packs}
            for fallback in fallbacks:
                name = fallback['path'].lstrip('/')
                if name.casefold() not in selected_paths:
                    expected_names.add(name.casefold())
                    item = dependencies.get(name.casefold())
                    if item is None or item.get('kind') != 'jpeg' or item['bytes'] != fallback['bytes']:
                        raise ValueError('Photo JPEG recovery metadata differs from its catalog.')
            if set(dependencies) != expected_names or any(
                    dependencies[name.casefold()].get('kind', 'pack') != 'pack' or
                    dependencies[name.casefold()]['bytes'] != size or
                    not dependencies[name.casefold()]['sha256'].startswith(prefix)
                    for name, (prefix, size) in expected_packs.items()):
                raise ValueError('Photo pack recovery metadata differs from its catalog.')
            grid_name = '.LINNET/PHOTOS/GRID.PGR'
            if grid_name.casefold() in selected_paths:
                from .photo_grid import inspect_grid
                grid = (trash / grid_name) if grid_name.casefold() in moved_names else (root / grid_name)
                _plain(grid, trash if grid_name.casefold() in moved_names else root)
                inspect_grid(grid, package=package, checkpoint=lambda: _volume_guard(identity))
        found_dependencies = {source.relative_to(trash).as_posix().casefold() for source, _, _, _ in dependency_sources}
        for name, item in dependencies.items():
            if name not in found_dependencies:
                existing = root / item['relative']
                _plain(existing, root)
                if (not existing.is_file() or existing.stat().st_size != item['bytes'] or
                        hash_file(existing, item['bytes']) != item['sha256']):
                    raise ValueError('A photo pack needed for recovery is missing.')
        for source, destination, info, item in dependency_sources:
            _plain(source, trash); _plain(destination, root)
            destination_valid = destination.exists() and destination.stat().st_size == item['bytes'] and hash_file(destination, item['bytes']) == item['sha256']
            if destination.exists() and not destination_valid:
                raise ValueError('A different photo pack occupies the recovery path.')
            source_valid = info.st_size == item['bytes'] and hash_file(source, info.st_size) == item['sha256']
            if not source_valid and not destination_valid:
                raise ValueError('Photo recovery pack changed in Trash.')
        backup = trash / 'INDEX.VID.before'
        video_involved = (backup.exists() or
                          any(_video_location(destination.relative_to(root).as_posix())
                              for _, destination, _ in moves) or
                          any(row.get('kind') == 'Videos' and row.get('category') != 'supporting'
                              for row in report.get('selected', [])) or
                          any(_video_location(row['relative'])
                              for row in report['selected']))
        if video_involved:
            catalog, before, entries = library._catalog(root)
        else:
            catalog, before, entries = root / '.LINNET' / 'VIDEOS' / 'INDEX.VID', None, []
        entries_by_key = {entry[0]: entry for entry in entries}
        restore_entries = []
        restore_paths = {}
        if backup.exists():
            _plain(backup, trash)
            _, old = _read_catalog_file(backup)
            selected_keys = {r.get('key') for r in report.get('selected', []) if r.get('category') == 'published'}
            restored_paths = {str(d).casefold() for _, d, _ in moves}
            for entry in old:
                relative = report.get('catalog_paths', {}).get(entry[0].hex())
                path = library._device_path(root, '/' + relative if relative else entry[1])
                if entry[0].hex() in selected_keys or str(path).casefold() in restored_paths:
                    existing = entries_by_key.get(entry[0])
                    if existing is not None and existing != entry:
                        existing_path = library._device_path(root, existing[1])
                        if (existing[2] != entry[2] or
                                str(existing_path).casefold() != str(path).casefold()):
                            raise ValueError('A newer video uses this library entry. Resolve the conflict before restoring.')
                    if existing is None:
                        if not path.exists() and str(path).casefold() not in restored_paths:
                            raise ValueError('The original video needed for recovery is missing.')
                        restore_entries.append(entry)
                        restore_paths[entry[0]] = path
        data = _encode_catalog(entries + restore_entries) if restore_entries else None
        for source, destination, info, item in dependency_sources:
            _volume_guard(identity); _plain(source, trash); _plain(destination, root)
            if destination.exists():
                if (destination.stat().st_size != item['bytes'] or
                        hash_file(destination, item['bytes']) != item['sha256']):
                    raise ValueError('A photo pack changed during recovery; its Trash copy was retained.')
            else:
                if source.stat().st_size != item['bytes'] or hash_file(source, item['bytes']) != item['sha256']:
                    raise ValueError('The photo pack in Trash changed during recovery.')
                destination.parent.mkdir(parents=True, exist_ok=True)
                source.rename(destination)
        for source, destination, info in moves:
            _volume_guard(identity)
            _plain(source, trash)
            _plain(destination, root)
            current = source.stat()
            if (current.st_size, current.st_mtime_ns) != (info.st_size, info.st_mtime_ns):
                raise ValueError('Trash changed during recovery; refresh to continue.')
            destination.parent.mkdir(parents=True, exist_ok=True)
            source.rename(destination)
        if data is not None:
            from .publish_video import device_video_path
            restore_entries = [(key, device_video_path(restore_paths[key], root), title) for key, _, title in restore_entries]
            data = _encode_catalog(entries + restore_entries)
            _volume_guard(identity)
            current = catalog.read_bytes() if catalog.exists() else None
            if current != before:
                raise ValueError('Video catalog changed. Files restored; retry to restore playback entries.')
            temporary = catalog.with_name('R' + uuid.uuid4().hex[:7] + '.TMP')
            _plain(temporary, root)
            owned_temporaries = []
            try:
                _write_verified(temporary, data, 'Restored catalog verification failed.', owned=owned_temporaries)
                _volume_guard(identity)
                _plain(catalog, root)
                os.replace(temporary, catalog)
                if catalog.read_bytes() != data:
                    raise OSError('Published recovery catalog verification failed.')
            finally:
                if temporary in owned_temporaries and temporary.exists():
                    _volume_guard(identity)
                    _plain(temporary, root)
                    temporary.unlink()
        report['restored'] = True
        _save_device(identity, root, trash, report)
        save_json(receipt, report)
        # Backup is no longer needed only after file and catalog restoration succeeded.
        for source, destination, _, item in dependency_sources:
            if source.exists():
                _volume_guard(identity); _plain(destination, root)
                if (not destination.is_file() or destination.stat().st_size != item['bytes'] or
                        hash_file(destination, item['bytes']) != item['sha256']):
                    raise ValueError('A restored photo pack changed; its Trash copy was retained.')
        for backup_path in (backup, trash / 'LIBRARY.PDB.before', trash / 'COVERS.CFL.before',
                            *(source for source, _, _, _ in dependency_sources if source.exists()),
                            *recovery_temporaries):
            if backup_path.exists():
                _volume_guard(identity)
                _plain(backup_path, trash)
                backup_path.unlink()
        for path in sorted((p for p in trash.rglob('*') if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
            _volume_guard(identity)
            _plain(path, trash)
            path.rmdir()
        _volume_guard(identity); _plain(manifest, trash); manifest.unlink()
        _volume_guard(identity)
        trash.rmdir()
        return dict(receipt=str(receipt), summary='Content restored. Update the affected categories to refresh their libraries.',
                    requires_update=sorted({row['kind'] for row in report.get('selected', []) if row['kind'] in ('Music', 'Photos', 'Notes')}))
