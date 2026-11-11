# Hardware support

For installation, use [Media Manager](media-manager.md). For a new model, use
the [porting guide](../targets/README.md).

## Tested devices

| Device | Target | Storage | RAM | Display |
| --- | --- | --- | --- | --- |
| iPod Photo / Color A1099 (M9829) | `photo4g-p98` | iFlash, 64 GB | 32 MiB | 220 × 176 color |
| iPod Video A1136 (MA146) | `video5g-m25` | Toshiba MK3008GAL, 30 GB | 32 MiB | 320 × 240 color |
| iPod Video A1136 | `video5g-m25` | Hitachi HTC426030G5CE00, 30 GB | 32 MiB | 320 × 240 color |
| iPod Video A1136 | `video5g-m25` | iFlash, 125 GB | 32 MiB | 320 × 240 color |
| iPod Video A1136 | `video5g-m25` | Toshiba MK8010GAH, 80 GB | 64 MiB | 320 × 240 color |
| iPod classic A1238 (MB029) | `classic6g-n25` | Samsung HS081HA, 80 GB | 64 MiB | 320 × 240 color |
| iPod classic A1238 (MB147) | `classic6g-n25` | iFlash, 125 GB | 64 MiB | 320 × 240 color |
| iPod classic A1238 (MB562) | `classic6g-n25` | Toshiba MK1231GAL, 120 GB | 64 MiB | 320 × 240 color |
| iPod classic A1238 (MC297, Late 2009 / 7th generation) | `classic6g-n25` | Toshiba MK1634GAL, 160 GB | 64 MiB | 320 × 240 color |
| iPod 4G monochrome A1059 (M9282) | `ipod4g-q21` | Toshiba MK2006GAL, 20 GB | 32 MiB | 160 × 128 grayscale |
| iPod 4G monochrome MP102 (PE435A) | `ipod4g-q21` | Toshiba MK2006GAL, 20 GB | 32 MiB | 160 × 128 grayscale |

Storage capacities describe the tested configurations, not capacity limits.

## Features

| Target | Features |
| --- | --- |
| [Photo P98](../targets/photo4g-p98/README.md) | Music, photos, prepared IPVF video, Cover Flow, and games |
| [Video M25](../targets/video5g-m25/README.md) | Music, photos, native H.264/MPEG-4 video, Cover Flow, and games |
| [Classic N25](../targets/classic6g-n25/target.toml) | Music, photos, native MP4 video, Cover Flow, games, [runtime power management](classic-power.md), and [headset remote controls](headset-remote.md) on 2008/2009 models |
| [Monochrome Q21](../targets/ipod4g-q21/target.toml) | Music and the iPod Mono interface; photos, video, Cover Flow, and games are disabled |

All four targets support native Disk Mode, A/B OS updates, and
[Linnet Link](device-link.md) in development builds. Media Manager handles
installation; Classic requires guided DFU entry for its initial Boot setup.

## Compatibility

### Classic revisions and identification

All Classic revisions use the shared `classic6g-n25` S5L8702 firmware family.
`classic6g`, `classic65g`, and `classic7g` are build aliases for that target;
the target ID does not identify the retail generation.
Download the ZIP labeled `classic6g-7g` for any of these revisions.

| Apple order numbers | Retail revision | Original capacity |
| --- | --- | --- |
| MB029, MB147 | 2007 / 6th generation | 80 GB |
| MB145, MB150 | 2007 / 6th generation, thick enclosure | 160 GB |
| MB562, MB565 | 2008 / 6.5th generation | 120 GB |
| MC293, MC297 | Late 2009 / 7th generation, thin enclosure | 160 GB |

Media Manager normalizes regional order numbers such as `MC297LL/A` and uses
the live Apple product serial when SysInfo is empty. Reports include the model,
revision, original capacity, and identification source without including the
serial. Capacity changes do not change the identified product generation.
Unknown identifiers and ambiguous suffixes remain unknown. Native Classic
Disk Mode and Link can recover the original product metadata from the host's
setup record bound to the current logic-board USB identity.

The [historical model catalogue](../third_party/ipod_models/PROVENANCE.md)
also identifies known variants of the other supported families and contains
metadata for unsupported iPods. Identification does not grant firmware support.
The catalogue can be extended as additional model numbers are verified.

Classic setup saves `hardware.json` and `hardware-attestation.json` with its
host recovery records before flashing. The attestation checks the target,
64 MiB RAM initialization/address tests, panel transfer, supported PMU revision,
PATA compatibility, disk capacity and partition-table signature. NOR installation
separately requires its supported JEDEC identity, two matching complete original
flash reads, a device-bound write plan, and installed-image readback. Unknown
storage, panel output quality, audio quality, physical controls and power behavior
still require physical qualification; a model match or frame-transfer completion
does not establish those results.

The 2008 and 2009 Classics also have an optional headphone-jack controller for
[three-button headset remotes](headset-remote.md). Linnet probes it independently
of the model catalogue; live hardware reports include controller observations
when the running OS exposes the driver.

### Storage and flash

Classic requires PATA storage; CE-ATA is not supported. Its NOR writer supports
JEDEC `BF 25 8E`. Native storage uses 512-byte logical sectors.
See [storage compatibility](storage.md) for filesystem and disk requirements,
[video formats](video-format.md) for supported media, and
[Apple OS library updates](apple-stock-music.md) for sharing music with Apple's
original software.
