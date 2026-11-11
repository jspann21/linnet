"""Desktop media preparation orchestration and compatible workflow entry points."""
from __future__ import annotations

import json
import os
import re
import shutil
import time
import uuid
from .guard import require_same_target as _same_target  # noqa: F401 - existing facade export
from .host_state import cache_directory as _cache_directory  # noqa: F401 - existing facade export
from .host_state import file_stamp as _receipt_stamp  # noqa: F401 - existing facade export
from concurrent.futures import TimeoutError as FutureTimeout  # noqa: F401 - existing facade export
import hashlib  # noqa: F401 - existing facade export
import math  # noqa: F401 - existing facade export
import stat  # noqa: F401 - existing facade export
import struct  # noqa: F401 - existing facade export
import subprocess  # noqa: F401 - existing facade export
import tempfile  # noqa: F401 - existing facade export
import threading  # noqa: F401 - existing facade export
import unicodedata  # noqa: F401 - existing facade export
from .guard import (
    guard_volume as _volume_guard,
    guarded_path as _plain,
    hash_file as _hash,
    inspect_target as inspect_target,
    require_linnet,
)
from .host_state import (
    cache_data_directory,
    cache_root,
    cache_volume_scope as cache_volume_scope,
    load_json_object as _optional_state,
    media_state_root as media_state_root,
    save_json as _save,
)
from .inventory import (
    KINDS,
    VIDEO_EXTENSIONS as VIDEO_EXTENSIONS,
    extensions,
    file_records as _file_records,
    scan,
    skipped_paths as _skipped_paths,
    source_folders,
)
from .media_audio import (
    _append_audio_journal,
    _convert_audio as _convert_audio,
    _import_metadata_seed as _import_metadata_seed,
    _prepare_audio,
    _prepare_import_aac as _prepare_import_aac,
    _prepare_import_flac as _prepare_import_flac,
    _prepare_import_mp3 as _prepare_import_mp3,
    _save_seek_state as _save_seek_state,
    _seed_imported_music as _seed_imported_music,
    _seek_stamp as _seek_stamp,
    _verified_audio_hash as _verified_audio_hash,
    _wave_metadata as _wave_metadata,
    _wave_pcm_hash as _wave_pcm_hash,
)
from .media_imports import (
    _copy_additions,
    _music_import_name as _music_import_name,
    _normalize_music_paths,
    _planning_records as _planning_records,
    _same_existing_file as _same_existing_file,
    _same_file_bytes as _same_file_bytes,
    additions,
)
from .media_options import (
    Cancelled,
    _RECENT_OUTPUT_NS as _RECENT_OUTPUT_NS,
    format_size as format_size,
    native_video_extensions,
    preparation_option,
    supported_kinds,
    target_args,
    video_source_extensions,
)
from .media_outputs import (
    _checking_progress,
    _music_outputs_from_publication,
    _outputs_match,
    _photo_output as _photo_output,
    _published_photo_output,
    _throttled_progress,
)
from .media_space import preparation_space, reconciliation_actions
from .media_video import (
    _native_check_reusable as _native_check_reusable,
    _prepare_videos,
    _video_duration as _video_duration,
    _video_inventory,
    _video_is_native as _video_is_native,
    _video_outputs_from_publication,
    video_jobs,
)
from .media_volume import (
    _identify as _identify,
    _inspect_identify as _inspect_identify,
    _retail_cache_identity as _retail_cache_identity,
    discover as discover,
    eject_device as eject_device,
    list_skipped_files as list_skipped_files,
    refresh,
    restore_skipped_files as restore_skipped_files,
    skip_files,
)
from concurrent.futures import ThreadPoolExecutor as ThreadPoolExecutor
from contextlib import ExitStack as ExitStack
from pathlib import Path


def _error_record(kind, error, inventory, root):
    """Attach a concrete source when a compiler identifies one in its error."""
    from . import SourceError
    message = str(error)
    if isinstance(error, TimeoutError):
        return dict(kind=kind, message=message)
    if isinstance(error, SourceError):
        folded = str(error.source).casefold()
        matches = [Path(value) for value in inventory.get("files", []) if str(value).casefold() == folded]
    else:
        folded = message.casefold()
        matches = [Path(value) for value in inventory.get("files", []) if str(value).casefold() in folded]
    record = dict(kind=kind, message=message)
    if len(matches) == 1:
        source = matches[0]
        name = source.relative_to(root).as_posix() if source.is_relative_to(root) else source.name
        record.update(file=name, source=str(source))
    return record


def _source_error(kind, name, errors):
    source = Path(name)
    for error in errors:
        if error.get('kind') != kind:
            continue
        value = error.get('source')
        if value and (Path(value) == source or Path(value).is_relative_to(source)):
            return error
        relative = str(error.get('file') or '').replace('\\', '/')
        for prefix in (source.name, f'{kind}/{source.name}'):
            if relative == prefix or relative.startswith(prefix + '/'):
                return error
    return None


