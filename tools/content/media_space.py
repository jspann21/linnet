"""Peak preparation-space estimates and reviewed reconciliation plans."""
from __future__ import annotations

import math
import os
import shutil
import threading
import time
from .guard import guarded_path as _plain
from .host_state import (
    cache_data_directory,
    cache_root,
    file_stamp as _receipt_stamp,
    load_json_object as _optional_state,
)
from .inventory import KINDS, extensions, scan, skipped_paths as _skipped_paths
from .media_audio import _seek_stamp
from .media_options import Cancelled, _RECENT_OUTPUT_NS, native_video_extensions
from .media_video import _native_check_reusable, _video_duration, _video_is_native
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def preparation_space(identity, copies, jobs, selected, cache, *, photo_dimension=640,
                      current_inventory=None, ffprobe=None, ffmpeg=None,
                      no_conversions=False, force=False, skipped=None,
                      cancelled=lambda: False):
    """Bound peak free-space needs before the first device copy.

    Estimates include originals, prepared outputs, host scratch, existing
    publication backups, and the staged index recovery groups. Publishers
    still make their own exact checks immediately before individual writes.
    """
    def check():
        if cancelled():
            raise Cancelled('Stopped while estimating preparation space.')

    check()
    root = Path(identity['root'])
    cache = Path(cache)
    inventory = (current_inventory if current_inventory is not None else
                 scan(root, tuple(k for k in selected if k != 'Videos'), skipped=skipped,
                      checkpoint=check))
    counts = {kind: inventory.get(kind, {}).get('count', 0) for kind in KINDS}
    for copy in copies:
        check()
        if Path(copy['source']).suffix.lower() in extensions(copy['kind']):
            counts[copy['kind']] += 1
    original_bytes = sum(item.get('converted_bytes', item['bytes']) for item in copies)
    converted_sizes = [item['converted_bytes'] for item in copies if 'converted_bytes' in item]
    audio_scratch = min(sum(converted_sizes), max(64 * 1024 * 1024, max(converted_sizes, default=0)))
    staged_sizes = [item['bytes'] for item in copies if 'converted_bytes' in item and
                    Path(item['source']).is_relative_to(root)]
    audio_scratch += max(staged_sizes, default=0)
    # Audio index, artwork and transaction files grow with the track count.
    music_bytes = 0
    if 'Music' in selected and counts['Music']:
        music_bytes = 8 * 1024 * 1024 + counts['Music'] * 384 * 1024
    photo_bytes = 0
    if 'Photos' in selected and counts['Photos']:
        photo_bytes = 8 * 1024 * 1024 + counts['Photos'] * (
            2 * photo_dimension * photo_dimension * 2 + 16 * 1024)
    notes_bytes = (1024 * 1024 + counts['Notes'] * 64 * 1024
                   if 'Notes' in selected and counts['Notes'] else 0)
    audio_backup = 0
    largest_audio = 0
    if 'Music' in selected:
        seek_state = {} if force else _optional_state(cache_data_directory(cache) / 'seek-checked.json')
        for value in inventory.get('Music', {}).get('files', []):
            check()
            path = Path(value)
            if not path.is_relative_to(root) or path.suffix.lower() not in ('.m4a', '.m4b', '.flac', '.flc'):
                continue
            info = path.stat()
            stamp = _seek_stamp(path, info)
            if seek_state.get(path.relative_to(root).as_posix().casefold()) != stamp:
                audio_backup += info.st_size
                largest_audio = max(largest_audio, info.st_size)
        for copy in copies:
            check()
            if copy['kind'] == 'Music' and Path(copy['source']).suffix.lower() in ('.m4a', '.m4b', '.flac', '.flc'):
                audio_backup += copy['bytes']
                largest_audio = max(largest_audio, copy['bytes'])
    video_bytes = 0
    video_host = 0
    video_conversion_required = False
    if 'Videos' in selected:
        backend = identity['media_profile']['video_backend']
        native_extensions = native_video_extensions(identity)

        def checked_duration(source, probe):
            seconds, before = probe.get('seconds'), probe.get('stamp')
            if not seconds or before is None:
                return None
            try:
                after = _receipt_stamp(Path(source).stat())
            except (OSError, ValueError):
                return None
            if (before != after or
                    (os.name == 'nt' and after[3] > time.time_ns() - _RECENT_OUTPUT_NS)):
                return None
            return seconds, after

        def inspect_host(job, stop):
            from .native_video import inspect_file
            probe = {}
            try:
                details = inspect_file(job['source'], profile=identity['media_profile'],
                                       ffprobe=ffprobe, cancelled=lambda: stop.is_set() or cancelled(),
                                       duration_probe=probe)
            except (OSError, ValueError):
                details = None
            return details, checked_duration(job['source'], probe) if details is None else None

        def estimate_jobs(items, futures=None):
            nonlocal video_bytes, video_host, video_conversion_required
            for index, job in enumerate(items):
                check()
                source = Path(job['source'])
                details = duration_hint = None
                if backend in ('bcm-mp4', 'native-mp4') and source.suffix.lower() in native_extensions:
                    hint = job.get('_native_details') if no_conversions else None
                    if (isinstance(hint, dict) and type(hint.get('fast_start')) is bool and
                            hint.get('file_bytes') == job['bytes'] and
                            _native_check_reusable(job, source, identity['media_profile'])):
                        details = hint
                    elif futures is not None:
                        details, duration_hint = futures[index].result()
                    else:
                        from .native_video import inspect_file
                        probe = {}
                        try:
                            details = inspect_file(source, profile=identity['media_profile'],
                                                   ffprobe=ffprobe, cancelled=cancelled,
                                                   duration_probe=probe)
                        except (OSError, ValueError):
                            pass
                        if details is None:
                            duration_hint = checked_duration(source, probe)
                    native = details is not None
                else:
                    native = (no_conversions and
                              _native_check_reusable(job, source, identity['media_profile']))
                    if not native:
                        native = _video_is_native(source, identity, ffprobe=ffprobe,
                                                  cancelled=cancelled)
                if identity['media_profile'].get('native_video', {}).get('normalize', False):
                    native = False
                encoder = ffmpeg or shutil.which('ffmpeg')
                if native:
                    if backend in ('bcm-mp4', 'native-mp4'):
                        converts = (not details['fast_start'] or backend == 'bcm-mp4') and bool(encoder)
                        video_conversion_required |= converts
                        direct = no_conversions or not converts
                    else:
                        direct = True
                    if direct and source.is_relative_to(root):
                        continue
                    estimate = job['bytes'] if direct else min(0x7fffffff, job['bytes'] * 2)
                    scratch = 0
                else:
                    video_conversion_required = True
                    duration = None
                    if duration_hint is not None:
                        seconds, stamp = duration_hint
                        try:
                            if _receipt_stamp(source.stat()) == stamp:
                                duration = seconds
                        except (OSError, ValueError):
                            pass
                    if duration is None:
                        duration = _video_duration(source, ffprobe, cancelled=cancelled)
                    if backend in ('bcm-mp4', 'native-mp4'):
                        limit = identity['media_profile']['native_video']['max_video_kbps'] + 256
                        estimate = min(0x7fffffff, max(job['bytes'] * 3,
                            math.ceil(duration * limit * 1000 / 8 * 1.25)))
                        scratch = 0
                    else:
                        profile = identity['media_profile']
                        estimate = min(0x7fffffff,
                            math.ceil(duration * profile['video_fps'] * profile['max_read_sectors'] * 512))
                        scratch = math.ceil(duration * 44100 * 4) + 32 * 1024 * 1024
                video_bytes += estimate
                video_host += estimate * 2 + scratch

        all_jobs = list(jobs)
        host_checks = [not no_conversions and backend in ('bcm-mp4', 'native-mp4') and
                       Path(job['source']).suffix.lower() in native_extensions and
                       not Path(job['source']).is_relative_to(root) for job in all_jobs]
        first_host = next((index for index, eligible in enumerate(host_checks) if eligible), len(all_jobs))
        host_count = sum(host_checks)
        if host_count > 1 and all(host_checks[first_host:]):
            estimate_jobs(all_jobs[:first_host])
            stop = threading.Event()
            with ThreadPoolExecutor(max_workers=min(4, host_count)) as pool:
                try:
                    futures = [pool.submit(inspect_host, job, stop) for job in all_jobs[first_host:]]
                    estimate_jobs(all_jobs[first_host:], futures)
                finally:
                    stop.set()
        else:
            estimate_jobs(all_jobs)
    files_entries = sum(counts.values()) + len(jobs)
    preview_sources = counts['Photos'] + counts['Notes'] + len(jobs)
    files_bytes = 4 * 1024 * 1024 + files_entries * 512 + preview_sources * 256 * 1024
    prepared = music_bytes + photo_bytes + notes_bytes + video_bytes + files_bytes
    previous = 0
    fixed = []
    if 'Music' in selected:
        fixed += ['.LINNET/COVERS.CFL', '.LINNET/BROWSE.BIN', '.LINNET/LIBRARY.PDB']
    if 'Photos' in selected:
        fixed += ['.LINNET/PHOTOS/GRID.PGR', '.LINNET/PHOTOS/LIBRARY.PIM']
    if 'Notes' in selected:
        fixed += ['.LINNET/NOTES/LIBRARY.NNT']
    if 'Videos' in selected:
        fixed += ['.LINNET/VIDEOS/INDEX.VID']
    fixed += ['.LINNET/FILES/INDEX.FIL']
    for name in fixed:
        check()
        path = root / name
        _plain(path, root)
        if path.is_file():
            previous += path.stat().st_size
    # A video replacement can keep both the prior active package and backup.
    # Budget a previous package as large as the candidate when the exact old
    # path is unavailable from a host receipt.
    previous += video_bytes
    device_needed = original_bytes + prepared + previous * 2 + largest_audio + 16 * 1024 * 1024
    host_needed = (music_bytes * 2 + photo_bytes * 2 + notes_bytes * 2 + video_host + audio_scratch +
                   files_bytes * 2 + audio_backup * 2 + 16 * 1024 * 1024)
    host_free = []
    for parent in dict.fromkeys((cache, cache_data_directory(cache))):
        while not parent.exists():
            check()
            parent = parent.parent
        host_free.append(shutil.disk_usage(parent).free)
    check()
    return dict(device_peak_bytes=device_needed, computer_peak_bytes=host_needed,
                device_free_bytes=shutil.disk_usage(root).free,
                computer_free_bytes=min(host_free),
                video_conversion_required=video_conversion_required)


