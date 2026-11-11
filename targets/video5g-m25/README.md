# iPod Video M25

The `video5g-m25` target uses PP5022, WM8758 audio, BCM2722 video/display,
and a 320 × 240 LCD. See [hardware support](../../docs/hardware.md) for tested
configurations and features.

## Source ownership

| Path | Responsibility |
| --- | --- |
| `target.toml`, `system.toml` | Hardware/build policy and default content |
| `device.c`, `hardware.h` | Board wiring, storage power, charging, and bounded quirks |
| `lcd.c`, `display.c` | BCM display transport and presentation |
| `../../hal/pp502x/`, `../../hal/codecs/` | Shared SoC and selected audio driver |
| `../../ui/profiles/color320/` | Native screens, layouts, and theme editions |
| `../../services/games/presentations/color320/` | Game presentation and artwork |

The display inherits ROM-initialized resources. Linnet does not embed an Apple
firmware image; [native video](NATIVE_VIDEO.md) obtains required vendor resources
from the device's verified backup.

Theme availability follows the manifest. Game controls are shared with
[Photo](../photo4g-p98/README.md#games), with a dedicated 320 × 240 layout.
Media Manager's **Games** selection applies after save, eject, and restart.

## Development

Use the [Windows setup](../../README.md#develop-on-windows), then build with
`--target video5g-m25`. Development images require the observed device serial.
Upload the resulting `.los` envelope, not its raw `.bin` payload.

[Linnet Link](../../docs/device-link.md) supports RAM boot, persistent updates,
input, and screenshots. Native Disk Mode handles library transfers. When using
Apple Disk Mode, follow the reviewed refresh-session workflow in that guide.

The battery percentage curve is an estimate, not a capacity measurement.
Replacement storage uses its own identified capabilities; the target's narrow
factory partition-tail exception does not relax general FAT checks.

Original and Late 2006 Video models share this target. Retail model metadata
identifies the original product; live SysCfg revision selects the BCM display
configuration. The OS starts with a 32 MiB memory layout and admits an upper
bank only for supported revisions after its address-independence probe passes.
Link device details and hardware reports include the live board revision and
usable RAM. Usable RAM describes memory admitted by the OS; it does not infer
physical chip capacity from the original drive size or a replacement drive.

The Hitachi HTC426030G5CE00 omits the later ATA rotation-rate field. The exact
model is classified as a 4200 RPM mechanical drive using Hitachi's specification.
This fallback does not authorize supply removal or add the drive to the DMA
allowlist. Native capability checks and conservative PIO transfers still apply.

## Hardware references

- [Original firmware research](https://github.com/giek2000/ipod-classic-firmware-research/blob/main/specs/iPod_5th_Gen_Video_13_1_2_1.md)
- [BCM display protocol](https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/ipod/video/lcd-video.c)
- [M25 board configuration](https://github.com/Rockbox/rockbox/blob/master/firmware/export/config/ipodvideo.h)
- [WM8758 definitions](https://github.com/Rockbox/rockbox/blob/master/firmware/export/wm8758.h)
- [Hitachi C4K60 Slim manufacturer datasheet](https://datasheet.datasheetarchive.com/originals/crawler/hgst.com/d92c3aab98e4baa4c9cb67c57fa9e796.pdf)

Imported codecs retain their own [licenses and provenance](../../third_party/README.md).
