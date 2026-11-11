"""Non-wiping Linnet installer orchestration for mounted Windows iPods.

The ordinary process identifies the target and validates selected artifacts. Native
firmware access is isolated in an elevated worker whose inputs and receipts are
persisted under LocalAppData. Existing media is never enumerated for deletion,
formatted, repartitioned, or overwritten by this module.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid
import zlib

from tools.inspect.image import inspect as inspect_image
from tools.studio.targets import ROOT, TARGETS


CORE_FILES = ("OS0.LOS", "OS1.LOS", "STATE0.BIN", "STATE1.BIN",
              "SCRATCH.BIN", "BOOT0.BIN", "BOOT1.BIN")
JOURNAL = Path(".LINNET/RECOVERY/INSTALL.JSON")
REINSTALL_SYSTEM_ENTRIES = {
    "OS0.LOS", "OS1.LOS", "BOOT0.BIN", "BOOT1.BIN", "SCRATCH.BIN",
    "WORK.BIN", "RBPATH.CFG", "APPLE.CFG", "APPLE.BIN", "RECOVERY", "LNETOLD0",
}
INSTALLATION_METHODS = {
    "native-preserve": dict(
        label="Linnet with Apple fallback",
        detail="Linnet starts normally; hold MENU during power-on to choose Linnet or the preserved Apple software.",
        strategy="native-directory", boot_selector="none", requires_admin=True),
    "native-selector": dict(
        label="Native Linnet with Apple and Rockbox fallback",
        detail="Linnet starts normally; hold MENU during power-on to choose Linnet, Apple software, or the verified Rockbox image.",
        strategy="native-directory", boot_selector="rockbox", requires_admin=True),
    "rockbox-loader": dict(
        label="Use the existing Rockbox bootloader",
        detail="Replace Rockbox’s boot file reversibly; Linnet depends on the Rockbox bootloader.",
        strategy="rockbox-file", boot_selector="none", requires_admin=False),
}


def _now():
    return datetime.now(timezone.utc).isoformat()


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _save(path, value, *, replace=True, durable=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not replace:
        with path.open("x", encoding="utf-8", newline="\n") as writer:
            json.dump(value, writer, indent=2, sort_keys=True)
            writer.write("\n")
            writer.flush()
            os.fsync(writer.fileno())
        if durable:
            with path.open("r+b") as saved:
                os.fsync(saved.fileno())
        return
    temporary = path.with_suffix(path.suffix + ".tmp-" + uuid.uuid4().hex)
    with temporary.open("x", encoding="utf-8", newline="\n") as writer:
        json.dump(value, writer, indent=2, sort_keys=True)
        writer.write("\n")
        writer.flush()
        os.fsync(writer.fileno())
    if durable and os.name == "nt":
        # The uninstall receipt must remain discoverable after a host power loss.
        # MoveFileExW waits for the rename to reach disk before cleanup begins.
        import ctypes

        move = ctypes.WinDLL("kernel32", use_last_error=True).MoveFileExW
        move.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
        move.restype = ctypes.c_int
        if not move(os.path.abspath(temporary), os.path.abspath(path), 0x1 | 0x8):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        temporary.replace(path)
    if durable:
        with path.open("r+b") as saved:
            os.fsync(saved.fileno())


def cache_root(identity):
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    serial = re.sub(r"[^A-Za-z0-9-]", "_", identity["disk_serial"])
    return base / "Linnet" / "Installer" / f"{identity['target_id']}-{serial}"


def identity_for_mount(mount):
    from tools.content.media_manager import discover
    wanted = Path(mount).resolve(strict=True)
    matches = [item for item in discover() if Path(item["root"]).resolve() == wanted]
    if len(matches) != 1:
        raise ValueError("the selected mount does not resolve to exactly one supported iPod")
    return matches[0]


def _wrapper(path, install_spec, maximum):
    try:
        if not path.is_file() or path.is_symlink() or not 40 <= path.stat().st_size <= maximum:
            return None
        with path.open('rb') as reader:
            data=reader.read(maximum+1)
        if len(data)>maximum:return None
        from tools.studio.bootstrap import ipod_payload
        _payload, model = ipod_payload(data, install_spec["wrapper_model"].encode("ascii"),
                                       install_spec["wrapper_checksum_seed"])
        return model.decode("ascii")
    except (ValueError, OSError, UnicodeDecodeError):
        return None


def _selector_rockbox(path, spec):
    """Require a bounded, checksummed Rockbox image with its runtime marker."""
    install_spec = spec["installer"]
    try:
        path = Path(path)
        if (path.is_symlink() or not path.is_file() or
                not 48 <= path.stat().st_size <= spec["stage_bytes"] + 8):
            return False
        from tools.studio.bootstrap import ipod_payload
        with path.open('rb') as reader:
            data=reader.read(spec['stage_bytes']+9)
        if len(data)>spec['stage_bytes']+8:return False
        payload, model = ipod_payload(data, install_spec["wrapper_model"].encode("ascii"),
                                      install_spec["wrapper_checksum_seed"])
        if model != install_spec['wrapper_model'].encode('ascii'):
            return False
        if spec['id'] == 'classic6g-n25':
            # S5L crt0 has vector literals at offset 32, followed by a
            # trampoline into SDRAM. The PortalPlayer marker is absent.
            if len(payload) < 72 or struct.unpack_from('<I',payload)[0] != 0xea00000d:
                return False
            if struct.unpack_from('<7I',payload,4) != (0xe59ff014,)*7:
                return False
            trampoline,entry=struct.unpack_from('<II',payload,60)
            return (trampoline==0xe51ff004 and not entry%4 and
                    spec['load_address']+68 <= entry < spec['load_address']+len(payload))
        return payload[32:40] == b"Rockbox\x01"
    except (ValueError, OSError):
        return False


def _saved_method(value):
    method = (value or {}).get("installation_method")
    if method in INSTALLATION_METHODS:
        return method
    return None


def _saved_previous_boot(value):
    value = value or {}
    if value.get("strategy") == "rockbox-file":
        return "rockbox"
    previous = value.get("previous_boot")
    if previous not in ("apple", "rockbox"):
        raise ValueError("Saved previous startup software is missing or invalid")
    return previous


def _installation_methods(action, rockbox, marker, resume):
    selected = _saved_method(resume if action == "resume" else marker) if (resume or marker) else None
    can_change = action in ("install", "replace", "adopt")
    selector_compatible = rockbox.get("selector_compatible", False)
    options = []
    for method_id, definition in INSTALLATION_METHODS.items():
        needs_rockbox = method_id in ("rockbox-loader", "native-selector")
        if method_id == "native-selector":
            compatible = selector_compatible
        elif method_id == "rockbox-loader":
            compatible = selector_compatible and rockbox.get("loader_available", False)
        else:
            compatible = True
        enabled = ((can_change and (compatible or not needs_rockbox)) or
                   (action == "resume" and method_id == selected))
        detail = definition["detail"]
        if needs_rockbox and not rockbox.get("compatible"):
            detail += " A compatible Rockbox image was not found."
        elif method_id == "rockbox-loader" and not rockbox.get("loader_available"):
            detail += " An installed Rockbox bootloader could not be verified."
        elif needs_rockbox and not selector_compatible:
            detail += " The file is not a bounded Rockbox runtime image for this target."
        option = dict(definition)
        option.update(id=method_id, enabled=enabled, detail=detail)
        options.append(option)
    default = selected if selected in INSTALLATION_METHODS else (
        "native-selector" if selector_compatible else "native-preserve")
    return options, default


def _rockbox_bootloader_record(root):
    """Return bounded Rockbox Utility evidence that its iPod bootloader was installed."""
    log = Path(root) / ".rockbox" / "rbutil.log"
    try:
        if log.is_symlink() or not log.is_file() or log.stat().st_size > 4 * 1024 * 1024:
            return False
        section = None
        for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
            elif section == "bootloader" and re.fullmatch(
                    r"bootloader-[a-z0-9_-]+\.ipod\s*=\s*.+", line, re.IGNORECASE):
                return True
    except OSError:
        pass
    return False


BUILD_LABELS = {"release": "User build", "dev": "Developer build", "trace": "Developer trace build"}


def build_description(state):
    if state.get("state") == "installed":
        label = BUILD_LABELS.get(state.get("profile"), "Unknown build")
        pending = " · candidate pending" if state.get("candidate_pending") else ""
        return f"{label} · {state['build_id'][:12]}{pending}"
    return {"absent": "Not installed", "incomplete": "Incomplete installation",
            "invalid": "Installation needs attention"}.get(state.get("state"), "Unknown installation")


def installed_state(root, target_id):
    root = Path(root)
    capacity = TARGETS[target_id]["os_capacity_bytes"]
    directory = root / ".LINNET"
    present = [name for name in CORE_FILES if (directory / name).exists()]
    if not present:
        return dict(state="absent", present=0, required=len(CORE_FILES))
    if len(present) != len(CORE_FILES):
        return dict(state="incomplete", present=len(present), required=len(CORE_FILES),
                    detail="An interrupted Linnet file transaction is present and can only be resumed with the same build.")
    try:
        from tools.studio.update_history import _boot_record
        records = [_boot_record(directory / name, capacity) for name in ("BOOT0.BIN", "BOOT1.BIN")]
        if records[0][1] == records[1][1] and records[0][0] != records[1][0]:
            raise ValueError("conflicting boot records at the same generation")
        selected = max(records, key=lambda item: item[1])
        build_id = selected[2]
        confirmed_slot = selected[0][52]
        metadata_offset = 68 + confirmed_slot * 72
        expected_length, expected_profile = struct.unpack_from("<II", selected[0], metadata_offset)
        expected_sha = selected[0][metadata_offset + 40:metadata_offset + 72].hex()
        images = []
        for slot, name in enumerate(("OS0.LOS", "OS1.LOS")):
            path = directory / name
            with path.open("rb") as reader:
                header = reader.read(128)
                if not any(header):
                    continue
                length = struct.unpack_from("<I", header, 20)[0]
                if not 4 <= length <= capacity - 128:
                    raise ValueError("invalid installed image length")
                image = header + reader.read(length)
            identity = inspect_image(image)
            if identity["target_id"] != target_id:
                raise ValueError("installed Linnet image targets a different device")
            identity["slot"] = slot
            images.append(identity)
        confirmed = next((item for item in images if item["slot"] == confirmed_slot), None)
        if confirmed is None:
            raise ValueError("confirmed Linnet image is missing")
        if (confirmed["build_id"] != build_id or confirmed["payload_bytes"] + 128 != expected_length or
                ("release", "dev", "trace").index(confirmed["profile"]) != expected_profile or
                confirmed["sha256"] != expected_sha):
            raise ValueError("confirmed image differs from boot metadata")
        return dict(state="installed", present=len(present), required=len(CORE_FILES),
                    build_id=build_id, image_sha256=confirmed['sha256'], profile=confirmed["profile"], build="user" if confirmed["profile"] == "release" else "dev", images=images,
                    next_profile=next((item["profile"] for item in images if item["slot"] == selected[0][53]), confirmed["profile"]),
                    candidate_pending=selected[0][53] != 255 or bool(selected[3]))
    except (ValueError, OSError, KeyError, struct.error) as error:
        return dict(state="invalid", present=len(present), required=len(CORE_FILES), detail=str(error))


def _resume_context(identity, linnet, *, initialized=True):
    from tools.studio import native_recovery
    booted = native_recovery.pending(identity, linnet) if initialized else None
    if booted or native_recovery.eligible(identity):
        return booted
    if linnet["state"] not in ("incomplete", "installed"):
        return None
    transactions = cache_root(identity) / "transactions"
    if not transactions.is_dir():
        return None
    root = Path(identity["root"])
    for transaction in sorted(transactions.iterdir(), reverse=True):
        context_path = transaction / "context.json"
        package = transaction / "storage"
        try:
            context = json.loads(context_path.read_text(encoding="utf-8"))
            if (context.get("target_id") != identity["target_id"] or
                    context.get("device_id") != identity["disk_serial"] or
                    context.get("strategy") not in ("rockbox-file", "native-directory") or
                    context.get("build") not in ("user", "dev")):
                continue
            method = _saved_method(context)
            if method is None or context["strategy"] != INSTALLATION_METHODS[method]["strategy"]:
                continue
            context["previous_boot"] = _saved_previous_boot(context)
            artifacts = context["artifacts"]
            if (artifacts.get("build") != context["build"] or
                    artifacts.get("installation_method") != method or
                    artifacts.get("boot_selector") != INSTALLATION_METHODS[method]["boot_selector"]):
                continue
            if any(not Path(artifacts[name]).is_file() for name in ("os_image", "boot_image", "boot_wrapper")):
                continue
            manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
            if manifest.get("image") != inspect_image(Path(artifacts['os_image']).read_bytes()):
                continue
            matches = True
            for name in CORE_FILES:
                installed, expected = root / ".LINNET" / name, package / name
                if installed.exists() and (installed.is_symlink() or installed.read_bytes() != expected.read_bytes()):
                    matches = False
                    break
            if not matches:
                continue
            return dict(context, transaction=str(transaction))
        except (KeyError, OSError, ValueError):
            continue
    return None


def _boot_options(linnet, rockbox, others, marker, action, resume, active_override=None):
    """Describe detected boot software and the verified ways to activate it."""
    active_linnet = linnet.get("state") == "installed" and (
        marker is None or marker.get("phase") == "installed") and active_override is None
    resume_build = resume.get("build") if action == "resume" and resume else None
    linnet_selectable = action in ("install", "replace", "adopt")
    options = []
    for build, label, detail in (
            ("user", "Linnet · User build", "Everyday firmware without developer tools."),
            ("dev", "Linnet · Developer build", "Diagnostics and Linnet Link, bound to this iPod.")):
        options.append(dict(
            id=f"linnet:{build}", label=label, detail=detail, kind="linnet", build=build,
            detected=linnet.get("state") != "absent",
            active=active_linnet and linnet.get("build") == build,
            selectable=linnet_selectable or (action == "resume" and resume_build == build)))

    previous = None
    if active_override is None and marker and marker.get("strategy") == "rockbox-file":
        previous = "rockbox"
    elif active_override is None and marker and marker.get("strategy") == "native-directory":
        previous = _saved_previous_boot(marker)
    previous_active = bool(marker and marker.get("phase") == "restored")
    previous_selectable = bool(marker and marker.get("phase") in ("installed", "restored"))

    rockbox_detected = bool(rockbox.get("present"))
    rockbox_active = active_override == "rockbox" or (previous == "rockbox" and previous_active)
    if (not marker and linnet.get("state") == "absent" and rockbox.get("compatible") and
            rockbox.get("bootloader_recorded")):
        rockbox_active = True
    if rockbox_detected:
        if active_override == "rockbox":
            rockbox_detail = "The current verified Rockbox boot file supersedes Linnet's saved boot state."
        elif marker and marker.get("phase") == "installed" and marker.get("installation_method") == "native-selector":
            rockbox_detail = "Available in Linnet’s Menu-held startup chooser."
            if previous == "rockbox":
                rockbox_detail += " Restore the saved Rockbox boot path to make it the sole default."
        elif marker and marker.get("phase") == "installed" and marker.get("installation_method") == "native-preserve":
            rockbox_detail = "Files are preserved; choose Rockbox under Startup software to register them in Linnet’s startup chooser."
        elif previous == 'rockbox' and marker.get('phase') == 'installed' and marker.get('strategy') == 'rockbox-file':
            rockbox_detail = "The saved Rockbox runtime is available in Linnet’s startup chooser; its original boot file can also be restored and verified."
        elif previous == "rockbox":
            rockbox_detail = "Saved Rockbox boot file can be restored and verified."
        elif rockbox.get("compatible") and rockbox.get("bootloader_recorded") is False:
            rockbox_detail = ("Rockbox files are present; its bootloader installation has not been verified. "
                              "Native Linnet can start a verified Rockbox runtime directly.")
        else:
            rockbox_detail = "Detected, but no verified saved Rockbox boot file is available."
        options.append(dict(
            id="rockbox", label="Rockbox", kind="previous", detected=True,
            active=rockbox_active, selectable=previous == "rockbox" and previous_selectable,
            detail=rockbox_detail))

    apple_active = previous == "apple" and previous_active
    if not marker and linnet.get("state") == "absent" and not rockbox_active:
        apple_active = not rockbox.get('selector_compatible')
    options.append(dict(
        id="apple", label="Apple iPod software", kind="previous", detected=True,
        active=apple_active, selectable=previous == "apple" and previous_selectable,
        detail=("Saved original firmware directory can be restored and verified."
                if previous == "apple" else
                "Detected; no verified route is saved for making it the default boot.")))

    for name in others:
        if name == "Rockbox":
            continue
        options.append(dict(
            id="detected:" + name.lower(), label=name, kind="unsupported", detected=True,
            active=False, selectable=False,
            detail="Detected; default-boot switching is not yet supported for this installation."))
    return options


def default_boot(review):
    """Return the single boot option currently established as the default."""
    from tools.studio.startup_default import inspect as startup_state
    startup = startup_state(review)
    if startup['supported']:
        choice = startup['default']
        selected = ('linnet:' + review['linnet']['build']) if choice == 'linnet' else choice
        option = next((item for item in review.get('boot_options', []) if item['id'] == selected), None)
        if option:
            return dict(id=option['id'],label=option['label'],detail=option['detail'])
    active = [item for item in review.get("boot_options", []) if item.get("active")]
    if len(active) == 1:
        item = active[0]
        return dict(id=item["id"], label=item["label"], detail=item["detail"])
    return dict(id="unknown", label="Unknown", detail="The current default boot could not be verified.")


def _previous_boot(review):
    """Return the boot path a reversible install must restore."""
    if review.get("action") == "resume" and review.get("resume"):
        return _saved_previous_boot(review["resume"])
    marker = review.get("recovery")
    if review.get("action") == "replace" and marker:
        if marker.get("strategy") == "rockbox-file":
            return "rockbox"
        return _saved_previous_boot(marker)
    active = default_boot(review)["id"]
    return active if active in ("apple", "rockbox") else "apple"


def assess(identity, *, _validated_identity=False):
    """Inspect a mounted device without changing it."""
    if not _validated_identity:
        from tools.content.media_manager import refresh
        identity = refresh(identity)
    root = Path(identity["root"]).resolve(strict=True)
    target_id = identity["target_id"]
    spec = TARGETS[target_id]
    install_spec = spec["installer"]
    expected_model = install_spec["wrapper_model"]
    linnet = installed_state(root, target_id)
    resume = None
    rockbox_file = root.joinpath(*install_spec["file_loader"].split("/"))
    wrapper = _wrapper(rockbox_file, install_spec, spec['stage_bytes']+8)
    rockbox = dict(present=(root / ".rockbox").is_dir(), boot_file=str(rockbox_file),
                   boot_file_present=rockbox_file.is_file(), model_code=wrapper,
                   compatible=wrapper == expected_model,
                   selector_compatible=_selector_rockbox(rockbox_file, spec))
    marker = None
    marker_path = root / JOURNAL
    if marker_path.is_file() and not marker_path.is_symlink() and marker_path.stat().st_size <= 65536:
        try:
            value = json.loads(marker_path.read_text(encoding="utf-8"))
            same_storage = value.get("device_id") == identity["disk_serial"]
            same_native_usb = (identity.get("native_target_id") == target_id and
                               value.get("linnet_usb_serial") == identity.get("linnet_usb_serial"))
            if (value.get("target_id") == target_id and
                    (same_storage or same_native_usb) and
                    value.get("strategy") in ("rockbox-file", "native-directory") and
                    value.get("phase") in ("installed", "restored", "boot-upgrade-pending")):
                marker = value
        except (ValueError, OSError):
            marker = None
    if marker is None and linnet["state"] != "absent":
        bootstrap_path = root / ".LINNET" / "RECOVERY" / "BOOTSTRAP.JSON"
        if bootstrap_path.is_file() and not bootstrap_path.is_symlink():
            try:
                from tools.studio.bootstrap import status as bootstrap_status
                bootstrap = bootstrap_status(root)
                phase = bootstrap.get("phase")
                active = bootstrap.get("candidate_sha256") if phase == "installed" else bootstrap.get("original_sha256")
                if (bootstrap.get("target_id") == target_id and phase in ("installed", "restored") and
                        bootstrap.get("active_sha256") == active and bootstrap.get("backup_valid")):
                    marker = dict(target_id=target_id, device_id=identity["disk_serial"],
                                  strategy="rockbox-file", installation_method="rockbox-loader",
                                  phase=phase, recovered_from="BOOTSTRAP.JSON")
            except (ValueError, OSError):
                pass
    recovery_error = None
    if install_spec["boot_layout"] == "s5l-nor" and marker is None:
        from tools.studio.classic_recovery import find as classic_recovery
        try:
            marker = classic_recovery(identity)
        except (OSError, ValueError, TypeError, KeyError) as error:
            recovery_error = str(error)
    external_rockbox = bool(rockbox['selector_compatible'] and marker and marker.get('strategy') == 'rockbox-file' and
        marker.get('phase') == 'installed')
    rockbox["bootloader_recorded"] = _rockbox_bootloader_record(root)
    rockbox["loader_available"] = bool(
        rockbox["bootloader_recorded"] or
        marker and marker.get("strategy") == "rockbox-file" and marker.get("phase") == "installed")
    if marker and marker.get("strategy") == "rockbox-file" and not rockbox["selector_compatible"]:
        saved_rockbox = root / '.LINNET' / 'RECOVERY' / 'ORIGINAL.IPOD'
        rockbox['selector_compatible'] = _selector_rockbox(saved_rockbox, spec)
        if rockbox['selector_compatible']:
            rockbox['boot_file'] = str(saved_rockbox)
    rockbox["supersedes_linnet_recovery"] = external_rockbox
    if marker is None or marker.get("phase") == "restored":
        resume = _resume_context(identity, linnet, initialized=marker is None)
    others = []
    for name, paths in (("iPodLinux", ("iPodLinux", "linux.bin", "loader2")),
                        ("Podzilla", ("podzilla", ".podzilla"))):
        if any((root / value).exists() for value in paths):
            others.append(name)
    if rockbox["present"]:
        others.insert(0, "Rockbox")
    native = not external_rockbox and identity.get("native_target_id") == target_id and (
        install_spec["boot_layout"] == "s5l-nor" or
        install_spec["boot_layout"] == "ipod-directory" and linnet["state"] == "installed")
    classic_pending = None
    classic_conflict = False
    if install_spec['boot_layout'] == 's5l-nor':
        from tools.studio.classic_setup import pending as pending_classic_setup
        saved_setups = pending_classic_setup(identity)
        if len(saved_setups) > 1:
            classic_conflict = True
        elif saved_setups:
            classic_pending = saved_setups[0]
    classic_initial = (install_spec['boot_layout'] == 's5l-nor' and not native and
                       linnet['state'] == 'absent' and marker is None and not recovery_error and not classic_conflict)
    if marker and marker.get("phase") == "boot-upgrade-pending":
        action, strategy = "attention", None
        summary = "A startup upgrade was interrupted. Resume it before changing iPod setup."
    elif classic_conflict:
        action, strategy = 'attention', None
        summary = 'Multiple unfinished Classic installations were found. Resolve their saved recovery records before continuing.'
    elif classic_pending:
        action, strategy = 'resume', 'classic-bootstrap'
        resume = dict(build=classic_pending['build'], artifacts=classic_pending['artifacts'],
                      transaction=str(Path(classic_pending['path']).parent), installation_method='native-preserve')
        summary = 'A Classic installation is ready to resume. Media Manager will reconnect and continue the saved setup.'
    elif classic_initial:
        action, strategy = 'install', 'classic-bootstrap'
        summary = 'Install Linnet Boot and OS together. Media Manager checks this Classic, saves its original startup software, and guides you through the one button step.'
    elif native and resume:
        action, strategy = 'resume', resume['strategy']
        summary = 'Finish verifying the interrupted installation through Linnet Disk Mode. Your installed software and media are kept.'
    elif native and linnet["state"] == "installed":
        action, strategy = "replace", "native-volume"
        summary = "Linnet is installed. Update the OS through native Disk Mode, preserving the confirmed OS for rollback."
    elif native and linnet["state"] in ("absent", "incomplete"):
        action, strategy = "install", "native-volume"
        summary = "Native Boot is running. Install Linnet on this iPod; an interrupted initial installation requires the same build."
    elif not install_spec["enabled"]:
        action, strategy = "unsupported", None
        summary = recovery_error or spec.get("bringup_blocker", "This target is recognized, but installation is not enabled for it.")
    elif external_rockbox and linnet["state"] != "absent":
        action, strategy = "adopt", "rockbox-file"
        summary = ("Rockbox is the current default boot. The existing Linnet files can be "
                   "preserved before switching back to Linnet.")
    elif linnet["state"] != "absent" and marker:
        action, strategy = "replace", marker["strategy"]
        if linnet["state"] == "installed":
            summary = f"{build_description(linnet)} is installed. Switch builds or reinstall, including the same version."
        else:
            summary = "The existing Linnet files use another layout and can be preserved before a fresh install."
    elif resume:
        action, strategy = "resume", resume["strategy"]
        summary = "A matching interrupted Linnet installation is ready to resume."
    elif linnet["state"] == "installed":
        action, strategy = "installed", "existing"
        summary = (f"{build_description(linnet)} is installed. Startup changes are unavailable because "
                   "this installation has no saved installer recovery record. Your media can still be managed normally.")
    elif linnet["state"] in ("incomplete", "invalid"):
        action, strategy = "attention", None
        summary = linnet.get("detail", "The existing Linnet installation needs inspection.")
    elif not install_spec["enabled"]:
        action, strategy = "unsupported", None
        summary = "This target is recognized, but installation is not enabled for it."
    else:
        action, strategy = "install", "native-directory"
        summary = "Ready for a native, reversible installation with an original firmware backup."
    boot_options = _boot_options(
        linnet, rockbox, others, marker, action, resume,
        active_override="rockbox" if external_rockbox else None)
    installation_methods, default_method = _installation_methods(action, rockbox, marker, resume)
    if classic_conflict:
        installation_methods, default_method = [], None
        for option in boot_options:option['selectable']=False
    elif classic_initial or classic_pending:
        installation_methods = [dict(id='native-preserve',label='Linnet with Apple fallback',
            detail='Media Manager installs Boot and OS and preserves Apple and compatible Rockbox startup choices.',
            strategy='classic-bootstrap',enabled=True,requires_admin=False)]
        default_method = 'native-preserve'
    elif native and action != 'resume':
        available = (linnet["state"] in ("installed", "absent", "incomplete") and
                     not linnet.get("candidate_pending") and
                     not (marker and marker.get("phase") == "boot-upgrade-pending"))
        initial = linnet["state"] in ("absent", "incomplete")
        boot_options = [option for option in boot_options if option["kind"] == "linnet" or
                        marker and marker.get("phase") == "restored" and option["id"] == _saved_previous_boot(marker)]
        for option in boot_options:
            if option["kind"] == "linnet":
                option["selectable"] = available
        if linnet['state'] == 'installed' and not (marker and marker.get('phase') in ('restored','boot-upgrade-pending')):
            from tools.studio.apple_firmware import available as apple_available
            if apple_available(root, target_id):
                boot_options.append(dict(id="apple", label="Apple iPod software", kind="previous",
                    detected=True, active=False, selectable=False, chooser_available=True,
                    detail="Available on the iPod: hold Menu during startup, then choose Apple iPod software."))
            if marker and marker.get('strategy') in ('native-directory','native-nor','rockbox-file') and rockbox['selector_compatible']:
                from tools.studio.startup_default import _rockbox_available
                configured = _rockbox_available(root, marker, dict(rockbox=rockbox))
                boot_options.append(dict(id='rockbox',label='Rockbox',kind='previous',detected=True,
                    active=False,selectable=False,chooser_available=configured,
                    detail=('Available on the iPod: hold Menu during startup, then choose Rockbox.' if configured else
                            'Detected. Choose Rockbox under Startup software to enable it in the startup chooser.')))
        installation_methods = [dict(id="native-volume", label="Install Linnet" if initial else "Update installed Linnet",
            detail="Install the OS and safely eject to start Linnet." if initial else "Publish an OS update to the inactive slot. Safely eject to start it; the confirmed OS is retained for rollback.",
            strategy="native-volume", enabled=available, requires_admin=False)]
        default_method = "native-volume"
    elif external_rockbox and identity.get('native_target_id') == target_id:
        # Keep the original Apple/Rockbox disk identity for the reinstall.
        action, strategy = 'attention', None
        summary = ('Rockbox replaced Linnet’s startup file. Restart the iPod and reconnect through '
                   'Rockbox or Apple Disk Mode to reinstall Linnet.')
        installation_methods, default_method = [], None
        for option in boot_options:
            option['selectable'] = False
    elif not install_spec["enabled"]:
        installation_methods, default_method = [], None
        for option in boot_options:
            option["selectable"] = False
            option["active"] = False
            if option["id"] == "apple":
                option["detail"] = "Apple Disk Mode detected; normal firmware startup has not been verified."
    pending_uninstall = _pending_uninstall(identity, root)
    from tools.studio import native_recovery
    native_directory_recovery = native_recovery.available(identity, marker)
    from tools.content.media_trash import summary as trash_summary
    return dict(checked_at=_now(), identity=identity,
                target=dict(id=target_id, name=spec["name"],
                            installer_enabled=install_spec["enabled"] or native or classic_initial or bool(classic_pending), boot_layout=install_spec["boot_layout"],
                            wrapper_model=expected_model),
                retail_os=not native, linnet=linnet, rockbox=rockbox, other_installations=others,
                boot_options=boot_options, installation_methods=installation_methods,
                default_installation_method=default_method,
                uninstall=_uninstall_option(marker, linnet, root,
                                            identity.get("native_target_id") == target_id,
                                            pending_uninstall, install_spec["boot_layout"], recovery_error,
                                            native_directory_recovery),
                recovery=marker, resume=resume, action=action, strategy=strategy, summary=summary,
                trash=trash_summary(identity),
                classic_setup=classic_pending,
                preservation=dict(formats_disk=False, repartitions_disk=False, deletes_media=False,
                                  overwrites_existing_linnet_files=native, archives_previous_linnet=not native,
                                  preserves_settings=True, preserves_media_libraries=True,
                                  archive_contains_superseded_system_files_only=True,
                                  original_boot_backed_up=not native))


def _private_stamp(path):
    info = path.stat()
    return [info.st_dev, info.st_ino, info.st_ctime_ns]


def _pending_uninstall(identity, root):
    """Find a host recovery copy for this exact unfinished device directory."""
    from tools.content.guard import guarded_path

    private = root / ".LINNET"
    try:
        guarded_path(private, root)
        if not private.is_dir():
            return None
        stamp = _private_stamp(private)
    except (OSError, ValueError):
        return None
    scope = cache_root(identity)
    matches = []
    for receipt_path in scope.glob("uninstall-*.json"):
        try:
            if receipt_path.is_symlink() or receipt_path.stat().st_size > 1048576:
                continue
            value = json.loads(receipt_path.read_text(encoding="utf-8"))
            saved = value.get("identity", {})
            backup = receipt_path.with_suffix(".recovery")
            if (value.get("operation") != "uninstall-linnet" or value.get("passed") or
                    value.get("phase") not in ("failed", "remove-linnet-files") or
                    value.get("private_stamp") != stamp or
                    value.get("recovery_backup") != str(backup) or
                    value.get("previous_boot") not in ("apple", "rockbox") or
                    not value.get("boot", {}).get("passed") or
                    not backup.is_dir() or backup.is_symlink() or
                    not (backup / "INSTALL.JSON").is_file() or
                    any(saved.get(key) != identity.get(key) for key in
                        ("disk_serial", "volume_serial", "volume_unique_id", "target_id", "disk_bytes", "partition_offset"))):
                continue
            matches.append((receipt_path.stat().st_mtime_ns, value, backup))
        except (OSError, ValueError, TypeError, AttributeError):
            continue
    return max(matches, default=None, key=lambda item: item[0])[1:] if matches else None


def _uninstall_option(marker, linnet, root, native, pending=None, boot_layout=None, recovery_error=None,
                      native_directory_recovery=False):
    if marker and marker.get("phase") in ("installed", "restored") or pending:
        classic = bool(marker and marker.get("strategy") == "native-nor")
        if native and not classic and not native_directory_recovery:
            return dict(available=False, previous=None,
                        detail="Restart into Apple iPod software, reconnect in Apple Disk Mode, then refresh to remove Linnet OS. The saved boot backup is bound to the original Apple storage identity.")
        if classic and not native and marker["phase"] != "restored":
            return dict(available=False, previous=None,
                        detail="This Classic uses Linnet’s NOR bootloader. Start Linnet, open Disk Mode, and reconnect to restore the verified original boot flash before removal.")
        if classic and native and linnet.get("next_profile", linnet.get("profile")) == "release":
            return dict(available=False, previous=None,
                        detail="This Classic’s NOR backup is verified. Removal needs Linnet Link: install a Developer build, start it, then reconnect in Linnet Disk Mode.")
        if (root / "LNETPEND").exists() or (root / "LNETPEND").is_symlink():
            return dict(available=False, previous=None,
                        detail="An unfinished Linnet reinstall archive (LNETPEND) is on this iPod. Resolve that archive before removing Linnet OS so its saved media is not lost.")
        previous = ("Rockbox" if pending and pending[0]["previous_boot"] == "rockbox" or
                    marker and (marker.get("strategy") == "rockbox-file" or _saved_previous_boot(marker) == "rockbox")
                    else "Apple iPod software")
        trash = root / ".LINNET" / "TRASH"
        if trash.exists() or trash.is_symlink():
            from tools.content.guard import guarded_path
            try:
                guarded_path(trash, root)
                if not trash.is_dir():
                    raise ValueError("Trash is not a directory")
                if any(trash.iterdir()):
                    return dict(available=False, previous=previous, blocker="trash",
                                detail="Trash isn't empty. Empty Trash before removing Linnet. Choose Review Trash to check its contents first.")
            except (OSError, ValueError):
                return dict(available=False, previous=None,
                            detail="Could not inspect Trash. Reconnect the iPod and refresh before removing Linnet OS.")
        return dict(available=True, previous=previous,
                    detail=(f"Finish removing Linnet after the interrupted cleanup. The verified {previous} startup backup is saved on this computer."
                            if pending else
                            "Original boot flash has already been restored and verified. Finish removing Linnet’s files from this iPod."
                            if classic and marker["phase"] == "restored" else
                            (f"Restore {previous} startup and remove Linnet’s system files. " +
                             ("After restoration, follow the button instructions to enter Apple Disk Mode for cleanup; keep USB connected."
                              if previous == 'Rockbox' else
                              "Media Manager handles the restart and cleanup automatically; keep USB connected."))
                            if classic and marker["phase"] != "restored" else
                            f"Restore {previous} as the default startup software, then remove Linnet’s files from this iPod."))
    if linnet.get("state") != "absent":
        if boot_layout == "s5l-nor":
            return dict(available=False, previous=None,
                        detail=("The Classic’s NOR bootloader needs its verified original flash backup and installation record. "
                                "An OS reinstall does not recreate them." + (" " + recovery_error if recovery_error else "")))
        return dict(available=False, previous=None,
                    detail="Linnet files were found, but no verified installer recovery record is available. Removal is disabled to protect the iPod’s startup software.")
    return dict(available=False, previous=None, detail="Linnet is not installed on this iPod.")


def _run_build(arguments):
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    if getattr(sys, "frozen", False):
        raise ValueError("Build from source using the Media Manager in your Linnet source checkout.")
    command = [sys.executable, "-X", "utf8", str(ROOT / "linnet"), "build", *arguments]
    result = subprocess.run(command, cwd=ROOT, text=True, encoding="utf-8", capture_output=True,
                            creationflags=creationflags)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip() or "Linnet build failed")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        detail = (result.stderr or result.stdout).strip()
        suffix = f": {detail[:500]}" if detail else ": the build produced no output"
        raise RuntimeError("Linnet build returned an unreadable report" + suffix) from error


def build_artifacts(identity, progress=lambda _message: None, build="user",
                    installation_method="native-preserve", *, boot_profile=None):
    if build not in ("user", "dev"):
        raise ValueError("installer build must be user or dev")
    if installation_method not in INSTALLATION_METHODS:
        raise ValueError("unsupported Linnet installation method")
    target, serial = identity["target_id"], identity["linnet_usb_serial"]
    os_profile = "release" if build == "user" else "dev"
    boot_profile=boot_profile or os_profile
    if boot_profile!=os_profile and not (target=='classic6g-n25' and boot_profile=='release' and installation_method=='native-preserve'):
        raise ValueError('Only Classic native Boot may use a separate release profile')
    progress(f"Building the Linnet {build} OS for {TARGETS[target]['name']}…")
    os_arguments = ["--target", target, "--profile", os_profile,
                    "--kind", "os", "--verify"]
    if os_profile == "dev":
        os_arguments += ["--device-serial", serial]
    os_report = _run_build(os_arguments)
    progress("Building the recoverable device loader…")
    boot_arguments = ["--target", target, "--profile", boot_profile,
                      "--kind", "boot", "--verify"]
    boot_selector = INSTALLATION_METHODS[installation_method]["boot_selector"]
    if boot_selector != "none":
        boot_arguments += ["--boot-selector", boot_selector]
    if boot_profile == "dev":
        boot_arguments += ["--device-serial", serial]
    boot_report = _run_build(boot_arguments)
    def artifact(report, name):
        path = ROOT / report["artifacts"][name]
        return path.resolve(strict=True)
    os_image, boot_image = artifact(os_report, "los"), artifact(boot_report, "los")
    boot_wrapper = artifact(boot_report, "ipod")
    artifacts = dict(os_image=str(os_image), os_report=str(os_image.with_suffix(".json")),
                     boot_image=str(boot_image), boot_wrapper=str(boot_wrapper),
                      boot_report=str(boot_image.with_suffix(".json")),
                      build=build, os_profile=os_profile, boot_profile=boot_profile, os_build_id=os_report["build_id"],
                      boot_build_id=boot_report["build_id"],
                      installation_method=installation_method, boot_selector=boot_selector)
    _validate_artifacts(identity, artifacts)
    if TARGETS[target]['capabilities'].get('games',False):
        from tools.studio.game_build import build_library
        progress('Preparing native game packages…')
        artifacts['game_library']=build_library(target,ROOT/'out/game-packages'/target)['directory']
    return artifacts


def _validate_artifacts(identity, artifacts):
    spec = TARGETS[identity["target_id"]]
    if artifacts.get('game_library'):
        from tools.studio.game_build import library
        library(artifacts['game_library'],identity['target_id'])
    install_spec = spec["installer"]
    paths = {key: Path(artifacts[key]).resolve(strict=True) for key in
             ("os_image", "os_report", "boot_image", "boot_wrapper", "boot_report")}
    os_data, boot_data = paths["os_image"].read_bytes(), paths["boot_image"].read_bytes()
    os_identity, boot_identity = inspect_image(os_data), inspect_image(boot_data)
    if len(os_data) > spec["os_capacity_bytes"] or len(boot_data) > spec["os_capacity_bytes"]:
        raise ValueError("installer image exceeds the target OS capacity")
    build = artifacts.get("build")
    installation_method = artifacts.get("installation_method")
    if installation_method not in INSTALLATION_METHODS:
        raise ValueError("installer artifact installation method is invalid")
    expected_selector = INSTALLATION_METHODS[installation_method]["boot_selector"]
    if artifacts.get("boot_selector") != expected_selector:
        raise ValueError("installer Boot selector differs from the selected installation method")
    expected_profile = "release" if build == "user" else "dev" if build == "dev" else None
    if expected_profile is None or expected_profile != artifacts.get("os_profile"):
        raise ValueError("installer artifact build selection is invalid")
    boot_profile=artifacts.get('boot_profile')
    if boot_profile!=expected_profile and not (identity['target_id']=='classic6g-n25' and
            boot_profile=='release' and installation_method=='native-preserve'):
        raise ValueError('Invalid installer Boot profile')
    if ((os_identity["target_id"], os_identity["profile"], os_identity["build_id"]) !=
            (identity["target_id"], expected_profile, artifacts["os_build_id"]) or
            (boot_identity["target_id"], boot_identity["profile"], boot_identity["build_id"]) !=
            (identity["target_id"], boot_profile, artifacts["boot_build_id"])):
        raise ValueError("installer image identity does not match the detected iPod")
    from tools.studio.bootstrap import ipod_payload
    wrapped, _model = ipod_payload(paths["boot_wrapper"].read_bytes(),
                                   install_spec["wrapper_model"].encode("ascii"),
                                   install_spec["wrapper_checksum_seed"])
    if wrapped != boot_data[128:]:
        raise ValueError("installer Boot wrapper differs from its validated image")
    for kind, report_path, image_identity in (("os", paths["os_report"], os_identity),
                                               ("boot", paths["boot_report"], boot_identity)):
        if report_path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("installer build report exceeds its bound")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        profile=boot_profile if kind=='boot' else expected_profile
        expected_serial = "" if profile == "release" else identity["linnet_usb_serial"]
        if (not isinstance(report, dict) or not isinstance(report.get("image"), dict) or
                report.get("kind") != kind or report.get("profile") != profile or
                report.get("probe") != "frame" or
                report.get("target_id") != identity["target_id"] or
                report.get("device_serial") != expected_serial or
                report.get("build_id") != image_identity["build_id"] or
                report.get("image", {}).get("sha256") != image_identity["sha256"]):
            raise ValueError("installer build report does not match its image")
        if kind == "boot" and report.get("boot_selector") != expected_selector:
            raise ValueError("installer Boot report uses the wrong startup selector")
        if kind == "boot" and report.get("apple_firmware_fallback") is not True:
            raise ValueError("installer Boot report does not preserve the Apple firmware fallback")
        if kind == "boot" and report.get("startup_default_format") != 1:
            raise ValueError("installer Boot image does not support startup defaults")
        if profile == 'release':
            exclusion = report.get("release_exclusion")
            if not isinstance(exclusion, dict) or exclusion.get("passed") is not True:
                raise ValueError("user installer artifacts require release exclusion verification")
        elif 'serial_binding' in report:
            from tools.studio.firmware_binding import validate_binding
            validate_binding(os_data if kind == 'os' else boot_data, report, expected_serial)


def _same_device(expected, observed):
    def values(item):
        return (item["target_id"], item["disk_number"], item["disk_serial"], item["disk_bytes"],
                item["partition_offset"], item["partition_bytes"], item["linnet_usb_serial"])
    if values(expected) != values(observed):
        raise RuntimeError("iPod identity changed after installation review")


def _transaction(identity, artifacts):
    return cache_root(identity) / "transactions" / f"{artifacts['build']}-{artifacts['os_build_id'][:16]}-{uuid.uuid4().hex}"


def reboot_plan(disk_number, expected_serial):
    """Resolve the current native plan recorded by the installed iPod."""
    from tools.content.media_manager import discover
    from tools.studio import native_bootstrap as native

    matches = [item for item in discover()
               if item.get("disk_number") == disk_number and
               item.get("disk_serial") == expected_serial]
    if len(matches) != 1:
        raise ValueError("the selected disk does not resolve to exactly one supported iPod")
    identity = matches[0]
    root = Path(identity["root"]).resolve(strict=True)
    marker_path = root / JOURNAL
    if (not marker_path.is_file() or marker_path.is_symlink() or
            marker_path.stat().st_size > 65536):
        raise ValueError("the installed iPod has no usable recovery record")
    marker = json.loads(marker_path.read_text(encoding="utf-8"))
    if (marker.get("target_id") != identity["target_id"] or
            marker.get("device_id") != identity["disk_serial"] or
            marker.get("strategy") != "native-directory" or
            marker.get("phase") != "installed"):
        raise ValueError("the installed recovery record does not describe active native Boot")
    if not isinstance(marker.get("plan"), str) or not marker["plan"]:
        raise ValueError("the installed recovery record has no plan path")
    plan_path = Path(marker["plan"]).resolve(strict=True)
    plan_sha = marker.get("plan_sha256", "")
    try:
        native.load_plan(plan_path, plan_sha)
    except ValueError as error:
        if str(error) == "unsupported native installation plan":
            raise ValueError("installed Boot metadata is obsolete; reinstall Linnet with the current installer") from error
        raise
    return plan_path, plan_sha


def _persist_artifacts(artifacts, transaction):
    destination = transaction / "artifacts"
    destination.mkdir(parents=True, exist_ok=True)
    names = {"os_image": "operating-system.los", "os_report": "operating-system.json",
             "boot_image": "boot.los", "boot_wrapper": "boot.ipod",
             "boot_report": "boot.json"}
    persisted = dict(artifacts)
    for key, name in names.items():
        source, target = Path(artifacts[key]).resolve(strict=True), destination / name
        data = source.read_bytes()
        if target.exists():
            if target.is_symlink() or target.read_bytes() != data:
                raise ValueError("saved installer artifact differs: " + name)
        else:
            with target.open("xb") as writer:
                writer.write(data)
                writer.flush()
                os.fsync(writer.fileno())
            if target.read_bytes() != data:
                raise OSError("saved installer artifact failed verification: " + name)
        persisted[key] = str(target.resolve())
    if artifacts.get('game_library'):
        from tools.studio.game_build import library,library_files
        target_id=inspect_image(Path(artifacts['os_image']).read_bytes())['target_id']
        source=Path(artifacts['game_library']);packages=library_files(source,target_id)
        game_destination=destination/'games';game_destination.mkdir(exist_ok=True)
        for path in packages:
            target=game_destination/path.name;data=path.read_bytes()
            if target.exists():
                if target.is_symlink() or target.read_bytes()!=data:raise ValueError('Saved game artifact differs')
            else:
                with target.open('xb') as stream:stream.write(data);stream.flush();os.fsync(stream.fileno())
        library(game_destination,target_id)
        persisted['game_library']=str(game_destination.resolve())
    return persisted


def _package_storage(identity, artifacts, transaction, archive=None):
    from tools.studio import storage, theme_settings
    package = transaction / "storage"
    if package.exists():
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        if manifest.get("image") != inspect_image(Path(artifacts['os_image']).read_bytes()):
            raise ValueError("saved storage transaction belongs to another build")
    else:
        storage.prepare(Path(artifacts["os_image"]), package)
    receipt = storage.provision(package, Path(identity["root"]), identity["disk_serial"])
    if archive is not None:
        receipt["reinstall_preservation"] = _restore_reinstall_data(identity["root"], archive)
    receipt["theme"] = theme_settings.provision(
        Path(identity["root"]), identity["disk_serial"], identity["disk_bytes"],
        artifacts["os_build_id"])
    from tools.content.publish import provision_collections
    def guard_collections():
        root, current = storage._mount_identity(Path(identity["root"]), identity["disk_serial"])
        if current.get("size") != identity["disk_bytes"]:
            raise ValueError("disk changed during playlist provisioning")
    provision_collections(Path(identity["root"]) / ".LINNET", guard_collections)
    if TARGETS[identity['target_id']]['capabilities'].get('games',False) and artifacts.get('game_library'):
        from tools.content import games
        selected=games.read(identity)
        games.save(identity,selected,library=artifacts['game_library'])
        receipt['games']=dict(installed=len(selected),format=1)
    return receipt


def _install_selector_config(identity, *, boot_file=None, replace=False):
    root = Path(identity["root"]).resolve(strict=True)
    spec = TARGETS[identity["target_id"]]
    install_spec = spec["installer"]
    rockbox = Path(boot_file) if boot_file is not None else root.joinpath(*install_spec['file_loader'].split('/'))
    from tools.content.publish import _plain, _volume_guard
    _plain(rockbox, root)
    if not _selector_rockbox(rockbox, spec):
        raise ValueError("the Rockbox image is not compatible with the native startup selector")
    from tools.content.music import device_media_path
    firmware_path = device_media_path(rockbox, root)
    encoded = firmware_path.encode("ascii")
    if not encoded or len(encoded) > 255:
        raise ValueError("the Rockbox FAT path exceeds the startup selector bound")
    data = bytearray(512)
    data[:8] = b"RBPATH1\0"
    struct.pack_into("<I", data, 8, len(encoded))
    data[12:12 + len(encoded)] = encoded
    struct.pack_into("<I", data, 508, zlib.crc32(data[:508]))
    target = root / ".LINNET" / "RBPATH.CFG"
    _plain(target, root)
    if target.exists():
        if target.is_symlink() or not target.is_file() or target.stat().st_size != 512:
            raise ValueError("the existing Rockbox selector record differs")
        previous=target.read_bytes()
        if previous==data:
            return dict(path=firmware_path, bytes=len(data), sha256=_sha(data))
        if (not replace or previous[:8]!=b'RBPATH1\0' or
                zlib.crc32(previous[:508])!=struct.unpack_from('<I',previous,508)[0]):
            raise ValueError('the existing Rockbox selector record differs')
    temporary=target.with_name(target.name+'.tmp-'+uuid.uuid4().hex)
    _plain(temporary,root)
    owned=False
    try:
        _volume_guard(identity)
        with temporary.open('xb') as writer:
            owned=True
            writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        if temporary.read_bytes()!=data:
            raise OSError('the Rockbox selector staging failed verification')
        _volume_guard(identity)
        os.replace(temporary,target)
    finally:
        if owned and temporary.exists():
            _volume_guard(identity);_plain(temporary,root);temporary.unlink()
    _volume_guard(identity)
    if target.read_bytes() != data:
        raise OSError("the Rockbox selector record failed verification")
    return dict(path=firmware_path, bytes=len(data), sha256=_sha(data))


def _install_apple_firmware(identity, plan_path, plan_sha256, archive=None):
    """Publish the exact preserved RetailOS payload consumed by native Boot."""
    from tools.studio import native_bootstrap as native
    manifest, _artifacts, firmware = native.load_plan(plan_path, plan_sha256)
    if manifest["target_id"] != identity["target_id"]:
        raise ValueError("Apple firmware plan target differs from the connected iPod")
    original = manifest["original_osos"]
    previous_boot = native.original_boot(original,firmware)
    start, size = original["payload_offset"], original["payload_bytes"]
    payload = firmware[start:start + size]
    from tools.studio.apple_firmware import encode, matches_signature, publish
    if not matches_signature(identity["target_id"], payload):
        # Reinstall may find Linnet Boot in OSOS. Preserve an archived Apple
        # image only when its target-bound configuration verifies exactly.
        sources = []
        if archive is not None:
            sources.append(Path(archive) / "APPLE.BIN")
        sources.append(Path(identity["root"]) / ".LINNET" / "APPLE.BIN")
        for source in sources:
            config = source.with_name("APPLE.CFG")
            if not source.is_file() or source.is_symlink() or not config.is_file() or config.is_symlink():
                continue
            preserved = source.read_bytes()
            try:
                expected_config = encode(identity["target_id"], preserved)[1]
            except ValueError:
                continue
            if config.read_bytes() == expected_config:
                return dict(publish(identity["root"], identity["target_id"], preserved),previous_boot=previous_boot)
        raise ValueError("Preserved Apple firmware is unavailable; the original OSOS image lacks its target signature")
    return dict(publish(identity["root"], identity["target_id"], payload,
                        original["payload_sha256"]),previous_boot=previous_boot)


def _journal(identity, artifacts, strategy, transaction, extra=None):
    value = dict(target_id=identity["target_id"], device_id=identity["disk_serial"],
                 linnet_usb_serial=identity["linnet_usb_serial"], strategy=strategy,
                 build=artifacts["build"],
                 os_build_id=artifacts["os_build_id"], boot_build_id=artifacts["boot_build_id"],
                 startup_default_format=1,
                 transaction=str(transaction), installed_at=_now(), phase="installed")
    if extra:
        value.update(extra)
    root = Path(identity["root"]).resolve(strict=True)
    path = root / JOURNAL
    from tools.content.publish import _plain
    _plain(path, root)
    if path.exists():
        existing = json.loads(path.read_text(encoding="utf-8"))
        stable = (set(existing) | set(value)) - {"phase", "installed_at", "restored_at"}
        if any(existing.get(key) != value.get(key) for key in stable) or existing.get("phase") != "restored":
            raise ValueError("an installation recovery journal already exists")
        _save(path, value)
    else:
        _save(path, value, replace=False)
    return value


def _install_rockbox(identity, artifacts, progress, transaction, archive=None):
    from tools.studio import bootstrap
    transaction.mkdir(parents=True, exist_ok=True)
    boot_path = Path(identity["root"]).joinpath(*TARGETS[identity["target_id"]]["installer"]["file_loader"].split("/"))
    bootstrap_journal = Path(identity["root"]) / ".LINNET" / "RECOVERY" / "BOOTSTRAP.JSON"
    original_path=bootstrap_journal.parent/'ORIGINAL.IPOD' if bootstrap_journal.is_file() else boot_path
    if not _selector_rockbox(original_path,TARGETS[identity['target_id']]):
        raise ValueError('A verified Rockbox runtime is required before replacing its boot file')
    original_sha = (json.loads(bootstrap_journal.read_text(encoding="utf-8"))["original_sha256"]
                    if bootstrap_journal.is_file() else _sha(boot_path.read_bytes()))
    progress("Preparing Linnet’s transactional storage files…")
    storage_receipt = _package_storage(identity, artifacts, transaction, archive)
    progress("Backing up the Rockbox loader and installing Linnet’s loader…")
    boot_receipt = bootstrap.install(Path(identity["root"]), Path(artifacts["boot_wrapper"]),
                                     Path(artifacts["boot_image"]), original_sha)
    selector = _install_selector_config(identity,boot_file=Path(identity['root'])/'.LINNET/RECOVERY/ORIGINAL.IPOD')
    marker = _journal(identity, artifacts, "rockbox-file", transaction,
                      dict(original_boot_sha256=original_sha,
                           installation_method=artifacts["installation_method"],selector=selector))
    result = dict(passed=True, strategy="rockbox-file", storage=storage_receipt,
                  boot=boot_receipt, recovery=marker, requires_safe_eject=True)
    if archive is not None:
        final_archive = _finalize_reinstall_archive(identity["root"], archive)
        result["previous_linnet_archive"] = final_archive
        storage_receipt["reinstall_preservation"]["archive"] = final_archive
    return result


def _encode(value):
    return base64.b64encode(str(value).encode("utf-8")).decode("ascii")


def _run_elevated(job_path):
    if os.name != "nt":
        raise RuntimeError("native installation currently requires Windows")
    python, job, working = map(_encode, (sys.executable, job_path, ROOT))
    worker_arguments = "" if getattr(sys, "frozen", False) else "'-m','tools.studio.installer',"
    ps = f"""
