"""Guarded, unelevated publication of staged PODB music to Windows Disk Mode.

This module never opens a physical disk or requests elevation. Media is immutable
by staged name: an existing different hash is a collision and stops publication.
The old database is backed up before the new database is replaced last. File
fsync and readback detect normal I/O failures; FAT rename is not a power-loss
journal and storage firmware may retain volatile caches. Safely eject afterward.
Failures leave evidence and copied files in place; there is no automatic cleanup,
rollback, or replay. The caller must keep the mounted iPod exclusively in Disk
Mode throughout publication and handle reboot only after a successful receipt.
"""
from __future__ import annotations

from contextlib import ExitStack, contextmanager
from concurrent.futures import ThreadPoolExecutor
from functools import lru_cache
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import time
import uuid
import zlib
from typing import Callable

from .music import ART, MAX_DATABASE, TRACK, validate_database
from .host_state import file_stamp

CHUNK = 1024 * 1024
HISTORY_PAGE_BYTES = 4096
HISTORY_PAGE_COUNT = 1024
HISTORY_PAGE_RECORDS = 125
HISTORY_BANK_BYTES = (HISTORY_PAGE_COUNT + 1) * HISTORY_PAGE_BYTES


@contextmanager
def _exclusive(identity):
    """Serialize cooperating media publishers without creating a device file."""
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, w.BOOL, w.LPCWSTR]
    kernel.CreateMutexW.restype = w.HANDLE
    kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    kernel.WaitForSingleObject.restype = w.DWORD
    kernel.ReleaseMutex.argtypes = [w.HANDLE]
    kernel.ReleaseMutex.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    key = hashlib.sha256(identity['volume_unique_id'].casefold().encode()).hexdigest()
    handle = kernel.CreateMutexW(None, False, 'Local\\LinnetPhotoPublish-'+key)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        result = kernel.WaitForSingleObject(handle, 0)
        acquired = result in (0, 0x80)
        if result == 0x80:
            raise RuntimeError('previous media publisher was abandoned; inspect its receipt before retrying')
        if result != 0:
            raise RuntimeError('another media publisher is active or exclusivity could not be established')
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def _write_receipt(path, receipt):
    with path.open("w", encoding="utf-8", newline="\n") as writer:
        json.dump(receipt, writer, indent=2)
        writer.write("\n")
        writer.flush()
        os.fsync(writer.fileno())


def _host_receipts(identity):
    from .host_state import receipt_directory
    return receipt_directory(identity)


def _write_verified(path, data, error, *, owned=None):
    with path.open('xb') as writer:
        if owned is not None:
            owned.append(path)
        writer.write(data)
        writer.flush()
        os.fsync(writer.fileno())
    if path.read_bytes() != data:
        raise OSError(error)


def _history_superblock(data: bytes) -> bytes:
    expected = (b'HIST', 0, 64, HISTORY_BANK_BYTES, HISTORY_PAGE_COUNT, HISTORY_PAGE_RECORDS, 32)
    if len(data) != HISTORY_PAGE_BYTES or struct.unpack_from('<4sHHIIII', data) != expected or any(data[40:4092]):
        raise ValueError('invalid music history geometry or reserved bytes')
    if struct.unpack_from('<I', data, 4092)[0] != zlib.crc32(data[:4092]):
        raise ValueError('invalid music history superblock CRC')
    return data[24:40]


def _empty_history_bank(epoch: bytes) -> bytes:
    if len(epoch) != 16:
        raise ValueError('music history epoch must be 16 bytes')
    bank = bytearray(HISTORY_BANK_BYTES)
    struct.pack_into('<4sHHIIII16s', bank, 0, b'HIST', 0, 64, HISTORY_BANK_BYTES,
                     HISTORY_PAGE_COUNT, HISTORY_PAGE_RECORDS, 32, epoch)
    struct.pack_into('<I', bank, 4092, zlib.crc32(bank[:4092]))
    for index in range(HISTORY_PAGE_COUNT):
        start = (index + 1) * HISTORY_PAGE_BYTES
        struct.pack_into('<4sHHIIII', bank, start, b'HPGE', 0, 64, index, 0, 0, 0)
        bank[start + 40:start + 56] = epoch
        struct.pack_into('<I', bank, start + 4092, zlib.crc32(bank[start:start + 4092]))
    return bytes(bank)


def _plain(path: Path, boundary: Path, *, parents=None) -> os.stat_result | None:
    """Reject links and return leaf metadata.

    Optional parents belong to one read-only batch and root. Each leaf is still
    checked; _recheck_parents must finish the batch before accepting its result.
    Never share this map with a write guard or a later operation.
    """
    try:
        relative = path.relative_to(boundary)
    except ValueError as exc:
        raise ValueError("path escaped its declared root") from exc
    cursor = boundary
    for component in (None, *relative.parts):
        if component is not None:
            cursor /= component
        if parents is not None and cursor != path and cursor in parents:
            continue
        try:
            info = cursor.lstat()
        except FileNotFoundError:
            info = None
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f"reparse/symlink path rejected: {cursor}")
        if parents is not None and cursor != path and stat.S_ISDIR(info.st_mode):
            parents[cursor] = file_stamp(info)
    return info


def _recheck_parents(parents, boundary):
    """Finish a bounded read-only batch before trusting its shared directories."""
    for path, before in parents.items():
        info = _plain(path, boundary)
        if info is None or not stat.S_ISDIR(info.st_mode) or file_stamp(info) != before:
            raise ValueError(f"media directory changed during validation: {path}")


def _bounded(path: Path, maximum: int) -> bytes:
    with path.open("rb") as reader:
        if maximum <= CHUNK:
            data = reader.read(maximum + 1)
            if len(data) > maximum:
                raise ValueError(f"file exceeds limit: {path.name}")
            return data
        size = os.fstat(reader.fileno()).st_size
        if size > maximum:
            raise ValueError(f"file exceeds limit: {path.name}")
        data = reader.read(size + 1)
        if len(data) != size:
            raise ValueError(f"file changed while reading: {path.name}")
    return data


def _hash(path: Path, expected_size: int | None = None, *,
          progress: Callable[[int, int], None] | None = None) -> str:
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or (expected_size is not None and before.st_size != expected_size):
        raise ValueError(f"unexpected file type/size: {path}")
    digest = hashlib.sha256()
    count = 0
    if progress:
        progress(0, before.st_size)
    with path.open("rb") as reader:
        chunk_size = min(CHUNK, max(16 * 1024, before.st_size))
        while chunk := reader.read(chunk_size):
            count += len(chunk)
            digest.update(chunk)
            if progress:
                progress(count, before.st_size)
    after = path.stat()
    if count != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError(f"file changed while hashing: {path}")
    return digest.hexdigest()


