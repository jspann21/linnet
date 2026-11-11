"""Read existing iTunes media without modifying Apple's databases or originals."""
from collections import OrderedDict
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from functools import lru_cache
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct
import time
import uuid

from .publish import _bounded, _plain
from .host_state import cache_volume_scope, file_stamp, save_json


class LibraryBusy(RuntimeError):
    def __init__(self):
        super().__init__('The iTunes library is unavailable or changing. Let any iTunes sync finish, then refresh the iPod.')


def _check_database_stamp(path, stamp):
    try:
        info = path.stat()
    except FileNotFoundError as error:
        raise LibraryBusy() from error
    if (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino) != stamp:
        raise LibraryBusy()


def _database_bytes(path, maximum, stamp):
    _check_database_stamp(path, stamp)
    try:
        data = _bounded(path, maximum)
    except (OSError, ValueError) as error:
        if isinstance(error, PermissionError) or getattr(error, 'winerror', None) in (32, 33):
            raise LibraryBusy() from error
        _check_database_stamp(path, stamp)
        raise
    _check_database_stamp(path, stamp)
    return data


def check_tracks_access(root):
    """Avoid scanning every media file when Apple's music database is locked."""
    root = Path(root)
    path = root / 'iPod_Control/iTunes/iTunesDB'
    try:
        _plain(path, root)
        if path.is_file():
            with path.open('rb') as reader:
                reader.read(1)
    except OSError as error:
        if isinstance(error, PermissionError) or getattr(error, 'winerror', None) in (32, 33):
            raise LibraryBusy() from error
    except ValueError:
        # Unusable optional metadata follows the existing fallback path. The
        # guard rejects redirected paths before opening any source bytes.
        pass


def _u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def _record(data, offset, end, tag):
    if offset + 12 > end or data[offset:offset + 4] != tag:
        raise ValueError('Invalid iTunes media record')
    header, total = struct.unpack_from('<II', data, offset + 4)
    if header < 12 or total < header or offset + total > end:
        raise ValueError('Invalid iTunes media record bounds')
    return offset + header, offset + total


def _list(data, magic, list_tag):
    start, end = _record(data, 0, len(data), magic)
    while start < end:
        body, following = _record(data, start, end, b'mhsd')
        if body - start < 16:
            raise ValueError('Truncated iTunes section')
        if _u32(data, start + 12) == 1:
            if body + 12 > following or data[body:body + 4] != list_tag:
                raise ValueError('Invalid iTunes media list')
            header, count = struct.unpack_from('<II', data, body + 4)
            if header < 12 or body + header > following or count > 100000:
                raise ValueError('Invalid iTunes media count')
            return body + header, following, count
        start = following
    return 0, 0, 0


def _path(root, value, *, with_identity=False, parent_checks=None, known_paths=None):
    parts = value.replace(':', '/').strip('/').split('/')
    # Apple's tested Classic database route to a shared /Music tree begins at
    # iPod_Control/Music, then steps back to the volume root. Accept that one
    # fixed traversal only; all other parent components remain invalid.
    if parts[:4] == ['iPod_Control', 'Music', '..', '..'] and len(parts) > 5 \
            and parts[4] in ('Music', 'Videos'):
        parts = parts[4:]
    if not parts or any(not p or p in ('.', '..') or '\\' in p for p in parts):
        raise ValueError('Unsafe iTunes media path')
    path = root.joinpath(*parts)
    if known_paths is not None:
        known = known_paths.get(str(path).casefold())
        if known is not None:
            # The shared inventory already rejected links while traversing
            # this exact file, so no second USB lookup is needed.
            return (path, known) if with_identity else path
    if parent_checks is None:
        _plain(path, root)
    else:
        # Tracks in one iTunesDB share a small set of directories. Check each
        # directory once for this read-only parse, but still check every leaf
        # and resolve every full path below.
        cursor = root
        for component in (None, *parts[:-1]):
            if component is not None:
                cursor /= component
            if cursor not in parent_checks:
                _plain(cursor, root)
                try:
                    info = cursor.lstat()
                except FileNotFoundError:
                    continue
                if stat.S_ISDIR(info.st_mode):
                    parent_checks[cursor] = (info.st_dev, info.st_ino, info.st_ctime_ns)
        try:
            info = path.lstat()
        except FileNotFoundError:
            pass
        else:
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError(f'reparse/symlink path rejected: {path}')
    resolved = path.resolve()
    if not resolved.is_relative_to(root):
        raise ValueError('iTunes media path escaped the iPod')
    return (path, resolved) if with_identity else path


