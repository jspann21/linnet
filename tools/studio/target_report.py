"""Read-only account of a target's resolved build and presentation inputs."""
from __future__ import annotations

from pathlib import Path
import tomllib

from tools.studio.targets import ROOT, TARGETS, target_id, conventional_sources as _conventional_sources


def _path(value: Path) -> str:
    return value.relative_to(ROOT).as_posix()


def _sources(target: str, spec: dict) -> tuple[dict, list[str]]:
    candidates = _conventional_sources(ROOT / 'targets' / target, spec)
    resolved = spec.get('sources') or {}
    entries = {}
    missing = []
    for role, candidate in candidates.items():
        path = ROOT / resolved[role] if role in resolved else candidate
        exists = path.is_file()
        origin = ('target file' if path.is_relative_to(ROOT / 'targets' / target) else
                  'HAL family' if path.is_relative_to(ROOT / 'hal' / spec.get('hal_family', spec['soc'])) else
                  'shared source')
        entries[role] = dict(path=_path(path), exists=exists, origin=origin)
        if not exists:
            missing.append(_path(path))
    for role in ('coprocessor', 'display_assembly'):
        if role in resolved:
            path = ROOT / resolved[role]
            entries[role] = dict(path=_path(path), exists=path.is_file(),
                                 origin='resolved build selection')
            if not path.is_file():
                missing.append(_path(path))
    extras = []
    family = spec.get('hal_family', spec['soc'])
    for source_list in (ROOT / 'hal' / family / 'SOURCES', ROOT / 'targets' / target / 'SOURCES'):
        if not source_list.is_file():
            continue
        for line, name in enumerate(source_list.read_text(encoding='utf-8').splitlines(), 1):
            path = (ROOT / name).resolve()
            inside = path.is_relative_to(ROOT)
            exists = inside and path.is_file()
            extras.append(dict(path=_path(path) if inside else name,
                               origin=f'{_path(source_list)}:{line}', exists=exists))
            if not exists:
                missing.append(name)
    return dict(conventional=entries, extra_sources=extras), missing


def _themes(target: str, spec: dict, configured_theme: str | None) -> dict:
    profile = spec.get('presentation_profile')
    roots = [ROOT / 'themes']
    if profile:
        roots.append(ROOT / 'ui' / 'profiles' / profile / 'themes')
    roots.append(ROOT / 'targets' / target / 'themes')
    found = {}
    for base in roots:
        for path in sorted(base.glob('*/theme.toml')):
            document = tomllib.loads(path.read_text(encoding='utf-8'))
            metadata = document.get('theme', {})
            allowed_targets = metadata.get('targets')
            if allowed_targets is not None and target not in allowed_targets:
                continue
            found[metadata['id']] = _path(path)
    selection = spec.get('ui')
    missing = []
    if selection:
        missing = [name for name in selection['allowed_themes'] if name not in found]
        found = {name: found[name] for name in selection['allowed_themes'] if name in found}
    return dict(default=selection['default_theme'] if selection else None,
                configured=configured_theme,
                selected=(configured_theme if configured_theme in found else
                          selection['default_theme'] if selection else configured_theme),
                unavailable_allowed=missing, available=found)


def describe(value: str) -> dict:
    from tools.content.compiler import resolve_features

    target = target_id(value)
    spec = TARGETS[target]
    sources, missing = _sources(target, spec)
    placeholders = []
    if not spec.get('build_enabled', True):
        placeholders.extend(name for name in ('board_id', 'model_alias', 'soc', 'hal_family',
                                              'cpu', 'audio_codec') if spec.get(name) == 'unassigned')
        placeholders.extend(f'retail.{name}' for name in ('usb_vid', 'usb_pid')
                            if spec['retail'].get(name) == 0)
        for role, entry in sources['conventional'].items():
            path = ROOT / entry['path']
            if entry['exists'] and path.is_relative_to(ROOT / 'targets' / target):
                if path.read_text(encoding='utf-8').startswith('#error'):
                    placeholders.append(f'source:{role}:{entry["path"]}')
    target_root = ROOT / 'targets' / target
    system = target_root / 'system.toml'
    if not system.is_file():
        if not spec.get('build_enabled', True):
            missing.append(_path(system))
        system = ROOT / 'config' / 'system.toml'
    profile = spec.get('presentation_profile')
    if profile and not (ROOT / 'ui' / 'profiles' / profile / 'profile.toml').is_file():
        missing.append(f'ui/profiles/{profile}/profile.toml')
    if not system.is_file():
        missing.append(_path(system))
    configuration = tomllib.loads(system.read_text(encoding='utf-8')) if system.is_file() else {}
    themes = _themes(target, spec, configuration.get('ui', {}).get('theme'))
    missing.extend(f'theme:{name}' for name in themes['unavailable_allowed'])
    memory = {name: dict(address=spec[address], bytes=spec[size]) for name, address, size in (
        ('resident', 'load_address', 'os_capacity_bytes'),
        ('staging', 'stage_address', 'stage_bytes'),
        ('recovery', 'recovery_snapshot_address', 'recovery_snapshot_bytes'),
        ('retained', 'mailbox_address', 'mailbox_bytes'))}
    return dict(target_id=target, build_enabled=spec.get('build_enabled', True),
                bringup_blocker=spec.get('bringup_blocker'),
                manifest=f'targets/{target}/target.toml', system_config=_path(system),
                prerequisites_missing=sorted(set(missing)), placeholders=placeholders,
                sources=sources,
                presentation_profile=profile, themes=themes,
                capabilities=spec['capabilities'], configured_features=configuration.get('features', {}),
                resolved_features=resolve_features(configuration, spec),
                decoders=dict(video_backend=spec['media_video_backend'],
                              h264=spec.get('h264_decoder', 'software'),
                              mpeg4=spec.get('mpeg4_decoder', 'none'),
                              audio_codec=spec.get('audio_codec')),
                memory=dict(sdram_address=spec.get('sdram_address', 0),
                            sdram_bytes=spec['sdram_bytes'],
                            resident_limit_bytes=spec['resident_limit_bytes'], regions=memory))