def _pool_string(database: bytes, pool: int, offset: int) -> str:
    start = pool + offset
    return database[start:database.index(0, start)].decode("utf-8")


def _existing_stat(local: Path, root: Path, size: int, mtime_ns: int | None = None, *, parents=None):
    """Check referenced media without reading its audio payload."""
    info = _plain(local, root, parents=parents)
    if info is None:
        info = local.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size != size:
        raise ValueError(f"existing iPod media type/size changed: {local}")
    if mtime_ns is not None and info.st_mtime_ns != mtime_ns:
        raise ValueError(f"existing iPod media timestamp changed: {local}")
    return info


def validate_stage(stage: Path, *, device_root: Path | None = None,
                   progress: Callable[[str], None] | None = None) -> dict:
    """Validate PODB, manifest and art; existing metadata references use stat checks."""
    stage = Path(os.path.abspath(stage))
    _plain(stage, Path(stage.anchor))
    stage = stage.resolve(strict=True)
    if not stage.is_dir():
        raise ValueError("stage must be a directory")
    for name in ("LIBRARY.PDB", "music-manifest.json"):
        _plain(stage / name, stage)
    database = _bounded(stage / "LIBRARY.PDB", MAX_DATABASE)
    info = validate_database(database)
    manifest_bytes = _bounded(stage / "music-manifest.json", 128 * 1024 * 1024)
    manifest = json.loads(manifest_bytes)
    db_hash = hashlib.sha256(database).hexdigest()
    expected = dict(format="PODB", tracks=info["tracks"], groups=info["groups"],
                    artwork=info["artwork"], bytes=len(database), sha256=db_hash)
    if not isinstance(manifest, dict) or any(manifest.get(key) != value for key, value in expected.items()):
        raise ValueError("music manifest does not match the complete database")
    entries = manifest.get("entries")
    if not isinstance(entries, list) or len(entries) != info["tracks"]:
        raise ValueError("manifest track inventory mismatch")
    width = TRACK
    track_offset, pool, art_offset = (struct.unpack_from("<I", database, offset)[0] for offset in (32, 48, 60))
    files: dict[str, dict] = {}
    sources = set()
    external = []
    external_paths = set()
    external_parents = {}
    if device_root is not None:
        _plain(Path(os.path.abspath(device_root)), Path(Path(os.path.abspath(device_root)).anchor))
        device_root = Path(device_root).resolve(strict=True)
    for index, entry in enumerate(entries):
        if progress and (index % 100 == 0 or index + 1 == len(entries)):
            progress(f"Checking music references {index + 1:,}/{len(entries):,}…")
        if not isinstance(entry, dict):
            raise ValueError("invalid manifest entry")
        offset = track_offset + index * width
        identity, size = struct.unpack_from("<QQ", database, offset)
        if size > 0xFFFFFFFF:
            raise ValueError("Music file exceeds the iPod's FAT32 file size limit")
        path = _pool_string(database, pool, struct.unpack_from("<I", database, offset + 64)[0])
        sha = database[offset + 96:offset + 128].hex()
        if entry.get("id") != f"{identity:016x}" or entry.get("path") != path or entry.get("sha256") != sha:
            raise ValueError("manifest track identity/path/hash mismatch")
        source = entry.get("source")
        if (not isinstance(source, str) or not source or "\0" in source or "\\" in source or
            PurePosixPath(source).is_absolute() or ".." in PurePosixPath(source).parts or source in sources or
            int.from_bytes(hashlib.sha256(("track:" + source).encode()).digest()[:8], "little") != identity):
            raise ValueError("manifest source identity mismatch")
        sources.add(source)
        storage = entry.get("storage", "staged")
        if storage not in ("staged", "existing"):
            raise ValueError("unknown music storage mode")
        integrity = entry.get("integrity", "sha256")
        if integrity not in ("sha256", "metadata") or (integrity == "metadata" and storage != "existing"):
            raise ValueError("invalid music integrity mode")
        if integrity == "metadata" and (sha != "0" * 64 or type(entry.get("bytes")) is not int or
                entry["bytes"] != size or type(entry.get("mtime_ns")) is not int or entry["mtime_ns"] < 0):
            raise ValueError("invalid metadata-only music reference")
        if path.upper().startswith("/.LINNET/MUSIC/"):
            raise ValueError("Music must be imported into /Music; rebuild this music stage")
        for field, position in (("title", 52), ("artist", 56), ("album", 60)):
            if entry.get(field) != _pool_string(database, pool, struct.unpack_from("<I", database, offset + position)[0]):
                raise ValueError("manifest text mismatch")
        for field, position in (("codec", 48), ("track", 80), ("disc", 84)):
            if entry.get(field) != struct.unpack_from("<I", database, offset + position)[0]:
                raise ValueError("manifest numeric metadata mismatch")
        if storage == "existing":
            if device_root is None:
                raise ValueError("This library references existing iPod files; connect the iPod and supply device_root")
            local = device_root / path.lstrip("/")
            snapshot = _existing_stat(local, device_root, size,
                                      entry["mtime_ns"] if integrity == "metadata" else None,
                                      parents=external_parents if integrity == "metadata" else None)
            if integrity == "sha256" and _hash(local, size) != sha:
                raise ValueError(f"existing iPod media hash mismatch: {path}")
            if path in external_paths:
                raise ValueError("duplicate existing media path")
            external_paths.add(path)
            external.append(dict(path=path, bytes=size, sha256=sha, kind="music", integrity=integrity,
                                 mtime_ns=snapshot.st_mtime_ns, inode=snapshot.st_ino))
            continue
        if not re.fullmatch(r"/MUSIC/[A-Z0-9_-]{1,8}\.[A-Z0-9]{1,3}", path):
            raise ValueError("staged media must use a readable short name under /Music")
        relative = path.lstrip("/")
        if relative in files:
            raise ValueError("duplicate staged media path")
        local = stage / relative
        _plain(local, stage)
        if _hash(local, size) != sha:
            raise ValueError(f"staged media hash mismatch: {relative}")
        files[relative] = dict(path=relative, bytes=size, sha256=sha, kind="music")
    if external_parents:
        _recheck_parents(external_parents, device_root)
    def check_artwork(index):
        offset = art_offset + index * ART
        identity, path_offset, width, height, encoding, count, crc, reserved = struct.unpack_from("<QIHHIIII", database, offset)
        path = _pool_string(database, pool, path_offset)
        relative = path.removeprefix("/.LINNET/")
        local = stage / relative
        _plain(local, stage)
        pixels = _bounded(local, count)
        digest = hashlib.sha256(pixels).digest()
        if (width not in (64, 128, 160) or height != width or
            not ((encoding == 1 and count == width * height * 2) or
                 (encoding == 2 and width == 160 and count in (76288, 108040))) or
            reserved or len(pixels) != count or
            int.from_bytes(digest[:8], "little") != identity or zlib.crc32(pixels) != crc or
            Path(path).stem != digest[:4].hex().upper()):
            raise ValueError(f"staged artwork identity/CRC mismatch: {relative}")
        record = dict(path=relative, bytes=count, sha256=digest.hex(), kind="art")
        return relative, record
    if info["artwork"]:
        # Bound both open files and queued results while preserving database order.
        with ThreadPoolExecutor(max_workers=min(8, info["artwork"])) as executor:
            for first in range(0, info["artwork"], 8):
                pending = [executor.submit(check_artwork, index)
                           for index in range(first, min(first + 8, info["artwork"]))]
                for future in pending:
                    relative, record = future.result()
                    if relative in files and files[relative] != record:
                        raise ValueError("conflicting artwork short name")
                    files[relative] = record
    if (stage / "BROWSE.BIN").exists():
        _plain(stage / "BROWSE.BIN", stage)
        from .browse import prepare as prepare_browse
        browse = _bounded(stage / "BROWSE.BIN", MAX_DATABASE)
        if browse != prepare_browse(database):
            raise ValueError("Browse index does not match library")
        files['BROWSE.BIN'] = dict(path='BROWSE.BIN', bytes=len(browse), sha256=hashlib.sha256(browse).hexdigest(), kind='art')
    if (stage / "COVERS.CFL").exists():
        _plain(stage / "COVERS.CFL", stage)
        from .coverflow import validate as validate_coverflow
        validate_coverflow(stage / 'COVERS.CFL', database)
        size = (stage / 'COVERS.CFL').stat().st_size
        files['COVERS.CFL'] = dict(path='COVERS.CFL', bytes=size, sha256=_hash(stage / 'COVERS.CFL', size), kind='art')
    return dict(stage=str(stage), database=expected, manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                files=list(files.values()), existing_files=external, media_bytes=sum(item["bytes"] for item in files.values()))