def tracks(root, *, known_records=None, known_inodes=None):
    root = Path(root).resolve()
    path = root / 'iPod_Control/iTunes/iTunesDB'
    _plain(path, root)
    if not path.is_file():
        return {}
    info = path.stat()
    stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
    return (_tracks(str(root), *stamp) if known_records is None else
            _parse_tracks(str(root), stamp, known_records, known_inodes))


def available_tracks(root, *, known_records=None, known_inodes=None):
    """Missing or damaged Apple metadata is optional; active sync must finish."""
    try:
        return tracks(root, known_records=known_records, known_inodes=known_inodes)
    except PermissionError as error:
        # Python's Windows CRT file reader can discard the sharing/lock code
        # and report only EACCES. Do not silently begin scanning audio files.
        raise LibraryBusy() from error
    except OSError as error:
        if getattr(error, 'winerror', None) in (32, 33):
            raise LibraryBusy() from error
        return {}
    except (ValueError, UnicodeError, struct.error):
        return {}


def media_kind(track):
    """Classify Apple's movie, music-video and TV-show bitfields separately."""
    media_type = track.get('media_type', 1)
    if media_type & ((1 << 1) | (1 << 5) | (1 << 6)):
        return 'Videos'
    if media_type & ((1 << 22) | (1 << 23)):
        return 'Other'
    # Older databases omit media_type; these extensions still identify video.
    if Path(track.get('path', '')).suffix.lower() in ('.m4v', '.mov'):
        return 'Videos'
    return 'Music'


def _tracks_cache_path(root):
    from .host_cache import cache_base
    base = cache_base() / 'retail'
    key = hashlib.sha256(cache_volume_scope(root).encode()).hexdigest()[:24]
    return base / ('tracks-' + key + '.json')


def _known_paths(root, records, inodes):
    if records is None:
        return None, None
    paths, stamps = {}, []
    parents = {}
    for path, info in records:
        key = str(path).casefold()
        paths[key] = None if key in paths else path
        inode = inodes.get(path) if inodes is not None else None
        if not inode:
            stamps = None
        if stamps is not None:
            parent = path.parent
            relative = parents.get(parent)
            if relative is None:
                relative = parent.relative_to(root).as_posix()
                relative = relative + '/' if relative != '.' else ''
                parents[parent] = relative
            stamps.append(((relative + path.name).casefold(), inode,
                           info.st_size, info.st_mtime_ns, info.st_ctime_ns))
    digest = (hashlib.sha256(json.dumps(sorted(stamps), separators=(',', ':')).encode()).hexdigest()
              if stamps is not None else None)
    return paths, digest


