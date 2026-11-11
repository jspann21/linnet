"""Portable User and Developer firmware bundles consumed without a compiler."""
import hashlib
import json
import os
import re
from pathlib import Path
import tempfile
import struct
import uuid
import zipfile
import zlib

from tools.inspect.image import inspect, verify_release
from tools.studio.targets import ROOT, target_id, target_spec
from tools.studio import firmware_binding


OS_NAME = "firmware/linnet-os"
BOOT_NAMES = {"none": "firmware/linnet-boot-standard",
              "rockbox": "firmware/linnet-boot-multiboot"}
FILES = {OS_NAME + ".los", OS_NAME + ".json", "README.txt", "LICENSE.txt", "NOTICE.txt"} | {
    f"{name}.{extension}"
    for name in BOOT_NAMES.values() for extension in ("los", "json", "ipod")}
CLASSIC_FILES = {'bootstrap/manifest.json'} | {
    'bootstrap/' + name for name in ('monitor.bin', 'rom_transfer.bin', 'probe.bin', 'sram_bridge.bin')}
PACKAGE_README = """Linnet firmware

Select this entire ZIP in Linnet Media Manager:
iPod setup > Choose ZIP…

Do not extract the ZIP or choose individual files. Media Manager verifies your
iPod model and selects the required Linnet bootloader for your installation.

The firmware folder contains Linnet OS and two Linnet bootloader variants:
- standard and multiboot: Linnet with Apple software and verified Rockbox startup
  choices. Rockbox files can also be registered after installing Linnet.
Fresh installations use native Linnet by default, keeping a verified Rockbox
image available in the startup chooser when compatible. Reinstalls and resumes
retain the saved installation method; the existing Rockbox loader is optional.

For a fresh iPod Classic, Media Manager also installs Linnet Boot. Follow its
on-screen connection instructions; it detects the device's mode and continues
automatically. Original startup software and recovery backups are preserved.

Installation and use are at your own risk. Linnet is provided without warranty;
to the extent permitted by law, its copyright holders and contributors are not
liable for device damage, data loss, or repair and recovery costs. Read
NOTICE.txt and LICENSE.txt, and keep independent backups before changing your
device.
"""
MAX_BYTES = 64 * 1024 * 1024