def _volume_serial(value: str) -> str:
    value = str(value).replace("-", "").strip().upper()
    if not re.fullmatch("[0-9A-F]{8}", value):
        raise ValueError("expected volume serial must be eight hex digits (XXXX-XXXX is accepted)")
    return value


def inspect_target(disk_number: int, expected_disk_serial: str, expected_volume_serial: str,
                   expected_drive: str | None = None) -> dict:
    """Query Windows storage identity without opening devices or requesting UAC."""
    if os.name != "nt":
        raise RuntimeError("music publication requires Windows Disk Mode")
    if type(disk_number) is not int or disk_number < 0 or not isinstance(expected_disk_serial, str) or not expected_disk_serial.strip():
        raise ValueError("explicit disk number and disk serial required")
    volume_serial = _volume_serial(expected_volume_serial)
    drive = None if expected_drive is None else str(expected_drive).rstrip("\\/").upper()
    if drive is not None and not re.fullmatch("[A-Z]:", drive):
        raise ValueError("expected drive must be a drive letter such as F:")
    from tools.studio.targets import TARGETS
    native=[dict(spec["native_storage"],target_id=target) for target,spec in TARGETS.items() if "native_storage" in spec]
    config = base64.b64encode(json.dumps(dict(number=disk_number, serial=expected_disk_serial.strip(),
                                            volume=volume_serial, drive=drive,native=native)).encode()).decode()
    script = r'''
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$cfg=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('CONFIG')) | ConvertFrom-Json
$disk=Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Disk -Filter ('Number='+$cfg.number)
if (!$disk) {throw 'Disk identity unavailable'}
$physical=Get-CimInstance Win32_DiskDrive -Filter ('Index='+$cfg.number)
$native=@($cfg.native | Where-Object {$_.model -ceq $physical.Model -and $_.friendly_name -ceq $disk.FriendlyName})
$apple=$disk.FriendlyName -ceq 'Apple iPod' -and $physical.PNPDeviceID -match '^USBSTOR\\.*VEN_APPLE.*PROD_IPOD'
$target=''
if ($native.Count -eq 1) {
 if($cfg.serial -cnotmatch '^[0-9A-F]{16}$' -or $disk.LogicalSectorSize -notin $native[0].sector_bytes) {throw 'Native USB identity rejected'}
 $target=$native[0].target_id
}
if ($disk.SerialNumber.Trim() -cne $cfg.serial -or $disk.BusType -ne 7 -or
    (-not $apple -and -not $target) -or $disk.IsBoot -or $disk.IsSystem -or
    $disk.IsOffline -or $disk.IsReadOnly -or $disk.LogicalSectorSize -notin @(512,2048,4096) -or
    $physical.SerialNumber.Trim() -cne $cfg.serial) {
    throw 'Require expected serial, supported USB iPod, writable online non-system disk and supported sectors'
}
$parts=@(Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Partition -Filter ('DiskNumber='+$disk.Number) | Where-Object {$_.DriveLetter})
if ($parts.Count -ne 1) {throw 'Require exactly one mounted partition'}
$part=$parts[0]
$drive=[string]$part.DriveLetter+':'
$volumes=@(Get-CimInstance Win32_Volume -Filter ("DriveLetter='"+$drive+"'"))
if ($volumes.Count -ne 1) {throw 'Partition volume identity rejected'}
$volume=$volumes[0]
if ($volume.DeviceID -notin $part.AccessPaths) {
 $bound=Get-Volume -Partition $part
 if ($bound.UniqueId -ne $volume.DeviceID) {throw 'Partition volume identity rejected'}
}
$logical=Get-CimInstance Win32_LogicalDisk -Filter ("DeviceID='"+$drive+"'")
if ([string]$volume.FileSystem -ne 'FAT32' -or $logical.VolumeSerialNumber.ToUpper() -cne $cfg.volume -or
    ($cfg.drive -and $drive -cne $cfg.drive) -or $drive -eq $env:SystemDrive) {throw 'Volume identity rejected'}
@{disk_number=[int]$disk.Number;disk_serial=$disk.SerialNumber.Trim();disk_unique_id=[string]$disk.UniqueId;
  disk_bytes=[uint64]$disk.Size;sector_bytes=[int]$disk.LogicalSectorSize;partition_number=[int]$part.PartitionNumber;partition_offset=[uint64]$part.Offset;
  partition_bytes=[uint64]$part.Size;drive=$drive;root=$drive+'\';volume_unique_id=[string]$volume.DeviceID;
  volume_serial=$logical.VolumeSerialNumber.ToUpper();filesystem=[string]$volume.FileSystem;
  pnp_id=[string]$physical.PNPDeviceID;native_target_id=$target;free_bytes=[uint64]$volume.FreeSpace} | ConvertTo-Json -Compress
'''.replace("CONFIG", config)
    # EncodedCommand otherwise serializes errors as CLIXML, which is not a
    # readable explanation when storage was removed outside Media Manager.
    script = "try {\n" + script + "\n} catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }"
    encoded = base64.b64encode(script.encode("utf-16-le")).decode()
    for attempt in range(2):
        try:
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                                    capture_output=True, text=True, timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            break
        except subprocess.TimeoutExpired as error:
            if not attempt:
                # Retry only a stalled read, with the same expected disk and
                # volume identity. A returned rejection must still fail below.
                continue
            # TimeoutExpired includes the full encoded script in its message.
            # Keep the command out of setup errors and bounded diagnostics.
            raise RuntimeError(
                "Windows did not finish checking the iPod's disk and volume identity after two attempts. "
                "Wait a few seconds, then choose Find iPod to try again."
            ) from error
    if result.returncode:
        raise RuntimeError("iPod identity inspection failed: " + result.stderr.strip())
    identity = json.loads(result.stdout.lstrip("\ufeff"))
    if (identity.get("disk_number") != disk_number or identity.get("disk_serial") != expected_disk_serial.strip() or
        identity.get("volume_serial") != volume_serial or not re.fullmatch(r"[A-Z]:\\", identity.get("root", "")) or
        not identity.get("disk_unique_id") or not identity.get("volume_unique_id")):
        raise RuntimeError("incomplete Windows disk/volume identity")
    from tools.inspect.windows_pnp import usb_parent
    identity["usb_pnp_id"] = usb_parent(identity["pnp_id"])
    if identity["native_target_id"]:
        spec = TARGETS[identity["native_target_id"]]["native_storage"]
        expected_usb = f"USB\\VID_{spec['usb_vid']:04X}&PID_{spec['usb_pid']:04X}\\{expected_disk_serial.strip()}"
        if identity["usb_pnp_id"] != expected_usb:
            raise RuntimeError("Native USB identity rejected")
    return identity


