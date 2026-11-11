"""Publish an OS candidate through an identified native Disk Mode volume."""
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib

from tools.inspect.image import inspect
from tools.studio.storage import record
from tools.studio.targets import TARGETS
from tools.studio.update_history import _boot_record


def _provision(root, image, capacity, guard):
    from tools.content.publish import _plain
    from tools.studio.storage import FILES, SCRATCH_BYTES, _write_new, initial_metadata
    directory=root/'.LINNET'
    metadata=initial_metadata(image)
    payloads=((image,capacity),(b'',capacity),(record(b''),512),(record(b''),512),
              (b'',SCRATCH_BYTES),(metadata,512),(metadata,512))
    # Validate every existing file before creating anything during a resume.
    present=set()
    for name,(data,size) in zip(FILES,payloads):
        path=directory/name;_plain(path,root)
        if not path.exists():continue
        if not path.is_file() or path.stat().st_size!=size:
            raise ValueError('Initial installation file differs: '+name)
        with path.open('rb') as stream:
            if stream.read(len(data))!=data:
                raise ValueError('Initial installation requires the same build: '+name)
            if any(any(chunk) for chunk in iter(lambda:stream.read(65536),b'')):
                raise ValueError('Initial installation padding differs: '+name)
        present.add(name)
    guard();directory.mkdir(exist_ok=True)
    for name,(data,size) in zip(FILES,payloads):
        if name in present:continue
        guard();_plain(directory/name,root)
        _write_new(directory/name,data,size)
    guard()


def publish(identity, image_path):
    from tools.content.media_manager import refresh
    from tools.content.publish import _exclusive, _plain, _volume_guard
    from tools.studio.installer import installed_state
    image_path = Path(image_path)
    spec = TARGETS[identity['target_id']]
    with image_path.open('rb') as stream:
        image = stream.read(spec['os_capacity_bytes'] + 1)
    if len(image) > spec['os_capacity_bytes']:
        raise ValueError('native update image exceeds device capacity')
    info = inspect(image)
    with image_path.with_suffix('.json').open('rb') as stream:
        report_bytes = stream.read(1024 * 1024 + 1)
    if len(report_bytes) > 1024 * 1024:
        raise ValueError('native update build report exceeds size limit')
    report = json.loads(report_bytes.decode('utf-8'))
    if (not isinstance(report, dict) or
            identity.get('native_target_id') != identity['target_id'] or
            spec['installer']['boot_layout'] not in ('s5l-nor', 'ipod-directory') or
            info['target_id'] != identity['target_id'] or report.get('kind') != 'os' or
            report.get('image') != info or
            report.get('device_serial') != ('' if info['profile'] == 'release' else identity['linnet_usb_serial']) or
            info['load_address'] != spec['load_address'] or len(image) > spec['os_capacity_bytes']):
        raise ValueError('native update image or mounted target mismatch')
    if 'serial_binding' in report:
        from tools.studio.firmware_binding import validate_binding
        validate_binding(image, report, identity['linnet_usb_serial'])
    with _exclusive(identity):
        current = refresh(identity)
        root = Path(current['root']).resolve(strict=True)
        directory = root / '.LINNET'
        _plain(directory, root)
        if spec['installer']['boot_layout'] == 's5l-nor':
            from tools.studio import classic_recovery
            if classic_recovery.find(current):
                classic_recovery.preserve(current)
        state = installed_state(root, identity['target_id'])
        if state['state'] in ('absent','incomplete'):
            if spec['installer']['boot_layout'] != 's5l-nor':
                raise ValueError('disk-backed native updates require an existing confirmed installation')
            _provision(root,image,spec['os_capacity_bytes'],lambda:_volume_guard(current))
            if classic_recovery.find(current):
                classic_recovery.preserve(current)
            refresh(current)
            verified=installed_state(root,identity['target_id'])
            if (verified.get('build_id')!=info['build_id'] or verified.get('image_sha256')!=info['sha256'] or
                    verified['state']!='installed'):
                raise OSError('Initial OS installation readback failed')
            # Native Classic setup bypasses the generic package provisioner.
            # Create the shared brightness/menu store before reporting success.
            from tools.studio.theme_settings import provision as provision_preferences
            preferences=provision_preferences(root,current['disk_serial'],current['disk_bytes'],info['build_id'])
            return dict(passed=True,initial_installation=True,requires_safe_eject=True,image=info,slot=0,
                        preferences=preferences,
                        summary='Linnet is installed. Safely eject the iPod to start it.')
        if state['state'] != 'installed':
            raise ValueError('native update requires a valid confirmed installation')
        paths = [directory / name for name in ('BOOT0.BIN', 'BOOT1.BIN')]
        copies = [_boot_record(path, spec['os_capacity_bytes']) for path in paths]
        selected = max(range(2), key=lambda index: copies[index][1])
        data, generation = copies[selected][:2]
        if data[53] != 255 or data[54]:
            raise ValueError('finish the pending firmware update before publishing another')
        payload = bytearray(data[28:212])
        boot_generation = struct.unpack_from('<I', payload, 16)[0]
        if generation == 0xffffffff or boot_generation == 0xffffffff:
            raise ValueError('boot metadata generation exhausted')
        slot = 1 - data[52]
        image_path = directory / f'OS{slot}.LOS'
        _plain(image_path, root)
        if image_path.stat().st_size != spec['os_capacity_bytes']:
            raise ValueError('inactive OS slot has incorrect capacity')
        _volume_guard(current)
        with image_path.open('r+b') as stream:
            stream.write(image)
            stream.flush()
            os.fsync(stream.fileno())
        with image_path.open('rb') as stream:
            if stream.read(len(image)) != image:
                raise OSError('inactive OS image readback failed; candidate not published')
        refresh(current)
        if any(_boot_record(path, spec['os_capacity_bytes'])[0] != prior[0]
               for path, prior in zip(paths, copies)):
            raise RuntimeError('boot metadata changed during update')
        payload[25] = slot
        struct.pack_into('<I', payload, 16, boot_generation + 1)
        struct.pack_into('<I', payload, 36, zlib.crc32(payload[8:36]))
        offset = 40 + slot * 72
        struct.pack_into('<II', payload, offset, len(image), ('release', 'dev', 'trace').index(info['profile']))
        payload[offset + 8:offset + 40] = bytes.fromhex(info['build_id'])
        payload[offset + 40:offset + 72] = hashlib.sha256(image).digest()
        publication = record(payload, generation + 1)
        _volume_guard(current)
        destination = paths[1 - selected]
        with destination.open('r+b') as stream:
            stream.write(publication)
            stream.flush()
            os.fsync(stream.fileno())
        if _boot_record(destination, spec['os_capacity_bytes'])[0] != publication:
            raise OSError('candidate metadata readback failed; inspect before retrying')
        return dict(passed=True, candidate_published=True, requires_safe_eject=True,
                    image=info, slot=slot, confirmed_build=state['build_id'],
                    summary='Firmware update prepared. Safely eject the iPod to start it; the previous confirmed OS remains available for rollback.')