def _pending_outcomes(pending, selected, plan, videos, steps, errors, planned):
    added = {step['added'] for step in steps if 'added' in step}
    ready_videos = {step['file'] for step in steps
                    if step.get('kind') == 'Videos' and
                    step.get('status') in ('published', 'unchanged', 'catalog-updated') and
                    'file' in step}
    pending_video_names = {Path(name).name for kind, name in pending if kind == 'Videos'}
    unfinished_videos = set()
    for job in (videos if pending_video_names else ()):
        if job['relative'] not in ready_videos:
            first = job['relative'].split('/', 1)[0]
            if first in pending_video_names:
                unfinished_videos.add(first)
    pending_sources = {(kind, Path(name)) for kind, name in pending if kind != 'Videos'}
    pending_kinds = {kind for kind, _ in pending_sources}
    unfinished = set()
    for item in (plan if pending_sources else ()):
        kind = item['kind']
        if kind not in pending_kinds or item['destination'] in added:
            continue
        source = Path(item['source'])
        if source.drive:
            drive_source = Path(source.drive)
            key = kind, drive_source
            if key in pending_sources and source.is_relative_to(drive_source):
                unfinished.add(key)
        while True:
            key = kind, source
            if key in pending_sources:
                unfinished.add(key)
            if source == source.parent:
                break
            source = source.parent
    outcomes = []
    for kind, name in pending:
        source = Path(name)
        if kind == 'Videos':
            finished = kind in selected and planned and source.name not in unfinished_videos
        else:
            finished = kind in selected and planned and (kind, source) not in unfinished
        matching_error = _source_error(kind, name, errors)
        outcomes.append(dict(kind=kind, source=name,
                             status='completed' if finished else 'unfinished',
                             reason='' if finished else (matching_error or {}).get('message', 'Source did not finish.')))
    return outcomes


def _finish_retries(attempts, skipped, unresolved):
    report = attempts[-1]
    report['started'] = attempts[0]['started']
    report['steps'] = [step for attempt in attempts for step in attempt['steps']]
    report['cleanup'] = {
        key: [entry for attempt in attempts for entry in attempt['cleanup'][key]]
        for key in ('removed', 'retained')
    }
    outcomes = {}
    for attempt in attempts:
        for row in attempt.get('source_outcomes', []):
            outcomes[(row['kind'], row['source'])] = row
    report['source_outcomes'] = list(outcomes.values())
    report['errors'] = skipped + unresolved
    report['auto_skipped'] = skipped
    report['complete'] = not unresolved and not report.get('stopped')
    if len(attempts) > 1:
        report['retry_receipts'] = [attempt['receipt'] for attempt in attempts[:-1]]
    for outcome in report['source_outcomes']:
        matching = _source_error(outcome['kind'], outcome['source'], skipped)
        if matching:
            outcome.update(status='skipped', reason=matching['message'])
    if skipped:
        count = len(skipped)
        outcome = 'stopped' if report.get('stopped') else 'finished'
        report['summary'] = (
            f"Update {outcome} with {count} problem file{'s' if count != 1 else ''} skipped. "
            'Select a skipped file in Media Manager if you want to retry it.'
        )
        if unresolved:
            report['summary'] += ' Some non-file errors still need attention.'
    _save(Path(report['receipt']), report)
    return report


def run(identity, selected, pending=(), **options):
    from .publish import _exclusive
    # The same Windows mutex is recursive for nested publishers on this thread.
    # A second manager must not overwrite the first manager's cache or catalog.
    with _exclusive(identity):
        automatically_skipped = []
        skipped_keys = set()
        excluded_sources = set()
        attempts = []
        unresolved = []
        retry_selected = selected
        while True:
            # Verify the device after taking the mutex, including on a source-error retry.
            identity = refresh(identity)
            require_linnet(identity)
            root = Path(identity["root"]).resolve(strict=True)
            allowed = video_source_extensions(identity)
            current_options = dict(options, excluded_sources=excluded_sources)
            if attempts:
                current_options['force'] = False
                if any(attempt.get('reconciliation') for attempt in attempts):
                    current_options['approved_reconciliation'] = None
            retry_pending = tuple(item for item in pending if item[0] in retry_selected)
            report = _run(identity, retry_selected, retry_pending, **current_options)
            attempts.append(report)
            remaining = []
            newly_skipped = []
            for error in report["errors"]:
                kind, source_value = error.get("kind"), error.get("source")
                if not kind or not source_value or report.get("stopped"):
                    remaining.append(error)
                    continue
                try:
                    source = Path(source_value).resolve()
                except (OSError, ValueError):
                    remaining.append(error)
                    continue
                key = (kind, str(source).casefold())
                if key in skipped_keys:
                    remaining.append(error)
                    continue
                try:
                    if source.is_relative_to(root):
                        if not skip_files(identity, [(kind, str(source))]):
                            remaining.append(error)
                            continue
                    else:
                        if kind != "Videos" or source.suffix.lower() not in allowed:
                            remaining.append(error)
                            continue
                        excluded_sources.add(str(source).casefold())
                except (OSError, ValueError):
                    remaining.append(error)
                    continue
                skipped_keys.add(key)
                skipped = dict(error, status="skipped")
                newly_skipped.append(skipped)
                automatically_skipped.append(skipped)
                options.get("progress", lambda message: None)(
                    f"Skipped problem file and continuing: {source}"
                )
            unresolved.extend(remaining)
            unsafe = {error.get('kind') for error in remaining}
            retry_kinds = {error['kind'] for error in newly_skipped} - unsafe
            if not retry_kinds or None in unsafe:
                return _finish_retries(attempts, automatically_skipped, unresolved)
            retry_selected = tuple(kind for kind in KINDS if kind in retry_kinds)