def reconciliation_actions(identity, selected, copies, *, replacements=(),
                           current_inventory=None, skipped=None, checkpoint=None):
    """Describe removals that require explicit review before preparation."""
    if checkpoint:
        checkpoint()
    root = Path(identity['root'])
    actions = list(replacements)
    if current_inventory is None:
        if skipped is None:
            state = _optional_state(cache_root(identity) / 'state.json')
            skipped = {kind: _skipped_paths(state, kind) for kind in KINDS}
        inventory = scan(root, tuple(k for k in selected if k != 'Videos'), skipped=skipped,
                         checkpoint=checkpoint)
    else:
        inventory = current_inventory
    for kind, name in (('Music', '.LINNET/LIBRARY.PDB'),
                       ('Photos', '.LINNET/PHOTOS/LIBRARY.PIM'),
                       ('Notes', '.LINNET/NOTES/LIBRARY.NNT')):
        if checkpoint:
            checkpoint()
        if kind not in selected:
            continue
        count = inventory.get(kind, {}).get('count', 0)
        count += sum(item['kind'] == kind and
                     Path(item['source']).suffix.lower() in extensions(kind) for item in copies)
        path = root / name
        _plain(path, root)
        if not count and path.is_file():
            info = path.stat()
            actions.append(dict(action='retire-library', kind=kind, destination=str(path),
                                old_bytes=info.st_size, old_mtime_ns=info.st_mtime_ns))
    if 'Videos' in selected:
        from .media_library import _catalog, _device_path
        _, _, entries = _catalog(root)
        for key, name, title in entries:
            if checkpoint:
                checkpoint()
            path = _device_path(root, name)
            if not path.is_file():
                actions.append(dict(action='prune-video', kind='Videos',
                                    destination=str(path), key=key.hex(), title=title))
    if checkpoint:
        checkpoint()
    return sorted(actions, key=lambda row: (row['kind'], row['action'], row['destination'].casefold()))
