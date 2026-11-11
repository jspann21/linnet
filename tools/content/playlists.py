import os
from pathlib import Path
import re
import struct
import zlib
from collections import defaultdict
from urllib.parse import unquote, urlsplit

from . import media_manager as manager
from .music import MAX_DATABASE, TRACK, validate_database
from .publish import _bounded, _exclusive, _plain, _volume_guard, provision_collections

SIZE, SLOTS, CAPACITY = 147456, 16, 1024
RESUME = 80 + 6 * CAPACITY + 44
LISTS, ENTRY = 512 + 4 + RESUME, 40 + 8 * CAPACITY
RECENT = LISTS + SLOTS * ENTRY
MAX_M3U_BYTES = 2 * 1024 * 1024
MAX_M3U_ENTRIES = 10000


def _m3u_key(path):
    """Normalize a playlist reference without resolving or opening a file."""
    key = path.strip(' \t').replace('\\', '/')
    if key.casefold().startswith('file://'):
        key = unquote(urlsplit(key).path, errors='strict')
    key = key.lstrip('/')
    if re.match(r'^[A-Za-z]:/', key):
        key = key[3:]
    while key.startswith('./'):
        key = key[2:]
    return key.casefold()


def preview_m3u(source, tracks, paths=None):
    """Match an M3U to known library IDs; leave missing/ambiguous rows for review.

    The file is only read as text. In particular, its paths are never followed.
    Matches retain file order and duplicates, as the device playlist does.
    """
    source = Path(source)
    if source.stat().st_size > MAX_M3U_BYTES:
        raise ValueError('M3U file exceeds the 2 MiB import limit')
    with source.open('rb') as reader:
        data = reader.read(MAX_M3U_BYTES + 1)
    if len(data) > MAX_M3U_BYTES or b'\0' in data:
        raise ValueError('M3U file is too large or contains invalid text')
    text = data.decode('utf-8-sig')
    by_path, by_basename, by_label = defaultdict(list), defaultdict(list), defaultdict(list)
    for identity, label in tracks.items():
        by_label[label.casefold()].append(identity)
        if paths and identity in paths:
            key = _m3u_key(paths[identity])
            by_path[key].append(identity)
            by_basename[key.rsplit('/', 1)[-1]].append((identity, key))
    matched, unmatched = [], []
    hint, label = None, None
    for line_number, raw in enumerate(re.split(r'\r\n|\r|\n', text), 1):
        line = raw.strip(' \t')
        if not line:
            continue
        if line.startswith('#LINNET-ID:'):
            try:
                hint = int(line.partition(':')[2].strip(), 16)
            except ValueError:
                hint = None
            continue
        if line.startswith('#EXTINF:'):
            label = line.partition(',')[2].strip() or None
            continue
        if line.startswith('#'):
            continue
        if len(matched) + len(unmatched) >= MAX_M3U_ENTRIES:
            raise ValueError('M3U file exceeds the 10,000-entry review limit')
        key = _m3u_key(line)
        candidates = by_path.get(key, ())
        if not candidates:
            basename = key.rsplit('/', 1)[-1]
            candidates = [identity for identity, known in by_basename.get(basename, ())
                          if known.endswith('/' + key) or key.endswith('/' + known)]
        if not candidates and hint in tracks:
            candidates = (hint,)
        if not candidates and label:
            candidates = by_label.get(label.casefold(), ())
        if len(candidates) == 1:
            matched.append(candidates[0])
        else:
            unmatched.append((line_number, line, 'ambiguous' if candidates else 'not in library'))
        hint, label = None, None
    return matched, unmatched


def export_m3u(ids, tracks, paths=None):
    if len(ids) > CAPACITY:
        raise ValueError('A playlist can contain at most 1024 songs')
    lines = ['#EXTM3U']
    for identity in ids:
        if identity not in tracks:
            raise ValueError(f'Track {identity:016x} is missing from the music library')
        label = tracks[identity].replace('\r', ' ').replace('\n', ' ')
        location = (paths or {}).get(identity, f'LINNET-ID-{identity:016x}')
        if '\r' in location or '\n' in location or '\0' in location:
            raise ValueError('A track path contains invalid text')
        lines.extend((f'#EXTINF:-1,{label}', f'#LINNET-ID:{identity:016x}', location))
    return ('\n'.join(lines) + '\n').encode('utf-8')


def decode(data):
    if len(data) != SIZE:
        raise ValueError('Unexpected playlist bank size')
    raw = memoryview(data)
    magic, version, generation, checksum = struct.unpack_from('<4sIII', raw)
    actual_crc = zlib.crc32(raw[16:], zlib.crc32(bytes(4), zlib.crc32(raw[:12])))
    if magic != b'LLST' or version != 1 or not generation or actual_crc != checksum:
        raise ValueError('Invalid playlist bank checksum or version')
    if struct.unpack_from('<I', raw, 512)[0] > RESUME or struct.unpack_from('<I', raw, RECENT)[0] > 256:
        raise ValueError('Invalid playlist playback state')
    lists = []
    for slot in range(SLOTS):
        at = LISTS + slot * ENTRY
        count = struct.unpack_from('<I', raw, at + 32)[0]
        if raw[at + 31] or count > CAPACITY or (not raw[at] and count):
            raise ValueError('Invalid playlist entry')
        lists.append(dict(name=bytes(raw[at:at + 32]).split(b'\0', 1)[0].decode('utf-8'),
                          ids=list(struct.unpack_from(f'<{count}Q', raw, at + 40))))
    return generation, lists