def _prepare_music(root, cache, identity, skipped, *, ffprobe, verification, force,
                   cancelled, checkpoint, progress, report, begin_publication):
    from .music import compile_music
    from .publish import publish_music
    cache = cache_data_directory(cache)
    stage = cache / "music"
    music_progress = _throttled_progress(checkpoint, progress,
        lambda event: f'{"Using saved metadata" if event.get("cached") else "Reading metadata"} '
                      f'{event["current"]+1}/{event["total"]}: {Path(event["file"]).name}')
    preparation = compile_music(root, stage, ffprobe=ffprobe or shutil.which("ffprobe"), device_root=root,
                  verification=verification, cache_path=cache / "music-metadata.sqlite3",
                  cache_scope=identity.get('media_cache_volume_scope'),
                  bypass_cache=force, progress=music_progress, include_roots=source_folders(root, "Music"),
                  excluded=skipped, target=identity["target_id"], cancelled=cancelled)
    report["steps"].append(dict(kind="Music", metadata=preparation.get("cache"), integrity=verification))
    checkpoint()
    progress("Publishing the music index and artwork…")
    begin_publication("Music")
    publication = publish_music(stage, progress=progress, **target_args(identity))
    verified_outputs = publication.pop('_verified_outputs')
    published = [publication]
    output_paths = [".LINNET/LIBRARY.PDB"] + [entry["path"].lstrip("/") for entry in preparation["entries"]
                    if entry["storage"] == "staged"] + [
        ".LINNET/ART/" + path.name for path in sorted((stage / "ART").glob("*.RGB"))]
    if (stage / "COVERS.CFL").exists(): output_paths.append(".LINNET/COVERS.CFL")
    if (stage / "BROWSE.BIN").exists(): output_paths.append(".LINNET/BROWSE.BIN")
    return output_paths, published, verified_outputs


def _prepare_photos(root, cache, identity, media_profile, skipped, retail_photos, *,
                    dimension, checkpoint, progress, begin_publication):
    from .photos import FAT_PATH, compile_photos, fallback_jpeg_header_compatible
    from .publish_photos import publish_photos
    from .music import _device_media_path
    from PIL import Image

    fallbacks = {}
    for path, info in _file_records(root / 'Photos', allowed={'.jpg'}):
        checkpoint()
        if info.st_size > 262144 or path.relative_to(root).as_posix().casefold() in skipped:
            continue
        try:
            with Image.open(path) as image:
                width, height = image.size
                eligible = (image.format == 'JPEG' and image.mode in ('RGB', 'L') and
                            not image.info.get('progressive') and not image.info.get('progression') and
                            max(width, height) <= 2048 and width * height <= 4_000_000)
            if not eligible:
                continue
            with path.open('rb') as source:
                if not fallback_jpeg_header_compatible(source.read(262145)):
                    continue
            device_path = _device_media_path(path, root)
            if len(device_path) <= 127 and FAT_PATH.fullmatch(device_path):
                fallbacks[path.relative_to(root / 'Photos').as_posix()] = device_path
        except (OSError, ValueError, SyntaxError, Image.DecompressionBombError):
            pass  # The photo compiler reports unreadable originals separately.

    cache = cache_data_directory(cache)
    package = cache / "photos.pimg"
    photo_progress = _throttled_progress(checkpoint, progress,
        lambda event: f'Preparing photos {event["current"]+1}/{event["total"]}: {Path(event["file"]).name}')
    prepared = compile_photos(root / "Photos", package, max_dimension=dimension,
                   target=identity['target_id'],
                   viewport_width=media_profile["photo_width"],
                   viewport_height=media_profile["photo_height"],
                   cache_dir=cache / "photo-cache", progress=photo_progress, checkpoint=checkpoint,
                   fallback_paths=fallbacks,
                   excluded={Path(value).relative_to("Photos").as_posix() for value in skipped},
                   additional_sources={Path(item['relative']).relative_to('Photos').as_posix():
                                       (item['preview'], item['title']) for item in retail_photos})
    checkpoint()
    progress("Publishing prepared photos…")
    begin_publication("Photos")
    return [".LINNET/PHOTOS/LIBRARY.PIM", ".LINNET/PHOTOS/GRID.PGR"] + list(dict.fromkeys(
        item['device_path'].lstrip('/') for item in prepared.get('packs', []))), [
        publish_photos(package, grid=package.with_suffix('.pgr'), **target_args(identity))]


def _prepare_notes(root, cache, identity, skipped, *, checkpoint, progress,
                   report, begin_publication):
    from .notes import compile_notes
    from .publish_notes import publish_notes
    package = cache_data_directory(cache) / "notes.pnot"
    note_progress = _throttled_progress(checkpoint, progress,
        lambda event: f'Preparing notes {event["current"]+1}/{event["total"]}: {Path(event["file"]).name}')
    preparation = compile_notes(root / "Notes", package, target=identity['target_id'],
        progress=note_progress, checkpoint=checkpoint,
        excluded={Path(value).relative_to("Notes").as_posix() for value in skipped})
    report["steps"].append(dict(kind="Notes", preparation=preparation))
    if preparation.get("ascii_converted"):
        progress("Notes: non-ASCII text was adapted for the iPod font; originals are unchanged. See the report.")
    checkpoint()
    progress("Publishing prepared notes…")
    begin_publication("Notes")
    return [".LINNET/NOTES/LIBRARY.NNT"], [publish_notes(package, **target_args(identity))]


