"""Preparation output receipts, readback reuse, and progress throttling."""
from __future__ import annotations

import os
import re
import stat
import threading
import time
from .guard import (
    guard_volume as _volume_guard,
    guarded_path as _plain,
    hash_file as _hash,
    require_same_target as _same_target,
)
from .host_state import file_stamp as _receipt_stamp
from .media_options import Cancelled, _RECENT_OUTPUT_NS, format_size
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
from pathlib import Path


def _throttled_progress(checkpoint, progress, describe):
    last = 0.0
    def report(event):
        nonlocal last
        checkpoint()
        now = time.monotonic()
        if now - last >= 0.2 or event['current'] + 1 == event['total']:
            progress(describe(event))
            last = now
    return report


def _outputs_match(root, outputs, *, verified=None, progress=None):
    """Treat malformed/stale host cache entries as misses, with bounded paths."""
    if not isinstance(outputs, list) or not outputs:
        return False
    prepass = verified is not None and (len(outputs) >= 64 or progress is None)
    pending = []
    cached_count = 0
    last_cached = None
    last_prepass = time.monotonic()
    for index, item in enumerate(outputs):
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            return False
        name, digest = item
        if not isinstance(name, str) or not isinstance(digest, str):
            return False
        if prepass and progress and time.monotonic() - last_prepass >= 0.2:
            # A long stat/path pass still needs to service Stop and volume checks.
            progress(dict(current=0, total=len(outputs), file=name,
                          bytes=0, file_bytes=1))
            last_prepass = time.monotonic()
        relative = Path(name)
        if (relative.is_absolute() or relative.drive or ".." in relative.parts or not relative.parts or
                relative.parts[0].casefold() not in {".linnet", "music", "videos", "ipod_control"}):
            return False
        path = root / relative
        try:
            info = _plain(path, root)
            if info is None or not stat.S_ISREG(info.st_mode):
                return False
            stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
            # The cache is already scoped to this disk, volume and partition.
            # A Windows drive-letter change must not discard a recent hash.
            key = (relative.as_posix(), digest)
            # NTFS can report the same timestamps for two immediate writes of
            # equal size. Hash recent files on every check until that window
            # has passed, then reuse the verified stamp on later refreshes.
            recent = os.name == "nt" and info.st_mtime_ns > time.time_ns() - _RECENT_OUTPUT_NS
            def notify(current, total):
                if progress:
                    progress(dict(current=index, total=len(outputs), file=name,
                                  bytes=current, file_bytes=total))
            if verified is not None and not recent and verified.get(key) == stamp:
                if prepass:
                    cached_count += 1
                    last_cached = (name, info.st_size)
                else:
                    notify(info.st_size, info.st_size)
                continue
            if prepass:
                pending.append((path, digest, stamp, key, recent, name))
                continue
            if _hash(path, progress=notify if progress else None) != digest:
                return False
            after = path.stat()
            if stamp != (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino):
                return False
            if verified is not None and not recent:
                verified[key] = stamp
        except (OSError, ValueError):
            return False
    if progress and cached_count:
        progress(dict(current=cached_count - 1, total=len(outputs),
                      file=last_cached[0], bytes=last_cached[1], file_bytes=last_cached[1]))
    if pending:
        if progress is None:
            pending.sort(key=lambda entry: entry[2][0])
        stop = threading.Event()
        def stop_check(_current, _total):
            if stop.is_set():
                raise Cancelled('Stopped while checking published output.')
        def check(entry):
            path, digest, stamp = entry[:3]
            try:
                # Recheck the path immediately before each concurrent read.
                _plain(path, root)
                if _hash(path, progress=stop_check) != digest:
                    return False
                after = path.stat()
                return stamp == (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino)
            except (OSError, ValueError):
                return False
        def report(entry, current, completed):
            if progress:
                size = entry[2][0]
                progress(dict(current=current, total=len(outputs), file=entry[5],
                              bytes=size if completed else 0,
                              file_bytes=size if completed else max(1, size)))

        if len(pending) < 8 or len(outputs) < 64:
            for index, entry in enumerate(pending):
                current = cached_count + index
                try:
                    _plain(entry[0], root)
                    notify = (lambda done, total: progress(dict(
                            current=current, total=len(outputs), file=entry[5],
                            bytes=done, file_bytes=total))) if progress else None
                    if _hash(entry[0], progress=notify) != entry[1]:
                        return False
                    after = entry[0].stat()
                    if entry[2] != (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino):
                        return False
                except (OSError, ValueError):
                    return False
                if not entry[4]:
                    verified[entry[3]] = entry[2]
        else:
            completed = cached_count
            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = []
                try:
                    # Bound outstanding reads and keep a corrupt early output cheap.
                    for start in range(0, len(pending), 32):
                        batch = pending[start:start + 32]
                        futures = [pool.submit(check, entry) for entry in batch]
                        for entry, future in zip(batch, futures):
                            while True:
                                try:
                                    matched = future.result(timeout=0.2 if progress else None)
                                    break
                                except FutureTimeout:
                                    if future.done():
                                        future.result()
                                    report(entry, completed, False)
                            if not matched:
                                return False
                            if not entry[4]:
                                verified[entry[3]] = entry[2]
                            report(entry, completed, True)
                            completed += 1
                finally:
                    stop.set()
                    for future in futures:
                        future.cancel()
    return True