def _same_target(before: dict, after: dict) -> None:
    for key in ("disk_number", "disk_serial", "disk_unique_id", "disk_bytes", "partition_number", "partition_offset",
                "partition_bytes", "sector_bytes", "drive", "root", "volume_unique_id", "volume_serial", "filesystem", "pnp_id",
                "native_target_id", "usb_pnp_id"):
        if before.get(key) != after.get(key):
            raise RuntimeError(f"iPod identity changed during publication: {key}")


@lru_cache(maxsize=1)
def _volume_api():
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetVolumeInformationW.argtypes = [w.LPCWSTR, w.LPWSTR, w.DWORD, ctypes.POINTER(w.DWORD),
        ctypes.POINTER(w.DWORD), ctypes.POINTER(w.DWORD), w.LPWSTR, w.DWORD]
    kernel.GetVolumeInformationW.restype = w.BOOL
    kernel.GetVolumeNameForVolumeMountPointW.argtypes = [w.LPCWSTR, w.LPWSTR, w.DWORD]
    kernel.GetVolumeNameForVolumeMountPointW.restype = w.BOOL
    return kernel.GetVolumeInformationW, kernel.GetVolumeNameForVolumeMountPointW


def _volume_guard(identity: dict) -> None:
    """Cheap per-file mounted-volume identity guard; does not open raw storage."""
    from ctypes import wintypes as w
    volume_info, volume_name = _volume_api()
    serial, maximum, flags = w.DWORD(), w.DWORD(), w.DWORD()
    filesystem, volume = ctypes.create_unicode_buffer(32), ctypes.create_unicode_buffer(1024)
    if not volume_info(identity["root"], None, 0, ctypes.byref(serial), ctypes.byref(maximum),
                       ctypes.byref(flags), filesystem, len(filesystem)):
        raise OSError(ctypes.get_last_error(), "iPod volume is unavailable")
    if not volume_name(identity["root"], volume, len(volume)):
        raise OSError(ctypes.get_last_error(), "iPod volume GUID is unavailable")
    if (f"{serial.value:08X}" != identity["volume_serial"] or filesystem.value != "FAT32" or
        volume.value.casefold() != identity["volume_unique_id"].casefold()):
        raise RuntimeError("mounted iPod volume changed during publication")


def _copy_verified(source: Path, destination: Path, item: dict, root: Path, guard,
                   destination_root: Path | None = None) -> bool:
    """Create only; preserve existing different objects and failed partial files."""
    guard()
    _plain(destination, destination_root or root)
    if destination.exists():
        if _hash(destination, item["bytes"]) != item["sha256"]:
            raise ValueError(f"existing different object is a hash collision: {destination}")
        return False
    hasher = hashlib.sha256()
    count = 0
    chunk_size = min(CHUNK, max(16 * 1024, item["bytes"]))
    with source.open("rb") as reader, destination.open("xb") as writer:
        while chunk := reader.read(chunk_size):
            guard()
            writer.write(chunk)
            count += len(chunk)
            hasher.update(chunk)
        writer.flush()
        os.fsync(writer.fileno())
    guard()
    if count != item["bytes"] or hasher.hexdigest() != item["sha256"] or _hash(destination, count) != item["sha256"]:
        raise OSError(f"copied file failed complete readback: {destination}")
    return True


def _copy_art_batch(batch, stage: Path, root: Path, guard) -> list:
    """Write bounded independent covers before flushing and reading them back."""
    if len(batch) > 16 or any(item['bytes'] not in (8192, 32768, 51200, 76288, 108040) for _, _, item in batch):
        raise ValueError('artwork batch exceeds its file or memory bound')
    prepared = []
    for source, destination, item in batch:
        _plain(source, stage)
        data = _bounded(source, item['bytes'])
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise RuntimeError(f"staged artwork changed during publication: {source}")
        prepared.append((destination, item, data))
    with ExitStack() as handles:
        written = []
        for destination, item, data in prepared:
            guard()
            _plain(destination, root)
            if destination.exists():
                raise ValueError(f"artwork destination appeared during publication: {destination}")
            writer = handles.enter_context(destination.open('xb'))
            writer.write(data)
            written.append((writer, destination, item))
        for writer, _, _ in written:
            guard()
            writer.flush()
            os.fsync(writer.fileno())
    verified = []
    for _, destination, item in written:
        guard()
        before = destination.stat()
        if _hash(destination, item['bytes']) != item['sha256']:
            raise OSError(f"copied artwork failed complete readback: {destination}")
        after = destination.stat()
        stamp = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != stamp:
            raise OSError(f"copied artwork changed during readback: {destination}")
        verified.append((destination, item, stamp))
    return verified


_GROUP_PATHS = {
    'MUSIC': ('.LINNET/COVERS.CFL', '.LINNET/BROWSE.BIN', '.LINNET/LIBRARY.PDB'),
    'PHOTOS': ('.LINNET/PHOTOS/GRID.PGR', '.LINNET/PHOTOS/LIBRARY.PIM'),
}


