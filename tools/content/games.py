"""Guarded selection of firmware games on capable targets."""
from pathlib import Path

from . import media_manager as media
from .publish import _plain
from tools.studio.targets import target_spec

GAMES = (("Comet Run", "Steer through a rising meteor storm."),
         ("Pulse Path", "Remember and repeat an expanding light pattern."),
         ("Solitaire", "Play draw-one Klondike and build four foundations."),
         ("Sudoku", "Solve 9 by 9 puzzles at four difficulty levels."),
         ("Brick", "Break six handcrafted walls with precise paddle play."),
         ("Music Quiz", "Name songs from your own music library."),
         ("Vortex", "Orbit the core and clear luminous enemy waves."),
         ("Minesweeper", "Clear safe tiles across three difficulty levels."),
         ("Falling Blocks", "Build clean lines with seven classic pieces."),
         ("Reversi", "Outflank a thoughtful computer opponent."),
         ("Snake", "Turn, boost, and grow through a polished grid."),
         ("Word Search", "Find hidden words across three curated boards."),
         ("Scrub", "Pin one echo, then scrub time to align all three."),
         ("Dial Heist", "Find the tumblers before the patrol clock runs out."),
         ("Mixtape", "Place loops on a rotating eight-beat map."),
         ("Signal", "Tune the dial and hold each broadcast to decode it."),
         ("Clockwork City", "Rotate street rings to open the courier route."),
         ("Pocket Pinball", "Tilt the table and kick the ball through targets."))


def validate_selection(selected):
    if not isinstance(selected, (set, list, tuple)) or any(type(i) is not int or not 0 <= i < len(GAMES) for i in selected):
        raise ValueError("Select known games only.")
    return set(selected)


def available(identity):
    return target_spec(identity['target_id'])['capabilities'].get('games', False)


def supported(identity):
    spec = target_spec(identity['target_id'])
    count = len(GAMES)
    return set(range(count)) if spec['capabilities'].get('games', False) else set()


def _root(identity):
    if not available(identity):
        raise ValueError("Games are unavailable on this iPod.")
    root = Path(identity['root'])
    path = root / '.LINNET'
    _plain(path, root)
    if not path.is_dir():
        raise ValueError("Install Linnet on this iPod first.")
    return root


def read(identity):
    identity = media.refresh(identity)
    root = _root(identity)
    from .game_install import inventory
    snapshot=inventory(root,identity['target_id'])
    if snapshot['valid']:
        return {r['builtin'] for r in snapshot['rows'] if r['builtin']!=0xffffffff}
    return supported(identity)


def prepared(target, directory=None):
    from tools.studio.game_build import library
    from tools.studio.targets import ROOT
    candidates=[Path(directory)] if directory is not None else [ROOT/'resources/games'/target,ROOT/'out/game-packages'/target]
    for path in candidates:
        if (path/'library.json').is_file():return library(path,target)
    raise ValueError('Prepared game packages are missing. Install the matching Media Manager release or build the game library from source.')


def custom(identity):
    identity=media.refresh(identity)
    from .game_install import inventory
    return [r for r in inventory(identity['root'],identity['target_id'])['rows'] if r['builtin']==0xffffffff]


def choices(identity):
    identity=media.refresh(identity)
    from .game_install import inventory
    snapshot=inventory(identity['root'],identity['target_id'])
    return dict(selected={r['builtin'] for r in snapshot['rows'] if r['builtin']!=0xffffffff} if snapshot['valid'] else read(identity),
                custom=[r for r in snapshot['rows'] if r['builtin']==0xffffffff])


def save(identity, selected, library=None, recover=False):
    validate_selection(selected)
    if set(selected) - supported(identity):
        raise ValueError("This game is unavailable on this iPod.")
    identity = media.refresh(identity)
    from . import game_install,game_package
    if recover:game_install.recover_catalogues(identity)
    root=_root(identity)
    available=prepared(identity['target_id'],library) if selected else {}
    if any('linnet.'+game_package.BUILTIN_IDS[i] not in available for i in selected):
        raise ValueError('The prepared library is missing a selected built-in game')
    packages=[available['linnet.'+game_package.BUILTIN_IDS[i]] for i in sorted(set(selected))]
    # Changing built-in choices must preserve independently installed games.
    snapshot=game_install.inventory(root,identity['target_id'])
    packages.extend(root/'.LINNET/GAMES'/r['filename'] for r in snapshot['rows'] if r['builtin']==0xffffffff)
    game_install.install(identity,packages,expected=snapshot['tokens'])
    return set(selected)


def import_package(identity, path):
    identity=media.refresh(identity)
    from . import game_install,game_package
    candidate=Path(path)
    if candidate.stat().st_size>256+game_package.SLOT_BYTES:raise ValueError('Game package is too large')
    info=game_package.inspect(candidate.read_bytes(),identity['target_id'])
    snapshot=game_install.inventory(identity['root'],identity['target_id']);rows=snapshot['rows']
    paths=[Path(identity['root'])/'.LINNET/GAMES'/r['filename'] for r in rows if r['id']!=info['id']]
    return game_install.install(identity,[*paths,candidate],expected=snapshot['tokens'])


def remove_package(identity, game_id):
    identity=media.refresh(identity)
    from . import game_install
    snapshot=game_install.inventory(identity['root'],identity['target_id']);rows=snapshot['rows']
    paths=[Path(identity['root'])/'.LINNET/GAMES'/r['filename'] for r in rows if r['id']!=game_id]
    return game_install.install(identity,paths,expected=snapshot['tokens'])
