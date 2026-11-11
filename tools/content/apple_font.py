"""Optional, locally extracted Podium Sans; no Apple glyphs are bundled here.

The pinned Apple Photo 1.2.1 image owns all offsets below. Resource field
layout is documented by iPodWizard Font.h/Font.cpp:
https://github.com/DavidBuchanan314/iPodWizard-mirror
"""
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import uuid
import zipfile
import zlib

from .host_state import storage_root

URL = ('https://secure-appldnld.apple.com/iPod/SBML/osx/bundles/'
       '061-2693.20060912.PdwCD/iPod_5.1.2.1.ipsw')
LICENSE_URL = 'https://www.apple.com/legal/sla/docs/ipod.pdf'
ARCHIVE_BYTES = 3831893
ARCHIVE_SHA256 = '03643928fd4b5d180f92396382680c48e745826b8b170bf2e298bef2bbe26464'
FIRMWARE_SHA256 = '9321189b846a7317f4f575075696056e9a18c79644886a00055a402259c6fadc'
MAGIC = b'LNFONT1\0'
WIDTH, HEIGHT, ADVANCE, FIRST, COUNT = 15, 17, 14, 32, 95
PIXELS = WIDTH * HEIGHT * COUNT
ALPHA_BYTES = 7678
GLYPHS_AT, ALPHA_AT = 128, 888
BYTES = ALPHA_AT + ALPHA_BYTES + 4
DEVICE_NAME = 'PODIUM.FNT'
TARGETS = ('photo4g-p98', 'video5g-m25', 'classic6g-n25')
PACKAGE_DIRECTORIES = ('config', 'targets', 'hal', 'services', 'ui', 'assets', 'themes', 'sdk/assets')


def check_packaged_assets(root):
    """Reject known Apple inputs/outputs anywhere inside recursive app assets."""
    root = Path(root)
    names = {DEVICE_NAME.casefold(), 'font-podium-sans16.json', 'ipod_5.1.2.1.ipsw'}
    for directory in PACKAGE_DIRECTORIES:
        for path in (root / directory).rglob('*'):
            if path.is_symlink() or getattr(path, 'is_junction', lambda: False)():
                raise ValueError('Package assets must be ordinary files and directories: ' + str(path.relative_to(root)))
            if not path.is_file():
                continue
            forbidden = path.name.casefold() in names
            size = path.stat().st_size
            if not forbidden and size in (BYTES, ARCHIVE_BYTES, 5387540):
                data = path.read_bytes()
                forbidden = data.startswith(MAGIC) or hashlib.sha256(data).hexdigest() in (ARCHIVE_SHA256, FIRMWARE_SHA256)
            if not forbidden and path.suffix.casefold() == '.json' and size < 2 * 1024 * 1024:
                try:
                    value = json.loads(path.read_text(encoding='utf-8'))
                    forbidden = isinstance(value, dict) and value.get('format') == 'FONT' and value.get('source_sha256') == FIRMWARE_SHA256
                except (ValueError, UnicodeError):
                    pass
            if forbidden:
                raise ValueError('Keep locally extracted Apple font data outside package assets: ' + str(path.relative_to(root)))


