"""Target manifest discovery shared by build, image, and device tooling."""
from pathlib import Path, PurePosixPath
from types import MappingProxyType
import re
import tomllib

ROOT = Path(__file__).resolve().parents[2]
TARGETS_ROOT = ROOT / "targets"
REQUIRED_KEYS = {
    "id", "name", "aliases", "image_target", "board_id", "model_alias",
    "soc", "cpu", "architecture", "endianness", "sdram_bytes",
    "os_capacity_bytes", "resident_limit_bytes", "stage_address", "stage_bytes",
    "recovery_snapshot_address", "recovery_snapshot_bytes",
    "storage_max_read_sectors", "lcd_width",
    "lcd_height", "lcd_format", "capabilities", "media_photo_width",
    "media_photo_height", "media_video_width", "media_video_height",
    "media_video_fps", "media_video_color", "media_video_extensions", "media_video_backend", "media_art_side",
    "cop_display", "entry_address", "load_address", "mailbox_address",
    "mailbox_bytes", "retail", "installer", "resources",
}
OPTIONAL_KEYS = {"ui", "hal_family", "audio_codec", "idle_clock_hz", "active_clock_hz", "native_video", "game_presentation", "presentation_policy", "video_start_audio_frames",
                 "sdram_address", "build_enabled", "bringup_blocker", "presentation_profile", "runtime_policy", "native_storage", "h264_decoder", "mpeg4_decoder", "video_catalog_cache", "coverflow_cache", "boot_size_optimization"}
RUNTIME_POLICIES = {"display_pipeline", "audio_render_reserve", "music_browse_index", "extended_sound", "resident_monitor"}
PRESENTATION_POLICIES = {"music_hint_menu_back", "battery_theme_monochrome"}
CAPABILITIES = {"photos", "video", "files", "demo", "themes", "cover_flow", "games"}
GAME_PRESENTATIONS = {"photo220": (220, 176), "color320": (320, 240)}
LCD_FORMATS = {"rgb565-swapped": 1, "gray2-horizontal": 2, "rgb565": 3}

# Source manifests may omit these fields. Keep their effective values in one
# place for firmware builds, content compilation, and host media preparation.
CAPABILITY_DEFAULTS = MappingProxyType({
    'photos': True,
    'video': True,
    'files': True,
    'demo': True,
    'themes': True,
    'cover_flow': False,
    'games': False,
})
TARGET_DEFAULTS = MappingProxyType({
    'audio_codec': 'wm8975',
    'idle_clock_hz': 0,
    'active_clock_hz': 80000000,
    'h264_decoder': 'software',
    'mpeg4_decoder': 'none',
    'video_start_audio_frames': 16384,
    'build_enabled': True,
    'boot_size_optimization': False,
})


def _frozen(value):
    if isinstance(value, dict):
        return MappingProxyType({key: _frozen(item) for key, item in value.items()})
    if isinstance(value, list):
        return tuple(_frozen(item) for item in value)
    return value


def resolve_target(source):
    """Return an immutable view with documented optional defaults applied."""
    resolved = dict(TARGET_DEFAULTS)
    resolved['hal_family'] = source['soc']
    resolved.update(source)
    resolved['capabilities'] = dict(CAPABILITY_DEFAULTS, **source['capabilities'])
    return _frozen(resolved)


def _valid_ascii_list(value):
    return (isinstance(value, list) and
            all(isinstance(item, str) and item and item.isascii() for item in value))


