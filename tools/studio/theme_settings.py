"""Guarded, additive provisioning of optional theme selection records."""
import hashlib
from pathlib import Path
import re
import struct

from tools.studio.storage import _mount_identity, _write_new, record
from tools.studio.update_history import _boot_snapshot, _read_record


def _theme_record(path):
    data, generation, payload = _read_record(path)
    from .device_preferences import decode_payload
    decode_payload(payload)
    return data, generation


def provision(mount, serial, expected_size, expected_confirmed_build):
    """Create missing THEME copies without modifying existing installation files.

    Empty records use the system theme. A valid selection is copied when repairing
    a missing partner so equal transaction generations never conflict.
    """
    if (isinstance(expected_size, bool) or not isinstance(expected_size, int)
            or expected_size <= 0 or expected_size % 512):
        raise ValueError('expected disk size must be a positive exact sector multiple')
    if (not isinstance(expected_confirmed_build, str)
            or not re.fullmatch(r'[0-9a-fA-F]{64}', expected_confirmed_build)):
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
    names = ('THEME0.BIN', 'THEME1.BIN')
    existing = {}
    for name in names:
        path = directory / name
        if path.exists() or path.is_symlink():
            existing[name] = _theme_record(path)
    if (len(existing) == 2 and existing[names[0]][1] == existing[names[1]][1]
            and existing[names[0]][0] != existing[names[1]][0]):
        raise ValueError('conflicting theme records at the same generation')

    def unchanged():
        current_root, current = _mount_identity(mount, serial)
        if current_root != root or current != identity:
            raise ValueError('disk identity changed during theme provisioning')
        if (directory.is_symlink() or not directory.is_dir()
                or directory.resolve().parent != root):
            raise ValueError('LINNET directory changed during theme provisioning')
        if _boot_snapshot(directory, expected_confirmed_build) != before:
            raise ValueError('boot metadata changed during theme provisioning')
        for name, (data, _) in existing.items():
            if _theme_record(directory / name)[0] != data:
                raise ValueError('theme selection changed during provisioning')

    written = {}
    for name in names:
        if name in existing:
            continue
        unchanged()
        initial = record(b'')
        if existing:
            data, generation = next(iter(existing.values()))
            if generation == 1:
                initial = data
        written[name] = _write_new(directory / name, initial, 512)
        existing[name] = _theme_record(directory / name)
    unchanged()
    return dict(passed=True, disk=identity, confirmed_build=expected_confirmed_build,
                created=written, preserved=[name for name in names if name not in written],
                files={name: dict(bytes=512, sha256=hashlib.sha256(existing[name][0]).hexdigest()) for name in names},
                requires_safe_eject=True)
