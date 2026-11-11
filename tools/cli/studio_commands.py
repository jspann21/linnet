"""Build, studio, storage, and report CLI commands."""
from pathlib import Path
import json
import sys

from tools.cli.arguments import _record_arguments
from tools.inspect.image import inspect, verify_release
from tools.studio.build import build, toolchain
from tools.studio.targets import ROOT, TARGETS, target_id


def register_primary(commands):
    commands.add_parser("doctor")
    commands.add_parser("setup", help="Install the Windows compiler and build the Apple database tools")
    target_report = commands.add_parser("target-report", help="Inspect resolved target inputs without building")
    target_report.add_argument("--target", required=True, type=target_id)
    commands.add_parser("media", help="Open the desktop iPod media manager")
    support=commands.add_parser('hardware-report',help='Read hardware information for support without installing or uploading')
    support.add_argument('--output',type=Path,help='Save a shareable JSON report on this computer')
    installer = commands.add_parser("installer", help="Inspect, install, restore, or remove Linnet without erasing media")
    installer_actions = installer.add_subparsers(dest="installer_action", required=True)
    for action in ("inspect", "install", "restore", "uninstall", "upgrade-boot"):
        entry = installer_actions.add_parser(action)
        entry.add_argument("--mount", type=Path, required=True)
        if action in ("install", "upgrade-boot"):
            entry.add_argument("--build", choices=("user", "dev"),
                               help="Source build type (default: user); ZIP build type is detected automatically")
            if action == "install":
                entry.add_argument("--method", choices=("native-preserve", "native-selector", "rockbox-loader"),
                                   help="boot installation method (default: native for fresh installs, saved method for reinstall/resume)")
            source = entry.add_mutually_exclusive_group()
            source.add_argument("--firmware", type=Path, help="prebuilt firmware ZIP")
            source.add_argument("--from-source", action="store_true", help="compile fresh firmware locally")
    compile_ = commands.add_parser("build")
    compile_.add_argument("--target", default="photo4g-p98", type=target_id)
    compile_.add_argument("--profile", choices=("release", "dev", "trace"), required=True)
    compile_.add_argument("--jobs", type=int, choices=range(1, 5), default=2,
                          help="parallel compiler jobs, 1–4 (default: 2)")
    compile_.add_argument("--verify", action="store_true")
    compile_.add_argument("--kind", choices=("os", "boot"), default="os")
    compile_.add_argument("--probe", choices=("frame", "crash", "hang"), default="frame")
    compile_.add_argument("--device-serial", default="")
    compile_.add_argument("--boot-selector", choices=("none", "rockbox"), default="none")
    compile_.add_argument("--link-optimization", choices=("none", "services-size", "services-thin-lto"), default="none",
                          help="optional M25 OS service link optimization (default: none)")
    compile_.add_argument("--system", type=Path, help="system.toml configuration")
    ui=commands.add_parser("ui").add_subparsers(dest="action",required=True)
    ui_compile=ui.add_parser("compile")
    ui_compile.add_argument("--output",type=Path,default=Path('out/ui'))
    ui_compile.add_argument("--system",type=Path)
    simulator=commands.add_parser("simulator")
    simulator.add_argument("--output",type=Path,default=Path('out/simulator'))
    simulator.add_argument("--system",type=Path)
    simulator.add_argument("--target",choices=sorted(TARGETS))
    simulator.add_argument("--all-scenes",action="store_true")
    simulator.add_argument("--menus",type=Path,help="Preview an encoded Media Manager menu selection")
    simulator.add_argument("--tape",type=Path,help="JSON list of native input events")
    simulator.add_argument("--boot-menu",action="store_true",help="Render the native 320-pixel Boot chooser")
    simulator.add_argument("--apple-font",type=Path,help="Preview a locally extracted PODIUM.FNT")


