"""Disposable object cache with compiler-reported dependency sets.

The first compile records all dependencies selected by the effective flags.
Later builds hash those same files before object reuse. A changed include graph
changes a parent dependency and forces a fresh compile and dependency record.
"""
from __future__ import annotations

import hashlib
import json
import os
from functools import lru_cache
from pathlib import Path
import stat
import tempfile


MAX_OBJECT_BYTES = 64 * 1024 * 1024
MAX_CACHE_BYTES = 1024 * 1024 * 1024
INCLUDE_SUFFIXES = {'.h', '.hh', '.hpp', '.hxx', '.inc', '.inl', '.c', '.cc', '.cpp', '.s', '.def', '.asm'}


def include_names(root, generated, compiler, source_paths, run):
    """Fingerprint candidate include names, including newly shadowing files."""
    resource = Path(run([compiler, '-print-resource-dir']))
    paths = ((path, 'source/' + path.relative_to(root).as_posix())
             for path in source_paths if path.suffix.lower() in INCLUDE_SUFFIXES)
    generated_paths = ((path, 'generated/' + path.relative_to(generated).as_posix())
                       for path in generated.rglob('*') if path.is_file())
    resource_paths = ((path, 'resource/' + path.relative_to(resource).as_posix())
                      for path in resource.rglob('*') if path.is_file()
                      and path.suffix.lower() in INCLUDE_SUFFIXES)
    digest = hashlib.sha256()
    for _, name in sorted((*paths, *generated_paths, *resource_paths), key=lambda item: item[1]):
        encoded = name.encode()
        digest.update(len(encoded).to_bytes(4, 'little'))
        digest.update(encoded)
    return digest.hexdigest()


def _dependencies(path):
    data = path.read_text(encoding='utf-8').replace('\\\n', '')
    if not data.startswith('cache:'):
        raise ValueError('compiler dependency output has an unexpected target')
    result, word, index = [], [], 6
    while index < len(data):
        char = data[index]
        if index + 1 < len(data) and (
                char == '\\' and data[index + 1] in (' ', '\\', '#') or
                char == '$' and data[index + 1] == '$'):
            word.append(data[index + 1])
            index += 2
            continue
        if char.isspace():
            if word:
                result.append(''.join(word))
                word = []
        else:
            word.append(char)
        index += 1
    if word:
        result.append(''.join(word))
    if not result:
        raise ValueError('compiler returned no source dependencies')
    return result


def _identity(path, root, generated):
    path = path.resolve(strict=True)
    if path.is_relative_to(generated):
        return 'generated/' + path.relative_to(generated).as_posix()
    if path.is_relative_to(root):
        return 'source/' + path.relative_to(root).as_posix()
    return 'external/' + path.as_posix()


def _path(identity, root, generated):
    if identity.startswith('generated/'):
        return generated / identity[len('generated/'):]
    if identity.startswith('source/'):
        return root / identity[len('source/'):]
    if identity.startswith('external/'):
        return Path(identity[len('external/'):])
    raise ValueError('invalid object dependency identity')


def _base_key(command, source, root, generated, compiler_sha256, name_inventory):
    flags = [str(value).replace(str(generated), '<generated>')
             for value in command[1:-4]]
    source_name = _identity(source, root, generated)
    return hashlib.sha256(json.dumps([compiler_sha256, flags, source_name, name_inventory],
        separators=(',', ':')).encode()).hexdigest()


def file_stamp(info):
    # Windows stat/fstat disagree about ctime on newer Python versions. Use
    # the explicit creation time there, alongside file identity and mtime.
    change_time = getattr(info, 'st_birthtime_ns', info.st_ctime_ns) if os.name == 'nt' else info.st_ctime_ns
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, change_time)


def _hash_file(path):
    return _hash_stamp(str(path), file_stamp(path.stat()))


@lru_cache(maxsize=8192)
def _hash_stamp(path, stamp):
    with Path(path).open('rb') as stream:
        before = os.fstat(stream.fileno())
        if file_stamp(before) != stamp:
            raise RuntimeError(f'build dependency changed while hashing: {path}')
        checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
        after = os.fstat(stream.fileno())
    if file_stamp(after) != stamp:
        raise RuntimeError(f'build dependency changed while hashing: {path}')
    return checksum


def _fingerprints(values, root, generated):
    result = {}
    for value in values:
        path = Path(value)
        if not path.is_absolute():
            path = root / path
        identity = _identity(path, root, generated)
        if identity not in result:
            result[identity] = _hash_file(path)
    return result


