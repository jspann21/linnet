import struct
import zlib


def prepare(database: bytes) -> bytes:
    """Compile exact artist/album relations from the validated PODB membership lists."""
    from .music import validate_database
    validate_database(database)
    groups, start, members = (struct.unpack_from('<I', database, offset)[0] for offset in (20, 36, 40))
    rows = [struct.unpack_from('<6IQ', database, start + i * 32) for i in range(groups)]
    track_albums = {}
    owners = set()
    owned = {}
    for album, row in enumerate(rows):
        if row[0] != 2:
            continue
        owners.add(row[2])
        owned.setdefault(row[2], set()).add(album)
        for (track,) in struct.iter_unpack('<I', database[members + row[3]*4:members + (row[3]+row[4])*4]):
            track_albums.setdefault(track, set()).add(album)
    records, links = [], []
    for artist, row in enumerate(rows):
        if row[0] != 1:
            continue
        albums = set(owned.get(artist, ()))
        for (track,) in struct.iter_unpack('<I', database[members + row[3]*4:members + (row[3]+row[4])*4]):
            albums.update(track_albums.get(track, ()))
        records.append(struct.pack('<III', artist, len(links), len(albums)))
        links.extend(sorted(albums))
    ranges = []
    for kind in range(1, 9):
        indices = [i for i, row in enumerate(rows) if row[0] == kind]
        if indices and indices != list(range(indices[0], indices[0] + len(indices))):
            raise ValueError('Browse groups are not contiguous by kind')
        ranges.extend((indices[0] if indices else 0, len(indices)))
    payload = struct.pack('<16I', *ranges) + b''.join(records) + struct.pack(f'<{len(links)}I', *links) + struct.pack(f'<{len(owners)}I', *sorted(owners))
    searches, search_at, pool_at = (struct.unpack_from('<I', database, offset)[0] for offset in (28, 44, 48))
    prefixes, fences = [], []
    previous = -1
    for ordinal in range(searches):
        text = pool_at + struct.unpack_from('<I', database, search_at + ordinal * 8 + 4)[0]
        key = database[text] << 8
        if database[text]:
            key |= database[text + 1]
        if key != previous:
            prefixes.append((key, ordinal))
            previous = key
        if not ordinal % 64:
            end = database.index(0, text)
            fences.append(struct.pack('<I32s', ordinal, database[text:min(end, text + 32)]))
    prefixes.append((65536, searches))
    payload += struct.pack('<I', len(prefixes))
    payload += b''.join(struct.pack('<II', *entry) for entry in prefixes)
    payload += struct.pack('<I', len(fences)) + b''.join(fences)
    tracks, track_at = (struct.unpack_from('<I', database, offset)[0] for offset in (16, 32))
    identities = sorted((struct.unpack_from('<Q', database, track_at + i * 132)[0], i)
                        for i in range(tracks))
    id_index = b''.join(struct.pack('<QI', identity, ordinal) for identity, ordinal in identities)
    header = bytearray(64)
    struct.pack_into('<4s5I', header, 0, b'LBRI', 64 + len(payload) + 4 + len(id_index),
                     len(records), len(links), len(owners), groups)
    header[24:56] = database[64:96]
    struct.pack_into('<I', header, 56, zlib.crc32(payload))
    struct.pack_into('<I', header, 60, zlib.crc32(header[:60]))
    return bytes(header) + payload + struct.pack('<I', zlib.crc32(id_index)) + id_index
