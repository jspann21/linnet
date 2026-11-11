"""Readable catalog labels for recursively discovered video sources."""
import hashlib
from pathlib import Path


def catalog_contains(root, relative, title, path=None, *, snapshot=None):
    """Check the small bounded device catalog before trusting a video cache hit."""
    from .publish import _plain
    from .video_catalog import read_file
    catalog = Path(root) / ".LINNET" / "VIDEOS" / "INDEX.VID"
    try:
        if snapshot is not None and 'entries' in snapshot:
            entries = snapshot['entries']
        else:
            _plain(catalog, Path(root))
            if not catalog.is_file():
                entries = []
            else:
                _, entries = read_file(catalog)
            if snapshot is not None:
                snapshot['entries'] = entries
        key = hashlib.sha256(relative.encode("utf-8")).digest()
        return (any(k == key and p == path and (title is None or t == title)
                    for k, p, t in entries) if path else
                any(k == key and (title is None or t == title) and
                    (Path(root) / p.lstrip('/')).is_file() for k, p, t in entries))
    except (OSError, ValueError):
        return False


def video_title(relative):
    """Keep folder context and extensions within the firmware's 127-byte label.

    A stable source digest distinguishes long paths after shortening; keeping
    the end of the path preserves the actual video name instead of just a
    potentially long series of parent folders.
    """
    source = str(relative).replace("\\", "/")
    label = "".join(character if ord(character) >= 32 else " " for character in source).strip()
    if not label:
        label = "Video"
    encoded = label.encode("utf-8")
    if len(encoded) <= 127:
        return label
    suffix = " [" + hashlib.sha256(source.encode("utf-8")).hexdigest()[:16] + "]"
    prefix = encoded[:32].decode("utf-8", errors="ignore")
    separator = "…"
    budget = 127 - len((prefix + separator + suffix).encode("utf-8"))
    tail = encoded[-budget:].decode("utf-8", errors="ignore")
    return prefix + separator + tail + suffix
