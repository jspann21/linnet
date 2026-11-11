"""Native game authoring and prepared-package inspection."""
from pathlib import Path
from tools.studio.targets import target_id


def register(commands):
    games=commands.add_parser('games',help='Build or inspect native game packages').add_subparsers(dest='action',required=True)
    build=games.add_parser('build',help='Compile a game project for a device target')
    build.add_argument('--manifest',type=Path,required=True)
    build.add_argument('--target',type=target_id,required=True)
    build.add_argument('--output',type=Path,required=True)
    builtins=games.add_parser('build-library',help='Prepare built-in packages for release/Media Manager')
    builtins.add_argument('--target',type=target_id,required=True)
    builtins.add_argument('--output',type=Path,required=True)
    preview=games.add_parser('preview',help='Build and render a portable C game on the desktop')
    preview.add_argument('--manifest',type=Path,required=True)
    preview.add_argument('--target',type=target_id,required=True)
    preview.add_argument('--output',type=Path,required=True)
    preview.add_argument('--tape',type=Path)
    inspect=games.add_parser('inspect')
    inspect.add_argument('package',type=Path)
    inspect.add_argument('--target',type=target_id,required=True)


def run(args,emit):
    if args.action=='build':
        from tools.studio.game_build import build
        emit(build(args.manifest,args.target,args.output))
    elif args.action=='build-library':
        from tools.studio.game_build import build_library
        emit(build_library(args.target,args.output))
    elif args.action=='preview':
        from tools.simulator.runner import simulate
        from tools.studio.build import ROOT
        emit(simulate(ROOT,args.output,target=args.target,tape=args.tape,game_manifest=args.manifest))
    else:
        from tools.content.game_package import inspect,SLOT_BYTES
        if not args.package.is_file() or args.package.stat().st_size>256+SLOT_BYTES:
            raise ValueError('Invalid game package size')
        emit(inspect(args.package.read_bytes(),args.target))
