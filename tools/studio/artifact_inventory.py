"""Read-only inventory of build output and recovery references under out/."""
from __future__ import annotations

import json
import os
from pathlib import Path

from tools.studio.targets import ROOT

RECORD_NAMES = {"plan.json", "context.json", "receipt.json", "manifest.json"}
PATH_SUFFIXES = {".bin", ".elf", ".ipod", ".json", ".los", ".map", ".pimg"}


def _relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _read_json(path: Path, size: int):
    if size > 4 * 1024 * 1024:
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, UnicodeError):
        return None


def _referenced_paths(value, record: Path, root: Path, output: Path):
    """Find existing out/ paths in bounded plan/receipt JSON structures."""
    found = set()
    stack = [(value, 0)]
    while stack:
        item, depth = stack.pop()
        if depth > 16:
            continue
        if isinstance(item, dict):
            for key, child in item.items():
                if key == "artifacts" and isinstance(child, dict):
                    for name in child:
                        if isinstance(name, str) and Path(name).name == name:
                            candidate = record.parent / name
                            try:
                                resolved = candidate.resolve()
                                if resolved.is_relative_to(output) and resolved.is_file():
                                    found.add(resolved)
                            except OSError:
                                continue
            stack.extend((child, depth + 1) for child in item.values())
            continue
        if isinstance(item, list):
            stack.extend((child, depth + 1) for child in item)
            continue
        if not isinstance(item, str) or len(item) > 4096:
            continue
        path = Path(item)
        if path.suffix.lower() not in PATH_SUFFIXES or not (path.is_absolute() or '/' in item or '\\' in item):
            continue
        candidates = [path] if path.is_absolute() else [root / path, record.parent / path]
        for candidate in candidates:
            try:
                resolved = candidate.resolve()
                if resolved.is_relative_to(output) and resolved.is_file():
                    found.add(resolved)
                    break
            except OSError:
                continue
    return found


def inventory(root: Path = ROOT) -> dict:
    root = Path(root).resolve()
    output = root / "out"
    if not output.is_dir():
        return dict(output=str(output), files=0, bytes=0, builds=[], recovery_records=[],
                    referenced_files=dict(files=0, bytes=0),
                    unclassified=dict(files=0, bytes=0), scan_errors=[])
    sizes = {}
    scan_errors = []
    def on_error(error):
        scan_errors.append(str(error))
    for directory, _folders, names in os.walk(output, followlinks=False, onerror=on_error):
        for name in names:
            path = Path(directory) / name
            try:
                sizes[path.absolute()] = path.lstat().st_size
            except OSError as error:
                scan_errors.append(str(error))
    known = set()
    run_directories = set()
    builds = []
    for image in sorted(path for path in sizes if path.suffix == '.los'):
        report = image.with_suffix('.json')
        if report not in sizes:
            continue
        data = _read_json(report, sizes[report])
        if not isinstance(data, dict) or not all(key in data for key in
            ('build_id', 'target_id', 'profile', 'kind', 'artifacts')) or not isinstance(data['artifacts'], dict):
            continue
        artifacts = {}
        for name, value in data['artifacts'].items():
            if not isinstance(value, str):
                continue
            path = (root / value).resolve()
            if not path.is_relative_to(output):
                continue
            artifacts[name] = dict(path=_relative(path, root), bytes=sizes.get(path),
                                   present=path in sizes)
            known.add(path)
        known.add(report)
        # Invocation directories also contain generated headers and objects.
        if report.parent.parent.name == 'runs':
            run_directories.add(report.parent)
        builds.append(dict(report=_relative(report, root), build_id=data['build_id'],
                           content_id=data.get('content_id'),
                           target_id=data['target_id'], profile=data['profile'],
                           kind=data['kind'], artifacts=artifacts))
    known.update(path for path in sizes if any(parent in run_directories for parent in path.parents))
    references = {}
    recovery_records = []
    for path, size in sorted(sizes.items()):
        if path.suffix != '.json' or not (path.name in RECORD_NAMES or
                                         'receipt' in path.stem or path.name.endswith('.result.json')):
            continue
        data = _read_json(path, size)
        if not isinstance(data, dict) or not any(key in data for key in
            ('artifacts', 'plan', 'build', 'transaction', 'archive', 'original')):
            continue
        found = _referenced_paths(data, path, root, output)
        known.add(path)
        for target in found:
            references.setdefault(target, set()).add(path)
        record_type = ('plan' if path.name == 'plan.json' else
                       'receipt' if 'receipt' in path.stem else 'recovery record')
        recovery_records.append(dict(path=_relative(path, root), type=record_type, bytes=size,
                                     references=sorted(_relative(item, root) for item in found)))
    for build in builds:
        report = (root / build['report']).resolve()
        related = {report, *(root / artifact['path'] for artifact in build['artifacts'].values())}
        build['referenced_by'] = sorted({_relative(record, root) for item in related
                                       for record in references.get(item.resolve(), ())})
    referenced = {path: sizes[path] for path in references if path in sizes}
    known.update(referenced)
    unknown = {path: size for path, size in sizes.items() if path not in known}
    return dict(output=str(output), files=len(sizes), bytes=sum(sizes.values()),
                builds=builds, recovery_records=recovery_records,
                referenced_files=dict(files=len(referenced), bytes=sum(referenced.values())),
                unclassified=dict(files=len(unknown), bytes=sum(unknown.values())),
                scan_errors=scan_errors)


if __name__ == '__main__':
    print(json.dumps(inventory(), indent=2))
