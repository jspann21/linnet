"""Strict host reader for the Linnet OS envelope; no device writes."""
import hashlib
import json
from pathlib import Path
import struct
import zlib
from tools.studio.targets import TARGET_IDS_BY_NUMBER as TARGETS, TARGET_NUMBERS, TARGETS as TARGET_SPECS

PROFILES = ("release", "dev", "trace")

def pack(payload, profile, build_id, load, entry, target="photo4g-p98"):
    if target not in TARGET_NUMBERS:
        raise ValueError(f"unsupported image target: {target}")
    header = bytearray(128)
    struct.pack_into("<4sHH7I", header, 0, b"LNOS", 0, 128, TARGET_NUMBERS[target], 0,
                     PROFILES.index(profile), len(payload), load, entry,
                     zlib.crc32(payload))
    build_digest = bytes.fromhex(build_id)
    if len(build_digest) != 32:
        raise ValueError("build ID must decode to exactly 32 bytes")
    header[36:68] = build_digest
    struct.pack_into("<I", header, 124, zlib.crc32(header[:124]))
    return bytes(header) + payload

def inspect(data):
    if len(data) < 128:
        raise ValueError("truncated LOS header")
    magic, reserved16, size, target, reserved32, profile, length, load, entry, crc = struct.unpack_from("<4sHH7I", data)
    if (magic, reserved16, size, reserved32) != (b"LNOS", 0, 128, 0) or target not in TARGETS:
        raise ValueError("unsupported image identity")
    if profile >= len(PROFILES) or any(data[68:124]):
        raise ValueError("invalid profile or reserved bytes")
    if len(data) != 128 + length or length < 4:
        raise ValueError("invalid payload size")
    raw = memoryview(data)
    if zlib.crc32(raw[:124]) != struct.unpack_from("<I", data, 124)[0] or zlib.crc32(raw[128:]) != crc:
        raise ValueError("image CRC mismatch")
    spec = TARGET_SPECS[TARGETS[target]]
    if load % 4 or entry % 4 or not (spec["load_address"] <= load <= entry <= load + length - 4 and
                                      load + length <= spec["load_address"] + spec["os_capacity_bytes"]):
        raise ValueError("invalid image address range")
    return dict(target_id=TARGETS[target], profile=PROFILES[profile],
                build_id=data[36:68].hex(), payload_bytes=length,
                load_address=load, entry_address=entry,
                sha256=hashlib.sha256(data).hexdigest())

def verify_device_report(path, image, serial, target):
    """Bind a development image to its build report and the Link device."""
    report_path = Path(path).with_suffix('.json')
    try:
        with report_path.open('rb') as source:
            raw = source.read(8 * 1024 * 1024 + 1)
    except OSError as exc:
        raise ValueError('firmware image needs its adjacent build report') from exc
    if len(raw) > 8 * 1024 * 1024:
        raise ValueError('firmware build report exceeds its bound')
    report = json.loads(raw)
    expected_serial = '' if image['profile'] == 'release' else serial
    if (image['target_id'] != target or
            not isinstance(report, dict) or
            report.get('device_serial') != expected_serial or
            report.get('kind') != 'os' or
            any(report.get(key) != image[key] for key in ('target_id', 'profile', 'build_id')) or
            not isinstance(report.get('image'), dict) or
            report['image'].get('sha256') != image['sha256']):
        raise ValueError('firmware build report does not match this Link device and image')

def verify_release(data, symbols):
    identity = inspect(data)
    if identity["profile"] != "release":
        raise ValueError("release verifier requires release profile")
    forbidden = (b"LINK", b"LNID", b"linnet-link", b"ln_link_", b"ln_development_", b"ln_recovery_", b"devlink_", b"trace_",
                 b"inject_", b"screenshot_", b"chainload", b".dev_staging", b".dev_trace", b"ln_usb_", b"ln_crash_mailbox", b"ln_crash_encode", b"ln_crash_decode",
                 b"ln_lcd_diagnostics", b"ln_ata_diagnostics", b"history_store", b"sync_max_wait_us", b"transfer_max_us", b"max_present_us")
    if TARGET_SPECS[identity["target_id"]].get("hal_family") in ("pp502x", "s5l8702"):
        # Release Disk Mode uses the same low-level USB owner as Link.
        # Link dispatch, protocol and development endpoints remain forbidden.
        forbidden = tuple(item for item in forbidden if item != b"ln_usb_")
    haystack = data[128:] + symbols.encode("utf-8")
    hits = [item.decode() for item in forbidden if item in haystack]
    if hits:
        raise ValueError("release exclusion failure: " + ", ".join(hits))
    return dict(passed=True, checked_patterns=[item.decode() for item in forbidden], **identity)