def _group_directory(root: Path, name: str) -> Path:
    if name not in _GROUP_PATHS:
        raise ValueError('unknown publication group')
    return root / '.LINNET' / 'TXN' / name


def _group_digest(path: Path, item: dict | None) -> bool:
    return (path.is_file() and item is not None and
            path.stat().st_size == item['bytes'] and
            _hash(path, item['bytes']) == item['sha256'])


def _clear_group(root: Path, directory: Path, guard) -> None:
    """Remove only our known transaction files, after the marker is gone."""
    _plain(directory, root)
    if not directory.exists():
        return
    for path in directory.iterdir():
        guard(); _plain(path, root)
        if not (re.fullmatch(r'[BNR][0-2]\.DAT', path.name) or path.name == 'STATE.NEW'):
            raise ValueError(f'unexpected publication recovery file: {path}')
        path.unlink()
    guard(); directory.rmdir()


def _recover_group(root: Path, identity: dict, name: str) -> bool:
    """Restore the old complete set after an interrupted grouped replacement."""
    guard = lambda: _volume_guard(identity)
    directory = _group_directory(root, name)
    marker = directory / 'STATE.TXN'
    guard(); _plain(marker, root)
    if not marker.exists():
        _clear_group(root, directory, guard)
        return False
    state = json.loads(_bounded(marker, 8192))
    if (state.get('version') != 1 or state.get('name') != name or
            not isinstance(state.get('items'), list) or
            not 1 <= len(state['items']) <= len(_GROUP_PATHS[name])):
        raise ValueError('invalid publication recovery record')
    paths = [item.get('path') for item in state['items']]
    if len(paths) != len(set(paths)) or any(path not in _GROUP_PATHS[name] for path in paths):
        raise ValueError('invalid publication recovery paths')
    for index, item in reversed(list(enumerate(state['items']))):
        old, new = item.get('old'), item.get('new')
        if not isinstance(new, dict) or not isinstance(new.get('bytes'), int) or not isinstance(new.get('sha256'), str):
            raise ValueError('invalid publication recovery digest')
        if old is not None and (not isinstance(old, dict) or not isinstance(old.get('bytes'), int) or
                                not isinstance(old.get('sha256'), str)):
            raise ValueError('invalid publication recovery backup digest')
        active = root.joinpath(*item['path'].split('/'))
        backup = directory / f'B{index}.DAT'
        _plain(active, root); _plain(backup, root)
        guard()
        if old is not None and not _group_digest(backup, old):
            raise OSError(f'publication recovery backup missing or changed: {backup}')
        if _group_digest(active, old):
            continue
        if active.exists() and not _group_digest(active, new):
            raise RuntimeError(f'publication output changed; preserving recovery record: {active}')
        if old is None:
            if active.exists():
                guard(); active.unlink()
        else:
            restored = directory / f'R{index}.DAT'
            guard(); _plain(restored, root)
            if restored.exists():
                if not _group_digest(restored, old):
                    # Rn is only a disposable scratch copy. The verified Bn
                    # remains the authority even after interrupted rollback.
                    guard(); restored.unlink()
            if not restored.exists():
                _copy_verified(backup, restored, old, root, guard)
            guard(); _plain(active, root); os.replace(restored, active)
            guard()
            if not _group_digest(active, old):
                raise OSError(f'publication recovery readback failed: {active}')
    guard(); marker.unlink()
    _clear_group(root, directory, guard)
    return True


def _recover_pending_groups(root: Path, identity: dict) -> list[str]:
    """Resolve incomplete related-index switches before any publisher proceeds."""
    return [name for name in _GROUP_PATHS if _recover_group(root, identity, name)]


def pending_publications(identity: dict) -> list[str]:
    """Read only the fixed transaction marker names for a verified device."""
    _same_target(identity, inspect_target(identity['disk_number'], identity['disk_serial'],
                                          identity['volume_serial'], identity['drive']))
    root = Path(identity['root'])
    pending = []
    for name in _GROUP_PATHS:
        _volume_guard(identity)
        directory = _group_directory(root, name)
        marker = directory / 'STATE.TXN'
        _plain(marker, root)
        if marker.exists():
            pending.append(name)
    return pending


def recover_pending_publications(*, disk_number: int, expected_disk_serial: str,
                                 expected_volume_serial: str,
                                 expected_drive: str | None = None) -> list[str]:
    """Recover interrupted index groups on a verified, exclusively held iPod.

    This can run on reconnect without an old host stage or a new media package.
    A corrupt record or an unavailable volume remains untouched and raises.
    """
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    with _exclusive(identity):
        _same_target(identity, inspect_target(disk_number, expected_disk_serial,
                                              expected_volume_serial, expected_drive))
        return _recover_pending_groups(Path(identity['root']), identity)


def _publish_group(root: Path, identity: dict, name: str, outputs: list[tuple[str, Path, dict]],
                   verify_complete=None) -> None:
    """Stage new bytes and old recovery copies before changing any live name."""
    guard = lambda: _volume_guard(identity)
    _recover_group(root, identity, name)
    paths = [entry[0] for entry in outputs]
    if len(paths) != len(set(paths)) or paths != [p for p in _GROUP_PATHS[name] if p in paths]:
        raise ValueError('invalid publication group order')
    guard()
    required = 1024 * 1024
    for relative, _, new in outputs:
        active = root.joinpath(*relative.split('/'))
        _plain(active, root)
        required += new['bytes'] + (active.stat().st_size if active.exists() else 0) + 2 * 65536
    if shutil.disk_usage(root).free < required:
        raise ValueError('insufficient device space for related outputs and recovery copies')
    directory = _group_directory(root, name)
    for part in (directory.parent.parent, directory.parent, directory):
        guard(); _plain(part, root); part.mkdir(exist_ok=True)
    items = []
    try:
        for index, (relative, source, new) in enumerate(outputs):
            active = root.joinpath(*relative.split('/'))
            _plain(active, root); _plain(source, Path(source.anchor))
            guard()
            active.parent.mkdir(parents=True, exist_ok=True)
            _plain(active.parent, root)
            old = None
            if active.exists():
                size = active.stat().st_size
                if size > 0x7fffffff:
                    raise ValueError(f'publication output exceeds recovery size: {active}')
                old = dict(bytes=size, sha256=_hash(active, size))
                _copy_verified(active, directory / f'B{index}.DAT', old, root, guard)
            _copy_verified(source, directory / f'N{index}.DAT', new, root, guard)
            items.append(dict(path=relative, old=old, new=dict(bytes=new['bytes'], sha256=new['sha256'])))
        state = dict(version=1, name=name, items=items)
        encoded = json.dumps(state, sort_keys=True).encode('utf-8')
        marker = directory / 'STATE.TXN'
        marker_new = directory / 'STATE.NEW'
        guard(); _write_verified(marker_new, encoded, 'publication recovery record readback mismatch')
        guard(); os.replace(marker_new, marker)
        if _bounded(marker, 8192) != encoded:
            raise OSError('publication recovery record changed after replacement')
        for index, item in enumerate(items):
            active = root.joinpath(*item['path'].split('/'))
            staged = directory / f'N{index}.DAT'
            guard(); _plain(active, root)
            if item['old'] is None:
                if active.exists():
                    raise RuntimeError(f'publication output appeared: {active}')
            elif not _group_digest(active, item['old']):
                raise RuntimeError(f'publication output changed: {active}')
            os.replace(staged, active)
            guard()
            with active.open('rb+') as writer:
                writer.flush()
                os.fsync(writer.fileno())
            if not _group_digest(active, item['new']):
                raise OSError(f'publication output readback mismatch: {active}')
        if verify_complete is not None:
            verify_complete()
        guard(); marker.unlink()
        _clear_group(root, directory, guard)
    except Exception:
        # A failed guard also prevents rollback. Keep the record for the next
        # run on the same verified device rather than writing to a new volume.
        if (directory / 'STATE.TXN').exists():
            try:
                _recover_group(root, identity, name)
            except Exception:
                pass
        raise


