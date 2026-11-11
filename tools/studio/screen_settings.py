"""Device menu preferences shared by the screen editor and firmware."""
from pathlib import Path
import struct
import tomllib
import zlib
from tools.content import media_manager as media
from tools.studio.targets import target_spec
from tools.content.compiler import resolve_features
from tools.content.layouts import feature_enabled
from tools.content.guard import guard_volume as _volume_guard
from tools.content.guard import require_linnet
from tools.studio.menu_definition import SCREENS, DEFAULTS, REQUIRED, ITEMS

HINTS = {**{label: 'Shown under Music > More, in the order chosen here.' for label in ('Album Artists', 'Years', 'Folders', 'Bookmarks', 'Recently Added', 'Recently Played', 'Most Played')}, 'Artists': 'Performers credited on individual tracks.', 'Album Artists': 'Music > More: main artist credited for each album; useful for compilations.', 'Composers': 'Writers of the music; useful for classical collections.'}


def item_availability(name, identity=None):
    root = Path(__file__).resolve().parents[2]
    target = (identity or {}).get('target_id')
    config = root / 'targets' / target / 'system.toml' if target else root / 'config' / 'system.toml'
    if not config.is_file():
        config = root / 'config' / 'system.toml'
    settings = tomllib.loads(config.read_text(encoding='utf-8'))
    target = target or settings['system']['target']
    configured = resolve_features(settings)
    spec = target_spec(target)
    features = resolve_features(settings, spec)
    apps = set(settings.get('ui', {}).get('apps', []))
    game_app = spec['capabilities'].get('games', False) and 'linnet.system.games' in apps
    available, unavailable = [], []
    for i, item in enumerate(ITEMS[name]):
        feature = item.feature
        if feature == 'demo_or_games' and game_app:
            available.append(i)
            continue
        if item.target and not item.target.startswith('@') and item.target not in apps:
            continue
        if feature == 'demo_or_games':
            feature = 'demo'
        if feature and not feature_enabled(configured, feature):
            continue
        if feature and not feature_enabled(features, feature):
            unavailable.append(i)
        else:
            available.append(i)
    return available, unavailable


def available_items(name, identity=None):
    return item_availability(name, identity)[0]


def locked_items(name):
    """Required navigation and settings items."""
    return set(REQUIRED.get(name, ()))


def defaults(identity=None):
    result = {}
    for name, rows in DEFAULTS.items():
        rows = list(rows)
        available = available_items(name, identity) if identity is not None else rows
        result[name] = [i for i in rows if i in available]
    return result


def encode(state):
    data = bytearray(b'MENU' + struct.pack('<I', 3))
    for name, labels in SCREENS.items():
        rows = state[name]
        if (not isinstance(rows, (list, tuple)) or not rows or any(type(i) is not int or i not in range(len(labels)) for i in rows) or len(set(rows)) != len(rows)
                or not REQUIRED.get(name, set()).issubset(rows)):
            raise ValueError(f'{name}: select at least one item and keep required navigation.')
        data.extend(bytes([len(rows), *rows]) + bytes(16 - len(rows)))
    return bytes(data) + struct.pack('<I', zlib.crc32(data))


def decode(data, identity=None):
    if len(data) != 80 or data[:4] != b'MENU' or struct.unpack_from('<I', data, 4)[0] != 3 or zlib.crc32(data[:76]) != struct.unpack_from('<I', data, 76)[0]:
        raise ValueError('Screen settings are damaged or from an unsupported version. Reset defaults to replace them.')
    state = {name: list(data[9 + g * 17:9 + g * 17 + data[8 + g * 17]]) for g, name in enumerate(SCREENS)}
    encode(state)
    if identity is not None and 10 not in available_items('Settings', identity):
        state['Settings'] = [item for item in state['Settings'] if item != 10]
    return state


def read(identity):
    identity = media.refresh(identity)
    root = Path(identity['root'])
    from . import device_preferences
    saved = device_preferences.read(root)['value']['menus']
    if saved is not None:
        return decode(saved, identity)
    return defaults(identity)


def reconcile(base, desired, current):
    """Merge edits to separate groups; require review for competing group edits."""
    if base is None:
        raise ValueError('Reload screen settings from the iPod before saving.')
    result = {}
    for name in SCREENS:
        if desired[name] == base[name]:
            result[name] = list(current[name])
        elif current[name] == base[name] or current[name] == desired[name]:
            result[name] = list(desired[name])
        else:
            raise ValueError(f'{name} was edited on the iPod and in Media Manager. Reload from iPod, review the changes, then apply your edits again.')
    encode(result)
    return result


def save(identity, state, expected=None):
    from tools.content.publish import _exclusive
    from . import device_preferences
    encode(state)
    identity = media.refresh(identity)
    with _exclusive(identity):
        require_linnet(identity)
        root = Path(identity['root'])
        snapshot = device_preferences.read(root)
        saved = snapshot['value']['menus']
        current = decode(saved, identity) if saved is not None else read(identity)
        merged = reconcile(expected, state, current)
        data = encode(merged)
        # Preserve current theme, scores and brightness; publish the authoritative
        # menus through the same A/B store the firmware uses.
        device_preferences.publish(root, snapshot, data, lambda: _volume_guard(identity))
        return merged
