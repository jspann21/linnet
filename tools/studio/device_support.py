"""Read-only compatibility explanations and shareable device reports.

Reports are evidence for investigation, never authority to install firmware.
Only allowlisted hardware fields leave this module; no media is enumerated.
"""

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib

from .targets import TARGETS, ROOT


def describe(identity=None, review=None):
    target = (identity or {}).get("target_id")
    if target not in TARGETS:
        return dict(
            status="unidentified",
            title="iPod not identified",
            detail="Choose Find iPod. If it fails, save a hardware report.",
        )
    spec = TARGETS[target]
    if not spec.get("build_enabled", True):
        return dict(
            status="unsupported",
            title="Installation unavailable",
            detail=spec.get(
                "bringup_blocker",
                "This model is recognized, but no supported firmware is available.",
            ),
        )
    if review and review.get("action") in ("unsupported", "attention"):
        return dict(
            status=review["action"],
            title="Installation unavailable"
            if review["action"] == "unsupported"
            else "Setup needs attention",
            detail=review["summary"],
        )
    if review and review.get("linnet", {}).get("state") == "installed":
        return dict(
            status="installed",
            title="Linnet installed",
            detail="Software and recovery records found.",
        )
    classic = spec["installer"]["boot_layout"] == "s5l-nor"
    model = identity or {}
    if model.get('model_identity_status') == 'conflict' or model.get('model_identity_conflict'):
        return dict(status='attention', title='Model identity needs checking',
                    detail='Live hardware and saved model information disagree. Setup must verify the board before installation.')
    if model.get('image_kind') == 'os':
        return dict(status='running', title='Linnet is running',
                    detail='Open Disk Mode to manage media and check installed software.')
    return dict(
        status="supported-family",
        title="iPod recognized",
        detail=(
            ((model.get('product_name') or 'iPod classic') + '. '
             'Setup will check your iPod before installing Linnet.')
            if classic
            else "Setup checks the disk and backs up the original startup software."
        ),
    )


def _inventory():
    if os.name != "nt":
        raise RuntimeError("Hardware reports currently require Windows")
    script = r"""$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$usb=@(Get-PnpDevice -PresentOnly | Where-Object {
 $_.InstanceId -match '^USB\\VID_(05AC|0000)&PID_' -or
 $_.InstanceId -match '^USB\\VID_1209&PID_504[A-F]\\'
} | Select-Object -First 32 | ForEach-Object {
 $problem=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue
 $driver=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue
 @{instance=$_.InstanceId;status=[string]$_.Status;problem=$problem.Data;driver_version=$driver.Data}
})
$disks=@(Get-CimInstance Win32_DiskDrive | Where-Object {
 $_.InterfaceType -eq 'USB' -and ($_.Model -eq 'Apple iPod USB Device' -or $_.Model -like 'LINNET IPOD*')
} | Select-Object -First 16 | ForEach-Object {
 $w=$_
 try {
 $d=Get-Disk -Number $w.Index
 $parts=@(Get-Partition -DiskNumber $d.Number | ForEach-Object {
  $p=$_;$v=$null;$letter=[string]$p.DriveLetter
  if ($letter -notmatch '^[A-Za-z]$') {$letter=''}
  if ($letter) {$v=Get-Volume -Partition $p -ErrorAction SilentlyContinue}
  @{offset=[long]$p.Offset;bytes=[long]$p.Size;type=[string]$p.Type;
    drive=$letter;filesystem=[string]$v.FileSystemType;
    volume_id=[string]$v.UniqueId}
 })
 @{pnp_id=$w.PNPDeviceID;product_serial=([string]$w.SerialNumber).Trim();bytes=[long]$d.Size;logical_sector_bytes=[int]$d.LogicalSectorSize;
   physical_sector_bytes=[int]$d.PhysicalSectorSize;read_only=[bool]$d.IsReadOnly;
   offline=[bool]$d.IsOffline;partitions=$parts}
 } catch {@{pnp_id=$w.PNPDeviceID;error='storage-query-failed'}}
})
ConvertTo-Json -Depth 6 -Compress -InputObject @{usb=$usb;disks=$disks}
"""
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
        check=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    return json.loads(result.stdout.lstrip("\ufeff"))


