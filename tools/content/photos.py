"""Deterministic LPIC compiler and bounded, streaming package validator."""
from __future__ import annotations

import hashlib
import inspect as source_inspect
import json
import os
from contextlib import ExitStack
from collections import deque
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
from pathlib import Path
import re
import struct
import tempfile
import threading
import time
import unicodedata
import zlib
from . import SourceError

HEADER, ALBUM, PHOTO, LEVEL, TILE = 512, 32, 64, 24, 12
PACK_COUNT, PACK_LIMIT = 32, 1 << 27
MAX_FILE, MAX_PHOTOS, MAX_ALBUMS, MAX_TILES = 0x7fffffff, 65535, 4096, 1_000_000
MAX_STRINGS, MAX_LEVELS = 16 * 1024 * 1024, 6
THUMB, EDGE, NO_INDEX = 64, 32, 0xffffffff
DEVICE_PATH = '/.LINNET/PHOTOS/LIBRARY.PIM'
EXTENSIONS = {'.jpg', '.jpeg', '.png', '.bmp', '.tif', '.tiff', '.webp'}
_GREEN_LOW = [(v & 28) << 3 for v in range(256)]
_BLUE_LOW = [v >> 3 for v in range(256)]
_RED_HIGH = [v & 248 for v in range(256)]
_GREEN_HIGH = [v >> 5 for v in range(256)]
FAT_PATH = re.compile(r'(?:/[A-Z0-9_]{1,8}(?:\.[A-Z0-9_]{1,3})?)+')
PHOTO_CACHE_BYTES = 512 * 1024 * 1024
PHOTO_CACHE_ENTRY_BYTES = 32 * 1024 * 1024


def _source_stamp(info):
    return (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)


def _crc(data):
    return zlib.crc32(data) & 0xffffffff


def fallback_jpeg_header_compatible(data):
    """Accept only the marker and component layout understood by device JPEG fallback."""
    if len(data) > 262144 or data[:2] != b'\xff\xd8':
        return False
    at, markers, components, quant_ids, adobe = 2, 0, None, None, None
    quant, huffman = set(), set()
    while at < len(data):
        if data[at] != 0xff:
            return False
        start = at
        while at < len(data) and data[at] == 0xff:
            at += 1
        markers += at - start
        if markers > 4096 or at + 2 >= len(data):
            return False
        marker = data[at]
        at += 1
        size = int.from_bytes(data[at:at+2], 'big')
        if size < 2 or at + size > len(data):
            return False
        payload = data[at+2:at+size]
        at += size
        if marker == 0xdb:
            if len(payload) > 260 or len(payload) % 65:
                return False
            for pos in range(0, len(payload), 65):
                spec = payload[pos]
                if spec >= 4 or spec in quant or 0 in payload[pos+1:pos+65]:
                    return False
                quant.add(spec)
        elif marker == 0xc4:
            if len(payload) > 2184:
                return False
            pos = 0
            while pos < len(payload):
                if len(payload) - pos < 17:
                    return False
                spec = payload[pos]
                key = (spec >> 4, spec & 15)
                counts = payload[pos+1:pos+17]
                total = sum(counts)
                if key[0] > 1 or key[1] >= 4 or key in huffman or not 1 <= total <= 256 or pos+17+total > len(payload):
                    return False
                code = 0
                for depth, count in enumerate(counts, 1):
                    code += count
                    if code > 1 << depth or (count and code == 1 << depth):
                        return False
                    code <<= 1
                symbols = payload[pos+17:pos+17+total]
                if any(value > 11 if key[0] == 0 else
                       (value & 15) > 10 or ((value & 15) == 0 and value not in (0, 240))
                       for value in symbols):
                    return False
                huffman.add(key)
                pos += 17 + total
        elif marker == 0xdd:
            if len(payload) != 2:
                return False
        elif marker == 0xc0:
            count = payload[5] if len(payload) >= 6 else 0
            if components or payload[:1] != b'\x08' or count not in (1, 3) or len(payload) != 6 + count*3:
                return False
            components = [payload[6+i*3] for i in range(count)]
            quant_ids = [payload[8+i*3] for i in range(count)]
            if len(set(components)) != count or (count == 3 and components != [1, 2, 3]):
                return False
            if any(not 1 <= payload[7+i*3] >> 4 <= 2 or
                   not 1 <= (payload[7+i*3] & 15) <= 2 or
                   (i and payload[7+i*3] != 0x11) or payload[8+i*3] >= 4
                   for i in range(count)):
                return False
            if count == 1 and payload[7] != 0x11:
                return False
        elif marker == 0xee and payload.startswith(b'Adobe') and len(payload) >= 12:
            adobe = payload[11]
        elif marker == 0xda:
            return (components is not None and len(payload) == 4 + 2*len(components) and
                    payload[0] == len(components) and
                    [payload[1+i*2] for i in range(len(components))] == components and
                    all(q in quant for q in quant_ids) and
                    all((0, payload[2+i*2] >> 4) in huffman and
                        (1, payload[2+i*2] & 15) in huffman for i in range(len(components))) and
                    payload[-3:] == b'\x00\x3f\x00' and
                    (len(components) == 1 or adobe in (None, 1)))
        elif marker != 0xfe and not 0xe0 <= marker <= 0xef:
            return False
    return False


def _record(data):
    return data + struct.pack('<I', _crc(data))


def _title(value):
    value = unicodedata.normalize('NFC', str(value)).strip()
    if not value or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('photo/album title is empty or contains control characters')
    if len(value.encode('utf-8')) > 191:
        raise ValueError('photo/album title exceeds 191 UTF-8 bytes')
    return value


def _rgb565(image):
    from PIL import Image, ImageChops

    # Pack little-endian bytes through Pillow's C channel operations. Each
    # addition combines disjoint bits, preserving exact truncation without
    # dithering, rescaling or a Python operation for every output pixel.
    red, green, blue = (image if image.mode == 'RGB' else image.convert('RGB')).split()
    low = ImageChops.add(green.point(_GREEN_LOW), blue.point(_BLUE_LOW))
    high = ImageChops.add(red.point(_RED_HIGH), green.point(_GREEN_HIGH))
    return Image.merge('LA', (low, high)).tobytes()


