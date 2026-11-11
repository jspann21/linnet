import hashlib
from concurrent.futures import ThreadPoolExecutor
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageOps, __version__ as pillow_version

CATALOG = 1024
ART_RECORD = 13312
NATIVE_SIDE = 130
NATIVE_HEIGHT = 174
NATIVE_RECORD = 23552
PACKED_HEADER = 512
PACKED_CAPACITY = (630 + 466) * ART_RECORD + 0x800000 - 0x38000 - (65536 + 1152) * 4
PACKED_NATIVE_SLOTS = 630 * ART_RECORD // NATIVE_RECORD + 466 * ART_RECORD // NATIVE_RECORD
PACKED_INPUT_MAX = NATIVE_RECORD + NATIVE_RECORD // 255 + 16
PACKED_INDEX_CRC = 1


def _tilted_layout(native):
    side, texture_height = (130, 174) if native else (96, 128)
    width, stride, height = (53, 54, 190) if native else (39, 39, 140)
    record = (2*stride*height*2+4+31) & ~31
    capacity = 630*ART_RECORD // ((NATIVE_RECORD if native else ART_RECORD)+record)
    return side, texture_height, width, stride, height, record, capacity


def _tilted_sources(native=True):
    """Bake both tilted views in the target renderer's native traversal order."""
    side, texture_height, width, stride, height, _, _ = _tilted_layout(native)
    screen_height = 240 if native else 176
    columns = []
    for x in range(width):
        column_height = round((96-15+(x+.5)*13/width)*side/96)
        columns.append(((side << 16)//column_height, (2*x+1)*side//(2*width),
                        24*screen_height//176+(side-column_height)//2,
                        column_height*texture_height//side))
    result = []
    for mirror in (False, True):
        coordinates = ((x,y) for y in range(height) for x in range(stride)) if native else (
                       (x,y) for x in range(stride) for y in range(height))
        for x, y in coordinates:
            if x == width:
                result.append(None)
                continue
            step, sx, top, length = columns[width-1-x if mirror else x]
            sy = ((step >> 1)+(y-top)*step) >> 16
            sx = side-1-sx if mirror else sx
            index = sy*side+sx if native else sx*texture_height+sy
            result.append(index if top <= y < top+length else None)
    return result


def _tilted_art(record, sources):
    colors = struct.unpack_from('<256H', record)
    pixels = [0 if index is None else colors[record[512+index]] for index in sources]
    pixel_bytes = len(pixels)*2
    result = bytearray((pixel_bytes+4+31) & ~31)
    struct.pack_into(f'<{len(pixels)}H', result, 0, *pixels)
    struct.pack_into('<I', result, pixel_bytes, zlib.crc32(result[:pixel_bytes]))
    return result


def _packed_art(records, cache=None):
    from lz4.block import compress
    if len(records) <= PACKED_NATIVE_SLOTS:
        return None
    chunks = []
    total = len(records) * 8
    if total > PACKED_CAPACITY:
        return None
    for record in records:
        key = hashlib.sha256(b'LCFN-lz4-1\0' + record).digest()
        saved = cache.coverflow_get(key, None) if cache is not None else None
        chunk = saved if saved is not None else compress(record, mode='high_compression', compression=12, store_size=False)
        if len(chunk) > PACKED_INPUT_MAX:
            raise ValueError('Packed artwork exceeds the decoder input bound')
        if saved is None and cache is not None:
            cache.coverflow_put(key, chunk)
        chunks.append(chunk)
        total += len(chunk)
        if total > PACKED_CAPACITY:
            return None
    at = len(chunks) * 8
    table = bytearray()
    for chunk in chunks:
        table.extend(struct.pack('<II', at, len(chunk)))
        at += len(chunk)
    header = bytearray(PACKED_HEADER)
    struct.pack_into('<4sIII', header, 0, b'LCZ1', len(chunks), NATIVE_RECORD, at)
    struct.pack_into('<I', header, 16, 1)  # LZ4 blocks, exact native tile bytes
    struct.pack_into('<I', header, 20, PACKED_INDEX_CRC)
    struct.pack_into('<I', header, 28, zlib.crc32(table))
    crc = zlib.crc32(table)
    for chunk in chunks:
        crc = zlib.crc32(chunk, crc)
    struct.pack_into('<I', header, 24, crc)
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    return bytes(header), bytes(table), chunks


def _layout(header):
    native = header[:4] == b'LCFN'
    return (NATIVE_SIDE, NATIVE_HEIGHT, NATIVE_RECORD) if native else (96, 128, ART_RECORD)


def _row_major_art(record: bytes | bytearray, side=96, height=128) -> bytearray:
    packed = bytearray(record)
    end = 512 + side * height
    packed[512:end] = Image.frombytes('P', (height, side), packed[512:end]).transpose(
        Image.Transpose.TRANSPOSE).tobytes()
    struct.pack_into('<I', packed, end, zlib.crc32(packed[:end]))
    return packed


def _original_from_rgb(image: Image.Image, side=96) -> Image.Image:
    return ImageOps.fit(image, (side, side), method=Image.Resampling.LANCZOS)


def _indexed_tile(tile: Image.Image) -> tuple[Image.Image, list[int]]:
    palette = tile.quantize(colors=256, method=Image.Quantize.MEDIANCUT, kmeans=3)
    rgb = palette.getpalette()
    rgb += [0] * (768 - len(rgb))
    colors = [((rgb[i] * 31 + 127) // 255) << 11 |
              ((rgb[i+1] * 63 + 127) // 255) << 5 |
              ((rgb[i+2] * 31 + 127) // 255) for i in range(0, 768, 3)]
    # Diffuse against the actual LCD colors, including RGB565 rounding error.
    palette.putpalette([c for p in colors for c in
                        (((p >> 11) * 255 + 15) // 31,
                         (((p >> 5) & 63) * 255 + 31) // 63,
                         ((p & 31) * 255 + 15) // 31)])
    return tile.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG), colors


def validate(path: Path, database: bytes) -> None:
    with path.open('rb') as reader:
        header = reader.read(1024)
        side, height, art_record = _layout(header)
        mapped = header[:4] == b'LCFN'
        end = 512 + side * height
        if (len(header) != 1024 or header[:4] not in (b'LCF2', b'LCFN') or header[16:48] != database[64:96] or
                struct.unpack_from('<II', header, 8) != (art_record, side) or
                struct.unpack_from('<I', header, 48)[0] != CATALOG or
                struct.unpack_from('<II', header, 56) != (24, 32) or
                (mapped and struct.unpack_from('<I', header, 68)[0] != 1) or
                zlib.crc32(header[512:]) != struct.unpack_from('<I', header, 64)[0] or
                zlib.crc32(header[:508]) != struct.unpack_from('<I', header, 508)[0]):
            raise ValueError('Invalid Cover Flow header or library identity')
        count, arts = struct.unpack_from('<I', header, 4)[0], struct.unpack_from('<I', header, 52)[0]
        raw_end = 1024 + count * CATALOG + arts * art_record
        packed_at, packed_bytes = struct.unpack_from('<II', header, 72)
        tilted_at, tilted_record = struct.unpack_from('<II', header, 80)
        *_, prepared_record, prepared_capacity = _tilted_layout(mapped)
        tilted_bytes = arts * prepared_record if tilted_at else 0
        if (count > 100000 or arts > count or
                (packed_at and (not mapped or packed_at != raw_end or packed_bytes < PACKED_HEADER + arts * 8)) or
                (not packed_at and packed_bytes) or
                (tilted_at and (packed_at or tilted_at != raw_end or
                                tilted_record != prepared_record or
                                not 0 < arts <= prepared_capacity)) or
                (not tilted_at and tilted_record) or
                path.stat().st_size != raw_end + packed_bytes + tilted_bytes or
                path.stat().st_size > 0xffffffff):
            raise ValueError('Invalid Cover Flow size')
        groups, groups_at = struct.unpack_from('<I', database, 20)[0], struct.unpack_from('<I', database, 36)[0]
        expected = [i for i in range(groups) if struct.unpack_from('<I', database, groups_at + i * 32)[0] == 2]
        if len(expected) != count:
            raise ValueError('Incomplete Cover Flow album catalog')
        for group in expected:
            record = reader.read(CATALOG)
            if (len(record) != CATALOG or struct.unpack_from('<II', record) != (group, struct.unpack_from('<I', database, groups_at + group * 32 + 16)[0]) or
                    struct.unpack_from('<I', record, 248)[0] >= arts or record[127] or record[239 if mapped else 247] or
                    zlib.crc32(record[256:], zlib.crc32(record[:252])) != struct.unpack_from('<I', record, 252)[0]):
                raise ValueError('Invalid Cover Flow catalog record')
        hashes = []
        for _ in range(arts):
            record = reader.read(art_record)
            if len(record) != art_record or zlib.crc32(record[:end]) != struct.unpack_from('<I', record, end)[0]:
                raise ValueError('Invalid Cover Flow artwork')
            if packed_at:
                hashes.append(hashlib.sha256(record).digest())
        if packed_at:
            packed_header = reader.read(PACKED_HEADER)
            payload_bytes = packed_bytes - PACKED_HEADER
            if (packed_header[:4] != b'LCZ1' or struct.unpack_from('<III', packed_header, 4) != (arts, NATIVE_RECORD, payload_bytes) or
                    struct.unpack_from('<I', packed_header, 16)[0] != 1 or zlib.crc32(packed_header[:508]) != struct.unpack_from('<I', packed_header, 508)[0]):
                raise ValueError('Invalid packed artwork header')
            table = reader.read(arts * 8)
            if len(table) != arts * 8:
                raise ValueError('Truncated packed artwork index')
            flags = struct.unpack_from('<I', packed_header, 20)[0]
            if flags != PACKED_INDEX_CRC:
                raise ValueError('Unsupported packed artwork features')
            if zlib.crc32(table) != struct.unpack_from('<I', packed_header, 28)[0]:
                raise ValueError('Invalid packed artwork index checksum')
            at, crc = len(table), zlib.crc32(table)
            for i in range(arts):
                offset, size = struct.unpack_from('<II', table, i * 8)
                if offset != at or not 0 < size <= PACKED_INPUT_MAX or size > payload_bytes - at:
                    raise ValueError('Invalid packed artwork extent')
                chunk = reader.read(size)
                from lz4.block import decompress, LZ4BlockError
                try:
                    raw = decompress(chunk, uncompressed_size=NATIVE_RECORD)
                except LZ4BlockError as error:
                    raise ValueError('Invalid packed artwork stream') from error
                if len(raw) != NATIVE_RECORD or hashlib.sha256(raw).digest() != hashes[i]:
                    raise ValueError('Packed artwork differs from its native tile')
                crc = zlib.crc32(chunk, crc)
                at += size
            if at != payload_bytes or crc != struct.unpack_from('<I', packed_header, 24)[0]:
                raise ValueError('Invalid packed artwork checksum')
        if tilted_at:
            sources = _tilted_sources(mapped)
            for art in range(arts):
                reader.seek(1024 + count*CATALOG + art*art_record)
                expected = _tilted_art(reader.read(art_record), sources)
                reader.seek(tilted_at + art*prepared_record)
                if reader.read(prepared_record) != expected:
                    raise ValueError('Prepared tilted artwork differs from its native tile')


def remove_tracks(source: Path, before: bytes, after: bytes, output: Path) -> None:
    """Remap the album catalog after deletion, reusing prepared artwork bytes."""
    from .music import validate_database
    validate_database(before)
    validate_database(after)
    validate(source, before)

    def albums(database):
        count, start = struct.unpack_from('<I', database, 20)[0], struct.unpack_from('<I', database, 36)[0]
        return {struct.unpack_from('<Q', database, start + i * 32 + 24)[0]:
                (i, struct.unpack_from('<I', database, start + i * 32 + 16)[0])
                for i in range(count) if struct.unpack_from('<I', database, start + i * 32)[0] == 2}

    old, new = albums(before), albums(after)
    if not new.keys() <= old.keys():
        raise ValueError('Cover Flow deletion introduced new albums')
    mapping = {old[key][0]: value for key, value in new.items()}
    created = False
    try:
        with source.open('rb') as reader, output.open('xb') as writer:
            created = True
            header = bytearray(reader.read(1024))
            packed_at = struct.unpack_from('<I', header, 72)[0]
            tilted = bool(struct.unpack_from('<I', header, 80)[0])
            native = header[:4] == b'LCFN'
            prepared_record = _tilted_layout(native)[5]
            _, _, art_record = _layout(header)
            count = struct.unpack_from('<I', header, 4)[0]
            records, art_ids = [], {}
            for _ in range(count):
                row = bytearray(reader.read(CATALOG))
                group = struct.unpack_from('<I', row)[0]
                if group not in mapping:
                    continue
                struct.pack_into('<II', row, 0, *mapping[group])
                art = struct.unpack_from('<I', row, 248)[0]
                art_ids.setdefault(art, len(art_ids))
                struct.pack_into('<I', row, 248, art_ids[art])
                struct.pack_into('<I', row, 252,
                                 zlib.crc32(row[256:], zlib.crc32(row[:252])))
                records.append(row)
            records.sort(key=lambda row: struct.unpack_from('<I', row)[0])
            header[16:48] = after[64:96]
            struct.pack_into('<I', header, 4, len(records))
            struct.pack_into('<I', header, 52, len(art_ids))
            artwork = []
            for art in art_ids:
                reader.seek(1024 + count * CATALOG + art * art_record)
                artwork.append(reader.read(art_record))
            packed_parts = _packed_art(artwork) if packed_at else None
            struct.pack_into('<II', header, 72, 0, 0)
            struct.pack_into('<II', header, 80, 0, 0)
            if packed_parts:
                packed_header, table, chunks = packed_parts
                struct.pack_into('<II', header, 72, 1024 + len(records)*CATALOG + len(artwork)*art_record,
                                 PACKED_HEADER + len(table) + sum(map(len, chunks)))
            if tilted and artwork:
                struct.pack_into('<II', header, 80,
                                 1024 + len(records)*CATALOG + len(artwork)*art_record, prepared_record)
            struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
            writer.write(header)
            for row in records:
                writer.write(row)
            for record in artwork:
                writer.write(record)
            if packed_parts:
                writer.write(packed_header)
                writer.write(table)
                for chunk in chunks:
                    writer.write(chunk)
            if tilted and artwork:
                sources = _tilted_sources(native)
                for record in artwork:
                    writer.write(_tilted_art(record, sources))
        validate(output, after)
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise


def prepare(database: bytes, root: Path, output: Path, *, originals: dict[int, Image.Image] | None = None,
            cache=None, bypass_cache: bool = False, row_major: bool = False, packed: bool = False) -> None:
    from .music import validate_database, TRACK
    from .publish import _bounded
    validate_database(database)
    side, height, art_record = (NATIVE_SIDE, NATIVE_HEIGHT, NATIVE_RECORD) if row_major else (96, 128, ART_RECORD)
    width = TRACK
    _, _, _, groups, _, _, tracks_at, groups_at, members_at, _, pool, _, _, art_at = struct.unpack_from('<14I', database, 8)

    def text(offset):
        start = pool + offset
        return database[start:database.index(0, start)].decode('utf-8')

    palette = Image.new('P', (1, 1))
    colors = [(r * 255 // 7, g * 255 // 7, b * 255 // 3) for r in range(8) for g in range(8) for b in range(4)]
    palette.putpalette([v for color in colors for v in color])
    reflection_shape = [(1 - ((x - (side - 1) / 2) / (side / 2)) ** 2) ** 2 for x in range(side)]
    source_ids, image_ids, artwork, previews, catalog = {}, {}, [], [], []
    catalog_rows, pending, workers = [], [], None

    def finish_tile():
        art_id, key, future = pending.pop(0)
        packed, preview = future.result()
        artwork[art_id], previews[art_id] = packed, preview
        if cache is not None:
            cache.coverflow_put(key, bytes(packed) + preview)

    try:
        for group in range(groups):
            kind, name, parent, first, count = struct.unpack_from('<5I', database, groups_at + group * 32)
            if kind != 2:
                continue
            artist = text(struct.unpack_from('<I', database, groups_at + parent * 32 + 4)[0])
            # Prefer a member with a cover when the first track has no artwork.
            art = 0xffffffff
            for member in range(first, first + count):
                track = struct.unpack_from('<I', database, members_at + member * 4)[0]
                art = struct.unpack_from('<I', database, tracks_at + track * width + 88)[0]
                if art != 0xffffffff:
                    break
            if art not in source_ids:
                cover = Image.new('RGB', (side, side), '#23374f')
                if originals is not None and art in originals:
                    cover = originals[art]
                elif art != 0xffffffff:
                    path_offset, source_side, _, _, art_bytes = struct.unpack_from('<IHHII', database, art_at + art * 32 + 8)
                    path = text(path_offset)
                    raw = _bounded(root / path.lstrip('/'), art_bytes)
                    pixels = struct.unpack(f'<{source_side * source_side}H', raw[:source_side * source_side * 2])
                    cover = Image.new('RGB', (source_side, source_side))
                    cover.putdata([((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in pixels])
                    cover = cover.resize((side, side), Image.Resampling.LANCZOS)
                else:
                    draw = ImageDraw.Draw(cover)
                    draw.ellipse((side//4, side//4, side*3//4, side*3//4), outline='#819fbc', width=2)
                    draw.ellipse((side*11//24, side*11//24, side*13//24, side*13//24), fill='#819fbc')
                if cover.size != (side, side):
                    cover = _original_from_rgb(cover, side)
                # Include the recipe and Pillow version; never reuse art by timestamp.
                key = hashlib.sha256(b'LCF-tile-2\0' + struct.pack('<II', side, height) + pillow_version.encode() + b'\0' +
                                     cover.mode.encode() + struct.pack('<II', *cover.size) + cover.tobytes()).digest()
                saved = cache.coverflow_get(key, art_record + 768) if cache is not None and not bypass_cache else None
                if key in image_ids:
                    source_ids[art] = image_ids[key]
                elif saved is not None:
                    source_ids[art] = image_ids[key] = len(artwork)
                    artwork.append(saved[:art_record])
                    previews.append(saved[art_record:])
                else:
                    source_ids[art] = image_ids[key] = len(artwork)
                    artwork.append(None)
                    previews.append(None)
                    if workers is None:
                        workers = ThreadPoolExecutor(max_workers=4)
                    pending.append((source_ids[art], key,
                                    workers.submit(_prepare_tile, cover, palette.copy(), reflection_shape, height, art_record)))
                    if len(pending) >= 4:
                        finish_tile()
            art_id = source_ids[art]
            catalog_rows.append((group, count, name, artist, art_id, art))
        while pending:
            finish_tile()
    finally:
        if workers is not None:
            workers.shutdown(wait=True, cancel_futures=True)
    for group, count, name, artist, art_id, source_art in catalog_rows:
        record = bytearray(CATALOG)
        struct.pack_into('<II', record, 0, group, count)
        for offset, value in ((8, text(name)), (128, artist)):
            maximum = 111 if row_major and offset == 128 else 119
            encoded = value.encode('utf-8')[:maximum].decode('utf-8', 'ignore').encode('utf-8')
            record[offset:offset + len(encoded)] = encoded
        if row_major and source_art != 0xffffffff:
            source = art_at + source_art * 32
            struct.pack_into('<II', record, 240, struct.unpack_from('<I', database, source)[0],
                             struct.unpack_from('<I', database, source + 24)[0])
        struct.pack_into('<I', record, 248, art_id)
        record[256:] = previews[art_id]
        struct.pack_into('<I', record, 252, zlib.crc32(record[256:], zlib.crc32(record[:252])))
        catalog.append(record)
    header = bytearray(1024)
    struct.pack_into('<4sIII', header, 0, b'LCFN' if row_major else b'LCF2', len(catalog), art_record, side)
    header[16:48] = database[64:96]
    struct.pack_into('<IIII', header, 48, CATALOG, len(artwork), 24, 32)
    if row_major:
        struct.pack_into('<I', header, 68, 1)
    struct.pack_into('<256H', header, 512, *[(r >> 3) << 11 | (g >> 2) << 5 | (b >> 3) for r, g, b in colors])
    struct.pack_into('<I', header, 64, zlib.crc32(header[512:]))
    native_records = [_row_major_art(record, side, height) if row_major else record for record in artwork]
    packed_parts = _packed_art(native_records, cache if not bypass_cache else None) if packed and row_major else None
    *_, prepared_record, prepared_capacity = _tilted_layout(row_major)
    tilted = not packed_parts and 0 < len(native_records) <= prepared_capacity
    if tilted:
        struct.pack_into('<II', header, 80, 1024 + len(catalog)*CATALOG + len(artwork)*art_record,
                         prepared_record)
    if packed_parts:
        packed_header, packed_table, chunks = packed_parts
        struct.pack_into('<II', header, 72, 1024 + len(catalog)*CATALOG + len(artwork)*art_record,
                         PACKED_HEADER + len(packed_table) + sum(map(len, chunks)))
    if (1024 + len(catalog)*CATALOG + len(artwork)*art_record + struct.unpack_from('<I', header, 76)[0] +
            (len(artwork)*prepared_record if tilted else 0)) > 0xffffffff:
        raise ValueError('Cover Flow exceeds the FAT32 file-size bound')
    struct.pack_into('<I', header, 508, zlib.crc32(header[:508]))
    temporary = output.with_suffix('.TMP')
    with temporary.open('wb') as writer:
        writer.write(header)
        for record in catalog:
            writer.write(record)
        for record in native_records:
            writer.write(record)
        if packed_parts:
            writer.write(packed_header)
            writer.write(packed_table)
            for chunk in chunks:
                writer.write(chunk)
        if tilted:
            sources = _tilted_sources(row_major)
            for record in native_records:
                writer.write(_tilted_art(record, sources))
    validate(temporary, database)
    temporary.replace(output)


def _prepare_tile(cover, palette, reflection_shape, height=128, art_record=ART_RECORD):
    side = cover.width
    cover = cover.filter(ImageFilter.UnsharpMask(radius=.55, percent=35, threshold=3))
    accent = cover.resize((1, 1), Image.Resampling.BOX).getpixel((0, 0))
    mean = sum(accent) / 3
    accent = tuple(max(0, min(255, round(mean + (c - mean) * 1.4))) for c in accent)
    tile = Image.new('RGB', (side, height))
    tile.paste(cover)
    gap = max(2, round(side / 48))
    reflection_height = height - side - gap
    reflection = ImageOps.flip(cover).crop((0, 0, side, reflection_height))
    reflection_accent = [c * .18 for c in accent]
    for y in range(reflection_height):
        fade = (1 - y / reflection_height) ** 2
        row = reflection.crop((0, y, side, y + 1))
        row = Image.blend(Image.new('RGB', row.size), row, .48 * fade * (1 + .8 * y / reflection_height))
        row.putdata([tuple(min(255, round(c + reflection_accent[channel] * reflection_shape[x] * fade))
                     for channel, c in enumerate(pixel))
                     for x, pixel in enumerate(row.getdata())])
        tile.paste(row, (0, side + gap + y))
    indexed, rgb565 = _indexed_tile(tile)
    packed = bytearray(art_record)
    struct.pack_into('<256H', packed, 0, *rgb565)
    end = 512 + side * height
    packed[512:end] = indexed.transpose(Image.Transpose.TRANSPOSE).tobytes()
    struct.pack_into('<I', packed, end, zlib.crc32(packed[:end]))
    preview = tile.resize((24, 32), Image.Resampling.LANCZOS).quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    return packed, preview.transpose(Image.Transpose.TRANSPOSE).tobytes()
