"""Linnet command surface. Command parsing and dispatch."""
import argparse
import json
import subprocess
import sys

def emit(value):
    print(json.dumps(value, indent=2))

def parser():
    cli = argparse.ArgumentParser(prog="linnet", description="Native open firmware for classic iPods")
    commands = cli.add_subparsers(dest="command", required=True)
    from tools.cli.studio_commands import register_primary, register_support, register_report
    register_primary(commands)
    from tools.cli.game_commands import register as register_games
    register_games(commands)
    from tools.cli.media_commands import register_media
    register_media(commands)
    register_support(commands)
    from tools.cli.device_commands import register_device
    register_device(commands)
    register_report(commands)
    return cli

def main():
    args = parser().parse_args()
    try:
        if args.command in ('music', 'video', 'photos'):
            from tools.cli.media_commands import run_media
            from tools.content.host_cache import activity
            with activity():
                return run_media(args, emit)
        if args.command == 'device':
            from tools.cli.device_commands import run_device
            return run_device(args, emit)
        if args.command == 'games':
            from tools.cli.game_commands import run
            return run(args,emit)
        from tools.cli.studio_commands import run_studio
        return run_studio(args, emit)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"linnet: {exc}", file=sys.stderr)
        return 1