$ErrorActionPreference='Stop'
function D([string]$s) {{ [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($s)) }}
$p=Start-Process -FilePath (D '{python}') -ArgumentList @({worker_arguments}'--worker-token','{job}') -WorkingDirectory (D '{working}') -Verb RunAs -WindowStyle Hidden -Wait -PassThru
exit $p.ExitCode
"""
    encoded = base64.b64encode(ps.encode("utf-16le")).decode("ascii")
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                            capture_output=True, text=True,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    job_value = json.loads(job_path.read_text(encoding="utf-8"))
    response_path = Path(job_value["result"])
    if not response_path.is_file():
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(detail or "administrator approval was cancelled or the installer worker did not finish")
    response = json.loads(response_path.read_text(encoding="utf-8"))
    if not response.get("passed"):
        failure = str(response.get("failure", "native installation failed; inspect its receipt"))
        while failure.startswith("RuntimeError: "):
            failure = failure[len("RuntimeError: "):]
        raise RuntimeError(failure)
    return response


def _archive_destination(root):
    """Reserve one FAT-safe staging name while retaining the last good archive."""
    from tools.content.publish import _plain
    root = Path(root).resolve(strict=True)
    source = root / ".LINNET"
    _plain(source, root)
    if not source.is_dir() or source.is_symlink():
        raise ValueError("the existing LINNET directory is unsafe or missing")
    candidate = root / "LNETPEND"
    _plain(candidate, root)
    if candidate.exists() or candidate.is_symlink():
        raise ValueError("an unfinished LNETPEND reinstall archive already exists; inspect it before reinstalling")
    return candidate


def _archive_linnet(root, destination):
    from tools.content.publish import _plain
    root, destination = Path(root).resolve(strict=True), Path(destination)
    source = root / ".LINNET"
    _plain(source, root)
    _plain(destination, root)
    if destination.parent != root or destination.exists() or destination.is_symlink():
        raise ValueError("the selected Linnet archive destination is no longer available")
    source.replace(destination)
    if source.exists() or not destination.is_dir():
        raise OSError("the previous Linnet folder was not archived completely")
    return str(destination)


def _finalize_reinstall_archive(root, staging):
    """Rotate a verified staging archive into the single previous-system slot."""
    from tools.content.publish import _plain
    root = Path(root).resolve(strict=True)
    _plain(Path(staging).absolute(), root)
    staging = Path(staging).resolve(strict=True)
    active = root / ".LINNET"
    previous = active / "LNETOLD0"
    _plain(active, root)
    _plain(staging, root)
    _plain(previous, root)
    if (staging.parent != root or staging.name != "LNETPEND" or
            staging.is_symlink() or not staging.is_dir() or not active.is_dir()):
        raise ValueError("the pending Linnet reinstall archive is unsafe or missing")
    # The old archive travels with LINNET into staging during a reinstall.
    # Retire it only after the new installation verifies, without nesting archives.
    obsolete = (previous, staging / "LNETOLD0")
    for path in obsolete:
        _plain(path, root)
        if path.exists() and (not path.is_dir() or path.resolve().parent != path.parent):
            raise ValueError("the previous Linnet system archive is unsafe")
    for path in obsolete:
        if path.exists():
            shutil.rmtree(path)
    staging.replace(previous)
    if staging.exists() or previous.is_symlink() or not previous.is_dir():
        raise OSError("the previous Linnet system archive was not rotated completely")
    return str(previous)


def _restore_reinstall_data(root, archive):
    """Return persistent settings and media from a full reinstall archive."""
    from tools.content.publish import _plain
    from tools.studio.storage import record
    from tools.studio.update_history import _read_record
    root = Path(root).resolve(strict=True)
    active = root / ".LINNET"
    archive = Path(archive).absolute()
    _plain(archive, root)
    archive = archive.resolve(strict=True)
    _plain(active, root)
    _plain(archive, root)
    if (archive not in {root / "LNETPEND", active / "LNETOLD0"} or
            archive.is_symlink() or not archive.is_dir() or
            active.is_symlink() or not active.is_dir()):
        raise ValueError("the Linnet reinstall archive or active directory is unsafe")
    entries = []
    for source in archive.iterdir():
        _plain(source, root)
        if source.name.upper() not in REINSTALL_SYSTEM_ENTRIES:
            entries.append(source)
    state_names = {"STATE0.BIN", "STATE1.BIN"}
    archived_states = {item.name.upper(): item for item in entries if item.name.upper() in state_names}
    if archived_states and set(archived_states) != state_names:
        raise ValueError("the archived settings record pair is incomplete")
    state_records = {}
    for name, path in archived_states.items():
        state_records[name] = _read_record(path)
    if (len(state_records) == 2 and state_records["STATE0.BIN"][1] == state_records["STATE1.BIN"][1]
            and state_records["STATE0.BIN"][0] != state_records["STATE1.BIN"][0]):
        raise ValueError("the archived settings records conflict at the same generation")
    planned = []
    empty = record(b"")
    for source in entries:
        target = active / source.name
        _plain(target, root)
        if target.exists() or target.is_symlink():
            if (source.name.upper() not in {"STATE0.BIN", "STATE1.BIN", "THEME0.BIN", "THEME1.BIN", "LISTS0.BIN", "LISTS1.BIN"}
                    or target.is_symlink() or not target.is_file() or target.read_bytes() != empty):
                raise ValueError("fresh installation conflicts with preserved data: " + source.name)
        planned.append((source, target))
    restored = {}
    for source, target in planned:
        if target.exists():
            target.unlink()
        source.replace(target)
        restored[source.name] = dict(kind="directory" if target.is_dir() else "file",
                                     bytes=target.stat().st_size if target.is_file() else None)
    return dict(passed=True, archive=str(archive), restored=restored,
                retained_system=sorted(item.name for item in archive.iterdir()))


def install(identity, progress=lambda _message: None, build=None, installation_method=None,
            firmware=None, from_source=False, instruction=lambda _event: None, cancelled=lambda:False):
    """Keep file-bootloader transactions in the caller's publisher exclusion.

    Native directory transactions are owned by the elevated worker; holding the
    mutex here while waiting for that worker would deadlock it.
    """
    from tools.content.publish import _exclusive
    reviewed = assess(identity)
    from tools.studio.firmware_bundle import selected_build
    build = ((reviewed.get('resume') or reviewed.get('classic_setup') or {}).get('build') if build is None and reviewed['action'] == 'resume'
             else selected_build(firmware, build))
    method_id = installation_method or reviewed["default_installation_method"]
    method = next((item for item in reviewed["installation_methods"]
                   if item["id"] == method_id), None)
    route = (reviewed["action"], (reviewed.get("recovery") or {}).get("strategy"),
             method["strategy"] if method else None)
    mixed_native_restore = (reviewed["action"] == "replace" and
                            (reviewed.get("recovery") or {}).get("strategy") == "native-directory")
    if method and method["strategy"] == "rockbox-file" and not mixed_native_restore:
        with _exclusive(reviewed["identity"]):
            return _install(identity, progress, build, method_id, firmware, from_source, route,
                            instruction=instruction,cancelled=cancelled)
    return _install(identity, progress, build, method_id, firmware, from_source, route,
                    instruction=instruction,cancelled=cancelled)


def _install(identity, progress=lambda _message: None, build="user", installation_method=None,
            firmware=None, from_source=False, reviewed_route=None, *, instruction=lambda _event:None, cancelled=lambda:False):
    if build not in ("user", "dev"):
        raise ValueError("installer build must be user or dev")
    if from_source and firmware is not None:
        raise ValueError("choose a firmware bundle or build from source")
    reviewed = assess(identity)
    if reviewed["action"] not in ("install", "resume", "replace", "adopt"):
        raise ValueError(reviewed["summary"])
    method_id = installation_method or reviewed["default_installation_method"]
    method = next((item for item in reviewed["installation_methods"]
                   if item["id"] == method_id), None)
    route = (reviewed["action"], (reviewed.get("recovery") or {}).get("strategy"),
             method["strategy"] if method else None)
    if route != reviewed_route:
        raise RuntimeError("installation route changed after review; inspect the iPod again")
    if not method or not method["enabled"]:
        raise ValueError("the selected Linnet installation method is unavailable")
    strategy = method["strategy"]
    if strategy == 'classic-bootstrap':
        from tools.studio import classic_setup
        pending=reviewed.get('classic_setup')
        path=(pending['path'] if pending else classic_setup.prepare(reviewed['identity'],
              firmware=firmware,from_source=from_source,build=build,progress=progress))
        return classic_setup.run(path,progress=progress,instruction=instruction,cancelled=cancelled)
    if strategy == "native-volume":
        from tools.studio.native_update import publish
        if from_source:
            profile = "release" if build == "user" else "dev"
            arguments = ["--target", reviewed["identity"]["target_id"], "--profile", profile, "--kind", "os", "--verify"]
            if profile == "dev":
                arguments += ["--device-serial", reviewed["identity"]["linnet_usb_serial"]]
            progress("Building Linnet for this iPod…")
            report = _run_build(arguments)
            image = ROOT / report["artifacts"]["los"]
            game_library=None
            if TARGETS[reviewed['identity']['target_id']]['capabilities'].get('games',False):
                from tools.studio.game_build import build_library
                game_library=build_library(reviewed['identity']['target_id'],ROOT/'out/game-packages'/reviewed['identity']['target_id'])['directory']
        else:
            if firmware is None:
                raise ValueError("Choose a prebuilt Linnet firmware ZIP before installing.")
            from tools.studio.firmware_bundle import load
            progress("Verifying the selected firmware bundle…")
            artifacts=load(firmware, reviewed["identity"], method_id, build=build)
            image = Path(artifacts["os_image"])
            game_library=artifacts.get('game_library')
        if game_library:
            from tools.content import games
            from tools.content.guard import guarded_path,guard_volume
            system=Path(reviewed['identity']['root'])/'.LINNET'
            guarded_path(system,Path(reviewed['identity']['root']));guard_volume(reviewed['identity']);system.mkdir(exist_ok=True)
            progress('Installing selected game packages…')
            games.save(reviewed['identity'],games.read(reviewed['identity']),library=game_library)
        progress("Writing and verifying Linnet…")
        return publish(reviewed["identity"], image)
    previous_boot = _previous_boot(reviewed)
    identity = reviewed["identity"]
    archive = (_archive_destination(identity["root"])
               if reviewed["action"] in ("replace", "adopt") else None)
    if reviewed["action"] == "resume":
        progress("Verifying the saved interrupted installation…")
        artifacts = reviewed["resume"]["artifacts"]
        if reviewed["resume"].get("build") != build or artifacts.get("build") != build:
            raise ValueError("resume must use the build selected by the saved installation")
        if (_saved_method(reviewed["resume"]) != method_id or
                artifacts.get("installation_method") != method_id):
            raise ValueError("resume must use the installation method selected by the saved installation")
        transaction = Path(reviewed["resume"]["transaction"])
        _validate_artifacts(identity, artifacts)
    else:
        if from_source:
            artifacts = build_artifacts(identity, progress=progress, build=build,
                                        installation_method=method_id)
        else:
            if firmware is None:
                raise ValueError("Choose a prebuilt Linnet firmware ZIP before installing.")
            from tools.studio.firmware_bundle import load
            progress("Verifying the selected firmware bundle…")
            artifacts = load(firmware, identity, method_id, build=build)
        transaction = _transaction(identity, artifacts)
    transaction.mkdir(parents=True, exist_ok=True)
    if reviewed["action"] != "resume":
        artifacts = _persist_artifacts(artifacts, transaction)
        _validate_artifacts(identity, artifacts)
    context = dict(target_id=identity["target_id"], device_id=identity["disk_serial"],
                   strategy=strategy, installation_method=method_id,
                   previous_boot=previous_boot, build=build, artifacts=artifacts)
    context_path = transaction / "context.json"
    if reviewed["action"] == "resume":
        if not context_path.is_file():
            raise ValueError("saved install context is missing")
    elif context_path.exists():
        if json.loads(context_path.read_text(encoding="utf-8")) != context:
            raise ValueError("saved install context differs from this transaction")
    else:
        _save(context_path, context, replace=False)
    receipt_path = transaction / ("install-" + uuid.uuid4().hex + ".json")
    receipt = dict(operation="install-linnet", started_at=_now(), passed=False,
                   strategy=strategy, installation_method=method_id, build=build,
                   identity=identity, artifacts=artifacts,
                   preservation=reviewed["preservation"], device_write_attempted=False)
    _save(receipt_path, receipt)
    try:
        current = assess(identity)
        current_method = next((item for item in current["installation_methods"]
                               if item["id"] == method_id), None)
        if current["action"] != reviewed["action"] or not current_method or not current_method["enabled"]:
            raise RuntimeError("installation state changed after review")
        combined_native_reinstall = (
            reviewed["action"] == "replace" and
            (reviewed.get("recovery") or {}).get("strategy") == "native-directory" and
            strategy == "native-directory")
        mixed_native_restore = (
            reviewed["action"] == "replace" and
            (reviewed.get("recovery") or {}).get("strategy") == "native-directory" and
            strategy == "rockbox-file")
        if strategy == "rockbox-file" and reviewed["action"] == "replace" and not mixed_native_restore:
            receipt["device_write_attempted"] = True
            receipt["phase"] = "restore-before-reinstall"
            _save(receipt_path, receipt)
            progress("Restoring and verifying the previous boot before the fresh install…")
            receipt["restored_boot"] = restore(identity)
            progress("Preserving the previous Linnet folder before the fresh install…")
            receipt["previous_linnet_archive"] = _archive_linnet(identity["root"], archive)
            _save(receipt_path, receipt)
            current = assess(identity)
            current_method = next((item for item in current["installation_methods"]
                                   if item["id"] == method_id), None)
            # restore() verified the installed loader journal before archival;
            # the optional Utility log need not repeat that evidence.
            if (current['action'] != 'install' or not current_method or
                    not current['rockbox']['selector_compatible'] or
                    not reviewed['rockbox']['loader_available']):
                raise RuntimeError("fresh installation state differs after preserving the previous Linnet folder")
        elif strategy == "rockbox-file" and reviewed["action"] == "adopt":
            receipt["device_write_attempted"] = True
            receipt["phase"] = "preserve-superseded-install"
            progress("Preserving the previous Linnet folder before the fresh installation…")
            receipt["previous_linnet_archive"] = _archive_linnet(identity["root"], archive)
            _save(receipt_path, receipt)
            current = assess(identity)
            current_method = next((item for item in current["installation_methods"]
                                   if item["id"] == method_id), None)
            # Archiving removes the installed file-loader journal. Its verified
            # loader evidence remains valid even when Utility's log is absent.
            if (current['action'] != 'install' or not current_method or
                    not current['rockbox']['selector_compatible'] or
                    not reviewed['rockbox']['loader_available']):
                raise RuntimeError("Rockbox installation changed while preserving the previous Linnet folder")
        receipt["device_write_attempted"] = True
        receipt["phase"] = "device-transaction"
        _save(receipt_path, receipt)
        if mixed_native_restore:
            progress("Requesting administrator access to restore the previous boot and reinstall Linnet…")
            marker = reviewed["recovery"]
            job = dict(mode="rockbox-reinstall", identity=identity, artifacts=artifacts,
                       transaction=str(transaction), installation_method=method_id,
                       reviewed_action=reviewed["action"], archive=str(archive),
                       restoration=dict(transaction=marker["transaction"], plan=marker["plan"],
                                        plan_sha256=marker["plan_sha256"], marker=marker),
                       result=str(receipt_path.with_suffix(".result.json")))
            job_path = transaction / ("worker-" + uuid.uuid4().hex + ".json")
            _save(job_path, job, replace=False)
            result = _run_elevated(job_path)
            receipt["restored_boot"] = result.pop("restored_boot")
            receipt["previous_linnet_archive"] = result["previous_linnet_archive"]
        elif strategy == "rockbox-file":
            result = _install_rockbox(identity, artifacts, progress, transaction, archive)
        else:
            progress(("Requesting administrator access to restore and reinstall the iPod boot partition…"
                      if combined_native_reinstall else
                      "Requesting administrator access to back up and update the iPod boot partition…"))
            job = dict(mode="reinstall" if reviewed["action"] in ("replace", "adopt") else "install",
                       identity=identity, artifacts=artifacts,
                       transaction=str(transaction), installation_method=method_id,
                       previous_boot=previous_boot, reviewed_action=reviewed["action"],
                       result=str(receipt_path.with_suffix(".result.json")))
            if reviewed['action'] == 'resume' and reviewed['resume'].get('initialized_resume'):
                job.update(initialized_resume=True, recovery_cache_root=str(transaction.parent.parent))
            if reviewed["action"] == "replace":
                marker = reviewed["recovery"]
                job["restoration"] = (dict(
                    transaction=marker["transaction"], plan=marker["plan"],
                    plan_sha256=marker["plan_sha256"], marker=marker)
                    if combined_native_reinstall else dict(marker=marker))
            if archive is not None:
                job["archive"] = str(archive)
            job_path = transaction / ("worker-" + uuid.uuid4().hex + ".json")
            _save(job_path, job, replace=False)
            result = _run_elevated(job_path)
            if reviewed["action"] == "replace":
                reinstall = result.pop("reinstall")
                receipt["restored_boot"] = reinstall["restored_boot"]
                receipt["previous_linnet_archive"] = reinstall["previous_linnet_archive"]
            elif archive is not None:
                receipt["previous_linnet_archive"] = result["previous_linnet_archive"]
        receipt.update(passed=True, result=result, phase="complete")
    except BaseException as error:
        receipt.update(failure=f"{type(error).__name__}: {error}", phase="failed")
        raise
    finally:
        receipt["finished_at"] = _now()
        receipt["receipt"] = str(receipt_path)
        _save(receipt_path, receipt)
    return receipt


def _read_raw_backup(identity, transaction):
    sector = identity["sector_bytes"]
    if sector not in (512, 2048, 4096):
        raise ValueError("unsupported native installer sector size")
    install_spec = TARGETS[identity["target_id"]]["installer"]
    if not install_spec["enabled"] or install_spec["boot_layout"] != "ipod-directory":
        raise ValueError("the detected target does not enable native directory installation")
    backup = transaction / "backup"
    backup.mkdir(parents=True, exist_ok=True)
    disk_number = identity["disk_number"]
    path = r"\\.\PhysicalDrive" + str(disk_number)
    with open(path, "rb", buffering=0) as device:
        mbr = device.read(sector)
        if len(mbr) != sector or mbr[510:512] != b"\x55\xaa":
            raise ValueError("the iPod MBR is invalid")
        partitions = []
        for index in range(4):
            offset = 446 + index * 16
            kind = mbr[offset + 4]
            lba, sectors = struct.unpack_from("<II", mbr, offset + 8)
            if sectors:
                partitions.append((kind, lba, sectors))
        firmware = [item for item in partitions if item[0] in install_spec["firmware_partition_types"]]
        media = [item for item in partitions if item[0] in install_spec["media_partition_types"]]
        if len(firmware) != 1 or len(media) != 1:
            raise ValueError("expected one firmware partition and one media partition")
        _kind, firmware_lba, firmware_sectors = firmware[0]
        if (media[0][1] * sector, media[0][2] * sector) != (identity["partition_offset"], identity["partition_bytes"]):
            raise ValueError("raw and mounted partition geometry disagree")
        firmware_bytes = firmware_sectors * sector
        if not sector <= firmware_bytes <= min(install_spec["max_firmware_bytes"], identity["disk_bytes"]):
            raise ValueError("firmware partition exceeds the installer bound")
        device.seek(firmware_lba * sector)
        firmware_data = device.read(firmware_bytes)
        if len(firmware_data) != firmware_bytes:
            raise OSError("short firmware backup read")
    for name, data in (("mbr.bin", mbr), ("firmware.bin", firmware_data)):
        target = backup / name
        if target.exists():
            if target.read_bytes() != data:
                raise ValueError("saved original backup differs from the connected iPod")
        else:
            with target.open("xb") as writer:
                writer.write(data)
                writer.flush()
                os.fsync(writer.fileno())
            if target.read_bytes() != data:
                raise OSError("saved firmware backup failed verification")
    return backup / "mbr.bin", backup / "firmware.bin"


def _native_worker(job):
    from tools.inspect.retail_device import detect
    from tools.studio import native_bootstrap as native
    if job["mode"] in ("install", "reinstall"):
        _saved_previous_boot(job)
    transaction = Path(job["transaction"]).resolve()
    expected = job["identity"]
    detected = detect(expected["root"])
    observed = dict(target_id=detected["target_id"], disk_number=detected["storage"]["disk_number"],
                    disk_serial=detected["storage"]["disk_serial"], disk_bytes=detected["storage"]["disk_bytes"],
                    partition_offset=detected["storage"]["partition_offset"],
                    partition_bytes=detected["storage"]["partition_bytes"],
                    linnet_usb_serial=detected["linnet_usb_serial"])
    _same_device(expected, observed)
    current = assess(expected)
    from tools.studio import native_recovery
    if (job['mode'] == 'install' and job.get('reviewed_action') == 'resume' and
            job.get('initialized_resume')):
        return native_recovery.finish(job)
    if job["mode"] != "restore":
        # Recheck admission after UAC, before restoring old startup files or
        # writing any new files. The caller's earlier review is not sufficient.
        install_spec = TARGETS[expected["target_id"]]["installer"]
        if not install_spec["enabled"] or install_spec["boot_layout"] != "ipod-directory":
            raise ValueError("this device does not support firmware-directory installation")
        method_id = job.get("installation_method")
        method = INSTALLATION_METHODS.get(method_id)
        strategy = "rockbox-file" if job["mode"] == "rockbox-reinstall" else "native-directory"
        if (not method or method["strategy"] != strategy or
                job["artifacts"].get("installation_method") != method_id):
            raise ValueError("installer firmware and installation method disagree")
        if job["reviewed_action"] != "resume":
            available = next((item for item in current["installation_methods"]
                              if item["id"] == method_id), None)
            if not available or not available["enabled"]:
                raise RuntimeError("the selected installation method is no longer compatible with this iPod")
        _validate_artifacts(expected, job["artifacts"])
    if job["mode"] == "restore":
        marker = current.get("recovery")
        if (not marker or marker.get("strategy") != "native-directory" or
                marker.get("phase") not in ("installed", "restored") or
                marker.get("plan") != job["plan"] or
                marker.get("plan_sha256") != job["plan_sha256"]):
            raise RuntimeError("native recovery state changed before the worker acquired exclusivity")
    elif job["reviewed_action"] == "resume":
        # UAC may run under another administrator account with a different
        # LocalAppData cache, so assess() cannot discover the caller's context.
        context = json.loads((transaction / "context.json").read_text(encoding="utf-8"))
        saved_method = _saved_method(context)
        saved_artifacts = dict(context.get("artifacts") or {})
        if (current.get("recovery") is not None or
                current["linnet"]["state"] not in ("absent", "incomplete", "installed") or
                context.get("target_id") != expected["target_id"] or
                context.get("device_id") != expected["disk_serial"] or
                context.get("strategy") != "native-directory" or
                saved_method is None or
                saved_method != job["installation_method"] or
                _saved_previous_boot(context) != job["previous_boot"] or
                context.get("build") != job["artifacts"].get("build") or
                saved_artifacts != job["artifacts"]):
            raise RuntimeError("saved native installation changed before resume")
    elif current["action"] != job["reviewed_action"]:
        raise RuntimeError("installation state changed before the worker acquired exclusivity")
    if job["mode"] == "restore":
        path, digest = Path(job['plan']), job['plan_sha256']
        if native_recovery.eligible(expected):
            scope = job['recovery_cache_root']
            if not native_recovery.available(expected, marker, cache_scope=scope):
                raise ValueError('Original recovery plan does not match this native iPod')
            path, digest = native_recovery.bind(expected, transaction, path, digest, marker['device_id'], cache_scope=scope)
        result = native.execute("restore", path, digest, expected["disk_number"])
        if not result.get("passed"):
            raise RuntimeError(result.get("failure", "native restore failed"))
        _save(Path(expected["root"]) / JOURNAL,
              dict(marker, phase="restored", restored_at=_now()))
        return result
    reinstall = None
    if job["mode"] in ("reinstall", "rockbox-reinstall"):
        action = job["reviewed_action"]
        if action not in ("replace", "adopt"):
            raise ValueError("native reinstall action is invalid")
        if (job["mode"] == "rockbox-reinstall" and
                (action != "replace" or
                 job.get("restoration", {}).get("marker", {}).get("strategy") != "native-directory")):
            raise ValueError("mixed bootloader reinstall state is invalid")
        if action == "replace":
            restoration = job["restoration"]
            marker = restoration["marker"]
            if marker != current.get("recovery"):
                raise RuntimeError("saved recovery state changed before reinstall")
            if marker.get("strategy") == "native-directory":
                saved_transaction = Path(restoration["transaction"]).resolve(strict=True)
                plan = Path(restoration["plan"]).resolve(strict=True)
                marker_transaction = Path(marker["transaction"]).resolve(strict=True)
                marker_plan = Path(marker["plan"]).resolve(strict=True)
                if (plan.parent.parent != saved_transaction or
                        marker_transaction != saved_transaction or marker_plan != plan or
                        marker.get("plan_sha256") != restoration["plan_sha256"]):
                    raise ValueError("saved native restore plan does not match its recovery journal")
                if (marker.get("phase") != "installed" or
                        marker.get("target_id") != expected["target_id"] or
                        marker.get("device_id") != expected["disk_serial"]):
                    raise ValueError("saved native recovery journal differs from the connected iPod")
                restored = native.execute("restore", plan, restoration["plan_sha256"], expected["disk_number"])
                if not restored.get("passed"):
                    raise RuntimeError(restored.get("failure", "native restore failed"))
                _save(Path(expected["root"]) / JOURNAL,
                      dict(marker, phase="restored", restored_at=_now()))
            elif marker.get("strategy") == "rockbox-file":
                restored = restore(expected)
            else:
                raise ValueError("unsupported previous boot strategy")
        else:
            restored = None
        archived = _archive_linnet(expected["root"], Path(job["archive"]))
        current = assess(expected)
        current_method = next((item for item in current["installation_methods"]
                               if item["id"] == job["installation_method"]), None)
        if current["action"] != "install" or not current_method or not current_method["enabled"]:
            raise RuntimeError("fresh installation state differs after preserving the previous Linnet folder")
        reinstall = dict(previous_linnet_archive=archived)
        if restored is not None:
            reinstall["restored_boot"] = (restored if marker.get("strategy") == "rockbox-file" else
                                          dict(passed=True, boot=restored, media_preserved=True,
                                               linnet_files_preserved=True, requires_safe_eject=True))
        if job["mode"] == "rockbox-reinstall":
            result = _install_rockbox(expected, job["artifacts"], lambda _message: None,
                                      transaction, Path(job["archive"]))
            result["restored_boot"] = reinstall["restored_boot"]
            return result
    plan_dir = transaction / "native-plan"
    hash_path = transaction / "native-plan.sha256"
    if job["reviewed_action"] == "resume" and plan_dir.exists():
        # A verified boot write can finish before the final full-device read or
        # journal publication fails. Keep the original backup: the live boot
        # partition may already contain the reviewed candidate. execute() below
        # validates every current byte and skips completed writes before resuming.
        plan_sha = hash_path.read_text(encoding="ascii").strip()
        manifest, plan_artifacts, _original = native.load_plan(plan_dir / "plan.json", plan_sha)
        mbr, firmware = transaction / "backup/mbr.bin", transaction / "backup/firmware.bin"
        if (Path(manifest["firmware_backup"]).resolve(strict=True) != firmware.resolve(strict=True) or
                mbr.read_bytes() != plan_artifacts["mbr.bin"]):
            raise ValueError("saved native plan differs from its original backup")
        del _original, plan_artifacts
    else:
        mbr, firmware = _read_raw_backup(expected, transaction)
    device_path = transaction / "device.json"
    device_value = detected
    if device_path.exists():
        if json.loads(device_path.read_text(encoding="utf-8")) != device_value:
            raise ValueError("saved device identity differs")
    else:
        _save(device_path, device_value, replace=False)
    if plan_dir.exists():
        plan_path = plan_dir / "plan.json"
        plan_sha = hash_path.read_text(encoding="ascii").strip()
        native.load_plan(plan_path, plan_sha)
    else:
        planned = native.plan(firmware, mbr, device_path, Path(job["artifacts"]["boot_image"]), plan_dir)
        plan_path, plan_sha = Path(planned["plan"]), planned["plan_sha256"]
        hash_path.write_text(plan_sha + "\n", encoding="ascii")
    storage_receipt = _package_storage(expected, job["artifacts"], transaction, job.get("archive"))
    apple_firmware = _install_apple_firmware(expected, plan_path, plan_sha, job.get("archive"))
    if expected['target_id'] == 'video5g-m25':
        from tools.studio.apple_firmware import publish_video_resources
        firmware_data = firmware.read_bytes()
        parsed = native.parse_backup(firmware_data, mbr.read_bytes(), detected)
        apple_firmware['video_resources'] = publish_video_resources(
            expected, firmware_data, parsed['entries'])
        del firmware_data
    selector = (_install_selector_config(expected)
                if job.get("installation_method") == "native-selector" else None)
    applied = native.execute("apply", plan_path, plan_sha, expected["disk_number"])
    if not applied.get("passed"):
        raise RuntimeError(applied.get("failure", "native boot publication failed"))
    marker = _journal(expected, job["artifacts"], "native-directory", transaction,
                      dict(plan=str(plan_path), plan_sha256=plan_sha,
                            firmware_backup_sha256=_sha(firmware.read_bytes()),
                            installation_method=job["installation_method"],
                            previous_boot=apple_firmware['previous_boot'],
                            apple_firmware=apple_firmware,
                            selector=selector))
    result = dict(passed=True, strategy="native-directory", storage=storage_receipt,
                  boot=applied, apple_firmware=apple_firmware, selector=selector,
                  recovery=marker, requires_safe_eject=True)
    if job.get("archive") is not None:
        final_archive = _finalize_reinstall_archive(expected["root"], job["archive"])
        result["previous_linnet_archive"] = final_archive
        storage_receipt["reinstall_preservation"]["archive"] = final_archive
    if reinstall is not None:
        reinstall["previous_linnet_archive"] = result["previous_linnet_archive"]
        result["reinstall"] = reinstall
    return result


def restore(identity, expected=None):
    from tools.content.publish import _exclusive
    reviewed = assess(identity)
    strategy = (reviewed.get("recovery") or {}).get("strategy")
    if strategy == "native-nor":
        previous = _saved_previous_boot(reviewed['recovery'])
        if expected not in (None, previous):
            raise ValueError('Classic NOR recovery restores the saved '+previous+' bootloader')
        from tools.studio.classic_recovery import restore as restore_classic
        return restore_classic(reviewed["identity"])
    if strategy == "rockbox-file":
        with _exclusive(reviewed["identity"]):
            return _restore(identity, expected, strategy)
    return _restore(identity, expected, strategy)


def _restore(identity, expected=None, reviewed_strategy=None):
    reviewed = assess(identity)
    marker = reviewed.get("recovery")
    if not marker or marker.get("phase") not in ("installed", "restored"):
        raise ValueError("no Linnet installation with a verified recovery journal was found")
    if marker.get("strategy") != reviewed_strategy:
        raise RuntimeError("the boot recovery strategy changed after review")
    if expected is not None and expected not in ("rockbox", "apple"):
        raise ValueError("unsupported default boot selection")
    previous_boot = ("rockbox" if marker.get("strategy") == "rockbox-file" else
                     _saved_previous_boot(marker))
    if expected is not None and expected != previous_boot:
        raise RuntimeError("the saved default boot choice changed after review; check the iPod again")
    if marker["strategy"] == "rockbox-file":
        from tools.studio.bootstrap import restore as restore_file
        result = restore_file(Path(reviewed["identity"]["root"]))
    elif marker["strategy"] == "native-directory":
        transaction = Path(marker["transaction"]).resolve(strict=True)
        result_path = transaction / ("restore-" + uuid.uuid4().hex + ".result.json")
        job = dict(mode="restore", identity=reviewed["identity"],
                   transaction=str(transaction), plan=marker["plan"], plan_sha256=marker["plan_sha256"],
                   result=str(result_path))
        from tools.studio import native_recovery
        if native_recovery.eligible(reviewed['identity']):
            job['recovery_cache_root'] = str(transaction.parent.parent)
        job_path = transaction / ("restore-worker-" + uuid.uuid4().hex + ".json")
        _save(job_path, job, replace=False)
        result = _run_elevated(job_path)
    else:
        raise ValueError("unsupported recovery strategy")
    if marker["strategy"] == "rockbox-file":
        marker = dict(marker, phase="restored", restored_at=_now())
        _save(Path(reviewed["identity"]["root"]) / JOURNAL, marker)
    return dict(passed=True, boot=result, media_preserved=True, linnet_files_preserved=True,
                requires_safe_eject=True)


def _remove_private_tree(path, root, identity):
    """Delete one ordinary directory with a volume and path check per entry."""
    from tools.content.guard import guarded_path, guard_volume

    for child in sorted(path.iterdir(), key=lambda entry: entry.name.upper() == "INSTALL.JSON"):
        guard_volume(identity)
        guarded_path(child, root)
        if child.is_dir():
            _remove_private_tree(child, root, identity)
        else:
            child.unlink()
    guard_volume(identity)
    guarded_path(path, root)
    path.rmdir()


def _verify_uninstall(identity, media_folders, progress, *, timeout=15):
    """Wait for deletion to become visible without replaying any device writes."""
    from tools.content.guard import guarded_path, guard_volume

    root = Path(identity["root"]).resolve(strict=True)
    private = root / ".LINNET"
    deadline = time.monotonic() + timeout
    waiting = False
    while True:
        # An unavailable or replaced volume must never count as deleted files.
        guard_volume(identity)
        try:
            private_removed = guarded_path(private, root) is None
            observed = {name: guarded_path(root / name, root) is not None for name in media_folders}
        except OSError as error:
            detail = "Could not read the iPod folders after cleanup: " + str(error)
        else:
            missing = [name for name, existed in media_folders.items() if existed and not observed[name]]
            guard_volume(identity)
            # Apple software may create a previously absent media folder.
            # Preservation requires keeping existing folders, not their absence.
            if private_removed and not missing:
                return dict(private_removed=True, media_folders=observed)
            detail = ("Linnet's .LINNET directory is still visible after cleanup."
                      if not private_removed else
                      "Could not verify preserved media folders: " + ", ".join(missing))
        if time.monotonic() >= deadline:
            raise RuntimeError(detail)
        if not waiting:
            # Windows can retain a directory marked for deletion until another
            # process closes its handle. Retry only the guarded readback.
            progress("Waiting for Windows to finish updating the iPod folders…")
            waiting = True
        time.sleep(min(.25, max(0, deadline - time.monotonic())))


def _repair_uninstall_recovery(identity, pending):
    """Repopulate only missing recovery files after an interrupted cleanup."""
    from tools.content.guard import guarded_path, guard_volume
    from tools.content.publish import _exclusive
    from tools.content.media_manager import refresh

    receipt, backup = pending
    root = Path(identity["root"]).resolve(strict=True)
    private, recovery = root / ".LINNET", root / ".LINNET" / "RECOVERY"

    def restore_file(source, target):
        data = source.read_bytes()
        temporary = target.with_name("L" + uuid.uuid4().hex[:8] + ".TMP")
        guarded_path(temporary, root)
        with temporary.open("xb") as writer:
            writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        guard_volume(identity)
        if temporary.read_bytes() != data:
            raise OSError("saved recovery copy failed verification on the iPod")
        os.replace(temporary, target)
        if target.read_bytes() != data:
            raise OSError("restored recovery file failed verification")

    with _exclusive(identity):
        refresh(identity)
        guard_volume(identity)
        guarded_path(private, root)
        if not private.is_dir() or _private_stamp(private) != receipt["private_stamp"]:
            raise ValueError("the interrupted Linnet directory changed; removal cannot resume")
        guarded_path(recovery, root)
        recovery.mkdir(exist_ok=True)
        for source in backup.rglob("*"):
            if source.is_file() and re.fullmatch(r"L[0-9a-f]{8}\.TMP", source.name, re.IGNORECASE):
                continue
            guarded_path(source, backup)
            target = recovery / source.relative_to(backup)
            guard_volume(identity)
            guarded_path(target, root)
            if source.is_dir():
                target.mkdir(exist_ok=True)
            elif target.exists():
                if not target.is_file():
                    raise ValueError("on-device recovery differs from the saved uninstall backup")
                if target.read_bytes() != source.read_bytes():
                    if source.name != "INSTALL.JSON":
                        raise ValueError("on-device recovery differs from the saved uninstall backup")
                    try:
                        saved = json.loads(source.read_text(encoding="utf-8"))
                        current = json.loads(target.read_text(encoding="utf-8"))
                        stable = (set(saved) | set(current)) - {"restored_at"}
                        if (saved.get("phase") != "restored" or current.get("phase") != "restored" or
                                any(saved.get(key) != current.get(key) for key in stable)):
                            raise ValueError("the recovery journal changed outside this removal")
                    except (OSError, ValueError, TypeError, AttributeError) as error:
                        raise ValueError("on-device recovery differs from the saved uninstall backup") from error
                    restore_file(source, target)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                restore_file(source, target)


def _uninstall_identity(identity, progress):
    """Reacquire this iPod only before removal has attempted any writes."""
    from tools.content import media_manager as media

    try:
        return media.refresh(identity)
    except (OSError, RuntimeError, ValueError):
        serial = identity.get("linnet_usb_serial", "")
        if not re.fullmatch(r"[0-9A-F]{16}", serial):
            raise
    # Disk numbers, letters and USB storage serials can change across a reboot.
    # Pin the board and original FAT geometry before accepting a new connection;
    # all later write guards continue to use its complete current identity.
    fields = ("target_id", "linnet_usb_serial", "disk_bytes", "sector_bytes",
              "partition_offset", "partition_bytes", "volume_serial", "filesystem")

    def checked(current):
        if any(not identity.get(key) or current.get(key) != identity[key] for key in fields):
            raise ValueError("The reconnected iPod or its volume differs from the reviewed removal.")
        return media.refresh(current)

    progress("Reconnecting the same iPod before removal…")
    mounted = [item for item in media.discover(detailed=True)["devices"]
               if item.get("linnet_usb_serial") == serial]
    if len(mounted) > 1:
        raise ValueError("More than one mounted iPod matches the reviewed removal.")
    if mounted:
        return checked(mounted[0])
    from tools.studio import media_link
    connections = [item for item in media_link.discover() if item["serial"] == serial]
    if len(connections) != 1:
        raise RuntimeError("The reviewed iPod is not connected. Reconnect it in Disk Mode, then choose Find iPod.")
    connection = connections[0]
    if (connection["identity"]["target_id"] != identity["target_id"] or
            not connection.get("can_open_disk")):
        raise ValueError("The connected iPod cannot open Disk Mode for this removal.")
    opened = media_link.open_disk(connection, progress=progress)
    return checked(opened["reconnected_identity"])


def uninstall(identity, progress=lambda _message: None, expected=None, *, instruction=lambda _event: None):
    """Verify the saved previous boot, then remove only Linnet's private directory."""
    from tools.content.guard import guarded_path, guard_volume
    from tools.content.publish import _exclusive

    progress("Checking the iPod and its saved startup recovery record…")
    identity = _uninstall_identity(identity, progress)
    pending = _pending_uninstall(identity, Path(identity["root"]).resolve(strict=True))
    if pending:
        if expected is not None and expected != pending[0]["previous_boot"]:
            raise RuntimeError("the saved startup software changed after review; check the iPod again")
        progress("Recovering the saved startup backup after an interrupted removal…")
        _repair_uninstall_recovery(identity, pending)
    reviewed = assess(identity)
    option = reviewed["uninstall"]
    if not option["available"]:
        raise ValueError(option["detail"])
    previous = "rockbox" if option["previous"] == "Rockbox" else "apple"
    if expected is not None and expected != previous:
        raise RuntimeError("the saved startup software changed after review; check the iPod again")
    root = Path(reviewed["identity"]["root"]).resolve(strict=True)
    private = root / ".LINNET"
    guarded_path(private, root)
    if private.is_symlink() or not private.is_dir() or private.resolve(strict=True).parent != root:
        raise ValueError("the Linnet system directory is unsafe or missing")
    # Refuse a linked subtree before any boot or file change. No media folder
    # outside .LINNET is part of the removal plan.
    def fail_walk(error):
        raise error

    progress("Checking Linnet system files before removal…")
    for base, directories, files in os.walk(private, followlinks=False, onerror=fail_walk):
        for name in directories + files:
            path = Path(base) / name
            guarded_path(path, root)
    if (reviewed["recovery"] or {}).get("strategy") == "native-nor" and reviewed["recovery"]["phase"] != "restored":
        from tools.studio.classic_recovery import restore as restore_classic
        boot = restore_classic(reviewed["identity"], progress=progress, instruction=instruction)
        # Storage reconnection succeeded. A later cleanup failure needs its own
        # diagnosis; it does not mean Apple USB storage needs another reboot.
        result = uninstall(boot["identity"], progress=progress, expected=previous,
                           instruction=instruction)
        result["reconnected_identity"] = boot["identity"]
        return result
    media_folders = {name: guarded_path(root / name, root) is not None
                     for name in ("Music", "Photos", "Videos", "Notes")}
    receipt_path = cache_root(reviewed["identity"]) / ("uninstall-" + uuid.uuid4().hex + ".json")
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    host_directory = receipt_path.parent.resolve(strict=True)
    if (host_directory.stat().st_dev == root.stat().st_dev or
            root.drive and host_directory.drive.casefold() == root.drive.casefold()):
        raise ValueError("The installer recovery location must be on the computer, not on the iPod.")
    receipt = dict(operation="uninstall-linnet", started_at=_now(), passed=False,
                   identity=reviewed["identity"], previous_boot=previous,
                   media_folders=media_folders, private_stamp=_private_stamp(private),
                   phase="restore-boot")
    _save(receipt_path, receipt, replace=False, durable=True)
    try:
        progress(f"Restoring and verifying {option['previous']} as the default startup software…")
        boot = restore(reviewed["identity"], expected=previous)
        receipt.update(boot=boot, phase="remove-linnet-files")
        _save(receipt_path, receipt, durable=True)
        progress("Removing Linnet system files and prepared libraries from the iPod…")
        with _exclusive(reviewed["identity"]):
            from tools.content.media_manager import refresh

            refresh(reviewed["identity"])
            guard_volume(reviewed["identity"])
            guarded_path(private, root)
            if private.is_symlink() or not private.is_dir() or private.resolve(strict=True).parent != root:
                raise ValueError("the Linnet system directory changed after startup restoration")
            trash = private / "TRASH"
            guarded_path(trash, root)
            if trash.exists() and (not trash.is_dir() or any(trash.iterdir())):
                raise ValueError("Trash changed during removal. Restore or empty its media before trying again.")
            recovery = private / "RECOVERY"
            guarded_path(recovery, root)
            if not recovery.is_dir():
                raise ValueError("the Linnet recovery directory disappeared after startup restoration")
            progress("Saving and verifying the startup recovery backup on this computer…")
            recovery_backup = receipt_path.with_suffix(".recovery")
            shutil.copytree(recovery, recovery_backup, copy_function=shutil.copyfile)
            from tools.content.guard import hash_file

            for source in recovery.rglob("*"):
                if source.is_file():
                    guard_volume(reviewed["identity"])
                    guarded_path(source, root)
                    copy = recovery_backup / source.relative_to(recovery)
                    if not copy.is_file() or hash_file(source, source.stat().st_size) != hash_file(copy, copy.stat().st_size):
                        raise OSError("the computer recovery copy differs from the iPod")
                    with copy.open("r+b") as saved:
                        os.fsync(saved.fileno())
            receipt["recovery_backup"] = str(recovery_backup)
            _save(receipt_path, receipt, durable=True)
            try:
                for child in private.iterdir():
                    if child.name.upper() != "RECOVERY":
                        progress("Removing Linnet system files: " + child.name)
                        guard_volume(reviewed["identity"])
                        guarded_path(child, root)
                        if child.is_dir():
                            _remove_private_tree(child, root, reviewed["identity"])
                        else:
                            child.unlink()
                _remove_private_tree(recovery, root, reviewed["identity"])
                guard_volume(reviewed["identity"])
                private.rmdir()
            except (OSError, RuntimeError) as error:
                raise OSError(
                    "Startup software was restored, but Linnet cleanup stopped. "
                    f"Reconnect this iPod and retry removal; recovery files are saved at {recovery_backup}."
                ) from error
            progress("Verifying removal and preserved media folders…")
            receipt["verification"] = _verify_uninstall(reviewed["identity"], media_folders, progress)
        receipt.update(passed=True, phase="complete", media_preserved=True,
                       reconnected_identity=reviewed["identity"],
                       requires_safe_eject=True,
                       summary=f"Linnet removed. {option['previous']} is the default startup software. Your Music, Photos, Videos, and Notes folders remain on the iPod.")
    except BaseException as error:
        receipt.update(phase="failed", failure=f"{type(error).__name__}: {error}")
        raise
    finally:
        receipt.update(finished_at=_now(), receipt=str(receipt_path))
        _save(receipt_path, receipt, durable=True)
    return receipt


