"""Refresh an exact installed native boot directory before Apple Disk Mode eject.

The operation writes the same verified directory sector. It does not install an image.
Automatic restart is a hardware observation, never implied by successful refresh.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
from tools.studio.native_bootstrap import load_plan, identify_disk, guard_device, write_verified, save_json


def refresh(plan_path, plan_sha256, disk_number, receipt_path):
    receipt_path = Path(receipt_path).resolve()
    receipt = dict(operation="refresh-installed-directory", passed=False,
                   device_write_attempted=False, writes=[], phase="validate-plan",
                   started_at=datetime.now(timezone.utc).isoformat(),
                   plan=str(Path(plan_path).resolve()), plan_sha256=plan_sha256)
    try:
        save_json(receipt_path, receipt)
        manifest, artifacts, original = load_plan(Path(plan_path).resolve(), plan_sha256)
        receipt["device"] = identify_disk(disk_number, manifest)
        with open(r"\\.\PhysicalDrive" + str(disk_number), "r+b", buffering=0) as device:
            receipt["phase"] = "verify-before-write"
            receipt["before"] = guard_device(device, manifest, artifacts, original)
            if receipt["before"]["directory"] != "candidate":
                raise RuntimeError("refresh requires the exact candidate already installed")
            write_verified(device, manifest["directory_disk_offset"], artifacts["directory-candidate.bin"],
                           receipt, receipt_path, "refresh-identical-directory", manifest.get("sector_bytes", 512))
            receipt["phase"] = "verify-after-write"
            receipt["after"] = guard_device(device, manifest, artifacts, original)
            if receipt["after"] != receipt["before"]:
                raise RuntimeError("directory refresh changed firmware state")
        receipt.update(passed=True, phase="complete", bytes_changed=0)
    except Exception as exc:
        receipt.update(failure=f"{type(exc).__name__}: {exc}")
    receipt["finished_at"] = datetime.now(timezone.utc).isoformat()
    save_json(receipt_path, receipt)
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--disk-number", type=int, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    result = refresh(args.plan, args.plan_sha256, args.disk_number, args.receipt)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
