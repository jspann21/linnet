# Storage compatibility

Linnet uses an MBR-partitioned FAT32 volume. GPT and exFAT are not supported.
The block API addresses 512-byte units; the FAT32 reader accepts power-of-two
BPB sector sizes from 512 through 4096 and normalizes their geometry. This does
not mean every drive or adapter with those sector sizes is hardware-qualified.

See [hardware support](hardware.md) for tested configurations and variant limits.
Installer plans are tied to the inspected disk and firmware backup; they cannot
be reused on another disk or layout.

## Capacity and power

The PP502x ATA driver reads capacity from IDENTIFY data and checks LBA28/LBA48
bounds. It does not infer capacity or power behavior from an SD card's size.

| Storage class | Policy |
| --- | --- |
| Recognized iFlash adapter | Uses the exact adapter profile and guarded flush/rail-off behavior. |
| Mechanical drive | Uses reported or explicitly recognized rotation and power capabilities; standby and rail-off remain guarded. |
| Other SSD/CF | Uses identified capabilities; removes power only through an admitted safe policy. |
| Unknown adapter | Retains conservative classification and does not inherit another model's power rules. |

Wake restores the board's storage supply, resets, and re-identifies the device.
Unexpected identity or geometry changes reject access. Idle power work is
serialized with the block queue and excluded from filesystem transactions.
Failed write/flush barriers prevent further writes until recovery.

Full-sector file data can use contiguous writes of up to eight logical sectors,
bounded by the current request and cluster. The block queue snapshots those bytes
in its existing buffers and requires exact completion. Partial-sector data and
allocation/directory metadata retain individual sector I/O. Save headers remain
last, with the existing flush barriers, complete checksum, and readback checks.
Transfer failures quarantine the filesystem until explicit recovery.

ATA identity does not measure current draw or establish charging limits.
Replacement storage needs its own power and reliability checks.

Both ATA drivers read sector sizes from the drive: 512-byte logical sectors and
512, 1024, 2048 or 4096-byte physical sectors. Transfers use whole physical blocks;
partial writes preserve neighboring data. Unsupported alignment or geometry is
rejected, and Classic rechecks it after wake. USB sector sizes may differ.

Geometry follows [ATA IDENTIFY](https://read.seas.harvard.edu/cs161/2024/pdf/ata-atapi-8.pdf),
as in [Rockbox](https://github.com/Rockbox/rockbox/blob/master/firmware/drivers/ata-common.c).
Classic reports `pata_physical_transfers` when alignment is active. Model-specific
fallbacks supply missing rotation rates only; power policies remain separate.

## Diagnostics and implementation

In a development build:

```console
python linnet device media-info --port PORT
```

Replace `PORT` with the current Linnet Link port. This reports storage and power
state without waking the drive. General file changes use Disk Mode; see
[Linnet Link](device-link.md#disk-mode-and-refresh-sessions).

- [PP502x ATA](../hal/pp502x/ata.c) and [Classic ATA](../hal/s5l8702/ata.c)
- [Block queue](../services/storage/block.c), [FAT32 reader](../services/storage/fat32.c), and [storage owner](../services/storage/storage.c)
- [Target manifests and hardware policies](../targets/README.md)

FAT geometry, chain, allocation, and write checks remain active. Power loss,
USB loss, and software fault injection are separate qualification cases.
