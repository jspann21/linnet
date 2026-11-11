"""Plan and apply a target-neutral iPod firmware-directory Boot redirect."""
from datetime import datetime, timezone
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.inspect.image import inspect
from tools.studio.targets import TARGETS

SECTOR = 512
ARTIFACTS = {"mbr.bin", "boot.los", "candidate.bin", "directory-original.bin",
             "directory-candidate.bin", "boot-build-report.json", "device.json"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(size, sector=SECTOR):
    return (size + sector - 1) // sector * sector


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp-" + uuid.uuid4().hex)
    owned = False
    try:
        with temporary.open("x", encoding="utf-8", newline="\n") as writer:
            owned = True
            writer.write(json.dumps(value, indent=2, sort_keys=True) + "\n")
            writer.flush()
            os.fsync(writer.fileno())
        if os.name == "nt":
            import ctypes

            move = ctypes.WinDLL("kernel32", use_last_error=True).MoveFileExW
            move.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
            move.restype = ctypes.c_int
            if not move(os.path.abspath(temporary), os.path.abspath(path), 0x1 | 0x8):
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            temporary.replace(path)
        with path.open("r+b") as saved:
            os.fsync(saved.fileno())
    finally:
        if owned:
            temporary.unlink(missing_ok=True)


def _device(data):
    if (not isinstance(data, dict) or data.get("read_only") is not True or
            not isinstance(data.get("target_id"), str) or
            not isinstance(data.get("linnet_usb_serial"), str)):
        raise ValueError("invalid read-only device configuration")
    storage = data.get("storage", {})
    required = ("disk_number", "disk_serial", "disk_bytes", "sector_bytes",
                "partition_offset", "partition_bytes", "model", "bus_type")
    if (not isinstance(storage, dict) or any(key not in storage for key in required) or
            type(storage["sector_bytes"]) is not int or storage["sector_bytes"] not in (512, 2048, 4096)):
        raise ValueError("device configuration lacks supported storage geometry")
    return data


def parse_backup(firmware, mbr, device):
    storage = device["storage"]
    sector = storage["sector_bytes"]
    target = device.get("target_id")
    if target not in TARGETS:
        raise ValueError("device target has no manifest")
    install = TARGETS[target]["installer"]
    if not install["enabled"] or install["boot_layout"] != "ipod-directory":
        raise ValueError("device target does not enable the iPod directory installer")
    if len(mbr) != sector or mbr[510:512] != b"\x55\xaa":
        raise ValueError("invalid MBR backup")
    partitions = []
    for index in range(4):
        offset = 446 + index * 16
        kind = mbr[offset + 4]
        lba, sectors = struct.unpack_from("<II", mbr, offset + 8)
        if sectors:
            partitions.append((index, kind, lba, sectors))
    firmware_parts = [part for part in partitions if part[1] in install["firmware_partition_types"]]
    media_parts = [part for part in partitions if part[1] in install["media_partition_types"]]
    if len(firmware_parts) != 1 or len(media_parts) != 1:
        raise ValueError("expected one firmware and one media partition")
    _index, _kind, partition_lba, partition_sectors = firmware_parts[0]
    if (len(firmware) != partition_sectors * sector or
            media_parts[0][2] * sector != storage["partition_offset"] or
            media_parts[0][3] * sector != storage["partition_bytes"]):
        raise ValueError("backup and mounted media geometry disagree")
    if (len(firmware) > install["max_firmware_bytes"] or
            firmware[0x100:0x104] != bytes.fromhex(install["firmware_header_magic"]) or
            struct.unpack_from("<H", firmware, 0x10a)[0] != install["firmware_header_field"]):
        raise ValueError("unsupported Apple firmware header")
    directory_offset = struct.unpack_from("<I", firmware, 0x104)[0] + SECTOR
    if directory_offset % SECTOR or directory_offset + SECTOR > len(firmware):
        raise ValueError("invalid firmware directory offset")
    # Firmware directory pointers use a fixed 512-byte bias; image pointers
    # use the USB sector size. Preserve the entire containing disk sector.
    directory_start = directory_offset % sector
    directory_offset -= directory_start
    if directory_start + SECTOR > sector or directory_offset + sector > len(firmware):
        raise ValueError("firmware directory crosses its disk sector")
    directory = firmware[directory_offset:directory_offset + sector]
    entries = []
    for offset in range(directory_start, directory_start + SECTOR - 39, 40):
        entry = directory[offset:offset + 40]
        if entry[:4] == bytes(4):
            break
        if entry[:4] != b"!ATA":
            raise ValueError("unknown firmware directory entry")
        kind = entry[4:8]
        ident, dev_offset, size, address, start, checksum, header_field, load = struct.unpack_from("<8I", entry, 8)
        payload_offset = dev_offset + sector
        if not size or payload_offset < directory_offset + sector or payload_offset + size > len(firmware):
            raise ValueError("firmware image extent is outside its partition")
        payload = firmware[payload_offset:payload_offset + size]
        entries.append(dict(directory_entry_offset=offset, type=kind.decode("ascii"), id=ident,
                            dev_offset=dev_offset, payload_offset=payload_offset, payload_bytes=size,
                            address=address, entry_offset=start, checksum=checksum, header_field=header_field,
                            load_address=load, payload_sha256=sha(payload)))
    osos = [entry for entry in entries if entry["type"] == install["firmware_entry_type"]]
    if len(osos) != 1:
        raise ValueError("expected exactly one RetailOS directory entry")
    original = osos[0]
    payload = firmware[original["payload_offset"]:original["payload_offset"] + original["payload_bytes"]]
    if sum(payload) & 0xffffffff != original["checksum"]:
        raise ValueError("RetailOS additive checksum mismatch")
    return dict(partition_lba=partition_lba, partition_sectors=partition_sectors,
                directory_offset=directory_offset, entries=entries, original=original,
                directory=directory, installer_profile=install)


def original_boot(original, firmware):
    """Identify Rockbox's active OSOS entry independently of Utility logs."""
    start,size=original['payload_offset'],original['payload_bytes']
    entry=original['entry_offset']
    if start<0 or start+size>len(firmware) or entry<0 or entry%4 or entry+40>size:
        raise ValueError('Original firmware entry is outside its payload')
    active=firmware[start+entry:start+size]
    # The distributed PP bootloaders predate the runtime's offset-32 marker.
    # Their banner identifies an appended loader without a Utility log.
    return 'rockbox' if (active[32:40]==b'Rockbox\x01' or
                         b'Rockbox boot loader' in active) else 'apple'


def derive(firmware, mbr, image, device):
    parsed = parse_backup(firmware, mbr, device)
    sector = device["storage"]["sector_bytes"]
    identity = inspect(image)
    if identity["target_id"] != device["target_id"]:
        raise ValueError("Boot image target differs from detected device target")
    if identity["load_address"] != 0 or identity["entry_address"] != 0:
        raise ValueError("initial bootstrap requires an image linked at zero")
    payload = image[128:]
    if not 32 <= len(payload) <= 0x100000:
        raise ValueError("Boot payload exceeds the one MiB bootstrap bound")
    span = max(65536, align(len(payload), sector))
    first = align(max(entry["payload_offset"] + entry["payload_bytes"] for entry in parsed["entries"]), sector)
    if first + span > len(firmware):
        raise ValueError("firmware partition has no bounded staging extent after its images")
    # Keep existing plans stable by preferring their first all-zero extent.
    # Unreferenced trailing sectors need not be erased: the full backup pins
    # their original bytes, which guard_device verifies before publication.
    stage = next((offset for offset in range(first, len(firmware) - span + 1, sector)
                  if not any(firmware[offset:offset + span])), first)
    candidate = payload + bytes(span - len(payload))
    patched = bytearray(parsed["directory"])
    entry_offset = parsed["original"]["directory_entry_offset"]
    for field, value in ((12, stage - sector), (16, len(payload)),
                         (24, 0), (28, sum(payload) & 0xffffffff)):
        struct.pack_into("<I", patched, entry_offset + field, value)
    lba = parsed["partition_lba"]
    details = dict(image=identity, original_entries=parsed["entries"], original_osos=parsed["original"],
                   installer_profile=parsed["installer_profile"],
                   partition_lba=lba, partition_sectors=parsed["partition_sectors"],
                   directory_partition_offset=parsed["directory_offset"],
                   directory_disk_offset=lba * sector + parsed["directory_offset"],
                   candidate_partition_offset=stage, candidate_disk_offset=lba * sector + stage,
                   candidate_write_bytes=span, candidate_payload_bytes=len(payload),
                   candidate_dev_offset=stage - sector, candidate_checksum=sum(payload) & 0xffffffff,
                   directory_changed_field_offsets=[entry_offset + value for value in (12, 16, 24, 28)])
    if sector != SECTOR:
        details["sector_bytes"] = sector
    return details, candidate, parsed["directory"], bytes(patched)


def plan(firmware_path, mbr_path, device_path, boot_path, output):
    paths = [Path(value).resolve() for value in (firmware_path, mbr_path, device_path, boot_path, output)]
    firmware_path, mbr_path, device_path, boot_path, output = paths
    firmware, mbr, device_bytes, image = (firmware_path.read_bytes(), mbr_path.read_bytes(),
                                           device_path.read_bytes(), boot_path.read_bytes())
    device = _device(json.loads(device_bytes))
    details, candidate, original_sector, patched_sector = derive(firmware, mbr, image, device)
    report_path = boot_path.with_suffix(".json")
    report_bytes = report_path.read_bytes()
    report = json.loads(report_bytes)
    if (not isinstance(report, dict) or not isinstance(report.get("image"), dict) or
            (report.get("kind"), report.get("probe"), report.get("device_serial"),
         report.get("target_id"), report.get("build_id")) !=
            ("boot", "frame", "" if details["image"]["profile"] == "release" else device["linnet_usb_serial"], device["target_id"], details["image"]["build_id"]) or
            report["image"].get("sha256") != sha(image)):
        raise ValueError("matching storage Boot build report is required")
    if output.exists() and any(output.iterdir()):
        raise ValueError("plan output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    artifacts = {"mbr.bin": mbr, "boot.los": image, "candidate.bin": candidate,
                 "directory-original.bin": original_sector, "directory-candidate.bin": patched_sector,
                 "boot-build-report.json": report_bytes, "device.json": device_bytes}
    for name, data in artifacts.items():
        (output / name).write_bytes(data)
    storage = device["storage"]
    manifest = dict(operation="ipod-native-osos-redirect",
                    target_id=device["target_id"], expected_device_bytes=storage["disk_bytes"],
                    expected_storage_serial=storage["disk_serial"],
                    expected_linnet_usb_serial=device["linnet_usb_serial"],
                    firmware_backup=str(firmware_path), firmware_backup_sha256=sha(firmware),
                    device_write_attempted=False,
                    artifacts={name: dict(bytes=len(data), sha256=sha(data)) for name, data in artifacts.items()},
                    **details)
    manifest_path = output / "plan.json"
    save_json(manifest_path, manifest)
    return dict(passed=True, plan=str(manifest_path), plan_sha256=sha(manifest_path.read_bytes()),
                device_write_attempted=False, original_payload_preserved=True,
                candidate_disk_offset=manifest["candidate_disk_offset"],
                candidate_write_bytes=manifest["candidate_write_bytes"],
                directory_disk_offset=manifest["directory_disk_offset"])


def load_plan(path, expected_sha):
    path = Path(path).resolve()
    raw = path.read_bytes()
    if len(expected_sha) != 64 or sha(raw) != expected_sha.lower():
        raise ValueError("reviewed plan SHA-256 does not match")
    manifest = json.loads(raw)
    if not isinstance(manifest, dict) or manifest.get("operation") != "ipod-native-osos-redirect":
        raise ValueError("unsupported native installation plan")
    if not isinstance(manifest.get("artifacts"), dict) or set(manifest["artifacts"]) != ARTIFACTS:
        raise ValueError("unexpected plan artifact set")
    if not isinstance(manifest.get("firmware_backup"), str):
        raise ValueError("native installation plan lacks a firmware backup path")
    artifacts = {}
    for name in ARTIFACTS:
        data = (path.parent / name).read_bytes()
        if manifest["artifacts"][name] != dict(bytes=len(data), sha256=sha(data)):
            raise ValueError("plan artifact changed: " + name)
        artifacts[name] = data
    device = _device(json.loads(artifacts["device.json"]))
    firmware = Path(manifest["firmware_backup"]).read_bytes()
    details, candidate, original, patched = derive(
        firmware, artifacts["mbr.bin"], artifacts["boot.los"], device)
    if (manifest.get("firmware_backup_sha256") != sha(firmware) or
            any(manifest.get(key) != value for key, value in details.items()) or
            (candidate, original, patched) != (artifacts["candidate.bin"], artifacts["directory-original.bin"],
                                               artifacts["directory-candidate.bin"])):
        raise ValueError("plan no longer matches its independently derived inputs")
    return manifest, artifacts, firmware


def identify_disk(number, manifest):
    if os.name != "nt" or type(number) is not int or not 0 <= number <= 1024:
        raise RuntimeError("raw apply requires an explicit Windows disk number")
    script = (f"$d=Get-Disk -Number {number} -ErrorAction Stop; "
              "$d | Select-Object Number,FriendlyName,SerialNumber,Size,BusType,LogicalSectorSize | ConvertTo-Json -Compress")
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                            text=True, capture_output=True, timeout=15, check=True,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    disk = json.loads(result.stdout)
    observed = (disk.get("Number"), str(disk.get("SerialNumber", "")).strip(),
                int(disk.get("Size", 0)), int(disk.get("LogicalSectorSize", 0)))
    expected = (number, manifest["expected_storage_serial"], manifest["expected_device_bytes"], manifest.get("sector_bytes", SECTOR))
    if observed != expected or "ipod" not in str(disk.get("FriendlyName", "")).casefold() or disk.get("BusType") not in (7, "USB"):
        raise RuntimeError("selected disk differs from the reviewed iPod")
    return disk


def read_at(device, offset, length):
    device.seek(offset)
    # Full firmware verification can span over 80 MiB. Keep each raw USB disk
    # request small and sector aligned, while still checking every byte once.
    chunks = []
    transferred = 0
    while transferred < length:
        count = min(65536, length - transferred)
        try:
            data = device.read(count)
        except OSError as exc:
            raise OSError(exc.errno, f"raw read failed at {offset + transferred}, "
                          f"{transferred}/{length} bytes read: {exc}") from exc
        if len(data) != count:
            raise OSError(f"short raw read at {offset + transferred}: {len(data)}/{count} bytes")
        chunks.append(data)
        transferred += count
    return b''.join(chunks)


def _directory_records(directory):
    records = []
    for offset in range(0, SECTOR - 39, 40):
        entry = directory[offset:offset + 40]
        if entry[:4] == bytes(4):
            break
        if entry[:4] != b"!ATA":
            raise RuntimeError("firmware directory contains an unknown entry")
        values = struct.unpack_from("<8I", entry, 8)
        records.append(dict(offset=offset, type=entry[4:8], id=values[0],
                            dev_offset=values[1], length=values[2], address=values[3],
                            entry_offset=values[4], checksum=values[5],
                            header_field=values[6], load_address=values[7]))
    return records


def _rockbox_uninstalled_residue(observed, manifest, artifacts, original, directory):
    """Validate ipodpatcher's install-then-delete transformation over native Linnet."""
    if manifest.get("sector_bytes", SECTOR) != SECTOR:
        return None
    old = _directory_records(artifacts["directory-original.bin"])
    candidate = _directory_records(artifacts["directory-candidate.bin"])
    current = _directory_records(directory)
    if len(old) < 2 or len(current) != len(old) or len(candidate) != len(old):
        return None
    expected_osos, current_osos = candidate[0], current[0]
    aligned_linnet = align(expected_osos["length"])
    osos_fields = ("type", "id", "dev_offset", "address", "entry_offset", "checksum",
                   "header_field", "load_address")
    if (any(current_osos[key] != expected_osos[key] for key in osos_fields) or
            current_osos["length"] != aligned_linnet):
        return None
    deltas = []
    stable_fields = ("type", "id", "length", "address", "entry_offset", "checksum",
                     "header_field", "load_address")
    for prior, moved in zip(old[1:], current[1:]):
        if any(moved[key] != prior[key] for key in stable_fields):
            return None
        deltas.append(moved["dev_offset"] - prior["dev_offset"])
    if not deltas or len(set(deltas)) != 1 or deltas[0] <= 0 or deltas[0] % SECTOR:
        return None
    delta = deltas[0]
    source_start = old[1]["dev_offset"]
    source_end = old[-1]["dev_offset"] + SECTOR + align(old[-1]["length"])
    destination_start, destination_end = source_start + delta, source_end + delta
    stage = manifest["candidate_partition_offset"]
    bootloader_start = stage + aligned_linnet
    if (source_start < SECTOR or source_end <= source_start or
            destination_start <= bootloader_start or destination_end > len(original)):
        return None
    if (observed[source_start:source_end] != original[source_start:source_end] or
            observed[destination_start:destination_end] != original[source_start:source_end] or
            observed[stage:bootloader_start] != artifacts["candidate.bin"][:aligned_linnet] or
            not any(observed[bootloader_start:destination_start])):
        return None
    normalized = bytearray(observed)
    normalized[stage:destination_end] = original[stage:destination_end]
    normalized[manifest["directory_partition_offset"]:
               manifest["directory_partition_offset"] + SECTOR] = artifacts["directory-original.bin"]
    if normalized != original:
        return None
    return dict(kind="rockbox-uninstalled-over-native-linnet", start=stage,
                end=destination_end, source_start=source_start, source_end=source_end,
                destination_start=destination_start, destination_end=destination_end,
                shift_bytes=delta)


def _rockbox_residue_from_hint(observed, manifest, artifacts, original, directory, hint):
    """Re-derive a prior Rockbox residue; a receipt never defines ignored bytes."""
    if not isinstance(hint, dict) or manifest.get("sector_bytes", SECTOR) != SECTOR:
        return None
    shift = hint.get("shift_bytes")
    if type(shift) is not int or shift <= 0 or shift % SECTOR or shift >= len(original):
        return None
    old = _directory_records(artifacts["directory-original.bin"])
    candidate = _directory_records(artifacts["directory-candidate.bin"])
    if len(old) < 2 or len(candidate) != len(old):
        return None
    reconstructed = bytearray(artifacts["directory-candidate.bin"])
    struct.pack_into("<I", reconstructed, candidate[0]["offset"] + 16,
                     align(candidate[0]["length"]))
    for prior, entry in zip(old[1:], candidate[1:]):
        moved = prior["dev_offset"] + shift
        if moved > 0xffffffff:
            return None
        struct.pack_into("<I", reconstructed, entry["offset"] + 12, moved)
    reconstructed = bytes(reconstructed)
    if not all(value in (before, after) for value, before, after in
               zip(directory, artifacts["directory-original.bin"], reconstructed)):
        return None
    derived = _rockbox_uninstalled_residue(observed, manifest, artifacts, original, reconstructed)
    return derived if derived == hint else None


def _validate_residue(observed, original, residue):
    required = {"kind", "start", "end", "source_start", "source_end",
                "destination_start", "destination_end", "shift_bytes"}
    if (not isinstance(residue, dict) or set(residue) != required or
            residue.get("kind") != "rockbox-uninstalled-over-native-linnet"):
        raise RuntimeError("compatible firmware residue record is invalid")
    start, end = residue["start"], residue["end"]
    source_start, source_end = residue["source_start"], residue["source_end"]
    destination_start, destination_end = residue["destination_start"], residue["destination_end"]
    if (any(type(residue[key]) is not int for key in required - {"kind"}) or
            not 0 <= start < end <= len(original) or
            not 0 <= source_start < source_end <= len(original) or
            destination_end - destination_start != source_end - source_start or
            destination_start - source_start != residue["shift_bytes"] or
            observed[source_start:source_end] != original[source_start:source_end] or
            observed[destination_start:destination_end] != original[source_start:source_end]):
        raise RuntimeError("compatible firmware residue changed after validation")
    normalized = bytearray(observed)
    normalized[start:end] = original[start:end]
    return normalized


def guard_device(device, manifest, artifacts, original, *, allow_rockbox_uninstalled=False,
                 compatible_residue=None, allow_interrupted_restore=False, rockbox_residue_hint=None):
    sector = manifest.get("sector_bytes", SECTOR)
    if read_at(device, 0, sector) != artifacts["mbr.bin"]:
        raise RuntimeError("device MBR changed")
    partition_start = manifest["partition_lba"] * sector
    observed = read_at(device, partition_start, len(original))
    directory_offset = manifest["directory_partition_offset"]
    directory = observed[directory_offset:directory_offset + sector]
    allowed_directories = [artifacts["directory-original.bin"], artifacts["directory-candidate.bin"]]
    residue = compatible_residue
    if residue is not None and allow_interrupted_restore:
        residue = _rockbox_residue_from_hint(observed, manifest, artifacts, original,
                                             directory, residue)
        if residue is None:
            raise RuntimeError("compatible Rockbox residue changed after validation")
    if residue is None and allow_interrupted_restore and rockbox_residue_hint is not None:
        residue = _rockbox_residue_from_hint(observed, manifest, artifacts, original,
                                             directory, rockbox_residue_hint)
    interrupted_directory = False
    if directory not in allowed_directories:
        if residue is None and allow_rockbox_uninstalled:
            residue = _rockbox_uninstalled_residue(observed, manifest, artifacts, original, directory)
        interrupted_directory = (allow_interrupted_restore and residue is None and
                                 all(byte in (before, after) for byte, before, after in
                                     zip(directory, *allowed_directories)))
        if residue is None and not interrupted_directory:
            raise RuntimeError("firmware directory differs from both reviewed states")
    stage, span = manifest["candidate_partition_offset"], manifest["candidate_write_bytes"]
    staged = observed[stage:stage + span]
    original_stage = original[stage:stage + span]
    if residue is None and not allow_interrupted_restore and staged not in (original_stage, artifacts["candidate.bin"]):
        raise RuntimeError("staging region differs from both reviewed states")
    if (residue is None and not allow_interrupted_restore and
            directory == artifacts["directory-candidate.bin"] and staged != artifacts["candidate.bin"]):
        raise RuntimeError("published directory references absent candidate bytes")
    normalized = bytearray(observed)
    normalized[stage:stage + span] = original[stage:stage + span]
    normalized[directory_offset:directory_offset + sector] = artifacts["directory-original.bin"]
    if residue is not None:
        normalized = _validate_residue(observed, original, residue)
        normalized[directory_offset:directory_offset + sector] = artifacts["directory-original.bin"]
    if normalized != original:
        raise RuntimeError("firmware changed outside the reviewed regions")
    old = manifest["original_osos"]
    if sha(observed[old["payload_offset"]:old["payload_offset"] + old["payload_bytes"]]) != old["payload_sha256"]:
        raise RuntimeError("original RetailOS payload is no longer intact")
    directory_state = ("candidate" if directory == artifacts["directory-candidate.bin"] else
                       "original" if directory == artifacts["directory-original.bin"] else
                       "candidate-rockbox-uninstalled" if residue is not None else "interrupted")
    staging_state = ("candidate-rockbox-residue" if residue is not None else
                     "candidate" if staged == artifacts["candidate.bin"] else
                     "zero" if staged == original_stage == bytes(span) else
                     "original" if staged == original_stage else "interrupted")
    result = dict(directory=directory_state, staging=staging_state, original_payload_intact=True)
    if residue is not None:
        result["compatible_residue"] = residue
    return result


def write_verified(device, offset, payload, receipt, receipt_path, phase, sector=SECTOR):
    if sector not in (512, 2048, 4096) or offset % sector or len(payload) % sector:
        raise ValueError("raw writes must be complete aligned sectors")
    receipt.update(phase=phase, device_write_attempted=True)
    receipt["writes"].append(dict(offset=offset, bytes=len(payload), sha256=sha(payload), complete=False))
    save_json(receipt_path, receipt)
    device.seek(offset)
    written = device.write(payload)
    if written != len(payload):
        raise OSError("short raw write")
    device.flush()
    os.fsync(device.fileno())
    if read_at(device, offset, len(payload)) != payload:
        raise OSError("raw write readback mismatch")
    receipt["writes"][-1].update(returned_bytes=written, complete=True)
    save_json(receipt_path, receipt)


def _prior_restore_hints(plan_path, expected_sha, disk, manifest, artifacts, current_receipt):
    """Find interrupted restore intents for this plan and storage identity."""
    expected_write = (manifest["directory_disk_offset"],
                      len(artifacts["directory-original.bin"]),
                      sha(artifacts["directory-original.bin"]))
    expected_residue_keys = {"kind", "start", "end", "source_start", "source_end",
                             "destination_start", "destination_end", "shift_bytes"}
    for path in plan_path.parent.glob("restore-receipt-*.json"):
        try:
            if path == current_receipt or path.is_symlink() or not path.is_file() or path.stat().st_size > 1048576:
                continue
            saved = json.loads(path.read_text(encoding="utf-8"))
            recorded = saved.get("device", {})
            before = saved.get("before", {})
            hint = before.get("compatible_residue")
            recorded_sha = saved.get("reviewed_plan_sha256")
            if (saved.get("operation") != "restore" or saved.get("plan") != str(plan_path) or
                    not isinstance(recorded_sha, str) or recorded_sha.lower() != expected_sha.lower() or
                    not saved.get("device_write_attempted") or
                    recorded.get("SerialNumber", "").strip() != manifest["expected_storage_serial"] or
                    recorded.get("Size") != manifest["expected_device_bytes"] or
                    recorded.get("LogicalSectorSize") != manifest.get("sector_bytes", SECTOR) or
                    recorded.get("SerialNumber", "").strip() != str(disk.get("SerialNumber", "")).strip() or
                    before.get("original_payload_intact") is not True or
                    before.get("directory") != "candidate-rockbox-uninstalled" or
                    before.get("staging") != "candidate-rockbox-residue" or
                    not isinstance(hint, dict) or set(hint) != expected_residue_keys or
                    not any((entry.get("offset"), entry.get("bytes"), entry.get("sha256")) == expected_write
                            for entry in saved.get("writes", ()) if isinstance(entry, dict))):
                continue
            yield hint
        except (OSError, ValueError, TypeError, AttributeError):
            continue


def execute(mode, plan_path, expected_sha, disk_number):
    """Own the media publisher mutex for direct raw apply and restore calls."""
    from tools.content.guard import require_same_target
    from tools.content.publish import _exclusive
    from tools.content.media_manager import discover, refresh
    manifest, artifacts, _original = load_plan(plan_path, expected_sha)
    planned_device = _device(json.loads(artifacts["device.json"]))
    storage = planned_device["storage"]
    volume_id = storage.get("volume_id")
    if not isinstance(volume_id, str) or not volume_id:
        raise ValueError("native plan lacks a mounted volume identity for publisher exclusion")
    if (manifest['target_id'] != planned_device['target_id'] or
            manifest['expected_linnet_usb_serial'] != planned_device['linnet_usb_serial'] or
            manifest['expected_device_bytes'] != storage['disk_bytes']):
        raise ValueError('native plan hardware identity differs from its device record')
    # Windows assigns different volume IDs to Apple and Linnet Disk Mode, and
    # may assign another after reconnecting. Bind the current mount to the
    # reviewed board, mode-specific serial and complete partition geometry.
    expected = dict(disk_number=disk_number, target_id=manifest['target_id'],
                    linnet_usb_serial=manifest['expected_linnet_usb_serial'],
                    disk_serial=manifest['expected_storage_serial'],
                    disk_bytes=manifest['expected_device_bytes'],
                    sector_bytes=manifest.get('sector_bytes', SECTOR),
                    partition_offset=storage['partition_offset'],
                    partition_bytes=storage['partition_bytes'])
    matches = [item for item in discover()
               if all(item.get(key) == value for key, value in expected.items())]
    if len(matches) != 1:
        raise RuntimeError("native plan volume is not uniquely mounted")
    mounted = matches[0]
    if not mounted.get('volume_unique_id'):
        raise ValueError('mounted iPod lacks a volume identity for publisher exclusion')
    with _exclusive(mounted):
        observed = refresh(mounted)
        require_same_target(mounted, observed)
        if any(observed.get(key) != value for key, value in expected.items()):
            raise RuntimeError("native plan no longer matches the mounted iPod volume")
        return _execute(mode, plan_path, expected_sha, disk_number)


def _execute(mode, plan_path, expected_sha, disk_number):
    plan_path = Path(plan_path).resolve()
    receipt_path = plan_path.parent / (f"{mode}-receipt-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8] + ".json")
    receipt = dict(operation=mode, plan=str(plan_path), reviewed_plan_sha256=expected_sha,
                   started_at=datetime.now(timezone.utc).isoformat(), phase="validate-plan", passed=False,
                   device_write_attempted=False, writes=[])
    try:
        save_json(receipt_path, receipt)
        if mode not in ("apply", "restore"):
            raise ValueError("unsupported operation")
        manifest, artifacts, original = load_plan(plan_path, expected_sha)
        sector = manifest.get("sector_bytes", SECTOR)
        receipt["device"] = identify_disk(disk_number, manifest)
        with open(r"\\.\PhysicalDrive" + str(disk_number), "r+b", buffering=0) as device:
            try:
                receipt["before"] = guard_device(
                    device, manifest, artifacts, original,
                    allow_rockbox_uninstalled=mode == "restore",
                    allow_interrupted_restore=mode == "restore")
            except RuntimeError:
                if mode != "restore":
                    raise
                for hint in _prior_restore_hints(plan_path, expected_sha, receipt["device"],
                                                 manifest, artifacts, receipt_path):
                    try:
                        receipt["before"] = guard_device(
                            device, manifest, artifacts, original,
                            allow_interrupted_restore=True, rockbox_residue_hint=hint)
                    except RuntimeError:
                        continue
                    break
                else:
                    raise
            if mode == "apply":
                if receipt["before"]["staging"] != "candidate":
                    write_verified(device, manifest["candidate_disk_offset"], artifacts["candidate.bin"],
                                   receipt, receipt_path, "write-candidate", sector)
                staged = guard_device(device, manifest, artifacts, original)
                if staged["directory"] != "candidate":
                    write_verified(device, manifest["directory_disk_offset"], artifacts["directory-candidate.bin"],
                                   receipt, receipt_path, "publish-directory", sector)
            elif receipt["before"]["directory"] != "original":
                write_verified(device, manifest["directory_disk_offset"], artifacts["directory-original.bin"],
                               receipt, receipt_path, "restore-directory", sector)
            # A retry may see a completed write in cache after the previous flush
            # failed. Flush even when there is no new directory write.
            device.flush()
            os.fsync(device.fileno())
            receipt["after"] = guard_device(
                device, manifest, artifacts, original,
                compatible_residue=receipt["before"].get("compatible_residue"),
                allow_interrupted_restore=mode == "restore")
            expected = "candidate" if mode == "apply" else "original"
            if receipt["after"]["directory"] != expected:
                raise RuntimeError("directory did not reach the requested state")
        receipt.update(passed=True, phase="complete")
    except BaseException as exc:
        receipt["failure"] = f"{type(exc).__name__}: {exc}"
    finally:
        receipt["finished_at"] = datetime.now(timezone.utc).isoformat()
        receipt["receipt"] = str(receipt_path)
        save_json(receipt_path, receipt)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    planner = commands.add_parser("plan")
    planner.add_argument("--firmware-backup", type=Path, required=True)
    planner.add_argument("--mbr-backup", type=Path, required=True)
    planner.add_argument("--device", type=Path, required=True)
    planner.add_argument("--boot-los", type=Path, required=True)
    planner.add_argument("--out", type=Path, required=True)
    for mode in ("apply", "restore"):
        writer = commands.add_parser(mode)
        writer.add_argument("--plan", type=Path, required=True)
        writer.add_argument("--plan-sha256", required=True)
        writer.add_argument("--disk-number", type=int, required=True)
    args = parser.parse_args()
    if args.mode == "plan":
        result = plan(args.firmware_backup, args.mbr_backup, args.device, args.boot_los, args.out)
    else:
        result = execute(args.mode, args.plan, args.plan_sha256, args.disk_number)
    print(json.dumps(result, indent=2))
    return 0 if result.get("passed", True) else 2


if __name__ == "__main__":
    raise SystemExit(main())
