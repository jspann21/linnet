"""Imported audio conversion, verification, metadata seeding, and seek preparation."""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
from .guard import guarded_path as _plain, hash_file as _hash
from .host_state import (
    cache_data_directory,
    file_stamp as _receipt_stamp,
    load_json_object as _optional_state,
    save_json as _save,
)
from .inventory import source_folders
from .media_options import Cancelled, _RECENT_OUTPUT_NS
from pathlib import Path


def _seek_stamp(path, info):
    stamp = [info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino]
    if path.suffix.lower() in ('.m4a', '.m4b'):
        from .aac_index import PREPARATION_VERSION
        stamp.append(PREPARATION_VERSION)
    return stamp


def _save_seek_state(path, state):
    try:
        _save(path, state)
    except OSError:
        pass  # Seek checks are disposable; publication and recovery continue.


def _append_audio_journal(path, kind, change):
    entry = json.dumps(dict(kind=kind, change=change), separators=(',', ':')).encode('utf-8') + b'\n'
    with path.open('ab') as writer:
        writer.write(entry)
        writer.flush()
        os.fsync(writer.fileno())


def _import_metadata_seed(local, ffprobe, should_stop):
    from .music import _embedded_cover, _quick_metadata, _read_metadata, _probe
    try:
        metadata = _read_metadata(local, ffprobe, True, should_stop, include_cover=True)
    except (OSError, ValueError, subprocess.SubprocessError):
        metadata = _quick_metadata(local, should_stop, include_cover=True, allow_unknown=True)
        if metadata is None and ffprobe:
            try:
                metadata = _probe(local, ffprobe, exact_frames=False, cancelled=should_stop,
                                  allow_unknown=True)
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
        if metadata is not None and '_embedded_cover' not in metadata:
            metadata['_embedded_cover'] = _embedded_cover(local)
    return (metadata, metadata.pop('_embedded_cover', None)) if metadata else None


def _prepare_import_aac(item, scratch, cache, ffprobe, cancelled, stopped):
    from .aac_index import import_suffix

    def should_stop():
        return stopped.is_set() or cancelled()

    def check():
        if should_stop():
            raise Cancelled('Stopped while preparing AAC on the computer.')

    source = Path(item['source'])
    _plain(source, Path(source.anchor))
    before = source.stat()
    if (before.st_size, before.st_mtime_ns) != (item['bytes'], item['mtime_ns']):
        raise ValueError(f"Source changed since selection: {source}")
    local = scratch / ('source' + source.suffix.lower())
    with source.open('rb') as reader, local.open('xb') as frozen:
        while chunk := reader.read(1024 * 1024):
            check()
            frozen.write(chunk)
    if (_receipt_stamp(before) != _receipt_stamp(source.stat()) or
            local.stat().st_size != before.st_size):
        raise ValueError(f"Source changed during preparation: {source}")
    suffix, unsupported = import_suffix(local, cache / 'aac-import', checkpoint=check)
    metadata_seed = _import_metadata_seed(local, ffprobe, should_stop)
    check()
    return local, suffix, unsupported, metadata_seed, _receipt_stamp(before)


def _prepare_import_mp3(item, scratch, ffprobe, cancelled, stopped):
    def should_stop():
        return stopped.is_set() or cancelled()
    def check():
        if should_stop():
            raise Cancelled('Stopped while preparing MP3 on the computer.')

    source = Path(item['source'])
    _plain(source, Path(source.anchor))
    before = source.stat()
    if (before.st_size, before.st_mtime_ns) != (item['bytes'], item['mtime_ns']):
        raise ValueError(f"Source changed since selection: {source}")
    local = source
    suffix = b''
    if before.st_size >= 8 * 1024 * 1024:
        local = scratch / 'source.mp3'
        with source.open('rb') as reader, local.open('xb') as frozen:
            while chunk := reader.read(1024 * 1024):
                check()
                frozen.write(chunk)
        if (_receipt_stamp(before) != _receipt_stamp(source.stat()) or
                local.stat().st_size != before.st_size):
            raise ValueError(f"Source changed during preparation: {source}")
        from .mp3_index import import_suffix
        suffix = import_suffix(local, checkpoint=check)
    metadata_seed = _import_metadata_seed(local, ffprobe, should_stop)
    if should_stop():
        raise Cancelled('Stopped while reading MP3 metadata on the computer.')
    if _receipt_stamp(source.stat()) != _receipt_stamp(before):
        raise ValueError(f"Source changed during preparation: {source}")
    return local, suffix, metadata_seed, _receipt_stamp(before)


