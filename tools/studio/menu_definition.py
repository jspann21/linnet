"""Stable editable-menu IDs shared by host settings and generated firmware data.

The numeric IDs and group order are the MENUS.BIN version-three wire format.
Changing a label or target never renumbers an existing saved choice.
Device editing controls shortcuts only. Entries marked device_editable=False
remain host-controlled without changing their wire IDs or host availability.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Item:
    key: str
    label: str
    target: str | None = None
    feature: str | None = None
    device_editable: bool = True


GROUPS = (
    ('Home', (
        Item('music', 'Music', 'linnet.system.music'),
        Item('cover_flow', 'Cover Flow', 'linnet.system.cover-flow', 'cover_flow'),
        Item('photos', 'Photos', 'linnet.system.photos', 'photos'),
        Item('videos', 'Videos', 'linnet.system.videos', 'video'),
        Item('files', 'Files', 'linnet.system.files', 'files'),
        Item('now_playing', 'Now Playing', 'linnet.system.now-playing'),
        Item('settings', 'Settings', 'linnet.system.settings'),
        Item('showcase', 'UI Demo', 'linnet.system.ui-demo', 'demo_or_games', False),
        Item('extras', 'Extras', 'linnet.system.extras'),
        Item('notes', 'Notes', 'linnet.system.notes'),
        Item('shuffle_all', 'Shuffle Songs', '@shuffle-all'),
    ), (0, 3, 2, 4, 8, 6, 10, 5), (6,)),
    ('Music', (
        Item('all_tracks', 'Songs'), Item('artists', 'Artists'),
        Item('albums', 'Albums'), Item('genres', 'Genres'),
        Item('composers', 'Composers'), Item('years', 'Years'),
        Item('folders', 'Folders'), Item('playlists', 'Playlists'),
        Item('search', 'Search'), Item('favorites', 'Favorites'),
        Item('bookmarks', 'Bookmarks'), Item('recently_played', 'Recently Played'),
        Item('most_played', 'Most Played'), Item('recently_added', 'Recently Added'),
        Item('album_artists', 'Album Artists'),
        Item('shuffle_all', 'Shuffle Songs'),
    ), (7, 1, 2, 0, 3, 4, 9, 8, 14, 5, 6, 10, 13, 11, 12), ()),
    ('Settings', (
        Item('display', 'Display', 'linnet.system.display'),
        Item('themes', 'Themes', 'linnet.system.themes', 'themes'),
        Item('clock', 'Date & Time', 'linnet.system.clock'),
        Item('battery', 'Battery', 'linnet.system.battery'),
        Item('auto_shutdown', 'Power', 'linnet.system.shutdown'),
        Item('sleep_timer', 'Sleep Timer', 'linnet.system.sleep-timer'),
        Item('sync', 'USB Disk Mode', '@sync'),
        Item('update', 'Firmware Info', 'linnet.system.update'),
        Item('about', 'About Linnet', 'linnet.system.about'),
        Item('home', 'Back to Home', 'linnet.system.home'),
        Item('video_settings', 'Video Settings', 'linnet.system.video-settings', 'video_settings'),
        Item('sound', 'Sound', 'linnet.system.music-sound'),
    ), (11, 0, 10, 2, 4, 6, 7, 8), (6, 11)),
    ('Extras', (
        Item('clock', 'Date & Time', 'linnet.system.clock'),
        Item('notes', 'Notes', 'linnet.system.notes'),
        Item('videos', 'Videos', 'linnet.system.videos', 'video'),
        Item('files', 'Files', 'linnet.system.files', 'files'),
        Item('demo', 'UI Demo', 'linnet.system.ui-demo', 'demo', False),
        Item('games', 'Games', 'linnet.system.games', 'games', False),
        Item('sleep_timer', 'Sleep Timer', 'linnet.system.sleep-timer'),
    ), (5, 1, 0, 6), ()),
)

SCREENS = {name: tuple(item.label for item in items) for name, items, _, _ in GROUPS}
DEFAULTS = {name: tuple(defaults) for name, _, defaults, _ in GROUPS}
REQUIRED = {name: frozenset(required) for name, _, _, required in GROUPS if required}
ITEMS = {name: items for name, items, _, _ in GROUPS}


def _numbers(values):
    return '{' + ','.join(f'{value}u' for value in values) + '}'


def render_firmware_header() -> str:
    """Emit only the menu data consumed by ui.c; no hand-edited C mirror."""
    if len(GROUPS) != 4 or tuple(SCREENS) != ('Home', 'Music', 'Settings', 'Extras'):
        raise ValueError('MENUS.BIN group order changed')
    for name, items, defaults, required in GROUPS:
        if (not items or len(items) > 16 or len({item.key for item in items}) != len(items) or
                len(set(defaults)) != len(defaults) or any(i not in range(len(items)) for i in defaults) or
                any(i not in range(len(items)) for i in required)):
            raise ValueError(f'invalid stable menu definition: {name}')
    orders = ','.join(_numbers(defaults) for _, _, defaults, _ in GROUPS)
    counts = _numbers(len(defaults) for _, _, defaults, _ in GROUPS)
    limits = _numbers(len(items) for _, items, _, _ in GROUPS)
    required = _numbers(sum(1 << index for index in indices) for _, _, _, indices in GROUPS)
    targets = ',\n    '.join('{' + ','.join('0' if item.target is None else '"' + item.target + '"'
                                       for item in items) + '}' for _, items, _, _ in GROUPS)
    labels = ',\n    '.join('{' + ','.join('"' + item.label + '"' for item in items) + '}' for _, items, _, _ in GROUPS)
    editable = _numbers(sum(1 << index for index, item in enumerate(items) if item.device_editable)
                        for _, items, _, _ in GROUPS)

    def scene_index(item):
        if item.target is None or item.target.startswith('@'):
            return '64u'  # Native action/category, independent of a scene.
        if item.target.startswith('linnet.system.'):
            return 'LN_UI_SCENE_' + item.target.removeprefix('linnet.system.').replace('-', '_').upper()
        return 'UINT32_MAX'

    scenes = ',\n    '.join('{' + ','.join(scene_index(item) for item in items) + '}'
                           for _, items, _, _ in GROUPS)
    return ('/* Generated from tools/studio/menu_definition.py. */\n'
            '#ifndef LINNET_MENU_DEFINITION_H\n#define LINNET_MENU_DEFINITION_H\n'
            '#include "linnet_scene_ids.h"\n'
            f'#define LN_MENU_DEFAULT_ORDER {{{orders}}}\n'
            f'#define LN_MENU_DEFAULT_COUNTS {counts}\n'
            f'static const uint8_t ln_menu_limits[LN_MENU_GROUPS]={limits};\n'
            f'static const uint32_t ln_menu_required[LN_MENU_GROUPS]={required};\n'
            f'static const uint32_t ln_menu_device_editable[LN_MENU_GROUPS]={editable};\n'
            f'static const uint32_t ln_menu_scene_ids[LN_MENU_GROUPS][16]={{\n    {scenes}\n}};\n'
            f'static const char *const ln_menu_labels[LN_MENU_GROUPS][16]={{\n    {labels}\n}};\n'
            f'static const char *const ln_menu_targets[LN_MENU_GROUPS][16]={{\n    {targets}\n}};\n'
            '#endif\n')


def write_firmware_header(directory: Path) -> Path:
    path = Path(directory) / 'linnet_menu_definition.h'
    path.write_text(render_firmware_header(), encoding='utf-8')
    return path
