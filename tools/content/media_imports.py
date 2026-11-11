"""Source import planning, duplicate detection, and bounded verified copy batches."""
from __future__ import annotations

import hashlib
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import threading
import time
import unicodedata
import uuid
from .guard import guarded_path as _plain, hash_file as _hash
from .host_state import (
    cache_data_directory,
    file_stamp as _receipt_stamp,
    load_json_object as _optional_state,
)
from .inventory import KINDS, extensions, file_records as _file_records, source_folders
from .media_audio import (
    _convert_audio,
    _import_metadata_seed,
    _prepare_import_aac,
    _prepare_import_flac,
    _prepare_import_mp3,
    _save_seek_state,
    _seed_imported_music,
    _seek_stamp,
    _verified_audio_hash,
)
from .media_options import Cancelled, _RECENT_OUTPUT_NS
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from pathlib import Path


def _music_import_name(name, preserved_suffix=None):
    # Non-ASCII FAT aliases cannot be opened by the firmware. Keep readable
    # names, transliterating accents and punctuation before Windows makes aliases.
    if name.isascii():
        result = name.replace('"', "'")
    else:
        punctuation = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"',
                                    "‐": "-", "‑": "-", "–": "-", "—": "-", "…": "..."})
        normalized = unicodedata.normalize("NFD", name.translate(punctuation))
        normalized = "".join(c for c in normalized if not unicodedata.combining(c))
        if normalized.isascii():
            result = normalized.replace('"', "'")
        else:
            # A readable ASCII prefix also gives names in other scripts a usable alias.
            result = "Music - " + name
    if len(result) > 127 and len(result.encode('utf-16-le')) > 510:
        suffix = preserved_suffix or Path(result).suffix
        marker = '-' + hashlib.sha256(name.encode('utf-8')).hexdigest()[:12]
        budget = 255 - len((marker + suffix).encode('utf-16-le')) // 2
        if budget < 1:
            raise ValueError('Music filename extension is too long; rename it before importing.')
        stem = result[:-len(suffix)] if suffix else result
        stem = stem.encode('utf-16-le')[:budget * 2].decode('utf-16-le', errors='ignore')
        result = stem + marker + suffix
    return result


