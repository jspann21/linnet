"""Host-side JSON cache reads and atomic state and receipt writes."""
import hashlib
import json
import os
from pathlib import Path
import time
import uuid


def file_stamp(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def storage_root():
    return Path(os.environ.get("LOCALAPPDATA", Path.home() / ".cache")) / "Linnet"


def media_state_root():
    # Receipts and recovery journals contain absolute paths. Keep them stable.
    return storage_root() / 'Media'


def cache_data_directory(directory):
    """Route regenerable data separately from saved state and recovery files."""
    from .host_cache import cache_base
    directory = Path(directory)
    if directory.is_relative_to(media_state_root()):
        return cache_base() / directory.relative_to(media_state_root())
    return directory


def cache_directory(disk_serial, volume_serial):
    key = hashlib.sha256((disk_serial + volume_serial).encode()).hexdigest()[:24]
    return media_state_root() / key


def cache_root(identity):
    return cache_directory(identity.get("media_cache_disk_serial") or identity["disk_serial"],
                           identity["volume_serial"])


def receipt_directory(identity):
    from .host_cache import _plain
    directory = _plain(cache_root(identity) / 'receipts')
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def cache_volume_scope(root):
    """Use the host's volume identity when a Windows mount letter changes."""
    root = Path(root)
    name = str(root)
    if os.name == 'nt':
        if root == Path(root.anchor):
            mount = name
        elif name.startswith('\\\\?\\Volume{') and name.endswith('}'):
            mount = name + '\\'
        else:
            return name
        import ctypes
        volume = ctypes.create_unicode_buffer(64)
        lookup = ctypes.windll.kernel32.GetVolumeNameForVolumeMountPointW
        lookup.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_ulong)
        lookup.restype = ctypes.c_int
        if lookup(mount, volume, len(volume)):
            return volume.value
    return name


def load_json_object(path, *, encoding='utf-8'):
    """Missing, unreadable, or non-object cache state is a cache miss."""
    try:
        value = json.loads(path.read_text(encoding=encoding))
    except (OSError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def save_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '-' + uuid.uuid4().hex + '.tmp')
    owned = False
    try:
        with temporary.open('x', encoding='utf-8') as writer:
            owned = True
            writer.write(json.dumps(value, separators=(',', ':')))
            writer.flush()
            os.fsync(writer.fileno())
        # Indexers and antivirus can briefly open the existing receipt without
        # delete sharing on Windows. Keep the fsynced temporary and retry only
        # those transient sharing/access failures; other errors remain fatal.
        for attempt in range(7):
            try:
                temporary.replace(path)
                break
            except OSError as error:
                if (os.name != 'nt' or getattr(error, 'winerror', None) not in (5, 32, 33)
                        or attempt == 6):
                    raise
                time.sleep(min(0.05 * (2 ** attempt), 0.5))
    finally:
        if owned:
            temporary.unlink(missing_ok=True)
