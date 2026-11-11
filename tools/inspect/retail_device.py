"""Read and identify a mounted RetailOS iPod without raw-device access."""
import json
import os
from pathlib import Path
import re
import subprocess
from tools.studio.targets import TARGETS
from .ipod_models import identify as identify_model, normalize_model, model_info


def _sysinfo(root):
    path = root / "iPod_Control" / "Device" / "SysInfo"
    if not path.exists():
        return {}
    if not path.is_file() or path.stat().st_size > 65536:
        raise ValueError("RetailOS SysInfo exceeds its bound or is not a file")
    fields = {}
    for line in path.read_text(encoding="ascii", errors="strict").splitlines():
        key, separator, value = line.partition(":")
        if separator:
            fields[key.strip()] = value.strip()
    return fields


def _windows_storage(root):
    if os.name != "nt" or not re.fullmatch(r"[A-Za-z]:\\", str(root)):
        raise ValueError("retail-device inspection requires a Windows drive root")
    letter = root.drive[0]
    script = rf"""$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$p=Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Partition -Filter "DriveLetter='{letter}'"
if (!$p) {{ throw 'Could not resolve the mounted iPod partition' }}
$d=Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Disk -Filter ('Number='+$p.DiskNumber)
if (!$d) {{ throw 'Could not resolve the mounted iPod disk' }}
$bus=if ($d.BusType -eq 7) {{ 'USB' }} else {{ [string]$d.BusType }}
$volumes=@(Get-CimInstance Win32_Volume -Filter "DriveLetter='{letter}:'")
if ($volumes.Count -ne 1) {{ throw 'Could not resolve the mounted iPod volume' }}
$v=$volumes[0]
if ($v.DeviceID -notin $p.AccessPaths) {{
 $bound=Get-Volume -Partition $p
 if ($bound.UniqueId -ne $v.DeviceID) {{ throw 'Mounted iPod volume does not match its partition' }}
}}
$w=Get-CimInstance Win32_DiskDrive | Where-Object Index -eq $p.DiskNumber
@{{disk_number=$p.DiskNumber;disk_serial=$w.SerialNumber.Trim();disk_bytes=[long]$d.Size;
model=$w.Model;pnp_id=$w.PNPDeviceID;bus_type=$bus;sector_bytes=$d.LogicalSectorSize;
partition_offset=[long]$p.Offset;partition_bytes=[long]$p.Size;drive='{letter}:';
volume_label=$v.Label;filesystem=$v.FileSystem;volume_id=$v.DeviceID}} | ConvertTo-Json -Compress
"""
    for attempt in range(2):
        try:
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                    text=True, encoding="utf-8", capture_output=True, timeout=15, check=True,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            break
        except subprocess.TimeoutExpired as error:
            if not attempt:
                # Re-read the same mount; successful results must still pass
                # the USB, storage and cross-probe identity checks.
                continue
            raise RuntimeError(
                "Windows did not finish inspecting the mounted iPod after two attempts. "
                "Wait a few seconds, then choose Find iPod to try again."
            ) from error
        except subprocess.CalledProcessError as error:
            detail = (error.stderr or error.stdout or "Windows storage query failed").strip()
            raise RuntimeError("Could not inspect the mounted iPod: " + detail) from error
    storage = json.loads(result.stdout)
    from .windows_pnp import usb_parent
    storage["usb_pnp_id"] = usb_parent(storage["pnp_id"])
    models={"Apple iPod USB Device"} | {spec["native_storage"]["model"] for spec in TARGETS.values() if "native_storage" in spec}
    if (storage.get("model") not in models or
            storage.get("bus_type") != "USB" or storage.get("filesystem") != "FAT32" or
            storage.get("sector_bytes") not in (512, 2048, 4096)):
        raise ValueError("mounted volume is not a supported RetailOS iPod disk")
    return storage


