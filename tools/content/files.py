"""Host-prepared Files directory pages and native-size previews.

The device reads one 4 KiB page, validates its records, and opens the selected
short name relative to its live parent handle. It never sorts or recursively
indexes the disk. Original files are not renamed or rewritten.
"""
from collections import OrderedDict, deque
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
import hashlib
import json
import os
import stat
import struct
import threading
import time
import traceback
import unicodedata
import uuid
import zlib
from types import SimpleNamespace

from .publish import _plain, _hash
from .host_state import cache_volume_scope, file_stamp as _preview_source_stamp, save_json

RECORD = 256
MAX_RECORDS = 1_000_000
MAX_FILE = 512 + MAX_RECORDS * RECORD
PACK_BYTES = 4 * 1024 * 1024
IMAGES = {'.jpg', '.jpeg', '.png', '.bmp', '.gif', '.tif', '.tiff', '.webp', '.heic', '.avif'}
TEXT = {'.txt', '.text', '.md', '.rst', '.log', '.ini', '.cfg', '.toml', '.yaml', '.yml',
        '.json', '.xml', '.csv', '.nfo', '.c', '.h', '.py', '.js', '.ts', '.tsx', '.css', '.html', '.htm'}
PREVIEW_DECODE_PIXELS = 32_000_000
MAX_TEXT_SOURCE_BYTES = 16 * 1024 * 1024
_TEXT_CONTROL_SPACES = dict.fromkeys((*range(32), 127), ' ')
_NEGATIVE_TEXT_PREVIEWS = OrderedDict()
_NEGATIVE_TEXT_PREVIEWS_LOCK = threading.Lock()
_NEGATIVE_TEXT_PREVIEW_LIMIT = 128
_NEGATIVE_TEXT_PREVIEW_SECONDS = 300


def _forget_negative_text_preview(key, cached):
    with _NEGATIVE_TEXT_PREVIEWS_LOCK:
        if _NEGATIVE_TEXT_PREVIEWS.get(key) == cached:
            del _NEGATIVE_TEXT_PREVIEWS[key]


def _negative_text_preview_hit(source, root, info, key, checkpoint):
    with _NEGATIVE_TEXT_PREVIEWS_LOCK:
        cached = _NEGATIVE_TEXT_PREVIEWS.get(key)
        if cached is None:
            return False
        if time.monotonic() >= cached[2]:
            del _NEGATIVE_TEXT_PREVIEWS[key]
            return False
    checkpoint()
    _plain(source, root)
    before = source.stat()
    if ((before.st_size, before.st_mtime_ns) != (info.st_size, info.st_mtime_ns) or
            _preview_source_stamp(before) != cached[0]):
        _forget_negative_text_preview(key, cached)
        return False
    # FAT timestamps can repeat after a same-size edit. Verify source bytes
    # before reusing a decision that no native text preview can be made.
    try:
        digest = _hash(source, before.st_size)
    except (OSError, ValueError):
        _forget_negative_text_preview(key, cached)
        return False
    checkpoint()
    if _preview_source_stamp(source.stat()) != cached[0] or digest != cached[1]:
        _forget_negative_text_preview(key, cached)
        return False
    with _NEGATIVE_TEXT_PREVIEWS_LOCK:
        if _NEGATIVE_TEXT_PREVIEWS.get(key) == cached:
            _NEGATIVE_TEXT_PREVIEWS.move_to_end(key)
    return True


def _remember_negative_text_preview(source, info, key, raw, before):
    try:
        after = source.stat()
    except OSError:
        return
    if ((after.st_size, after.st_mtime_ns) != (info.st_size, info.st_mtime_ns) or
            _preview_source_stamp(after) != _preview_source_stamp(before)):
        return
    value = (_preview_source_stamp(after), hashlib.sha256(raw).hexdigest(),
             time.monotonic() + _NEGATIVE_TEXT_PREVIEW_SECONDS)
    with _NEGATIVE_TEXT_PREVIEWS_LOCK:
        _NEGATIVE_TEXT_PREVIEWS[key] = value
        _NEGATIVE_TEXT_PREVIEWS.move_to_end(key)
        if len(_NEGATIVE_TEXT_PREVIEWS) > _NEGATIVE_TEXT_PREVIEW_LIMIT:
            _NEGATIVE_TEXT_PREVIEWS.popitem(last=False)


