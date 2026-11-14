# Bootstrap tooling

Use [Media Manager](media-manager.md) for supported installation. This map is for
contributors working on the lower-level boot paths.

| Stage | PortalPlayer targets | Classic S5L8702 |
| --- | --- | --- |
| Build | [Shared builder](../tools/studio/build.py) creates Boot/OS images. | Shared builder for ordinary images; [SRAM stage builder](../tools/studio/s5l_bootstrap.py) for initial bootstrap. |
| Inspect and plan | [Installer](../tools/studio/installer.py) assesses the disk; [native bootstrap](../tools/studio/native_bootstrap.py) derives a backup-bound firmware-directory plan. | [S5L bootstrap](../tools/link/s5l_bootstrap.py) captures the device/USB context; [NOR maintenance](../tools/link/classic_nor.py) uses its separate plan. |
| Transfer | [Link Session](../tools/link/session.py) handles ordinary uploads. | BootROM/DFU transfers the temporary monitor; ordinary OS updates then use Link. |
| Publish and recover | Candidate write and readback precede the directory change; restoration uses the saved original directory. | [Classic setup](../tools/studio/classic_setup.py) in Media Manager owns initial DFU entry, hardware checks, NOR backup/install and native Disk Mode OS publication. Recovery restores verified original flash through the SRAM monitor before cleanup. |

PortalPlayer directory planning exposes `plan`, `apply`, and `restore` through
`python -m tools.studio.native_bootstrap`. Classic entry uses
`python linnet device sram-bootstrap`. Inspect each command's `--help` before
preparing a developer plan; ordinary users follow Media Manager's prompts.
Classic release ZIPs include a precompiled bootstrap template. Media Manager
binds the template to the inspected device and validates all hashes before
loading it. The smaller User Boot fits in the NOR archive even
when the selected OS is a Developer build. The mounted-volume route for an installed file bootloader is owned by
[bootstrap.py](../tools/studio/bootstrap.py).

Directory plans stage Boot in aligned space after every referenced firmware
image. They prefer an all-zero extent, but can use nonzero trailing space when
no zero-filled extent fits. Before writing, the installer verifies those bytes
against the full saved firmware backup; it verifies the staged Boot before
publishing the directory. Restoring the original directory leaves the staged
bytes unreferenced and keeps every original firmware image intact.

Classic `install-nor` retains its original and installed flash snapshots under
`%LOCALAPPDATA%/Linnet/Installer`, independently of build output. `preserve-apple`
and native OS publication copy the completed record onto the iPod for portable
recovery. Media Manager validates the current recovery record and snapshots
against this logic board.

Classic User Disk Mode supports a recovery eject through Windows
[SCSI pass-through](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ns-ntddscsi-_scsi_pass_through).
After verifying, locking and dismounting the selected volume, Media Manager sends
a 16-byte vendor CDB: `0xc0` followed by ASCII `LINNET-RECOVERY`, with no data
transfer. Disk Mode flushes ATA and acknowledges the command before resetting
directly into the installed SRAM monitor, bypassing the OS and saved startup
default. Disk Mode has no NOR write commands; the monitor still checks its build
identity and the device-bound backups before restoration. Older Developer
firmware retains its Link restart path. If older User firmware rejects recovery
eject, update to the current User build, start the updated Linnet OS, then enter
**Settings → USB Disk Mode**. The OS provides the updated Disk Mode directly;
entering Disk Mode through an older persistent Boot can still use the old code.

If a Classic restore stops while the SRAM monitor is running, use
`python linnet device sram-bootstrap restore-nor --help`. Supply the saved
`NOR.JSON`, its SHA-256, and the same host restore receipt. Restoration accepts
only sectors from the verified installed or original snapshots; unknown or
partially programmed sectors stop further writes. The original entry sector is
written last, and a full NOR readback must match before restarting Apple.
After reconnecting to Apple's USB disk, **iPod setup → Remove…** finishes cleanup.
Apple software's normal USB disk connection is sufficient; recovery Disk Mode
is needed only if the disk does not appear.
The original backup, installed snapshot, and receipts must remain available.

These workflows are not interchangeable. Preserve the original firmware, plan,
predecessor files, hashes, and receipt together. A successful build or plan does
not establish the device's current state. See [local artifacts](local-artifacts.md)
and [Linnet Link](device-link.md) for recovery and mode transitions.
