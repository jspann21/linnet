"""Prepare exact AAC-LC seek checkpoints with the device's fixed-point decoder."""
from functools import lru_cache
import hashlib
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

from .host_state import file_stamp
from . import SourceError

ROOT = Path(__file__).resolve().parents[2]
NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
PREPARATION_VERSION = 3


@lru_cache(maxsize=1)
def executable():
    bundled = Path(getattr(sys, '_MEIPASS', ROOT)) / 'aac-index.exe'
    if getattr(sys, 'frozen', False):
        if not bundled.is_file():
            raise RuntimeError('AAC preparation tool is missing from this Media Manager')
        return bundled
    from tools.studio.build import toolchain
    compiler = toolchain()['compiler']
    sources = ['tools/content/aac_index.c', 'services/audio/aac/aac.c',
               'services/audio/aac/bits.c', 'services/audio/aac/stereo.c',
               'services/audio/aac/reconstruct.c', 'services/audio/aac/tns.c',
               'services/audio/aac/gain.c', 'services/audio/aac/window.c',
               'services/audio/aac/m4a.c', 'services/audio/m4a/demux.c']
    sources += ['third_party/aac/' + name for name in (ROOT / 'third_party/aac/SOURCES').read_text().splitlines() if name]
    flags = ['-O3', '-fwrapv', '-fno-strict-aliasing', '-I.',
             '-Ithird_party/aac', '-Ithird_party/aac/faad', '-Ithird_party/aac/lib']
    if os.name == 'nt':
        flags.extend(('-flto', '-municode'))
    digest = hashlib.sha256(compiler['sha256'].encode())
    digest.update('\0'.join(flags).encode())
    inputs = set(sources)
    for folder in ('services/audio/aac', 'services/audio/m4a', 'third_party/aac', 'boot/include'):
        inputs.update(p.relative_to(ROOT).as_posix() for p in (ROOT / folder).rglob('*.h'))
    for name in sorted(inputs):
        digest.update(name.encode()); digest.update((ROOT / name).read_bytes())
    directory = ROOT / 'out/aac-index' / digest.hexdigest()[:24]
    destination = directory / ('aac-index.exe' if os.name == 'nt' else 'aac-index')
    if destination.is_file():
        return destination
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / (uuid.uuid4().hex + destination.suffix)
    command = [compiler['path'], *flags]
    try:
        result = subprocess.run([*command, *sources, '-o', str(temporary)], cwd=ROOT,
                                capture_output=True, text=True, timeout=120, creationflags=NO_WINDOW)
        if result.returncode:
            raise RuntimeError('AAC preparation build failed: ' + result.stderr[-4000:])
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def original_size(path, size=None, *, checkpoint=lambda: None):
    """Return the original prefix length only for a valid trailing seek atom."""
    checkpoint()
    if size is None:
        size = path.stat().st_size
    with path.open('rb') as reader:
        at = 0
        for _ in range(4096):
            checkpoint()
            if size - at < 8:
                return None
            reader.seek(at)
            head = reader.read(8)
            if len(head) != 8:
                return None
            length, kind = struct.unpack('>I4s', head)
            header = 8
            if length == 1:
                extended = reader.read(8)
                if len(extended) != 8:
                    return None
                length = struct.unpack('>Q', extended)[0]; header = 16
            if length < header or length > size - at:
                return None
            if kind == b'lnsk' and header == 8 and at + length == size:
                h = reader.read(64)
                checkpoint()
                if (len(h) != 64 or h[:4] != b'LNS1' or struct.unpack_from('<I', h, 4)[0] != 16
                        or struct.unpack_from('<Q', h, 16)[0] != at or any(h[36:60])
                        or zlib.crc32(h[:60]) != struct.unpack_from('<I', h, 60)[0]):
                    return None
                packets, count = struct.unpack_from('<II', h, 8)
                return at if 0 < packets <= 1_000_000 and count == (packets + 15) // 16 and length == 72 + count * 32 else None
            at += length
    return None


def same_original(source, destination, *, checkpoint=lambda: None):
    source, destination = Path(source), Path(destination)
    if not destination.is_file():
        return False
    paths = (source, destination)
    before = [path.stat() for path in paths]
    if any(not stat.S_ISREG(info.st_mode) for info in before):
        raise ValueError('file changed while comparing existing AAC')
    size = original_size(destination, before[1].st_size, checkpoint=checkpoint)
    if size is None or before[0].st_size != size:
        return False
    equal = True
    with source.open('rb') as left, destination.open('rb') as right:
        while size:
            checkpoint()
            count = min(size, 1024 * 1024)
            a, b = left.read(count), right.read(count)
            if len(a) != count or len(b) != count:
                raise ValueError('file changed while comparing existing AAC')
            if a != b:
                equal = False
                break
            size -= count
        if equal and left.read(1):
            raise ValueError('file changed while comparing existing AAC')
    if any(file_stamp(info) != file_stamp(path.stat()) for path, info in zip(paths, before)):
        raise ValueError('file changed while comparing existing AAC')
    return equal