def conventional_sources(target_root: Path, spec: dict) -> dict[str, Path]:
    """Resolve shared source candidates for builds and target reports."""
    family = spec.get('hal_family', spec['soc'])
    hal = ROOT / 'hal' / family
    codec = spec.get('audio_codec', 'wm8975')
    sources = {
        'hardware': target_root / 'hardware.h',
        'startup': hal / 'startup.S',
        'entry': hal / 'entry.S',
        'linker': hal / 'memory.ld',
        'platform': hal / 'platform.c',
        'core': hal / 'core.c',
        'input': hal / 'input.c',
        'ata': hal / 'ata.c',
        'persistent_enter': hal / 'persistent_enter.S',
        'lcd': target_root / 'lcd.c',
        'display': target_root / 'display.c' if (target_root / 'display.c').is_file() else hal / 'display.c',
        'device': target_root / 'device.c' if (target_root / 'device.c').is_file() else target_root / 'board.c',
        'power': target_root / 'power.c' if (target_root / 'power.c').is_file() else hal / 'power.c',
        'codec': ROOT / 'hal' / 'codecs' / (codec + '.c'),
    }
    backend = spec['media_video_backend']
    if backend == 'ipvf':
        sources['video'] = ROOT / 'services' / 'video' / 'ipvf' / 'player.c'
    elif backend == 'native-mp4':
        sources['video'] = ROOT / 'services' / 'video' / 'native' / 'player.c'
    elif backend == 'bcm-mp4':
        sources['video'] = target_root / 'video.c'
    return sources


def _target_sources(path, spec):
    """Resolve the conventional source layout for one target."""
    resolved = resolve_target(spec)
    target_root = path.parent
    if not re.fullmatch(r"[a-z][a-z0-9-]*", resolved["hal_family"]):
        raise RuntimeError(f"invalid HAL family: {path}")
    codec = resolved["audio_codec"]
    if not re.fullmatch(r"[a-z][a-z0-9-]*", codec):
        raise RuntimeError(f"invalid audio codec: {path}")
    candidates = conventional_sources(target_root, resolved)
    sources = {name: candidates[name] for name in (
        "hardware", "startup", "linker", "platform", "lcd",
        "display", "device", "power", "codec",
    )}
    if spec["media_video_backend"] not in ("ipvf", "native-mp4", "bcm-mp4", "none"):
        raise RuntimeError(f'unsupported video backend: {spec["media_video_backend"]}')
    if "video" in candidates:
        sources["video"] = candidates["video"]
    coprocessor = target_root / "coprocessor.c"
    if coprocessor.is_file():
        sources["coprocessor"] = coprocessor
    display_assembly = target_root / "display.S"
    if display_assembly.is_file():
        sources["display_assembly"] = display_assembly
    if any(not source.is_file() for source in sources.values()):
        missing = ", ".join(
            source.relative_to(ROOT).as_posix()
            for source in sources.values()
            if not source.is_file()
        )
        raise RuntimeError(f"missing target source for {path}: {missing}")
    return {
        name: source.relative_to(ROOT).as_posix()
        for name, source in sources.items()
    }


def _validate_ui(path, spec):
    ui = spec.get("ui")
    if ui is None:
        return
    if not isinstance(ui, dict) or set(ui) != {"allowed_themes", "default_theme"}:
        raise RuntimeError(f"invalid UI profile: {path}")
    allowed = ui["allowed_themes"]
    default = ui["default_theme"]
    if not isinstance(allowed, list) or not allowed:
        raise RuntimeError(f"invalid UI profile: {path}")
    if any(not isinstance(theme, str) or not theme for theme in allowed):
        raise RuntimeError(f"invalid UI profile: {path}")
    if len(allowed) != len(set(allowed)):
        raise RuntimeError(f"invalid UI profile: {path}")
    if not isinstance(default, str) or default not in allowed:
        raise RuntimeError(f"invalid UI profile: {path}")


def _require(path, spec, field, valid, rule, *, value=None):
    if not valid:
        rejected = spec.get(field) if value is None else value
        raise RuntimeError(f"{spec.get('id', path.parent.name)}: {field}={repr(rejected)[:160]} must {rule} ({path})")


