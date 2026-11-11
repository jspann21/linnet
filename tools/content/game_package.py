"""Canonical native game packages and bounded on-device catalogues (version 1)."""
from __future__ import annotations
import re
import hashlib
import struct
import zlib
from tools.studio.targets import target_spec

ABI = LAYOUT = 1
SLOT_OFFSET, SLOT_BYTES = 0x7c0000, 0x40000
HEADER_BYTES, CATALOGUE_MAX, ROW_BYTES = 256, 128, 256
ID_PATTERN = re.compile(r'[a-z0-9-]+(?:\.[a-z0-9-]+)+', re.ASCII)
FILE_PATTERN = re.compile(r'[A-Z0-9]{8}\.LGM', re.ASCII)
BUILTIN_IDS = ('comet-run', 'pulse-path', 'solitaire', 'sudoku', 'brick', 'music-quiz',
              'vortex', 'minesweeper', 'falling-blocks', 'reversi', 'snake',
              'word-search', 'scrub', 'dial-heist', 'mixtape', 'signal',
              'clockwork-city', 'pocket-pinball')


def identity(value):
    if not isinstance(value, str) or len(value) >= 64 or not ID_PATTERN.fullmatch(value):
        raise ValueError('Game ID must be lowercase ASCII, dot-namespaced, and at most 63 bytes')
    return value


def _text(value, size):
    if not isinstance(value, str) or any(not 32 <= ord(c) <= 126 for c in value) or len(value) >= size:
        raise ValueError(f'Game text must be printable ASCII and shorter than {size} bytes')
    return value.encode('ascii').ljust(size, b'\0')


def _read_text(data):
    value, separator, padding = data.partition(b'\0')
    if not separator or any(padding) or any(not 32 <= c <= 126 for c in value):
        raise ValueError('Invalid padded game text')
    return value.decode('ascii')


def _builtin(game_id, builtin):
    if type(builtin) is not int or not (builtin == 0xffffffff or 0 <= builtin < len(BUILTIN_IDS)):
        raise ValueError('Invalid builtin game ID')
    expected = 'linnet.' + BUILTIN_IDS[builtin] if builtin != 0xffffffff else None
    if expected is not None and game_id != expected:
        raise ValueError('Built-in game identity mismatch')
    if game_id in {'linnet.' + name for name in BUILTIN_IDS} and expected != game_id:
        raise ValueError('Reserved builtin game identity')


def package(payload, *, target, game_id, title, description='', bss=0, entry=0,
            capabilities=0, builtin=0xffffffff, stack=8192, text=None, schema=1, version=1):
    spec = target_spec(target)
    identity(game_id); _builtin(game_id, builtin)
    if not spec['capabilities'].get('games',False) or spec['endianness'] != 'little':
        raise ValueError('Target does not support native games')
    text=len(payload) if text is None else text
    if (len(payload)<32 or len(payload)&31 or len(payload) > SLOT_BYTES
            or type(bss) is not int or bss&31 or not 0 <= bss <= SLOT_BYTES-len(payload)
            or type(entry) is not int or not 0 <= entry < len(payload) or entry & 3
            or type(text) is not int or text&3 or not 4 <= text <= len(payload) or entry > text-4
            or type(schema) is not int or not 0 < schema <= 0xffffffff
            or type(version) is not int or not 0 < version <= 0xffffffff
            or type(stack) is not int or not 0 < stack <= 8192
            or type(capabilities) is not int or capabilities & ~1 or not title):
        raise ValueError('Game payload, memory, entry, capabilities or stack exceeds format limits')
    header = bytearray(256)
    struct.pack_into('<16I', header, 0, 0x4d474e4c, 1, 256, spec['image_target'], ABI, LAYOUT,
                     spec['load_address']+SLOT_OFFSET, len(payload), bss, entry,
                     capabilities, builtin, zlib.crc32(payload), stack,text,schema)
    header[64:128] = _text(game_id,64)
    header[128:168] = _text(title,40)
    header[168:216] = _text(description,48)
    header[216:248] = hashlib.sha256(payload).digest()
    struct.pack_into('<I',header,248,version)
    struct.pack_into('<I',header,252,zlib.crc32(header[:252]))
    return bytes(header)+payload