def _prepare_import_flac(item, scratch, cache, ffprobe, cancelled, stopped):
    from .flac_index import prepare_file, same_original

    def should_stop():
        return stopped.is_set() or cancelled()

    def check():
        if should_stop():
            raise Cancelled('Stopped while preparing FLAC on the computer.')

    source = Path(item['source'])
    _plain(source, Path(source.anchor))
    before = source.stat()
    if (before.st_size, before.st_mtime_ns) != (item['bytes'], item['mtime_ns']):
        raise ValueError(f"Source changed since selection: {source}")
    local = scratch / 'source.flac'
    with source.open('rb') as reader, local.open('xb') as frozen:
        while chunk := reader.read(1024 * 1024):
            check()
            frozen.write(chunk)
    if (_receipt_stamp(before) != _receipt_stamp(source.stat()) or
            local.stat().st_size != before.st_size):
        raise ValueError(f"Source changed during preparation: {source}")
    outcome = prepare_file(local, cache / 'flac-import', ffprobe=ffprobe, checkpoint=check)
    if outcome and not same_original(source, local, checkpoint=check):
        raise ValueError(f"FLAC preparation changed original audio: {source}")
    metadata_seed = _import_metadata_seed(local, ffprobe, should_stop)
    check()
    if _receipt_stamp(source.stat()) != _receipt_stamp(before):
        raise ValueError(f"Source changed during preparation: {source}")
    return local, metadata_seed, outcome is not None, _receipt_stamp(before)


def _wave_pcm_hash(path, metadata, *, checkpoint=lambda: None):
    digest = hashlib.sha256()
    with path.open('rb') as wave:
        wave.seek(metadata['data_offset'])
        remaining = metadata['data_bytes']
        while remaining:
            checkpoint()
            chunk = wave.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError('Converted WAV data changed while checking it')
            digest.update(chunk)
            remaining -= len(chunk)
    checkpoint()
    return digest.hexdigest()


def _verified_audio_hash(path, root, expected, verified, metadata=None, *, checkpoint=lambda: None):
    checkpoint()
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected):
        return False
    before = path.stat()
    stamp = (before.st_size, before.st_mtime_ns, before.st_ctime_ns, before.st_ino)
    recent = os.name == 'nt' and before.st_mtime_ns > time.time_ns() - _RECENT_OUTPUT_NS
    relative = path.relative_to(root).as_posix() if path.is_relative_to(root) else None
    key = (relative, 'pcm:' + expected if metadata else expected) if relative else None
    if key and verified is not None and not recent and verified.get(key) == stamp:
        return True
    actual = (_wave_pcm_hash(path, metadata, checkpoint=checkpoint) if metadata else
              _hash(path, progress=lambda _current, _total: checkpoint()))
    after = path.stat()
    if stamp != (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino):
        raise ValueError(f'Audio changed during verification: {path}')
    if actual == expected and key and verified is not None and not recent:
        verified[key] = stamp
    return actual == expected