def register_support(commands):
    read = commands.add_parser("inspect")
    read.add_argument("image", type=Path)
    releases = commands.add_parser("release").add_subparsers(dest="action", required=True)
    release = releases.add_parser("verify")
    release.add_argument("image", type=Path)
    release.add_argument("--symbols", type=Path, required=True)
    bundle = releases.add_parser("bundle", help="Build a portable prebuilt firmware ZIP")
    bundle.add_argument("--target", required=True, type=target_id)
    bundle.add_argument("--output", required=True, type=Path)
    bundle.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    bundle.add_argument('--build', choices=('user', 'dev'), default='user', help='Firmware build type (default: user)')
    media = releases.add_parser("media", help="Package the standalone Windows Media Manager")
    media.add_argument("--output", required=True, type=Path)
    media.add_argument("--installer-output", type=Path, help="Also build a Windows setup EXE")
    storage=commands.add_parser("storage").add_subparsers(dest="action",required=True)
    package=storage.add_parser("package")
    package.add_argument("--image",type=Path,required=True)
    package.add_argument("--output",type=Path,required=True)
    provision=storage.add_parser("provision")
    provision.add_argument("--package",type=Path,required=True)
    provision.add_argument("--mount",type=Path,required=True)
    provision.add_argument("--serial",required=True)
    theme_provision=storage.add_parser("theme-provision",help="Add optional theme selection records to an identified Disk Mode installation")
    _record_arguments(theme_provision)


def register_report(commands):
    report = commands.add_parser("report")
    report.add_argument("run_id")


def run_studio(args, emit):
    if args.command == "setup":
        from tools.studio.toolchain_setup import setup
        emit(setup())
    elif args.command == "doctor":
        emit(dict(toolchain=toolchain(), python=sys.version.split()[0],
                  supported_targets=sorted(target for target, spec in TARGETS.items() if spec.get('build_enabled', True)),
                  bringup_targets={target: spec['bringup_blocker'] for target, spec in TARGETS.items() if not spec.get('build_enabled', True)}))
    elif args.command == "target-report":
        from tools.studio.target_report import describe
        emit(describe(args.target))
    elif args.command == "media":
        from tools.studio.media_app import launch
        launch()
    elif args.command == 'hardware-report':
        from tools.studio.device_support import collect
        result=collect()
        if args.output:
            args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        emit(result)
    elif args.command == "installer":
        from tools.studio.installer import assess, identity_for_mount, install, restore, uninstall
        identity = identity_for_mount(args.mount)
        if args.installer_action == "inspect":
            emit(assess(identity))
        elif args.installer_action == "install":
            emit(install(identity, progress=lambda message: print(message, file=sys.stderr), build=args.build,
                         installation_method=args.method, firmware=args.firmware, from_source=args.from_source))
        elif args.installer_action == "upgrade-boot":
            from tools.studio.boot_upgrade import upgrade
            emit(upgrade(identity, progress=lambda message: print(message, file=sys.stderr),
                         build=args.build, firmware=args.firmware, from_source=args.from_source))
        elif args.installer_action == "uninstall":
            emit(uninstall(identity, progress=lambda message: print(message, file=sys.stderr)))
        else:
            emit(restore(identity))
    elif args.command == "build":
        emit(build(args.profile, args.target, args.verify, args.kind, args.probe,
                   args.device_serial, system_config=args.system,
                   boot_selector=args.boot_selector, jobs=args.jobs,
                   link_optimization=args.link_optimization))
    elif args.command == "ui":
        from tools.content.compiler import compile_system
        emit(compile_system(ROOT,args.output,args.system))
    elif args.command == "simulator":
        if args.boot_menu:
            if args.tape or args.system or args.all_scenes or args.apple_font or args.menus:
                raise ValueError("Boot chooser simulation does not accept UI scene options")
            from tools.simulator.boot import simulate_boot
            emit(simulate_boot(args.output,args.target or "classic6g-n25"))
        else:
            from tools.simulator.runner import simulate
            emit(simulate(ROOT,args.output,args.tape,args.system,args.target,args.all_scenes,args.apple_font,args.menus))
    elif args.command == "inspect":
        emit(inspect(args.image.read_bytes()))
    elif args.command == "release":
        if args.action == "bundle":
            from tools.studio.firmware_bundle import create
            emit(create(args.target, args.output, args.jobs, build=args.build))
        elif args.action == "media":
            from tools.studio.media_distribution import create
            emit(create(args.output, args.installer_output))
        else:
            emit(verify_release(args.image.read_bytes(), args.symbols.read_text()))
    elif args.command == "storage" and args.action in ("package", "provision"):
        from tools.studio.storage import prepare,provision
        emit(prepare(args.image,args.output) if args.action=="package" else provision(args.package,args.mount,args.serial))
    elif args.command == "report":
        if not args.run_id or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-TZ" for c in args.run_id):
            raise ValueError("invalid run ID")
        emit(json.loads((ROOT / "out/runs" / args.run_id / "receipt.json").read_text()))
    elif args.command == "storage" and args.action == "theme-provision":
        from tools.studio.theme_settings import provision
        emit(provision(args.mount,args.disk_serial,args.disk_bytes,args.confirmed_build))
    return 0
