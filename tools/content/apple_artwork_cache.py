"""Disposable, content-addressed host copies of verified Apple artwork."""
from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import stat
import tempfile

from .apple_process import checkpoint
from .guard import guarded_path
from .host_state import file_stamp, save_json


_BLOB = re.compile(r'[0-9a-f]{64}\.blob')
_ARTWORK = 'iPod_Control/Artwork/'
_MAX_ENTRIES = 4096
_MAX_BYTES = 2 * 1024 ** 3


class ArtworkCache:
    """Reuse source proofs only while volume, membership and file stamps match.

    Cached bytes are hashed while copying into a private stage. A corrupt cache
    is a miss, so it can never provide unverified data to the database writer.
    """

    def __init__(self, directory, binding, files, *, reserve_bytes, cancelled):
        self.directory = Path(directory)
        self.binding = binding
        self.members = sorted(name for name in files if name.startswith(_ARTWORK))
        self.reserve_bytes = reserve_bytes
        self.cancelled = cancelled
        self.previous = {}
        self.verified = {}
        self.enabled = False
        try:
            guarded_path(self.directory, self.directory.parent)
            self.directory.mkdir(parents=True, exist_ok=True)
            self.enabled = True
            index = self.directory / 'index.json'
            info = guarded_path(index, self.directory)
            if info is None or not stat.S_ISREG(info.st_mode) or info.st_size > 4 * 1024 ** 2:
                return
            value = json.loads(index.read_text(encoding='utf-8'))
            if (isinstance(value, dict) and value.get('version') == 1 and
                    value.get('binding') == binding and value.get('members') == self.members and
                    isinstance(value.get('files'), dict) and len(value['files']) <= _MAX_ENTRIES):
                self.previous = value['files']
        except (OSError, ValueError):
            pass

    @staticmethod
    def accepts(name):
        return name.startswith(_ARTWORK) and re.fullmatch(
            r'F[0-9]+_[0-9]+\.ithmb', Path(name).name, re.IGNORECASE) is not None

    def copy(self, name, info, destination, stage, copy_hash):
        """Return the verified digest on a cache hit, otherwise None."""
        if not self.enabled or not self.accepts(name):
            return None
        saved = self.previous.get(name)
        if (not isinstance(saved, dict) or saved.get('stamp') != list(file_stamp(info)) or
                not isinstance(saved.get('sha256'), str) or
                re.fullmatch(r'[0-9a-f]{64}', saved['sha256']) is None):
            return None
        expected = saved['sha256']
        source = self.directory / (expected + '.blob')
        try:
            blob_info = guarded_path(source, self.directory)
            if blob_info is None or not stat.S_ISREG(blob_info.st_mode) or blob_info.st_size != info.st_size:
                return None
            # A failed disposable-cache read cannot leave a partial stage file.
            with tempfile.TemporaryDirectory(prefix='artwork-cache-read-', dir=stage) as temporary:
                copied = Path(temporary) / 'artwork'
                digest = copy_hash(source, copied, blob_info, self.cancelled)
                if digest != expected:
                    return None
                checkpoint(self.cancelled)
                if destination.exists():
                    raise ValueError('The Apple artwork staging path is already occupied.')
                copied.rename(destination)
            self.verified[name] = {'stamp': list(file_stamp(info)), 'sha256': digest}
            return digest
        except (OSError, ValueError):
            return None

    def remember(self, name, info, source, digest, copy_hash):
        """Keep a verified private copy after the initial source read."""
        if not self.enabled or not self.accepts(name):
            return False
        try:
            if shutil.disk_usage(self.directory).free < info.st_size + self.reserve_bytes:
                return False
            target = self.directory / (digest + '.blob')
            guarded_path(target, self.directory)
            if not self._within_budget(target, info.st_size):
                return False
            # Temporary bytes belong to the already private stage, so an
            # interrupted process cannot accumulate unindexed cache files.
            with tempfile.TemporaryDirectory(prefix='artwork-cache-write-', dir=source.parent) as temporary:
                copied = Path(temporary) / 'artwork'
                if copy_hash(source, copied, source.stat(), self.cancelled) != digest:
                    return False
                checkpoint(self.cancelled)
                copied.replace(target)
            self.verified[name] = {'stamp': list(file_stamp(info)), 'sha256': digest}
            return True
        except (OSError, ValueError):
            return False

    def _within_budget(self, target, size):
        """Count every owned object, including orphans from interrupted runs."""
        total, count = size, 1
        for path in self.directory.iterdir():
            checkpoint(self.cancelled)
            if not _BLOB.fullmatch(path.name) or path == target:
                continue
            count += 1
            if count > _MAX_ENTRIES:
                return False
            info = guarded_path(path, self.directory)
            if info is not None:
                if not stat.S_ISREG(info.st_mode):
                    return False
                total += info.st_size
                if total > _MAX_BYTES:
                    return False
        return total <= _MAX_BYTES

    def finish(self):
        if not self.enabled:
            return
        checkpoint(self.cancelled)
        try:
            retained, objects, total = {}, set(), 0
            for name, row in self.verified.items():
                extra = 0 if row['sha256'] in objects else row['stamp'][2]
                if total + extra > _MAX_BYTES or len(retained) >= _MAX_ENTRIES:
                    continue
                retained[name] = row
                total += extra
                objects.add(row['sha256'])
            save_json(self.directory / 'index.json', {
                'version': 1, 'binding': self.binding, 'members': self.members,
                'files': retained})
            keep = {digest + '.blob' for digest in objects}
            count = 0
            for path in self.directory.iterdir():
                checkpoint(self.cancelled)
                if not _BLOB.fullmatch(path.name):
                    continue
                count += 1
                if count > _MAX_ENTRIES * 2:
                    break
                if path.name in keep:
                    continue
                info = guarded_path(path, self.directory)
                if info is not None and stat.S_ISREG(info.st_mode):
                    path.unlink()  # Only unreferenced objects owned by this cache.
        except (OSError, ValueError):
            pass  # A disposable host cache never determines publication success.
