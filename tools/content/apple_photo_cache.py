"""Disposable, bounded host cache of normalized Apple photo inputs."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import time
import uuid

from .apple_process import checkpoint
from .guard import guarded_path
from .host_state import file_stamp, save_json


_BUDGET = 1024 * 1024 ** 2
_MAX_ENTRIES = 4096
_MAX_IMAGE = 128 * 1024 ** 2
_RECIPE = 'exif-transpose-rgb-or-rgba-png-v1'
_KEY = re.compile(r'[0-9a-f]{64}')


def _copy(source, destination, cancelled):
    digest = hashlib.sha256()
    copied = 0
    with source.open('rb') as reader, destination.open('xb') as writer:
        while data := reader.read(1024 * 1024):
            checkpoint(cancelled)
            copied += len(data)
            if copied > _MAX_IMAGE:
                raise ValueError('Normalized photo cache entry exceeds its size limit.')
            digest.update(data)
            writer.write(data)
    return digest.hexdigest()


class NormalizedPhotos:
    def __init__(self, cache, *, cancelled=lambda: False):
        self.directory = Path(cache) / 'apple-photo-normalized'
        self.cancelled = cancelled
        self.entries = {}
        self.enabled = False
        self.dirty = False
        self.files = {}
        try:
            guarded_path(self.directory, Path(cache))
            self.directory.mkdir(parents=True, exist_ok=True)
            # Inventory only our flat disposable files; never recurse or remove
            # unexpected contents. A damaged/oversized cache is simply unused.
            with os.scandir(self.directory) as files:
                for index, entry in enumerate(files):
                    checkpoint(cancelled)
                    if index >= _MAX_ENTRIES * 2:
                        return
                    if (entry.name.endswith('.png') and _KEY.fullmatch(entry.name[:-4]) and
                            entry.is_file(follow_symlinks=False)):
                        info = entry.stat(follow_symlinks=False)
                        self.files[entry.name[:-4]] = (info.st_size, info.st_mtime_ns)
            index_path = self.directory / 'index.json'
            guarded_path(index_path, self.directory)
            if index_path.is_file() and index_path.stat().st_size <= 2 * 1024 ** 2:
                saved = json.loads(index_path.read_text(encoding='utf-8'))
                rows = saved.get('entries') if isinstance(saved, dict) else None
                if isinstance(saved, dict) and saved.get('version') == 1 and isinstance(rows, dict) and len(rows) <= _MAX_ENTRIES:
                    for key, row in rows.items():
                        if (_KEY.fullmatch(key) and isinstance(row, dict) and
                                type(row.get('bytes')) is int and 0 < row['bytes'] <= _MAX_IMAGE and
                                isinstance(row.get('sha256'), str) and _KEY.fullmatch(row['sha256']) and
                                type(row.get('used')) is int and row['used'] >= 0):
                            self.entries[key] = row
            self.enabled = True
            self._prune()
        except (OSError, ValueError, AttributeError):
            pass  # Normalization can always run without its disposable cache.

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.enabled and self.dirty:
            try:
                save_json(self.directory / 'index.json', {'version': 1, 'entries': self.entries})
            except OSError:
                pass

    def _prune(self, reserve=0):
        total = sum(size for size, _ in self.files.values())
        order = sorted(self.files, key=lambda key: self.entries.get(key, {}).get('used', 0))
        for key in order:
            if total + reserve <= _BUDGET and len(self.files) < _MAX_ENTRIES:
                break
            checkpoint(self.cancelled)
            path = self.directory / (key + '.png')
            try:
                guarded_path(path, self.directory)
                path.unlink(missing_ok=True)
            except (OSError, ValueError):
                continue
            total -= self.files.pop(key)[0]
            self.entries.pop(key, None)
            self.dirty = True
        # Missing entries do not retain ownership or affect publication.
        absent = set(self.entries) - set(self.files)
        for key in absent:
            self.entries.pop(key)
        self.dirty |= bool(absent)
        return total + reserve <= _BUDGET and len(self.files) < _MAX_ENTRIES

    def prepare(self, path, info, root, destination, *, work_bytes):
        """Return True on a verified cache hit; otherwise normalize the source."""
        from PIL import Image, ImageOps, __version__ as pillow_version
        from .apple_library import _copy_hash

        def verify_source():
            current = guarded_path(path, root)
            if current is None or not stat.S_ISREG(current.st_mode) or file_stamp(current) != file_stamp(info):
                raise ValueError(f'Photo changed during preparation: {path.relative_to(root)}')

        checkpoint(self.cancelled)
        verify_source()
        if destination.exists():
            raise ValueError('Normalized photo staging path is already occupied.')
        if info.st_size > _MAX_IMAGE:
            raise ValueError(f'Photo is too large for Apple OS preparation: {path.relative_to(root)}')
        key = hashlib.sha256(json.dumps([path.relative_to(root).as_posix(), file_stamp(info),
                                        _RECIPE, pillow_version], separators=(',', ':')).encode()).hexdigest()
        cached = self.directory / (key + '.png')
        row = self.entries.get(key) if self.enabled else None
        hit = False
        if row:
            try:
                before = guarded_path(cached, self.directory)
                if before is not None and stat.S_ISREG(before.st_mode) and before.st_size == row['bytes']:
                    digest = _copy(cached, destination, self.cancelled)
                    hit = digest == row['sha256'] and file_stamp(cached.stat()) == file_stamp(before)
            except (OSError, ValueError):
                hit = False
            finally:
                if not hit:
                    destination.unlink(missing_ok=True)
        if hit:
            verify_source()
            os.utime(destination, ns=(info.st_atime_ns, info.st_mtime_ns))
            row['used'] = time.time_ns()
            self.dirty = True
            return True

        original = destination.with_name(destination.stem + '.original' + path.suffix.lower())
        try:
            _copy_hash(path, original, info, self.cancelled)
            checkpoint(self.cancelled)
            with Image.open(original) as image:
                if (not image.width or not image.height or max(image.size) > 16384 or
                        image.width * image.height > 32_000_000):
                    raise ValueError(f'Photo dimensions exceed the decode limit: {path.relative_to(root)}')
                if image.width * image.height * 4 + work_bytes > shutil.disk_usage(destination.parent).free:
                    raise ValueError('Not enough computer space to prepare this Apple photo.')
                with ImageOps.exif_transpose(image) as oriented:
                    with oriented.convert('RGBA' if 'A' in oriented.getbands() or
                                           'transparency' in oriented.info else 'RGB') as pixels:
                        pixels.save(destination, format='PNG')
            checkpoint(self.cancelled)
            verify_source()
            os.utime(destination, ns=(info.st_atime_ns, info.st_mtime_ns))
        finally:
            original.unlink(missing_ok=True)
        self._store(key, destination)
        return False

    def _store(self, key, source):
        if not self.enabled:
            return
        size = source.stat().st_size
        if size > _MAX_IMAGE or not self._prune(reserve=size):
            return
        temporary = self.directory / (uuid.uuid4().hex + '.tmp')
        try:
            digest = _copy(source, temporary, self.cancelled)
            target = self.directory / (key + '.png')
            guarded_path(target, self.directory)
            temporary.replace(target)
            info = target.stat()
            self.files[key] = (size, info.st_mtime_ns)
            self.entries[key] = {'bytes': size, 'sha256': digest, 'used': time.time_ns()}
            self.dirty = True
        except (OSError, ValueError):
            pass
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
