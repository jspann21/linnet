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
    from tools.inspect.windows_pnp import present_devices, usb_parent

    usb = {
        instance.casefold(): dict(instance=instance)
        for instance in present_devices("USB")
        if re.match(r"^USB\\VID_(?:05AC|0000)&PID_|^USB\\VID_1209&PID_504[A-F]\\", instance, re.I)
    }
    disks = {}
    disk_class = "{4d36e967-e325-11ce-bfc1-08002be10318}"
    for instance in present_devices(disk_class, setup_class=True):
        try:
            parent = usb_parent(instance)
        except RuntimeError:
            continue
        pair = _usb_id(parent)
        apple_ipod = pair[0] == 0x05AC and instance.lower().startswith(r"usbstor\disk&ven_apple&prod_ipod&")
        if parent.casefold() in usb and (_family(pair) or apple_ipod):
            disks[instance.casefold()] = dict(usb_instance=parent)
    errors = []
    script = r"""$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$ids=ConvertFrom-Json -InputObject ([Console]::In.ReadToEnd())
$errors=@()
$usb=@(foreach ($id in $ids.usb) {
 try {
  $node=Get-PnpDevice -InstanceId $id -PresentOnly
  $problem=Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue
  $driver=Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue
  @{instance=$id;status=[string]$node.Status;problem=$problem.Data;driver_version=$driver.Data}
 } catch {
  $errors+=@{stage='usb-properties';type=$_.Exception.GetType().Name}
  break
 }
})
$disks=@()
if ($ids.disks.Count) {
 try {
  $disks=@(Get-CimInstance Win32_DiskDrive | Where-Object { $_.PNPDeviceID -in $ids.disks } | ForEach-Object {
   $w=$_
   $entry=@{pnp_id=$w.PNPDeviceID;product_serial=([string]$w.SerialNumber).Trim()}
   try {
    $d=Get-Disk -Number $w.Index
    $entry.bytes=[long]$d.Size
    $entry.logical_sector_bytes=[int]$d.LogicalSectorSize
    $entry.physical_sector_bytes=[int]$d.PhysicalSectorSize
    $entry.partition_style=[string]$d.PartitionStyle
    $entry.operational_status=[string]$d.OperationalStatus
    $entry.read_only=[bool]$d.IsReadOnly
    $entry.offline=[bool]$d.IsOffline
    if ($entry.bytes -gt 0 -and $entry.partition_style -in @('MBR','GPT') -and !$entry.offline) {
     $entry.partitions=@(Get-Partition -DiskNumber $d.Number | Select-Object -First 16 | ForEach-Object {
      $p=$_
      $letter=[string]$p.DriveLetter
      if ($letter -notmatch '^[A-Za-z]$') {$letter=''}
      $v=Get-Volume -Partition $p -ErrorAction SilentlyContinue
      @{offset=[long]$p.Offset;bytes=[long]$p.Size;type=[string]$p.Type;
        mbr_type=$p.MbrType;drive=$letter;filesystem=[string]$v.FileSystem;
        volume_id=[string]$v.UniqueId}
     })
    }
   } catch {$entry.error='storage-query-failed'}
   $entry
  })
 } catch {$errors+=@{stage='disk-inventory';type=$_.Exception.GetType().Name}}
}
ConvertTo-Json -Depth 6 -Compress -InputObject @{usb=$usb;disks=$disks;errors=$errors}
"""
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            input=json.dumps(dict(usb=list(usb)[:32], disks=list(disks)[:16])),
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=30,
            check=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        properties = json.loads(result.stdout.lstrip("\ufeff"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        properties = {}
        errors.append(dict(stage="windows-storage-properties", type=type(error).__name__))
    errors.extend(properties.get("errors", []))
    for item in properties.get("usb", []):
        key = item["instance"].casefold()
        if key in usb:
            usb[key].update(item)
    for item in properties.get("disks", []):
        key = item["pnp_id"].casefold()
        if key in disks:
            try:
                if usb_parent(item["pnp_id"]).casefold() == disks[key]["usb_instance"].casefold():
                    disks[key].update(item)
            except RuntimeError:
                disks[key]["error"] = "disk-disconnected"
    return dict(usb=list(usb.values()), disks=list(disks.values()), errors=errors)


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


def _storage_status(disk):
    if "bytes" not in disk:
        return "disk-unavailable"
    if disk.get("operational_status") == "No Media":
        return "no-media"
    if disk.get("offline"):
        return "offline"
    if disk["bytes"] == 0:
        return "no-media"
    partitions = disk["partitions"]
    unknown = {"", "UNKNOWN", "RAW"}
    filesystems = {part.get("filesystem", "").upper() for part in partitions} - unknown
    if filesystems & {"HFS", "HFS+", "HFSX", "APFS", "MAC OS EXTENDED"}:
        return "mac-format"
    if disk.get("partition_style") == "GPT":
        return "unsupported-layout"
    if any(part.get("filesystem", "").upper() == "FAT32" and part["mounted"] for part in partitions):
        return "volume-unverified"
    if disk.get("partition_style") == "MBR" and any(
        part.get("offset", 0) > 0 and not part["mounted"] and
        (part.get("filesystem", "").upper() == "FAT32" or
         (part.get("mbr_type") in (11, 12) and part.get("filesystem", "").upper() in unknown))
        for part in partitions
    ):
        return "unmounted-fat32"
    # Windows can expose an Apple Partition Map disk as a whole-disk volume
    # with a synthetic MBR/FAT16 partition. The type alone is not a filesystem.
    if not filesystems and len(partitions) == 1 and disk.get("partition_style") == "MBR":
        part = partitions[0]
        if part.get("offset") == 0 and part.get("bytes") == disk["bytes"] and part.get("mbr_type") == 4:
            return "possible-mac-format"
    if filesystems:
        return "unsupported-layout"
    return "format-unknown"


def _connection_status(usb, disks):
    families = [item for item in usb if item["targets"]]
    family_ids = {item["vid_pid"] for item in families}
    disks = [disk for disk in disks if disk["vid_pid"] in family_ids]
    targets = {target for item in families for target in item["targets"]}
    names = {TARGETS[target]["name"].removeprefix("Linnet for ") for target in targets}
    if len(families) == len(disks) == 1:
        products = [part.get("product", {}) for part in disks[0]["partitions"]]
        models = {product["product_name"] for product in products
                  if product.get("target_id") in targets
                  and product.get("model_identity_source") == "live-product-serial"
                  and not product.get("model_identity_conflict")}
        if len(models) == 1:
            names = models
    title = next(iter(names)) if len(names) == 1 else "iPod"
    statuses = {_storage_status(disk) for disk in disks}
    if len(disks) < len(families):
        statuses.add("usb-only")
    status = next(iter(statuses)) if len(statuses) == 1 else "mixed"
    mac_detail = ("To use Linnet, back up on a Mac and restore in iTunes for Windows. "
                  "Restoring erases the iPod.")
    messages = {
        "usb-only": ("iPod connected", "Enter Apple Disk Mode to access its storage."),
        "disk-unavailable": ("Storage unavailable", "Windows couldn’t read the iPod’s disk. Choose Find iPod to retry."),
        "no-media": ("Storage unavailable", "Windows reports no accessible storage. Re-enter Apple Disk Mode."),
        "offline": ("iPod disk offline", "Check the disk in Windows Disk Management."),
        "mac-format": ("Mac-formatted iPod", mac_detail),
        "possible-mac-format": ("Likely Mac-formatted iPod", mac_detail),
        "unsupported-layout": ("Unsupported disk format", "Linnet requires an MBR disk with a FAT32 volume."),
        "unmounted-fat32": ("Drive letter needed", "Assign a letter to the existing FAT32 volume in Disk Management."),
        "volume-unverified": ("Storage needs checking", "Linnet couldn’t verify the FAT32 volume. Choose Find iPod to retry."),
        "format-unknown": ("Unknown disk format", "Check the iPod’s format on a Mac before restoring."),
        "mixed": ("Multiple iPods connected", "Reconnect the iPod you want to check."),
    }
    label, detail = messages[status]
    return dict(status=status, title=title, label=label, detail=detail)


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
    report["errors"].extend(inventory.get("errors", []))
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
            pair = _usb_id(item["usb_instance"])
            disk = dict(
                vid_pid=f"{pair[0]:04X}:{pair[1]:04X}",
                **_fields(
                    item,
                    (
                        "bytes",
                        "logical_sector_bytes",
                        "physical_sector_bytes",
                        "partition_style",
                        "operational_status",
                        "read_only",
                        "offline",
                        "error",
                    ),
                ),
                partitions=[],
            )
            for partition in item.get("partitions", [])[:16]:
                entry = _fields(partition, ("offset", "bytes", "type", "mbr_type", "filesystem"))
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
        connection = _connection_status(report["usb"], report["disks"])
        report["connection"] = connection
        report["summary"] = f'{connection["title"]} · {connection["label"]}. {connection["detail"]}'
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