def detect(mount):
    root = Path(mount).resolve(strict=True)
    storage = _windows_storage(root)
    pnp = storage["usb_pnp_id"]
    match = re.search(r"VID_([0-9A-F]{4})&PID_([0-9A-F]{4})\\([^\\]+)$", pnp, re.I)
    if not match:
        raise ValueError("iPod USB identity is unavailable")
    vid, pid = int(match.group(1), 16), int(match.group(2), 16)
    usb_serial = match.group(3).upper()
    serial_source = "usb"
    if (vid, pid) == (0x05AC, 0x1261) and storage["model"] == "Apple iPod USB Device":
        # Apple OS can enumerate this Classic with a Windows location-based USB
        # instance. Its live USBSTOR node still carries the board serial, distinct
        # from the product serial returned by the disk. Never use saved SysInfo or
        # a historical device node to fill in the current hardware identity.
        disk_instance = re.fullmatch(
            r"USBSTOR\\DISK&VEN_APPLE&PROD_IPOD&REV_[0-9A-Z_.]+\\"
            r"(?:[0-9A-F]+&[0-9A-F]+&[0-9A-F]+&)?([0-9A-F]{16})&0",
            storage["pnp_id"], re.I)
        if re.fullmatch(r"[0-9A-F]{16}", usb_serial):
            if disk_instance and disk_instance.group(1).upper() != usb_serial:
                raise ValueError("Classic USB and storage board identities disagree")
        elif re.fullmatch(r"[0-9A-F]+&[0-9A-F]+&[0-9A-F]+&[0-9A-F]+", usb_serial) and disk_instance:
            usb_serial = disk_instance.group(1).upper()
            serial_source = "apple-usbstor"
        else:
            raise ValueError("iPod USB serial identity is unavailable")
    if not re.fullmatch(r"[0-9A-F]{6,16}", usb_serial):
        raise ValueError("iPod USB serial identity is unavailable")
    native = [target for target,spec in TARGETS.items() if
              (spec.get("native_storage", {}).get("usb_vid"),spec.get("native_storage", {}).get("usb_pid")) == (vid,pid)]
    if native:
        if len(native)!=1:
            raise ValueError("native USB identity is ambiguous")
        profile=TARGETS[native[0]]["native_storage"]
        if (storage["model"] != profile["model"] or storage["sector_bytes"] not in profile["sector_bytes"] or
                not re.fullmatch(r"[0-9A-F]{16}",usb_serial) or storage["disk_serial"] != usb_serial):
            raise ValueError("native USB and storage identities disagree")
        # A transplanted disk's SysInfo describes its former logic board.
        return dict(target_id=native[0],device_id=usb_serial,linnet_usb_serial=usb_serial,
                    native_usb=dict(vid=vid,pid=pid,pnp_id=pnp),retail_os={},sysinfo={},
                    storage=storage,read_only=True,identity_source="linnet-usb")
    if storage["model"] != "Apple iPod USB Device":
        raise ValueError("unrecognized native storage USB identity")
    info = _sysinfo(root)
    model_number = normalize_model(info.get("ModelNumStr"))
    catalog_model = model_info(model_number)
    firewire = info.get("FirewireGuid", "").removeprefix("0x").upper()
    matches = []
    for target, spec in TARGETS.items():
        retail = spec["retail"]
        boards = retail["board_names"]
        models = retail["model_numbers"]
        if (retail["usb_vid"], retail["usb_pid"]) == (vid, pid) and (
                retail.get("usb_identity_only", False) or
                info.get("BoardHwName") in boards or model_number in models or
                catalog_model is not None and catalog_model['target_id'] == target):
            matches.append(target)
    if len(matches) != 1:
        raise ValueError("RetailOS identity does not resolve to exactly one Linnet target")
    retail_serial = info.get("pszSerialNumber", "").upper()
    profile = TARGETS[matches[0]]["retail"]
    if storage["sector_bytes"] not in profile.get("sector_bytes", [512]):
        raise ValueError("logical sector size is unsupported for this target")
    live_identity = profile.get("usb_identity_only", False)
    if not live_identity and not re.fullmatch(r"[0-9A-F]{16}", firewire):
        raise ValueError("RetailOS FirewireGuid is unavailable")
    if not storage["disk_serial"] or (not live_identity and not retail_serial) or (retail_serial and storage["disk_serial"].upper() != retail_serial):
        raise ValueError("SysInfo and mounted iPod serial identities disagree")
    product = identify_model(info.get("ModelNumStr"), product_serial=storage["disk_serial"], target=matches[0])
    return dict(target_id=matches[0], device_id=storage["disk_serial"],
                linnet_usb_serial=usb_serial, retail_usb=dict(vid=vid, pid=pid, pnp_id=pnp),
                retail_os=dict(board=info.get("BoardHwName"), model_number=info.get("ModelNumStr"),
                               build=info.get("visibleBuildID"), family=info.get("iPodFamily"),
                               updater_family=info.get("updaterFamily"), region=info.get("RegionCode")),
                storage=storage, sysinfo=info, product=product, read_only=True,
                identity_source=serial_source if live_identity else "usb-and-sysinfo")