def _run(identity, selected, pending=(), *, progress=lambda message: None, cancelled=lambda: False,
        photo_dimension=640, ffmpeg=None, ffprobe=None, force=False, music_verification="metadata",
        video_selection=None, progress_detail=None, progress_stage=lambda event: None,
        excluded_sources=None, online_artwork=False, approved_reconciliation=None,
        convert_audio=False):
    """Copy additions, prepare and publish selected folders; retain per-step receipts.

    Stop requests are honored between files and between compiler/publication calls.
    Failed publication is never retried automatically.
    """
    requested = tuple(kind for kind in KINDS if kind in selected)
    unsupported = tuple(kind for kind in requested if kind not in supported_kinds(identity))
    if unsupported:
        raise ValueError(f"{', '.join(unsupported)} playback is unavailable on this iPod model.")
    selected = requested
    if not selected:
        raise ValueError("Select at least one media folder to prepare.")
    total_stages = 4 + 2 * len(selected)
    def emit_stage(name, unit, fraction=0, can_stop=True):
        progress_stage(dict(name=name, completed=unit, total=total_stages,
                            fraction=fraction, can_stop=can_stop))
    if music_verification not in ("metadata", "full"):
        raise ValueError("Music verification must be metadata or full")
    root = Path(identity["root"])
    media_profile = identity["media_profile"]
    cache = cache_root(identity)
    data_cache = cache_data_directory(cache)
    if any(path.drive.casefold() == root.drive.casefold() for path in (cache, data_cache)):
        raise ValueError("The preparation cache must be on the computer, not the iPod.")
    cache.mkdir(parents=True, exist_ok=True)
    data_cache.mkdir(parents=True, exist_ok=True)
    state_path = cache / "state.json"
    state = _optional_state(state_path)
    if not isinstance(state.get("skipped_files", {}), dict):
        state["skipped_files"] = {}
    skipped = {kind: _skipped_paths(state, kind) for kind in KINDS}
    report = dict(started=time.time(), identity=identity, steps=[], errors=[], complete=False,
                  stopped=False, cache_directory=str(cache), reusable_cache_directory=str(data_cache),
                  cleanup=dict(removed=[], retained=[]))
    plan, videos, planned = [], [], False
    owned_temporaries = []
    from .media_status import _remember_verified, _verified_outputs
    verified_outputs = _verified_outputs(identity['disk_serial'], identity['volume_serial'],
                                         identity['partition_offset'], identity['volume_unique_id'],
                                         cache_disk_serial=identity.get('media_cache_disk_serial'))
    receipt = cache / ("session-" + uuid.uuid4().hex + ".json")
    report["receipt"] = str(receipt)
    audio_journal_path = receipt.with_suffix('.audio.jsonl')
    report['audio_journal'] = str(audio_journal_path)
    aac_recovery = report['aac_preparations'] = []
    flac_recovery = report['flac_preparations'] = []
    aac_committed = aac_publication_started = False
    apple_database = root / 'iPod_Control/iTunes/iTunesDB'
    apple_stamp, next_apple_check = None, 0.0

    def checkpoint():
        nonlocal next_apple_check
        if cancelled():
            raise Cancelled("Stopped between steps. Completed copies and publications have been retained.")
        _volume_guard(identity)
        if apple_stamp is not None and time.monotonic() >= next_apple_check:
            from .retail_media import _check_database_stamp
            _check_database_stamp(apple_database, apple_stamp)
            next_apple_check = time.monotonic() + 0.25

    def record():
        _save(receipt, report)

    def journal_audio(kind, change):
        _append_audio_journal(audio_journal_path, kind, change)

    record()
    try:
        _plain(apple_database, root)
        if apple_database.is_file():
            info = apple_database.stat()
            apple_stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
        if video_selection is not None:
            video_selection = [value for value in video_selection
                               if Path(value).resolve().relative_to(root).as_posix().casefold()
                               not in skipped["Videos"]]
        replacements = []
        videos = video_jobs(root, pending, video_selection, skipped=skipped["Videos"],
                            identity=identity, ffprobe=ffprobe, cancelled=cancelled,
                            replacements=replacements) if "Videos" in selected else []
        excluded_sources = {str(Path(value).resolve()).casefold() for value in (excluded_sources or ())}
        videos = [job for job in videos if str(Path(job["source"]).resolve()).casefold() not in excluded_sources]
        reused_audio = []
        verified_before = verified_outputs.copy()
        plan = additions(root, [item for item in pending if item[0] in selected and item[0] != "Videos"],
                         replacements=replacements, convert_audio=convert_audio,
                         target=identity['target_id'], ffprobe=ffprobe, cancelled=cancelled,
                         reused_audio=reused_audio, verified=verified_outputs)
        _remember_verified(identity, verified_outputs, verified_before)
        device_conversions = [source for source, _ in reused_audio] + [Path(item['source']) for item in plan
            if 'converted_bytes' in item and Path(item['source']).is_relative_to(root)]
        projected_skipped = {kind: paths.copy() for kind, paths in skipped.items()}
        for source in device_conversions:
            projected_skipped['Music'].add(source.relative_to(root).as_posix().casefold())
        for _, destination in reused_audio:
            projected_skipped['Music'].discard(destination.relative_to(root).as_posix().casefold())
        if any('converted_bytes' in item for item in plan) and not ffmpeg:
            raise ValueError('Audio conversion needs FFmpeg on the computer.')
        planned = True
        source_kinds = tuple(kind for kind in selected if kind != "Videos")
        current_inventory = (scan(root, source_kinds, skipped=projected_skipped,
                                  checkpoint=checkpoint) if source_kinds else {})
        reconciliation = reconciliation_actions(identity, selected, plan,
            replacements=replacements, current_inventory=current_inventory,
            skipped=projected_skipped, checkpoint=checkpoint)
        report['reconciliation'] = reconciliation
        if ((reconciliation and approved_reconciliation is None) or
                (approved_reconciliation is not None and approved_reconciliation != reconciliation)):
            raise ValueError('Source replacement or library cleanup needs review. Refresh the preparation plan and approve its exact actions.')
        for kind, maximum in (("Notes", 256),):
            if kind in selected:
                existing = current_inventory[kind]["count"]
                replacing = {action['destination'].casefold() for action in replacements
                             if action['kind'] == kind and
                             Path(action['destination']).relative_to(root).as_posix().casefold()
                             not in skipped[kind]}
                added = sum(item["kind"] == kind and
                            Path(item["source"]).suffix.lower() in extensions(kind) and
                            item['destination'].casefold() not in replacing for item in plan)
                if existing + added > maximum:
                    raise ValueError(f"{kind} supports {maximum} files; this selection contains {existing + added}. Choose a smaller source folder.")
        space = preparation_space(identity, plan, videos, selected, cache,
            photo_dimension=photo_dimension, ffprobe=ffprobe, ffmpeg=ffmpeg,
            no_conversions=video_selection is not None and not video_selection,
            force=force, current_inventory=current_inventory, skipped=projected_skipped,
            cancelled=cancelled)
        report['space'] = space
        if space['device_peak_bytes'] > min(identity['free_bytes'], space['device_free_bytes']):
            raise ValueError('Not enough iPod space for originals, prepared media, and recovery copies.')
        if space['computer_peak_bytes'] > space['computer_free_bytes']:
            raise ValueError('Not enough computer space for prepared media and conversion scratch.')
        if reconciliation:
            from .media_library import list_items, remove_items, remove_supporting_file
            supporting = [action for action in reconciliation
                if action['action'] == 'replace' and
                Path(action['destination']).suffix.lower() not in extensions(action['kind'])]
            normal = [action for action in reconciliation if action not in supporting]
            selected_rows = []
            if normal:
                # A damaged video catalog must not prevent reviewed changes to
                # other categories; video removals still require the catalog.
                rows = (list_items(identity) if any(action['kind'] == 'Videos' for action in normal)
                        else list_items(identity, category_errors=[]))
                for action in normal:
                    matches = [row for row in rows if row['kind'] == action['kind'] and
                        (row.get('key') == action.get('key') if action['action'] == 'prune-video' else
                         row['path'].casefold() == action['destination'].casefold()) and
                        row['category'] == ('library' if action['action'] == 'retire-library' else
                                            'published' if action['action'] == 'prune-video' else row['category']) and
                        row.get('removable')]
                    if len(matches) != 1:
                        raise ValueError('A reconciliation target changed. Refresh the preparation plan.')
                    selected_rows.append(matches[0])
            checkpoint()
            if selected_rows:
                removal = remove_items(identity, selected_rows)
                report['steps'].append(dict(kind='Reconciliation', actions=normal, removal=removal))
                record()
            for action in supporting:
                checkpoint()
                relative = Path(action['destination']).relative_to(root).as_posix()
                removal = remove_supporting_file(identity, action['kind'], relative,
                    expected_bytes=action['old_bytes'],
                    expected_mtime_ns=action['old_mtime_ns'],
                    expected_sha256=action['old_sha256'])
                report['steps'].append(dict(kind='Reconciliation', actions=[action], removal=removal))
                record()
            for action in reconciliation:
                if action['action'] == 'retire-library':
                    state.pop(action['kind'], None)
                elif action['action'] == 'prune-video':
                    state.pop('Videos', None)
                    saved_videos = state.get('video_files', {})
                    if isinstance(saved_videos, dict):
                        for relative, saved in list(saved_videos.items()):
                            if not isinstance(saved, dict):
                                continue
                            outputs = saved.get('outputs', [])
                            if not isinstance(outputs, list):
                                continue
                            if any(isinstance(output, (list, tuple)) and len(output) == 2
                                   and isinstance(output[0], str) and
                                   str(root / output[0]).casefold() == action['destination'].casefold()
                                   for output in outputs):
                                saved_videos.pop(relative, None)
            _save(state_path, state)
        if reused_audio:
            for source, destination in reused_audio:
                skipped['Music'].add(source.relative_to(root).as_posix().casefold())
                skipped['Music'].discard(destination.relative_to(root).as_posix().casefold())
            state.setdefault('skipped_files', {})['Music'] = sorted(skipped['Music'])
            _save(state_path, state)
        emit_stage('Copying originals', 0)
        def include_copied(items):
            changed = False
            for copied in items:
                kind = copied['kind']
                key = Path(copied['destination']).relative_to(root).as_posix().casefold()
                if key in skipped[kind]:
                    skipped[kind].remove(key)
                    changed = True
                source = Path(copied['source'])
                if 'converted_bytes' in copied and source.is_relative_to(root):
                    source_key = source.relative_to(root).as_posix().casefold()
                    if source_key not in skipped['Music']:
                        skipped['Music'].add(source_key)
                        changed = True
            if changed:
                saved_skipped = state.setdefault('skipped_files', {})
                for kind, paths in skipped.items():
                    saved_skipped[kind] = sorted(paths)
                _save(state_path, state)
        _copy_additions(plan, root, report, owned_temporaries, checkpoint, record, progress,
                        cache=cache, cancelled=cancelled, ffmpeg=ffmpeg, ffprobe=ffprobe,
                        cache_scope=identity.get('media_cache_volume_scope'),
                        completed_batch=include_copied,
                        stage_progress=lambda current, total, name:
                            emit_stage(name, 0, current / total if total else 1))
        checkpoint()
        if "Music" in selected:
            def renamed_music(old, new):
                old_key = old.relative_to(root).as_posix().casefold()
                new_key = new.relative_to(root).as_posix().casefold()
                skipped["Music"] = {new_key + key[len(old_key):]
                                    if key == old_key or key.startswith(old_key + "/") else key
                                    for key in skipped["Music"]}
                state.setdefault("skipped_files", {})["Music"] = sorted(skipped["Music"])
                _save(state_path, state)
                progress(f"Renamed for music playback: {old.name} → {new.name}")
                report["steps"].append(dict(renamed=str(old), destination=str(new)))
                record()
            if online_artwork:
                from .album_art import fetch
                report['steps'].append(dict(kind='Music', artwork_lookup=fetch(root, progress=progress,
                    checkpoint=checkpoint, retry_missing=force)))
        emit_stage('Scanning folders', 1)
        progress("Scanning the iPod folders…")
        retail_photos = None
        if "Photos" in selected:
            from .retail_media import images
            retail_photos = images(root)
        inventory = scan(root, selected, skipped=skipped, retail_photos=retail_photos,
                         fingerprint_kinds=(kind for kind in selected if kind != "Videos"),
                         checkpoint=checkpoint)
        if "Videos" in selected:
            inventory["Videos"] = _video_inventory(videos)
        for kind in KINDS:
            if kind not in selected:
                continue
            unit = 2 + 2 * selected.index(kind)
            emit_stage(f'Preparing {kind}', unit)
            checkpoint()
            item = inventory[kind]
            if not item["count"]:
                progress(f"{kind}: no supported files to prepare.")
                continue
            option = preparation_option(kind, identity, photo_dimension, music_verification)
            video_version = 6 if media_profile.get("video_backend") == "native-mp4" else 5 if media_profile.get("video_backend") == "bcm-mp4" else 4
            video_mode = (':chunked' if (video_selection is None or video_selection) and (ffmpeg or shutil.which('ffmpeg')) else ':copy') if media_profile.get('video_backend') == 'bcm-mp4' else ''
            if media_profile.get('native_video', {}).get('normalize', False):
                video_mode = ':normalized'
            if media_profile.get('video_backend') == 'ipvf':
                video_mode = ':realtime-native-key1s'
            signature = (f"videos-v{video_version}{video_mode}-{json.dumps(media_profile, sort_keys=True)}:{item['fingerprint']}" if kind == "Videos"
                         else f"{option}:{item['fingerprint']}")
            saved = state.get(kind, {})
            if not isinstance(saved, dict):
                saved = {}
            outputs = saved.get("outputs", [])
            probe = (ffprobe or shutil.which('ffprobe')) if kind == 'Music' else None
            probe_retry = bool(kind == 'Music' and saved.get('seek_probe_deferred') and probe)
            if not force and not probe_retry and not (kind == "Music" and music_verification == "full") and signature == saved.get("signature"):
                emit_stage(f'Checking saved {kind}', unit)
                checking = _checking_progress(kind, checkpoint, progress,
                    lambda fraction: emit_stage(f'Checking saved {kind}', unit, fraction))
                verified_before = verified_outputs.copy()
                if _outputs_match(root, outputs, verified=verified_outputs, progress=checking):
                    _remember_verified(identity, verified_outputs, verified_before)
                    videos_compatible = True
                    if kind == 'Videos' and media_profile['video_backend'] in ('bcm-mp4', 'native-mp4'):
                        from .native_video import inspect_file as inspect_native
                        video_outputs = [root / name for name, _ in outputs
                                         if Path(name).suffix.lower() in native_video_extensions(identity)]
                        videos_compatible = bool(video_outputs)
                        for path in video_outputs:
                            try:
                                inspect_native(path, profile=media_profile, ffprobe=ffprobe,
                                               cancelled=cancelled)
                            except (OSError, ValueError):
                                videos_compatible = False
                                break
                    if videos_compatible:
                        progress(f"{kind}: already up to date.")
                        report["steps"].append(dict(kind=kind, status="unchanged"))
                        continue
            if kind == 'Music':
                # Unchanged source fingerprints and verified outputs already
                # prove these paths were accepted by the previous compiler.
                renamed = _normalize_music_paths(root, checkpoint, renamed_music)
                prepared, probe_deferred = _prepare_audio(root, cache, skipped['Music'], probe,
                                                          checkpoint, progress, report, aac_recovery, flac_recovery, record,
                                                          stage_progress=lambda fraction: emit_stage('Preparing audio seeks', unit, fraction),
                                                          force=force, cancelled=cancelled,
                                                          audio_journal=journal_audio,
                                                          cache_scope=identity.get('media_cache_volume_scope'))
                if prepared or renamed:
                    # Renames and seek atoms change the source fingerprint.
                    item = scan(root, ('Music',), skipped=skipped, fingerprint_kinds=('Music',),
                                checkpoint=checkpoint)['Music']
                    inventory[kind] = item
                    signature = f"{option}:{item['fingerprint']}"
            progress(f"Preparing {kind.lower()} ({item['count']:,} files)…")
            publication_started = False
            try:
                def begin_publication(published_kind):
                    nonlocal publication_started, aac_publication_started
                    emit_stage(f'Publishing {published_kind}', unit + 1, can_stop=False)
                    publication_started = True
                    if published_kind == 'Music':aac_publication_started = True
                def mark_publication():
                    nonlocal publication_started
                    publication_started = True
                if kind == "Music":
                    output_paths, published, music_proofs = _prepare_music(root, cache, identity, skipped["Music"],
                        ffprobe=ffprobe, verification=music_verification, force=force,
                        cancelled=cancelled, checkpoint=checkpoint, progress=progress,
                        report=report, begin_publication=begin_publication)
                    aac_committed = True
                elif kind == "Photos":
                    output_paths, published = _prepare_photos(root, cache, identity, media_profile,
                        skipped["Photos"], retail_photos, dimension=photo_dimension,
                        checkpoint=checkpoint, progress=progress, begin_publication=begin_publication)
                elif kind == "Notes":
                    output_paths, published = _prepare_notes(root, cache, identity, skipped["Notes"],
                        checkpoint=checkpoint, progress=progress, report=report,
                        begin_publication=begin_publication)
                else:
                    output_paths, published, failed, video_proofs = _prepare_videos(
                        root, cache, identity, media_profile, state, state_path, videos, item,
                        ffmpeg=ffmpeg, ffprobe=ffprobe, force=force, cancelled=cancelled,
                        checkpoint=checkpoint, progress=progress, emit_stage=emit_stage, unit=unit,
                        record=record, report=report, verified_outputs=verified_outputs,
                        owned_temporaries=owned_temporaries, progress_detail=progress_detail,
                        mark_publication=mark_publication,
                        no_conversions=video_selection is not None and not video_selection)
                    if failed:
                        progress(f"Videos: {failed} file(s) need attention; completed videos are saved.")
                        continue
                emit_stage(f'Checking {kind}', unit + 1, 0.5)
                checkpoint()
                # Do not mark a changed source tree up to date.
                current = (_video_inventory(videos) if kind == "Videos" else
                           scan(root, (kind,), skipped=skipped, checkpoint=checkpoint)[kind])
                if current["fingerprint"] != item["fingerprint"]:
                    raise ValueError(f"{kind} changed during processing. Scan again before updating.")
                if kind == 'Photos':
                    checkpoint()
                    outputs, photo_stamps = _published_photo_output(root, identity, output_paths, published)
                elif kind == 'Music':
                    outputs, music_stamps = _music_outputs_from_publication(root, output_paths, music_proofs, identity)
                elif kind == 'Videos':
                    outputs = _video_outputs_from_publication(root, identity, output_paths, video_proofs)
                else:
                    outputs = [(name, _hash(root / name)) for name in output_paths]
                state[kind] = dict(signature=signature, outputs=outputs)
                if kind == 'Music':
                    state[kind]['seek_probe_deferred'] = probe_deferred
                if kind == "Videos":
                    state[kind]["media_profile"] = media_profile
                _save(state_path, state)
                if kind == 'Music':
                    # Reuse publication readbacks for later preparation and
                    # status checks without rereading unchanged iPod covers.
                    before = verified_outputs.copy()
                    verified_outputs.update(music_stamps)
                    _remember_verified(identity, verified_outputs, before)
                elif kind == 'Photos':
                    # Publication already hashed reused immutable tile packs.
                    before = verified_outputs.copy()
                    verified_outputs.update(photo_stamps)
                    _remember_verified(identity, verified_outputs, before)
                report["steps"].append(dict(kind=kind, status="published", publications=published))
                progress(f"{kind}: published and verified on your iPod ({root / '.LINNET'}).")
                record()
            except Cancelled:
                raise
            except Exception as error:
                if publication_started:
                    report["errors"].append(dict(kind=kind, message=str(error),
                                                 status="publication-uncertain"))
                else:
                    report["errors"].append(_error_record(kind, error, item, root))
                progress(f"{kind} could not finish: {error}")
                record()
        # File ordering, names, text wrapping and image conversion belong on
        # the host. Publish this snapshot after the media libraries and files.
        checkpoint()
        emit_stage('Preparing Files', total_stages - 2)
        from .files import compile_files, publish_files
        # Include the managed Files folder in the first snapshot. Its contents
        # are excluded by compile_files, so publication cannot add a new row
        # that forces an otherwise unchanged second update to rewrite INDEX.FIL.
        files_dir = root / '.LINNET' / 'FILES'
        _plain(files_dir, root)
        files_dir.mkdir(parents=True, exist_ok=True)
        files_package = data_cache / 'files.fil'
        next_files_guard = 0.0
        def files_checkpoint():
            nonlocal next_files_guard
            now = time.monotonic()
            if cancelled() or now >= next_files_guard:
                checkpoint()
                next_files_guard = time.monotonic() + 0.05
        files_report = compile_files(root, files_package, data_cache / 'file-previews',
            target=identity['target_id'],
            width=media_profile['photo_width'] if 'Photos' in supported_kinds(identity) else 0,
            height=media_profile['photo_height'] if 'Photos' in supported_kinds(identity) else 0,
            text_columns=20 if media_profile['photo_width'] < 220 else 24,
            checkpoint=files_checkpoint, progress=progress)
        checkpoint()
        emit_stage('Publishing Files', total_stages - 2, can_stop=False)
        files_outputs = [('.LINNET/FILES/INDEX.FIL', files_report['sha256'])]
        files_outputs.extend((item['path'], item['sha256']) for item in files_report['previews'])
        verified_before = verified_outputs.copy()
        if _outputs_match(root, files_outputs, verified=verified_outputs):
            checkpoint()
            _remember_verified(identity, verified_outputs, verified_before)
            files_publication = dict(operation='publish-files', passed=True, phase='verified',
                                     reused_index=True, previews_added=0,
                                     entries=files_report['entries'],
                                     preview_files=len(files_report['previews']))
        else:
            files_publication = publish_files(files_package, files_report, **target_args(identity))
        files_status = 'unchanged' if files_publication.get('reused_index') and not files_publication['previews_added'] else 'published'
        report['steps'].append(dict(kind='Files', status=files_status, publication=files_publication,
                                   warnings=files_report['warnings']))
        record()
        emit_stage('Finishing checks', total_stages - 2)
        report["complete"] = not report["errors"]
    except Exception as error:
        report["stopped"] = isinstance(error, Cancelled) or cancelled()
        report["errors"].append(dict(message=str(error)))
        progress(str(error))
    finally:
        emit_stage('Cleaning up', total_stages - 1, can_stop=False)
        from .aac_index import finish_preparation
        for change in aac_recovery + flac_recovery:
            try:
                _volume_guard(identity)
                _plain(Path(change['path']), root)
                _plain(Path(change['backup']), cache)
                if aac_publication_started and not aac_committed:
                    raise OSError('Music publication is uncertain; audio original retained for recovery')
                finish_preparation(change, committed=aac_committed, kind=change.get('kind', 'AAC'))
                change['outcome'] = 'published' if aac_committed else 'restored'
            except Exception as error:
                report['complete'] = False
                report['errors'].append(dict(kind='Music', message=str(error)))
                report['cleanup']['retained'].append(dict(path=change['backup'], reason=str(error)))
        for temporary, boundary in owned_temporaries:
            try:
                if boundary == root:
                    _volume_guard(identity)
                _plain(temporary, boundary)
                if temporary.exists():
                    temporary.unlink()
                    report["cleanup"]["removed"].append(str(temporary))
            except Exception as error:
                report["cleanup"]["retained"].append(dict(path=str(temporary), reason=str(error)))
        # Audit scratch locations without deleting files that could belong to another run.
        publications = []
        publication_name = re.compile(r'publish-.*\.json', re.DOTALL | (re.IGNORECASE if os.name == 'nt' else 0)).fullmatch
        for path in (path for base in dict.fromkeys((cache, data_cache)) for path in base.rglob('*')):
            name = path.name.casefold()
            if publication_name(path.name):
                publications.append(path)
            if (name.startswith(('ipvf-encode-', 'music-prepare-')) or
                    '.music-prepare-' in name or name.endswith((
                        '.tmp', '.music-previous', '.aac-original', '.aac-append', '.flac-original'))):
                report["cleanup"]["retained"].append(dict(path=str(path),
                    reason="Preparation scratch remains; review before removing if another update is running."))
        for path in publications:
            try:
                publication = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(publication, dict):
                    raise ValueError("invalid publication receipt")
                warnings = publication.get("cleanup_warnings", [])
                if not isinstance(warnings, list) or any(
                        not isinstance(warning, dict) or
                        any(not isinstance(warning.get(key), str) for key in ("path", "error"))
                        for warning in warnings):
                    raise ValueError("invalid publication cleanup warnings")
                for warning in warnings:
                    report["cleanup"]["retained"].append(dict(path=warning["path"], reason=warning["error"]))
                if not publication.get("passed"):
                    report["cleanup"]["retained"].append(dict(path=str(path),
                        reason="Incomplete publication receipt: inspect its temporary and backup paths before cleanup."))
            except (OSError, ValueError):
                report["cleanup"]["retained"].append(dict(path=str(path), reason="Unreadable publication receipt; review required."))
        count = len(report["cleanup"]["removed"])
        progress(f"Cleanup: removed {count} temporary file(s).")
        for item in report["cleanup"]["retained"]:
            progress(f"Retained for review: {item['path']} — {item['reason']}")
        if not report["cleanup"]["retained"]:
            progress("Cleanup: no preparation scratch files remain.")
        report['source_outcomes'] = _pending_outcomes(pending, selected, plan, videos,
                                                      report['steps'], report['errors'], planned)
        ready = [kind for kind in KINDS if any(step.get("kind") == kind and
                 step.get("status") in ("published", "unchanged") for step in report["steps"])]
        failed = list(dict.fromkeys(error["kind"] for error in report["errors"] if error.get("kind")))
        summary = []
        if ready:
            summary.append(f"On your iPod ({root}): {', '.join(ready)}.")
        if failed:
            summary.append(f"Could not finish: {', '.join(failed)}. Open report for the errors.")
        for step in report['steps']:
            artwork = step.get('artwork_lookup')
            if artwork:
                summary.append(f"Album covers: {artwork['downloaded']} downloaded; {artwork.get('cached', 0)} reused; {artwork['unmatched']} without a confident match.")
                if artwork['errors']:
                    summary.append('Artwork service unavailable; cached covers were kept. Prepare again to retry.')
        if report["stopped"]:
            summary.append("Update stopped; completed work is retained on the iPod.")
        elif report["errors"] and not failed:
            summary.append("Update could not finish. Open report for the error.")
        report["summary"] = " ".join(summary)
        progress(f"Computer cache for faster retries (separate from the published iPod files): {data_cache}")
        if report["summary"]:
            progress(report["summary"])
        report["finished"] = time.time()
        record()
        try:
            audio_journal_path.unlink(missing_ok=True)
        except OSError as error:
            report['cleanup']['retained'].append(dict(path=str(audio_journal_path), reason=str(error)))
            try:
                record()
            except OSError:
                pass  # The prior receipt is durable; return the retained path in this report.
    return report