def _validate_identity(path, spec):
    _require(path, spec, 'name', isinstance(spec['name'], str) and bool(spec['name']), 'be a nonempty string')
    aliases = spec['aliases']
    _require(path, spec, 'aliases', isinstance(aliases, list) and
             all(isinstance(alias, str) and re.fullmatch(r'[a-z0-9][a-z0-9-]*', alias) for alias in aliases) and
             len(aliases) == len(set(aliases)), 'contain distinct lowercase target aliases')
    for field, pattern in (('board_id', r'[a-z0-9][a-z0-9-]*'),
                           ('model_alias', r'[a-z0-9][a-z0-9-]*'),
                           ('soc', r'[a-z][a-z0-9-]*'),
                           ('cpu', r'[a-z0-9][a-z0-9-]*')):
        value = spec[field]
        _require(path, spec, field, isinstance(value, str) and bool(re.fullmatch(pattern, value)),
                 f'match {pattern}')
    for field, allowed in (('lcd_format', LCD_FORMATS),
                           ('architecture', {'armv4t', 'armv5te', 'armv6'}),
                           ('endianness', {'little', 'big'})):
        value = spec[field]
        _require(path, spec, field, isinstance(value, str) and value in allowed,
                 f'be one of {sorted(allowed)}')
    for field in ('lcd_width', 'lcd_height', 'media_photo_width', 'media_photo_height',
                  'media_video_width', 'media_video_height'):
        value = spec[field]
        _require(path, spec, field, type(value) is int and 1 <= value <= 1024,
                 'be an integer from 1 through 1024')
    _require(path, spec, 'lcd_width/lcd_height',
             spec['lcd_width'] * spec['lcd_height'] <= 131072,
             'fit within 131072 pixels', value=(spec['lcd_width'], spec['lcd_height']))
    for kind in ('photo', 'video'):
        for axis in ('width', 'height'):
            field = f'media_{kind}_{axis}'
            _require(path, spec, field, spec[field] <= spec[f'lcd_{axis}'],
                     f'not exceed lcd_{axis}={spec[f"lcd_{axis}"]}')
    ram_base = spec.get('sdram_address', 0)
    _require(path, spec, 'sdram_address', type(ram_base) is int and ram_base >= 0 and ram_base % 4096 == 0,
             'be a nonnegative 4096-byte-aligned integer', value=ram_base)
    ram_bytes = spec['sdram_bytes']
    _require(path, spec, 'sdram_bytes', type(ram_bytes) is int and ram_bytes >= 8 * 1024 * 1024,
             'be at least 8 MiB')
    _require(path, spec, 'sdram_address/sdram_bytes', ram_base + ram_bytes <= 0x100000000,
             'fit within the 32-bit address space', value=(ram_base, ram_bytes))
    for field in ('entry_address', 'load_address', 'stage_address',
                  'recovery_snapshot_address', 'mailbox_address'):
        value = spec[field]
        _require(path, spec, field, type(value) is int and 0 <= value <= 0xffffffff,
                 'be a 32-bit address')
    for field in ('os_capacity_bytes', 'stage_bytes', 'recovery_snapshot_bytes', 'mailbox_bytes'):
        value = spec[field]
        _require(path, spec, field, type(value) is int and value >= 4096,
                 'reserve at least 4096 bytes')
    _require(path, spec, 'stage_bytes', spec['stage_bytes'] >= spec['os_capacity_bytes'],
             f'cover os_capacity_bytes={spec["os_capacity_bytes"]}')
    _require(path, spec, 'os_capacity_bytes', spec['os_capacity_bytes'] % 512 == 0,
             'be a multiple of 512 bytes')
    regions = [(name, spec[f'{name}_address' if name != 'load' else 'load_address'],
                spec[f'{name}_bytes' if name != 'load' else 'os_capacity_bytes'])
               for name in ('load', 'stage', 'recovery_snapshot', 'mailbox')]
    for name, address, size in regions:
        _require(path, spec, f'{name}_region', ram_base <= address and address + size <= ram_base + ram_bytes,
                 'fit within SDRAM', value=(address, size))
    for index, (name, address, size) in enumerate(regions):
        for other_name, other_address, other_size in regions[index + 1:]:
            _require(path, spec, f'{name}/{other_name}_regions',
                     not (address < other_address + other_size and other_address < address + size),
                     'not overlap', value=((address, size), (other_address, other_size)))
    _require(path, spec, 'mailbox_address', spec['mailbox_address'] >= 8192,
             'leave at least 8192 bytes before the mailbox')
    _require(path, spec, 'recovery_snapshot_address/recovery_snapshot_bytes',
             spec['recovery_snapshot_address'] + spec['recovery_snapshot_bytes'] +
             spec['lcd_width'] * spec['lcd_height'] * 2 <= spec['mailbox_address'] - 8192,
             'leave one framebuffer and 8192 bytes before the mailbox',
             value=(spec['recovery_snapshot_address'], spec['recovery_snapshot_bytes']))
    _require(path, spec, 'entry_address',
             spec['load_address'] <= spec['entry_address'] < spec['load_address'] + spec['os_capacity_bytes'],
             'fall within the OS load region')
    sectors = spec['storage_max_read_sectors']
    _require(path, spec, 'storage_max_read_sectors', type(sectors) is int and 8 <= sectors <= 1024,
             'be an integer from 8 through 1024')
    _require(path, spec, 'cop_display', type(spec['cop_display']) is bool, 'be a boolean')


