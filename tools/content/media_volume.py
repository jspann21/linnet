"""Mounted-device discovery, identity binding, and skipped-source settings."""
from __future__ import annotations

import json
import os
import re
import stat
import subprocess
from .guard import guarded_path as _plain, inspect_target, require_same_target as _same_target
from .host_state import (
    cache_directory as _cache_directory,
    cache_root,
    file_stamp as _receipt_stamp,
    load_json_object as _optional_state,
    media_state_root,
    save_json as _save,
)
from .inventory import KINDS, extensions, skipped_paths as _skipped_paths, source_folders
from .media_options import target_args
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


_RETAIL_ADOPTION = None


def discover(*, detailed=False):
    """Find mounted iPods and resolve each through publication guards."""
    if os.name != "nt":
        raise RuntimeError("The media manager currently supports Windows Disk Mode.")
    from tools.studio.targets import TARGETS
    names=["Apple iPod"]+[spec["native_storage"]["friendly_name"] for spec in TARGETS.values() if "native_storage" in spec]
    script = r"""
$ErrorActionPreference='Stop'
$names=ConvertFrom-Json 'NATIVE_NAMES'
$items=@()
$errors=@()
foreach ($disk in @(Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Disk -Filter 'BusType=7' | Where-Object {$_.FriendlyName -in $names})) {
 try {
  $allPartitions=@(Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_Partition -Filter ('DiskNumber='+$disk.Number))
  $partitions=@($allPartitions | Where-Object {$_.DriveLetter})
  if (!$partitions.Count -and @($allPartitions | Where-Object {$_.MbrType -in @(11,12)}).Count) {
   $errors+=@{drive="Disk $($disk.Number)";message='Windows has not mounted this iPod. Assign its existing volume a drive letter in Disk Management, then choose Find iPod.'}
  }
 }
 catch {$errors+=@{drive="Disk $($disk.Number)";message=[string]$_};continue}
 foreach ($partition in $partitions) {
  $drive=[string]$partition.DriveLetter+':'
  try {
   $logical=Get-CimInstance Win32_LogicalDisk -Filter ("DeviceID='"+$drive+"'")
   if (!$logical -or [string]$logical.VolumeSerialNumber -notmatch '^[0-9A-Fa-f]{8}$') {
    throw 'Windows has found this iPod but has not finished mounting its volume. Wait a few seconds, then reconnect the same iPod if its disk still does not appear.'
   }
   $items+=@{number=[int]$disk.Number;serial=$disk.SerialNumber.Trim();volume=$logical.VolumeSerialNumber;drive=$drive}
  }
  catch {$errors+=@{drive=$drive;message=[string]$_}}
 }
}
ConvertTo-Json -InputObject @{items=@($items);errors=@($errors)} -Compress
""".replace('NATIVE_NAMES',json.dumps(names).replace("'","''"))
    for attempt in range(2):
        try:
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, text=True, timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            break
        except subprocess.TimeoutExpired as error:
            if attempt:
                raise RuntimeError(
                    "Windows did not finish reading the iPod's storage identity. "
                    "Reconnect it in Disk Mode and choose Find iPod."
                ) from error
            # A just-enumerated hard disk can finish mounting after the first
            # storage query stalls. Retry with a fresh Windows provider query.
    if result.returncode:
        raise RuntimeError(
            "We couldn't read your iPod's storage. Reconnect your iPod in Disk Mode, "
            "wait a few seconds, then try again. If the problem continues, try a different "
            "USB cable or port."
        ) from subprocess.CalledProcessError(
            result.returncode, result.args, output=result.stdout, stderr=result.stderr
        )
    discovered = json.loads(result.stdout.lstrip("\ufeff") or "{}")
    items = discovered.get('items', [])
    devices = []
    errors = [f"{entry.get('drive', '?')}: {entry.get('message', '')[:240]}"
              for entry in discovered.get('errors', [])[:16] if isinstance(entry, dict)]
    with ThreadPoolExecutor(max_workers=min(4, max(1, len(items)))) as pool:
        pending = []
        for item in items:
            try:
                args = dict(disk_number=item["number"], expected_disk_serial=item["serial"],
                            expected_volume_serial=item["volume"], expected_drive=item["drive"])
                pending.append((item, pool.submit(_inspect_identify, args, Path(item["drive"] + "\\"))))
            except Exception as error:
                if len(errors) < 16:
                    drive = item.get('drive', '?') if isinstance(item, dict) else '?'
                    errors.append(f"{drive}: {(str(error) or type(error).__name__)[:240]}")
        for item, future in pending:
            try:
                devices.append(future.result())
            except Exception as error:
                if len(errors) < 16:
                    drive = item.get('drive', '?') if isinstance(item, dict) else '?'
                    errors.append(f"{drive}: {(str(error) or type(error).__name__)[:240]}")
    if detailed:
        return dict(devices=devices, errors=errors)
    if not devices and errors:
        raise RuntimeError('Could not read a connected iPod: ' + '; '.join(errors))
    return devices


