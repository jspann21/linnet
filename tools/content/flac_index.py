"""Add native FLAC seek points to an iPod copy without changing audio frames."""
from bisect import bisect_right
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import time
import uuid

from .host_state import file_stamp

NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
MAX_METADATA = 16 * 1024 * 1024
MAX_PACKET_INDEX = 64 * 1024 * 1024
MAX_SEEK_TABLE = 1024 * 1024
PLACEHOLDER = b'\xff' * 8 + b'\0' * 10


def _layout(path):
    size = path.stat().st_size
    with path.open('rb') as reader:
        if reader.read(4) != b'fLaC':
            return None
        at, first, seek = 4, True, False
        while at + 4 <= size and at < MAX_METADATA:
            reader.seek(at)
            header = reader.read(4)
            if len(header) != 4:
                return None
            kind, length = header[0] & 127, int.from_bytes(header[1:], 'big')
            if at + 4 + length > size:
                return None
            if first:
                if kind != 0 or length != 34:
                    return None
                stream = reader.read(34)
                if len(stream) != 34:
                    return None
                rate = int.from_bytes(stream[10:13], 'big') >> 4
                total = ((stream[13] & 15) << 32) | int.from_bytes(stream[14:18], 'big')
                if not rate or not total:
                    return None
                first = False
            elif kind == 0:
                return None
            seek |= kind == 3
            last = at
            at += 4 + length
            if header[0] & 128:
                return (at, last, rate, total, seek) if at < size and at <= MAX_METADATA else None
    return None