def banks(root):
    paths = [root / '.LINNET' / f'LISTS{i}.BIN' for i in range(2)]
    snapshots, valid = [], []
    for index, path in enumerate(paths):
        _plain(path, root)
        if not path.exists():
            snapshots.append(None)
            continue
        if path.stat().st_size != SIZE:
            raise ValueError('Unexpected playlist bank size; preserving it')
        data = _bounded(path, SIZE)
        snapshots.append(data)
        try:
            generation, lists = decode(data)
            valid.append((generation, index, lists))
        except ValueError:
            pass
    if not valid:
        if any(data is not None for data in snapshots):
            raise ValueError('No valid playlist bank; preserving iPod state')
        return snapshots, None, [dict(name='', ids=[]) for _ in range(SLOTS)]
    if len(valid) == 2 and valid[0][0] == valid[1][0] and snapshots[0] != snapshots[1]:
        raise ValueError('Playlist banks disagree at the same generation')
    _, active, lists = max(valid, key=lambda item: item[0])
    return snapshots, active, lists


def read(identity):
    identity = manager.refresh(identity)
    root = Path(identity['root'])
    snapshots, _, lists = banks(root)
    path = root / '.LINNET' / 'LIBRARY.PDB'
    _plain(path, root)
    tracks = {}
    paths = {}
    if path.exists():
        if path.stat().st_size > MAX_DATABASE:
            raise ValueError('Music library is too large')
        data = _bounded(path, MAX_DATABASE)
        validate_database(data)
        width = TRACK
        fields = struct.unpack_from('<14I', data, 8)
        labels = {}
        for index in range(fields[2]):
            at = fields[6] + index * width
            strings = []
            for offset in struct.unpack_from('<3I', data, at + 52):
                if offset not in labels:
                    start = fields[10] + offset
                    labels[offset] = data[start:data.index(b'\0', start)].decode('utf-8')
                strings.append(labels[offset])
            tracks[struct.unpack_from('<Q', data, at)[0]] = ' · '.join(filter(None, strings))
            path_offset = struct.unpack_from('<I', data, at + 64)[0]
            if path_offset not in labels:
                start = fields[10] + path_offset
                labels[path_offset] = data[start:data.index(b'\0', start)].decode('utf-8')
            paths[struct.unpack_from('<Q', data, at)[0]] = labels[path_offset]
    return dict(identity=identity, snapshots=snapshots, lists=lists, tracks=tracks, paths=paths)


def save(state):
    identity = manager.refresh(state['identity'])
    root = Path(identity['root'])
    with _exclusive(identity):
        snapshots, active, _ = banks(root)
        if snapshots != state['snapshots']:
            raise ValueError('Playlists changed on the iPod. Reopen Playlists before saving.')
        if len(state['lists']) != SLOTS:
            raise ValueError('The iPod supports 16 playlists')
        if active is None:
            data = bytearray(SIZE)
            struct.pack_into('<4sIII', data, 0, b'LLST', 1, 1, 0)
        else:
            data = bytearray(snapshots[active])
        generation = struct.unpack_from('<I', data, 8)[0]
        if generation == 0xffffffff:
            raise ValueError('Playlist generation limit reached')
        for slot, playlist in enumerate(state['lists']):
            name, ids = playlist['name'].encode('utf-8'), playlist['ids']
            if len(name) > 31 or b'\0' in name or len(ids) > CAPACITY or (not name and ids):
                raise ValueError('Use names of 1–31 UTF-8 bytes and at most 1024 tracks per playlist')
            at = LISTS + slot * ENTRY
            data[at:at + ENTRY] = bytes(ENTRY)
            data[at:at + len(name)] = name
            struct.pack_into('<I', data, at + 32, len(ids))
            struct.pack_into(f'<{len(ids)}Q', data, at + 40, *ids)
        struct.pack_into('<II', data, 8, generation + 1, 0)
        struct.pack_into('<I', data, 12, zlib.crc32(data))
        decode(data)
        guard = lambda: _volume_guard(identity)
        directory = root / '.LINNET'
        _plain(directory, root)
        guard()
        directory.mkdir(exist_ok=True)
        provision_collections(directory, guard)
        target = directory / f'LISTS{1 - active if active is not None else 1}.BIN'
        guard()
        with target.open('r+b') as writer:
            writer.seek(512)
            writer.write(data[512:])
            writer.flush()
            os.fsync(writer.fileno())
            guard()
            writer.seek(0)
            writer.write(data[:512])
            writer.flush()
            os.fsync(writer.fileno())
        guard()
        if target.read_bytes() != data:
            raise OSError('Playlist save readback failed; reopen Playlists before retrying')