def inspect(data, target=None):
    if len(data) < 288 or len(data) > HEADER_BYTES+SLOT_BYTES:
        raise ValueError('Invalid game package length')
    fields=struct.unpack_from('<16I',data)
    magic,format_version,size,number,abi,layout,load,length,bss,entry,caps,builtin,crc,stack,text,schema=fields
    version=struct.unpack_from('<I',data,248)[0]
    if (magic != 0x4d474e4c or format_version != 1 or size != 256 or abi != ABI or layout != LAYOUT
            or not version or not schema or not 4 <= text <= length or text&3 or length&31 or bss&31
            or zlib.crc32(data[:252]) != struct.unpack_from('<I',data,252)[0]
            or length != len(data)-256 or length > SLOT_BYTES or bss > SLOT_BYTES-length
            or entry > text-4 or entry & 3 or caps & ~1 or not 0 < stack <= 8192
            or zlib.crc32(data[256:]) != crc or hashlib.sha256(data[256:]).digest() != data[216:248]):
        raise ValueError('Damaged or incompatible native game package')
    game_id=identity(_read_text(data[64:128]));_builtin(game_id,builtin)
    title=_read_text(data[128:168]);description=_read_text(data[168:216])
    if not title:
        raise ValueError('Game title is empty')
    if target is not None:
        spec=target_spec(target)
        if not spec['capabilities'].get('games',False) or number != spec['image_target'] or load != spec['load_address']+SLOT_OFFSET:
            raise ValueError('Game package belongs to a different device')
    return dict(id=game_id,title=title,description=description,target=number,load=load,
                payload=length,bss=bss,entry=entry,capabilities=caps,builtin=builtin,
                crc=crc,stack=stack,text=text,schema=schema,version=version,
                payload_sha256=data[216:248].hex(),bytes=len(data))


def catalogue(rows, *, generation, target):
    spec=target_spec(target)
    if not spec['capabilities'].get('games',False) or type(generation) is not int or not 0 < generation < 0xffffffff or len(rows)>CATALOGUE_MAX:
        raise ValueError('Game catalogue exceeds generation or count limits')
    data=bytearray(struct.pack('<8I',0x43474e4c,1,generation,spec['image_target'],len(rows),ROW_BYTES,0,0))
    seen=set();names=set();slots=set()
    for row in rows:
        game_id=identity(row['id']);_builtin(game_id,row['builtin'])
        filename=row['filename'];slot=row['save_slot']
        if (game_id in seen or filename in names or not FILE_PATTERN.fullmatch(filename)
                or type(slot) is not int or not 0 <= slot < CATALOGUE_MAX or slot in slots
                or not HEADER_BYTES+32 <= row['bytes'] <= HEADER_BYTES+SLOT_BYTES or not row['title']
                or (row['bytes']-HEADER_BYTES)&31
                or row['capabilities'] & ~1 or not 0 <= row['file_crc'] <= 0xffffffff):
            raise ValueError('Invalid or duplicate game catalogue entry')
        seen.add(game_id);names.add(filename);slots.add(slot)
        record=bytearray(256)
        record[:64]=_text(game_id,64);record[64:104]=_text(row['title'],40)
        record[104:184]=_text(row['description'],80);record[184:197]=_text(filename,13)
        struct.pack_into('<5I',record,200,row['bytes'],row['file_crc'],row['builtin'],row['capabilities'],slot)
        data.extend(record)
    return bytes(data)+struct.pack('<I',zlib.crc32(data))


def read_catalogue(data, target):
    if not 36 <= len(data) <= 36+CATALOGUE_MAX*ROW_BYTES:
        raise ValueError('Invalid catalogue length')
    magic,version,generation,number,count,row_size,r0,r1=struct.unpack_from('<8I',data)
    if (magic != 0x43474e4c or version != 1 or number != target_spec(target)['image_target']
            or row_size != ROW_BYTES or r0 or r1 or len(data) != 36+count*ROW_BYTES
            or zlib.crc32(data[:-4]) != struct.unpack_from('<I',data,len(data)-4)[0]):
        raise ValueError('Damaged or incompatible game catalogue')
    rows=[]
    for offset in range(32,len(data)-4,256):
        p=data[offset:offset+256]
        if any(p[197:200]) or any(p[220:]):
            raise ValueError('Nonzero catalogue reserved bytes')
        size,crc,builtin,caps,slot=struct.unpack_from('<5I',p,200)
        rows.append(dict(id=_read_text(p[:64]),title=_read_text(p[64:104]),
                         description=_read_text(p[104:184]),filename=_read_text(p[184:197]),
                         bytes=size,file_crc=crc,builtin=builtin,capabilities=caps,save_slot=slot))
    if catalogue(rows,generation=generation,target=target) != data:
        raise ValueError('Noncanonical game catalogue')
    return dict(generation=generation,rows=rows)
