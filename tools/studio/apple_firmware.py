"""Publish a target-bound, verified Apple firmware fallback on an iPod volume."""
import hashlib
import os
from pathlib import Path
import struct
import zlib

from tools.studio.targets import TARGETS


def digest(data):
    return hashlib.sha256(data).hexdigest()


def matches_signature(target_id, payload):
    """Apply an optional target-owned marker for the archived Apple format."""
    signature = TARGETS[target_id]["retail"].get("firmware_signature")
    if signature is None:
        return True
    marker = signature["ascii"].encode("ascii")
    start = signature["offset"]
    return payload[start:start + len(marker)] == marker


def encode(target_id, payload, expected_sha256=None):
    if target_id not in TARGETS:
        raise ValueError("Apple firmware fallback target is unsupported")
    payload = bytes(payload)
    actual = digest(payload)
    if (expected_sha256 is not None and actual != expected_sha256.lower()) or not 32 <= len(payload) <= TARGETS[target_id]["stage_bytes"]:
        raise ValueError("Apple firmware fallback failed its reviewed bounds or hash")
    if not matches_signature(target_id, payload):
        raise ValueError("Apple firmware is missing its target's image signature")
    config = bytearray(512)
    config[:8] = b"APPLEOS1"
    struct.pack_into("<II", config, 8, TARGETS[target_id]["image_target"], len(payload))
    config[16:48] = hashlib.sha256(payload).digest()
    struct.pack_into("<I", config, 508, zlib.crc32(config[:508]))
    return payload, bytes(config)


def publish(root, target_id, payload, expected_sha256=None):
    root = Path(root).resolve(strict=True)
    system = root / ".LINNET"
    if not system.is_dir() or system.is_symlink():
        raise ValueError("the mounted iPod has no ordinary .LINNET system directory")
    payload, config = encode(target_id, payload, expected_sha256)
    from tools.content.publish import _plain
    published = []
    for name, data in (("APPLE.BIN", payload), ("APPLE.CFG", config)):
        target = system / name
        _plain(target, root)
        if target.exists():
            if target.is_symlink() or target.read_bytes() != data:
                raise ValueError("the existing Apple firmware fallback differs: " + name)
        else:
            temporary = target.with_name(name + ".new")
            _plain(temporary, root)
            if temporary.exists():
                raise ValueError("Apple firmware fallback staging already exists: " + name)
            with temporary.open("xb") as writer:
                writer.write(data)
                writer.flush()
                os.fsync(writer.fileno())
            if temporary.read_bytes() != data:
                raise OSError("Apple firmware fallback staging failed verification: " + name)
            temporary.replace(target)
        if target.read_bytes() != data:
            raise OSError("Apple firmware fallback failed verification: " + name)
        published.append(dict(path=str(target.relative_to(root)).replace("\\", "/"),
                              bytes=len(data), sha256=digest(data)))
    return dict(target_id=target_id, payload_sha256=digest(payload), files=published)


def video_resources(firmware, entries):
    """Extract only the Broadcom resource versions admitted by the M25 driver."""
    resources = [entry for entry in entries if entry['type'] == 'crsr']
    if len(resources) != 1:
        return {}
    entry = resources[0]
    data = firmware[entry['payload_offset']:entry['payload_offset'] + entry['payload_bytes']]
    if len(data) != entry['payload_bytes'] or sum(data) & 0xffffffff != entry['checksum']:
        raise ValueError('Apple video resource checksum mismatch')
    # Resource offsets are relative to the entry point, including the extra
    # 1536-byte prefix in the 80GB's 2048-byte-sector firmware partition.
    versions = (
        ('VMCS.BIN', 3983872, 201376, 'cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be'),
        ('MPLAYER.VLL', 4497920, 51620, 'c100e0cb0782277f81d2b9bceb29340df70d2993e34f77248a8e3c1979bc1fc6'),
        ('H264DEC.VLL', 4241920, 106960, '3915ec9295dc0d9cf9405810a66788e5b544c819347e05a350df6857120a9e8f'),
        ('MPG4DEC.VLL', 4350464, 147232, '08ba2f33f267803746c3b3de025e615f4a4b899611d7a4e3eea819f39d3697fe'),
        ('AACDEC.VLL', 4188672, 52664, '0a8c63080120a8af36a2d68ed4ade7eae81eee15f1e344f66cd49a508162c2cc'),
        ('PASSTHRU.VLL', 4551168, 6528, '0b657b1a4b50198da3ad3d55ecf9d239ffbae8d7da7b3aaca138dcdae5b6d019'),
    )
    result = {}
    for name, offset, size, expected in versions:
        offset += entry['entry_offset']
        payload = data[offset:offset + size]
        if len(payload) != size or digest(payload) != expected:
            return {}
        result[name] = payload
    return result


def publish_video_resources(identity, firmware, entries):
    from tools.content.publish import _plain, _volume_guard
    payloads = video_resources(firmware, entries)
    if not payloads:
        return dict(available=False, reason='Apple video resource version is unsupported')
    root = Path(identity['root']).resolve(strict=True)
    directory = root / '.LINNET' / 'BCM'
    _volume_guard(identity)
    _plain(directory, root)
    directory.mkdir(exist_ok=True)
    for name, data in payloads.items():
        _volume_guard(identity)
        path = directory / name
        _plain(path, root)
        if path.exists():
            if path.read_bytes() != data:
                raise ValueError('Existing video resource differs: ' + name)
            continue
        temporary = path.with_suffix(path.suffix + '.new')
        _plain(temporary, root)
        with temporary.open('xb') as writer:
            writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        if temporary.read_bytes() != data:
            raise OSError('Video resource readback failed: ' + name)
        _volume_guard(identity)
        temporary.replace(path)
        if path.read_bytes() != data:
            raise OSError('Published video resource readback failed: ' + name)
    return dict(available=True, files={name: digest(data) for name, data in payloads.items()})


def available(root, target_id):
    """Match Boot's on-disk Apple fallback checks before listing it in Media Manager."""
    if target_id not in TARGETS:
        return False
    system = Path(root) / ".LINNET"
    config_path, image_path = system / "APPLE.CFG", system / "APPLE.BIN"
    try:
        if not config_path.is_file() or not image_path.is_file():
            return False
        if config_path.is_symlink() or image_path.is_symlink():
            return False
        config = config_path.read_bytes()
        if len(config) != 512 or config[:8] != b"APPLEOS1" or any(config[48:508]):
            return False
        target, size = struct.unpack_from("<II", config, 8)
        if (target != TARGETS[target_id]["image_target"] or not 32 <= size <= TARGETS[target_id]["stage_bytes"] or
                image_path.stat().st_size != size or zlib.crc32(config[:508]) != struct.unpack_from("<I", config, 508)[0]):
            return False
        image = image_path.read_bytes()
        return (len(image) == size and hashlib.sha256(image).digest() == config[16:48] and
                matches_signature(target_id, image))
    except OSError:
        # The volume may disappear during a Media Manager refresh.
        return False
