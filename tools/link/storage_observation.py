"""Read-only host evidence for the Apple disk-mode transition."""
import json
import os
from pathlib import Path
import subprocess
import time

def disks():
    if os.name == 'nt':
        script = "Get-CimInstance Win32_DiskDrive | Select-Object DeviceID,Model,SerialNumber,PNPDeviceID | ConvertTo-Json -Compress"
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, timeout=5, check=True)
        value = json.loads(result.stdout.strip() or '[]')
        if value is None:
            return []
        return value if isinstance(value, list) else [value]
    result = []
    for device in Path('/sys/class/block').glob('*'):
        serial = device/'device/serial'
        if serial.is_file():
            result.append(dict(DeviceID='/dev/'+device.name, SerialNumber=serial.read_text().strip(), PNPDeviceID=''))
    return result

def wait_for_disk(serial_number, timeout):
    if not serial_number:
        raise ValueError('disk observation requires known hardware serial')
    deadline = time.monotonic()+timeout
    while time.monotonic()<deadline:
        matches = [disk for disk in disks() if serial_number.casefold() in
                   ((disk.get('SerialNumber') or '')+' '+(disk.get('PNPDeviceID') or '')).casefold()]
        if len(matches)==1:
            return matches[0]
        if len(matches)>1:
            raise RuntimeError('multiple disks match the device serial')
        time.sleep(.5)
    raise TimeoutError('requested device did not appear as host mass storage')