def _validate_resources(path, spec):
    r=spec["resources"]
    limit=spec["resident_limit_bytes"]
    if type(limit) is not int or not 1024 * 1024 <= limit <= spec["os_capacity_bytes"]:
        raise RuntimeError(f"invalid resident memory limit: {path}")
    if type(spec.get("boot_size_optimization", False)) is not bool:
        raise RuntimeError(f"invalid Boot optimization policy: {path}")
    cache=spec.get("coverflow_cache")
    coverflow=spec["capabilities"].get("cover_flow", False)
    if cache not in (("compact", "expanded") if coverflow else (None,)) or (cache == "expanded" and
            (spec["soc"] != "s5l8702" or spec["sdram_bytes"] < 64 * 1024 * 1024 or
             not coverflow)):
        raise RuntimeError(f"invalid Cover Flow cache policy: {path}")
    if not isinstance(r,dict) or set(r)!={"music_titles_address","music_titles_bytes","coverflow_catalog_slots"}:
        raise RuntimeError(f"invalid resource budget: {path}")
    start,size,slots=(r[k] for k in ("music_titles_address","music_titles_bytes","coverflow_catalog_slots"))
    if (any(type(v) is not int for v in (start,size,slots)) or start%32 or size<840896 or
        not 16<=slots<=(4096 if cache=="expanded" else 2048) or
        start < spec["recovery_snapshot_address"]+spec["recovery_snapshot_bytes"]+spec["lcd_width"]*spec["lcd_height"]*2 or
        start+size>spec["mailbox_address"]-8192):
        raise RuntimeError(f"resource budget overlaps recovery or exceeds limits: {path}")


