"""Read-only Windows evidence for an installation disk-access failure."""
import base64
import json
import os
import subprocess


def recent_bad_block(identity, since):
    """Correlate disk events with the mounted device and this installation attempt.

    A generic errno is not evidence of damaged media. If Windows cannot supply
    matching evidence, leave the cause unknown. Never probe or repair the disk.
    """
    if (os.name != 'nt' or not identity or since is None or
            not isinstance(identity.get('disk_number'), int) or
            not identity.get('pnp_id')):
        return None
    config = base64.b64encode(json.dumps(dict(number=identity['disk_number'],
        pnp=identity['pnp_id'], since=int(since * 1000))).encode()).decode()
    script = r'''
$ErrorActionPreference='Stop'
$cfg=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('CONFIG')) | ConvertFrom-Json
# A disk number can be reused after removal. Reject a different current device.
$disk=Get-CimInstance Win32_DiskDrive -Filter ('Index='+$cfg.number)
if ($disk -and $disk.PNPDeviceID -ine $cfg.pnp) { exit }
$since=[DateTimeOffset]::FromUnixTimeMilliseconds($cfg.since).LocalDateTime
$events=Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='disk';Id=7;StartTime=$since} -MaxEvents 64 -ErrorAction SilentlyContinue
$prefix='\Device\Harddisk'+$cfg.number+'\'
foreach ($event in $events) {
    [xml]$xml=$event.ToXml()
    foreach ($data in $xml.GetElementsByTagName('Data')) {
        $device=[string]$data.InnerText
        if ($device.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {
            @{event_id=7;record_id=$event.RecordId;device=$device;time_utc=$event.TimeCreated.ToUniversalTime().ToString('o')} | ConvertTo-Json -Compress
            exit
        }
    }
}
'''.replace('CONFIG', config)
    try:
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
            capture_output=True, text=True, timeout=8, creationflags=subprocess.CREATE_NO_WINDOW)
        evidence = json.loads(result.stdout) if result.returncode == 0 and result.stdout.strip() else None
        if isinstance(evidence, dict) and evidence.get('event_id') == 7:
            return evidence
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    return None


def installation_error(error, identity, since):
    evidence = recent_bad_block(identity, since)
    if evidence:
        message = ('Windows reported an unreadable disk block on this iPod during installation. '
                   'The disk may have damaged sectors. Installation is incomplete. '
                   'Back up any readable files and have the disk checked before trying again. '
                   'Resuming installation cannot repair damaged sectors.')
    else:
        message = ('Windows could not read or write the installation files. Installation is incomplete. '
                   'Check that the iPod is still connected and its drive opens in Windows before resuming. '
                   'If this keeps happening, check the USB connection and the disk for unreadable sectors. '
                   'This error alone does not confirm disk damage.')
    return message + '\n\nDetails: ' + str(error), evidence