def extract(archive):
    """Accept only the reviewed Apple archive and reproduce the original glyphs."""
    if len(archive) != ARCHIVE_BYTES or hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
        raise ValueError('Apple font download checksum mismatch')
    with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
        entry = bundle.getinfo('Firmware-5.4.2.1')
        if entry.file_size != 6516224:
            raise ValueError('Apple font firmware size mismatch')
        firmware = bundle.read(entry)
    directory = struct.unpack_from('<I', firmware, 0x104)[0] + 512
    matches = []
    for offset in range(directory, directory + 512 - 39, 40):
        if firmware[offset:offset + 8] == b'!ATAsoso':
            start, size = struct.unpack_from('<II', firmware, offset + 12)
            payload = firmware[start + 512:start + 512 + size]
            if hashlib.sha256(payload).hexdigest() == FIRMWARE_SHA256:
                matches.append(payload)
    if len(matches) != 1:
        raise ValueError('Apple font firmware payload mismatch')
    source = matches[0]
    # Fixed offsets are safe only after verifying the complete original image.
    header1, header2 = 0x46783c, 0x467c94
    count = struct.unpack_from('<H', source, header2 + 74)[0]
    mapping = header2 + 100
    metrics = header1 + ((mapping + count * 2 - header1 + 3) // 4) * 4
    map_end, metrics_end = struct.unpack_from('<II', source, header2 + 92)
    bitmap = metrics + metrics_end - map_end
    _, height, _, stride, depth, _, _ = struct.unpack_from('<I4H2I', source, bitmap)
    if height != HEIGHT or depth != 4:
        raise ValueError('Apple font bitmap format mismatch')
    advances, pixels = bytearray(), bytearray()
    for char in range(COUNT):
        index = struct.unpack_from('<H', source, mapping + char * 2)[0]
        left, right, advance, bearing = struct.unpack_from('<HHhh', source, metrics + index * 8)
        if not 1 <= advance <= ADVANCE or not 0 <= left <= right <= stride * 2:
            raise ValueError('Apple font glyph metrics mismatch')
        advances.append(advance)
        cell = bytearray(WIDTH * HEIGHT)
        for y in range(HEIGHT):
            for x in range(right - left):
                destination = x + bearing + 1
                if not 0 <= destination < WIDTH:
                    raise ValueError('Apple font glyph exceeds its cell')
                column = left + x
                value = source[bitmap + 20 + y * stride + column // 2]
                cell[y * WIDTH + destination] = ((value >> (4 * (1 - column % 2))) & 15) * 17
        pixels.extend(cell)
    from .compiler import crop_font
    font = dict(width=WIDTH, height=HEIGHT, count=COUNT, alpha=list(pixels))
    alpha, glyphs = crop_font(font)
    if len(alpha) != ALPHA_BYTES:
        raise ValueError('Apple font packed geometry mismatch')
    data = (struct.pack('<8s6I', MAGIC, WIDTH, HEIGHT, ADVANCE, FIRST, COUNT, ALPHA_BYTES)
            + advances + b'\0' + b''.join(struct.pack('<I4B', *glyph) for glyph in glyphs) + bytes(alpha))
    return data + struct.pack('<I', zlib.crc32(data))


def decode(data):
    if (len(data) != BYTES or data[:32] != struct.pack('<8s6I', MAGIC, WIDTH, HEIGHT, ADVANCE, FIRST, COUNT, ALPHA_BYTES)
            or zlib.crc32(data[:-4]) != struct.unpack_from('<I', data, len(data) - 4)[0]
            or data[127] or any(not 1 <= n <= ADVANCE for n in data[32:32 + COUNT])):
        raise ValueError('Optional Apple font file is damaged or unsupported')
    pixels = bytearray(PIXELS)
    for index in range(COUNT):
        offset, x, y, width, height = struct.unpack_from('<I4B', data, GLYPHS_AT + index * 8)
        if x + width > WIDTH or y + height > HEIGHT or offset + width * height > ALPHA_BYTES:
            raise ValueError('Optional Apple font glyph exceeds its bounds')
        for row in range(height):
            start = index * WIDTH * HEIGHT + (y + row) * WIDTH + x
            pixels[start:start + width] = data[ALPHA_AT + offset + row * width:ALPHA_AT + offset + (row + 1) * width]
    return dict(format='FONT', width=WIDTH, height=HEIGHT, advance=ADVANCE,
                first=FIRST, count=COUNT, advances=list(data[32:32 + COUNT]),
                alpha=list(pixels), source_sha256=FIRMWARE_SHA256)


def _checkpoint(cancelled):
    if cancelled():
        from .media_manager import Cancelled
        raise Cancelled()


def download(progress=lambda _message: None, cancelled=lambda: False):
    """User-requested download, verified before extraction; local cache only."""
    folder = storage_root() / 'Fonts'
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / 'iPod_5.1.2.1.ipsw'
    _checkpoint(cancelled)
    data = archive.read_bytes() if archive.is_file() and archive.stat().st_size == ARCHIVE_BYTES else b''
    if hashlib.sha256(data).hexdigest() != ARCHIVE_SHA256:
        import urllib.request
        progress('Downloading Apple iPod Photo firmware (3.8 MB)…')
        chunks, total = [], 0
        with urllib.request.urlopen(URL, timeout=20) as response:
            if response.url != URL:
                raise ValueError('Apple font download redirected to an unexpected address')
            while chunk := response.read(65536):
                _checkpoint(cancelled)
                total += len(chunk)
                if total > ARCHIVE_BYTES:
                    raise ValueError('Apple font download exceeds its expected size')
                chunks.append(chunk)
        data = b''.join(chunks)
    _checkpoint(cancelled)
    progress('Verifying Apple firmware and extracting Podium Sans…')
    font = extract(data)
    decode(font)
    # Atomic replacement also permits independent Media Manager instances.
    for destination, content in ((archive, data), (folder / DEVICE_NAME, font)):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=folder, delete=False) as writer:
                temporary = Path(writer.name)
                writer.write(content); writer.flush(); os.fsync(writer.fileno())
            temporary.replace(destination)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return font


def supported(identity):
    return bool(identity and identity.get('target_id') in TARGETS)


def _device_path(identity):
    from .guard import guard_volume, guarded_path
    if not supported(identity):
        raise ValueError('The Apple color font is unavailable on this iPod model')
    root = Path(identity['root']).resolve(strict=True)
    guard_volume(identity)
    system = root / '.LINNET'
    guarded_path(system, root)
    if not system.is_dir():
        raise ValueError('Install Linnet firmware before choosing an iPod font')
    path = system / DEVICE_NAME
    guarded_path(path, root)
    return root, path


def install(identity, progress=lambda _message: None, cancelled=lambda: False):
    """Publish only the optional font; no Apple firmware is installed."""
    from .guard import guard_volume, guarded_path
    root, path = _device_path(identity)
    data = download(progress, cancelled)
    _checkpoint(cancelled)
    from .publish import _exclusive
    with _exclusive(identity):
        _, path = _device_path(identity)
        temporary = path.with_name(uuid.uuid4().hex[:8].upper() + '.TMP')
        owned = False
        guard_volume(identity); guarded_path(temporary, root)
        try:
            with temporary.open('xb') as writer:
                owned = True
                writer.write(data); writer.flush(); os.fsync(writer.fileno())
            if temporary.read_bytes() != data:
                raise OSError('Apple font readback failed')
            _checkpoint(cancelled)
            guard_volume(identity); guarded_path(path, root)
            temporary.replace(path)
            if path.read_bytes() != data:
                raise OSError('Installed Apple font readback failed')
        finally:
            if owned:
                guard_volume(identity); guarded_path(temporary, root)
                temporary.unlink(missing_ok=True)
    return dict(summary='Apple font installed. Safely eject and restart Linnet to use it in supported themes.')


def remove(identity):
    from .guard import guard_volume
    from .publish import _exclusive
    with _exclusive(identity):
        _, path = _device_path(identity)
        if path.exists():
            if not path.is_file():
                raise ValueError('Optional Apple font path is not an ordinary file')
            guard_volume(identity)
            path.unlink()
    return dict(summary='Included font selected. Safely eject and restart Linnet to apply it.')