def _validate_profiles(path, spec):
    native=spec.get("native_storage")
    if native is not None and (not isinstance(native,dict) or
            set(native)!={"usb_vid","usb_pid","model","friendly_name","sector_bytes"} or
            any(type(native.get(key)) is not int or not 1<=native[key]<=0xffff for key in ("usb_vid","usb_pid")) or
            any(not isinstance(native.get(key),str) or not re.fullmatch(r"[A-Za-z0-9 _-]{1,64}",native[key]) for key in ("model","friendly_name")) or
            not isinstance(native.get("sector_bytes"),list) or not native["sector_bytes"] or
            any(type(n) is not int or n not in (512,2048,4096) for n in native["sector_bytes"])):
        raise RuntimeError(f"invalid native storage profile: {path}")
    retail = spec["retail"]
    if (not isinstance(retail, dict) or
            type(retail.get("usb_vid")) is not int or not 0 <= retail["usb_vid"] <= 0xffff or
            type(retail.get("usb_pid")) is not int or not 0 <= retail["usb_pid"] <= 0xffff or
            not _valid_ascii_list(retail.get("board_names")) or
            not _valid_ascii_list(retail.get("model_numbers")) or
            not _valid_ascii_list(retail.get("regulatory_models")) or
            type(retail.get("usb_identity_only", False)) is not bool or
            not (retail["board_names"] or retail["model_numbers"] or retail.get("usb_identity_only")) or
            not isinstance(retail.get("sector_bytes", [512]), list) or
            not retail.get("sector_bytes", [512]) or
            any(type(n) is not int or n not in (512, 2048, 4096) for n in retail.get("sector_bytes", [512]))):
        raise RuntimeError(f"invalid retail detection profile: {path}")
    regulatory_map = retail.get("regulatory_model_by_model", {})
    if (not isinstance(regulatory_map, dict) or any(
            model not in retail["model_numbers"] or regulatory not in retail["regulatory_models"]
            for model, regulatory in regulatory_map.items())):
        raise RuntimeError(f"invalid retail model mapping: {path}")
    signature = retail.get("firmware_signature")
    if signature is not None and (
            not isinstance(signature, dict) or set(signature) != {"offset", "ascii"} or
            type(signature["offset"]) is not int or signature["offset"] < 0 or
            not isinstance(signature["ascii"], str) or
            not re.fullmatch(r"[\x20-\x7e]{1,64}", signature["ascii"]) or
            signature["offset"] + len(signature["ascii"]) > spec["stage_bytes"]):
        raise RuntimeError(f"invalid Apple firmware signature: {path}")
    install = spec["installer"]
    loader = PurePosixPath(str(install.get("file_loader", ""))) if isinstance(install, dict) else PurePosixPath()
    required = {"enabled", "boot_layout", "file_loader", "wrapper_model", "wrapper_checksum_seed"}
    directory = isinstance(install, dict) and install.get("boot_layout") == "ipod-directory"
    if directory:
        required |= {"firmware_header_magic", "firmware_header_field", "firmware_partition_types",
                     "media_partition_types", "firmware_entry_type", "max_firmware_bytes"}
    if (not isinstance(install, dict) or set(install) != required or type(install["enabled"]) is not bool or
            not re.fullmatch(r"[a-z][a-z0-9-]*", install["boot_layout"]) or
            (install["enabled"] and install["boot_layout"] != "ipod-directory") or
            loader.is_absolute() or ".." in loader.parts or len(loader.parts) < 2 or
            not re.fullmatch(r"[\x20-\x7e]{4}", install["wrapper_model"]) or
            type(install["wrapper_checksum_seed"]) is not int or not 0 <= install["wrapper_checksum_seed"] <= 0xffffffff):
        raise RuntimeError(f"invalid installer profile: {path}")
    if not directory:
        return
    if (not re.fullmatch(r"[0-9a-fA-F]{8}", install["firmware_header_magic"]) or
            type(install["firmware_header_field"]) is not int or not 0 <= install["firmware_header_field"] <= 0xffff or
            any(not isinstance(values, list) or not values or len(values) != len(set(values)) or
                any(type(value) is not int or not 0 <= value <= 0xff for value in values)
                for values in (install["firmware_partition_types"], install["media_partition_types"])) or
            set(install["firmware_partition_types"]) & set(install["media_partition_types"]) or
            not re.fullmatch(r"[\x20-\x7e]{4}", install["firmware_entry_type"]) or
            type(install["max_firmware_bytes"]) is not int or
            not 512 <= install["max_firmware_bytes"] <= 1024 * 1024 * 1024 or
            install["max_firmware_bytes"] % 512):
        raise RuntimeError(f"invalid installer profile: {path}")


