import struct

CATALOG_HEADER_SIZE = 12
CATALOG_ROW_SIZE = 416
CATALOG_MAX_BYTES = CATALOG_HEADER_SIZE + 256 * CATALOG_ROW_SIZE


def encode(entries):
    if len(entries) > 256:
        raise ValueError('video library supports up to 256 videos')
    data = bytearray(struct.pack('<4sII', b'LVIX', len(entries), CATALOG_ROW_SIZE))
    for key, path, title in entries:
        raw_path = path.encode('ascii')
        raw_title = title.encode('utf-8')
        if len(key) != 32 or b'\0' in raw_path or b'\0' in raw_title or not raw_path.startswith(b'/') or len(raw_path) > 255 or not 1 <= len(raw_title) <= 127:
            raise ValueError('video catalog path or title exceeds firmware bounds')
        data += key + raw_path.ljust(256, b'\0') + raw_title.ljust(128, b'\0')
    data = bytes(data)
    decode(data)
    return data


def _valid_path(path):
    parts = path.split('/')[1:]
    if not path.startswith('/') or not 1 <= len(parts) <= 16:
        return False
    for index, part in enumerate(parts):
        if index == 0 and part.upper() == '.LINNET':
            continue
        base, dot, extension = part.partition('.')
        if (not 1 <= len(base) <= 8 or (dot and not 1 <= len(extension) <= 3)
                or '.' in extension or any(ord(c) <= 32 or ord(c) >= 127 or c in '\"*+,:;<=>?[\\]|' for c in part)):
            return False
    return True


def decode(data):
    """Validate the bounded firmware catalog before preserving existing entries."""
    if len(data)<CATALOG_HEADER_SIZE:
        raise ValueError('invalid video catalog header')
    magic,count,size=struct.unpack_from('<4sII', data)
    if magic!=b'LVIX' or count>256 or size!=CATALOG_ROW_SIZE or len(data)!=CATALOG_HEADER_SIZE+count*size:
        raise ValueError('invalid video catalog layout')
    entries=[]
    names=set()
    keys=set()
    for at in range(CATALOG_HEADER_SIZE,len(data),size):
        key=data[at:at+32]
        raw_name=data[at+32:at+288]
        raw_title=data[at+288:at+size]
        path=raw_name.split(b'\0',1)[0].decode('ascii')
        title=raw_title.split(b'\0',1)[0].decode('utf-8')
        if not _valid_path(path) or path.casefold() in names or key in keys or not title or b'\0' not in raw_title or b'\0' not in raw_name:
            raise ValueError('invalid video catalog entry')
        names.add(path.casefold()); keys.add(key)
        entries.append((key,path,title))
    return entries


def read_file(path):
    with path.open("rb") as reader:
        data = reader.read(CATALOG_MAX_BYTES + 1)
    if len(data) > CATALOG_MAX_BYTES:
        raise ValueError("Video catalog exceeds supported size")
    return data, decode(data)