def _wave_metadata(output, tags):
    fields = {'title': b'INAM', 'artist': b'IART', 'album': b'IPRD', 'genre': b'IGNR',
              'date': b'ICRD', 'track': b'ITRK', 'album_artist': b'IAMP',
              'composer': b'ICMP', 'disc': b'IDIS', 'replaygain_track_gain': b'IGAN',
              'replaygain_album_gain': b'IGAL', 'compilation': b'ICPL',
              'source_sha256': b'IHS1', 'pcm_sha256': b'IHC1'}
    body = bytearray(b'INFO')
    for key, atom in fields.items():
        text = tags.get(key) or (tags.get('albumartist') if key == 'album_artist' else '')
        value = str(text or '').encode('utf-8')[:1024]
        if value:
            value += b'\0'
            body += atom + struct.pack('<I', len(value)) + value + (b'\0' if len(value) & 1 else b'')
    if len(body) == 4:
        return
    chunk = b'LIST' + struct.pack('<I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
    with output.open('r+b') as wave:
        if wave.read(4) != b'RIFF':
            raise ValueError('Audio conversion did not produce standard RIFF WAV')
        wave.seek(0, os.SEEK_END)
        size = wave.tell() + len(chunk)
        if size > 0xffffffff + 8:
            raise ValueError('Converted WAV exceeds the iPod file limit')
        wave.write(chunk)
        wave.seek(4)
        wave.write(struct.pack('<I', size - 8))


def _convert_audio(source, output, ffmpeg, checkpoint, max_bytes, expected_frames,
                   tags=None, cover=None, source_sha256=None):
    if not ffmpeg:
        raise ValueError('Audio conversion needs FFmpeg on the computer.')
    if shutil.disk_usage(output.parent).free < max_bytes + 1024 * 1024:
        raise ValueError('Not enough computer space for audio conversion.')
    command = [str(ffmpeg), '-nostdin', '-v', 'error', '-i', str(source), '-map', '0:a:0',
               '-vn', '-ac', '2', '-ar', '44100', '-c:a', 'pcm_s16le', '-map_metadata', '0',
               '-fs', str(max_bytes), '-f', 'wav', str(output)]
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=errors,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            while process.poll() is None:
                checkpoint()
                if output.exists() and output.stat().st_size >= max_bytes:
                    raise ValueError('Audio conversion exceeded its planned size.')
                try:
                    process.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
        if process.returncode:
            errors.seek(0)
            raise ValueError(f'Audio conversion failed: {errors.read(1000).decode("utf-8", "replace")}')
    if output.stat().st_size >= max_bytes * 0.95:
        raise ValueError('Audio conversion reached its size limit; source duration may be incorrect.')
    from .music import _pcm, compatibility_issue
    metadata = _pcm(output)
    if metadata['frames'] < expected_frames - max(2205, expected_frames // 100):
        raise ValueError('Audio conversion ended before the source duration.')
    source_sha256 = source_sha256 or _hash(source, progress=lambda _current, _total: checkpoint())
    pcm_sha256 = _wave_pcm_hash(output, metadata, checkpoint=checkpoint)
    _wave_metadata(output, {**(tags or {}), 'source_sha256': source_sha256,
                            'pcm_sha256': pcm_sha256})
    if cover:
        from mutagen.id3 import APIC
        from mutagen.wave import WAVE
        wave = WAVE(output)
        if wave.tags is None:
            wave.add_tags()
        mime = 'image/png' if cover.startswith(b'\x89PNG') else 'image/jpeg'
        wave.tags.add(APIC(encoding=3, mime=mime, type=3, desc='Cover', data=cover))
        wave.save()
    metadata = _pcm(output)
    if compatibility_issue(metadata):
        raise ValueError(f'Audio conversion produced an unsupported WAV: {source}')
    return metadata


def _seed_imported_music(root, cache, rows, *, cache_scope=None):
    """Reuse host-read tags and artwork after the verified iPod copy is published."""
    import sqlite3
    from .music import _MetadataCache
    try:
        saved = _MetadataCache(cache_data_directory(cache) / 'music-metadata.sqlite3',
                               source_root=root, source_scope=cache_scope)
    except (OSError, sqlite3.Error):
        return
    try:
        source = Path(os.path.abspath(root))
        for path, before, metadata, cover in rows:
            try:
                relative = path.relative_to(root).as_posix()
                value = dict(metadata, data_bytes=before.st_size if metadata['codec'] >= 16 else metadata['data_bytes'])
                if _MetadataCache._valid(value, relative, before):
                    saved.embedded_cover_lookup(path, before, bypass=True)
                    saved.put(source, relative, before, value)
                    saved.embedded_cover_store(path, before, cover)
            except (OSError, ValueError, TypeError, KeyError):
                continue  # Disposable cache; the normal compiler will reread.
    finally:
        saved.close()


def _prepare_audio(root, cache, skipped, ffprobe, checkpoint, progress, report,
                   aac_recovery, flac_recovery, record, stage_progress=lambda fraction: None,
                   force=False, cancelled=lambda: False, audio_journal=None, cache_scope=None):
    from . import SourceError
    from .aac_index import prepare_file as prepare_aac
    from .flac_index import prepare_file as prepare_flac
    supported = ('.m4a', '.m4b', '.flac', '.flc')
    checkpoint()
    next_check = time.monotonic() + 0.05
    pending = []
    for folder in source_folders(root, 'Music'):
        for path in folder.rglob('*'):
            if time.monotonic() >= next_check:
                checkpoint()
                next_check = time.monotonic() + 0.05
            suffix = path.suffix.lower()
            if suffix not in supported or not path.is_file():
                continue
            if path.relative_to(root).as_posix().casefold() in skipped:
                continue
            pending.append((path, suffix))
    totals = {'aac': sum(suffix in ('.m4a', '.m4b') for _, suffix in pending),
              'flac': sum(suffix in ('.flac', '.flc') for _, suffix in pending)}
    completed = {'aac': 0, 'flac': 0}
    seek_state_path = cache_data_directory(cache) / 'seek-checked.json'
    seek_state = {} if force else _optional_state(seek_state_path)
    seen = set()
    pending_checks = 0
    prepared = False
    probe_deferred = False
    durations = {'aac': [], 'flac': []}
    music_seeds = []
    seed_bytes = 0
    for path, suffix in pending:
        checkpoint()
        _plain(path, root)
        kind = 'aac' if suffix in ('.m4a', '.m4b') else 'flac'
        key = path.relative_to(root).as_posix().casefold()
        seen.add(key)
        before = path.stat()
        stamp = _seek_stamp(path, before)
        if seek_state.get(key) == stamp:
            completed[kind] += 1
            stage_progress((completed['aac'] + completed['flac']) / len(pending))
            continue
        label = 'AAC' if kind == 'aac' else 'FLAC'
        left = totals[kind] - completed[kind]
        recent = durations[kind]
        seconds = sum(recent) / len(recent) * left if len(recent) >= 2 else None
        eta = (f' · about {max(1, round(seconds / 60))}m left' if seconds >= 60 else
               ' · <1m left') if seconds is not None else ''
        stage_progress((completed['aac'] + completed['flac']) / len(pending))
        progress(f'{label} seeks {completed[kind]}/{totals[kind]} · {left} left{eta}: {path.name}')
        started = time.monotonic()
        metadata_seed = None
        verified_stamp = None
        if suffix in ('.m4a', '.m4b'):
            def seed_local(local):
                nonlocal metadata_seed
                try:
                    from .music import _read_metadata
                    metadata = _read_metadata(local, ffprobe or shutil.which('ffprobe'), True,
                                              cancelled, include_cover=True)
                    metadata_seed = (metadata, metadata.pop('_embedded_cover', None))
                except (OSError, ValueError, subprocess.SubprocessError):
                    pass  # The library compiler can still read the iPod copy.
            def retain_verified(info):
                nonlocal verified_stamp
                verified_stamp = _receipt_stamp(info)
            try:
                outcome = prepare_aac(path, cache / 'aac-seek', checkpoint=checkpoint,
                                      recovery=aac_recovery,
                                      journal=(lambda: audio_journal('AAC', aac_recovery[-1])) if audio_journal else record,
                                      local_copy=seed_local if not force else None,
                                      verified_copy=retain_verified if not force else None)
            except SourceError as error:
                report['errors'].append(dict(kind='Music', source=str(path),
                                             file=path.relative_to(root).as_posix(), message=str(error)))
                skipped.add(key)
                prepared = True  # Recompute the inventory without this source.
                completed[kind] += 1
                continue
            if outcome:
                prepared = True
                report['steps'].append(dict(prepared_aac=str(path.relative_to(root))))
                if audio_journal is None:
                    record()
        else:
            outcome = prepare_flac(path, cache / 'flac-seek', ffprobe=ffprobe,
                                   checkpoint=checkpoint, recovery=flac_recovery,
                                   journal=(lambda: audio_journal('FLAC', flac_recovery[-1])) if audio_journal else record)
            if outcome is None:
                probe_deferred = True
            elif outcome:
                prepared = True
                report['steps'].append(dict(prepared_flac=str(path.relative_to(root))))
                if audio_journal is None:
                    record()
        if outcome is not None:
            after = path.stat()
            if outcome and metadata_seed is not None and verified_stamp == _receipt_stamp(after):
                music_seeds.append((path, after, *metadata_seed))
                seed_bytes += len(metadata_seed[1] or b'')
                if len(music_seeds) >= 128 or seed_bytes >= 32 * 1024 * 1024:
                    _seed_imported_music(root, cache, music_seeds, cache_scope=cache_scope)
                    music_seeds.clear()
                    seed_bytes = 0
            if not outcome and _receipt_stamp(before) != _receipt_stamp(after):
                raise ValueError(f'Audio source changed during seek check: {path}')
            seek_state[key] = _seek_stamp(path, after)
            pending_checks += 1
            if pending_checks >= 128:
                _save_seek_state(seek_state_path, seek_state)
                pending_checks = 0
        recent.append(time.monotonic() - started)
        if len(recent) > 8:
            recent.pop(0)
        completed[kind] += 1
    if music_seeds:
        _seed_imported_music(root, cache, music_seeds, cache_scope=cache_scope)
    seek_state = {key: value for key, value in seek_state.items() if key in seen}
    _save_seek_state(seek_state_path, seek_state)
    stage_progress(1)
    for kind, label in (('aac', 'AAC'), ('flac', 'FLAC')):
        if totals[kind]:
            progress(f'{label} seeks {completed[kind]}/{totals[kind]} complete')
    checkpoint()
    return prepared, probe_deferred