def _validate_media(path, spec):
    _require(path, spec, 'media_art_side', type(spec['media_art_side']) is int and
             spec['media_art_side'] in (64, 128, 160) and
             (spec['lcd_format'] != 'gray2-horizontal' or spec['media_art_side'] == 64),
             'be 64, 128 or 160, and 64 for gray2-horizontal displays')
    backend = spec.get("media_video_backend")
    decoder = spec.get("h264_decoder", "software")
    _require(path, spec, 'h264_decoder', decoder in ('software', 's5l8702-vpu') and
             (decoder != 's5l8702-vpu' or (backend == 'native-mp4' and spec['soc'] == 's5l8702')),
             'be software, or s5l8702-vpu with native-mp4 on s5l8702', value=decoder)
    mpeg4_decoder = spec.get('mpeg4_decoder', 'none')
    _require(path, spec, 'mpeg4_decoder',
             mpeg4_decoder in ('none', 'pvmp4', 's5l8702-vpu') and
             (mpeg4_decoder != 's5l8702-vpu' or spec['soc'] == 's5l8702') and
             (mpeg4_decoder == 'none' or backend == 'native-mp4'),
             'be none, or a decoder compatible with native-mp4 and its SoC', value=mpeg4_decoder)
    enabled = spec["capabilities"].get("video", True)
    _require(path, spec, 'media_video_backend',
             backend in ('none', 'ipvf', 'bcm-mp4', 'native-mp4') and (backend != 'none') == enabled,
             f'match capabilities.video={enabled} and use a supported backend', value=backend)
    cache = spec.get("video_catalog_cache")
    _require(path, spec, 'video_catalog_cache',
             cache in ((None,) if backend == 'none' else ('paged', 'resident')),
             'be absent without video, or paged/resident with video', value=cache)
    expected = {"none": {".ipvf"}, "ipvf": {".ipvf"}, "bcm-mp4": {".mp4", ".m4v"}, "native-mp4": {".mp4", ".m4v", ".mov"}}[backend]
    _require(path, spec, 'media_video_extensions', set(spec['media_video_extensions']) == expected,
             f'match {backend} extensions {sorted(expected)}')
    native = spec.get("native_video", {})
    fields = {"h264_width", "h264_height", "h264_level", "mpeg4_width", "mpeg4_height",
              "max_fps", "video_kbps", "max_video_kbps", "buffer_kbits", "audio_kbps", "sample_bytes"}
    if backend in ("bcm-mp4", "native-mp4"):
        _require(path, spec, 'native_video', isinstance(native, dict), 'be a table', value=native)
        optional = {'normalize', 'h264_deblock'}
        _require(path, spec, 'native_video fields', fields <= set(native) <= fields | optional,
                 f'contain {sorted(fields)} and optional {sorted(optional)}', value=sorted(native))
        for field in fields:
            value = native[field]
            _require(path, spec, f'native_video.{field}', type(value) is int and value > 0,
                     'be a positive integer', value=value)
        for field in optional & set(native):
            _require(path, spec, f'native_video.{field}', type(native[field]) is bool,
                     'be a boolean', value=native[field])
        _require(path, spec, 'native_video.h264_deblock',
                 native.get('h264_deblock', True) or native.get('normalize', False),
                 'require normalization when disabled', value=native.get('h264_deblock', True))
        _require(path, spec, 'native_video.sample_bytes', native['sample_bytes'] in (65536, 262144),
                 'be 65536 or 262144', value=native['sample_bytes'])
        for axis in ('width', 'height'):
            field = f'media_video_{axis}'
            _require(path, spec, field, spec[field] <= native[f'h264_{axis}'],
                     f'not exceed native_video.h264_{axis}={native[f"h264_{axis}"]}')
        _require(path, spec, 'media_video_fps', spec['media_video_fps'] <= native['max_fps'],
                 f'not exceed native_video.max_fps={native["max_fps"]}')
        _require(path, spec, 'native_video.video_kbps', native['video_kbps'] <= native['max_video_kbps'],
                 f'not exceed native_video.max_video_kbps={native["max_video_kbps"]}',
                 value=native['video_kbps'])
    elif native:
        raise RuntimeError(f"unexpected native video profile: {path}")