def worker(job_path):
    job_path = Path(job_path).resolve(strict=True)
    job = json.loads(job_path.read_text(encoding="utf-8"))
    result_path = Path(job["result"]).resolve()
    response = dict(passed=False, operation=job.get("mode"), started_at=_now())
    try:
        if job.get("mode") not in ("install", "restore", "reinstall", "rockbox-reinstall", "upgrade-boot"):
            raise ValueError("unsupported installer worker job")
        from tools.content.publish import _exclusive
        with _exclusive(job["identity"]):
            if job["mode"] == "upgrade-boot":
                from tools.studio.boot_upgrade import worker as upgrade_worker
                response.update(upgrade_worker(job))
            else:
                response.update(_native_worker(job))
    except BaseException as error:
        response["failure"] = f"{type(error).__name__}: {error}"
    response["finished_at"] = _now()
    _save(result_path, response)
    return response


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--worker", type=Path)
    cli.add_argument("--worker-token")
    commands = cli.add_subparsers(dest="command")
    for name in ("inspect", "install", "restore", "uninstall", "upgrade-boot"):
        command = commands.add_parser(name)
        command.add_argument("--mount", type=Path, required=True)
        if name in ("install", "upgrade-boot"):
            command.add_argument("--build", choices=("user", "dev"), help="Source build type; ZIPs identify their build automatically")
            if name == "install":
                command.add_argument("--method", choices=tuple(INSTALLATION_METHODS),
                                     help="default: native for fresh installs, saved method for reinstall/resume")
            source = command.add_mutually_exclusive_group()
            source.add_argument("--firmware", type=Path, help="prebuilt firmware ZIP")
            source.add_argument("--from-source", action="store_true", help="compile fresh firmware locally")
    args = cli.parse_args()
    if args.worker or args.worker_token:
        worker_path = args.worker or Path(base64.b64decode(args.worker_token).decode("utf-8"))
        result = worker(worker_path)
    else:
        if not args.command:
            cli.error("choose inspect, install, restore, uninstall, or upgrade-boot")
        identity = identity_for_mount(args.mount)
        if args.command == "inspect":
            result = assess(identity)
        elif args.command == "install":
            result = install(identity, progress=print, build=args.build,
                             installation_method=args.method, firmware=args.firmware,
                             from_source=args.from_source)
        elif args.command == "upgrade-boot":
            from tools.studio.boot_upgrade import upgrade
            result = upgrade(identity, progress=print, build=args.build,
                             firmware=args.firmware, from_source=args.from_source)
        elif args.command == "uninstall":
            result = uninstall(identity)
        else:
            result = restore(identity)
    print(json.dumps(result, indent=2))
    return 0 if result.get("passed", True) else 2


if __name__ == "__main__":
    raise SystemExit(main())
