"""Bounded snapshots of the Apple library and its dependent sidecar files.

Only a host staging directory is changed here. Publication owns the device lock,
backups, recovery journal, and the final database replacement.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import struct

from .guard import guarded_path
from .host_state import file_stamp
from .apple_process import checkpoint as _checkpoint


DATABASE = 'iPod_Control/iTunes/iTunesDB'
SYSINFO = 'iPod_Control/Device/SysInfo'
ARTWORK = 'iPod_Control/Artwork'
ITUNES = 'iPod_Control/iTunes'
_ITUNES_NAMES = {'itunesdb', 'play counts', 'play counts.bak', 'itunesstats',
                 'playcounts.plist', 'itunesshuffle', 'otgplaylistinfo'}
_PENDING_STATS = {'play counts', 'itunesstats', 'playcounts.plist'}
_MAX_FILES = 4096
_MAX_TOTAL = 8 * 1024 ** 3
_CHUNK = 1024 * 1024


def _relevant(name):
    return name.casefold() in _ITUNES_NAMES or re.fullmatch(
        r'OTGPlaylistInfo_[1-9][0-9]*', name, re.IGNORECASE) is not None


def _inventory(root):
    """List the files the writer reads, including optional absent-file guards."""
    root = Path(root)
    result = {}
    names = set()
    total = 0

    def add(path, maximum):
        nonlocal total
        info = guarded_path(path, root)
        if info is None:
            return
        if not stat.S_ISREG(info.st_mode) or not 0 <= info.st_size <= maximum:
            raise ValueError(f'Apple library file has an unsupported type or size: {path}')
        name = path.relative_to(root).as_posix()
        if name.casefold() in names:
            raise ValueError(f'Apple library contains ambiguous filenames: {path}')
        names.add(name.casefold())
        total += info.st_size
        if len(result) >= _MAX_FILES or total > _MAX_TOTAL:
            raise ValueError('Apple library metadata exceeds its preparation limits.')
        result[name] = info

    add(root / SYSINFO, 65536)
    for folder, artwork in ((ITUNES, False), (ARTWORK, True)):
        directory = root / folder
        info = guarded_path(directory, root)
        if info is None:
            continue
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError(f'Apple library directory is not a folder: {directory}')
        for path in directory.iterdir():
            if artwork or _relevant(path.name):
                add(path, 0xffffffff if artwork and path.suffix.casefold() == '.ithmb'
                    else 256 * 1024 ** 2)
            elif path.name.casefold() in ('itunescdb', 'itunes library.itlp'):
                raise ValueError('This Apple database format is not supported by the library writer.')
    return dict(sorted(result.items(), key=lambda item: item[0].casefold()))


def _copy_hash(path, destination, expected, cancelled):
    """Copy and hash once while checking the open file and its directory entry."""
    def handle_stamp(info):
        # Windows path stat reports creation time as ctime on older Python,
        # while an open handle can report the NTFS change time instead.
        stamp = file_stamp(info)
        return stamp[:-1] if os.name == 'nt' else stamp

    digest = hashlib.sha256()
    count = 0
    with path.open('rb') as source:
        if handle_stamp(os.fstat(source.fileno())) != handle_stamp(expected):
            raise ValueError(f'Apple library changed before reading: {path}')
        writer = destination.open('xb') if destination is not None else None
        try:
            while data := source.read(_CHUNK):
                _checkpoint(cancelled)
                count += len(data)
                if count > expected.st_size:
                    raise ValueError(f'Apple library changed while reading: {path}')
                digest.update(data)
                if writer is not None:
                    writer.write(data)
            if (count != expected.st_size or
                    handle_stamp(os.fstat(source.fileno())) != handle_stamp(expected)):
                raise ValueError(f'Apple library changed while reading: {path}')
        finally:
            if writer is not None:
                writer.close()
    if file_stamp(path.stat()) != file_stamp(expected):
        raise ValueError(f'Apple library changed after reading: {path}')
    return digest.hexdigest()


def _check_pending(files):
    variants = [name for name in files
                if Path(name).parent.as_posix() == ITUNES and
                Path(name).name.casefold() in _PENDING_STATS]
    active = [Path(name).name.casefold() for name, info in files.items()
              if Path(name).parent.as_posix() == ITUNES and
              Path(name).name.casefold() in _PENDING_STATS and info.st_size]
    if len(variants) > 1 and active:
        raise ValueError('Multiple pending Apple playback-statistics formats need review before updating.')
    for name in variants:
        size = files[name].st_size
        kind = Path(name).name.casefold()
        if kind == 'play counts' and 0 < size < 96:
            raise ValueError('The pending Apple Play Counts file is truncated; repair it before updating.')
        if kind == 'itunesstats' and size:
            # This is the Shuffle format. None of our four supported families
            # uses it, and libgpod's later-format reader repeats its first row.
            raise ValueError('Shuffle playback history is not supported on this iPod family.')
    names = {Path(name).name.casefold() for name in files
             if Path(name).parent.as_posix() == ITUNES}
    if 'otgplaylistinfo' in names:
        numbers = sorted(int(name.rsplit('_', 1)[1]) for name in names
                         if re.fullmatch(r'otgplaylistinfo_[1-9][0-9]*', name))
        if numbers != list(range(1, len(numbers) + 1)):
            raise ValueError('The pending Apple On-The-Go playlists are incomplete; review them before updating.')
    if DATABASE.casefold() not in {name.casefold() for name in files} and (
            active or 'otgplaylistinfo' in names):
        raise ValueError('Apple playback history or playlists exist without their music database.')


def _verify_music_records(root, *, cancelled):
    """Reject record-count recovery that libgpod would silently write back."""
    from .retail_media import _record, _list

    path = root / DATABASE
    info = guarded_path(path, root)
    if info is None:
        return set(), 0
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 256 * 1024 ** 2:
        raise ValueError('Apple iTunesDB has an unsupported size or file type.')
    with path.open('rb') as stream:
        data = stream.read(info.st_size + 1)
    if len(data) != info.st_size or file_stamp(path.stat()) != file_stamp(info):
        raise ValueError('Apple iTunesDB changed during verification.')
    at, end, count = _list(data, b'mhbd', b'mhlt')
    if at == 0:
        raise ValueError('The Apple music database has no track list.')
    identifiers = set()
    for _ in range(count):
        _checkpoint(cancelled)
        body, following = _record(data, at, end, b'mhit')
        if body - at >= 120:
            identifiers.add(struct.unpack_from('<Q', data, at + 112)[0])
        at = following
    if at != end:
        raise ValueError('The Apple music database track count does not match its records.')

    at, end = _record(data, 0, len(data), b'mhbd')
    while at < end:
        body, following = _record(data, at, end, b'mhsd')
        if body - at < 16:
            raise ValueError('The Apple music database has a truncated section.')
        if struct.unpack_from('<I', data, at + 12)[0] in (2, 3):
            if body + 12 > following or data[body:body + 4] != b'mhlp':
                raise ValueError('The Apple music database has an invalid playlist list.')
            header, playlists = struct.unpack_from('<II', data, body + 4)
            if header < 12 or body + header > following or playlists > 100000:
                raise ValueError('The Apple music database has an invalid playlist count.')
            child = body + header
            for _ in range(playlists):
                _checkpoint(cancelled)
                _, child = _record(data, child, following, b'mhyp')
            if child != following:
                raise ValueError('The Apple music database playlist count does not match its records.')
        at = following
    return identifiers, count


def _verify_pending(root, files, identifiers, count, *, cancelled):
    """Ensure every pending statistics entry can be consumed exactly once."""
    for name, info in files.items():
        _checkpoint(cancelled)
        kind = Path(name).name.casefold()
        if kind not in ('play counts', 'playcounts.plist') or not info.st_size:
            continue
        data = (root / name).read_bytes()
        if kind == 'play counts':
            byteorder = '<' if data[:4] == b'mhdp' else '>' if data[:4] == b'pdhm' else None
            if byteorder is None:
                raise ValueError('The pending Apple Play Counts header is invalid.')
            header, stride, entries = struct.unpack_from(byteorder + 'III', data, 4)
            if header < 96 or stride < 12 or header + stride * entries > len(data):
                raise ValueError('The pending Apple Play Counts records are truncated.')
            if entries > count:
                raise ValueError('Apple playback history refers to tracks missing from its database.')
        else:
            try:
                pending = plistlib.loads(data)
            except (ValueError, OverflowError, plistlib.InvalidFileException) as error:
                raise ValueError('The pending Apple playback-history plist is invalid.') from error
            if not isinstance(pending, dict) or not isinstance(pending.get('tracks'), list):
                raise ValueError('The pending Apple playback-history plist has no track list.')
            seen = set()
            for track in pending['tracks']:
                _checkpoint(cancelled)
                identifier = track.get('persistentID') if isinstance(track, dict) else None
                if type(identifier) is not int or not -(1 << 63) <= identifier < 1 << 64:
                    raise ValueError('Apple playback history has an invalid persistent track ID.')
                identifier &= (1 << 64) - 1
                if identifier not in identifiers or identifier in seen:
                    raise ValueError('Apple playback history has a missing or repeated track ID.')
                seen.add(identifier)
                for field in ('bookmarkTimeInMS', 'playCount', 'playMacOSDate',
                              'skipCount', 'skipMacOSDate', 'userRating'):
                    if field in track and (type(track[field]) is not int or
                                           not 0 <= track[field] <= 0xffffffff):
                        raise ValueError(f'Apple playback history has an invalid {field} value.')


def stage_sysinfo(root, stage, model, guid):
    """Select the verified family format in staging, including a blank SysInfo."""
    root, stage = Path(root), Path(stage)
    if stage.resolve().is_relative_to(root.resolve()):
        raise ValueError('Apple library staging must be outside the source volume.')
    if not re.fullmatch(r'[A-Z0-9]{4,8}', model) or not re.fullmatch(
            r'(?:0[xX])?[0-9A-Fa-f]{16}', guid):
        raise ValueError('Apple library writer identity is invalid.')
    sysinfo = stage / SYSINFO
    sysinfo.parent.mkdir(parents=True, exist_ok=True)
    info = guarded_path(sysinfo, stage)
    if info is None:
        source_info = guarded_path(root / SYSINFO, root)
        if source_info is not None:
            if not stat.S_ISREG(source_info.st_mode) or source_info.st_size > 65536:
                raise ValueError('Apple device SysInfo exceeds its preparation limit.')
            _copy_hash(root / SYSINFO, sysinfo, source_info, lambda: False)
        text = sysinfo.read_text(encoding='ascii') if sysinfo.exists() else ''
    elif not stat.S_ISREG(info.st_mode) or info.st_size > 65536:
        raise ValueError('Staged Apple device SysInfo exceeds its preparation limit.')
    else:
        text = sysinfo.read_text(encoding='ascii')
    lines = [line for line in text.splitlines()
             if line.partition(':')[0].strip() not in ('ModelNumStr', 'FirewireGuid')]
    lines += [f'ModelNumStr: {model}', f'FirewireGuid: 0x{guid.removeprefix("0x").removeprefix("0X").upper()}']
    sysinfo.write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')


def pending(root):
    """Identify playback history or playlists needing a database update."""
    root = Path(root)
    directory = root / ITUNES
    info = guarded_path(directory, root)
    if info is None:
        return False
    if not stat.S_ISDIR(info.st_mode):
        raise ValueError('The Apple library path is not a directory.')
    for path in directory.iterdir():
        if path.name.casefold() in _PENDING_STATS | {'otgplaylistinfo', 'itunesshuffle'}:
            guarded_path(path, root)
            return True
    return False


def verify_artwork(root, *, cancelled=lambda: False):
    """Check every ArtworkDB thumbnail reference, including unknown formats.

    libgpod's thumbnail predicate only checks an in-memory association. It does
    not prove the referenced file still exists after its compaction pass.
    """
    from .retail_media import _record, _list, _image_string

    root = Path(root)
    folder = root / ARTWORK
    database = folder / 'ArtworkDB'
    info = guarded_path(database, root)
    if info is None:
        return
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 256 * 1024 ** 2:
        raise ValueError('Apple ArtworkDB has an unsupported size or file type.')
    with database.open('rb') as stream:
        data = stream.read(info.st_size + 1)
    if len(data) != info.st_size or file_stamp(database.stat()) != file_stamp(info):
        raise ValueError('Apple ArtworkDB changed during verification.')
    at, end, count = _list(data, b'mhfd', b'mhli')
    sizes = {}
    for _ in range(count):
        _checkpoint(cancelled)
        child, following = _record(data, at, end, b'mhii')
        while child < following:
            body, next_child = _record(data, child, following, b'mhod')
            if body - child < 16:
                raise ValueError('Apple artwork metadata is truncated.')
            if struct.unpack_from('<H', data, child + 12)[0] == 2:
                name_at, thumb_end = _record(data, body, next_child, b'mhni')
                if name_at - body < 36:
                    raise ValueError('Apple artwork thumbnail metadata is truncated.')
                offset, length = struct.unpack_from('<II', data, body + 20)
                _, name_end = _record(data, name_at, thumb_end, b'mhod')
                name = _image_string(data, name_at, name_end)
                if not re.fullmatch(r':F[0-9]+_[0-9]+\.ithmb', name, re.IGNORECASE):
                    raise ValueError('Apple artwork references an unsupported thumbnail path.')
                if name.casefold() not in sizes:
                    thumb_info = guarded_path(folder / name[1:], root)
                    if thumb_info is None or not stat.S_ISREG(thumb_info.st_mode):
                        raise ValueError(f'Apple artwork thumbnail file is missing: {name[1:]}')
                    sizes[name.casefold()] = thumb_info.st_size
                if length <= 0 or offset + length > sizes[name.casefold()]:
                    raise ValueError(f'Apple artwork thumbnail extends beyond its file: {name[1:]}')
            child = next_child
        at = following
    if at != end:
        raise ValueError('Apple ArtworkDB image count does not match its records.')


def snapshot(root, stage, model, guid, *, cancelled=lambda: False,
             artwork_cache=None, cache_binding=None):
    """Copy all writer inputs and set its format before libgpod parses artwork."""
    root, stage = Path(root), Path(stage)
    _checkpoint(cancelled)
    if stage.resolve().is_relative_to(root.resolve()):
        raise ValueError('Apple library staging must be outside the source volume.')
    files = _inventory(root)
    _check_pending(files)
    stage.mkdir(parents=True, exist_ok=True)
    reserve_bytes = sum(info.st_size for info in files.values()) * 2 + 64 * 1024 ** 2
    if shutil.disk_usage(stage).free < reserve_bytes:
        raise ValueError('The computer needs more free space to prepare and verify this Apple library.')
    artwork = None
    if artwork_cache is not None:
        if (Path(artwork_cache).resolve().is_relative_to(root.resolve()) or
                not isinstance(cache_binding, dict) or not cache_binding):
            raise ValueError('The Apple artwork cache must have a verified host volume binding.')
        from .apple_artwork_cache import ArtworkCache
        artwork = ArtworkCache(artwork_cache, cache_binding, files,
                               reserve_bytes=reserve_bytes, cancelled=cancelled)
    inputs = {}
    for name, info in files.items():
        _checkpoint(cancelled)
        destination = stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        digest = artwork.copy(name, info, destination, stage, _copy_hash) if artwork else None
        verified_artwork = digest is not None
        if digest is None:
            digest = _copy_hash(root / name, destination, info, cancelled)
            verified_artwork = bool(artwork and artwork.remember(
                name, info, destination, digest, _copy_hash))
        if file_stamp((root / name).stat()) != file_stamp(info):
            raise ValueError(f'Apple library changed during preparation: {name}')
        inputs[name] = {'sha256': digest, 'bytes': info.st_size,
                        'stamp': file_stamp(info)}
        if verified_artwork:
            inputs[name]['verified_artwork'] = True
    # Windows rename() cannot replace an existing backup. The source backup is
    # still in inputs and will be backed up by the publication transaction.
    if any(Path(name).name.casefold() == 'play counts' for name in files):
        for name in files:
            if Path(name).name.casefold() == 'play counts.bak':
                (stage / name).unlink()
    stage_sysinfo(root, stage, model, guid)
    (stage / ITUNES).mkdir(parents=True, exist_ok=True)
    identifiers, count = _verify_music_records(stage, cancelled=cancelled)
    _verify_pending(stage, files, identifiers, count, cancelled=cancelled)
    verify_artwork(stage, cancelled=cancelled)
    if set(_inventory(root)) != set(files):
        raise ValueError('Apple library files changed during preparation; retry the update.')
    if artwork:
        artwork.finish()
    return inputs


def verify(root, inputs, *, cancelled=lambda: False):
    """Require unchanged membership and content immediately before publication."""
    root = Path(root)
    current = _inventory(root)
    if set(current) != set(inputs):
        raise ValueError('Apple library files changed during preparation; retry the update.')
    for name, info in current.items():
        _checkpoint(cancelled)
        expected = inputs[name]
        if tuple(expected['stamp']) != file_stamp(info):
            raise ValueError(f'Apple library changed during preparation: {name}')
        if expected.get('verified_artwork') is True:
            from .apple_artwork_cache import ArtworkCache
            if ArtworkCache.accepts(name):
                continue  # The snapshot verified a private cached copy of these exact source bytes.
        if _copy_hash(root / name, None, info, cancelled) != expected['sha256']:
            raise ValueError(f'Apple library changed during preparation: {name}')


def outputs(stage, inputs, *, cancelled=lambda: False):
    """Collect only library changes, retiring sidecars already merged by libgpod."""
    stage = Path(stage)
    _verify_music_records(stage, cancelled=cancelled)
    verify_artwork(stage, cancelled=cancelled)
    current = _inventory(stage)
    original_names = {name.casefold(): name for name in inputs}
    consumed = {name for name in inputs if name.casefold().startswith(ITUNES.casefold() + '/') and
                Path(name).name.casefold() in ('play counts', 'itunesstats', 'itunesshuffle', 'playcounts.plist')}
    if any(Path(name).name.casefold() == 'otgplaylistinfo' for name in inputs):
        consumed.update(name for name in inputs if name.casefold().startswith(ITUNES.casefold() + '/') and
                        Path(name).name.casefold().startswith('otgplaylistinfo'))
    prepared = {}
    present = set()
    for stage_name, info in current.items():
        _checkpoint(cancelled)
        if stage_name.casefold() == SYSINFO.casefold():
            continue  # The representative writer model is never a device edit.
        name = original_names.get(stage_name.casefold(), stage_name)
        if name in consumed:
            continue
        present.add(name)
        digest = _copy_hash(stage / stage_name, None, info, cancelled)
        previous = inputs.get(name)
        if previous and digest == previous['sha256']:
            continue
        if stage_name.startswith(ARTWORK + '/') and not (
                Path(stage_name).name.casefold() == 'artworkdb' or
                re.fullmatch(r'F[0-9]+_[0-9]+\.ithmb', Path(stage_name).name, re.IGNORECASE)):
            raise ValueError(f'Apple writer changed an unexpected artwork file: {stage_name}')
        prepared[name] = {'path': stage / stage_name, 'sha256': digest, 'bytes': info.st_size}
    removed = sorted(name for name in inputs if name.casefold() != SYSINFO.casefold() and
                     (name in consumed or name not in present))
    for name in removed:
        if name.startswith(ARTWORK + '/') and not re.fullmatch(
                r'F[0-9]+_[0-9]+\.ithmb', Path(name).name, re.IGNORECASE):
            raise ValueError(f'Apple writer removed an unexpected artwork file: {name}')
    if not any(name.casefold() == DATABASE.casefold() for name in current):
        raise ValueError('Apple writer did not produce a music database.')
    return prepared, removed