def _usb_id(instance):
    match = re.match(r"^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})", instance, re.I)
    return (int(match[1], 16), int(match[2], 16)) if match else None


def _family(pair):
    return [
        key
        for key, spec in TARGETS.items()
        if pair
        in (
            (spec["retail"]["usb_vid"], spec["retail"]["usb_pid"]),
            (
                spec.get("native_storage", {}).get("usb_vid"),
                spec.get("native_storage", {}).get("usb_pid"),
            ),
        )
    ]


def _fields(value, names):
    return {
        name: value[name]
        for name in names
        if name in value and isinstance(value[name], (str, int, bool, type(None)))
    }


def _sysinfo(partition):
    """Read only bounded, non-identifying SysInfo fields from a pinned volume."""
    from tools.content.guard import guarded_path
    from tools.inspect.retail_device import _sysinfo as read_sysinfo
    import ctypes
    from ctypes import wintypes

    # A diagnostic read also rejects mount-point replacement and reparse paths.
    drive = partition.get("drive", "")
    if not re.fullmatch("[A-Za-z]", drive) or partition.get("filesystem") != "FAT32":
        return {}
    root = Path(drive + ":\\")

    def pinned():
        volume = ctypes.create_unicode_buffer(1024)
        api = ctypes.WinDLL(
            "kernel32", use_last_error=True
        ).GetVolumeNameForVolumeMountPointW
        api.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD)
        api.restype = wintypes.BOOL
        if (
            not api(str(root), volume, len(volume))
            or volume.value.casefold() != partition["volume_id"].casefold()
        ):
            raise RuntimeError("Diagnostic volume changed")

    pinned()
    guarded_path(root / "iPod_Control" / "Device" / "SysInfo", root)
    info = read_sysinfo(root)
    pinned()
    names = (
        "BoardHwName",
        "ModelNumStr",
        "visibleBuildID",
        "iPodFamily",
        "updaterFamily",
        "RegionCode",
    )
    return {
        key: "".join(c for c in info[key][:96] if c.isprintable())
        for key in names
        if key in info
    }