def _inspect_identify(args, root):
    """Run independent storage probes together, then require matching identities."""
    from tools.inspect.retail_device import detect

    with ThreadPoolExecutor(max_workers=2) as pool:
        guarded = pool.submit(inspect_target, **args)
        retail = pool.submit(detect, root)
        identity, detected = guarded.result(), retail.result()
    if Path(root).resolve(strict=True) != Path(identity["root"]).resolve(strict=True):
        raise RuntimeError("USB and mounted storage roots disagree")
    return _identify(identity, detected)


def _identify(identity, detected=None):
    """Bind Disk Mode storage identity to one target manifest."""
    from tools.inspect.retail_device import detect, _sysinfo
    from tools.studio.targets import TARGETS, media_profile
    if detected is None:
        detected = detect(identity["root"])
    storage = detected["storage"]
    if (detected["device_id"] != identity["disk_serial"] or
            storage["disk_number"] != identity["disk_number"] or
            storage["disk_bytes"] != identity["disk_bytes"] or
            storage["partition_offset"] != identity["partition_offset"] or
            storage["partition_bytes"] != identity["partition_bytes"] or
            storage["sector_bytes"] != identity["sector_bytes"] or
            storage["filesystem"] != identity["filesystem"] or
            storage["pnp_id"] != identity["pnp_id"] or
            storage["volume_id"].casefold() != identity["volume_unique_id"].casefold()):
        raise RuntimeError("USB and mounted storage identities disagree")
    native_target = identity.get("native_target_id")
    if (bool(native_target) != (detected.get("identity_source") == "linnet-usb") or
            native_target and (native_target != detected["target_id"] or
                               storage["usb_pnp_id"] != identity["usb_pnp_id"])):
        raise RuntimeError("USB and mounted storage target identities disagree")
    spec = TARGETS[detected["target_id"]]
    retail = dict(detected["retail_os"])
    if identity.get("native_target_id"):
        # Native USB identifies the running board. Retained Apple SysInfo is
        # optional display metadata and never decides the target identity.
        try:
            root = Path(identity["root"])
            _plain(root / "iPod_Control" / "Device" / "SysInfo", root)
            saved = _sysinfo(root)
            for key, field in (("ModelNumStr", "model_number"), ("BoardHwName", "board"),
                               ("visibleBuildID", "build"), ("iPodFamily", "family"),
                               ("RegionCode", "region")):
                retail[field] = retail.get(field) or saved.get(key)
        except (OSError, ValueError, UnicodeError):
            pass
    model_number = retail.get("model_number")
    from tools.inspect.ipod_models import identify as identify_model, normalize_model
    product = detected.get('product') or identify_model(model_number, target=detected['target_id'])
    if native_target == 'classic6g-n25':
        from tools.studio.classic_setup import product_identity
        product = product_identity(detected['linnet_usb_serial']) or product
    model_number = product.get('model_number') or normalize_model(model_number) or model_number
    regulatory_models = spec["retail"]["regulatory_models"]
    regulatory_model = spec["retail"].get("regulatory_model_by_model", {}).get(model_number)
    if regulatory_model is None and len(regulatory_models) == 1:
        regulatory_model = regulatory_models[0]
    result = dict(identity, target_id=detected["target_id"],
                target_name=('Linnet for ' + product['product_name'] if product.get('product_name') else spec['name']),
                usb_pnp_id=storage['usb_pnp_id'],
                volume_label=storage.get("volume_label") or "",
                linnet_usb_serial=detected["linnet_usb_serial"],
                model_number=model_number,
                regulatory_model=regulatory_model,
                board_name=retail.get("board"),
                apple_build=retail.get("build"),
                ipod_family=retail.get("family"),
                region_code=retail.get("region"),
                usb_storage_model=storage.get("model"),
                usb_vid_pid=(f"{detected['native_usb']['vid']:04X}:{detected['native_usb']['pid']:04X}"
                             if "native_usb" in detected else
                             f"{detected['retail_usb']['vid']:04X}:{detected['retail_usb']['pid']:04X}"),
                media_profile=media_profile(detected["target_id"]))
    result.update({key: value for key, value in product.items() if key != 'target_id'})
    if identity.get("native_target_id"):
        result["media_cache_disk_serial"], result["media_cache_volume_scope"] = _retail_cache_identity(result)
    if detected.get("identity_source") == "apple-usbstor":
        result["usb_serial_source"] = "apple-usbstor"
    return result


