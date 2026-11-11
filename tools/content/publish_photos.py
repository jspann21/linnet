"""Guarded, unelevated LPIC publication to an explicitly identified iPod.

Related grid and catalog outputs are staged with device recovery copies before
their guarded fixed-name replacements. Existing optional JPEG references must
already match on-device.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import struct
import time
import uuid
import zlib

from .photos import HEADER, PHOTO, MAX_FILE, inspect
from .host_state import file_stamp
from .publish import (_plain, _bounded, _hash, _copy_verified, inspect_target, _exclusive,
                      _publish_library, _volume_guard, _finish_backups, _write_receipt,
                      _publish_group, _recover_pending_groups, _same_target)


def _fallbacks(package):
    """Extract only already-validated, bounded optional device references."""
    result = {}
    with package.open('rb') as reader:
        header = reader.read(HEADER)
        count, offset = struct.unpack_from('<I', header, 20)[0], struct.unpack_from('<I', header, 36)[0]
        pool_offset, pool_bytes = struct.unpack_from('<II', header, 48)
        reader.seek(pool_offset)
        pool = reader.read(pool_bytes)
        reader.seek(offset)
        for _ in range(count):
            photo = reader.read(PHOTO)
            start, length = struct.unpack_from('<II', photo, 12)
            if not length:
                continue
            path = pool[start:start+length].decode('ascii')
            size, crc = struct.unpack_from('<II', photo, 44)
            item = dict(path=path, bytes=size, crc32=crc)
            if path in result and result[path] != item:
                raise ValueError('conflicting JPEG fallback references in the package')
            result[path] = item
    return list(result.values())


def _verify_fallbacks(items, root, guard):
    for item in items:
        guard()
        path = root.joinpath(*item['path'].lstrip('/').split('/'))
        _plain(path, root)
        digest = _hash(path, item['bytes'])
        data = _bounded(path, 262144)
        if (len(data) != item['bytes'] or zlib.crc32(data) & 0xffffffff != item['crc32'] or
            hashlib.sha256(data).hexdigest() != digest):
            raise ValueError(f"existing JPEG fallback differs: {item['path']}")


def _publish_packs(packs, identity, root, candidate_bytes, grid_bytes):
    """Publish immutable tile packs before either catalog can name them."""
    if not packs:
        return []

    directory = root/'.LINNET'/'PHOTOS'/'PACKS'
    guard = lambda: _volume_guard(identity)
    pending = []
    verified_existing = {}
    distinct = {}
    for item in packs:
        source = Path(item['path'])
        _plain(source, Path(source.anchor))
        suffix = item['sha256'][:16].upper()
        expected = f'/.LINNET/PHOTOS/PACKS/{suffix[:8]}/{suffix[8:]}.PAK'
        if item['device_path'] != expected or not 0 < item['bytes'] <= 1 << 27:
            raise ValueError('invalid staged photo pack identity')
        active = root.joinpath(*expected.lstrip('/').split('/'))
        _plain(active, root)
        previous = distinct.get(active)
        if previous is not None:
            if (previous['bytes'], previous['sha256']) != (item['bytes'], item['sha256']):
                raise ValueError('different photo packs share one immutable name')
            continue
        distinct[active] = item
        guard()
        if active.exists():
            before = file_stamp(active.stat())
            hash_started_ns = time.time_ns()
            if _hash(active, item['bytes']) != item['sha256']:
                raise ValueError(f'photo pack name collision or changed iPod bytes: {active}')
            after = file_stamp(active.stat())
            if before != after:
                raise OSError(f'photo pack changed during verification: {active}')
            verified_existing[active] = (after, hash_started_ns)
        else:
            pending.append(item)
    old_indexes = 0
    if pending and grid_bytes:
        for name in ('GRID.PGR', 'LIBRARY.PIM'):
            active = root/'.LINNET'/'PHOTOS'/name
            guard(); _plain(active, root)
            if active.exists():
                size = active.stat().st_size
                if size > MAX_FILE:
                    raise ValueError('existing photo output exceeds the supported backup size')
                old_indexes += size
    needed = (sum(item['bytes'] for item in pending) + candidate_bytes + grid_bytes +
              old_indexes + 2*1024*1024) if pending else 0
    refreshed = inspect_target(identity['disk_number'], identity['disk_serial'],
                               identity['volume_serial'], identity['drive'])
    _same_target(identity, refreshed)
    if refreshed['free_bytes'] < needed:
        raise ValueError('insufficient iPod space for photo packs and catalogs')
    results = []
    for active, item in distinct.items():
        guard()
        source = Path(item['path'])
        if _hash(source, item['bytes']) != item['sha256']:
            raise ValueError(f'staged photo pack changed: {source}')
        reused = active.exists()
        if not reused:
            for part in (root/'.LINNET', root/'.LINNET'/'PHOTOS', directory, active.parent):
                guard(); _plain(part, root); part.mkdir(exist_ok=True)
            temporary = active.parent/('P'+os.urandom(4).hex().upper()[:7]+'.TMP')
            _plain(temporary, root)
            _copy_verified(source, temporary, item, root, guard)
            guard()
            if active.exists():
                raise RuntimeError(f'photo pack appeared during publication: {active}')
            os.rename(temporary, active)
        guard()
        _plain(active, root)
        if reused:
            stamp = file_stamp(active.stat())
            verified_stamp, hash_started_ns = verified_existing[active]
            if stamp != verified_stamp:
                raise OSError(f'photo pack changed after verification: {active}')
        else:
            before = file_stamp(active.stat())
            if _hash(active, item['bytes']) != item['sha256']:
                raise OSError(f'photo pack readback mismatch: {active}')
            stamp = file_stamp(active.stat())
            if before != stamp:
                raise OSError(f'photo pack changed during readback: {active}')
        results.append(dict(path=item['device_path'].lstrip('/'), package=str(active),
                            bytes=item['bytes'], sha256=item['sha256'], reused=reused, stamp=stamp,
                            hash_started_ns=hash_started_ns if reused else None))
    _same_target(identity, inspect_target(identity['disk_number'], identity['disk_serial'],
                                          identity['volume_serial'], identity['drive']))
    return results


def _verify_pack_proofs(results, identity, root):
    for item in results:
        _volume_guard(identity)
        path = Path(item['package'])
        _plain(path, root)
        if file_stamp(path.stat()) != item['stamp']:
            raise OSError(f"photo pack changed after readback: {path}")


def publish_photos(package, *, disk_number, expected_disk_serial,
                   expected_volume_serial, expected_drive=None, grid=None):
    """Validate all local bytes, preserve the previous package, publish last.

    An interrupted pair is rolled back under the device mutex on the next run.
    The iPod must remain exclusively in Disk Mode throughout the operation.
    """
    package = Path(os.path.abspath(package))
    _plain(package, Path(package.anchor))
    package = package.resolve(strict=True)
    digest = _hash(package)
    candidate = inspect(package, verify_payloads=True)
    if candidate['sha256'] != digest:
        raise ValueError('photo package changed during validation')
    references = _fallbacks(package)
    if _hash(package, candidate['bytes']) != digest:
        raise ValueError('photo package changed during reference validation')
    grid_candidate = None
    if grid is not None:
        from .photo_grid import inspect_grid
        grid = Path(os.path.abspath(grid))
        _plain(grid, Path(grid.anchor));grid = grid.resolve(strict=True)
        grid_digest = _hash(grid)
        grid_candidate = inspect_grid(grid, package=package)
        if grid_candidate['sha256'] != grid_digest:
            raise ValueError('native photo grid changed during validation')
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    root = Path(identity['root'])
    from .media_manager import _identify
    target = identity.get('native_target_id') or _identify(identity)['target_id']
    if candidate['version'] != (0 if target == 'photo4g-p98' else 1):
        raise ValueError('photo package layout differs from the device target')
    if package.drive.casefold() == root.drive.casefold():
        raise ValueError('photo package must be staged on a different volume from the iPod')
    if grid is not None and grid.drive.casefold() == root.drive.casefold():
        raise ValueError('native photo grid must be staged on a different volume from the iPod')
    with _exclusive(identity):
        _recover_pending_groups(root, identity)
        pack_results = _publish_packs(candidate.get('packs', []), identity, root, candidate['bytes'],
                                      grid_candidate['bytes'] if grid_candidate else 0)
        if grid is not None:
            return _publish_pair(package, candidate, references, grid, grid_candidate, identity, root,
                                 disk_number, expected_disk_serial, expected_volume_serial, expected_drive, pack_results)
        receipt = _publish(package, candidate, references, identity, root, disk_number,
                           expected_disk_serial, expected_volume_serial, expected_drive,
                           retain_backup=bool(pack_results), receipt_fields=dict(packs=pack_results))
        try:
            _verify_pack_proofs(pack_results, identity, root)
        except Exception as exc:
            receipt.update(passed=False, phase='pack-verification-failed', error=str(exc))
            _write_receipt(Path(receipt['receipt']), receipt)
            raise
        if pack_results:
            _finish_backups(receipt, [Path(value) for value in receipt.get('backups_retained', [])], root,
                            lambda: _volume_guard(identity), host_root=package.parent)
            _write_receipt(Path(receipt['receipt']), receipt)
        return receipt


def _publish(package, candidate, references, identity, root, number, serial, volume, drive,
             receipt_fields=None, **options):
    return _publish_library(
        package, candidate, identity, root, number, serial, volume, drive,
        kind="photos", label="photo", filename="LIBRARY.PIM", max_file=MAX_FILE,
        summary=dict(photos=candidate['photos'], albums=candidate['albums']),
        verify_references=lambda root, guard: _verify_fallbacks(references, root, guard),
        receipt_fields=dict(fallback_files=references, **(receipt_fields or {})),
        backup_on_host=True, **options)


def _publish_pair(package, candidate, references, grid, grid_candidate, identity, root,
                  number, serial, volume, drive, pack_results):
    from .media_manager import _identify
    target = identity.get('native_target_id') or _identify(identity)['target_id']
    if target != grid_candidate['target']:
        raise ValueError('native photo grid target differs from the iPod')
    outputs = (('GRID.PGR', grid_candidate, grid), ('LIBRARY.PIM', candidate, package))
    previous, needed, changed, reused = {}, 0, [], []
    for name, item, staged in outputs:
        _volume_guard(identity)
        active = root/'.LINNET'/'PHOTOS'/name
        _plain(active, root)
        hint = None
        if active.exists():
            info = active.stat()
            if not 0 <= info.st_size <= MAX_FILE:
                raise ValueError('existing photo output exceeds the supported backup size')
            hint = dict(bytes=info.st_size, sha256=_hash(active, info.st_size),
                        stamp=(info.st_size,info.st_mtime_ns,info.st_ctime_ns,info.st_ino))
        previous[name] = hint
        if hint and hint['sha256'] == item['sha256']:
            reused.append((active, item))
        else:
            changed.append((f'.LINNET/PHOTOS/{name}', staged, item))
            needed += item['bytes'] + (hint['bytes'] if hint else 0) + 3*65536+1024*1024
    refreshed = inspect_target(number, serial, volume, drive)
    _same_target(identity, refreshed)
    if refreshed['free_bytes'] < needed:
        raise ValueError('insufficient free space for changed photo outputs and device recovery copies')
    from .host_state import receipt_directory
    receipt_path = receipt_directory(identity)/('publish-photos-'+uuid.uuid4().hex+'.json')
    receipt = dict(operation='publish-photos', passed=False, phase='preflight-complete',
                   identity=identity, candidate_sha256=candidate['sha256'],
                   candidate_bytes=candidate['bytes'], previous=previous['LIBRARY.PIM'],
                   grid_previous=previous['GRID.PGR'], packs=pack_results,
                   started_at=time.time(), receipt=str(receipt_path), elevation=False,
                   durability='file fsync and readback; FAT rename is not a power-loss journal')
    def record():
        _write_receipt(receipt_path, receipt)
    record()
    try:
        for source, item in ((grid, grid_candidate), (package, candidate)):
            _plain(source, Path(source.anchor))
            if _hash(source, item['bytes']) != item['sha256']:
                raise RuntimeError(f'staged photo output changed: {source}')
        _verify_fallbacks(references, root, lambda: _volume_guard(identity))
        _verify_pack_proofs(pack_results, identity, root)
        _same_target(identity, inspect_target(number, serial, volume, drive))
        def verify_related():
            _verify_fallbacks(references, root, lambda: _volume_guard(identity))
            _verify_pack_proofs(pack_results, identity, root)
            for active, item in reused:
                _volume_guard(identity)
                _plain(active, root)
                if _hash(active, item['bytes']) != item['sha256']:
                    raise RuntimeError(f'reused photo output changed: {active}')
        if changed:
            receipt['phase'] = 'replacing-related-indexes'; record()
            _publish_group(root, identity, 'PHOTOS', changed, verify_related)
        else:
            verify_related()
        reused_names = {active.name for active, _ in reused}
        grid_phase = 'reused' if 'GRID.PGR' in reused_names else 'published'
        package_phase = 'reused' if 'LIBRARY.PIM' in reused_names else 'published'
        grid_receipt = dict(operation='publish-photos', passed=True,
                            phase=grid_phase, identity=identity,
                            candidate_sha256=grid_candidate['sha256'], candidate_bytes=grid_candidate['bytes'],
                            package=str(root/'.LINNET'/'PHOTOS'/'GRID.PGR'), receipt=str(receipt_path))
        receipt.update(passed=True, phase=package_phase,
                       package=str(root/'.LINNET'/'PHOTOS'/'LIBRARY.PIM'),
                       grid=grid_receipt, photos=candidate['photos'], albums=candidate['albums'],
                       finished_at=time.time())
        record()
        return receipt
    except Exception as exc:
        receipt.update(error=str(exc), finished_at=time.time())
        try:
            record()
        except OSError:
            pass
        raise RuntimeError(f"Photo pair stopped in {receipt['phase']}: {exc}. Inspect {receipt_path} and the device recovery record") from exc