def collect(discovery=None, identity=None):
    """Capture observations without raw disk access, mode changes or writes."""
    from tools.inspect.windows_pnp import usb_parent

    report = dict(
        format="linnet-hardware-report",
        version=1,
        collected_at=datetime.now(timezone.utc).isoformat(),
        linnet_version=str(
            tomllib.loads((ROOT / "config/system.toml").read_text())["system"][
                "version"
            ]
        ),
        privacy="Hardware and connection details only. No serial numbers, volume labels, "
        "computer/user names, local paths or media. Read-only; nothing uploads automatically. "
        "Review before sharing.",
        usb=[],
        disks=[],
        recognized=[],
        runtime=[],
        limitations=[
            "Apple Disk Mode hides some hardware details.",
            "New hardware may need further checks and driver changes.",
        ],
        errors=[],
    )
    try:
        inventory = _inventory()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        inventory = {}
        report["errors"].append(
            dict(stage="windows-inventory", type=type(error).__name__)
        )
    for item in inventory.get("usb", [])[:32]:
        pair = _usb_id(item.get("instance", ""))
        if not pair:
            continue
        targets = _family(pair)
        mode = (
            "unidentified"
            if pair[0] == 0
            else "linnet-link"
            if pair == (0x1209, 0x504A)
            else "dfu"
            if pair == (0x05AC, 0x1223)
            else "disk-family"
            if targets
            else "other-apple"
        )
        report["usb"].append(
            dict(
                vid_pid=f"{pair[0]:04X}:{pair[1]:04X}",
                targets=targets,
                mode=mode,
                **_fields(item, ("status", "problem", "driver_version")),
            )
        )
    for item in inventory.get("disks", [])[:16]:
        try:
            pair = _usb_id(usb_parent(item["pnp_id"]))
            if not pair or not (pair[0] == 0x05AC or _family(pair)):
                continue
            disk = dict(
                vid_pid=f"{pair[0]:04X}:{pair[1]:04X}",
                **_fields(
                    item,
                    (
                        "bytes",
                        "logical_sector_bytes",
                        "physical_sector_bytes",
                        "read_only",
                        "offline",
                        "error",
                    ),
                ),
                partitions=[],
            )
            for partition in item.get("partitions", [])[:16]:
                entry = _fields(partition, ("offset", "bytes", "type", "filesystem"))
                entry["mounted"] = bool(
                    re.fullmatch("[A-Za-z]", partition.get("drive", ""))
                )
                try:
                    entry["sysinfo"] = _sysinfo(partition)
                    from tools.inspect.ipod_models import identify as identify_model
                    entry['product'] = identify_model(entry['sysinfo'].get('ModelNumStr'),
                                                     product_serial=item.get('product_serial'))
                except (OSError, RuntimeError, ValueError, UnicodeError) as error:
                    entry["sysinfo_error"] = type(error).__name__
                disk["partitions"].append(entry)
            report["disks"].append(disk)
        except (OSError, RuntimeError, ValueError) as error:
            report["errors"].append(
                dict(stage="disk-identity", type=type(error).__name__)
            )
    if discovery is None:
        from tools.content.media_manager import discover

        try:
            discovery = discover(detailed=True)
        except (OSError, RuntimeError, ValueError) as error:
            discovery = {}
            report["errors"].append(dict(stage="discovery", type=type(error).__name__))
    devices = discovery.get("devices", [])
    for device in devices:
        report["recognized"].append(
            dict(
                **_fields(
                    device,
                    (
                        "target_id",
                        "usb_vid_pid",
                        "board_name",
                        "model_number",
                        "apple_build",
                        "disk_bytes",
                        "sector_bytes",
                        "partition_offset",
                        "partition_bytes",
                        "filesystem",
                        "product_name",
                        "generation",
                        "original_capacity_gb",
                        "color",
                        "model_identity_status",
                        "model_identity_source",
                        "model_identity_conflict",
                    ),
                ),
                compatibility=describe(device),
            )
        )
    from .media_link import discover as links

    try:
        connections = discovery["links"] if "links" in discovery else links()
    except (OSError, RuntimeError, ValueError) as error:
        connections = []
        report["errors"].append(dict(stage="link-discovery", type=type(error).__name__))
    report["discovery_issue_count"] = len(discovery.get("errors", []))
    for connection in connections[:16]:
        current = connection["identity"]
        # Developer build hashes are device-bound, so omit them along with serials.
        entry = dict(
            identity=_fields(
                current,
                ("target_id", "profile", "image_kind", "healthy", "health_flags"),
            )
        )
        entry['product'] = _fields(connection.get('product', {}),
            ('model_number', 'product_name', 'generation', 'original_capacity_gb',
             'color', 'model_identity_status', 'model_identity_source', 'model_identity_conflict'))
        if current["image_kind"] == "os":
            from tools.link.campaign import connect

            session = None
            try:
                session, _, observed = connect(
                    serial_number=connection["serial"], deadline_seconds=5
                )
                if observed != current:
                    raise RuntimeError("Runtime identity changed")
                info = session.media_info()
                entry["ata"] = _fields(
                    info,
                    (
                        "model",
                        "firmware",
                        "logical_sector_bytes",
                        "physical_sector_bytes",
                        "sectors",
                        "capabilities",
                        "quirks",
                        "media_kind",
                        "rotation_rpm",
                    ),
                )
                entry["power"] = _fields(
                    session.power_state(),
                    ("failures", "bus_resets", "battery_mv", "charge_limit_ma"),
                )
                if current['target_id'] == 'video5g-m25':
                    try:
                        from tools.link.m25_hardware import read as read_hardware
                        entry['hardware'] = read_hardware(session, current)
                    except (OSError, RuntimeError, ValueError) as error:
                        entry['hardware_error'] = type(error).__name__
                if current['target_id'] == 'classic6g-n25':
                    try:
                        remote = session.input_state()['remote']
                        entry['headset_remote'] = dict(driver_available=remote is not None)
                        if remote is not None:
                            # Hardware only: omit pressed keys and command history.
                            entry['headset_remote'].update(_fields(remote,
                                ('present', 'armed', 'headphones', 'bus_fault', 'failures')))
                            entry['headset_remote']['mode'] = remote['registers']['mode']
                    except (OSError, RuntimeError, ValueError) as error:
                        entry['headset_remote_error'] = type(error).__name__
            except (OSError, RuntimeError, ValueError) as error:
                entry["runtime_error"] = type(error).__name__
            finally:
                if session:
                    session.close()
        report["runtime"].append(entry)
    known = bool(devices or report["runtime"])
    if known:
        observations = [*report['recognized'], *(item.get('product', {}) for item in report['runtime'])]
        products = sorted({item['product_name'] for item in observations if item.get('product_name')})
        found = products[0] + ' found.' if len(products) == 1 else 'Supported iPod family found.'
        report["summary"] = (
            found + " Open iPod setup to continue."
        )
    elif any(item["targets"] for item in report["usb"]):
        report["summary"] = (
            "iPod detected, but its storage identity could not be verified. "
            "Choose Find iPod to try again. If it still fails, check Disk Mode, "
            "its drive letter and FAT32 filesystem."
        )
    elif any(item["mode"] == "linnet-link" for item in report["usb"]):
        report["summary"] = (
            "Linnet is not responding. Reconnect directly, then choose Find iPod."
        )
    elif any(item["mode"] == "dfu" for item in report["usb"]):
        report["summary"] = (
            "DFU mode detected. Resume Classic installation, or enter Disk Mode to start setup."
        )
    elif any(item["mode"] == "unidentified" for item in report["usb"]):
        report["summary"] = (
            "USB device not identified. Try a direct port or another cable, then choose Find iPod."
        )
    elif report["usb"]:
        report["summary"] = (
            "Apple device detected; supported iPod not identified. Save this report for help."
        )
    elif report["errors"]:
        report["summary"] = (
            "Device check incomplete. Save this report and try Find iPod again."
        )
    else:
        report["summary"] = (
            "No iPod found. Connect in Disk Mode and choose Find iPod."
        )
    report["how_to_share"] = sharing()["instructions"]
    return report