def _normalize_music_paths(root, checkpoint, renamed):
    from .music import _device_media_path, UnsupportedMediaPath
    resolved_root = Path(root).resolve(strict=True)
    alias_cache = {}
    changed = False
    directories = source_folders(root, "Music")
    while directories:
        folder = directories.pop()
        _plain(folder, root)
        with os.scandir(folder) as iterator:
            entries = sorted(((folder / entry.name, entry) for entry in iterator),
                             key=lambda item: item[0])
        for path, entry in entries:
            checkpoint()
            # The parent was checked above. DirEntry keeps the leaf's file
            # attributes from enumeration, including Windows junctions.
            info = entry.stat(follow_symlinks=False)
            if (stat.S_ISLNK(info.st_mode) or
                    getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
                raise ValueError(f"reparse/symlink path rejected: {path}")
            directory = entry.is_dir(follow_symlinks=False)
            if not directory and path.suffix.lower() not in extensions("Music"):
                continue
            try:
                _device_media_path(path, resolved_root, alias_cache)
            except UnsupportedMediaPath:
                name = _music_import_name(path.name)
                if name == path.name:
                    raise ValueError(f"Music path is too long or deep; move it closer to /Music: {path}")
                destination = path.with_name(name)
                number = 2
                while destination.exists():
                    suffix = '' if directory else Path(name).suffix
                    stem = name[:-len(suffix)] if suffix else name
                    ending = f" ({number}){suffix}"
                    budget = 255 - len(ending.encode('utf-16-le')) // 2
                    stem = stem.encode('utf-16-le')[:budget * 2].decode('utf-16-le', errors='ignore')
                    candidate = stem + ending
                    destination = path.with_name(candidate)
                    number += 1
                _plain(destination, root)
                checkpoint()
                path.rename(destination)
                changed = True
                renamed(path, destination)
                path = destination
                _device_media_path(path, resolved_root, alias_cache)
            if directory:
                directories.append(path)
    return changed


def _planning_records(source, snapshots, *, checkpoint):
    checkpoint()
    if snapshots is not None and source in snapshots:
        return snapshots[source]
    records = (_file_records(source, checkpoint=checkpoint) if source.is_dir() else
               [(source, source.stat())])
    checkpoint()
    if snapshots is not None:
        snapshots[source] = records
    return records


def _same_file_bytes(left, right, size, digest=None, stable_right=None, *, checkpoint=lambda: None):
    checkpoint()
    paths = (left, right)
    before = [path.stat() for path in paths]
    if any(not stat.S_ISREG(info.st_mode) or info.st_size != size for info in before):
        raise ValueError('file changed while comparing existing media')
    equal = True
    with left.open('rb') as source, right.open('rb') as destination:
        remaining, chunk_size = size, 16 * 1024
        while remaining:
            checkpoint()
            count = min(remaining, chunk_size)
            a, b = source.read(count), destination.read(count)
            if len(a) != count or len(b) != count:
                raise ValueError('file changed while comparing existing media')
            if a != b:
                equal = False
                break
            if digest is not None:
                digest.update(a)
            remaining -= count
            chunk_size = 1024 * 1024
        if equal and (source.read(1) or destination.read(1)):
            raise ValueError('file changed while comparing existing media')
    checkpoint()
    after = [path.stat() for path in paths]
    if any((a.st_size, a.st_mtime_ns, a.st_ctime_ns, a.st_ino) !=
           (b.st_size, b.st_mtime_ns, b.st_ctime_ns, b.st_ino)
           for a, b in zip(before, after)):
        raise ValueError('file changed while comparing existing media')
    if stable_right is not None:
        stable_right.append(after[1])
    return equal


def _same_existing_file(source, destination, size, root, verified, known, info, *, checkpoint=lambda: None):
    checkpoint()
    relative = destination.relative_to(root).as_posix()
    stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
    recent = os.name == 'nt' and info.st_mtime_ns > time.time_ns() - _RECENT_OUTPUT_NS
    if not recent and stamp in known.get(relative, {}):
        before = source.stat()
        if not stat.S_ISREG(before.st_mode) or before.st_size != size:
            raise ValueError('file changed while comparing existing media')
        digest = _hash(source, progress=lambda _current, _total: checkpoint())
        after = source.stat()
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns, before.st_ino) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino):
            raise ValueError('file changed while comparing existing media')
        current = destination.stat()
        if stamp != (current.st_size, current.st_mtime_ns, current.st_ctime_ns, current.st_ino):
            raise ValueError('file changed while comparing existing media')
        valid = {value for value in known[relative][stamp]
                 if verified.get((relative, value)) == stamp}
        if valid:
            return digest in valid
    digest, stable_right = hashlib.sha256(), []
    same = _same_file_bytes(source, destination, size, digest, stable_right, checkpoint=checkpoint)
    current = stable_right[0]
    if same and not recent and stamp == (
            current.st_size, current.st_mtime_ns, current.st_ctime_ns, current.st_ino):
        value = digest.hexdigest()
        verified[(relative, value)] = stamp
        known.setdefault(relative, {}).setdefault(stamp, set()).add(value)
    return same