def _fit(width, height, bound_w, bound_h):
    scale = min(1.0, bound_w / width, bound_h / height)
    return max(1, int(width * scale)), max(1, int(height * scale))


def _sizes(width, height, max_dimension, viewport_width, viewport_height):
    largest = _fit(width, height, max_dimension, max_dimension)
    result = [_fit(width, height, viewport_width, viewport_height)]
    while result[-1] != largest:
        previous = result[-1]
        bound = min(2 * max(previous), max(largest))
        following = _fit(width, height, bound, bound)
        if following == previous:
            following = largest
        result.append(following)
        if len(result) > MAX_LEVELS:
            raise ValueError('photo pyramid exceeds six levels')
    return result


class _PhotoCache:
    """Bounded, disposable cache of one image's prepared pixel payload."""
    def __init__(self, directory, recipe, source):
        from .host_state import cache_volume_scope

        self.directory = Path(directory).resolve()
        self.recipe = recipe
        self.source = Path(source).resolve()
        self.source_scope = (cache_volume_scope(Path(self.source.anchor)) +
                             self.source.relative_to(self.source.anchor).as_posix())
        self.hash_path = self.directory / 'source-hashes.json'
        self.hashes = {}
        self.hashes_at = time.time()
        self.hashes_dirty = False
        self.entries = {}
        self.bytes = 0
        self.lock = threading.Lock()
        self.directory.mkdir(parents=True, exist_ok=True)
        for path in self.directory.glob('*.bin'):
            if not re.fullmatch(r'[0-9a-f]{64}', path.stem):
                continue
            try:
                info = path.stat()
            except OSError:
                continue
            self.entries[path] = (info.st_size, info.st_mtime_ns)
            self.bytes += info.st_size
        try:
            if self.hash_path.stat().st_size <= 16 * 1024 * 1024:
                saved = json.loads(self.hash_path.read_text(encoding='utf-8'))
                rows = saved.get('files', [])
                verified_at = saved.get('verified_at')
                if (saved.get('version') == 1 and saved.get('scope') == self.source_scope and
                        type(verified_at) in (int, float) and self.hashes_at - 3600 <= verified_at <= self.hashes_at and
                        isinstance(rows, list) and len(rows) <= MAX_PHOTOS):
                    for row in rows:
                        if (isinstance(row, list) and len(row) == 6 and isinstance(row[0], str) and
                                all(type(value) is int for value in row[1:5]) and
                                isinstance(row[5], str) and re.fullmatch(r'[0-9a-f]{64}', row[5])):
                            self.hashes[row[0]] = (tuple(row[1:5]), row[5])
                    self.hashes_at = verified_at
        except (OSError, ValueError, TypeError, AttributeError):
            pass

    def key(self, path, name, title, fallback, checkpoint, info, *, force_hash=False):
        relative = path.relative_to(self.source).as_posix() if path.is_relative_to(self.source) else None
        stamp = _source_stamp(info)
        now = time.time()
        recent = os.name == 'nt' and info.st_mtime_ns > time.time_ns() - 5_000_000_000
        with self.lock:
            if not 0 <= now - self.hashes_at <= 3600:
                self.hashes.clear()
                self.hashes_at = now
                self.hashes_dirty = True
            remembered = (self.hashes.get(relative)
                          if relative is not None and not recent and not force_hash else None)
        source_digest = remembered[1] if remembered is not None and remembered[0] == stamp else None
        reused_hash = source_digest is not None
        if source_digest is None:
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                chunk_size = min(1024 * 1024, max(16 * 1024, os.fstat(stream.fileno()).st_size))
                while chunk := stream.read(chunk_size):
                    checkpoint()
                    digest.update(chunk)
            source_digest = digest.hexdigest()
            if relative is not None and not recent:
                with self.lock:
                    self.hashes[relative] = (stamp, source_digest)
                    self.hashes_dirty = True
        identity = [name, title, fallback, source_digest, self.recipe]
        return hashlib.sha256(json.dumps(identity, separators=(',', ':')).encode()).hexdigest(), reused_hash

    def save_hashes(self):
        if not self.hashes_dirty:
            return
        from .host_state import save_json
        try:
            save_json(self.hash_path, dict(version=1, scope=self.source_scope,
                verified_at=self.hashes_at,
                files=[[name, *stamp, digest] for name, (stamp, digest) in self.hashes.items()]))
        except OSError:
            pass

    def get(self, key, checkpoint):
        path = self.directory / (key + '.bin')
        try:
            if not path.is_file() or path.stat().st_size > PHOTO_CACHE_ENTRY_BYTES:
                return None
            with path.open('rb') as stream:
                length = int.from_bytes(stream.read(4), 'little')
                if not 0 < length <= 1024 * 1024:
                    return None
                metadata = json.loads(stream.read(length))
                remaining = os.fstat(stream.fileno()).st_size - stream.tell()
                if remaining < 0:
                    return None
                payload = stream.read(min(remaining, PHOTO_CACHE_ENTRY_BYTES) + 1)
                if stream.read(1):
                    return None
            if (not isinstance(metadata, dict) or metadata.get('key') != key or
                    len(payload) > PHOTO_CACHE_ENTRY_BYTES or
                    len(payload) != metadata.get('bytes') or
                    hashlib.sha256(payload).hexdigest() != metadata.get('sha256') or
                    _cache_digest(metadata, payload) != metadata.get('entry_sha256')):
                return None
            if not _cached_photo_valid(metadata, payload, checkpoint):
                return None
            try:
                os.utime(path, None)
                info = path.stat()
                with self.lock:
                    self.entries[path] = (info.st_size, info.st_mtime_ns)
            except OSError:
                pass
            grid_bytes = metadata.get('grid_width', 0)*metadata.get('grid_height', 0)*2
            tile_end = len(payload)-grid_bytes
            thumb, tiles, grid = (payload[:THUMB*THUMB*2],
                                  payload[THUMB*THUMB*2:tile_end], payload[tile_end:])
            return (metadata, thumb, b'', grid), tiles, metadata['packed_lengths']
        except (OSError, ValueError, TypeError, KeyError, UnicodeError):
            return None

    def put(self, key, metadata, thumb, packed, lengths, grid_pixels=b''):
        payload = thumb + packed + grid_pixels
        metadata = dict(metadata, cache_format='packed-v1', packed_lengths=lengths,
                        key=key, bytes=len(payload),
                        sha256=hashlib.sha256(payload).hexdigest())
        metadata['entry_sha256'] = _cache_digest(metadata, payload)
        header = json.dumps(metadata, separators=(',', ':')).encode()
        if len(header) + len(payload) + 4 > PHOTO_CACHE_ENTRY_BYTES:
            return
        path = self.directory / (key + '.bin')
        temporary = None
        try:
            fd, name = tempfile.mkstemp(prefix=key + '.', suffix='.tmp', dir=self.directory)
            temporary = Path(name)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(len(header).to_bytes(4, 'little'))
                stream.write(header)
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            info = path.stat()
            with self.lock:
                previous = self.entries.get(path, (0, 0))[0]
                self.entries[path] = (info.st_size, info.st_mtime_ns)
                self.bytes += info.st_size - previous
                self._trim()
        except OSError:
            pass  # A host cache failure must not fail an otherwise valid LPIC.
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass

    def _trim(self):
        if self.bytes <= PHOTO_CACHE_BYTES:
            return
        for path, (size, _) in sorted(self.entries.items(), key=lambda item: item[1][1]):
            try:
                path.unlink()
            except OSError:
                continue
            self.bytes -= size
            del self.entries[path]
            if self.bytes <= PHOTO_CACHE_BYTES:
                break