def publish_music(stage: Path, *, disk_number: int, expected_disk_serial: str,
                  expected_volume_serial: str, expected_drive: str | None = None,
                  progress: Callable[[str], None] | None = None) -> dict:
    """Validate locally, guard the explicit iPod, then publish media before PODB.

    Related mutable indexes are staged together and recovered under the device
    mutex before another publication begins.
    """
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    with _exclusive(identity):
        return _publish_music(stage, identity, disk_number, expected_disk_serial,
                              expected_volume_serial, expected_drive, progress)


def _publish_music(stage, identity, disk_number, expected_disk_serial,
                   expected_volume_serial, expected_drive, progress):
    root = Path(identity["root"])
    _recover_pending_groups(root, identity)
    inventory = validate_stage(stage, device_root=root, progress=progress)
    stage = Path(inventory["stage"])
    if stage.drive.casefold() == root.drive.casefold():
        raise ValueError("local stage must reside on a different volume from the iPod")
    guard = lambda: _volume_guard(identity)
    guard()
    linnet = root / ".LINNET"
    _plain(linnet, root)
    # Preflight existing media before any device mutation, including collisions.
    # Playlist banks may be initialized even when the database is reused.
    needed = 2 * 147456
    unchanged_assets = set()
    verified_outputs = {}
    for item in inventory["files"]:
        destination = (root if item["kind"] == "music" else linnet) / item["path"]
        _plain(destination, root)
        if item["path"] in ("COVERS.CFL", "BROWSE.BIN"):
            # Cover Flow is replaced atomically below, like the library index.
            same_header = False
            if destination.is_file() and destination.stat().st_size == item["bytes"]:
                local = stage / item["path"]
                _plain(local, stage)
                with local.open("rb") as source, destination.open("rb") as current:
                    # Both formats carry the database identity within 56 bytes.
                    same_header = source.read(56) == current.read(56)
            if same_header and _hash(destination, item["bytes"]) == item["sha256"]:
                unchanged_assets.add(item["path"])
            else:
                needed += item["bytes"] + 65536
                if destination.exists():
                    needed += destination.stat().st_size + 65536
        elif destination.exists():
            immutable_cover = re.fullmatch(r'ART/[0-9A-F]{8}\.RGB', item['path']) is not None
            before = destination.stat() if immutable_cover else None
            if _hash(destination, item["bytes"]) != item["sha256"]:
                raise ValueError(f"existing media differs; refusing overwrite: {item['path']}")
            if immutable_cover:
                after = destination.stat()
                if file_stamp(before) != file_stamp(after):
                    raise RuntimeError(f"existing artwork changed during verification: {destination}")
                # Reuse this complete read within this publication, with full
                # path-stamp checks before the database can reference it.
                verified_outputs[destination.relative_to(root).as_posix()] = (
                    item['sha256'], file_stamp(after))
        else:
            needed += item["bytes"] + 65536  # conservative FAT cluster allowance
    active = linnet / "LIBRARY.PDB"
    journals=[linnet/'MUSIC0.BIN',linnet/'MUSIC1.BIN']
    for journal in journals:_plain(journal,root)
    journal_exists=[p.exists() for p in journals]
    if any(journal_exists) and not all(journal_exists):
        raise ValueError('Incomplete music journal pair; inspect existing state before publication')
    if all(journal_exists):
        sizes=[p.stat().st_size for p in journals]
        if sizes!=[HISTORY_BANK_BYTES,HISTORY_BANK_BYTES]:
            raise ValueError('Unexpected/mismatched music history sizes; preserving existing state')
        epochs=[]
        for journal in journals:
            guard()
            with journal.open('rb') as reader:epochs.append(_history_superblock(reader.read(HISTORY_PAGE_BYTES)))
        if epochs[0]!=epochs[1]:
            raise ValueError('Music history epoch mismatch; preserving existing state')
    else:
        needed += 2 * HISTORY_BANK_BYTES
    _plain(active, root)
    previous = None
    database_unchanged = False
    if active.exists():
        old_data = _bounded(active, MAX_DATABASE)
        validate_database(old_data)
        previous = dict(bytes=len(old_data), sha256=hashlib.sha256(old_data).hexdigest())
        database_unchanged = (previous["bytes"] == inventory["database"]["bytes"] and
                              previous["sha256"] == inventory["database"]["sha256"])
    if not database_unchanged:
        needed += inventory["database"]["bytes"]
        if previous:
            needed += previous["bytes"] + 65536
    elif any(item['path'] in ('COVERS.CFL', 'BROWSE.BIN') and item['path'] not in unchanged_assets
             for item in inventory['files']):
        needed += 2 * inventory['database']['bytes'] + 65536
    if identity["free_bytes"] < needed + 1024 * 1024:
        raise ValueError("insufficient free space for media, new database, and retained backup")
    _same_target(identity, inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive))
    guard()
    linnet.mkdir(exist_ok=True)
    _plain(linnet, root)
    directories = [linnet / "ART"]
    if any(item["kind"] == "music" for item in inventory["files"]):
        directories.append(root / "Music")
    for path in directories:
        _plain(path, root)
        path.mkdir(exist_ok=True)
    receipt = dict(operation="publish-music", passed=False, phase="copying-media", identity=identity,
                   candidate_sha256=inventory["database"]["sha256"], previous=previous,
                   copied=0, reused=len(inventory["existing_files"]), started_at=time.time(), elevation=False,
                   existing_metadata_checked=sum(item["integrity"] == "metadata" for item in inventory["existing_files"]),
                   existing_sha256_checked=sum(item["integrity"] == "sha256" for item in inventory["existing_files"]),
                   durability="file fsync and readback; FAT rename is not a power-loss journal")
    # Receipt stays on the host; it records uncertainty without writing extra
    # mutable state beside the device's immutable database.
    receipt_path = _host_receipts(identity) / ("publish-" + uuid.uuid4().hex + ".json")
    group_outputs = []

    def record() -> None:
        _write_receipt(receipt_path, receipt)

    record()
    try:
        art_batch = []
        def flush_art_batch():
            if art_batch:
                for destination, item, stamp in _copy_art_batch(art_batch, stage, root, guard):
                    verified_outputs[destination.relative_to(root).as_posix()] = (item['sha256'], stamp)
                receipt['copied'] += len(art_batch)
                art_batch.clear()
        for item in inventory["files"]:
            local = stage / item["path"]
            _plain(local, stage)
            destination = (root if item['kind'] == 'music' else linnet) / item['path']
            proof = verified_outputs.get(destination.relative_to(root).as_posix())
            if proof is not None:
                guard()
                _plain(destination, root)
                if proof != (item['sha256'], file_stamp(destination.stat())):
                    raise RuntimeError(f"existing artwork changed during publication: {destination}")
                receipt['reused'] += 1
                continue
            if re.fullmatch(r'ART/[0-9A-F]{8}\.RGB', item['path']) and not destination.exists():
                art_batch.append((local, destination, item))
                if len(art_batch) >= 16:
                    flush_art_batch()
                continue
            flush_art_batch()
            if item['path'] in ('COVERS.CFL', 'BROWSE.BIN'):
                if item['path'] in unchanged_assets:
                    guard()
                    if (_hash(local, item['bytes']) != item['sha256'] or
                            _hash(destination, item['bytes']) != item['sha256']):
                        raise RuntimeError(f"music asset changed during publication: {item['path']}")
                    copied = False
                else:
                    group_outputs.append(('.LINNET/' + item['path'], local, item))
                    copied = True
            else:
                copied = _copy_verified(local, destination, item, root, guard)
            receipt["copied" if copied else "reused"] += 1
        flush_art_batch()
        receipt["phase"] = "media-verified"
        record()
        if not any(journal_exists):
            bank=_empty_history_bank(uuid.uuid4().bytes)
            digest=hashlib.sha256(bank).hexdigest()
            receipt['phase']='initializing-music-state';record()
            for journal in journals:
                guard();_plain(journal,root)
                with journal.open('xb') as writer:
                    writer.write(bank);writer.flush();os.fsync(writer.fileno())
                if _hash(journal,HISTORY_BANK_BYTES)!=digest:raise OSError('Music history initialization readback mismatch')
            receipt['initialized_music_state']=True
        provision_collections(linnet, guard)
        if not database_unchanged or group_outputs:
            group_outputs.append(('.LINNET/LIBRARY.PDB', stage / 'LIBRARY.PDB', inventory['database']))
        receipt["phase"] = "candidate-verified"
        record()
        # Database and manifest are immutable inputs to this logical operation.
        if (_hash(stage / "LIBRARY.PDB", inventory["database"]["bytes"]) != inventory["database"]["sha256"] or
            _hash(stage / "music-manifest.json") != inventory["manifest_sha256"]):
            raise RuntimeError("stage changed during publication")
        _same_target(identity, inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive))
        guard()
        for index, item in enumerate(inventory["existing_files"]):
            if progress and index % 100 == 0:
                progress(f"Confirming music references {index + 1:,}/{len(inventory['existing_files']):,}…")
            snapshot = _existing_stat(root / item["path"].lstrip("/"), root, item["bytes"], item["mtime_ns"])
            if snapshot.st_ino != item["inode"]:
                raise RuntimeError(f"existing iPod media replaced during publication: {item['path']}")
        guard()
        _plain(active, root)
        if previous:
            if _hash(active, previous["bytes"]) != previous["sha256"]:
                raise RuntimeError("active database changed during publication")
        elif active.exists():
            raise RuntimeError("another database appeared during publication")
        receipt["phase"] = "reusing-database" if database_unchanged else "replacing-database"
        record()
        def verify_related():
            for name, (_, stamp) in verified_outputs.items():
                path = root / name
                guard(); _plain(path, root)
                info = path.stat()
                if (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) != stamp:
                    raise OSError(f'published music asset changed after readback: {path}')
        if group_outputs:
            group_outputs.sort(key=lambda item: _GROUP_PATHS['MUSIC'].index(item[0]))
            receipt['phase'] = 'replacing-related-indexes'
            record()
            _publish_group(root, identity, 'MUSIC', group_outputs, verify_related)
        else:
            verify_related()
            guard()
            with active.open("rb+") as writer:
                writer.flush()
                os.fsync(writer.fileno())
        if _hash(active, inventory["database"]["bytes"]) != inventory["database"]["sha256"]:
            raise OSError("published database readback mismatch")
        receipt.update(passed=True, phase="published", finished_at=time.time(), database=str(active),
                       receipt=str(receipt_path), tracks=inventory["database"]["tracks"],
                       database_reused=database_unchanged)
        record()
        receipt['_verified_outputs'] = verified_outputs
        return receipt
    except Exception as exc:
        receipt.update(error=str(exc), finished_at=time.time(), receipt=str(receipt_path))
        try:
            record()
        except OSError:
            pass
        raise RuntimeError(f"Music publication stopped in {receipt['phase']}: {exc}. Inspect {receipt_path}; do not replay automatically") from exc


