"""Host-prepared native photo grids, bound to the exact LPIC header."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import struct
import tempfile
import zlib

from .photos import _crc, _title, FAT_PATH, MAX_FILE, MAX_PHOTOS, MAX_ALBUMS, MAX_STRINGS

GRID_SHAPES = {'classic6g-n25': (1238, 56, 56), 'video5g-m25': (25, 50, 38),
               'photo4g-p98': (98, 40, 30)}
DEVICE_PATH = '/.LINNET/PHOTOS/GRID.PGR'


def _pim_header(header):
    if len(header) != 512:
        raise ValueError('invalid grid LPIC binding header')
    version, header_bytes = struct.unpack_from('<HH', header, 4)
    if (header[:4] != b'LPIC' or
            version not in (0, 1) or header_bytes != 512 or
            any(header[72 + (384 if version else 0):508]) or
            _crc(header[:508]) != struct.unpack_from('<I', header, 508)[0]):
        raise ValueError('invalid grid LPIC binding header')
    flags, size, ac, pc, lc, tc, ao, po, lo, to, so, sb, mc, do = struct.unpack_from('<14I', header, 8)
    if (flags or not 1 <= ac <= MAX_ALBUMS or not 1 <= pc <= MAX_PHOTOS or
            not pc <= lc <= pc*6 or not 1 <= tc <= 1_000_000 or not 1 <= sb <= MAX_STRINGS or
            struct.unpack_from('<4H', header, 64) != (64, 64, 32, 1) or
            ao != 512 or po != ao+ac*32 or lo != po+pc*64 or to != lo+lc*24 or
            so != to+tc*12 or do != (so+sb+511)&~511 or
            (size != do+pc*8192 if version else
             size < do+pc*8192+tc or size > do+pc*8192+tc*2048) or size > MAX_FILE):
        raise ValueError('invalid grid LPIC binding ranges')
    return dict(size=size, albums=ac, photos=pc, levels=lc, tiles=tc, photo_at=po,
                level_at=lo, tile_at=to, string_at=so, string_bytes=sb, metadata_crc=mc, data_at=do)


def _pim_metadata(pim, h):
    before = os.fstat(pim.fileno())
    pim.seek(512)
    metadata = pim.read(h['data_at']-512)
    after = os.fstat(pim.fileno())
    if (len(metadata) != h['data_at']-512 or _crc(metadata) != h['metadata_crc'] or
            (before.st_size,before.st_mtime_ns,before.st_ctime_ns) !=
            (after.st_size,after.st_mtime_ns,after.st_ctime_ns)):
        raise ValueError('LPIC metadata changed or failed CRC')
    return metadata


def _photo_checksums(metadata, h):
    prefix = metadata[:h['level_at']-512]
    descriptors = metadata[h['level_at']-512:h['string_at']-512]
    tile_base = h['tile_at']-h['level_at']
    checksums = bytearray()
    for index in range(h['photos']):
        first, count = struct.unpack_from('<II', prefix, h['photo_at']-512+index*64+28)
        if not count or first+count > h['levels']:
            raise ValueError('invalid LPIC photo levels')
        levels = descriptors[first*24:(first+count)*24]
        tile_first = struct.unpack_from('<I', levels, 12)[0]
        tile_end = tile_first
        for offset in range(0, len(levels), 24):
            photo, _, _, _, _, tile_at, tiles = struct.unpack_from('<I4H2I', levels, offset)
            if photo != index or tile_at != tile_end or not tiles or tiles > h['tiles']-tile_at:
                raise ValueError('invalid LPIC photo tile range')
            tile_end += tiles
        tiles = descriptors[tile_base+tile_first*12:tile_base+tile_end*12]
        if len(tiles) != (tile_end-tile_first)*12:
            raise ValueError('truncated LPIC photo tiles')
        checksums += struct.pack('<I', zlib.crc32(tiles, zlib.crc32(levels)))
    return bytes(checksums)


def _catalog(prefix, strings, h):
    pool = strings[:h['string_bytes']]
    if not pool or pool[0] or pool[-1] or any(strings[len(pool):]):
        raise ValueError('invalid grid string pool or padding')
    for raw in pool.split(b'\0'):
        if raw:
            value = raw.decode('utf-8', errors='strict')
            if _title(value) != value:
                raise ValueError('invalid grid text')

    def text(at, n, path=False):
        if (n > (127 if path else 191) or at >= len(pool) or n >= len(pool)-at or
                pool[at+n] or (at and pool[at-1]) or b'\0' in pool[at:at+n]):
            raise ValueError('invalid grid text reference')
        value = pool[at:at+n].decode('ascii' if path else 'utf-8', errors='strict')
        if path:
            if (not value and at != 0) or (value and (not FAT_PATH.fullmatch(value) or not value.endswith(('.JPG', '.JPEG')))):
                raise ValueError('invalid grid JPEG reference')
        elif not value or _title(value) != value:
            raise ValueError('invalid grid title')
    intervals, count = [], 0
    for index in range(h['albums']):
        record = prefix[index*32:(index+1)*32]
        title, n, first, photos, parent, r0, r1, crc = struct.unpack('<8I', record)
        if (_crc(record[:28]) != crc or r0 or r1 or first != count or photos > h['photos']-count or
                (parent != 0xffffffff if not index else parent >= index)):
            raise ValueError('invalid grid album record')
        text(title, n);intervals.append((first, first+photos));count += photos
    if count != h['photos']:
        raise ValueError('grid album coverage mismatch')
    base, next_level = h['albums']*32, 0
    for index in range(h['photos']):
        record = prefix[base+index*64:base+(index+1)*64]
        album, title, n, source, sn, w, height, first, levels, thumb, thumb_crc, jb, jc, orient, reserved, crc = struct.unpack('<16I', record)
        if (_crc(record[:60]) != crc or reserved or album >= h['albums'] or
                not intervals[album][0] <= index < intervals[album][1] or
                not 1 <= w <= 16384 or not 1 <= height <= 16384 or w*height > 32_000_000 or
                first != next_level or not 1 <= levels <= 6 or levels > h['levels']-first or
                thumb != h['data_at']+index*8192 or
                (not sn and (source or jb or jc or orient)) or
                (sn and (not 1 <= jb <= 262144 or not 1 <= orient <= 8 or max(w,height) > 2048 or w*height > 4_000_000))):
            raise ValueError('invalid grid photo record')
        text(title, n);text(source, sn, True);next_level += levels
    if next_level != h['levels']:
        raise ValueError('grid level coverage mismatch')


def inspect_grid(path, *, package=None, target=None, checkpoint=None):
    check = checkpoint or (lambda: None)
    path = Path(path);size = path.stat().st_size
    if not 1024 <= size <= MAX_FILE:
        raise ValueError('invalid native grid file size')
    hashed = hashlib.sha256()
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        header = stream.read(1024)
        own, binding = header[:512], header[512:]
        if len(header) != 1024:
            raise ValueError('invalid native grid header')
        version, header_bytes = struct.unpack_from('<HH', own, 4)
        if (own[:4] != b'LPGR' or
                version != 2 or header_bytes != 1024 or any(own[44:508]) or
                _crc(own[:508]) != struct.unpack_from('<I', own, 508)[0]):
            raise ValueError('invalid native grid header')
        total, model, w, height, count, prefix_bytes, string_bytes, data_at, crc, pim_size = struct.unpack_from('<IIHH6I', own, 8)
        shape = next((name for name, values in GRID_SHAPES.items() if values == (model,w,height)), None)
        h = _pim_header(binding)
        stride = w*height*2+4
        checksums_bytes = count*4
        if (shape is None or (target is not None and shape != target) or total != size or count != h['photos'] or
                pim_size != h['size'] or prefix_bytes != h['level_at']-512 or
                string_bytes != h['data_at']-h['string_at'] or
                data_at != (1024+prefix_bytes+string_bytes+checksums_bytes+511)&~511 or size != data_at+count*stride):
            raise ValueError('invalid native grid contract/ranges')
        if package is not None:
            package = Path(package)
            with package.open('rb') as pim:
                if os.fstat(pim.fileno()).st_size != pim_size or pim.read(512) != binding:
                    raise ValueError('native grid belongs to a different LPIC')
        check();metadata = stream.read(data_at-1024)
        metadata_end = prefix_bytes+string_bytes+checksums_bytes
        if len(metadata) != data_at-1024 or _crc(metadata) != crc or any(metadata[metadata_end:]):
            raise ValueError('native grid catalog CRC/padding mismatch')
        _catalog(metadata[:prefix_bytes], metadata[prefix_bytes:prefix_bytes+string_bytes], h)
        if package is not None:
            with package.open('rb') as pim:
                if pim.read(512) != binding:
                    raise ValueError('native grid LPIC changed')
                original = _pim_metadata(pim, h)
                expected = (original[:prefix_bytes]+original[h['string_at']-512:]+
                            _photo_checksums(original, h))
                if metadata[:metadata_end] != expected:
                    raise ValueError('native grid catalog differs from LPIC')
        hashed.update(header);hashed.update(metadata)
        for first in range(0,count,128):
            check();block = stream.read(min(128,count-first)*stride)
            if len(block) != min(128,count-first)*stride:
                raise ValueError('truncated native grid pixels')
            hashed.update(block)
            for offset in range(0,len(block),stride):
                if _crc(block[offset:offset+stride-4]) != struct.unpack_from('<I',block,offset+stride-4)[0]:
                    raise ValueError('native grid thumbnail CRC mismatch')
        after = os.fstat(stream.fileno())
        if (before.st_size,before.st_mtime_ns,before.st_ctime_ns) != (after.st_size,after.st_mtime_ns,after.st_ctime_ns):
            raise ValueError('native grid changed during validation')
    return dict(format='LPGR',version=version,passed=True,target=shape,photos=count,bytes=size,sha256=hashed.hexdigest(),
                width=w,height=height,metadata_bytes=metadata_end,
                output=str(path),device_path=DEVICE_PATH)


def write_grid(package, output, target, pixels, checkpoint):
    package, output = Path(package), Path(output)
    model,w,height = GRID_SHAPES[target]
    with package.open('rb') as pim:
        binding = pim.read(512);h = _pim_header(binding)
        if os.fstat(pim.fileno()).st_size != h['size']:
            raise ValueError('LPIC size changed before native grid preparation')
        original = _pim_metadata(pim,h)
        prefix = original[:h['level_at']-512]
        checksums = _photo_checksums(original,h)
        strings = original[h['string_at']-512:]
    metadata = prefix+strings+checksums
    data_at = (1024+len(metadata)+511)&~511
    metadata += bytes(data_at-1024-len(metadata))
    total = data_at+h['photos']*(w*height*2+4)
    header = bytearray(512)
    struct.pack_into('<4sHHIIHH6I',header,0,b'LPGR',2,1024,total,model,w,height,h['photos'],
                     len(prefix),len(strings),data_at,_crc(metadata),h['size'])
    struct.pack_into('<I',header,508,_crc(header[:508]))
    output.parent.mkdir(parents=True,exist_ok=True)
    fd,name = tempfile.mkstemp(prefix=output.name+'.',suffix='.tmp',dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd,'wb') as stream:
            stream.write(header);stream.write(binding);stream.write(metadata);pixels.seek(0)
            while chunk := pixels.read(1024*1024):
                checkpoint();stream.write(chunk)
            stream.flush();os.fsync(stream.fileno())
        report = inspect_grid(temporary,package=package,target=target,checkpoint=checkpoint)
        checkpoint();temporary.replace(output);report['output'] = str(output)
        return report
    finally:
        temporary.unlink(missing_ok=True)