def _checking_progress(kind, checkpoint, progress, stage):
    last, current_file, started = 0.0, -1, 0.0
    def report(event):
        nonlocal last, current_file, started
        now = time.monotonic()
        if event['current'] != current_file:
            current_file, started = event['current'], now
        finished = (event['current'] + 1 == event['total'] and
                    event['bytes'] == event['file_bytes'])
        if now - last < 0.2 and not finished:
            return
        checkpoint()
        fraction = (1.0 if event['bytes'] == event['file_bytes'] else
                    min(1.0, event['bytes'] / max(1, event['file_bytes'])))
        stage((event['current'] + fraction) / event['total'])
        detail = ''
        if event['file_bytes'] >= 1024 * 1024:
            detail = f" · {fraction:.0%} ({format_size(event['bytes'])} / {format_size(event['file_bytes'])})"
            elapsed = now - started
            if elapsed >= 1 and event['bytes'] and fraction < 1:
                seconds = elapsed * (event['file_bytes'] - event['bytes']) / event['bytes']
                detail += (f' · about {max(1, round(seconds / 60))}m left' if seconds >= 60 else
                           f' · about {max(1, round(seconds))}s left')
        progress(f"Checking {kind} {event['current']+1}/{event['total']}: {Path(event['file']).name}{detail}")
        last = now
    return report


def _published_photo_output(root, identity, output_paths, published):
    from .photos import MAX_FILE
    expected = '.LINNET/PHOTOS/LIBRARY.PIM'
    if (output_paths[:2] != [expected, '.LINNET/PHOTOS/GRID.PGR'] or
            len(published) != 1 or not isinstance(published[0], dict)):
        raise RuntimeError('Photo publication did not return its expected output')
    receipt = published[0]
    outputs, stamps = [], {}
    for name, item in [(expected, receipt), ('.LINNET/PHOTOS/GRID.PGR', receipt.get('grid'))]:
        outputs.append(_photo_output(root, identity, name, item, MAX_FILE))
    packs = receipt.get('packs', [])
    if output_paths[2:] != [item.get('path') for item in packs]:
        raise RuntimeError('Photo pack output list differs from its publication receipt')
    for item in packs:
        path = root/item['path']
        _volume_guard(identity)
        _plain(path, root)
        info = path.stat()
        if (Path(item['package']) != path or not stat.S_ISREG(info.st_mode) or
                tuple(item.get('stamp', ())) != _receipt_stamp(info) or
                info.st_size != item['bytes'] or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
            raise RuntimeError('Photo pack changed after verified publication')
        outputs.append((item['path'], item['sha256']))
        # Reused packs were hashed during publication. A fresh copy still
        # needs a hash after Windows' recent-write window before caching it.
        hash_started_ns = item.get('hash_started_ns')
        if (item.get('reused') is True and type(hash_started_ns) is int and
                (os.name != 'nt' or info.st_mtime_ns <= hash_started_ns - _RECENT_OUTPUT_NS)):
            stamps[(item['path'], item['sha256'])] = (
                info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
    return outputs, stamps


def _photo_output(root, identity, expected, receipt, max_file):
    if not isinstance(receipt,dict):
        raise RuntimeError('Photo publication did not return its expected receipt')
    if (receipt.get('passed') is not True or receipt.get('operation') != 'publish-photos' or
            receipt.get('phase') not in ('published', 'reused')):
        raise RuntimeError('Photo publication did not complete')
    _same_target(identity, receipt['identity'])
    active = root / expected
    if Path(receipt['package']) != active:
        raise RuntimeError('Photo publication output path changed')
    digest, size = receipt['candidate_sha256'], receipt['candidate_bytes']
    if (not isinstance(digest, str) or re.fullmatch('[0-9a-f]{64}', digest) is None or
            type(size) is not int or not 512 <= size <= max_file):
        raise RuntimeError('Photo publication returned invalid output metadata')
    _plain(active, root)
    info = active.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size != size:
        raise RuntimeError('Photo publication output changed after readback')
    return expected, digest


def _music_outputs_from_publication(root, output_paths, verified_outputs, identity):
    """Return publication digests and stamps safe for the status hash cache."""
    _volume_guard(identity)
    outputs, stamps = [], {}
    for name in output_paths:
        proof = verified_outputs.get(name)
        path = root / name
        _plain(path, root)
        if proof is None:
            before = path.stat()
            hash_started = time.time_ns()
            digest = _hash(path)
            info = path.stat()
            if _receipt_stamp(before) != _receipt_stamp(info):
                raise ValueError(f"music output changed after verification: {path}")
            # Match _outputs_match's cache admission rule. A hash of a recent
            # write cannot become a trusted cache entry once the window ends.
            cacheable = os.name != "nt" or before.st_mtime_ns <= hash_started - _RECENT_OUTPUT_NS
        else:
            if not isinstance(proof, tuple) or len(proof) != 2:
                raise ValueError(f"invalid music publication proof: {name}")
            digest, stamp = proof
            info = path.stat()
            if _receipt_stamp(info) != stamp:
                raise ValueError(f"music output changed after publication: {path}")
            # Publication proofs do not record when their readback happened.
            # Fresh copies must get one hash after the recent-write window.
            cacheable = os.name != "nt"
        outputs.append((name, digest))
        if cacheable:
            stamps[(name, digest)] = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
    return outputs, stamps