def _retail_cache_identity(identity):
    """Reuse the proven disk and volume cache keys across USB storage modes."""
    global _RETAIL_ADOPTION
    from tools.inspect.retail_device import _sysinfo
    root = Path(identity["root"])
    base = media_state_root()
    fields = ("volume_serial", "target_id", "disk_bytes", "partition_offset", "partition_bytes")
    key = (str(base), *(identity.get(field) for field in (
        'root', 'volume_unique_id', 'linnet_usb_serial', 'disk_serial', 'disk_number',
        'disk_unique_id', 'partition_number', 'sector_bytes', *fields)))
    cached = _RETAIL_ADOPTION
    if cached is not None and cached[0] == key:
        serial, scope, receipt, stamp = cached[1:]
        try:
            info = _plain(receipt, base)
            if info is not None and _receipt_stamp(info) == stamp:
                return serial, scope
        except (OSError, ValueError):
            pass
    serial_hint = ""
    try:
        _plain(root / "iPod_Control" / "Device" / "SysInfo", root)
        serial_hint = _sysinfo(root).get("pszSerialNumber", "").upper()
    except (OSError, ValueError, UnicodeError):
        pass
    try:
        # SysInfo can be empty. Preparation receipts also bind both USB modes
        # to the same hardware serial, volume serial and partition geometry.
        for receipt in base.glob("*/session-*.json"):
            if not re.fullmatch(r"[0-9a-f]{24}", receipt.parent.name):
                continue
            try:
                info = _plain(receipt, base)
                if info is None or not stat.S_ISREG(info.st_mode) or info.st_size > 1048576:
                    continue
                record = json.loads(receipt.read_text(encoding="utf-8"))
            except (OSError, ValueError, UnicodeError):
                continue
            saved = record.get("identity", {}) if isinstance(record, dict) else {}
            if not isinstance(saved, dict):
                continue
            serial = saved.get("media_cache_disk_serial") or saved.get("disk_serial")
            if not isinstance(serial, str) or not re.fullmatch(r"[A-Z0-9]{6,16}", serial):
                continue
            saved_usb = saved.get("linnet_usb_serial")
            same_device = (saved_usb == identity.get("linnet_usb_serial")
                           if saved_usb else serial == serial_hint)
            if (same_device and
                    receipt.parent == _cache_directory(serial, identity["volume_serial"]) and
                    all(saved.get(field) == identity[field] for field in fields)):
                scope = saved.get('media_cache_volume_scope') or saved.get('volume_unique_id')
                scope = scope if isinstance(scope, str) and scope else None
                _RETAIL_ADOPTION = (key, serial, scope, receipt, _receipt_stamp(info))
                return serial, scope
    except (OSError, ValueError, UnicodeError, KeyError, TypeError):
        pass
    return None, None


def refresh(identity):
    current = _inspect_identify(target_args(identity), Path(identity["root"]))
    _same_target(identity, current)
    if current["target_id"] != identity.get("target_id"):
        raise RuntimeError("iPod target identity changed during media operation")
    return current