def additions(root, pending, *, snapshots=None, verify_existing=True, unverified=None,
              replacements=None, convert_audio=False, target=None, ffprobe=None,
              cancelled=lambda: False, reused_audio=None, verified=None):
    """Resolve selected files/folders to concrete destinations without writing."""
    root = Path(root).resolve(strict=True)
    planned = {}
    known = {}

    def check():
        if cancelled():
            raise Cancelled('Stopped while checking queued media.')

    check()
    if verified is not None:
        for (relative, digest), stamp in verified.items():
            if isinstance(relative, str) and isinstance(digest, str) and re.fullmatch('[0-9a-f]{64}', digest):
                known.setdefault(relative, {}).setdefault(stamp, set()).add(digest)
    for kind, selected in pending:
        check()
        if kind not in KINDS:
            raise ValueError("Unknown media folder")
        source = Path(selected).absolute()
        _plain(source, Path(source.anchor))
        source = source.resolve(strict=True)
        on_device = source.is_relative_to(root)
        music_source = next((folder for folder in (root / 'Music', root / 'iPod_Control' / 'Music')
                             if source.is_relative_to(folder)), None)
        if on_device and not (kind == 'Music' and convert_audio and source.is_file() and
                              music_source is not None and not source.name.casefold().endswith('.linnet.wav')):
            raise ValueError(f"{source.name} is already on this iPod. Use its existing {kind} folder.")
        is_folder = source.is_dir()
        records = _planning_records(source, snapshots, checkpoint=check)
        for path, info in records:
            check()
            # Preserve supporting files (cover art, playlists) inside selected folders.
            if not is_folder and path.suffix.lower() not in extensions(kind):
                raise ValueError(f"{path.name} is not a supported {kind.lower()} source.")
            if info.st_size > 0xFFFFFFFF:
                raise ValueError(f"{path.name} exceeds the iPod's FAT32 file size limit (4 GiB minus 1 byte).")
            relative = (path.relative_to(music_source) if on_device else
                        Path(source.name) / path.relative_to(source) if is_folder else Path(source.name))
            if kind == "Music":
                relative = Path(*(_music_import_name(part) for part in relative.parts))
            converted_bytes = None
            converted_frames = None
            issue = None
            convert = False
            if target and kind == 'Music' and path.suffix.lower() in extensions('Music'):
                from .music import CODECS, _decoded_frames, _pcm, _quick_metadata, _probe, _read_metadata, compatibility_issue
                try:
                    if path.suffix.lower() in ('.wav', '.aif', '.aiff', '.aifc'):
                        try:
                            metadata = _pcm(path, cancelled)
                        except (ValueError, struct.error):
                            if not ffprobe:
                                raise
                            metadata = _probe(path, ffprobe, exact_frames=False,
                                              cancelled=cancelled, allow_unknown=True)
                    else:
                        metadata = _quick_metadata(path, cancelled, allow_unknown=True)
                    if metadata is None:
                        if not ffprobe:
                            raise ValueError('FFprobe is needed to check this audio file')
                        metadata = _probe(path, ffprobe, exact_frames=False,
                                          cancelled=cancelled, allow_unknown=True)
                    issue = compatibility_issue(metadata, target)
                    # Prepared PCM removes compressed-audio decoding from the
                    # playback CPU on every target, even for supported codecs.
                    convert = convert_audio and (issue or on_device or metadata['codec'] not in
                                                 (CODECS['pcm_le'], CODECS['pcm_be']))
                    if convert:
                        if metadata['codec'] == CODECS['unknown']:
                            if not ffprobe:
                                raise ValueError('FFprobe is needed to size audio conversion')
                            metadata = _probe(path, ffprobe, exact_frames=True, cancelled=cancelled,
                                              allow_unknown=True)
                            issue = compatibility_issue(metadata, target)
                        elif metadata['codec'] == CODECS['mp3']:
                            if not ffprobe:
                                raise ValueError('FFprobe is needed to size MP3 conversion')
                            metadata['frames'] = _decoded_frames(path, ffprobe, codec=metadata['codec'],
                                                                  cancelled=cancelled)
                        elif not metadata['frames']:
                            metadata = _read_metadata(path, ffprobe, True, cancelled)
                except (OSError, ValueError, struct.error, subprocess.SubprocessError) as error:
                    metadata = None
                    issue = f'Cannot inspect audio: {error}'
                if convert and metadata is not None:
                    relative = (relative.with_name(_music_import_name(
                        relative.name + '.linnet.wav', preserved_suffix='.linnet.wav'))
                                if on_device else relative.with_suffix('.wav'))
                    converted_frames = math.ceil(metadata['frames'] * 44100 / metadata['rate'])
                    converted_bytes = (math.ceil(converted_frames * 4 * 1.1)
                                       + min(info.st_size, 32 * 1024 * 1024) + 4096)
                    if converted_bytes > 0xffffffff:
                        raise ValueError(f'{path.name} would exceed the iPod file limit after PCM conversion.')
                after = path.stat()
                if (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (info.st_size, info.st_mtime_ns, info.st_ctime_ns):
                    raise ValueError(f'Source changed during audio planning: {path}')
            if on_device and converted_bytes is None:
                if issue:
                    from . import SourceError
                    raise SourceError(f'{path}: {issue}', path)
                continue
            destination = root / kind / relative
            _plain(destination, root)
            key = str(destination).casefold()
            if key in planned and planned[key]["source"] != str(path):
                raise ValueError(f"Two selected files have the same destination: {destination}")
            if destination.exists():
                if not destination.is_file():
                    raise ValueError(f"A different file already exists: {destination}. Rename the new file or folder first.")
                if converted_bytes is not None:
                    from .music import CODECS, _pcm, compatibility_issue
                    try:
                        prepared = _pcm(destination)
                        if (prepared['container'] == 'wav' and prepared['codec'] == CODECS['pcm_le'] and
                                prepared['bits'] == 16 and prepared['channels'] == 2 and
                                prepared['rate'] == 44100 and not compatibility_issue(prepared, target) and
                                prepared['frames'] >= converted_frames - max(2205, converted_frames // 100) and
                                _verified_audio_hash(path, root, prepared['tags'].get('source_sha256'), verified, checkpoint=check) and
                                _verified_audio_hash(destination, root, prepared['tags'].get('pcm_sha256'),
                                                     verified, prepared, checkpoint=check)):
                            if on_device and reused_audio is not None:
                                reused_audio.append((path, destination))
                            continue
                    except (OSError, ValueError, KeyError, struct.error):
                        pass
                old = destination.stat()
                different = converted_bytes is not None
                if not different and not verify_existing:
                    # Planning only estimates work. The preparation call checks
                    # the complete original before it copies or publishes.
                    if old.st_size != info.st_size and not (
                            kind == 'Music' and path.suffix.lower() in ('.m4a', '.m4b', '.flac', '.flc', '.mp3')):
                        different = True
                    elif unverified is not None:
                        unverified.append(kind)
                elif not different and (old.st_size != info.st_size or not (
                        _same_existing_file(path, destination, info.st_size, root, verified, known, old, checkpoint=check)
                        if verified is not None else _same_file_bytes(path, destination, info.st_size, checkpoint=check))):
                    equivalent = False
                    if kind == 'Music' and path.suffix.lower() in ('.m4a', '.m4b'):
                        from .aac_index import same_original
                        equivalent = same_original(path, destination, checkpoint=check)
                    elif kind == 'Music' and path.suffix.lower() in ('.flac', '.flc'):
                        from .flac_index import same_original
                        equivalent = same_original(path, destination, checkpoint=check)
                    elif kind == 'Music' and path.suffix.lower() == '.mp3':
                        from .mp3_index import same_original
                        equivalent = same_original(path, destination, checkpoint=check)
                    different = not equivalent
                if not different:
                    continue
                if replacements is None:
                    raise ValueError(f"A different file already exists: {destination}. Review its replacement before preparing.")
                replacements.append(dict(action='replace', kind=kind, source=str(path),
                    destination=str(destination), old_bytes=old.st_size,
                    old_mtime_ns=old.st_mtime_ns, new_bytes=info.st_size,
                    new_mtime_ns=info.st_mtime_ns,
                    old_sha256=_hash(destination, progress=lambda _current, _total: check()),
                    new_sha256=_hash(path, progress=lambda _current, _total: check())))
            planned[key] = dict(kind=kind, source=str(path), destination=str(destination),
                                bytes=info.st_size, mtime_ns=info.st_mtime_ns,
                                **({'compatibility_issue': issue} if issue else {}),
                                **({'converted_bytes': converted_bytes, 'converted_frames': converted_frames}
                                   if converted_bytes is not None else {}))
    reserved = {str(value).casefold() for source, destination in (reused_audio or ())
                for value in (source, destination)}
    reserved.update(str(item['source']).casefold() for item in planned.values()
                    if 'converted_bytes' in item and Path(item['source']).is_relative_to(root))
    if reserved.intersection(planned):
        raise ValueError('Queued sources overlap an iPod conversion. Remove one before preparing.')
    check()
    return list(planned.values())


def _copy_additions(plan, root, report, owned_temporaries, checkpoint, record, progress,
                    *, cache, cancelled, ffmpeg=None, ffprobe=None,
                    cache_scope=None,
                    stage_progress=lambda current, total, phase: None,
                    completed_batch=lambda items: None):
    # Queue a bounded set of writes before flushing and reading them back. On
    # FAT devices, alternating write/flush/read/rename for every small file is
    # expensive. Keep every handle until its flush, and publish only verified files.
    start = 0
    remaining_original = sum(item.get('converted_bytes', item['bytes']) for item in plan)
    seek_state_path = cache_data_directory(cache) / 'seek-checked.json'
    seek_state = (_optional_state(seek_state_path) if any(
        item['kind'] == 'Music' and Path(item['destination']).suffix.lower() in ('.m4a', '.m4b', '.flac', '.flc')
        for item in plan) else {})
    device_keys = {str(Path(item['source'])).casefold() for item in plan if 'converted_bytes' in item and
                   Path(item['source']).is_relative_to(root)}
    retail_rows, retail_artwork = {}, None
    if device_keys:
        from .retail_media import available_tracks
        tracks = available_tracks(root)
        retail_rows = {key: tracks[key] for key in device_keys if key in tracks}
    while start < len(plan):
        batch = []
        batch_bytes = 0
        seek_state_changed = False
        for item in plan[start:start + 16]:
            if batch and batch_bytes + item.get('converted_bytes', item['bytes']) > 64 * 1024 * 1024:
                break
            destination = Path(item['destination'])
            temporary = destination.parent / ('L' + uuid.uuid4().hex[:7] + '.TMP')
            batch.append((item, temporary))
            batch_bytes += item.get('converted_bytes', item['bytes'])
        checkpoint()
        # Persist all candidate scratch paths before creating any of them. The
        # receipt stays bounded during a batch instead of growing twice per song.
        report['copies_in_progress'] = [dict(source=item['source'], destination=item['destination'],
                                           temporary=str(temporary)) for item, temporary in batch]
        record()
        copied = []
        music_seeds = []
        with ExitStack() as handles:
            aac_positions = [offset for offset, (item, _) in enumerate(batch)
                             if item['kind'] == 'Music' and
                             Path(item['destination']).suffix.lower() in ('.m4a', '.m4b')]
            mp3_positions = [offset for offset, (item, _) in enumerate(batch)
                             if item['kind'] == 'Music' and
                             Path(item['destination']).suffix.lower() == '.mp3']
            flac_positions = [offset for offset, (item, _) in enumerate(batch)
                              if item['kind'] == 'Music' and
                              Path(item['destination']).suffix.lower() in ('.flac', '.flc')]
            preparation_positions = sorted((*aac_positions, *mp3_positions, *flac_positions))
            scratches = {offset: Path(handles.enter_context(tempfile.TemporaryDirectory(
                prefix='audio-source-', dir=cache))) for offset in preparation_positions}
            stopped = threading.Event()
            if aac_positions:
                from .aac_index import executable
                executable()
            if mp3_positions and any(batch[offset][0]['bytes'] >= 8 * 1024 * 1024 for offset in mp3_positions):
                from .mp3_index import executable
                executable()
            executor = handles.enter_context(ThreadPoolExecutor(max_workers=2)) if preparation_positions else None
            if executor is not None:
                handles.callback(stopped.set)
            upcoming = iter(preparation_positions)
            futures = {}
            seek_probe = (ffprobe or shutil.which('ffprobe')) if preparation_positions else None
            def fill_preparation():
                while executor is not None and len(futures) < 2:
                    offset = next(upcoming, None)
                    if offset is None:
                        break
                    if offset in aac_positions:
                        futures[offset] = executor.submit(_prepare_import_aac, batch[offset][0],
                            scratches[offset], cache, seek_probe, cancelled, stopped)
                    elif offset in flac_positions:
                        futures[offset] = executor.submit(_prepare_import_flac, batch[offset][0],
                            scratches[offset], cache, seek_probe, cancelled, stopped)
                    else:
                        futures[offset] = executor.submit(_prepare_import_mp3, batch[offset][0],
                            scratches[offset], seek_probe, cancelled, stopped)
            fill_preparation()
            try:
                for offset, (item, temporary) in enumerate(batch):
                    checkpoint()
                    stage_progress(start + offset / 2, len(plan), 'Copying originals')
                    source, destination = Path(item['source']), Path(item['destination'])
                    progress(f"Adding {start + offset + 1}/{len(plan)}: {source.name}")
                    _plain(source, Path(source.anchor))
                    _plain(destination, root)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    _plain(destination.parent, root)
                    info = source.stat()
                    if (info.st_size, info.st_mtime_ns) != (item['bytes'], item['mtime_ns']):
                        raise ValueError(f"Source changed since selection: {source}")
                    suffix = None
                    unsupported_aac = False
                    metadata_seed = None
                    flac_seek_checked = False
                    copy_source = source
                    if offset in futures:
                        prepared = futures.pop(offset).result()
                        fill_preparation()
                        if offset in aac_positions:
                            copy_source, suffix, unsupported_aac, metadata_seed, source_stamp = prepared
                        elif offset in flac_positions:
                            copy_source, metadata_seed, flac_seek_checked, source_stamp = prepared
                        else:
                            copy_source, suffix, metadata_seed, source_stamp = prepared
                        if _receipt_stamp(info) != source_stamp:
                            raise ValueError(f"Source changed during preparation: {source}")
                        checkpoint()
                    if 'converted_bytes' in item:
                        scratch = Path(handles.enter_context(tempfile.TemporaryDirectory(prefix='audio-convert-', dir=cache)))
                        copy_source = scratch / 'audio.wav'
                        progress(f'Converting on computer: {source.name}')
                        seed = _import_metadata_seed(source, ffprobe, cancelled)
                        saved = retail_rows.get(str(source).casefold())
                        if saved:
                            metadata, cover = seed or ({'tags': {}}, None)
                            metadata['tags'] = {**{key: str(value) for key, value in saved.items()
                                                   if key in ('title', 'artist', 'album', 'album_artist',
                                                              'genre', 'composer', 'date', 'year', 'track',
                                                              'disc', 'compilation') and value},
                                                **{key: value for key, value in metadata['tags'].items() if value}}
                            if cover is None:
                                if retail_artwork is None:
                                    from .retail_media import metadata as retail_metadata
                                    retail_artwork = retail_metadata(root, tracks_rows=retail_rows)
                                artwork = retail_artwork.get(str(source).casefold(), {}).get('artwork')
                                if artwork:
                                    art_path = Path(artwork)
                                    if art_path.is_file() and 0 < art_path.stat().st_size <= 32 * 1024 * 1024:
                                        cover = art_path.read_bytes()
                            seed = metadata, cover
                        conversion_source, source_sha256 = source, None
                        if source.is_relative_to(root):
                            conversion_source = scratch / ('source' + source.suffix)
                            digest = hashlib.sha256()
                            with source.open('rb') as reader, conversion_source.open('xb') as frozen:
                                while chunk := reader.read(1024 * 1024):
                                    checkpoint()
                                    frozen.write(chunk)
                                    digest.update(chunk)
                            if (conversion_source.stat().st_size != info.st_size or
                                    _receipt_stamp(source.stat()) != _receipt_stamp(info)):
                                raise ValueError(f'Source changed during conversion: {source}')
                            source_sha256 = digest.hexdigest()
                        converted = _convert_audio(conversion_source, copy_source, ffmpeg, checkpoint,
                                                   item['converted_bytes'], item['converted_frames'],
                                                   seed[0]['tags'] if seed else None,
                                                   seed[1] if seed else None, source_sha256)
                        if conversion_source != source:
                            conversion_source.unlink()
                        if copy_source.stat().st_size > 0xffffffff:
                            raise ValueError(f'Converted audio exceeds the iPod file limit: {source}')
                        converted['tags'] = {**converted['tags'], **(seed[0]['tags'] if seed else {})}
                        metadata_seed = (converted, seed[1] if seed else None)
                        if _receipt_stamp(source.stat()) != _receipt_stamp(info):
                            raise ValueError(f'Source changed during conversion: {source}')
                    target_bytes = copy_source.stat().st_size + len(suffix or b'')
                    if shutil.disk_usage(root).free < remaining_original - item.get('converted_bytes', item['bytes']) + target_bytes + 1024 * 1024:
                        raise ValueError('Not enough space for prepared additions')
                    digest = hashlib.sha256()
                    writer = handles.enter_context(temporary.open('xb'))
                    owned_temporaries.append((temporary, root))
                    with copy_source.open('rb') as reader:
                        while chunk := reader.read(1024 * 1024):
                            checkpoint()
                            writer.write(chunk)
                            digest.update(chunk)
                    if suffix:
                        writer.write(suffix)
                        digest.update(suffix)
                    writer.flush()
                    after = source.stat()
                    if _receipt_stamp(after) != _receipt_stamp(info):
                        raise ValueError(f"Source changed during copy: {source}")
                    copied.append((item, temporary, writer, digest.hexdigest(),
                                   copy_source.stat().st_size + len(suffix or b''), unsupported_aac,
                                   metadata_seed, flac_seek_checked))
                    remaining_original -= item.get('converted_bytes', item['bytes'])
            except BaseException:
                stopped.set()
                for future in futures.values():
                    future.cancel()
                raise
            progress(f"Saving additions {start + 1}–{start + len(batch)}/{len(plan)}…")
            for _, _, writer, _, _, _, _, _ in copied:
                checkpoint()
                os.fsync(writer.fileno())
        verified_stamps = {}
        for offset, (item, temporary, _, digest, prepared_bytes, _, _, flac_seek_checked) in enumerate(copied):
            checkpoint()
            stage_progress(start + (len(batch) + offset) / 2, len(plan), 'Checking copied files')
            destination = Path(item['destination'])
            progress(f"Verifying addition {start + offset + 1}/{len(plan)}: {destination.name}")
            _plain(temporary, root)
            before_readback = _receipt_stamp(temporary.stat())
            if _hash(temporary, prepared_bytes, progress=lambda _current, _total: checkpoint()) != digest:
                raise OSError(f"Copy verification failed: {destination}")
            after_readback = _receipt_stamp(temporary.stat())
            if before_readback != after_readback:
                raise OSError(f"Copied file changed during readback: {destination}")
            verified_stamps[temporary] = after_readback
            if prepared_bytes != item['bytes'] and not flac_seek_checked and 'converted_bytes' not in item:
                if destination.suffix.lower() == '.mp3':
                    from .mp3_index import original_size
                    valid = original_size(temporary, prepared_bytes, checkpoint=checkpoint) == item['bytes']
                else:
                    from .aac_index import original_size
                    valid = original_size(temporary, prepared_bytes, checkpoint=checkpoint) == item['bytes']
                if not valid:
                    raise ValueError(f"Prepared audio seek data is invalid: {destination}")
        for item, temporary, _, digest, prepared_bytes, unsupported_aac, metadata_seed, flac_seek_checked in copied:
            checkpoint()
            destination = Path(item['destination'])
            _plain(destination, root)
            # Windows rename refuses an existing destination, preserving both files.
            if destination.exists():
                raise ValueError(f"Destination appeared during copy: {destination}")
            if _receipt_stamp(temporary.stat()) != verified_stamps[temporary]:
                raise OSError(f"Copied file changed before rename: {destination}")
            temporary.rename(destination)
            owned_temporaries.remove((temporary, root))
            info = destination.stat()
            if _receipt_stamp(info) != verified_stamps[temporary]:
                before_recheck = _receipt_stamp(info)
                if info.st_size != prepared_bytes or _hash(
                        destination, prepared_bytes, progress=lambda _current, _total: checkpoint()) != digest:
                    raise OSError(f"Copied file changed after readback: {destination}")
                info = destination.stat()
                if _receipt_stamp(info) != before_recheck:
                    raise OSError(f"Copied file changed during final readback: {destination}")
            if unsupported_aac:
                from .aac_index import mark_unsupported
                mark_unsupported(destination, cache / 'aac-seek')
            if item['kind'] == 'Music' and destination.suffix.lower() in ('.m4a', '.m4b'):
                key = destination.relative_to(root).as_posix().casefold()
                seek_state[key] = _seek_stamp(destination, info)
                seek_state_changed = True
                if metadata_seed is not None:
                    music_seeds.append((destination, info, *metadata_seed))
            elif item['kind'] == 'Music' and destination.suffix.lower() == '.mp3' and metadata_seed is not None:
                music_seeds.append((destination, info, *metadata_seed))
            elif item['kind'] == 'Music' and 'converted_bytes' in item and metadata_seed is not None:
                music_seeds.append((destination, info, *metadata_seed))
            elif item['kind'] == 'Music' and destination.suffix.lower() in ('.flac', '.flc'):
                if flac_seek_checked:
                    key = destination.relative_to(root).as_posix().casefold()
                    seek_state[key] = _seek_stamp(destination, info)
                    seek_state_changed = True
                if metadata_seed is not None:
                    music_seeds.append((destination, info, *metadata_seed))
            report['steps'].append(dict(added=str(destination), bytes=prepared_bytes,
                                        **({'converted_from': str(item['source'])} if 'converted_bytes' in item else {})))
            completed_batch([item])
        report.pop('copies_in_progress', None)
        record()
        if seek_state_changed:
            _save_seek_state(seek_state_path, seek_state)
        if music_seeds:
            _seed_imported_music(root, cache, music_seeds, cache_scope=cache_scope)
        start += len(batch)
        stage_progress(start, len(plan), 'Copying originals')
