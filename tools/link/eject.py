"""Guarded Windows eject for an explicitly identified iPod disk."""
import base64
import json
import os
import subprocess
import sys
import uuid
import hashlib
import time
import re
from pathlib import Path


def connection_state(identity):
    """Distinguish a proven Windows eject from a vanished prior connection."""
    if os.name != 'nt' or not identity.get('usb_pnp_id'):
        return None
    config = base64.b64encode(json.dumps(dict(serial=identity['disk_serial'],
        usb=identity['usb_pnp_id'], volume=identity['volume_unique_id'])).encode()).decode()
    script = r'''
$ErrorActionPreference = 'Stop'
$cfg = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('CONFIG')) | ConvertFrom-Json
$nodes = @(Get-PnpDevice -PresentOnly -InstanceId $cfg.usb -ErrorAction SilentlyContinue)
$state = ''
$disks = @(Get-Disk | Where-Object { $_.SerialNumber -and $_.SerialNumber.Trim() -ceq $cfg.serial })
$volumes = @(Get-Volume | Where-Object { $_.UniqueId -ceq $cfg.volume -and $_.DriveLetter })
if ($nodes.Count -eq 1 -and $nodes[0].InstanceId -ceq $cfg.usb) {
    $problem = Get-PnpDeviceProperty -InstanceId $nodes[0].InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
    if ($problem.Data -eq 47 -and $disks.Count -eq 0 -and $volumes.Count -eq 0) { $state = 'ejected' }
    # Windows can retain CM_PROB_WILL_BE_REMOVED after the storage and volume
    # have gone. Treat it as a closed connection without claiming a flush.
    elseif ($problem.Data -eq 21 -and $disks.Count -eq 0 -and $volumes.Count -eq 0) { $state = 'disconnected' }
} elseif ($nodes.Count -eq 0 -and $disks.Count -eq 0 -and $volumes.Count -eq 0) {
    $state = 'disconnected'
}
@{ state=$state } | ConvertTo-Json -Compress
'''.replace('CONFIG', config)
    result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
        text=True, capture_output=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        return None
    try:
        state = json.loads(result.stdout).get('state')
        return state if state in ('ejected', 'disconnected') else None
    except (ValueError, AttributeError):
        return None


def already_ejected(identity):
    return connection_state(identity) == 'ejected'


def _token_elevated() -> bool:
    """Inspect this process token; membership in Administrators alone is insufficient."""
    import ctypes
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel.GetCurrentProcess.argtypes = []
    kernel.GetCurrentProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    security.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
    security.OpenProcessToken.restype = w.BOOL
    security.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                            w.DWORD, ctypes.POINTER(w.DWORD)]
    security.GetTokenInformation.restype = w.BOOL
    token = w.HANDLE()
    if not security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
        raise RuntimeError(f"Cannot inspect process token (error {ctypes.get_last_error()})")
    try:
        elevated, returned = w.DWORD(), w.DWORD()
        if not security.GetTokenInformation(token, 20, ctypes.byref(elevated),
                                            ctypes.sizeof(elevated), ctypes.byref(returned)):
            raise RuntimeError(f"Cannot read token elevation (error {ctypes.get_last_error()})")
        if returned.value != ctypes.sizeof(elevated):
            raise RuntimeError("Unexpected token elevation result size")
        return bool(elevated.value)
    finally:
        kernel.CloseHandle(token)