def create(target, output, jobs=2, build='user'):
    from tools.studio.build import build as build_image
    target = target_id(target)
    if build not in ('user', 'dev'):
        raise ValueError('Firmware bundle build must be user or dev')
    profile = 'release' if build == 'user' else 'dev'
    output = Path(output).resolve()
    if output.exists():
        raise ValueError("firmware bundle output already exists")
    from tools.content.apple_font import check_packaged_assets
    check_packaged_assets(ROOT)
    payloads = {"README.txt": (('User build: everyday use.\n' if build == 'user' else
        'Developer build: diagnostics and Linnet Link. Media Manager binds this ZIP to your iPod.\n') +
        ('For iPod Classic 6G and 7G, including the 6.5G revision.\n' if target == 'classic6g-n25' else '') +
        '\n' + PACKAGE_README).encode("utf-8"),
        'LICENSE.txt': (ROOT / 'LICENSE').read_bytes(),
        'NOTICE.txt': (ROOT / 'NOTICE.txt').read_bytes()}
    if target_spec(target)['capabilities'].get('games',False):
        from tools.studio.game_build import build_library,library_files
        prepared=ROOT/'out/game-packages'/target
        build_library(target,prepared)
        for path in library_files(prepared,target):payloads['games/'+path.name]=path.read_bytes()
    version = None
    for kind, selector in (("os", "none"), ("boot", "none"), ("boot", "rockbox")):
        component_profile = 'release' if target == 'classic6g-n25' and kind == 'boot' and selector == 'none' else profile
        report = build_image(component_profile, target, verify=True, kind=kind, boot_selector=selector, jobs=jobs,
            device_serial=firmware_binding.TEMPLATE_SERIAL if component_profile == 'dev' else '',
            portable_serial=component_profile == 'dev')
        if kind == 'os':
            version = report['version']
        name = OS_NAME if kind == "os" else BOOT_NAMES[selector]
        payloads[name + ".los"] = (ROOT / report["artifacts"]["los"]).read_bytes()
        if kind == "boot":
            payloads[name + ".ipod"] = (ROOT / report["artifacts"]["ipod"]).read_bytes()
        # Keep the verified image contract, without build-machine paths.
        portable = {key: report[key] for key in (
            "kind", "profile", "target_id", "device_serial", "build_id", "image", "release_exclusion", "probe")}
        if 'serial_binding' in report:
            portable['serial_binding'] = report['serial_binding']
        if kind == "boot":
            portable.update(boot_selector=selector, apple_firmware_fallback=report["apple_firmware_fallback"],
                            startup_default_format=report["startup_default_format"])
        payloads[name + ".json"] = (json.dumps(portable, indent=2) + "\n").encode("utf-8")
    if target == 'classic6g-n25':
        from tools.studio.s5l_bootstrap import build as build_bootstrap, TEMPLATE_SERIAL
        with tempfile.TemporaryDirectory(prefix='classic-bootstrap-') as temporary:
            directory=Path(temporary)
            build_bootstrap(TEMPLATE_SERIAL,directory)
            for name in CLASSIC_FILES:
                payloads[name]=(directory/Path(name).name).read_bytes()
    manifest = dict(format=2, target_id=target, build=build, version=version,
                    files={name: dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                           for name, data in sorted(payloads.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
            bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
            for name, data in sorted(payloads.items()):
                bundle.writestr(name, data)
        # Exercise the consumer's checks before publishing a bundle.
        identity = dict(target_id=target, linnet_usb_serial='1111111111111111')
        load(temporary, identity, "native-preserve", verify_only=True)
        load(temporary, identity, "native-selector", verify_only=True)
        temporary.rename(output)
    finally:
        temporary.unlink(missing_ok=True)
    return dict(bundle=str(output), target_id=target, build=build,
                sha256=hashlib.sha256(output.read_bytes()).hexdigest())


def _read(path):
    path = Path(path).resolve(strict=True)
    if path.stat().st_size > MAX_BYTES:
        raise ValueError("firmware bundle exceeds its size limit")
    try:
        with zipfile.ZipFile(path) as bundle:
            entries = bundle.infolist()
            names={item.filename for item in entries}
            if (len(entries) != len(names) or 'manifest.json' not in names or
                    any(item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for item in entries) or
                    any(item.file_size > MAX_BYTES for item in entries) or
                    sum(item.file_size for item in entries) > MAX_BYTES):
                raise ValueError('Firmware ZIP has unexpected files or sizes')
            manifest = json.loads(bundle.read('manifest.json'))
            if (not isinstance(manifest, dict) or manifest.get('format') != 2 or
                    manifest.get('build') not in ('user', 'dev') or
                    not isinstance(manifest.get('version'), str) or
                    not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,23}', manifest['version'])):
                raise ValueError('Choose a current Linnet User or Developer firmware ZIP')
            target = manifest.get('target_id')
            target_spec(target)
            game_files={name for name in names if name.startswith('games/')}
            from tools.content.game_package import FILE_PATTERN,CATALOGUE_MAX
            notice_files={'games/LICENSE.txt','games/NOTICE.txt'}
            if (len(game_files)>CATALOGUE_MAX+3 or any(name not in notice_files|{'games/library.json'} and
                    not FILE_PATTERN.fullmatch(name.removeprefix('games/')) for name in game_files)
                    or game_files and 'games/library.json' not in game_files):
                raise ValueError('Invalid prepared games in firmware bundle')
            files=FILES | (CLASSIC_FILES if target=='classic6g-n25' else set()) | game_files
            if (len(entries) != len(files) + 1 or
                    names != files | {"manifest.json"} or
                    any(item.file_size > MAX_BYTES for item in entries) or
                    sum(item.file_size for item in entries) > MAX_BYTES):
                raise ValueError("firmware bundle has unexpected files or sizes")
            if not isinstance(manifest.get("files"), dict) or set(manifest["files"]) != files:
                raise ValueError('Firmware ZIP manifest differs from its contents')
            payloads = {name: bundle.read(name) for name in files}
    except (zipfile.BadZipFile, KeyError, TypeError, UnicodeError, RuntimeError, EOFError, zlib.error) as error:
        raise ValueError("invalid firmware bundle") from error
    for name, data in payloads.items():
        if manifest["files"][name] != dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()):
            raise ValueError("firmware bundle checksum mismatch: " + name)
    for kind, name in (('os', OS_NAME), *(('boot', name) for name in BOOT_NAMES.values())):
        data = payloads[name + '.los']
        image = inspect(data)
        profile = ('release' if manifest['build'] == 'user' or
                   target == 'classic6g-n25' and name == BOOT_NAMES['none'] else 'dev')
        report = json.loads(payloads[name + '.json'])
        serial = '' if profile == 'release' else firmware_binding.TEMPLATE_SERIAL
        if (image['target_id'] != target or image['profile'] != profile or
                not isinstance(report, dict) or report.get('image') != image or
                report.get('kind') != kind or report.get('probe') != 'frame' or
                report.get('target_id') != target or report.get('profile') != profile or
                report.get('build_id') != image['build_id'] or report.get('device_serial') != serial):
            raise ValueError('Firmware ZIP image or build report does not match its build type')
        if profile == 'release':
            verify_release(data, '')
            if not isinstance(report.get('release_exclusion'), dict) or report['release_exclusion'].get('passed') is not True:
                raise ValueError('User firmware needs release exclusion verification')
        else:
            firmware_binding.validate_binding(data, report, serial)
        if kind == 'boot':
            from tools.studio.bootstrap import ipod_payload
            spec = target_spec(target)['installer']
            wrapped, _model = ipod_payload(payloads[name + '.ipod'], spec['wrapper_model'].encode('ascii'),
                                           spec['wrapper_checksum_seed'])
            selector = next(key for key, value in BOOT_NAMES.items() if value == name)
            if (wrapped != data[128:] or report.get('boot_selector') != selector or
                    report.get('apple_firmware_fallback') is not True or report.get('startup_default_format') != 1):
                raise ValueError('Firmware ZIP Boot component does not match its startup contract')
    return manifest, payloads, game_files


def describe(path):
    """Read the verified target and build type without preparing installation files."""
    manifest, _payloads, _games = _read(path)
    return {key: manifest[key] for key in ('target_id', 'build', 'version')}


def selected_build(path, build=None):
    if path is None:
        return build or 'user'
    selected = describe(path)['build']
    if build is not None and build != selected:
        raise ValueError('Firmware ZIP build type differs from the selected build')
    return selected


def load(path, identity, installation_method, *, verify_only=False, build=None):
    """Validate and bind a ZIP, retaining private installation files when needed."""
    from tools.studio.installer import INSTALLATION_METHODS, _validate_artifacts
    method = 'native-preserve' if installation_method == 'native-volume' else installation_method
    if method not in INSTALLATION_METHODS:
        raise ValueError('Unsupported firmware installation method')
    manifest, payloads, game_files = _read(path)
    if manifest['target_id'] != identity['target_id']:
        raise ValueError('Firmware ZIP is for a different iPod model')
    if build is not None and build != manifest['build']:
        raise ValueError('Firmware ZIP build type differs from the selected build')
    build = manifest['build']
    if build == 'dev':
        spec = target_spec(identity['target_id'])['installer']
        for name in (OS_NAME, *BOOT_NAMES.values()):
            data = payloads[name + '.los']
            if inspect(data)['profile'] != 'dev':
                continue
            bound, report = firmware_binding.bind(data, json.loads(payloads[name + '.json']),
                                                   identity['linnet_usb_serial'])
            payloads[name + '.los'] = bound
            payloads[name + '.json'] = (json.dumps(report, indent=2) + '\n').encode('utf-8')
            if name != OS_NAME:
                payloads[name + '.ipod'] = struct.pack('>I4s',
                    (spec['wrapper_checksum_seed'] + sum(bound[128:])) & 0xffffffff,
                    spec['wrapper_model'].encode('ascii')) + bound[128:]
    files = set(payloads)
    selector = INSTALLATION_METHODS[method]["boot_selector"]
    boot_name = BOOT_NAMES[selector]
    parent = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".cache")) / "Linnet/Firmware"
    parent.mkdir(parents=True, exist_ok=True)
    # Keep each selection private to its installation, including concurrent runs.
    with tempfile.TemporaryDirectory(prefix="verify-", dir=parent) as temporary:
        staging = Path(temporary)
        for name, data in payloads.items():
            (staging / name).parent.mkdir(parents=True, exist_ok=True)
            (staging / name).write_bytes(data)
        if game_files:
            from tools.studio.game_build import library
            prepared=library(staging/'games',identity['target_id'])
            if game_files!={'games/library.json','games/LICENSE.txt','games/NOTICE.txt'}|{'games/'+p.name for p in prepared.values()}:
                raise ValueError('Firmware game library contains unlisted files')
        def artifacts(directory):
            return dict(os_image=str(directory / (OS_NAME + ".los")), os_report=str(directory / (OS_NAME + ".json")),
                        boot_image=str(directory / (boot_name + ".los")),
                        boot_report=str(directory / (boot_name + ".json")),
                        boot_wrapper=str(directory / (boot_name + ".ipod")),
                        build=build, os_profile=inspect(payloads[OS_NAME + '.los'])['profile'],
                        boot_profile=inspect(payloads[boot_name + '.los'])['profile'],
                        os_build_id=inspect(payloads[OS_NAME + ".los"])["build_id"],
                        boot_build_id=inspect(payloads[boot_name + ".los"])["build_id"],
                        installation_method=method, boot_selector=selector,
                        game_library=str(directory/'games') if game_files else None)
        _validate_artifacts(identity, artifacts(staging))
        if verify_only:
            return None
        destination = parent / uuid.uuid4().hex
        destination.mkdir()
        for name in files:
            (destination / name).parent.mkdir(parents=True, exist_ok=True)
            (staging / name).replace(destination / name)
    result=artifacts(destination)
    if CLASSIC_FILES<=files:result['bootstrap_template']=str(destination/'bootstrap')
    return result
