"""Guarded, additive provisioning of optional rollback history records."""
import hashlib
from pathlib import Path
import re
import struct
import zlib

from tools.studio.storage import _mount_identity, _write_new, record
from tools.studio.targets import TARGETS


def _decode_record(data):
    if len(data) != 512:
        raise ValueError('transactional record must contain exactly 512 bytes')
    magic, reserved, generation, length = struct.unpack_from('<4sIII', data)
    if (magic != b'STAT' or reserved or not generation or length > 480
            or any(data[16:28]) or any(data[28 + length:508])
            or zlib.crc32(data[:508]) != struct.unpack_from('<I', data, 508)[0]):
        raise ValueError('invalid transactional record')
    return generation, data[28:28 + length]


def _read_record(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != 512:
        raise ValueError('unsafe or incorrectly sized record: ' + path.name)
    data = path.read_bytes()
    generation, payload = _decode_record(data)
    return data, generation, payload


def _boot_record(path, image_capacity=None):
    if image_capacity is None:
        image_capacity = max(spec['os_capacity_bytes'] for spec in TARGETS.values())
    data, generation, payload = _read_record(path)
    if len(payload) != 184 or struct.unpack_from('<4sI', payload) != (b'UPDT', 0):
        raise ValueError('invalid boot metadata: ' + path.name)
    boot = payload[8:40]
    if (struct.unpack_from('<4sI', boot) != (b'BOOT', 0) or any(boot[19:28])
            or zlib.crc32(boot[:28]) != struct.unpack_from('<I', boot, 28)[0]):
        raise ValueError('invalid boot state: ' + path.name)
    confirmed, candidate, pending = boot[16:19]
    if (confirmed not in (0, 1) or candidate not in (0, 1, 255)
            or candidate == confirmed or pending not in (0, 1)
            or (pending and candidate == 255)):
        raise ValueError('invalid boot slot state: ' + path.name)
    images = []
    for slot in range(2):
        image = payload[40 + slot * 72:112 + slot * 72]
        length, profile = struct.unpack_from('<II', image)
        if ((not length and any(image))
                or (length and (length < 132 or length > image_capacity or profile > 2))):
            raise ValueError('invalid boot image metadata: ' + path.name)
        images.append((length, image[8:40].hex()))
    if not images[confirmed][0] or (candidate != 255 and not images[candidate][0]):
        raise ValueError('boot slot has no image: ' + path.name)
    return data, generation, images[confirmed][1], pending


def _boot_snapshot(directory, expected_build):
    copies = [_boot_record(directory / name) for name in ('BOOT0.BIN', 'BOOT1.BIN')]
    if copies[0][1] == copies[1][1] and copies[0][0] != copies[1][0]:
        raise ValueError('conflicting boot records at the same generation')
    selected = max(copies, key=lambda item: item[1])
    if selected[2] != expected_build or selected[3]:
        raise ValueError('confirmed build mismatch or candidate attempt pending')
    return tuple(item[0] for item in copies)


def _history_record(path):
    data, generation, payload = _read_record(path)
    if payload:
        if len(payload) != 60:
            raise ValueError('invalid rollback history payload size: ' + path.name)
        magic, reserved, reason, failed, fallback, attempt, boot_generation = struct.unpack_from('<4s6I', payload)
        if (magic != b'RLBK' or reserved or reason not in (1, 2)
                or failed not in (0, 1) or fallback not in (0, 1, 255)):
            raise ValueError('invalid rollback history payload: ' + path.name)
    return data, generation


def provision(mount, serial, expected_size, expected_confirmed_build):
    """Create missing optional records, preserving every existing installation file.

    A partially completed earlier call is accepted only when all existing sidecar
    records validate. Failures leave any created files in place for inspection.
    """
    if (isinstance(expected_size, bool) or not isinstance(expected_size, int)
            or expected_size <= 0 or expected_size % 512):
        raise ValueError('expected disk size must be a positive exact sector multiple')
    if not isinstance(expected_confirmed_build, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', expected_confirmed_build):
        raise ValueError('expected confirmed build must be a full SHA-256 identity')
    expected_confirmed_build = expected_confirmed_build.lower()
    mount = Path(mount)
    root, identity = _mount_identity(mount, serial)
    if identity.get('size') != expected_size:
        raise ValueError('exact disk size mismatch')
    directory = root / '.LINNET'
    if (directory.is_symlink() or not directory.is_dir()
            or directory.resolve().parent != root):
        raise ValueError('unsafe or missing LINNET directory')
    before = _boot_snapshot(directory, expected_confirmed_build)
    names = ('UPDATE0.BIN', 'UPDATE1.BIN')
    existing = {}
    for name in names:
        path = directory / name
        if path.exists() or path.is_symlink():
            existing[name] = _history_record(path)
    if (len(existing) == 2 and existing[names[0]][1] == existing[names[1]][1]
            and existing[names[0]][0] != existing[names[1]][0]):
        raise ValueError('conflicting rollback records at the same generation')
    written = {}
    for name in names:
        if name in existing:
            continue
        current_root, current = _mount_identity(mount, serial)
        if current_root != root or current != identity:
            raise ValueError('disk identity changed during rollback history provisioning')
        if directory.is_symlink() or directory.resolve().parent != root:
            raise ValueError('LINNET directory changed during provisioning')
        if _boot_snapshot(directory, expected_confirmed_build) != before:
            raise ValueError('boot metadata changed during rollback history provisioning')
        for prior, (data, _) in existing.items():
            if _history_record(directory / prior)[0] != data:
                raise ValueError('rollback history changed during provisioning')
        # Exclusive creation deliberately refuses a file that appeared since inspection.
        initial = record(b'')
        # A valid generation-one history cannot share its generation with an
        # empty record. Preserve that record in the missing redundant copy.
        if existing:
            data, generation = next(iter(existing.values()))
            if generation == 1:
                initial = data
        written[name] = _write_new(directory / name, initial, 512)
        existing[name] = _history_record(directory / name)
    current_root, current = _mount_identity(mount, serial)
    if current_root != root or current != identity or _boot_snapshot(directory, expected_confirmed_build) != before:
        raise ValueError('disk identity or boot metadata changed after provisioning')
    for name, (data, _) in existing.items():
        if _history_record(directory / name)[0] != data:
            raise ValueError('rollback history changed after provisioning')
    return dict(passed=True, disk=identity, confirmed_build=expected_confirmed_build,
                created=written, preserved=[name for name in names if name not in written],
                files={name: dict(bytes=512, sha256=hashlib.sha256(existing[name][0]).hexdigest()) for name in names},
                requires_safe_eject=True)
