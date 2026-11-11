"""Read-only media source traversal, exclusions, and fingerprints."""
import hashlib
import json
import os
from pathlib import Path
import stat

from .guard import guarded_path as _plain

KINDS = ("Music", "Notes", "Photos", "Videos")
VIDEO_EXTENSIONS = {".mp4", ".m4v", ".mov", ".mkv", ".avi", ".webm", ".mpg", ".mpeg", ".wmv", ".ipvf"}


def extensions(kind):
    if kind == "Music":
        from .music import EXTENSIONS
        return EXTENSIONS
    if kind == "Photos":
        from .photos import EXTENSIONS
        return EXTENSIONS
    if kind == "Notes":
        from .notes import EXTENSIONS
        return EXTENSIONS
    if kind == "Videos":
        return VIDEO_EXTENSIONS
    raise ValueError("Unknown media folder")


def source_folders(root, kind):
    root = Path(root)
    folders = [root / kind]
    if kind in ("Music", "Videos"):
        folders.append(root / "iPod_Control" / "Music")
    return [folder for folder in folders if folder.is_dir()]


def file_records(folder, *, allowed=None, inodes=None, checkpoint=None, progress=None):
    if checkpoint:
        checkpoint()
    if not folder.exists():
        return []
    _plain(folder, Path(folder.anchor))
    if not folder.is_dir():
        raise ValueError(f"Expected a folder: {folder}")
    found = []
    directories = [folder]
    while directories:
        if progress:
            progress(len(found))
        if checkpoint:
            checkpoint()
        with os.scandir(directories.pop()) as entries:
            for entry in entries:
                if checkpoint:
                    checkpoint()
                info = entry.stat(follow_symlinks=False)
                path = Path(entry.path)
                if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
                    raise ValueError(f"Linked files or folders are unsupported: {path}")
                if stat.S_ISDIR(info.st_mode):
                    directories.append(path)
                elif stat.S_ISREG(info.st_mode) and (allowed is None or path.suffix.lower() in allowed):
                    if inodes is not None:
                        # Windows directory stats omit file IDs. Fetch the
                        # complete stat once so metadata readers can reuse it.
                        if os.name == 'nt':
                            info = path.lstat()
                            if (not stat.S_ISREG(info.st_mode) or
                                    getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
                                raise ValueError(f"Linked or changed file is unsupported: {path}")
                        inodes[path] = info.st_ino
                    found.append((path, info))
                    if progress and len(found) % 128 == 0:
                        progress(len(found))
    if checkpoint:
        checkpoint()
    if progress:
        progress(len(found))
    return sorted(found, key=lambda item: (str(item[0]).casefold(), str(item[0])))


def collect_records_by_kind(root, kinds=KINDS, *, include_inodes=True, checkpoint=None, progress=None):
    if checkpoint:
        checkpoint()
    kinds = tuple(kinds)
    inode_folders = set(source_folders(root, 'Music')) if include_inodes and 'Music' in kinds else set()
    folders, result = {}, _RecordInventory()
    for kind in kinds:
        if progress:
            progress('Reading ' + kind.lower() + ' folders…')
        if checkpoint:
            checkpoint()
        if kind not in KINDS:
            raise ValueError("Unknown media folder")
        result[kind] = []
        for folder in source_folders(root, kind):
            if folder not in folders:
                folders[folder] = file_records(
                    folder, inodes=result.inodes if folder in inode_folders else None,
                    checkpoint=checkpoint, progress=(lambda count: progress(
                        'Reading ' + kind.lower() + f' folders · {count:,} files found')) if progress else None)
            result[kind].extend(folders[folder])
    return result


class _RecordInventory(dict):
    """Directory records plus file IDs already returned by traversal."""
    def __init__(self):
        super().__init__()
        self.inodes = {}


def skipped_paths(state, kind):
    skipped = state.get("skipped_files", {}) if isinstance(state, dict) else {}
    values = skipped.get(kind, []) if isinstance(skipped, dict) else []
    if not isinstance(values, list):
        return set()
    result = set()
    prefixes = {(kind.casefold(),)}
    if kind in ("Music", "Videos"):
        prefixes.add(("ipod_control", "music"))
    for value in values:
        if not isinstance(value, str):
            continue
        path = Path(value)
        parts = tuple(part.casefold() for part in path.parts)
        if (path.is_absolute() or path.drive or ".." in path.parts or
                not any(parts[:len(prefix)] == prefix for prefix in prefixes)):
            continue
        result.add(path.as_posix().casefold())
    return result


def scan(root, kinds=KINDS, skipped=None, *, records_by_kind=None, retail_photos=None,
         fingerprint_kinds=None, retail_tracks=None, checkpoint=None, progress=None):
    if checkpoint:
        checkpoint()
    root = Path(root).resolve(strict=True)
    kinds = tuple(kinds)
    if records_by_kind is None:
        records_by_kind = collect_records_by_kind(
            root, kinds, include_inodes=False, checkpoint=checkpoint, progress=progress)
    skipped = skipped or {}
    fingerprint_kinds = set(KINDS if fingerprint_kinds is None else fingerprint_kinds)
    inodes = getattr(records_by_kind, 'inodes', {})
    result = {}
    for kind in kinds:
        if checkpoint:
            checkpoint()
        if kind not in KINDS:
            raise ValueError("Unknown media folder")
        excluded = {Path(value).as_posix().casefold() for value in skipped.get(kind, ())}
        fingerprint = kind in fingerprint_kinds
        rows, supported = [], []
        supported_bytes = skipped_count = ignored = 0
        allowed = extensions(kind)
        cover_names = None
        if kind == 'Music' and fingerprint:
            from .music import COVER_NAMES
            cover_names = {name.casefold() for name in COVER_NAMES}
        records = records_by_kind[kind]
        def source_stamp(path, info):
            # Replacements can retain size and modification time. Windows
            # directory stats omit file IDs; reuse traversal IDs when known.
            inode = inodes.get(path) or info.st_ino
            if not inode:
                inode = path.stat().st_ino
            return info.st_size, info.st_mtime_ns, info.st_ctime_ns, inode
        music_folders, cover_rows = set(), []
        last_parent, relative_prefix = None, None
        for index, (path, info) in enumerate(records):
            if progress and index % 128 == 0:
                progress('Checking ' + kind.lower() + f' for preparation · {index + 1:,} of {len(records):,} files')
            if checkpoint:
                checkpoint()
            relative = None
            if fingerprint or excluded:
                parent = str(path.parent)
                if parent == last_parent:
                    relative = relative_prefix + path.name
                else:
                    relative = path.relative_to(root).as_posix()
                    slash = relative.rfind('/')
                    # A root-level file has no reusable parent prefix.
                    last_parent = parent if slash >= 0 else None
                    relative_prefix = relative[:slash + 1] if slash >= 0 else None
            if excluded and relative.casefold() in excluded:
                skipped_count += 1
                continue
            supported_file = path.suffix.lower() in allowed
            # Music and Videos share iPod_Control/Music. A cover matters only
            # when its folder also contains a playable audio source.
            if fingerprint and supported_file:
                rows.append((relative, *source_stamp(path, info)))
                if cover_names is not None:
                    music_folders.add(path.parent)
            elif fingerprint and cover_names is not None and path.name.casefold() in cover_names:
                cover_rows.append((path.parent, (relative, *source_stamp(path, info))))
            if supported_file:
                supported.append(str(path))
                supported_bytes += info.st_size
            else:
                ignored += 1
        if fingerprint and cover_rows:
            rows.extend(row for folder, row in cover_rows if folder in music_folders)
        if fingerprint and kind in ('Music', 'Videos'):
            database = root / 'iPod_Control/iTunes/iTunesDB'
            _plain(database, root)
            if database.is_file():
                info = database.stat()
                rows.append(('iTunesDB', *source_stamp(database, info)))
            if kind == 'Music' and supported:
                # Artwork affects Music only when playable audio exists. Video
                # readiness uses source/output stamps without artwork scans.
                artwork_folder = root / 'iPod_Control' / 'Artwork'
                for path, info in file_records(artwork_folder, checkpoint=checkpoint,
                        progress=(lambda count: progress(f'Reading album artwork · {count:,} files found')) if progress else None):
                    rows.append((path.relative_to(root).as_posix(), *source_stamp(path, info)))
                from .album_art import signature as artwork_signature
                from .retail_media import available_tracks
                tracks = available_tracks(root) if retail_tracks is None else retail_tracks
                rows.append(('album-art', artwork_signature(tracks.values())))
        if kind == 'Photos':
            from .retail_media import images
            photos = retail_photos if retail_photos is not None else images(root)
            for photo in photos:
                if checkpoint:
                    checkpoint()
                if photo['relative'].casefold() in excluded:
                    skipped_count += 1
                    continue
                supported.append(str(root / photo['relative']))
                supported_bytes += photo['bytes']
                if fingerprint:
                    rows.append((photo['relative'], photo['bytes'], photo['mtime_ns'],
                                 photo['preview'], photo['title']))
        result[kind] = dict(count=len(supported), bytes=supported_bytes,
                            ignored=ignored, files=supported,
                            skipped=skipped_count,
                            fingerprint=hashlib.sha256(json.dumps(rows).encode()).hexdigest() if fingerprint else None)
    if checkpoint:
        checkpoint()
    return result
