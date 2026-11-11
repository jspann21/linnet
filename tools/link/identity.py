"""Linnet Link running-image identity shared by discovery and campaigns."""
import struct
import zlib
from tools.studio.targets import TARGET_IDS_BY_NUMBER as TARGETS

IDENTITY = struct.Struct("<4s7I32s32s")
CAP_UPLOAD = 1
CAP_RAM_BOOT = 2
CAP_CRASH = 4
CAP_REBOOT_OS = 8
CAP_REBOOT_BOOT = 16
CAP_REBOOT_DISK = 32
HEALTHY = 31

def decode(data):
    if len(data) != IDENTITY.size:
        raise ValueError("invalid Linnet identity response length")
    magic, target, reserved, profile, kind, health, generation, capabilities, build, sha = IDENTITY.unpack(data)
    if magic != b"LNID" or target not in TARGETS or reserved or profile > 2 or kind > 1 or health & ~HEALTHY or capabilities & ~63:
        raise ValueError("unsupported Linnet identity")
    return dict(target_id=TARGETS[target], profile=("release", "dev", "trace")[profile],
                image_kind=("os", "boot")[kind], health_flags=health, healthy=(health & HEALTHY) == HEALTHY,
                generation=generation, capabilities=capabilities, build_id=build.hex(), image_sha256=sha.hex())


def decode_crash(data):
    if len(data) != 144:
        raise ValueError("invalid Linnet crash response length")
    if struct.unpack_from('<4sHH', data) != (b'CRSH', 0, 144) or any(data[132:140]):
        raise ValueError("invalid Linnet crash header or reserved bytes")
    if zlib.crc32(data[:140]) != struct.unpack_from('<I', data, 140)[0]:
        raise ValueError("Linnet crash CRC mismatch")
    sequence, reason = struct.unpack_from('<II', data, 8)
    generation, service, event = struct.unpack_from('<III', data, 120)
    result = dict(sequence=sequence, reason=reason, generation=generation,
                build_id=data[88:120].hex(), active_service=service, last_event=event,
                registers=list(struct.unpack_from('<16I', data, 16)),
                cpsr=struct.unpack_from('<I', data, 80)[0], flags=struct.unpack_from('<I', data, 84)[0])
    if reason == 0xb0070001 and result['flags'] == 2:
        result['record_kind'] = 'interrupted-startup'
        result['startup_trace'] = dict(zip(('phase', 'image_bytes', 'status', 'elapsed_us'),
                                          result['registers'][:4]), last_marker=event)
    return result
