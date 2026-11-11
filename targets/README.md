# Adding an iPod target

A target owns hardware identity, memory, display, media, discovery, and installer
policy. Start with a manifest and native adapters; do not infer support from an
iPod's marketing name alone.

See [hardware support](../docs/hardware.md) for current targets, tested devices,
and features.

## Inspect or scaffold a target

```console
python linnet target-report --target photo4g-p98
python -m tools.studio.target_scaffold new-model
```

The report shows resolved sources, capabilities, memory, decoders, themes, and
blockers without building. The scaffold creates disabled placeholders; replace
them with measured values and implemented hardware before enabling builds.
Keep `installer.enabled = false` until installation and recovery are qualified.

## Repository contract

| Location | Owns |
| --- | --- |
| `hal/api/` | Portable interfaces consumed by kernel, services, and UI |
| `hal/<family>/` | Shared SoC startup, CPU, storage, USB, and input |
| `hal/codecs/` | Audio drivers selected by `audio_codec` |
| `targets/<id>/target.toml` | Identity, memory, capabilities, media, resources, and installer policy |
| `hardware.h`, `device.c`, `power.c` | Board wiring and bounded hardware policies |
| `lcd.c`, `display.c` | Panel driver and presentation adapter |
| `system.toml`, `screens/`, `themes/` | Optional default content and native presentation |
| `SOURCES` | Optional additional repository-relative native source paths |

The build selects target `device.c` or falls back to `board.c`. Target
`power.c` and `display.c` override their HAL counterparts. Startup, linker,
and platform files follow conventional resolution. Family and target
`SOURCES` files extend the source set; manifests do not contain arbitrary
source lists.

[targets.py](../tools/studio/targets.py) validates and resolves manifests.
The builder generates `linnet_target.h` from the resolved configuration and
checks the complete source set before compilation. Use the existing
[Photo manifest](photo4g-p98/target.toml) as a field reference.

Memory regions must account for the OS, stacks, staging, recovery, capture,
resources, and mailbox without overlap. Match CPU/ABI flags to the linker and
startup contract. A coherent manifest is not proof that a device can boot it.

## Presentation and assets

`presentation_profile = "color320"` shares native M25/N25 screens and layout
generation from `ui/profiles/color320/`. A target `screens/` directory
replaces the complete profile set; every selected scene must exist. Target
themes override profile themes, which override shared themes by stable ID.

Other targets can use the reference canvas and `presentation.toml`.
Capabilities, `game_presentation`, and named presentation/runtime policies
control features; avoid adding board-name conditions to portable services.

| Asset | Source or regeneration |
| --- | --- |
| Photo Boot menu font | `python tools/studio/boot_menu_font.py --output targets/photo4g-p98/boot_menu_font.h` |
| Photo Disk Mode screen | `python tools/studio/disk_screen_asset.py --output targets/photo4g-p98/disk_screen_runs.h` |
| Video and Classic Disk Mode screen | `python tools/studio/disk_screen_asset.py --target color320 --output ui/profiles/color320/disk_screen_runs.h` |
| Game graphics | Native drawing sources in `services/games/presentations/` |
| Solitaire court art | [Austin Gabriel's CC0 cards](https://github.com/AustinGabriel/Public-Domain-and-CC0-Playing-Cards); retain notices in both presentation headers |

Keep source font notices with regenerated artwork. Build-generated headers
remain under the invocation's `out/` directory.

## Discovery and installation

Populate `[retail]` from observed USB and device records. Labels and volume names
alone are insufficient.

The automatic installer currently enables only the `ipod-directory` layout.
Classic's `s5l-nor` path is separate. A target-specific maintenance writer must
retain its admission checks; ordinary A/B OS updates are shared.

See [bootstrap tooling](../docs/bootstrap-tooling.md) and
[Media Manager](../docs/media-manager.md) for installation and recovery.

## Validate a port

Use `target-report`, `doctor`, content compilation, and the simulator first.
Pass the same complete system configuration to UI, simulator, and OS builds.

```console
python linnet build --target TARGET_ID --profile release --kind os --verify
python linnet build --target TARGET_ID --profile release --kind boot --verify
python linnet build --target TARGET_ID --profile dev --kind os --device-serial DEVICE_SERIAL --verify
```

Replace placeholders with the actual target and observed serial. On hardware,
exercise startup, display, controls, audio, storage, power, updates, rollback,
and installer restoration using existing scenarios. Do not turn host or synthetic
input results into claims about physical switches, sound, current, or LCD quality.