def _restore(directory, output, root, generated, source_name):
    for manifest in directory.glob('*.json'):
        try:
            data = json.loads(manifest.read_text(encoding='utf-8'))
            if not isinstance(data, dict):
                continue
            manifest_digest = hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()
            if manifest_digest != manifest.stem:
                continue
            dependencies = data.get('dependencies')
            if not isinstance(dependencies, dict) or source_name not in dependencies:
                continue
            if any(_hash_file(_path(name, root, generated)) != checksum
                   for name, checksum in dependencies.items()):
                continue
            path = manifest.with_suffix('.bin')
            if not path.is_file() or not 1 <= path.stat().st_size <= MAX_OBJECT_BYTES:
                continue
            payload = path.read_bytes()
            if hashlib.sha256(payload).hexdigest() != data.get('object_sha256'):
                continue
            output.write_bytes(payload)
            os.utime(manifest, None)
            os.utime(path, None)
            return True
        except (OSError, ValueError, TypeError, KeyError):
            continue
    return False


def _atomic_bytes(path, payload):
    fd, name = tempfile.mkstemp(prefix=path.stem + '.', suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as writer:
            writer.write(payload)
            writer.flush()
            os.fsync(writer.fileno())
        os.replace(name, path)
    finally:
        try:
            Path(name).unlink(missing_ok=True)
        except OSError:
            pass


def _save(directory, dependencies, output):
    try:
        if output.stat().st_size > MAX_OBJECT_BYTES:
            return
        payload = output.read_bytes()
        if len(payload) > MAX_OBJECT_BYTES:
            return
        manifest = dict(dependencies=dependencies,
                        object_sha256=hashlib.sha256(payload).hexdigest())
        checksum = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
        path = directory / checksum
        directory.mkdir(parents=True, exist_ok=True)
        _atomic_bytes(path.with_suffix('.bin'), payload)
        _atomic_bytes(path.with_suffix('.json'), json.dumps(manifest, sort_keys=True).encode())
    except OSError:
        pass  # A host cache failure does not invalidate the compiled object.


def compile_object(command, root, generated, compiler_sha256, name_inventory, cache_root, run):
    """Return whether a validated cached object replaced a compiler invocation."""
    source, output = Path(command[-3]), Path(command[-1])
    source_name = _identity(source, root, generated)
    base = _base_key(command, source, root, generated, compiler_sha256, name_inventory)
    directory = cache_root / base[:2] / base
    if _restore(directory, output, root, generated, source_name):
        return True
    dependency_file = output.with_suffix(output.suffix + '.d')
    try:
        # On a miss, bracket code generation with dependency snapshots. A
        # concurrent header edit must not be published under the later bytes.
        run([*command[:-4], '-M', '-MT', 'cache', '-MF', dependency_file, source])
        before = _fingerprints(_dependencies(dependency_file), root, generated)
        run([*command, '-MD', '-MT', 'cache', '-MF', dependency_file])
        dependencies = _fingerprints(_dependencies(dependency_file), root, generated)
        if source_name not in dependencies:
            raise ValueError(f'compiler dependency output omitted source: {source}')
        if before != dependencies:
            raise RuntimeError(f'build dependency changed while compiling: {source}')
        _save(directory, dependencies, output)
    finally:
        dependency_file.unlink(missing_ok=True)
    return False


def trim(cache_root):
    """Remove oldest disposable versions only after an invocation has linked."""
    try:
        root_info = cache_root.lstat()
        if (not stat.S_ISDIR(root_info.st_mode) or
                getattr(root_info, 'st_file_attributes', 0) & 0x400):
            return
        entries, directories = [], [(cache_root, 0)]
        while directories:
            folder, depth = directories.pop()
            with os.scandir(folder) as children:
                for child in children:
                    if depth == 2 and not os.path.normcase(child.name).endswith('.bin'):
                        continue
                    # Windows enumeration already supplies size and mtime.
                    # Stay at the cache's fixed depth and never follow links.
                    info = child.stat(follow_symlinks=False)
                    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                        continue
                    if depth < 2 and stat.S_ISDIR(info.st_mode):
                        directories.append((child.path, depth + 1))
                    elif depth == 2 and stat.S_ISREG(info.st_mode):
                        entries.append((Path(child.path), info))
        total = sum(info.st_size for _, info in entries)
        if total <= MAX_CACHE_BYTES:
            return
        for path, info in sorted(entries, key=lambda item: item[1].st_mtime_ns):
            if total <= MAX_CACHE_BYTES:
                break
            try:
                path.unlink()
                path.with_suffix('.json').unlink(missing_ok=True)
                total -= info.st_size
            except OSError:
                pass
    except OSError:
        pass