def _relative_parts(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value:
        return None
    parts = value.split('/')
    return parts if all(part and part not in ('.', '..') for part in parts) else None


def _cached_tracks(root, stamp, known_paths, known_signature):
    path = _tracks_cache_path(root)
    try:
        if path.stat().st_size > 16 * 1024 * 1024:
            return None
        saved = json.loads(path.read_text(encoding='utf-8'))
        rows = saved.get('rows')
        age = time.time() - saved.get('created', -1)
        if (saved.get('version') != 2 or saved.get('stamp') != list(stamp) or
                not 0 <= age <= 3600 or not isinstance(rows, list) or len(rows) > 100000):
            return None
        if hashlib.sha256(json.dumps(rows, separators=(',', ':')).encode()).hexdigest() != saved.get('rows_hash'):
            return None
        fast = (known_paths is not None and known_signature is not None and
                saved.get('known_signature') == known_signature)
        result, parent_checks = {}, {}
        text_fields = {'title', 'album', 'artist', 'genre', 'composer', 'album_artist'}
        numbers = {'track', 'disc', 'dbid', 'media_type'}
        required = numbers | {'duration', 'compilation'}
        for entry in rows:
            if (not isinstance(entry, list) or len(entry) != 3 or
                    not isinstance(entry[2], dict)):
                return None
            relative, resolved_relative, row = entry
            display_parts = _relative_parts(relative)
            resolved_parts = _relative_parts(resolved_relative)
            if display_parts is None or resolved_parts is None:
                return None
            if (not required <= row.keys() or
                    any(type(row.get(name)) is not str for name in text_fields if name in row) or
                    any(type(row.get(name)) is not int for name in numbers) or
                    type(row.get('duration')) not in (int, float) or
                    not math.isfinite(row['duration']) or
                    type(row.get('compilation')) is not bool or
                    set(row) - text_fields - numbers - {'duration', 'compilation'}):
                return None
            resolved = (known_paths.get(str(root.joinpath(*resolved_parts)).casefold())
                        if fast else None)
            if resolved is not None:
                path = root.joinpath(*display_parts)
            else:
                path, resolved = _path(root, relative, with_identity=True,
                                       parent_checks=parent_checks, known_paths=known_paths)
            result[str(resolved).casefold()] = dict(row, path=str(path))
        for parent, before in parent_checks.items():
            _plain(parent, root)
            info = parent.lstat()
            if not stat.S_ISDIR(info.st_mode) or (info.st_dev, info.st_ino, info.st_ctime_ns) != before:
                return None
        info = (root / 'iPod_Control/iTunes/iTunesDB').stat()
        if (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino) != stamp:
            return None
        return result, saved.get('known_signature'), saved['created']
    except (OSError, ValueError, TypeError, AttributeError, OverflowError):
        return None


def _remember_tracks(root, stamp, rows, known_signature, *, created=None):
    try:
        entries = [[Path(row['path']).relative_to(root).as_posix(),
                    Path(resolved).relative_to(root).as_posix(),
                    {key: value for key, value in row.items() if key != 'path'}]
                   for resolved, row in rows.items()]
        payload = json.dumps(entries, separators=(',', ':')).encode()
        if len(payload) > 15 * 1024 * 1024:
            return
        save_json(_tracks_cache_path(root), dict(version=2, stamp=list(stamp),
                                                 known_signature=known_signature,
                                                 rows_hash=hashlib.sha256(payload).hexdigest(),
                                                 created=time.time() if created is None else created,
                                                 rows=entries))
    except (OSError, ValueError):
        pass  # Parsed Apple metadata remains usable without a host cache.


@lru_cache(maxsize=4)
def _tracks(root_name, size, mtime, ctime, inode):
    return _parse_tracks(root_name, (size, mtime, ctime, inode))


def _parse_tracks(root_name, stamp, known_records=None, known_inodes=None):
    root = Path(root_name)
    database = root / 'iPod_Control/iTunes/iTunesDB'
    known_paths, known_signature = _known_paths(root, known_records, known_inodes)
    cached = _cached_tracks(root, stamp, known_paths, known_signature)
    if cached is not None:
        rows, cached_signature, created = cached
        _check_database_stamp(database, stamp)
        if known_signature is not None and cached_signature != known_signature:
            _remember_tracks(root, stamp, rows, known_signature, created=created)
        _check_database_stamp(database, stamp)
        return rows
    data = _database_bytes(database, 128 * 1024 * 1024, stamp)
    at, end, count = _list(data, b'mhbd', b'mhlt')
    result = {}
    parent_checks = {}
    fields = {1: 'title', 2: 'path', 3: 'album', 4: 'artist', 5: 'genre',
              12: 'composer', 22: 'album_artist'}
    for _ in range(count):
        child, following = _record(data, at, end, b'mhit')
        if child - at < 120:
            raise ValueError('Truncated iTunes track')
        row = dict(duration=_u32(data, at + 40) / 1000, track=_u32(data, at + 44),
                   disc=_u32(data, at + 92), dbid=struct.unpack_from('<Q', data, at + 112)[0],
                   compilation=bool(data[at + 30]),
                   media_type=_u32(data, at + 208) if child - at >= 212 else 1)
        while child < following:
            body, next_child = _record(data, child, following, b'mhod')
            kind = _u32(data, child + 12)
            if kind in fields:
                if body + 16 > next_child:
                    raise ValueError('Truncated iTunes track text')
                length = _u32(data, body + 4)
                if length > next_child - body - 16:
                    raise ValueError('Invalid iTunes track text length')
                row[fields[kind]] = data[body + 16:body + 16 + length].decode('utf-16-le').rstrip('\0')
            child = next_child
        if row.get('path'):
            path, resolved = _path(root, row.pop('path'), with_identity=True,
                                   parent_checks=parent_checks, known_paths=known_paths)
            row['path'] = str(path)
            result[str(resolved).casefold()] = row
        at = following
    # A directory replaced during the parse must not leave a cached proof in
    # the returned metadata. This cache never survives the current parse.
    for path, before in parent_checks.items():
        _plain(path, root)
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or (info.st_dev, info.st_ino, info.st_ctime_ns) != before:
            raise LibraryBusy()
    _check_database_stamp(database, stamp)
    _remember_tracks(root, stamp, result, known_signature)
    return result


def _image_string(data, at, end):
    if at + 36 > end:
        raise ValueError('Truncated iTunes image text')
    length = _u32(data, at + 24)
    if length > end - at - 36:
        raise ValueError('Invalid iTunes image text length')
    return data[at + 36:at + 36 + length].decode('utf-16-le' if data[at + 28] == 2 else 'utf-8').rstrip('\0')


def image_count(root):
    """Validate the image list without rendering every thumbnail."""
    root = Path(root).resolve()
    database = root / 'Photos/Photo Database'
    _plain(database, root)
    data = _bounded(database, 64 * 1024 * 1024)
    at, end, count = _list(data, b'mhfd', b'mhli')
    for _ in range(count):
        _, at = _record(data, at, end, b'mhii')
    return count


class _ThumbnailReaders:
    """Keep a few shared thumbnail files open for one cold extraction."""

    def __init__(self):
        self._open = OrderedDict()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        for reader in self._open.values():
            reader.close()
        self._open.clear()

    def read(self, path, offset, size):
        reader = self._open.pop(path, None)
        if reader is None:
            reader = path.open('rb')
        self._open[path] = reader
        if len(self._open) > 4:
            _, retired = self._open.popitem(last=False)
            retired.close()
        reader.seek(offset)
        pixels = reader.read(size)
        if len(pixels) != size:
            raise ValueError('Truncated iTunes thumbnail')
        return pixels


def _write_preview(image, preview):
    temporary = preview.with_name(uuid.uuid4().hex + '.tmp')
    try:
        image.save(temporary, format='PNG', compress_level=3)
        os.replace(temporary, preview)
    finally:
        temporary.unlink(missing_ok=True)


def images(root, *, artwork=False, dbids=None, render=True):
    root = Path(root).resolve()
    folder = root / ('iPod_Control/Artwork' if artwork else 'Photos')
    database = folder / ('ArtworkDB' if artwork else 'Photo Database')
    _plain(database, root)
    if not database.is_file():
        return []
    info = database.stat()
    database_stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
    data = _database_bytes(database, 64 * 1024 * 1024, database_stamp)
    at, end, count = _list(data, b'mhfd', b'mhli')
    formats = {1009: (42, 30), 1015: (130, 88), 1024: (320, 240), 1036: (50, 41),
               1017: (56, 56), 1016: (140, 140), 1028: (100, 100), 1029: (200, 200)}
    from .host_cache import cache_base
    base = cache_base() / 'retail'
    # An Apple database rewrite may change one title or just serialization.
    # Keep unchanged thumbnail pixels reusable independently of that database.
    cache = base / ('images-v2-' + hashlib.sha256(cache_volume_scope(root).encode()).hexdigest())
    result = {}
    paths = {}
    file_info = {}
    file_stamps = {}
    pending, queued = deque(), set()
    selected = []
    for order in range(count):
        child, following = _record(data, at, end, b'mhii')
        if child - at < 28:
            raise ValueError('Truncated iTunes image')
        key = _u32(data, at + 16)
        dbid = struct.unpack_from('<Q', data, at + 20)[0]
        title, candidates = f'Photo {key}', []
        while child < following:
            body, next_child = _record(data, child, following, b'mhod')
            kind = struct.unpack_from('<H', data, child + 12)[0]
            if kind == 1:
                title = _image_string(data, child, next_child) or title
            elif kind == 2:
                name_at, thumb_end = _record(data, body, next_child, b'mhni')
                if name_at - body < 36:
                    raise ValueError('Truncated iTunes thumbnail')
                format_id, offset, length = struct.unpack_from('<III', data, body + 16)
                if format_id in formats:
                    _, name_end = _record(data, name_at, thumb_end, b'mhod')
                    name = _image_string(data, name_at, name_end)
                    if name not in paths:
                        paths[name] = _path(folder, name)
                    path = paths[name]
                    width, height = formats[format_id]
                    vertical, horizontal, image_h, image_w = struct.unpack_from('<4H', data, body + 28)
                    candidates.append((width * height, path, offset, length, width, height,
                                       horizontal, vertical, image_w, image_h))
            child = next_child
        if candidates and (dbids is None or dbid in dbids):
            selected.append((order, key, dbid, title,
                             sorted(candidates, key=lambda c: c[0], reverse=True)))
        at = following
    if render and len({row[1] for row in selected}) == len(selected):
        selected.sort(key=lambda row: (str(row[4][0][1]).casefold(), row[4][0][2]))
    with _ThumbnailReaders() as readers, ExitStack() as workers:
        encoder = None
        for order, key, dbid, title, candidates in selected:
            for _, path, offset, length, width, height, x, y, iw, ih in candidates:
                info = file_info.get(path)
                if info is None:
                    try:
                        info = path.stat()
                    except FileNotFoundError:
                        continue
                    file_info[path] = info
                    file_stamps[path] = (path.relative_to(root).as_posix().casefold(), file_stamp(info))
                if length < width * height * 2 or offset + length > info.st_size:
                    raise ValueError('iTunes thumbnail extends beyond its file')
                stamp = hashlib.sha256(json.dumps((
                    'rgb565-crop-v1', *file_stamps[path], offset, length, width, height, x, y, iw, ih,
                )).encode()).hexdigest()
                preview = cache / (stamp + '.png')
                if render and preview not in queued and not preview.is_file():
                    from PIL import Image
                    try:
                        pixels = readers.read(path, offset, width * height * 2)
                    except FileNotFoundError:
                        continue
                    image = Image.frombytes('RGB', (width, height), pixels, 'raw', 'BGR;16')
                    if 0 < iw <= width and 0 < ih <= height and x < iw and y < ih:
                        image = image.crop((x, y, iw, ih))
                    cache.mkdir(parents=True, exist_ok=True)
                    if encoder is None:
                        encoder = workers.enter_context(ThreadPoolExecutor(max_workers=4))
                    queued.add(preview)
                    pending.append(encoder.submit(_write_preview, image, preview))
                    if len(pending) >= 4:
                        pending.popleft().result()
                result[order] = dict(key=key, dbid=dbid, title=title, preview=str(preview),
                                     source=str(database), bytes=length, mtime_ns=info.st_mtime_ns,
                                     relative=f'Photos/@iTunes/{key}.png')
                break
        while pending:
            pending.popleft().result()
        for path, before in file_info.items():
            _plain(path, folder)
            after = path.stat()
            if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                    after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                raise LibraryBusy()
    _check_database_stamp(database, database_stamp)
    return [result[order] for order in sorted(result)]


def metadata(root, *, tracks_rows=None):
    from .album_art import _identity, cached
    from .host_cache import cache_base
    rows = available_tracks(root) if tracks_rows is None else tracks_rows
    if not rows:
        return {}
    used_dbids = {row['dbid'] for row in rows.values()}
    try:
        covers = {row['dbid']: row['preview'] for row in images(root, artwork=True, dbids=used_dbids)}
    except (OSError, ValueError, UnicodeError, struct.error):
        covers = {}
    cached_covers = {}
    cache = None
    def artwork(row):
        nonlocal cache
        if row['dbid'] in covers:
            return covers[row['dbid']]
        album = _identity(row)
        if album not in cached_covers:
            if cache is None and all(album):
                cache = cache_base()
            cached_covers[album] = cached(row, cache=cache)
        return cached_covers[album]
    return {path: dict(row, artwork=artwork(row)) for path, row in rows.items()}