def load_targets():
    targets = {}
    aliases = {}
    image_targets = {}
    for path in sorted(TARGETS_ROOT.glob("*/target.toml")):
        spec = tomllib.loads(path.read_text(encoding="utf-8"))
        target = spec.get("id")
        _require(path, spec, 'id', isinstance(target, str) and
                 bool(re.fullmatch(r'[a-z][a-z0-9-]*', target)) and path.parent.name == target,
                 f'match the lowercase directory name {path.parent.name!r}')
        missing = REQUIRED_KEYS - set(spec)
        unknown = set(spec) - REQUIRED_KEYS - OPTIONAL_KEYS
        _require(path, spec, 'manifest fields', not missing and not unknown,
                 f'include required fields and exclude unknown fields (missing={sorted(missing)}, unknown={sorted(unknown)})',
                 value=sorted(spec))
        idle_clock = spec.get('idle_clock_hz', 0)
        _require(path, spec, 'idle_clock_hz', type(idle_clock) is int and idle_clock in (0, 30000000),
                 'be 0 or 30000000', value=idle_clock)
        active_clock = spec.get('active_clock_hz', 80000000)
        _require(path, spec, 'active_clock_hz', type(active_clock) is int and
                 1000000 <= active_clock <= 500000000,
                 'be an integer from 1000000 through 500000000', value=active_clock)
        _validate_identity(path, spec)
        spec["target_define"] = "LINNET_TARGET_" + target.upper().replace("-", "_")
        number = spec["image_target"]
        _require(path, spec, 'image_target', type(number) is int and 0 <= number <= 0xffffffff,
                 'be a unique 32-bit unsigned integer')
        _require(path, spec, 'image_target', number not in image_targets,
                 f'differ from {image_targets.get(number)}')
        _require(path, spec, 'aliases', target not in spec['aliases'], 'not contain the target ID')
        capabilities = spec['capabilities']
        _require(path, spec, 'capabilities', isinstance(capabilities, dict), 'be a table')
        for key, value in capabilities.items():
            _require(path, spec, f'capabilities.{key}', key in CAPABILITIES and type(value) is bool,
                     f'be a boolean for a supported capability in {sorted(CAPABILITIES)}', value=value)
        game_presentation = spec.get('game_presentation')
        _require(path, spec, 'game_presentation',
                 (isinstance(game_presentation, str) and game_presentation in GAME_PRESENTATIONS and
                  (spec['lcd_width'], spec['lcd_height']) == GAME_PRESENTATIONS[game_presentation])
                 if capabilities.get('games', False) else game_presentation is None,
                 f'be absent without games, or name a supported layout matching the LCD: {GAME_PRESENTATIONS}',
                 value=game_presentation)
        fps = spec['media_video_fps']
        _require(path, spec, 'media_video_fps', type(fps) is int and 4 <= fps <= 30,
                 'be an integer from 4 through 30')
        _require(path, spec, 'media_video_color', spec['media_video_color'] in ('rgb565', 'gray4'),
                 "be 'rgb565' or 'gray4'")
        extensions = spec['media_video_extensions']
        _require(path, spec, 'media_video_extensions',
                 _valid_ascii_list(extensions) and bool(extensions) and
                 len(set(extensions)) == len(extensions) and
                 all(re.fullmatch(r'\.[a-z0-9]+', extension) for extension in extensions),
                 'contain distinct lowercase file extensions')
        if capabilities.get('video', True):
            required_bytes = spec['media_video_width'] * spec['media_video_height'] * 2 + 16384
            _require(path, spec, 'storage_max_read_sectors/media_video_width/media_video_height',
                     spec['storage_max_read_sectors'] * 512 >= required_bytes,
                     f'provide a read window of at least {required_bytes} bytes',
                     value=(spec['storage_max_read_sectors'], spec['media_video_width'],
                            spec['media_video_height']))
        _validate_media(path, spec)
        _validate_resources(path, spec)
        _validate_profiles(path, spec)
        if "native_storage" in spec and any(
                (previous.get("native_storage",{}).get("usb_vid"),previous.get("native_storage",{}).get("usb_pid")) ==
                (spec["native_storage"]["usb_vid"],spec["native_storage"]["usb_pid"]) for previous in targets.values()):
            raise RuntimeError(f"duplicate native storage USB identity: {path}")
        _validate_ui(path, spec)
        policy = spec.get("runtime_policy", {})
        if (not isinstance(policy, dict) or set(policy) - RUNTIME_POLICIES or
                any(type(value) is not bool for value in policy.values())):
            raise RuntimeError(f"invalid runtime policy: {path}")
        presentation_policy = spec.get("presentation_policy", {})
        if (not isinstance(presentation_policy, dict) or
                set(presentation_policy) - PRESENTATION_POLICIES or
                any(type(value) is not bool for value in presentation_policy.values())):
            raise RuntimeError(f"invalid presentation policy: {path}")
        audio_frames = spec.get("video_start_audio_frames", 16384)
        if type(audio_frames) is not int or not 1024 <= audio_frames <= 32768:
            raise RuntimeError(f"invalid video startup audio frame count: {path}")
        names = [target, *spec.get("aliases", [])]
        duplicate = next((name for name in names if name in aliases), None)
        _require(path, spec, 'aliases', duplicate is None,
                 f'not reuse {duplicate!r} from {aliases.get(duplicate)}', value=duplicate)
        enabled = spec.get("build_enabled", True)
        if type(enabled) is not bool or (not enabled and (
                spec["installer"]["enabled"] or not isinstance(spec.get("bringup_blocker"), str)
                or not spec["bringup_blocker"].strip())):
            raise RuntimeError(f"invalid build availability: {path}")
        profile = spec.get("presentation_profile")
        if profile is not None and (not isinstance(profile, str) or
                not re.fullmatch(r"[a-z][a-z0-9-]*", profile) or
                not (ROOT / "ui/profiles" / profile / "profile.toml").is_file()):
            raise RuntimeError(f"invalid presentation profile: {path}")
        spec["sources"] = _target_sources(path, spec) if enabled else {}
        targets[target] = spec
        image_targets[number] = target
        aliases.update((name, target) for name in names)
    if not targets:
        raise RuntimeError("no target manifests found")
    return targets, aliases, image_targets