def _elevated_refresh(executable: str, arguments: str, directory: str, *, allow_prompt: bool = True) -> None:
    """Reuse elevated token directly; otherwise optionally request normal RunAs consent."""
    if _token_elevated():
        child = subprocess.Popen(subprocess.list2cmdline([executable]) + " " + arguments,
                                 cwd=directory, creationflags=subprocess.CREATE_NO_WINDOW,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            code = child.wait(timeout=55)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("Elevated refresh exceeded 55 seconds; process was not terminated; inspect receipt") from exc
        if code:
            raise RuntimeError(f"Elevated directory refresh failed (exit {code}); inspect receipt")
        return
    if not allow_prompt:
        raise RuntimeError("Directory refresh requires an elevated process; unattended mode will not request UAC consent")
    import ctypes
    from ctypes import wintypes as w

    class ShellExecuteInfo(ctypes.Structure):
        _fields_ = [("cbSize", w.DWORD), ("fMask", w.ULONG), ("hwnd", w.HWND),
                    ("lpVerb", w.LPCWSTR), ("lpFile", w.LPCWSTR), ("lpParameters", w.LPCWSTR),
                    ("lpDirectory", w.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", w.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", w.LPCWSTR), ("hkeyClass", w.HKEY),
                    ("dwHotKey", w.DWORD), ("hIcon", w.HANDLE), ("hProcess", w.HANDLE)]

    shell = ctypes.WinDLL("shell32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    ole = ctypes.WinDLL("ole32", use_last_error=True)
    shell.ShellExecuteExW.argtypes = [ctypes.POINTER(ShellExecuteInfo)]
    shell.ShellExecuteExW.restype = w.BOOL
    kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    kernel.WaitForSingleObject.restype = w.DWORD
    kernel.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
    kernel.GetExitCodeProcess.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    ole.CoInitializeEx.argtypes = [ctypes.c_void_p, w.DWORD]
    ole.CoInitializeEx.restype = ctypes.c_long
    ole.CoUninitialize.argtypes = []
    ole.CoUninitialize.restype = None
    initialized = ole.CoInitializeEx(None, 2)  # apartment threaded Shell invocation
    if initialized < 0 and initialized != -2147417850:  # existing different apartment is usable
        raise RuntimeError(f"COM initialization failed: 0x{initialized & 0xffffffff:08x}")
    info = ShellExecuteInfo()
    info.cbSize = ctypes.sizeof(info)
    # NOCLOSEPROCESS | NOASYNC | FLAG_NO_UI (security prompts remain enabled).
    info.fMask = 0x40 | 0x100 | 0x400
    info.lpVerb, info.lpFile, info.lpParameters = "runas", executable, arguments
    info.lpDirectory, info.nShow = directory, 0  # SW_HIDE
    try:
        if not shell.ShellExecuteExW(ctypes.byref(info)):
            raise RuntimeError(f"Windows elevation failed (error {ctypes.get_last_error()})")
        if not info.hProcess:
            raise RuntimeError("Windows elevation returned no process handle")
        outcome = kernel.WaitForSingleObject(info.hProcess, 55000)
        if outcome == 258:
            raise RuntimeError("Elevated refresh exceeded 55 seconds; process was not terminated; inspect receipt")
        if outcome != 0:
            raise RuntimeError(f"Waiting for elevated refresh failed (error {ctypes.get_last_error()})")
        code = w.DWORD()
        if not kernel.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
            raise RuntimeError(f"Reading elevated exit status failed (error {ctypes.get_last_error()})")
        if code.value:
            raise RuntimeError(f"Elevated directory refresh failed (exit {code.value}); inspect receipt")
    finally:
        if info.hProcess:
            kernel.CloseHandle(info.hProcess)
        if initialized >= 0:
            ole.CoUninitialize()


def eject_reboot(disk_number: int, plan_path: Path, plan_sha256: str,
                 expected_os: dict | None = None, *, allow_prompt: bool = True) -> dict:
    """Refresh/eject once; verify reviewed Boot or an explicitly expected OS."""
    from tools.studio.native_bootstrap import load_plan
    if os.name != "nt" or disk_number < 0:
        raise ValueError("eject --reboot requires Windows and an explicit valid disk number")
    plan_path = Path(plan_path).resolve()
    manifest, artifacts, _ = load_plan(plan_path, plan_sha256)
    # Fail before consent or any raw write if storage is absent, already
    # ejected, or a different volume. Never substitute plain eject on failure.
    from tools.content.publish import inspect_target
    storage = json.loads(artifacts['device.json'])['storage']
    # Resolve the reviewed volume on the selected disk, even if Windows assigned
    # a new drive letter. A different volume must not authorize raw refresh.
    config = base64.b64encode(json.dumps(dict(number=disk_number,
        volume=storage['volume_id'])).encode()).decode()
    script = r'''
$ErrorActionPreference='Stop'
$cfg=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('CONFIG')) | ConvertFrom-Json
$parts=@(Get-Partition -DiskNumber $cfg.number | Where-Object {$_.DriveLetter})
if ($parts.Count -ne 1) {throw 'Require exactly one mounted partition'}
$volume=Get-Volume -Partition $parts[0]
if ($volume.UniqueId -cne $cfg.volume) {throw 'Reviewed volume identity changed'}
$drive=[string]$parts[0].DriveLetter+':'
$logical=Get-CimInstance Win32_LogicalDisk -Filter ("DeviceID='"+$drive+"'")
@{drive=$drive;serial=$logical.VolumeSerialNumber} | ConvertTo-Json -Compress
'''.replace('CONFIG', config)
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand',
        base64.b64encode(script.encode('utf-16-le')).decode()], capture_output=True, text=True,
        timeout=30, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    volume = json.loads(result.stdout.lstrip('\ufeff'))
    identity = inspect_target(disk_number, manifest['expected_storage_serial'], volume['serial'], volume['drive'])
    if (identity['volume_unique_id'] != storage['volume_id'] or
            identity['disk_bytes'] != manifest['expected_device_bytes'] or
            identity['partition_offset'] != storage['partition_offset'] or
            identity['partition_bytes'] != storage['partition_bytes']):
        raise RuntimeError('mounted storage no longer matches reviewed plan')
    if expected_os is None:
        from tools.studio.installer import installed_state
        installed = installed_state(Path(volume['drive'] + '/'), manifest['image']['target_id'])
        if installed['state'] == 'installed':
            if installed['candidate_pending']:
                raise ValueError('a pending OS candidate requires an explicit expected OS identity')
            if installed['profile'] in ('dev', 'trace'):
                expected_os = next(image for image in installed['images']
                                   if image['build_id'] == installed['build_id'] and
                                   image['sha256'] == installed['image_sha256'])
    if expected_os is not None:
        if (not isinstance(expected_os, dict) or expected_os.get('target_id') != manifest['image']['target_id'] or
            expected_os.get('profile') not in ('dev', 'trace') or
            any(not isinstance(expected_os.get(key), str) or not re.fullmatch('[0-9a-f]{64}', expected_os[key])
                for key in ('build_id', 'sha256'))):
            raise ValueError('expected OS must be a validated development LOS identity')
        expected_os = dict(expected_os)
    root = Path(__file__).resolve().parents[2]
    receipt_path = root / "out" / ("disk-restart-" + uuid.uuid4().hex + ".json")
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    arguments = subprocess.list2cmdline(["-m", "tools.link.disk_restart", "--plan", str(plan_path),
        "--plan-sha256", plan_sha256, "--disk-number", str(disk_number), "--receipt", str(receipt_path)])
    try:
        from tools.link.refresh_session import refresh as session_refresh
        if not session_refresh(plan_path, plan_sha256, disk_number, receipt_path):
            _elevated_refresh(sys.executable, arguments, str(root), allow_prompt=allow_prompt)
        receipt = json.loads(receipt_path.read_text())
        if receipt.get('passed') is not True and receipt.get('failure'):
            writes = receipt.get('writes', [])
            write_state = ('no device write attempted' if receipt.get('device_write_attempted') is False
                           else f"{sum(item.get('complete') is True for item in writes)}/{len(writes)} writes completed readback; overall verification failed")
            raise RuntimeError(f"refresh stopped in {receipt.get('phase')}: {receipt['failure']}; {write_state}")
        device = receipt.get("device", {})
        sector_bytes = manifest.get("sector_bytes", 512)
        expected_write = dict(bytes=sector_bytes, complete=True, offset=manifest["directory_disk_offset"],
                              returned_bytes=sector_bytes, sha256=hashlib.sha256(artifacts["directory-candidate.bin"]).hexdigest())
        if (receipt.get("operation") != "refresh-installed-directory" or receipt.get("passed") is not True or
            receipt.get("phase") != "complete" or receipt.get("bytes_changed") != 0 or
            receipt.get("device_write_attempted") is not True or receipt.get("writes") != [expected_write] or
            receipt.get("plan") != str(plan_path) or receipt.get("plan_sha256") != plan_sha256 or
            device.get("Number") != disk_number or device.get("SerialNumber", "").strip() != manifest["expected_storage_serial"] or
            device.get("Size") != manifest["expected_device_bytes"] or device.get("LogicalSectorSize") != sector_bytes or
            device.get("BusType") != "USB" or device.get("FriendlyName") != "Apple iPod" or
            not receipt.get("before") or receipt.get("before") != receipt.get("after") or
            receipt["before"].get("directory") != "candidate" or
            receipt["before"].get("staging") != "candidate" or
            receipt["before"].get("original_payload_intact") is not True or
            not receipt.get("finished_at")):
            raise RuntimeError("refresh receipt failed strict identity/completion validation")
        report = eject(disk_number, manifest["expected_storage_serial"], expected_os is None,
                       manifest["expected_linnet_usb_serial"])
        if expected_os is None:
            identity = report["identity"]
            if identity["image_kind"] != "boot" or identity["build_id"] != manifest["image"]["build_id"]:
                raise RuntimeError("ejection completed but returning Boot does not match the reviewed plan")
        else:
            from .campaign import connect, _check_healthy_identity
            from .identity import decode
            # Persistent Boot validates the filesystem, image and durable
            # attempt before the OS performs its own mount/healthy publication.
            # PIO storage can legitimately exceed the former one-minute wait.
            deadline = time.monotonic() + 180
            os_generation = None
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('expected OS did not return after eject')
                session, device, identity = connect(serial_number=manifest['expected_linnet_usb_serial'],
                                                    deadline_seconds=remaining)
                try:
                    if identity['image_kind'] == 'os':
                        # Validate image independently of its in-progress health publication.
                        if (identity.get('build_id') != expected_os['build_id'] or
                                identity.get('image_sha256') != expected_os['sha256'] or
                                any(identity.get(key) != expected_os[key]
                                    for key in ('target_id', 'profile'))):
                            raise ValueError('post-eject OS identity or hash mismatch')
                        if os_generation is None:
                            os_generation = identity['generation']
                        if identity['generation'] != os_generation:
                            raise RuntimeError('OS generation changed during post-eject verification')
                        status = decode(session.status())
                        if (status.get('image_kind') != 'os' or
                                status.get('build_id') != expected_os['build_id'] or
                                status.get('image_sha256') != expected_os['sha256'] or
                                any(status.get(key) != expected_os[key]
                                    for key in ('target_id', 'profile'))):
                            raise ValueError('post-eject OS identity or hash mismatch')
                        if status['generation'] != os_generation:
                            raise RuntimeError('OS generation changed during post-eject verification')
                        if status.get('healthy') is True:
                            _check_healthy_identity(status, expected_os)
                            report.update(identity=status, link_serial=device['serial_number'], link_reconnected=True)
                            break
                    elif os_generation is not None or identity['image_kind'] != 'boot' or identity['build_id'] != manifest['image']['build_id']:
                        raise RuntimeError('unexpected image returned after eject')
                finally:
                    session.close()
                # A verified Boot may be transient while persistent autoboot runs.
                time.sleep(min(.2, max(0, deadline-time.monotonic())))
        report.update(reboot_verified=True, refresh_receipt=str(receipt_path), plan=str(plan_path),
                      plan_sha256=plan_sha256, bytes_changed=0)
        return report
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        raise RuntimeError(f"Eject/reboot failed: {exc}. Retained refresh receipt: {receipt_path}; do not replay automatically") from exc


def eject(disk_number: int, expected_serial: str,
          wait_link: bool = False, link_serial: str | None = None, *, recovery: bool = False) -> dict:
    """Lock and flush the identified volume, then eject or enter Classic recovery."""
    if os.name != "nt":
        raise RuntimeError("device eject requires Windows")
    if (disk_number < 0 or not isinstance(expected_serial, str) or not expected_serial.strip() or
            (wait_link and (not isinstance(link_serial, str) or not link_serial.strip()))):
        raise ValueError("explicit disk number and nonempty expected serials are required")
    config = base64.b64encode(json.dumps(dict(number=disk_number,
        serial=expected_serial.strip(), recovery=recovery)).encode()).decode()
    script = r'''
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
$cfg = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('CONFIG')) | ConvertFrom-Json
$disk = Get-Disk -Number $cfg.number -ErrorAction Stop
if ($disk.SerialNumber.Trim() -cne $cfg.serial -or [string]$disk.BusType -ne 'USB' -or
    $disk.FriendlyName -notmatch 'iPod' -or $disk.IsBoot -or $disk.IsSystem) {
    throw 'Disk identity rejected: require expected serial, USB iPod, and non-system disk'
}
$parts = @(Get-Partition -DiskNumber $disk.Number | Where-Object { $_.DriveLetter })
if ($parts.Count -ne 1) { throw 'Expected exactly one mounted drive letter on the selected disk' }
$originalPartition = $parts[0]
$originalVolume = Get-Volume -Partition $originalPartition
if ([string]$originalVolume.FileSystemType -eq 'Unknown') { throw 'Selected volume media is already unavailable' }
$drive = [string]$parts[0].DriveLetter + ':'
$method = if ($cfg.recovery) { 'locked-volume-recovery' } else { 'locked-volume' }
# Recheck the identity immediately before the single eject request.
$check = Get-Disk -Number $cfg.number -ErrorAction Stop
if ($check.UniqueId -cne $disk.UniqueId -or $check.SerialNumber.Trim() -cne $cfg.serial) {
    throw 'Disk changed during eject preparation'
}
$checkPartition = Get-Partition -DiskNumber $check.Number -PartitionNumber $originalPartition.PartitionNumber
if ($checkPartition.Offset -ne $originalPartition.Offset -or $checkPartition.Size -ne $originalPartition.Size -or
    $checkPartition.DriveLetter -ne $originalPartition.DriveLetter) { throw 'Partition changed during eject preparation' }
# Shell InvokeVerb can silently do nothing in Apple or Linnet Disk Mode.
# Flush and lock the verified volume before issuing a checked media eject.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class LinnetVolumeEject {
    // ntddscsi.h SCSI_PASS_THROUGH; ULONG_PTR follows the process bitness.
    [StructLayout(LayoutKind.Sequential)]
    struct ScsiPassThrough {
        public ushort Length;
        public byte ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength, DataIn;
        public uint DataTransferLength, TimeOutValue;
        public UIntPtr DataBufferOffset;
        public uint SenseInfoOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst=16)] public byte[] Cdb;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        byte[] input, uint inputBytes, byte[] output, uint outputBytes,
        out uint returned, IntPtr overlapped);
    static uint Control(SafeFileHandle handle, uint code, byte[] input, byte[] output, string stage) {
        uint returned;
        if (!DeviceIoControl(handle, code, input, input == null ? 0u : (uint)input.Length,
                output, output == null ? 0u : (uint)output.Length, out returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), stage);
        return returned;
    }
    static void Verify(SafeFileHandle handle, uint disk, uint partition, string serial) {
        var number = new byte[12];
        if (Control(handle, 0x2d1080, null, number, "Cannot identify iPod volume") != 12 ||
                BitConverter.ToUInt32(number, 0) != 7 ||
                BitConverter.ToUInt32(number, 4) != disk ||
                BitConverter.ToUInt32(number, 8) != partition)
            throw new InvalidOperationException("iPod volume identity changed");
        var descriptor = new byte[4096];
        uint bytes = Control(handle, 0x2d1400, new byte[12], descriptor, "Cannot identify iPod disk");
        if (bytes < 36) throw new InvalidOperationException("Incomplete iPod disk identity");
        uint offset = BitConverter.ToUInt32(descriptor, 24);
        if (offset < 36 || offset >= bytes) throw new InvalidOperationException("iPod serial unavailable");
        int end = Array.IndexOf(descriptor, (byte)0, (int)offset, (int)(bytes-offset));
        if (end < 0 || Encoding.ASCII.GetString(descriptor, (int)offset, end-(int)offset).Trim() != serial)
            throw new InvalidOperationException("iPod disk serial changed");
    }
    static void RecoveryEject(SafeFileHandle handle) {
        int header = Marshal.SizeOf(typeof(ScsiPassThrough));
        var request = new ScsiPassThrough {
            Length=(ushort)header, CdbLength=16, SenseInfoLength=32, DataIn=2,
            TimeOutValue=20, SenseInfoOffset=(uint)header, Cdb=new byte[16]
        };
        request.Cdb[0]=0xc0;
        Encoding.ASCII.GetBytes("LINNET-RECOVERY").CopyTo(request.Cdb, 1);
        var packet = new byte[header+32];
        IntPtr memory = Marshal.AllocHGlobal(header);
        try {
            Marshal.StructureToPtr(request, memory, false);
            Marshal.Copy(memory, packet, 0, header);
        } finally { Marshal.FreeHGlobal(memory); }
        uint bytes = Control(handle, 0x4d004, packet, packet, "Classic recovery eject was not confirmed; reconnect and refresh before retrying");
        if (bytes < header) throw new InvalidOperationException("Incomplete Classic recovery eject result; reconnect and refresh");
        if (packet[2] != 0) {
            bool unsupported = bytes >= header+14 && packet[7] >= 14 &&
                (packet[header+2]&15) == 5 && (packet[header+12] == 0x20 || packet[header+12] == 0x24);
            throw new InvalidOperationException(unsupported ?
                "This firmware rejected Classic recovery eject. Update to the latest Classic User ZIP, eject safely, start the updated Linnet OS, then choose Settings > USB Disk Mode and reconnect before removing Linnet." :
                "Classic recovery eject failed. Reconnect in Linnet Disk Mode and refresh before retrying; no boot flash was written.");
        }
    }
    public static void Eject(string volume, uint disk, uint partition, string serial, bool recovery) {
        using (var handle = CreateFileW(volume.TrimEnd('\\'), 0xc0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open iPod volume");
            Verify(handle, disk, partition, serial);
            // A failed lock leaves other applications and the filesystem alone.
            // A successful lock flushes cached writes and proves no files are open.
            Control(handle, 0x90018, null, null, "iPod is busy or could not be flushed; close apps using it and try again");
            Verify(handle, disk, partition, serial);
            Control(handle, 0x90020, null, null, "Could not dismount iPod");
            if (recovery) RecoveryEject(handle);
            else Control(handle, 0x2d4808, null, null, "Windows could not eject iPod");
        }
    }
}
'@
[LinnetVolumeEject]::Eject($originalVolume.UniqueId, $disk.Number,
    $originalPartition.PartitionNumber, $cfg.serial, $cfg.recovery)
$clock = [Diagnostics.Stopwatch]::StartNew()
$confirmationState = 'Storage is still mounted'
do {
    Start-Sleep -Milliseconds 200
    $remaining = @(Get-Disk | Where-Object {
        $_.SerialNumber -and $_.SerialNumber.Trim() -ceq $cfg.serial
    })
    if ($remaining.Count -eq 0) {
        @{ ejected=$true; method=$method; disk_number=$cfg.number; disk_serial=$cfg.serial;
           drive=$drive; storage_disappeared=$true } | ConvertTo-Json -Compress
        exit 0
    }
    if ($remaining.Count -ne 1 -or $remaining[0].UniqueId -cne $disk.UniqueId) {
        throw 'Disk identity became ambiguous while confirming eject'
    }
    $diskOperational = @($remaining[0].OperationalStatus | ForEach-Object { [string]$_ })
    if ($diskOperational -contains 'No Media') {
        @{ ejected=$true; method=$method; disk_number=$cfg.number; disk_serial=$cfg.serial;
           drive=$drive; storage_disappeared=$false; media_unavailable=$true;
           operational_status=$diskOperational } | ConvertTo-Json -Compress
        exit 0
    }
    # Windows removes the volume and partition in separate enumeration steps.
    # Keep observing the same disk until removal is positively confirmed.
    $currentPartitions = @(Get-Partition -DiskNumber $remaining[0].Number -PartitionNumber $originalPartition.PartitionNumber -ErrorAction SilentlyContinue)
    if ($currentPartitions.Count -ne 1) {
        $confirmationState = 'Waiting for disk removal after partition disappeared'
        continue
    }
    $currentPartition = $currentPartitions[0]
    if ($currentPartition.Offset -ne $originalPartition.Offset -or $currentPartition.Size -ne $originalPartition.Size -or
        $currentPartition.DriveLetter -ne $originalPartition.DriveLetter) {
        $confirmationState = 'Partition changed; waiting for confirmed storage removal'
        continue
    }
    $volumes = @(Get-Volume -Partition $currentPartition -ErrorAction SilentlyContinue)
    if ($volumes.Count -ne 1) {
        $confirmationState = 'Waiting for disk removal after volume disappeared'
        continue
    }
    $volume = $volumes[0]
    $operational = @($volume.OperationalStatus | ForEach-Object { [string]$_ })
    if ($operational -contains 'Offline' -or
        ([string]$volume.FileSystemType -eq 'Unknown' -and $operational -contains 'Unknown')) {
        @{ ejected=$true; method=$method; disk_number=$cfg.number; disk_serial=$cfg.serial;
           drive=$drive; storage_disappeared=$false; media_unavailable=$true;
           filesystem=[string]$volume.FileSystemType; operational_status=$operational } | ConvertTo-Json -Compress
        exit 0
    }
    if ($volume.UniqueId -cne $originalVolume.UniqueId) {
        $confirmationState = 'Volume identity changed; waiting for confirmed storage removal'
        continue
    }
} while ($clock.Elapsed.TotalSeconds -lt 20)
throw ('Windows did not confirm eject. ' + $confirmationState + '. Recheck the connection before trying again.')
} catch {
    $reason = $_.Exception
    while ($reason.InnerException) { $reason = $reason.InnerException }
    $detail = $reason.Message
    if ($reason -is [ComponentModel.Win32Exception]) { $detail += ' (Windows error ' + $reason.NativeErrorCode + ')' }
    @{ error=$detail } | ConvertTo-Json -Compress
    exit 1
}
'''.replace("CONFIG", config)
    encoded = base64.b64encode(script.encode("utf-16-le")).decode()
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                             "-EncodedCommand", encoded], capture_output=True,
                            text=True, timeout=90,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        try:
            detail = json.loads(result.stdout)["error"]
        except (ValueError, KeyError):
            detail = "PowerShell failed without a structured result"
        raise RuntimeError("Windows eject failed: " + detail)
    report = json.loads(result.stdout)
    report["link_reconnected"] = False
    if wait_link:
        from .campaign import connect
        try:
            # Enumeration can precede command readiness. Retry only discovery,
            # opening and read-only HELLO on fresh sessions; the single eject
            # request above is outside this loop and is never replayed.
            session, device, identity = connect(serial_number=link_serial,
                                                deadline_seconds=60)
        except (OSError, RuntimeError) as exc:
            raise RuntimeError("Storage ejection confirmed, but Linnet Link did not reconnect within 60 seconds; rediscover the device before another action") from exc
        try:
            report["identity"] = identity
            report["link_serial"] = device["serial_number"]
            report["link_reconnected"] = True
        finally:
            session.close()
    return report