def _matches_prefix(path, size, expected_hash):
    before = path.stat()
    if before.st_size < size:
        return False
    digest = hashlib.sha256()
    with path.open('rb') as reader:
        remaining = size
        while remaining:
            chunk = reader.read(min(remaining, 1024 * 1024))
            if not chunk:
                return False
            digest.update(chunk)
            remaining -= len(chunk)
    after = path.stat()
    return ((before.st_size, before.st_mtime_ns, before.st_ino) ==
            (after.st_size, after.st_mtime_ns, after.st_ino)
            and digest.hexdigest() == expected_hash)


def _run_tool(tool, arguments, checkpoint, timeout, *, allow_timeout=False):
    child = subprocess.Popen([str(tool), *(str(arg) for arg in arguments)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=NO_WINDOW)
    started = time.monotonic()
    try:
        while child.poll() is None:
            checkpoint()
            remaining = timeout - (time.monotonic() - started)
            if remaining <= 0:
                if allow_timeout:
                    return None
                raise TimeoutError('AAC preparation timed out')
            try:
                child.wait(timeout=min(.05, remaining))
            except subprocess.TimeoutExpired:
                pass
    finally:
        if child.poll() is None:
            child.terminate(); child.wait(timeout=10)
    return child.returncode


class _ProbeCheckpoint(Exception):
    pass


class _ProbeReader:
    def __init__(self, stream, checkpoint):
        self.stream = stream
        self.checkpoint = checkpoint
        self.operations = 0
        self.bytes = 0
        self.next_checkpoint = 0

    def _step(self, *, finish=False):
        self.operations += 1
        if self.operations > 4096:
            raise ValueError('MP4 codec hint exceeded its operation budget')
        now = time.monotonic()
        if finish or now >= self.next_checkpoint:
            try:
                self.checkpoint()
            except Exception as error:
                raise _ProbeCheckpoint() from error
            self.next_checkpoint = now + .05

    def read(self, size=-1):
        self._step()
        if not 0 <= size <= 16384 or self.bytes + size > 65536:
            raise ValueError('MP4 codec hint exceeded its read budget')
        result = self.stream.read(size)
        self.bytes += len(result)
        return result

    def readinto(self, buffer):
        result = self.read(len(buffer))
        buffer[:len(result)] = result
        return len(result)

    def seek(self, offset, whence=0):
        self._step()
        return self.stream.seek(offset, whence)

    def tell(self):
        self._step()
        return self.stream.tell()


def _possibly_alac(path, checkpoint):
    try:
        from mutagen.mp4 import Atoms, MP4Info
        with path.open('rb') as stream:
            reader = _ProbeReader(stream, checkpoint)
            info = MP4Info()
            info.load(Atoms(reader), reader)
            reader._step(finish=True)
        return info.codec == 'alac'
    except _ProbeCheckpoint as error:
        raise error.__cause__
    except Exception:
        # This hint only avoids a host copy. The full indexer decides support.
        return False


def import_suffix(path, cache, *, checkpoint=lambda: None):
    """Return host-built seek data and unsupported status without changing a source."""
    path, cache = Path(path), Path(cache)
    checkpoint()
    before = path.stat()
    if original_size(path, before.st_size, checkpoint=checkpoint) is not None:
        return None, False
    cache.mkdir(parents=True, exist_ok=True)
    tool = executable()
    if _possibly_alac(path, checkpoint):
        result = _run_tool(tool, (path,), checkpoint, 30, allow_timeout=True)
        if result == 4:
            return None, True
    with tempfile.TemporaryDirectory(prefix='aac-import-', dir=cache) as scratch:
        index = Path(scratch) / 'index.bin'
        result = _run_tool(tool, (path, index), checkpoint, 600)
        if result in (6, 8):
            raise SourceError(f'{path}: AAC audio packets could not be decoded. Replace or repair the source file.', path)
        if result in (4, 7):
            return None, True
        if result:
            raise RuntimeError(f'AAC preparation failed ({result})')
        suffix = index.read_bytes()
        if not suffix:
            raise ValueError('AAC preparation produced no seek data')
    after = path.stat()
    if file_stamp(before) != file_stamp(after):
        raise OSError('AAC source changed during preparation')
    return (suffix, False) if before.st_size + len(suffix) <= 0xffffffff else (None, True)


def mark_unsupported(path, cache):
    path, cache = Path(path), Path(cache)
    before = path.stat()
    key = hashlib.sha256(f'{PREPARATION_VERSION}:{path}:{file_stamp(before)}'.encode()).hexdigest()
    try:
        cache.mkdir(parents=True, exist_ok=True)
        (cache / (key + '.unsupported')).touch()
    except OSError:
        pass  # The cache is disposable; device preparation can retry.


def prepare_file(path, cache, *, checkpoint=lambda: None, recovery=None, journal=None,
                 local_copy=None, verified_copy=None):
    """Atomically augment an iPod copy; retain its compressed audio and tags."""
    from .guard import hash_file
    path, cache = Path(path), Path(cache)
    checkpoint()
    before = path.stat()
    key = hashlib.sha256(f'{PREPARATION_VERSION}:{path}:{file_stamp(before)}'.encode()).hexdigest()
    unsupported = cache / (key + '.unsupported')
    if unsupported.exists():
        return False
    if original_size(path, before.st_size, checkpoint=checkpoint) is not None:
        return False
    cache.mkdir(parents=True, exist_ok=True)
    tool = executable()
    if _possibly_alac(path, checkpoint):
        checkpoint()
        probe = _run_tool(tool, (path,), checkpoint, 30, allow_timeout=True)
        if file_stamp(before) != file_stamp(path.stat()):
            raise OSError('AAC source changed during preparation')
        if probe == 4:
            unsupported.touch(); return False
    with tempfile.TemporaryDirectory(prefix='aac-', dir=cache) as scratch:
        local, index = Path(scratch) / 'source.m4a', Path(scratch) / 'index.bin'
        source_hash = hashlib.sha256()
        with path.open('rb') as reader, local.open('wb') as writer:
            while chunk := reader.read(1024 * 1024):
                checkpoint(); writer.write(chunk); source_hash.update(chunk)
            if recovery is not None:
                writer.flush(); os.fsync(writer.fileno())
        result = _run_tool(tool, (local, index), checkpoint, 600)
        if result in (6, 8):
            raise SourceError(f'{path}: AAC audio packets could not be decoded. Replace or repair the source file.', path)
        if result in (4, 7):
            unsupported.touch(); return False
        if result:
            raise RuntimeError(f'AAC preparation failed ({result})')
        suffix = index.read_bytes()
        if before.st_size + len(suffix) > 0xffffffff:
            return False
        digest = source_hash.copy(); digest.update(suffix)
        after = path.stat()
        if file_stamp(before) != file_stamp(after):
            raise OSError('AAC source changed during preparation')
        if recovery is not None and journal is not None:
            if local_copy is not None:
                local_copy(local)
                checkpoint()
            backup = cache / (uuid.uuid4().hex + '.aac-original')
            suffix_path = cache / (uuid.uuid4().hex + '.aac-append')
            with suffix_path.open('xb') as writer:
                writer.write(suffix); writer.flush(); os.fsync(writer.fileno())
            try:
                local.replace(backup)
            except Exception:
                suffix_path.unlink(missing_ok=True)
                raise
            recovery.append(dict(path=str(path), backup=str(backup), mode='append',
                suffix_path=str(suffix_path), suffix_sha256=hashlib.sha256(suffix).hexdigest(),
                original_bytes=before.st_size, original_sha256=source_hash.hexdigest(),
                atime_ns=before.st_atime_ns, mtime_ns=before.st_mtime_ns,
                prepared_bytes=before.st_size + len(suffix), prepared_sha256=digest.hexdigest()))
            journal()
            with path.open('r+b') as writer:
                opened = os.fstat(writer.fileno())
                # Windows stat() and fstat() can disagree on ctime; their
                # explicit birth times still describe the same file creation.
                if (file_stamp(before)[:4] != file_stamp(opened)[:4] or
                        getattr(before, 'st_birthtime_ns', before.st_ctime_ns) !=
                        getattr(opened, 'st_birthtime_ns', opened.st_ctime_ns)):
                    raise OSError('AAC source changed during preparation')
                writer.seek(0, os.SEEK_END)
                if writer.tell() != before.st_size:
                    raise OSError('AAC source changed during preparation')
                writer.write(suffix); writer.flush(); os.fsync(writer.fileno())
            checkpoint()
            verified_before = path.stat()
            if (hash_file(path, before.st_size + len(suffix)) != digest.hexdigest()
                    or original_size(path, checkpoint=checkpoint) != before.st_size):
                raise OSError('AAC prepared copy did not verify')
            verified_after = path.stat()
            if ((verified_before.st_dev, verified_before.st_ino, verified_before.st_size,
                 verified_before.st_mtime_ns, verified_before.st_ctime_ns) !=
                (verified_after.st_dev, verified_after.st_ino, verified_after.st_size,
                 verified_after.st_mtime_ns, verified_after.st_ctime_ns)):
                raise OSError('AAC prepared copy changed during verification')
            if verified_copy is not None:
                verified_copy(verified_after)
            return True
        temporary = path.with_name('L' + uuid.uuid4().hex[:7] + '.TMP')
        owned = False
        try:
            with temporary.open('xb') as writer, local.open('rb') as reader:
                owned = True
                while chunk := reader.read(1024 * 1024):
                    checkpoint(); writer.write(chunk)
                writer.write(suffix); writer.flush(); os.fsync(writer.fileno())
            checkpoint()
            if hash_file(temporary, before.st_size + len(suffix)) != digest.hexdigest():
                raise OSError('AAC prepared copy did not verify')
            after = path.stat()
            if file_stamp(before) != file_stamp(after):
                raise OSError('AAC source changed during preparation')
            if original_size(temporary, checkpoint=checkpoint) != before.st_size:
                raise ValueError('AAC seek index has invalid geometry')
            if recovery is not None:
                backup = cache / (uuid.uuid4().hex + '.aac-original')
                recovery.append(dict(path=str(path), backup=str(backup), source_copy=str(local),
                    temporary=str(temporary),
                    original_bytes=before.st_size, original_sha256=source_hash.hexdigest(),
                    atime_ns=before.st_atime_ns, mtime_ns=before.st_mtime_ns,
                    prepared_bytes=before.st_size + len(suffix), prepared_sha256=digest.hexdigest()))
                if journal is not None:
                    journal()
                local.replace(backup)
            if file_stamp(before) != file_stamp(path.stat()):
                raise OSError('AAC source changed during preparation')
            temporary.replace(path)
        finally:
            if owned:
                temporary.unlink(missing_ok=True)
    return True


def finish_preparation(change, *, committed, kind='AAC'):
    """Restore an unpublished original, or retire its owned host backup."""
    from .guard import hash_file
    path, backup = Path(change['path']), Path(change['backup'])
    suffix_path = Path(change['suffix_path']) if change.get('mode') == 'append' else None
    if not committed:
        current = path.stat().st_size
        if current == change['original_bytes'] and hash_file(path, current) == change['original_sha256']:
            if change.get('mode') == 'append':
                os.utime(path, ns=(change['atime_ns'], change['mtime_ns']))
                suffix_path.unlink(missing_ok=True)
            backup.unlink(missing_ok=True); return
        if change.get('mode') == 'append' and kind == 'AAC':
            if current < change['original_bytes'] or current > change['prepared_bytes']:
                raise OSError('AAC copy changed after preparation; original backup retained')
            if current == change['prepared_bytes']:
                if hash_file(path, current) != change['prepared_sha256']:
                    raise OSError('AAC copy changed after preparation; original backup retained')
            else:
                if not _matches_prefix(path, change['original_bytes'], change['original_sha256']):
                    raise OSError('AAC copy changed after preparation; original backup retained')
                suffix = suffix_path.read_bytes()
                if (hashlib.sha256(suffix).hexdigest() != change['suffix_sha256']
                        or len(suffix) != change['prepared_bytes'] - change['original_bytes']):
                    raise OSError('AAC recovery suffix changed; original backup retained')
                with path.open('rb') as reader:
                    reader.seek(change['original_bytes'])
                    if reader.read() != suffix[:current - change['original_bytes']]:
                        raise OSError('AAC append changed; original backup retained')
            with path.open('r+b') as writer:
                writer.truncate(change['original_bytes'])
                writer.flush(); os.fsync(writer.fileno())
            if hash_file(path, change['original_bytes']) != change['original_sha256']:
                raise OSError('AAC restoration did not verify; original backup retained')
            os.utime(path, ns=(change['atime_ns'], change['mtime_ns']))
            suffix_path.unlink(missing_ok=True)
            backup.unlink()
            return
        if current != change['prepared_bytes'] or hash_file(path, current) != change['prepared_sha256']:
            raise OSError(f'{kind} copy changed after preparation; original backup retained')
        if hash_file(backup, change['original_bytes']) != change['original_sha256']:
            raise OSError(f'{kind} original backup did not verify')
        temporary = path.with_name('L' + uuid.uuid4().hex[:7] + '.TMP')
        owned = False
        try:
            with temporary.open('xb') as writer, backup.open('rb') as reader:
                owned = True
                while chunk := reader.read(1024 * 1024):writer.write(chunk)
                writer.flush(); os.fsync(writer.fileno())
            if hash_file(temporary, change['original_bytes']) != change['original_sha256']:
                raise OSError(f'{kind} original restoration did not verify')
            temporary.replace(path)
            os.utime(path, ns=(change['atime_ns'], change['mtime_ns']))
        finally:
            if owned:temporary.unlink(missing_ok=True)
    if suffix_path is not None:
        suffix_path.unlink(missing_ok=True)
    backup.unlink()