TARGETS, ALIASES, TARGET_IDS_BY_NUMBER = load_targets()
TARGET_NUMBERS = {target: number for number, target in TARGET_IDS_BY_NUMBER.items()}


def target_id(value):
    if value not in ALIASES:
        raise ValueError(f"unsupported target: {value}")
    return ALIASES[value]


def target_spec(value):
    return TARGETS[target_id(value)]


def resolved_target_spec(value):
    return resolve_target(target_spec(value))


def presentation_defines(spec):
    """C definitions shared by firmware and native UI simulation."""
    spec = resolve_target(spec)
    policy = spec.get('presentation_policy', {})
    return {
        spec['target_define']: 1,
        'LINNET_UI_WIDTH': f'{spec["lcd_width"]}u',
        'LINNET_UI_HEIGHT': f'{spec["lcd_height"]}u',
        'LINNET_HAS_GAMES': int(spec['capabilities']['games']),
        'LINNET_GAME_PRESENTATION_PHOTO220': int(spec.get('game_presentation') == 'photo220'),
        'LINNET_GAME_PRESENTATION_COLOR320': int(spec.get('game_presentation') == 'color320'),
        'LINNET_MUSIC_HINT_MENU_BACK': int(policy.get('music_hint_menu_back', False)),
        'LINNET_BATTERY_THEME_MONOCHROME': int(policy.get('battery_theme_monochrome', True)),
    }


def media_profile(value):
    """Resolve Media Manager capabilities from the same manifest as firmware."""
    spec = resolved_target_spec(value)
    return dict(photo_enabled=spec["capabilities"]["photos"],
                video_enabled=spec["capabilities"]["video"],
                video_backend=spec["media_video_backend"],
                video_codecs=(["h264", "mpeg4"] if spec["media_video_backend"] == "bcm-mp4" or spec["mpeg4_decoder"] in ("pvmp4", "s5l8702-vpu") else ["h264"] if spec["media_video_backend"] == "native-mp4" else []),
                video_extensions=list(spec["media_video_extensions"]),
                video_fps=spec["media_video_fps"], video_color=spec["media_video_color"],
                photo_width=spec["media_photo_width"], photo_height=spec["media_photo_height"],
                video_width=spec["media_video_width"], video_height=spec["media_video_height"],
                max_read_sectors=spec["storage_max_read_sectors"],
                native_video=dict(spec.get("native_video", {})))
