"""File provisioning. Native Boot installation is a separate transaction."""
import hashlib
import json
import os
import re
import struct
import zlib
from tools.inspect.image import inspect
from tools.studio.targets import TARGETS

SCRATCH_BYTES = 4 * 1024 * 1024
FILES = ('OS0.LOS', 'OS1.LOS', 'STATE0.BIN', 'STATE1.BIN',
         'SCRATCH.BIN', 'BOOT0.BIN', 'BOOT1.BIN')


def image_capacity(target_id):
    try:
        return TARGETS[target_id]['os_capacity_bytes']
    except KeyError as error:
        raise ValueError('persistent image uses an unsupported target') from error


def record(payload, generation=1):
    if len(payload) > 480 or not 1 <= generation <= 0xffffffff:
        raise ValueError('invalid transactional record')
    data = bytearray(512)
    struct.pack_into('<4sIII', data, 0, b'STAT', 0, generation, len(payload))
    data[28:28 + len(payload)] = payload
    struct.pack_into('<I', data, 508, zlib.crc32(data[:508]))
    return bytes(data)


def initial_metadata(image):
    identity = inspect(image)
    if (len(image) > image_capacity(identity['target_id']) or
            identity['load_address'] != TARGETS[identity['target_id']]['load_address']):
        raise ValueError('persistent image exceeds the RAM staging policy')
    boot = bytearray(32)
    struct.pack_into('<4sIII3B', boot, 0, b'BOOT', 0, 0, 0, 0, 255, 0)
    struct.pack_into('<I', boot, 28, zlib.crc32(boot[:28]))
    payload = bytearray(184)
    struct.pack_into('<4sI', payload, 0, b'UPDT', 0)
    payload[8:40] = boot
    struct.pack_into('<II', payload, 40, len(image), ('release', 'dev', 'trace').index(identity['profile']))
    payload[48:80] = bytes.fromhex(identity['build_id'])
    payload[80:112] = hashlib.sha256(image).digest()
    return record(payload)


def _write_new(path, data, capacity):
    if len(data) > capacity:
        raise ValueError('file capacity exceeded')
    expected = hashlib.sha256()
    with path.open('xb') as stream:
        stream.write(data)
        expected.update(data)
        remaining = capacity - len(data)
        while remaining:
            chunk = bytes(min(65536, remaining))
            stream.write(chunk)
            expected.update(chunk)
            remaining -= len(chunk)
        stream.flush()
        os.fsync(stream.fileno())
    actual = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(65536), b''):
            actual.update(chunk)
    if path.stat().st_size != capacity or actual.digest() != expected.digest():
        raise OSError('provisioned file readback mismatch: ' + path.name)
    return dict(bytes=capacity, sha256=actual.hexdigest())


def prepare(image_path, directory):
    """Creates an immutable, locally inspectable file set, never accesses a disk."""
    image = image_path.read_bytes()
    identity = inspect(image)
    build = json.loads(image_path.with_suffix('.json').read_text())
    if (build.get('kind') != 'os' or build.get('target_id') != identity['target_id'] or
            build.get('profile') != identity['profile'] or
            build.get('build_id') != identity['build_id'] or
            build.get('image', {}).get('sha256') != identity['sha256']):
        raise ValueError('initial provisioning requires the matching OS build report')
    slot_bytes = image_capacity(identity['target_id'])
    metadata = initial_metadata(image)
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    payloads = ((image, slot_bytes), (b'', slot_bytes), (record(b''), 512),
                (record(b''), 512), (b'', SCRATCH_BYTES), (metadata, 512), (metadata, 512))
    entries = {name: _write_new(directory / name, data, size)
               for name, (data, size) in zip(FILES, payloads)}
    manifest = dict(target_id=identity['target_id'], os_capacity_bytes=slot_bytes,
                    scratch_bytes=SCRATCH_BYTES, image=identity,
                    files=entries,
                    purpose='Initial empty installation only; boot metadata is copied last.')
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def _mount_identity(mount, serial):
    if os.name != 'nt' or not re.fullmatch(r'[A-Za-z0-9-]{6,32}', serial):
        raise ValueError('provision requires Windows and an explicit physical disk serial')
    resolved = mount.resolve(strict=True)
    if not re.fullmatch(r'[A-Za-z]:\\', str(resolved)):
        raise ValueError('mount must be a drive root')
    from tools.inspect.retail_device import detect
    from tools.content.publish import inspect_target
    device=detect(resolved)
    if device['device_id'] != serial:
        raise ValueError('Physical serial mismatch')
    storage=device['storage']
    # Obtain the volume serial through the ordinary logical-volume API.
    from ctypes import wintypes as w
    import ctypes
    value=w.DWORD()
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetVolumeInformationW.argtypes=[w.LPCWSTR,w.LPWSTR,w.DWORD,ctypes.POINTER(w.DWORD),ctypes.c_void_p,ctypes.c_void_p,w.LPWSTR,w.DWORD]
    kernel.GetVolumeInformationW.restype=w.BOOL
    if not kernel.GetVolumeInformationW(str(resolved),None,0,ctypes.byref(value),None,None,None,0):
        raise ctypes.WinError(ctypes.get_last_error())
    checked=inspect_target(storage['disk_number'],serial,f'{value.value:08X}',resolved.drive)
    return resolved,dict(disk_number=checked['disk_number'],serial=serial,size=checked['disk_bytes'],
                         partition_offset=checked['partition_offset'],partition_size=checked['partition_bytes'],
                         volume_id=checked['volume_unique_id'],target_id=device['target_id'])



