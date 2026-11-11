"""Create an inert target skeleton without copying a qualified device identity."""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from tools.studio.targets import ALIASES, ROOT, TARGET_IDS_BY_NUMBER


MANIFEST = '''# All hardware, media, and memory values below are placeholders.
# Replace them with measured board data before enabling a build.
id = "{target}"
name = "Unqualified {target} target"
aliases = []
image_target = {image_target}
board_id = "unassigned"
model_alias = "unassigned"
soc = "unassigned"
hal_family = "unassigned"
audio_codec = "unassigned"
cpu = "unassigned"
architecture = "armv4t"
endianness = "little"
build_enabled = false
bringup_blocker = "Identify the board, memory, display, storage, codecs, sources, and recovery path."
sdram_bytes = 33554432
os_capacity_bytes = 4194304
resident_limit_bytes = 4194304
stage_address = 8388608
stage_bytes = 4194304
recovery_snapshot_address = 16777216
recovery_snapshot_bytes = 1048576
storage_max_read_sectors = 192
lcd_width = 160
lcd_height = 128
lcd_format = "gray2-horizontal"
media_photo_width = 160
media_photo_height = 128
media_video_width = 160
media_video_height = 128
media_video_fps = 12
media_video_color = "gray4"
media_video_backend = "none"
media_art_side = 64
media_video_extensions = [".ipvf"]
capabilities = {{ photos = false, video = false, files = false, demo = false, themes = false, cover_flow = false, games = false }}
cop_display = false
entry_address = 0
load_address = 0
mailbox_address = 33550336
mailbox_bytes = 4096

[retail]
usb_vid = 0
usb_pid = 0
usb_identity_only = true
board_names = []
model_numbers = []
regulatory_models = []

[installer]
enabled = false
boot_layout = "unavailable"
file_loader = "disabled/disabled"
wrapper_model = "NONE"
wrapper_checksum_seed = 0

[resources]
music_titles_address = 20971520
music_titles_bytes = 1048576
coverflow_catalog_slots = 16
'''

SKELETON = {
    'hardware.h': '#error "Replace placeholder hardware policy before enabling this target"\n',
    'board.c': '#error "Implement and validate board wiring before enabling this target"\n',
    'lcd.c': '#error "Implement and validate the display before enabling this target"\n',
    'display.c': '#error "Implement the target display adapter before enabling this target"\n',
    'SOURCES': '',
    'README.md': '''# {target} bring-up

This target is disabled. `target.toml` contains placeholders and does not
identify a qualified device. The C skeletons deliberately fail compilation.

Before setting `build_enabled = true`, replace the board, SoC, CPU, memory,
display, codec, USB and storage identities with measured values; implement the
HAL family and target sources; add a valid target `system.toml` and presentation
assets; establish original firmware backup and recovery; then review the
installer separately. Keep `installer.enabled = false` until its writer is
qualified on this exact device.

Run `python linnet target-report --target {target}` to see missing source and
presentation inputs while the port remains disabled.
''',
}


def create(target: str, destination: Path | None = None) -> dict:
    if not re.fullmatch(r'[a-z][a-z0-9-]*', target):
        raise ValueError('target ID must be lowercase letters, digits, and hyphens')
    if target in ALIASES:
        raise ValueError(f'target ID is already registered: {target}')
    parent = Path(destination) if destination is not None else ROOT / 'targets'
    parent = parent.resolve()
    directory = parent / target
    if directory.exists():
        raise ValueError(f'target directory already exists: {directory}')
    image_target = next(number for number in range(0xffffffff, -1, -1)
                        if number not in TARGET_IDS_BY_NUMBER)
    directory.mkdir(parents=True)
    (directory / 'target.toml').write_text(
        MANIFEST.format(target=target, image_target=image_target), encoding='utf-8')
    for name, content in SKELETON.items():
        (directory / name).write_text(content.format(target=target), encoding='utf-8')
    return dict(target_id=target, directory=str(directory), build_enabled=False,
                installer_enabled=False, files=sorted(['target.toml', *SKELETON]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target')
    args = parser.parse_args()
    result = create(args.target)
    print(result['directory'])


if __name__ == '__main__':
    main()