def _finish_backups(receipt, paths, root, guard, host_root=None):
    """Discard this operation's recovery copies only after verified publication."""
    for path in paths:
        try:
            guard()
            _plain(path, host_root if host_root is not None else root)
            path.unlink()
            if not any(path.parent.iterdir()):
                guard()
                path.parent.rmdir()
        except OSError as exc:
            receipt.setdefault('cleanup_warnings', []).append(dict(path=str(path), error=str(exc)))
    receipt['backups_retained'] = [str(path) for path in paths if path.exists()]


def _publish_library(package, candidate, identity, root, number, serial, volume, drive,
                     *, kind, label, filename, max_file, summary,
                     verify_references=None, receipt_fields=None, previous_hint=None,
                     retain_backup=False, backup_on_host=False):
    def guard():
        _volume_guard(identity)
    linnet, directory = root/'.LINNET', root/'.LINNET'/kind.upper()
    active, backups = directory/filename, directory/'BACKUP'
    guard()
    for path in (linnet, directory, active, backups):
        _plain(path, root)
    if verify_references:
        verify_references(root, guard)
    previous = None
    if active.exists():
        # Preserve exact previous bytes even if its content is corrupt; replacing
        # a damaged package must not require deleting recovery evidence first.
        info = active.stat()
        size = info.st_size
        if not 0 <= size <= max_file:
            raise ValueError(f'existing {label} package exceeds the supported backup size')
        stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
        digest = (previous_hint['sha256'] if previous_hint is not None and
                  previous_hint['bytes'] == size and previous_hint['stamp'] == stamp
                  else _hash(active, size))
        previous = dict(bytes=size, sha256=digest)
    reuse = previous is not None and previous['bytes'] == candidate['bytes'] and previous['sha256'] == candidate['sha256']
    needed = 0 if reuse else candidate['bytes'] + (previous['bytes'] if previous and not backup_on_host else 0) + 3*65536 + 1024*1024
    if identity['free_bytes'] < needed:
        raise ValueError(f'insufficient free space for new {label} package and retained backup')
    host_root = _host_receipts(identity).parent
    if previous and not reuse and backup_on_host and shutil.disk_usage(host_root).free < previous['bytes'] + 1024*1024:
        raise ValueError(f'insufficient computer space for previous {label} package')
    _same_target(identity, inspect_target(number, serial, volume, drive))
    receipt_path = _host_receipts(identity)/('publish-'+kind+'-'+uuid.uuid4().hex+'.json')
    receipt = dict(operation='publish-'+kind, passed=False, phase='preflight-complete',
                   identity=identity, candidate_sha256=candidate['sha256'], candidate_bytes=candidate['bytes'],
                   previous=previous, started_at=time.time(),
                   elevation=False, receipt=str(receipt_path),
                   durability='file fsync and complete readback; FAT rename is not a power-loss journal')

    receipt.update(receipt_fields or {})

    def record():
        _write_receipt(receipt_path, receipt)

    record()
    try:
        for path in (linnet, directory):
            guard()
            _plain(path, root)
            path.mkdir(exist_ok=True)
            _plain(path, root)
        if reuse:
            receipt['phase'] = 'verifying-existing-package'
            record()
            _plain(package, Path(package.anchor))
            if _hash(package, candidate['bytes']) != candidate['sha256']:
                raise RuntimeError(f'{label} source package changed during publication')
            if verify_references:
                verify_references(root, guard)
            _same_target(identity, inspect_target(number, serial, volume, drive))
            guard()
            _plain(active, root)
            if _hash(active, candidate['bytes']) != candidate['sha256']:
                raise RuntimeError(f'active {label} package changed during publication')
            receipt.update(passed=True, phase='reused', finished_at=time.time(), package=str(active),
                           package_reused=True,
                           durability='Existing package and references verified; package not rewritten',
                           **summary)
            record()
            return receipt
        if previous:
            receipt['phase'] = 'backing-up-previous'
            record()
            backup = ((host_root/'recovery')/(uuid.uuid4().hex+'-'+previous['sha256'][:8].upper()+Path(filename).suffix)
                      if backup_on_host else backups/(previous['sha256'][:8].upper()+Path(filename).suffix))
            _plain(backup.parent, host_root if backup_on_host else root)
            backup.parent.mkdir(exist_ok=True)
            _plain(active, root)
            _copy_verified(active, backup, previous, root, guard,
                           destination_root=host_root if backup_on_host else None)
            receipt['backup'] = str(backup)
        temporary = directory/('P'+uuid.uuid4().hex[:7].upper()+'.TMP')
        receipt.update(phase='copying-candidate', temporary=str(temporary))
        record()
        _plain(package, Path(package.anchor))
        _copy_verified(package, temporary, candidate, root, guard)
        receipt['phase'] = 'candidate-verified'
        record()
        _plain(package, Path(package.anchor))
        if _hash(package, candidate['bytes']) != candidate['sha256']:
            raise RuntimeError(f'{label} source package changed during publication')
        if verify_references:
            verify_references(root, guard)
        _same_target(identity, inspect_target(number, serial, volume, drive))
        guard()
        _plain(active, root)
        if previous:
            if _hash(active, previous['bytes']) != previous['sha256']:
                raise RuntimeError(f'active {label} package changed during publication')
        elif active.exists():
            raise RuntimeError(f'another {label} package appeared during publication')
        receipt['phase'] = 'replacing-package'
        record()
        os.replace(temporary, active)
        guard()
        with active.open('rb+') as writer:
            writer.flush()
            os.fsync(writer.fileno())
        if _hash(active, candidate['bytes']) != candidate['sha256']:
            raise OSError(f'published {label} package readback mismatch')
        if retain_backup:
            receipt['backups_retained'] = [str(backup)] if previous else []
        else:
            _finish_backups(receipt, [backup] if previous else [], root, guard,
                            host_root=host_root if backup_on_host else None)
        receipt.update(passed=True, phase='published', finished_at=time.time(), package=str(active),
                       **summary)
        record()
        return receipt
    except Exception as exc:
        receipt.update(error=str(exc), finished_at=time.time())
        try:
            record()
        except OSError:
            pass
        raise RuntimeError(f"{label.capitalize()} publication stopped in {receipt['phase']}: {exc}. Inspect {receipt_path}; do not replay automatically") from exc