class _PreviewDecodeBudget:
    def __init__(self):
        self.available = PREVIEW_DECODE_PIXELS
        self.condition = threading.Condition()

    @contextmanager
    def reserve(self, pixels, checkpoint):
        # An oversized image retains the sequential path's behavior, while
        # smaller images share a fixed aggregate decode budget.
        amount = min(max(pixels, 1), PREVIEW_DECODE_PIXELS)
        while True:
            checkpoint()
            with self.condition:
                if self.available >= amount:
                    self.available -= amount
                    break
                self.condition.wait(timeout=0.05)
        try:
            yield
        finally:
            with self.condition:
                self.available += amount
                self.condition.notify_all()


def _image_preview_pixels(original, width, height):
    from PIL import Image, ImageOps
    from .photos import _rgb565

    # JPEG decoders can discard high-resolution DCT detail before allocating
    # pixels that will be downsampled for this small native preview.
    side = 2 * max(width, height)
    original.draft('RGB', (side, side))
    oriented = ImageOps.exif_transpose(original)
    try:
        image = oriented.convert('RGB')
    finally:
        oriented.close()
    try:
        image.thumbnail((width, height), Image.Resampling.LANCZOS)
        canvas = Image.new('RGB', (width, height))
        try:
            canvas.paste(image, ((width - image.width) // 2, (height - image.height) // 2))
            return _rgb565(canvas)
        finally:
            canvas.close()
    finally:
        image.close()


def _lazy_image_header(header):
    # Pillow selects decoders by content, so a renamed WebP must not bypass
    # admission just because its extension says JPEG.
    return (header.startswith(b'\xff\xd8\xff') or
            header.startswith(b'\x89PNG\r\n\x1a\n') or header.startswith(b'BM'))


def _close_preview_original(original):
    try:
        original.close()
    finally:
        # Pillow's WebP decoder retains native canvases after Image.close().
        # A Future may retain an exception traceback containing this object.
        original.__dict__.pop('_decoder', None)


def _image_preview(source, width, height, image_budget, checkpoint):
    from PIL import Image

    if image_budget is None:
        image_budget = _PreviewDecodeBudget()
    with source.open('rb') as reader:
        header = reader.read(12)
        reader.seek(0)
        if not _lazy_image_header(header):
            # Some plugins allocate full frames in Image.open, before dimensions
            # are available to the caller. Reserve the entire budget first.
            with image_budget.reserve(PREVIEW_DECODE_PIXELS, checkpoint):
                try:
                    original = Image.open(reader)
                    try:
                        return _image_preview_pixels(original, width, height)
                    finally:
                        try:
                            _close_preview_original(original)
                        finally:
                            original = None
                except Exception as error:
                    # A failed Pillow open/decode can leave native buffers in its
                    # completed traceback frames, which Futures retain.
                    traceback.clear_frames(error.__traceback__)
                    raise
        original = Image.open(reader)
        try:
            with image_budget.reserve(original.width * original.height, checkpoint):
                try:
                    try:
                        return _image_preview_pixels(original, width, height)
                    finally:
                        try:
                            _close_preview_original(original)
                        finally:
                            original = None
                except Exception as error:
                    traceback.clear_frames(error.__traceback__)
                    raise
        finally:
            if original is not None:
                _close_preview_original(original)


def _short_name(path):
    if os.name == 'nt':
        from .music import _actual_short_component
        return _actual_short_component(_windows_path(path)).upper()
    return path.name.upper()


def _windows_path(path):
    name = os.path.abspath(path)
    if name.startswith('\\\\?\\'):
        return name
    return '\\\\?\\UNC\\' + name[2:] if name.startswith('\\\\') else '\\\\?\\' + name


def _directory_entries(folder):
    """Keep the short alias and metadata Windows already returns per entry."""
    if os.name != 'nt':
        with os.scandir(folder) as iterator:
            for entry in iterator:
                if entry.is_symlink():
                    continue
                info = entry.stat(follow_symlinks=False)
                if stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode):
                    yield entry.name, stat.S_ISDIR(info.st_mode), info, None
        return
    import ctypes
    from ctypes import wintypes
    from .music import _FindData, _find_kernel
    next_file = _find_kernel.FindNextFileW
    next_file.argtypes = (wintypes.HANDLE, ctypes.POINTER(_FindData))
    next_file.restype = wintypes.BOOL
    data = _FindData()
    handle = _find_kernel.FindFirstFileW(_windows_path(folder / '*'), ctypes.byref(data))
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        if error == 2:  # Empty directory.
            return
        raise ctypes.WinError(error)
    try:
        while True:
            if data.name not in ('.', '..') and not data.attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                stamp = (data.write.dwHighDateTime << 32) | data.write.dwLowDateTime
                info = SimpleNamespace(st_size=(data.size_high << 32) | data.size_low,
                                       st_mtime_ns=(stamp - 116444736000000000) * 100)
                yield data.name, bool(data.attributes & 16), info, data.alternate or data.name
            if not next_file(handle, ctypes.byref(data)):
                error = ctypes.get_last_error()
                if error != 18:  # ERROR_NO_MORE_FILES
                    raise ctypes.WinError(error)
                break
    finally:
        if not _find_kernel.FindClose(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def _label(value, limit):
    # Linnet's fonts are ASCII; normalize on the host once, not each UI frame.
    from .notes import _ascii
    value = _ascii(value)
    return value.translate(_TEXT_CONTROL_SPACES).encode('ascii')[:limit]


def _alias(value):
    value = value.upper()
    parts = value.split('.')
    if (len(parts) > 2 or not 1 <= len(parts[0]) <= 8 or
            (len(parts) == 2 and not 1 <= len(parts[1]) <= 3) or
            any(ord(c) < 33 or ord(c) > 126 or c in '/\\:*?"<>|+,;=[]' for c in value)):
        return b''
    return value.encode('ascii')


def _cache_bytes(output, data):
    temporary = output.with_name(uuid.uuid4().hex + '.tmp')
    owned = False
    try:
        with temporary.open('xb') as writer:
            owned = True
            writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        os.replace(temporary, output)
    finally:
        if owned:
            temporary.unlink(missing_ok=True)


def _preview(source, cache, width, height, columns, recipe, checkpoint, info, root, compressed,
             image_budget=None, cache_scope=None):
    suffix = source.suffix.lower()
    if suffix not in IMAGES | TEXT or (suffix in IMAGES and not width):
        return None
    checkpoint()
    current = source.stat()
    if (current.st_size, current.st_mtime_ns) != (info.st_size, info.st_mtime_ns):
        raise ValueError(f'File changed while preparing its preview: {source}')
    # Windows directory enumeration supplies size and write time, but not the
    # file ID needed to distinguish a replacement with identical timestamps.
    info = current
    # Cache-key work uses file metadata; unchanged images/text are not decoded
    # again during each preparation. Output hashes still guard publication.
    format_key = 'lfim' if compressed else 'fimg'
    source_key = f'{cache_scope}:{source.relative_to(root).as_posix()}' if cache_scope else str(source)
    source_stamp = _preview_source_stamp(info)
    key = hashlib.sha256(f'files-{format_key}:{source_key}:{source_stamp}:{width}:{height}:{columns}:{recipe[2]}'.encode()).hexdigest()
    output = cache / (key + ('.FIM' if suffix in IMAGES else '.TXT'))
    if not output.exists():
        negative_key = (os.path.normcase(os.path.abspath(source)),
                        os.path.normcase(os.path.abspath(output)))
        if suffix in TEXT and _negative_text_preview_hit(source, root, info, negative_key, checkpoint):
            return None
        checkpoint()
        # Enumeration rejects links; recheck the complete path immediately
        # before reading source bytes. Cache hits only read the local cache.
        _plain(source, root)
        if suffix in IMAGES:
            payload = _image_preview(source, width, height, image_budget, checkpoint)
            header = bytearray(512)
            if compressed:
                from lz4.block import compress as lz4_compress

                blocks = []
                lengths = []
                for at in range(0, len(payload), 2048):
                    checkpoint()
                    raw = payload[at:at + 2048].ljust(2048, b'\0')
                    packed = lz4_compress(raw, mode='high_compression', compression=9,
                                          store_size=False)
                    stored = packed if len(packed) < len(raw) else raw
                    blocks.append(stored)
                    lengths.append(len(stored))
                if len(lengths) > (508 - 24) // 2:
                    raise ValueError('Files image preview exceeds its block table')
                size = 512 + sum(lengths)
                struct.pack_into('<4s5I', header, 0, b'LFIM', size, width, height,
                                 zlib.crc32(payload), len(lengths))
                struct.pack_into(f'<{len(lengths)}H', header, 24, *lengths)
                data = header + b''.join(blocks)
            else:
                struct.pack_into('<4s4I', header, 0, b'FIMG', 1, width, height,
                                 zlib.crc32(payload))
                data = header + payload
            struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
            data[:512] = header
        else:
            if info.st_size > MAX_TEXT_SOURCE_BYTES:
                return None  # Native bounded byte/text paging remains available.
            before = info
            with source.open('rb') as reader:
                raw = reader.read(MAX_TEXT_SOURCE_BYTES + 1)
            if len(raw) > MAX_TEXT_SOURCE_BYTES:
                raise ValueError(f'File changed while preparing its preview: {source}')
            encoding = 'utf-16' if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else 'utf-8-sig'
            try:
                text = raw.decode(encoding)
            except UnicodeError:
                text = raw.decode('cp1252', errors='replace')
            if '\0' in text:
                _remember_negative_text_preview(source, info, negative_key, raw, before)
                return None
            from .notes import _ascii, _display_lines
            text = _ascii(text)
            lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
            if len(text) > 4 * 1024 * 1024 and columns > 0:
                # Every non-space character needs a display cell, and each
                # paragraph needs at least one row. Reject provably oversized
                # previews before measuring font widths and wrapping them.
                minimum_rows = 0
                next_checkpoint = time.monotonic() + .05
                for line in lines:
                    now = time.monotonic()
                    if now >= next_checkpoint:
                        checkpoint()
                        next_checkpoint = now + .05
                    clean = line.expandtabs(4).translate(_TEXT_CONTROL_SPACES)
                    nonspaces = len(clean) - clean.count(' ')
                    minimum_rows += max(1, (nonspaces + columns - 1) // columns)
                    if minimum_rows * columns > 4 * 1024 * 1024:
                        _remember_negative_text_preview(source, info, negative_key, raw, before)
                        return None
            # One record per visual line; whole pages start on line boundaries.
            data = bytearray()
            next_checkpoint = time.monotonic() + .05
            for line in lines:
                now = time.monotonic()
                if now >= next_checkpoint:
                    checkpoint()
                    next_checkpoint = now + .05
                clean = line.expandtabs(4).translate(_TEXT_CONTROL_SPACES)
                for wrapped in _display_lines(clean, recipe[0], recipe[1], columns):
                    now = time.monotonic()
                    if now >= next_checkpoint:
                        checkpoint()
                        next_checkpoint = now + .05
                    if len(data) + columns > 4 * 1024 * 1024:
                        _remember_negative_text_preview(source, info, negative_key, raw, before)
                        return None
                    data.extend(wrapped.encode('ascii').ljust(columns, b' '))
        checkpoint()
        after = source.stat()
        if _preview_source_stamp(after) != source_stamp:
            raise ValueError(f'File changed while preparing its preview: {source}')
        _cache_bytes(output, data)
    digest = _hash(output)
    relative = f'.LINNET/FILES/PREVIEW/{digest[:2].upper()}/{digest[2:10].upper()}{output.suffix}'
    return dict(source=str(output), path=relative, bytes=output.stat().st_size,
                sha256=digest, kind=1 if suffix in IMAGES else 2)


def _prepared_videos(root, warnings):
    """Resolve the existing host conversions once, including AVI/MKV sources."""
    from .video_catalog import read_file
    catalog = root / '.LINNET' / 'VIDEOS' / 'INDEX.VID'
    if not catalog.exists():
        return {}
    result = {}
    try:
        _plain(catalog, root)
        _, entries = read_file(catalog)
        for key, path, _ in entries:
            if len(path.encode('ascii')) >= 76:
                continue
            prepared = root / path.lstrip('/')
            _plain(prepared, root)
            if prepared.is_file():
                result[key] = dict(path=path, bytes=prepared.stat().st_size, kind=3)
    except (OSError, ValueError) as error:
        warnings.append(dict(path='.LINNET/VIDEOS/INDEX.VID', message=str(error)))
    return result


def _pack_previews(previews, cache, checkpoint):
    """Reuse sealed packs; adding one preview never rewrites existing packs."""
    cache = Path(cache) / 'packs'
    cache.mkdir(parents=True, exist_ok=True)
    manifest = cache / 'layout.json'
    packs, locations, layout = [], {}, []

    def add_pack(digest, size, members):
        path = f'.LINNET/FILES/PACKS/{digest[:8].upper()}/{digest[8:16].upper()}.FPK'
        packs.append(dict(source=str(cache / (digest + '.FPK')), path=path, bytes=size, sha256=digest))
        layout.append(dict(sha256=digest, members=members))
        for key, offset, length in members:
            locations[key] = (path, offset)

    try:
        previous = json.loads(manifest.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = []
    # This is only a host cache hint. Verify both the pack and each requested
    # slice before trusting offsets, and rebuild any missing/corrupt cache data.
    for item in previous if isinstance(previous, list) else []:
        checkpoint()
        if not isinstance(item, dict):
            continue
        digest, members = item.get('sha256'), item.get('members')
        if (not isinstance(digest, str) or len(digest) != 64 or
                any(c not in '0123456789abcdef' for c in digest) or not isinstance(members, list)):
            continue
        wanted = [m for m in members if isinstance(m, list) and len(m) == 3 and
                  isinstance(m[0], str) and m[0] in previews and m[0] not in locations and
                  type(m[1]) is int and type(m[2]) is int and m[1] >= 0 and not m[1] % 512 and
                  m[2] == previews[m[0]]['bytes'] and m[2] >= 0]
        if not wanted:
            continue
        try:
            with (cache / (digest + '.FPK')).open('rb') as reader:
                data = reader.read(PACK_BYTES + 1)
        except OSError:
            continue
        if len(data) > PACK_BYTES or hashlib.sha256(data).hexdigest() != digest:
            continue
        valid = [m for m in wanted if m[1] <= len(data) and m[2] <= len(data) - m[1] and
                 hashlib.sha256(data[m[1]:m[1]+m[2]]).hexdigest() == m[0]]
        if valid:
            add_pack(digest, len(data), valid)

    missing = sorted((key, item) for key, item in previews.items() if key not in locations)
    for bucket in '0123456789abcdef':
        payload, members = bytearray(), []

        def finish():
            if not members:
                return
            checkpoint()
            digest = hashlib.sha256(payload).hexdigest()
            output = cache / (digest + '.FPK')
            try:
                valid = _hash(output, len(payload)) == digest
            except (OSError, ValueError):
                valid = False
            if not valid:
                _cache_bytes(output, payload)
            add_pack(digest, len(payload), list(members))
            payload.clear()
            members.clear()

        for key, item in missing:
            if key[0] != bucket:
                continue
            checkpoint()
            size = item['bytes']
            if not 0 <= size <= PACK_BYTES:
                raise ValueError('Files preview exceeds pack limit')
            padded = (size + 511) & ~511
            if len(payload) + padded > PACK_BYTES:
                finish()
            with Path(item['source']).open('rb') as reader:
                data = reader.read(size + 1)
            if len(data) != size or hashlib.sha256(data).hexdigest() != key:
                raise ValueError('Files preview changed while packing')
            members.append((key, len(payload), size))
            payload.extend(data)
            payload.extend(bytes(padded - size))
        finish()
    save_json(manifest, layout)
    return packs, locations


def compile_files(root, package, cache, *, width=0, height=0, text_columns=24, target='photo4g-p98', checkpoint=lambda: None,
                  progress=lambda message: None, short_name=_short_name):
    if text_columns not in (20, 24):
        raise ValueError('Files text width must match a native layout')
    root, package, cache = Path(root).resolve(), Path(package).resolve(), Path(cache).resolve()
    if package.is_relative_to(root) or cache.is_relative_to(root):
        raise ValueError('Files index and preview cache must be staged on the computer')
    cache.mkdir(parents=True, exist_ok=True)
    cache_scope = cache_volume_scope(root)
    from .music import EXTENSIONS
    from .notes import _display_recipe
    recipe = _display_recipe(target, 'linnet.system.files')
    records = [bytearray(RECORD)]
    records[0][32:37] = b'Files'
    records[0][160:164] = b'ROOT'
    struct.pack_into('<I', records[0], 0, 16)
    folders = deque([(root, 0)])
    previews, warnings, preview_rows = {}, [], []
    image_budget = _PreviewDecodeBudget()
    videos = _prepared_videos(root, warnings)
    pending_previews = deque()

    def finish_preview():
        source, row, future = pending_previews.popleft()
        try:
            preview = future.result()
        except (OSError, ValueError) as error:
            warnings.append(dict(path=source.relative_to(root).as_posix(), message=str(error)))
            return
        if preview:
            previews[preview['sha256']] = preview
            preview_rows.append((row, preview['sha256']))
            struct.pack_into('<II', row, 16, preview['kind'], preview['bytes'])

    with ThreadPoolExecutor(max_workers=8, thread_name_prefix='files-preview') as preview_pool:
        while folders:
            folder, parent = folders.popleft()
            checkpoint()
            # Generated cache/backup contents remain accessible in Live folder;
            # indexing the index itself would grow a new snapshot on every run.
            if folder == root / '.LINNET' / 'FILES':
                struct.pack_into('<I', records[parent], 24, 2)
                continue
            _plain(folder, root)
            entries = []
            try:
                for entry in _directory_entries(folder):
                    checkpoint()
                    entries.append(entry)
            except PermissionError as error:
                warnings.append(dict(path=folder.relative_to(root).as_posix(), message=str(error)))
                struct.pack_into('<I', records[parent], 24, 2)
                continue
            entries.sort(key=lambda e: (not e[1], unicodedata.normalize('NFKC', e[0]).casefold(), e[0]))
            if len(records) + len(entries) > MAX_RECORDS:
                raise ValueError('Files index exceeds one million entries; use Live folder')
            struct.pack_into('<II', records[parent], 8, len(records), len(entries))
            for start in range(0, len(entries), 16):
                batch = entries[start:start + 16]
                futures = {}
                for index, (name, directory, info, _) in enumerate(batch):
                    source = folder / name
                    if (not directory and source.suffix.lower() in IMAGES | TEXT and
                            not source.is_relative_to(root / '.LINNET')):
                        futures[index] = preview_pool.submit(
                            _preview, source, cache, width, height, text_columns, recipe,
                            checkpoint, info, root, target != 'photo4g-p98', image_budget, cache_scope)
                for index, (name, directory, info, alternate) in enumerate(batch):
                    checkpoint()
                    source = folder / name
                    row = bytearray(RECORD)
                    struct.pack_into('<II', row, 0, 16 if directory else 0, 0 if directory else info.st_size)
                    native = 1 if source.suffix.lower() in EXTENSIONS else 2 if source.suffix.lower() in {'.mp4', '.m4v', '.mov', '.ipvf'} else 3 if source.suffix.lower() in {'.jpg', '.jpeg'} else 0
                    struct.pack_into('<I', row, 28, native)
                    label = _label(name, 127)
                    row[32:32+len(label)] = label
                    try:
                        alias = _alias(alternate if alternate is not None and short_name is _short_name else short_name(source))
                    except PermissionError:
                        alias = b''
                    # An unrepresentable short alias is shown, then resolved through
                    # the live folder reader instead of pointing at another file.
                    row[160:160+len(alias)] = alias
                    if not alias:
                        row[160:165] = b'LIVE!'
                        struct.pack_into('<I', row, 24, 1)
                    if directory:
                        folders.append((source, len(records)))
                    else:
                        preview = None
                        if videos:
                            key = hashlib.sha256(source.relative_to(root).as_posix().encode('utf-8')).digest()
                            preview = videos.get(key)
                        if not preview and index in futures:
                            pending_previews.append((source, row, futures[index]))
                            if len(pending_previews) >= 64:
                                finish_preview()
                        if preview:
                            if preview['kind'] != 3:
                                previews[preview['sha256']] = preview
                                preview_rows.append((row, preview['sha256']))
                            struct.pack_into('<II', row, 16, preview['kind'], preview['bytes'])
                            path = ('/' + preview['path'].lstrip('/')).encode('ascii')
                            assert len(path) < 76
                            row[176:176+len(path)] = path
                    records.append(row)
            progress(f'Indexing Files: {len(records)-1:,} entries')
        while pending_previews:
            checkpoint()
            finish_preview()
    progress('Packing Files previews')
    packs, locations = _pack_previews(previews, cache, checkpoint)
    for row, key in preview_rows:
        path, offset = locations[key]
        struct.pack_into('<H', row, 174, offset // 512)
        row[176:252] = bytes(76)
        encoded = ('/' + path).encode('ascii')
        assert len(encoded) < 76
        row[176:176+len(encoded)] = encoded
    header = bytearray(512)
    struct.pack_into('<4s3I', header, 0, b'FILS', 2, len(records), RECORD)
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    package.parent.mkdir(parents=True, exist_ok=True)
    with package.open('wb') as writer:
        writer.write(header)
        for row in records:
            struct.pack_into('<I', row, 252, zlib.crc32(row[:252]))
            writer.write(row)
        writer.flush()
        os.fsync(writer.fileno())
    return dict(bytes=package.stat().st_size, sha256=_hash(package), entries=len(records)-1,
                previews=packs, preview_count=len(previews), warnings=warnings)


def publish_files(package, candidate, *, disk_number, expected_disk_serial,
                  expected_volume_serial, expected_drive=None):
    from .publish import (inspect_target, _exclusive, _volume_guard, _copy_verified,
                          _publish_library, _same_target, _write_receipt)
    package = Path(package).resolve(strict=True)
    if package.stat().st_size != candidate['bytes'] or _hash(package) != candidate['sha256']:
        raise ValueError('Files package changed before publication')
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    root = Path(identity['root'])
    if package.drive.casefold() == root.drive.casefold():
        raise ValueError('Files package must be staged on a different volume from the iPod')
    with _exclusive(identity):
        started_at = time.time()
        guard = lambda: _volume_guard(identity)
        active = root / '.LINNET' / 'FILES' / 'INDEX.FIL'
        guard()
        _plain(active, root)
        previous_hint = None
        if active.is_file():
            info = active.stat()
            if info.st_size == candidate['bytes'] and candidate['bytes'] <= MAX_FILE:
                digest = _hash(active, info.st_size)
                info = active.stat()
                previous_hint = dict(bytes=info.st_size, sha256=digest,
                                     stamp=(info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino))
        unchanged = previous_hint is not None and previous_hint['sha256'] == candidate['sha256']
        guard()
        needed = 0 if unchanged else candidate['bytes'] * 2 + 1024 * 1024
        missing_preview = False
        added = 0
        for item in candidate['previews']:
            destination = root / item['path']
            _plain(destination, root)
            if not destination.exists():
                missing_preview = True
                needed += item['bytes'] + 65536
        if unchanged and missing_preview:
            needed += 1024 * 1024
        if needed > identity['free_bytes']:
            raise ValueError('Not enough space for Files previews and index; previous index retained')
        for item in candidate['previews']:
            guard()
            destination = root / item['path']
            _plain(destination, root)
            destination.parent.mkdir(parents=True, exist_ok=True)
            _plain(destination, root)
            added += _copy_verified(Path(item['source']), destination, item, root, guard)
        if unchanged:
            guard()
            _plain(package, Path(package.anchor))
            if _hash(package, candidate['bytes']) != candidate['sha256']:
                raise ValueError('Files package changed before reuse')
            _same_target(identity, inspect_target(disk_number, expected_disk_serial,
                                                   expected_volume_serial, expected_drive))
            guard()
            _plain(active, root)
            if _hash(active, candidate['bytes']) != candidate['sha256']:
                raise RuntimeError('Active Files index changed during publication')
            guard()
            from .host_state import receipt_directory
            receipt_path = receipt_directory(identity) / ('publish-files-' + uuid.uuid4().hex + '.json')
            receipt = dict(operation='publish-files', passed=True, phase='reused',
                           identity=identity, candidate_sha256=candidate['sha256'],
                           candidate_bytes=candidate['bytes'], package=str(active),
                           receipt=str(receipt_path), reused_index=True,
                           previews_added=added, entries=candidate['entries'],
                           previews=candidate.get('preview_count', len(candidate['previews'])),
                           preview_files=len(candidate['previews']), started_at=started_at,
                           finished_at=time.time(), elevation=False,
                           durability='Existing index and preview objects verified by complete hashes; index not rewritten')
            _write_receipt(receipt_path, receipt)
            return receipt
        return _publish_library(package, candidate, identity, root, disk_number,
            expected_disk_serial, expected_volume_serial, expected_drive,
            kind='files', label='Files index', filename='INDEX.FIL', max_file=MAX_FILE,
            previous_hint=previous_hint,
            summary=dict(entries=candidate['entries'], previews=candidate.get('preview_count', len(candidate['previews'])),
                         preview_files=len(candidate['previews'])))