def _rebalance(path, layout):
    """Spread an overlong native table across the stream without shifting audio."""
    audio_at, _, rate, total, _ = layout
    table = None
    with path.open('rb') as reader:
        at = 4
        while at < audio_at:
            reader.seek(at)
            header = reader.read(4)
            if len(header) != 4:
                return None
            length = int.from_bytes(header[1:], 'big')
            if header[0] & 127 == 3:
                if table is not None or length > MAX_SEEK_TABLE or length % 18:
                    return None
                data = reader.read(length)
                if len(data) != length:
                    return None
                table = (at + 4, data)
            at += 4 + length
    if table is None or len(table[1]) < 65 * 18:
        return None
    points = []
    previous_sample, previous_offset = -1, 0
    placeholders = False
    audio_bytes = path.stat().st_size - audio_at
    for at in range(0, len(table[1]), 18):
        raw = table[1][at:at + 18]
        sample, offset, frames = struct.unpack('>QQH', raw)
        if sample == 0xffffffffffffffff:
            placeholders = True
            continue
        if (placeholders or sample <= previous_sample or sample >= total or
                offset < previous_offset or offset >= audio_bytes or not frames):
            return None
        points.append((sample, raw))
        previous_sample, previous_offset = sample, offset
    if len(points) <= 64 or total - points[63][0] < rate * 30:
        return None
    samples = [sample for sample, _ in points]
    selected = {max(0, bisect_right(samples, i * total // 64) - 1)
                for i in range(63)} | {len(points) - 1}
    if len(selected) < 64:
        remaining = [i for i in range(len(points)) if i not in selected]
        needed = 64 - len(selected)
        selected.update(remaining[(i + 1) * len(remaining) // (needed + 1)]
                        for i in range(needed))
    indices = sorted(selected)
    replacement = b''.join(points[i][1] for i in indices)
    replacement += PLACEHOLDER * (len(table[1]) // 18 - len(indices))
    return (table[0], table[1], replacement) if replacement != table[1] else None


def same_original(source, destination, *, checkpoint=lambda: None):
    """Recognize this metadata-only augmentation during repeat imports."""
    source, destination = Path(source), Path(destination)
    checkpoint()
    paths = (source, destination)
    before = [path.stat() for path in paths]
    if any(not stat.S_ISREG(info.st_mode) for info in before):
        raise ValueError('file changed while comparing existing FLAC')
    original, prepared = _layout(source), _layout(destination)
    if not original or not prepared or not prepared[4]:
        return False
    audio_at, last_header, rate, total, _ = original
    if prepared[2:4] != (rate, total):
        return False
    if original[4]:
        change = _rebalance(source, original)
        if (not change or prepared[0] != audio_at or
                before[0].st_size != before[1].st_size):
            return False
    else:
        if prepared[1] != audio_at:
            return False
        added = prepared[0] - audio_at
        if (added < 22 or added > 4 + 64 * 18 or
                before[0].st_size + added != before[1].st_size):
            return False
    equal = True
    with source.open('rb') as left, destination.open('rb') as right:
        checkpoint()
        metadata = bytearray(left.read(audio_at))
        if len(metadata) != audio_at:
            return False
        if original[4]:
            offset, original_table, prepared_table = change
            if metadata[offset:offset + len(original_table)] != original_table:
                return False
            metadata[offset:offset + len(original_table)] = prepared_table
        else:
            if not metadata[last_header] & 128:
                return False
            metadata[last_header] &= 127
        if metadata != right.read(audio_at):
            return False
        if not original[4]:
            block = right.read(added)
            if (len(block) != added or block[0] != 0x83 or
                    int.from_bytes(block[1:4], 'big') != added - 4 or (added - 4) % 18):
                return False
        remaining = before[0].st_size - audio_at
        while remaining:
            checkpoint()
            size = min(remaining, 1024 * 1024)
            a, b = left.read(size), right.read(size)
            if len(a) != size or len(b) != size:
                raise ValueError('file changed while comparing existing FLAC')
            if a != b:
                equal = False
                break
            remaining -= size
        if equal and (left.read(1) or right.read(1)):
            raise ValueError('file changed while comparing existing FLAC')
    if any(file_stamp(info) != file_stamp(path.stat()) for path, info in zip(paths, before)):
        raise ValueError('file changed while comparing existing FLAC')
    return equal


def _points(path, probe, audio_at, rate, total, checkpoint):
    packet_path = path.with_name('packets.json')
    with packet_path.open('wb') as output:
        child = subprocess.Popen([str(probe), '-v', 'error', '-select_streams', 'a:0',
            '-show_entries', 'stream=sample_rate,time_base,duration_ts',
            '-show_entries', 'packet=pts,duration,pos,size', '-of', 'json', str(path)],
            stdout=output, stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)
        started = time.monotonic()
        oversized = False
        try:
            while child.poll() is None:
                checkpoint()
                if packet_path.stat().st_size > MAX_PACKET_INDEX:
                    oversized = True
                    break
                remaining = 600 - (time.monotonic() - started)
                if remaining <= 0:
                    raise TimeoutError('FLAC seek preparation exceeded ten minutes')
                try:
                    child.wait(timeout=min(.05, remaining))
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if child.poll() is None:
                child.terminate(); child.wait(timeout=10)
    if oversized or child.returncode or packet_path.stat().st_size > MAX_PACKET_INDEX:
        return None
    try:
        with packet_path.open('rb') as reader:
            result = json.load(reader)
        streams = result['streams']
        packets = result['packets']
        if len(streams) != 1 or len(packets) < 2 or len(packets) > 1_000_000:
            return None
        stream = streams[0]
        if (int(stream['sample_rate']) != rate or stream['time_base'] != f'1/{rate}'
                or int(stream['duration_ts']) != total):
            return None
        rows = [(int(row['pts']), int(row['pos']), int(row['size']), int(row['duration']))
                for row in packets]
    except (KeyError, ValueError, TypeError, json.JSONDecodeError):
        return None
    if rows[0][0] != 0 or rows[0][1] != audio_at:
        return None
    for left, right in zip(rows, rows[1:]):
        if (left[0] + left[3] != right[0] or left[1] + left[2] != right[1]
                or left[2] <= 0 or not 1 <= left[3] <= 65535):
            return None
    if (rows[-1][0] + rows[-1][3] != total or rows[-1][1] + rows[-1][2] != path.stat().st_size
            or rows[-1][2] <= 0 or not 1 <= rows[-1][3] <= 65535):
        return None
    starts = [row[0] for row in rows]
    indices = sorted({max(0, bisect_right(starts, i * total // 64) - 1) for i in range(64)})
    return b''.join(struct.pack('>QQH', rows[i][0], rows[i][1] - audio_at, rows[i][3])
                    for i in indices)


def prepare_file(path, cache, *, ffprobe, checkpoint=lambda: None, recovery=None, journal=None):
    """Atomically index an iPod FLAC copy; retain tags and compressed audio."""
    from .guard import hash_file
    path, cache = Path(path), Path(cache)
    checkpoint()
    before = path.stat()
    layout = _layout(path)
    if not layout or layout[3] < layout[2] * 30:
        return False
    audio_at, last_header, rate, total, has_table = layout
    change = _rebalance(path, layout) if has_table else None
    if has_table and change is None:
        return False
    if not has_table and not ffprobe:
        return None  # Retry when the host gains FFprobe.
    key = hashlib.sha256(f'{path}:{file_stamp(before)}'.encode()).hexdigest()
    cache.mkdir(parents=True, exist_ok=True)
    unsupported = cache / (key + '.unsupported')
    if not has_table and unsupported.exists():
        return False
    with tempfile.TemporaryDirectory(prefix='flac-', dir=cache) as scratch:
        local = Path(scratch) / 'source.flac'
        source_hash = hashlib.sha256()
        with path.open('rb') as reader, local.open('wb') as writer:
            while chunk := reader.read(1024 * 1024):
                checkpoint(); writer.write(chunk); source_hash.update(chunk)
            if recovery is not None:
                writer.flush(); os.fsync(writer.fileno())
        if file_stamp(before) != file_stamp(path.stat()):
            raise OSError('FLAC source changed during preparation')
        if has_table:
            block = b''
        else:
            points = _points(local, ffprobe, audio_at, rate, total, checkpoint)
            if not points:
                unsupported.touch(); return False
            block = bytes((0x83,)) + len(points).to_bytes(3, 'big') + points
        if before.st_size + len(block) > 0xffffffff:
            return False
        temporary = path.with_name('L' + uuid.uuid4().hex[:7] + '.TMP')
        owned = False
        try:
            digest = hashlib.sha256()
            with temporary.open('xb') as writer, local.open('rb') as reader:
                owned = True
                if has_table:
                    offset, prior, replacement = change
                    prefix = reader.read(offset)
                    if len(prefix) != offset or reader.read(len(prior)) != prior:
                        raise OSError('FLAC seek table changed during preparation')
                    for chunk in (prefix, replacement):
                        writer.write(chunk); digest.update(chunk)
                else:
                    metadata = bytearray(reader.read(audio_at))
                    if len(metadata) != audio_at or not metadata[last_header] & 128:
                        raise OSError('FLAC metadata changed during preparation')
                    metadata[last_header] &= 127
                    for chunk in (metadata, block):
                        writer.write(chunk); digest.update(chunk)
                while chunk := reader.read(1024 * 1024):
                    checkpoint(); writer.write(chunk); digest.update(chunk)
                writer.flush(); os.fsync(writer.fileno())
            checkpoint()
            if hash_file(temporary, before.st_size + len(block)) != digest.hexdigest():
                raise OSError('FLAC prepared copy did not verify')
            prepared = _layout(temporary)
            if (not prepared or prepared[0] != audio_at + len(block) or
                    prepared[3] != total or not prepared[4]):
                raise OSError('FLAC seek table has invalid geometry')
            after = path.stat()
            if file_stamp(before) != file_stamp(after):
                raise OSError('FLAC source changed during preparation')
            if recovery is not None:
                backup = cache / (uuid.uuid4().hex + '.flac-original')
                recovery.append(dict(kind='FLAC', path=str(path), backup=str(backup),
                    source_copy=str(local), temporary=str(temporary),
                    original_bytes=before.st_size, original_sha256=source_hash.hexdigest(),
                    atime_ns=before.st_atime_ns, mtime_ns=before.st_mtime_ns,
                    prepared_bytes=before.st_size + len(block), prepared_sha256=digest.hexdigest()))
                if journal is not None:
                    journal()
                local.replace(backup)
            if file_stamp(before) != file_stamp(path.stat()):
                raise OSError('FLAC source changed during preparation')
            temporary.replace(path)
        finally:
            if owned:
                temporary.unlink(missing_ok=True)
    return True
