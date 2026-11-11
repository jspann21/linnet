"""Read and publish the native Boot startup default on a mounted iPod."""
from __future__ import annotations

import os
from pathlib import Path
import struct
import uuid
import zlib

from tools.studio.targets import TARGETS


FILE = Path('.LINNET/STARTUP.CFG')
CHOICES = {'linnet': 1, 'rockbox': 2, 'apple': 3}
MAGIC = b'LNSTART1'


def _encode(target_id, choice):
    data = bytearray(512)
    data[:8] = MAGIC
    struct.pack_into('<II', data, 8, TARGETS[target_id]['image_target'], CHOICES[choice])
    struct.pack_into('<I', data, 508, zlib.crc32(data[:508]))
    return bytes(data)


def _decode(root, target_id):
    from tools.content.publish import _plain

    path = root / FILE
    _plain(path, root)
    if not path.is_file():
        return 'linnet'
    if path.is_symlink() or path.stat().st_size != 512:
        return 'linnet'
    data = path.read_bytes()
    if (len(data) != 512 or data[:8] != MAGIC or any(data[16:508]) or
            struct.unpack_from('<I', data, 8)[0] != TARGETS[target_id]['image_target'] or
            zlib.crc32(data[:508]) != struct.unpack_from('<I', data, 508)[0]):
        return 'linnet'
    choice = struct.unpack_from('<I', data, 12)[0]
    return next((name for name, value in CHOICES.items() if value == choice), 'linnet')


def _rockbox_available(root, marker, review):
    from tools.content.publish import _plain
    from tools.content.music import device_media_path

    if (marker.get('strategy') not in ('native-directory','native-nor','rockbox-file') or
            not review['rockbox'].get('selector_compatible')):
        return False
    try:
        configured_path = device_media_path(Path(review['rockbox']['boot_file']), root)
    except (KeyError,OSError,ValueError):
        return False
    try:
        encoded = configured_path.encode('ascii')
    except UnicodeEncodeError:
        return False
    if not encoded or len(encoded) > 255 or not encoded.startswith(b'/'):
        return False
    config = root / '.LINNET/RBPATH.CFG'
    _plain(config, root)
    if not config.is_file() or config.is_symlink() or config.stat().st_size != 512:
        return False
    data = config.read_bytes()
    return (data[:8] == b'RBPATH1\0' and
            struct.unpack_from('<I', data, 8)[0] == len(encoded) and
            data[12:12 + len(encoded)] == encoded and data[12 + len(encoded)] == 0 and
            zlib.crc32(data[:508]) == struct.unpack_from('<I', data, 508)[0])


def inspect(review):
    """Expose only defaults supported by the installed Boot and current files."""
    from tools.studio.apple_firmware import available as apple_available

    marker = review.get('recovery') or {}
    identity = review['identity']
    supported = (review['linnet']['state'] == 'installed' and
                 marker.get('phase') == 'installed' and
                 marker.get('startup_default_format') == 1 and
                 not review['rockbox'].get('supersedes_linnet_recovery'))
    choices = ['linnet']
    if supported:
        root = Path(identity['root']).resolve(strict=True)
        rockbox_configured = _rockbox_available(root, marker, review)
        if (rockbox_configured or marker.get('strategy') in ('native-directory','native-nor','rockbox-file') and
                review['rockbox'].get('selector_compatible')):
            choices.append('rockbox')
        if apple_available(root, identity['target_id']):
            choices.append('apple')
        default = _decode(root, identity['target_id'])
        if default not in choices or default == 'rockbox' and not rockbox_configured:
            default = 'linnet'
    else:
        default = 'linnet'
    return dict(supported=supported, choices=choices, default=default)


def set_default(identity, choice):
    """Guard identity, publish one checksummed sector, and verify its readback."""
    from tools.content.media_manager import refresh
    from tools.content.publish import _exclusive, _plain, _volume_guard
    from tools.studio.installer import assess

    if choice not in CHOICES:
        raise ValueError('Unknown startup software')
    with _exclusive(identity):
        current = refresh(identity)
        review = assess(current, _validated_identity=True)
        state = inspect(review)
        if not state['supported'] or choice not in state['choices']:
            raise ValueError('This startup software is unavailable on the installed Boot')
        if choice == 'rockbox':
            from tools.studio.installer import _install_selector_config
            _volume_guard(current)
            _install_selector_config(current,boot_file=review['rockbox']['boot_file'],replace=True)
        root = Path(current['root']).resolve(strict=True)
        destination = root / FILE
        _plain(destination, root)
        if destination.exists() and (destination.is_symlink() or not destination.is_file()):
            raise ValueError('The startup setting path is unsafe')
        data = _encode(current['target_id'], choice)
        temporary = destination.with_name(destination.name + '.tmp-' + uuid.uuid4().hex)
        _plain(temporary, root)
        _volume_guard(current)
        temporary_owned = False
        try:
            with temporary.open('xb') as writer:
                temporary_owned = True
                writer.write(data)
                writer.flush()
                os.fsync(writer.fileno())
            if temporary.read_bytes() != data:
                raise OSError('Startup setting write failed verification')
            _volume_guard(current)
            os.replace(temporary, destination)
        finally:
            if temporary_owned and temporary.exists():
                _volume_guard(current)
                _plain(temporary, root)
                temporary.unlink()
        _volume_guard(current)
        if destination.read_bytes() != data or _decode(root, current['target_id']) != choice:
            raise OSError('Startup setting failed readback verification')
        return dict(default=choice, requires_safe_eject=True)