def sharing():
    """An optional public support destination, configured by the distributor."""
    from urllib.parse import urlsplit

    config = tomllib.loads((ROOT / "config/support.toml").read_text(encoding="utf-8"))
    url = config.get("contact_url", "")
    email = config.get("email", "")
    if url:
        parsed = urlsplit(url)
        if (
            parsed.scheme != "https"
            or not parsed.netloc
            or parsed.username
            or parsed.password
            or email
        ):
            raise ValueError("Support contact must be one HTTPS URL or email address")
    if email and not re.fullmatch(
        r"[A-Za-z0-9.!#$%&\x27*+/=?^_`{|}~-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", email
    ):
        raise ValueError("Invalid support email address")
    return dict(
        url=url,
        email=email,
        instructions=(
            "1. Save or copy the report.\n"
            "2. Contact Linnet support and attach or paste it.\n"
            "3. Describe the problem and any storage changes.\n"
            + (
                "GitHub issues and attachments are public and show your account."
                if url and urlsplit(url).hostname == "github.com"
                else "Your support account or email address is visible to the recipient."
            )
            if url or email
            else "Save or copy the report and send it to the person helping you. "
            "Include the problem and any storage changes. No support address is configured."
        ),
    )


def contact_link():
    from urllib.parse import urlencode

    config = sharing()
    if config["url"]:
        return config["url"]
    if config["email"]:
        return (
            "mailto:"
            + config["email"]
            + "?"
            + urlencode(
                dict(
                    subject="Linnet iPod hardware report",
                    body="Please attach linnet-hardware-report.json and describe your iPod, storage changes and the problem.",
                )
            )
        )
    return None