def provision(package, mount, serial):
    from tools.content.publish import _plain
    package = package.resolve(strict=True)
    manifest = json.loads((package / 'manifest.json').read_text())
    if (manifest.get('target_id') != manifest.get('image', {}).get('target_id') or
            set(manifest.get('files', {})) != set(FILES)):
        raise ValueError('invalid provisioning manifest')
    slot_bytes = image_capacity(manifest['target_id'])
    if (manifest.get('os_capacity_bytes') != slot_bytes or
            manifest.get('scratch_bytes') != SCRATCH_BYTES):
        raise ValueError('provisioning manifest uses stale target capacities')
    content = {}
    for name in FILES:
        path = package / name
        if path.is_symlink():
            raise ValueError('package links are unsupported')
        data = path.read_bytes()
        entry = manifest['files'][name]
        expected_size = slot_bytes if name.startswith('OS') else SCRATCH_BYTES if name == 'SCRATCH.BIN' else 512
        if len(data) != expected_size or entry != dict(bytes=expected_size, sha256=hashlib.sha256(data).hexdigest()):
            raise ValueError('package identity mismatch: ' + name)
        content[name] = data
    length = 128 + struct.unpack_from('<I', content['OS0.LOS'], 20)[0]
    image = content['OS0.LOS'][:length]
    if inspect(image) != manifest['image'] or any(content['OS0.LOS'][length:]) or any(content['OS1.LOS']):
        raise ValueError('initial OS content mismatch')
    if content['BOOT0.BIN'] != initial_metadata(image) or content['BOOT1.BIN'] != content['BOOT0.BIN']:
        raise ValueError('initial metadata mismatch')
    if any(content[name] != record(b'') for name in ('STATE0.BIN', 'STATE1.BIN')) or any(content['SCRATCH.BIN']):
        raise ValueError('initial state/scratch content mismatch')
    root, identity = _mount_identity(mount, serial)
    if identity['target_id'] != manifest['target_id']:
        raise ValueError('Provisioning package targets a different device')
    destination = root / '.LINNET'
    _plain(destination, root)
    if destination.exists() and (destination.is_symlink() or destination.resolve().parent != root):
        raise ValueError('unsafe LINNET directory')
    destination.mkdir(exist_ok=True)
    written = {}
    preserved = {}
    for name in FILES:  # boot records last: unpublished partial preparation remains unbootable
        _, current = _mount_identity(mount, serial)
        if current != identity:
            raise ValueError('disk identity changed during provisioning')
        target = destination / name
        _plain(target, root)
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file() or target.read_bytes() != content[name]:
                raise ValueError('existing installation file differs; refusing to overwrite: ' + name)
            preserved[name] = manifest['files'][name]
        else:
            written[name] = _write_new(target, content[name], len(content[name]))
    return dict(passed=True, disk=identity, files=written, image=manifest['image'],
                preserved=preserved, requires_safe_eject=True)