def _cached_photo_valid(meta, payload, checkpoint):
    if (type(meta.get('width')) is not int or type(meta.get('height')) is not int or
            not 1 <= meta['width'] <= 16384 or not 1 <= meta['height'] <= 16384 or
            type(meta.get('fallback_bytes')) is not int or not 0 <= meta['fallback_bytes'] <= 262144 or
            type(meta.get('fallback_crc')) is not int or not 0 <= meta['fallback_crc'] <= 0xffffffff or
            type(meta.get('orientation')) is not int or not 0 <= meta['orientation'] <= 8 or
            type(meta.get('thumb_crc')) is not int or not 0 <= meta['thumb_crc'] <= 0xffffffff or
            not isinstance(meta.get('levels'), list) or not 1 <= len(meta['levels']) <= MAX_LEVELS or
            not isinstance(meta.get('tile_crcs'), list)):
        return False
    counts = 0
    for level in meta['levels']:
        if (not isinstance(level, list) or len(level) != 5 or
                any(type(value) is not int or value < 1 or value > MAX_TILES for value in level)):
            return False
        width, height, tx, ty, count = level
        if (width > 2048 or height > 2048 or tx != (width + EDGE - 1)//EDGE or
                ty != (height + EDGE - 1)//EDGE or count != tx*ty):
            return False
        counts += count
    gw, gh = meta.get('grid_width', 0), meta.get('grid_height', 0)
    if type(gw) is not int or type(gh) is not int or (gw, gh) not in ((0,0),(56,56),(50,38),(40,30)):
        return False
    grid_bytes = gw*gh*2
    if grid_bytes and (type(meta.get('grid_crc')) is not int or _crc(payload[-grid_bytes:]) != meta['grid_crc']):
        return False
    if counts != len(meta['tile_crcs']) or _crc(payload[:THUMB*THUMB*2]) != meta.get('thumb_crc'):
        return False
    if meta.get('cache_format') != 'packed-v1':
        return False
    lengths = meta.get('packed_lengths')
    if (not isinstance(lengths, list) or len(lengths) != counts or
            any(type(length) is not int or not 1 <= length <= 2048 for length in lengths) or
            len(payload) != THUMB*THUMB*2 + sum(lengths) + grid_bytes):
        return False
    from lz4.block import decompress as lz4_decompress, LZ4BlockError
    offset = THUMB*THUMB*2
    for index, expected in enumerate(meta['tile_crcs']):
        if index % 256 == 0:
            checkpoint()
        length = lengths[index]
        tile = payload[offset:offset + length]
        offset += length
        if length < 2048:
            try:
                tile = lz4_decompress(tile, uncompressed_size=2048)
            except LZ4BlockError:
                return False
        if type(expected) is not int or len(tile) != 2048 or _crc(tile) != expected:
            return False
    return True


def _cache_digest(metadata, payload):
    signed = {name: value for name, value in metadata.items() if name != 'entry_sha256'}
    digest = hashlib.sha256(json.dumps(signed, sort_keys=True, separators=(',', ':')).encode())
    digest.update(payload)
    return digest.hexdigest()


def _render_photo(path, fallback, max_dimension, viewport_width, viewport_height,
                  checkpoint, grid_shape=None):
    from PIL import Image, ImageOps

    fallback_bytes = fallback_crc = orientation = 0
    try:
        with Image.open(path) as original:
            width, height = original.size
            source_orientation = original.getexif().get(274, 1)
            if source_orientation not in range(1, 9):
                raise ValueError(f'invalid EXIF orientation: {path.name}')
            if not width or not height or max(width, height) > 16384 or width * height > 32_000_000:
                raise ValueError(f'image dimensions exceed the host decode bound: {path.name}')
            if getattr(original, 'n_frames', 1) != 1:
                raise ValueError(f'animated/multiframe image is unsupported: {path.name}')
            if fallback:
                with path.open('rb') as source:
                    source_bytes = source.read(262145)
                fallback_bytes = len(source_bytes)
                if (original.format != 'JPEG' or original.mode not in ('RGB', 'L') or
                    original.info.get('progressive') or original.info.get('progression') or
                    fallback_bytes > 262144 or max(width, height) > 2048 or width * height > 4_000_000 or
                    not fallback_jpeg_header_compatible(source_bytes)):
                    raise ValueError('fallback JPEG exceeds baseline format/size bounds')
                orientation = source_orientation
                fallback_crc = _crc(source_bytes)
            drafted = not fallback and original.format == 'JPEG'
            if drafted:
                side = 2 * max(max_dimension, viewport_width, viewport_height,
                               *(grid_shape or ()))
                original.draft(original.mode, (side, side))
            oriented = ImageOps.exif_transpose(original) if source_orientation != 1 else original
            if 'A' not in oriented.getbands() and 'transparency' not in oriented.info:
                if oriented.mode == 'RGB':
                    oriented.load()
                    image = oriented
                else:
                    image = oriented.convert('RGB')
            else:
                rgba = oriented.convert('RGBA')
                image = Image.new('RGB', rgba.size, (0, 0, 0))
                image.paste(rgba, mask=rgba.getchannel('A'))
    except (OSError, ValueError, SyntaxError, Image.DecompressionBombError) as error:
        raise SourceError(f'Could not prepare photo {path}: {error}', path) from error

    if drafted:
        if source_orientation in (5, 6, 7, 8):
            width, height = height, width
    else:
        width, height = image.size
    thumb = Image.new('RGB', (THUMB, THUMB), (0, 0, 0))
    tw, th = _fit(width, height, THUMB, THUMB)
    thumb.paste(image.resize((tw, th), Image.Resampling.LANCZOS, reducing_gap=3.0),
                ((THUMB-tw)//2, (THUMB-th)//2))
    raw_thumb = _rgb565(thumb)
    local_levels, local_crcs, tile_bytes = [], [], bytearray()
    for lw, lh in _sizes(width, height, max_dimension, viewport_width, viewport_height):
        checkpoint()
        tx, ty = (lw+EDGE-1)//EDGE, (lh+EDGE-1)//EDGE
        resized = image if image.size == (lw, lh) else image.resize(
            (lw, lh), Image.Resampling.LANCZOS, reducing_gap=3.0)
        if (lw, lh) == (tx*EDGE, ty*EDGE):
            padded = resized
        else:
            padded = Image.new('RGB', (tx*EDGE, ty*EDGE), (0, 0, 0))
            padded.paste(resized)
        raw = _rgb565(padded)
        stride = tx * EDGE * 2
        tile_rows = struct.Struct(f'<{EDGE*2}s' + f'{stride-EDGE*2}x{EDGE*2}s' * (EDGE-1))
        for y in range(ty):
            for x in range(tx):
                start = y * EDGE * stride + x * EDGE * 2
                tile = b''.join(tile_rows.unpack_from(raw, start))
                tile_bytes.extend(tile)
                local_crcs.append(_crc(tile))
        local_levels.append([lw, lh, tx, ty, tx*ty])
    metadata = dict(width=width, height=height, fallback_bytes=fallback_bytes,
                    fallback_crc=fallback_crc, orientation=orientation,
                    thumb_crc=_crc(raw_thumb), levels=local_levels,
                    tile_crcs=local_crcs)
    grid_pixels = b''
    if grid_shape is not None:
        gw, gh = grid_shape
        grid_pixels = _rgb565(ImageOps.fit(image, (gw, gh), method=Image.Resampling.LANCZOS))
        metadata.update(grid_width=gw, grid_height=gh, grid_crc=_crc(grid_pixels))
    return metadata, raw_thumb, bytes(tile_bytes), grid_pixels


def _pack_photo_tiles(tile_bytes, crcs, checkpoint):
    from lz4.block import compress as lz4_compress

    packed = bytearray()
    lengths = []
    for index in range(len(crcs)):
        if index % 256 == 0:
            checkpoint()
        raw = tile_bytes[index*2048:(index+1)*2048]
        compressed = lz4_compress(raw, mode='high_compression', compression=3,
                                  store_size=False)
        stored = compressed if len(compressed) < len(raw) else raw
        lengths.append(len(stored))
        packed.extend(stored)
    return bytes(packed), lengths


def _pack_path(directory, identity):
    return directory / f'{identity[:8].upper()}' / f'{identity[8:16].upper()}.PAK'


DEFAULT_PACKS = 8


def _pack_bucket(name):
    # Reserve sequential packs for default viewer tiles. Hash zoom levels so
    # editing one photo does not rewrite an entire default-resolution catalog.
    digest = hashlib.sha256(name.casefold().encode('utf-8')).digest()[0]
    return DEFAULT_PACKS + digest % (PACK_COUNT - DEFAULT_PACKS)


def compile_photos(source, output, *, max_dimension=640, viewport_width=220,
                   viewport_height=156, fallback_paths=None, excluded=None, additional_sources=None,
                   progress=None, checkpoint=None, cache_dir=None, target=None, grid_output=None):
    """Prepare a folder into one .pimg; optional mapping names existing JPEG copies.

    The default keeps zoom detail without a desktop-sized tile pyramid.
    fallback_paths maps source-relative filenames to their already-provisioned
    absolute FAT8.3 device paths. The compiler never copies or modifies originals.
    """
    from PIL import __version__ as pillow_version
    check = checkpoint or (lambda: None)
    check()
    packed_target = target in ('video5g-m25', 'classic6g-n25')
    source, output = Path(source).resolve(), Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    grid_shape = None
    if target is not None:
        from .photo_grid import GRID_SHAPES
        if target not in GRID_SHAPES:
            raise ValueError('unsupported native photo grid target')
        grid_shape = GRID_SHAPES[target][1:]
        grid_output = Path(grid_output).resolve() if grid_output is not None else output.with_suffix('.pgr')
        if grid_output == output or grid_output == source or source in grid_output.parents:
            raise ValueError('native grid output must be separate from LPIC and source photos')
    elif grid_output is not None:
        raise ValueError('native grid output requires an explicit target')
    if (not source.is_dir() or type(viewport_width) is not int or
            type(viewport_height) is not int or type(max_dimension) is not int or not 1 <= viewport_width <= 1024 or
            not 1 <= viewport_height <= 1024 or viewport_width*viewport_height > 131072 or
            not max(viewport_width, viewport_height) <= max_dimension <= 2048):
        raise ValueError('invalid photo source, viewport, or maximum dimension')
    if output.suffix.lower() not in ('.pimg', '.pim'):
        raise ValueError('photo output must have .pimg or .pim extension')
    cache = None
    if cache_dir is not None:
        cache_root = Path(cache_dir).resolve()
        if cache_root == source or source in cache_root.parents:
            raise ValueError('photo cache must be outside the source folder')
        render_source = ''.join(source_inspect.getsource(func) for func in
                                (_rgb565, _fit, _sizes, _render_photo))
        recipe = hashlib.sha256(json.dumps([
            hashlib.sha256(render_source.encode()).hexdigest(), pillow_version,
            max_dimension, viewport_width, viewport_height, THUMB, EDGE, target, grid_shape, 'LPGR1'],
            separators=(',', ':')).encode()).hexdigest()
        try:
            cache = _PhotoCache(cache_root, recipe, source)
        except OSError:
            pass
    excluded = {Path(value).as_posix().casefold() for value in (excluded or ())}
    paths = []
    for path in source.rglob('*'):
        check()
        relative = path.relative_to(source).as_posix().casefold()
        if path.suffix.lower() in EXTENSIONS and relative not in excluded and path.is_file():
            if path.is_symlink() or not path.resolve().is_relative_to(source):
                raise ValueError('photo source links may not escape or alias the source tree')
            paths.append(path)
            if len(paths) > MAX_PHOTOS:
                raise ValueError('photo count exceeds 65535')
    names = {path: path.relative_to(source).as_posix() for path in paths}
    titles = {}
    for name, (preview, title) in (additional_sources or {}).items():
        check()
        if name.casefold() not in excluded:
            path = Path(preview)
            paths.append(path)
            names[path], titles[path] = name, title
    check()
    if len(paths) > MAX_PHOTOS:
        raise ValueError('photo count exceeds 65535')
    if not paths:
        raise ValueError('source contains no supported still images')
    paths.sort(key=lambda p: (names[p].casefold(), names[p]))
    fallback_paths = dict(fallback_paths or {})
    known = set(names.values())
    if set(fallback_paths) - known:
        raise ValueError('fallback mapping names a photo outside the selected source set')
    strings, string_index = bytearray(b'\0'), {'': (0, 0)}

    def string(value):
        if value not in string_index:
            raw = value.encode('utf-8')
            if len(strings) + len(raw) + 1 > MAX_STRINGS:
                raise ValueError('photo string pool exceeds 16 MiB')
            string_index[value] = (len(strings), len(raw))
            strings.extend(raw + b'\0')
        return string_index[value]

    photos, levels, tile_records = [], [], []
    cache_reused = cache_rendered = 0
    with tempfile.TemporaryFile() as thumbs, tempfile.TemporaryFile() as tiles, tempfile.TemporaryFile() as grid_pixels, ExitStack() as pack_stack:
        pack_spools = [pack_stack.enter_context(tempfile.TemporaryFile()) for _ in range(PACK_COUNT)] if packed_target else []
        stopped = threading.Event()

        def worker_check():
            if stopped.is_set():
                from .media_manager import Cancelled
                raise Cancelled('Stopped while preparing photos on the computer.')

        def prepare(path, name, title, fallback):
            before = path.stat()
            key, reused_hash = (cache.key(path, name, title, fallback, worker_check, before)
                                if cache is not None else (None, False))
            if cache is not None:
                after_hash = path.stat()
                if _source_stamp(before) != _source_stamp(after_hash):
                    raise SourceError(f'Photo changed while preparing: {path}', path)
            cached = cache.get(key, worker_check) if cache is not None else None
            if cached is None and reused_hash:
                # A missing pixel entry must never be rendered under a digest
                # borrowed from the source stamp cache.
                key, _ = cache.key(path, name, title, fallback, worker_check, before,
                                   force_hash=True)
                if _source_stamp(before) != _source_stamp(path.stat()):
                    raise SourceError(f'Photo changed while preparing: {path}', path)
                cached = cache.get(key, worker_check)
            reused = cached is not None
            if cached is None:
                rendered = _render_photo(path, fallback, max_dimension,
                                         viewport_width, viewport_height, worker_check, grid_shape)
                packed, lengths = _pack_photo_tiles(rendered[2], rendered[0]['tile_crcs'], worker_check)
            else:
                rendered, packed, lengths = cached
            return before, key, reused, rendered, packed, lengths

        pending = deque()
        upcoming = iter(enumerate(paths))

        def enqueue(pool):
            item = next(upcoming, None)
            if item is None:
                return
            index, path = item
            check()
            try:
                title = _title(titles.get(path, path.stem))
            except ValueError as error:
                raise SourceError(f'Could not prepare photo {path}: {error}', path) from error
            fallback = fallback_paths.get(names[path], '')
            if fallback and (not FAT_PATH.fullmatch(fallback) or len(fallback) > 127 or
                             not fallback.endswith(('.JPG', '.JPEG'))):
                raise ValueError('fallback path must be an absolute uppercase FAT8.3 JPEG path')
            future = pool.submit(prepare, path, names[path], title, fallback)
            pending.append((index, path, title, fallback, future))

        with ThreadPoolExecutor(max_workers=4, thread_name_prefix='photo-prep') as pool:
            try:
                for _ in range(min(4, len(paths))):
                    enqueue(pool)
                while pending:
                    index, path, title_text, fallback, future = pending[0]
                    check()
                    if progress:
                        progress(dict(phase='photos', current=index, total=len(paths), file=str(path)))
                    while True:
                        check()
                        try:
                            before, key, reused, rendered, packed, lengths = future.result(timeout=0.1)
                            break
                        except FutureTimeout:
                            if future.done():
                                future.result()
                            continue
                    pending.popleft()
                    after_render = path.stat()
                    if _source_stamp(before) != _source_stamp(after_render):
                        raise SourceError(f'Photo changed while preparing: {path}', path)
                    if reused:
                        cache_reused += 1
                    else:
                        cache_rendered += 1
                    image_meta, raw_thumb, _, raw_grid = rendered
                    if cache is not None and not reused:
                        cache.put(key, image_meta, raw_thumb, packed, lengths, raw_grid)
                    title = string(title_text)
                    if grid_shape is not None:
                        if len(raw_grid) != grid_shape[0]*grid_shape[1]*2:
                            raise ValueError('native photo grid cache contract mismatch')
                        grid_pixels.write(raw_grid)
                        grid_pixels.write(struct.pack('<I', _crc(raw_grid)))
                    thumbs.write(raw_thumb)
                    first_level = len(levels)
                    local_offset = 0
                    for lw, lh, tx, ty, count in image_meta['levels']:
                        first_tile = len(tile_records) + local_offset
                        if first_tile + count > MAX_TILES or ((len(paths)*8192)+(first_tile+count)*2048) > MAX_FILE:
                            raise ValueError('photo tile payload exceeds package limits')
                        levels.append((index, lw, lh, tx, ty, first_tile, count))
                        local_offset += count
                    if packed_target:
                        zoom_bucket = _pack_bucket(names[path])
                        default_count = image_meta['levels'][0][4]
                        default_bytes = sum(lengths[:default_count])
                        sequential_bucket = min(DEFAULT_PACKS - 1, index * DEFAULT_PACKS // len(paths))
                        default_bucket = (sequential_bucket
                                          if pack_spools[sequential_bucket].tell() + default_bytes <= PACK_LIMIT
                                          else zoom_bucket)
                        packed_at = 0
                        for bucket, first, end in ((default_bucket, 0, default_count),
                                                   (zoom_bucket, default_count, len(lengths))):
                            if first == end:
                                continue
                            destination = pack_spools[bucket]
                            position = destination.tell()
                            group_bytes = sum(lengths[first:end])
                            if position + group_bytes > PACK_LIMIT:
                                raise ValueError('photo pack exceeds its 128 MiB address bound')
                            for tile_index in range(first, end):
                                if tile_index % 256 == 0:
                                    check()
                                length = lengths[tile_index]
                                tile_records.append(((bucket << 27) | position, length,
                                                     image_meta['tile_crcs'][tile_index]))
                                position += length
                            destination.write(memoryview(packed)[packed_at:packed_at + group_bytes])
                            packed_at += group_bytes
                    else:
                        position = tiles.tell()
                        for tile_index, (length, crc) in enumerate(zip(lengths, image_meta['tile_crcs'])):
                            if tile_index % 256 == 0:
                                check()
                            tile_records.append((position, length, crc))
                            position += length
                        tiles.write(packed)
                    photos.append((0, *title, *string(fallback), image_meta['width'], image_meta['height'],
                                   first_level, len(levels)-first_level, 0, image_meta['thumb_crc'],
                                   image_meta['fallback_bytes'], image_meta['fallback_crc'],
                                   image_meta['orientation'], 0))
                    enqueue(pool)
            finally:
                stopped.set()
        album_records = [_record(struct.pack('<7I', *string(_title(source.name)),
                                             0, len(photos), NO_INDEX, 0, 0))]
        album_offset = HEADER
        photo_offset = album_offset + len(album_records)*ALBUM
        level_offset = photo_offset + len(photos)*PHOTO
        tile_offset = level_offset + len(levels)*LEVEL
        string_offset = tile_offset + len(tile_records)*TILE
        data_offset = (string_offset + len(strings) + 511) & ~511
        tile_base = data_offset + len(photos)*8192
        total = tile_base if packed_target else tile_base + tiles.tell()
        if total > MAX_FILE:
            raise ValueError('LPIC exceeds the 2 GiB package bound')
        pack_manifest = []
        pack_table = bytearray(PACK_COUNT * 12)
        if packed_target:
            pack_root = output.parent / 'PACKS'
            pack_root.mkdir(parents=True, exist_ok=True)
            for bucket, spool in enumerate(pack_spools):
                size = spool.tell()
                if not size:
                    continue
                spool.seek(0)
                fd, name = tempfile.mkstemp(prefix='pack-', suffix='.tmp', dir=pack_root)
                temporary_pack = Path(name)
                digest = hashlib.sha256()
                try:
                    with os.fdopen(fd, 'wb') as writer:
                        while chunk := spool.read(1024 * 1024):
                            check(); writer.write(chunk); digest.update(chunk)
                        writer.flush(); os.fsync(writer.fileno())
                    identity = digest.hexdigest()
                    final_pack = _pack_path(pack_root, identity)
                    final_pack.parent.mkdir(exist_ok=True)
                    if final_pack.exists():
                        with final_pack.open('rb') as reader:
                            same = final_pack.stat().st_size == size and hashlib.file_digest(reader, 'sha256').hexdigest() == identity
                        if not same:
                            raise ValueError('photo pack name collision or changed host cache')
                    else:
                        temporary_pack.replace(final_pack)
                finally:
                    temporary_pack.unlink(missing_ok=True)
                struct.pack_into('<8sI', pack_table, bucket*12, bytes.fromhex(identity[:16]), size)
                pack_manifest.append(dict(bucket=bucket, path=str(final_pack),
                    device_path=f'/.LINNET/PHOTOS/PACKS/{identity[:8].upper()}/{identity[8:16].upper()}.PAK',
                    bytes=size, sha256=identity))
        metadata = bytearray(b''.join(album_records))
        for index, photo in enumerate(photos):
            values = list(photo)
            values[9] = data_offset + index*8192
            metadata.extend(_record(struct.pack('<15I', *values)))
        for level in levels:
            metadata.extend(_record(struct.pack('<I4H2I', *level)))
        for index, (relative, length, crc) in enumerate(tile_records):
            if index % 4096 == 0:
                check()
            metadata.extend(struct.pack('<III', relative if packed_target else tile_base+relative, length, crc))
        metadata.extend(strings)
        metadata.extend(bytes(data_offset - HEADER - len(metadata)))
        header = bytearray(HEADER)
        struct.pack_into('<4sHH14I4H', header, 0, b'LPIC', 1 if packed_target else 0, HEADER, 0, total,
                         len(album_records), len(photos), len(levels), len(tile_records),
                         album_offset, photo_offset, level_offset, tile_offset,
                         string_offset, len(strings), _crc(metadata), data_offset,
                         THUMB, THUMB, EDGE, 1)
        if packed_target:
            header[72:72+len(pack_table)] = pack_table
        struct.pack_into('<I', header, 508, _crc(header[:508]))
        output.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary_name = tempfile.mkstemp(prefix=output.name+'.', suffix='.tmp', dir=output.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(fd, 'wb') as result:
                result.write(header)
                result.write(metadata)
                for spool in ((thumbs,) if packed_target else (thumbs, tiles)):
                    spool.seek(0)
                    while chunk := spool.read(1024*1024):
                        check()
                        result.write(chunk)
                result.flush()
                os.fsync(result.fileno())
            report = inspect(temporary, checkpoint=checkpoint)
            check()
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)
        if grid_shape is not None:
            from .photo_grid import write_grid
            report['grid'] = write_grid(output, grid_output, target, grid_pixels, check)
    report.update(output=str(output), source=str(source), device_path=DEVICE_PATH,
                  orientation_applied=True, originals_modified=False,
                  cache_reused=cache_reused, cache_rendered=cache_rendered)
    if packed_target:
        report['packs'] = pack_manifest
    if cache is not None:
        cache.save_hashes()
    return report


def inspect(path, *, verify_payloads=True, checkpoint=None):
    """Validate every record and metadata range; optionally read all tile payloads."""
    check = checkpoint or (lambda: None)
    path = Path(path)
    size = path.stat().st_size
    if not HEADER <= size <= MAX_FILE:
        raise ValueError('invalid LPIC file size')
    hashed = hashlib.sha256() if verify_payloads else None
    with path.open('rb') as stream, ExitStack() as pack_stack:
        before = os.fstat(stream.fileno())
        def read(offset, count):
            check()
            if offset < 0 or count < 0 or offset > size or count > size-offset:
                raise ValueError('LPIC read outside package bounds')
            stream.seek(offset)
            data = stream.read(count)
            if len(data) != count:
                raise ValueError('truncated LPIC')
            return data
        header = read(0, HEADER)
        if hashed is not None:
            hashed.update(header)
        version, header_bytes = struct.unpack_from('<HH', header, 4)
        if (header[:4] != b'LPIC' or version not in (0, 1) or header_bytes != HEADER or
            _crc(header[:508]) != struct.unpack_from('<I', header, 508)[0] or
            any(header[72 + (PACK_COUNT*12 if version else 0):508])):
            raise ValueError('invalid LPIC header identity/CRC/reserved bytes')
        flags, total, ac, pc, lc, tc, ao, po, lo, to, so, sb, mc, do = struct.unpack_from('<14I', header, 8)
        if (flags or total != size or not 1 <= ac <= MAX_ALBUMS or not 1 <= pc <= MAX_PHOTOS or
            not pc <= lc <= pc*MAX_LEVELS or not 1 <= tc <= MAX_TILES or not 1 <= sb <= MAX_STRINGS or
            struct.unpack_from('<4H', header, 64) != (THUMB, THUMB, EDGE, 1)):
            raise ValueError('invalid LPIC counts or pixel contract')
        if (ao != HEADER or po != ao+ac*ALBUM or lo != po+pc*PHOTO or to != lo+lc*LEVEL or
            so != to+tc*TILE or do != (so+sb+511)&~511 or
            (size != do+pc*8192 if version else
             size < do+pc*8192+tc or size > do+pc*8192+tc*2048)):
            raise ValueError('noncanonical LPIC section offsets')
        pack_streams = [None] * PACK_COUNT
        pack_sizes = [0] * PACK_COUNT
        pack_reports = []
        if version:
            for bucket in range(PACK_COUNT):
                identity, pack_size = struct.unpack_from('<8sI', header, 72+bucket*12)
                if not pack_size:
                    if any(identity):
                        raise ValueError('empty photo pack has an identity')
                    continue
                if not any(identity) or pack_size > PACK_LIMIT:
                    raise ValueError('invalid photo pack identity or size')
                prefix = identity.hex()
                packed_path = _pack_path(path.parent/'PACKS', prefix)
                if packed_path.stat().st_size != pack_size:
                    raise ValueError('photo pack size changed')
                packed = pack_stack.enter_context(packed_path.open('rb'))
                digest = hashlib.file_digest(packed, 'sha256').hexdigest()
                if not digest.startswith(prefix):
                    raise ValueError('photo pack content differs from catalog')
                packed.seek(0)
                pack_streams[bucket], pack_sizes[bucket] = packed, pack_size
                pack_reports.append(dict(bucket=bucket, path=str(packed_path),
                    device_path=f'/.LINNET/PHOTOS/PACKS/{prefix[:8].upper()}/{prefix[8:16].upper()}.PAK',
                    bytes=pack_size, sha256=digest))
        crc = 0
        for offset in range(HEADER, do, 1024*1024):
            block = read(offset, min(1024*1024, do-offset))
            crc = zlib.crc32(block, crc)
            if hashed is not None:
                hashed.update(block)
        if crc & 0xffffffff != mc or any(read(so+sb, do-so-sb)):
            raise ValueError('LPIC metadata CRC/alignment mismatch')
        pool = read(so, sb)
        if pool[0] != 0 or pool[-1] != 0:
            raise ValueError('LPIC string pool lacks empty sentinel')
        pool.decode('utf-8', errors='strict')
        def text(offset, count, *, source=False):
            if count > (127 if source else 191) or offset >= sb or count >= sb-offset:
                raise ValueError('LPIC string outside pool')
            if pool[offset+count] != 0 or (offset and pool[offset-1] != 0) or b'\0' in pool[offset:offset+count]:
                raise ValueError('LPIC string is not independently NUL delimited')
            value = pool[offset:offset+count].decode('ascii' if source else 'utf-8', errors='strict')
            if source:
                if (not value and offset != 0) or (value and (not FAT_PATH.fullmatch(value) or not value.endswith(('.JPG', '.JPEG')))):
                    raise ValueError('invalid LPIC fallback device path')
            else:
                if _title(value) != value:
                    raise ValueError('noncanonical LPIC title')
            return value
        def records(offset, count, length):
            # Each table retains at most 64 KiB while photo and level records
            # are consumed together, avoiding a seek for every small record.
            batch = 65536 // length
            for first in range(0, count, batch):
                block = read(offset + first*length, min(batch, count-first)*length)
                for start in range(0, len(block), length):
                    data = block[start:start+length]
                    if _crc(data[:-4]) != struct.unpack_from('<I', data, length-4)[0]:
                        raise ValueError('LPIC record CRC mismatch')
                    yield data
        intervals, count, album_titles = [], 0, []
        for index, data in enumerate(records(ao, ac, ALBUM)):
            title, length, first, photos, parent, r0, r1 = struct.unpack('<7I', data[:-4])
            if r0 or r1 or first != count or photos > pc-count or parent != (NO_INDEX if index == 0 else parent) or (index and parent >= index):
                raise ValueError('invalid LPIC album ordering/range')
            intervals.append((first, photos))
            album_titles.append(text(title, length))
            count += photos
        if count != pc:
            raise ValueError('LPIC albums do not cover the photo catalog')
        next_level = next_tile = 0
        level_records = records(lo, lc, LEVEL)
        thumb_crcs = []
        for index, data in enumerate(records(po, pc, PHOTO)):
            p = struct.unpack('<15I', data[:-4])
            album, title, length, source_offset, source_length, width, height, first, count, thumb, thumb_crc, fallback_bytes, fallback_crc, orientation, reserved = p
            if (album >= ac or not intervals[album][0] <= index < sum(intervals[album]) or reserved or
                not 1 <= width <= 16384 or not 1 <= height <= 16384 or width*height > 32_000_000 or
                first != next_level or not 1 <= count <= MAX_LEVELS or count > lc-first or thumb != do+index*8192):
                raise ValueError('invalid LPIC photo record')
            text(title, length)
            fallback = text(source_offset, source_length, source=True)
            if (not fallback and (fallback_bytes or fallback_crc or orientation)) or (fallback and (not 1 <= fallback_bytes <= 262144 or not 1 <= orientation <= 8 or max(width, height) > 2048 or width*height > 4_000_000)):
                raise ValueError('invalid LPIC fallback metadata')
            if verify_payloads:
                thumb_crcs.append(thumb_crc)
            previous = None
            for ordinal in range(count):
                photo, w, h, x, y, first_tile, tiles = struct.unpack('<I4H2I', next(level_records)[:-4])
                if (photo != index or not 1 <= w <= min(width, 2048) or not 1 <= h <= min(height, 2048) or
                    x != (w+31)//32 or y != (h+31)//32 or tiles != x*y or first_tile != next_tile or tiles > tc-first_tile or
                    (ordinal == 0 and w*h > 131072) or
                    (previous and (w < previous[0] or h < previous[1] or (w, h) == previous))):
                    raise ValueError('invalid LPIC level/tile range')
                previous = (w, h)
                next_tile += tiles
            next_level += count
        if next_level != lc or next_tile != tc:
            raise ValueError('LPIC levels do not cover tile index exactly')
        for first in range(0, len(thumb_crcs), 128):
            count = min(128, len(thumb_crcs)-first)
            payload = memoryview(read(do+first*8192, count*8192))
            if hashed is not None:
                hashed.update(payload)
            for ordinal in range(count):
                if _crc(payload[ordinal*8192:(ordinal+1)*8192]) != thumb_crcs[first+ordinal]:
                    raise ValueError('LPIC thumbnail CRC mismatch')
        # Group the index and contiguous payloads to avoid two seeks per tile.
        # Keep payload memory bounded to 1 MiB even for maximum-size libraries.
        from lz4.block import decompress as lz4_decompress
        expected = do+pc*8192
        pack_expected = [0] * PACK_COUNT
        for first in range(0, tc, 512):
            count = min(512, tc-first)
            descriptors = list(struct.iter_unpack('<III', read(to+first*TILE, count*TILE)))
            for offset, length, _ in descriptors:
                if not 1 <= length <= 2048:
                    raise ValueError('invalid LPIC tile length')
                if version:
                    bucket, local = offset >> 27, offset & (PACK_LIMIT-1)
                    if (local != pack_expected[bucket] or
                        length > pack_sizes[bucket]-local):
                        raise ValueError('noncanonical photo pack tile offset')
                    pack_expected[bucket] += length
                else:
                    if offset != expected or length > size-offset:
                        raise ValueError('noncanonical LPIC tile payload offset')
                    expected += length
            if version:
                groups = []
                start = 0
                while start < len(descriptors):
                    bucket = descriptors[start][0] >> 27
                    end = start+1
                    while end < len(descriptors) and descriptors[end][0] >> 27 == bucket:
                        end += 1
                    groups.append((bucket, descriptors[start:end]))
                    start = end
            else:
                groups = [(None, descriptors)]
            if verify_payloads:
                for bucket, group in groups:
                    base = group[0][0] & (PACK_LIMIT-1) if version else group[0][0]
                    end = (group[-1][0] & (PACK_LIMIT-1) if version else group[-1][0]) + group[-1][1]
                    if version:
                        packed = pack_streams[bucket]
                        packed.seek(base)
                        payload = memoryview(packed.read(end-base))
                        if len(payload) != end-base:
                            raise ValueError('truncated photo pack payload')
                    else:
                        payload = memoryview(read(base, end-base))
                        if hashed is not None:
                            hashed.update(payload)
                    for offset, length, crc in group:
                        local = (offset & (PACK_LIMIT-1) if version else offset)-base
                        stored = payload[local:local+length]
                        try:
                            decoded = (stored if length == 2048 else
                                       lz4_decompress(stored, uncompressed_size=2048))
                        except Exception as error:
                            raise ValueError('LPIC tile LZ4 decode failed') from error
                        if len(decoded) != 2048 or _crc(decoded) != crc:
                            raise ValueError('LPIC tile CRC mismatch')
        if (any(pack_expected[index] != pack_sizes[index] for index in range(PACK_COUNT)) if version else expected != size):
            raise ValueError('noncanonical LPIC tile payload end')
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('LPIC changed during validation')
    if hashed is not None:
        digest = hashed.hexdigest()
    else:
        with path.open('rb') as stream:
            if checkpoint is None:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            else:
                hashed = hashlib.sha256()
                while chunk := stream.read(1024*1024):
                    check()
                    hashed.update(chunk)
                digest = hashed.hexdigest()
    result = dict(format='LPIC', version=version, passed=True, photos=pc, albums=ac,
                levels=lc, tiles=tc, bytes=size, sha256=digest,
                album_titles=album_titles, payloads_verified=bool(verify_payloads))
    if version:
        result['packs'] = pack_reports
    return result