def provision_collections(directory: Path, guard: Callable[[], None]) -> None:
    """Add the optional dual playlist banks, preserving every existing bank."""
    size = 147456
    banks = [directory / name for name in ('LISTS0.BIN', 'LISTS1.BIN')]
    present = []
    for path in banks:
        guard()
        if path.is_symlink():
            raise ValueError('unsafe playlist bank')
        if path.exists():
            if path.stat().st_size != size:
                raise ValueError('unexpected playlist bank size; preserving it')
            present.append(path)
    if len(present) == len(banks):
        return
    existing = []
    for path in present:
        data = bytearray(path.read_bytes())
        magic, version, generation, checksum = struct.unpack_from('<4sIII', data)
        struct.pack_into('<I', data, 12, 0)
        if magic != b'LLST' or version != 1 or not generation or zlib.crc32(data) != checksum:
            continue  # Firmware can recover from the other complete bank.
        struct.pack_into('<I', data, 12, checksum)
        existing.append((generation, data))
    if existing:
        initial = max(existing, key=lambda pair: pair[0])[1]
    elif any(path.exists() for path in banks):
        raise ValueError('no valid playlist bank; preserving state')
    else:
        initial = bytearray(size)
        struct.pack_into('<4sIII', initial, 0, b'LLST', 1, 1, 0)
        struct.pack_into('<I', initial, 12, zlib.crc32(initial))
    for path in banks:
        if path.exists():
            continue
        guard()
        with path.open('xb') as writer:
            writer.write(initial)
            writer.flush()
            os.fsync(writer.fileno())
        if path.read_bytes() != initial:
            raise OSError('playlist bank readback mismatch')
    guard()