def skip_files(identity, items):
    """Persist supported iPod source files that should be omitted from preparation."""
    identity = refresh(identity)
    root = Path(identity["root"]).resolve(strict=True)
    state_path = cache_root(identity) / "state.json"
    state = _optional_state(state_path)
    skipped = state.setdefault("skipped_files", {})
    if not isinstance(skipped, dict):
        skipped = state["skipped_files"] = {}
    added = []
    known_by_kind = {}
    retail_photo_paths = None
    for kind, value in items:
        if kind not in KINDS:
            raise ValueError("Unknown media folder")
        path = Path(value).resolve()
        retail_photo = False
        if kind == 'Photos' and path.is_relative_to(root / 'Photos' / '@iTunes'):
            if retail_photo_paths is None:
                from .retail_media import images
                retail_photo_paths = {root / photo['relative'] for photo in images(root)}
            retail_photo = path in retail_photo_paths
        if not retail_photo and (not path.is_file() or path.suffix.lower() not in extensions(kind)):
            raise ValueError(f"This is not a supported {kind.lower()} file: {path}")
        if not any(path.is_relative_to(folder.resolve()) for folder in source_folders(root, kind)):
            raise ValueError(f"The file is outside this iPod's {kind} sources: {path}")
        relative = path.relative_to(root).as_posix()
        values = skipped.setdefault(kind, [])
        if not isinstance(values, list):
            values = skipped[kind] = []
        if kind not in known_by_kind:
            known_by_kind[kind] = {Path(item).as_posix().casefold() for item in values if isinstance(item, str)}
        if relative.casefold() not in known_by_kind[kind]:
            values.append(relative)
            known_by_kind[kind].add(relative.casefold())
            added.append(relative)
        state.pop(kind, None)
        if kind == "Videos":
            video_state = state.get("video_files", {})
            if isinstance(video_state, dict):
                video_key = path.relative_to(root / "Videos").as_posix() if path.is_relative_to(root / "Videos") else relative
                video_state.pop(video_key, None)
    _save(state_path, state)
    return added


def list_skipped_files(identity):
    """Return cached skip entries with their current source locations."""
    root = Path(identity["root"]).resolve()
    state_path = cache_root(identity) / "state.json"
    state = _optional_state(state_path)
    skipped = state.get("skipped_files", {})
    if not isinstance(skipped, dict):
        return []
    result = []
    for kind in KINDS:
        values = skipped.get(kind, [])
        if not isinstance(values, list):
            continue
        allowed = _skipped_paths(state, kind)
        for relative in sorted((Path(value).as_posix() for value in values
                                if isinstance(value, str) and Path(value).as_posix().casefold() in allowed),
                               key=str.casefold):
            result.append(dict(kind=kind, relative=relative, source=str(root / Path(relative))))
    return result


def restore_skipped_files(identity, items):
    """Remove files from the persistent preparation skip list."""
    identity = refresh(identity)
    state_path = cache_root(identity) / "state.json"
    state = _optional_state(state_path)
    skipped = state.get("skipped_files", {})
    if not isinstance(skipped, dict):
        return []
    requested = {}
    requested_order = []
    for kind, value in items:
        if kind not in KINDS:
            raise ValueError("Unknown media folder")
        relative = Path(value).as_posix()
        key = relative.casefold()
        if key not in requested.setdefault(kind, {}):
            requested[kind][key] = relative
            requested_order.append((kind, key, relative))
    restored_keys = set()
    for kind, relatives in requested.items():
        values = skipped.get(kind, [])
        if not isinstance(values, list):
            continue
        present = {Path(item).as_posix().casefold() for item in values if isinstance(item, str)}
        kept = [item for item in values if not isinstance(item, str) or
                Path(item).as_posix().casefold() not in relatives]
        if len(kept) != len(values):
            skipped[kind] = kept
            restored_keys.update((kind, key) for key in relatives if key in present)
            state.pop(kind, None)
    _save(state_path, state)
    return [relative for kind, key, relative in requested_order if (kind, key) in restored_keys]


def eject_device(identity):
    from .publish import _exclusive
    from .about import publish as publish_about
    from tools.studio.installer import installed_state
    from tools.link.eject import eject, connection_state
    def closed_connection():
        state = connection_state(identity)
        if state == 'ejected':
            return dict(ejected=True, already_ejected=True, about_warning=None)
        if state == 'disconnected':
            return dict(ejected=False, disconnected=True, about_warning=None)
        return None
    with _exclusive(identity):
        closed = closed_connection()
        if closed:
            return closed
        try:
            current = refresh(identity)
        except (OSError, ValueError, RuntimeError):
            closed = closed_connection()
            if closed:
                return closed
            raise
        warning = None
        if installed_state(current['root'], current['target_id'])['state'] == 'installed':
            try:
                publish_about(current, refresh)
            except (OSError, ValueError) as error:
                # Optional About data must not prevent a requested safe eject.
                warning = str(error)
        report = eject(current['disk_number'], current['disk_serial'], wait_link=False)
        report['about_warning'] = warning
        return report
