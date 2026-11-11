"""Guarded Notes publication; retains the previous device catalog before replacement."""
from pathlib import Path
import os
from .notes import inspect, MAX_FILE
from .publish import _plain, _hash, inspect_target, _exclusive, _publish_library, _recover_pending_groups


def publish_notes(package, *, disk_number, expected_disk_serial,
                   expected_volume_serial, expected_drive=None):
    """Validate all local bytes, preserve the previous package, publish last.

    A failure after replacement is an uncertain publication; inspect the host
    receipt and retained package/backup instead of automatically replaying it.
    The iPod must remain exclusively in Disk Mode throughout the operation.
    """
    package = Path(os.path.abspath(package))
    _plain(package, Path(package.anchor))
    package = package.resolve(strict=True)
    digest = _hash(package)
    candidate = inspect(package)
    if candidate['sha256'] != digest:
        raise ValueError('notes package changed during validation')
    if _hash(package, candidate['bytes']) != digest:
        raise ValueError('notes package changed during reference validation')
    identity = inspect_target(disk_number, expected_disk_serial, expected_volume_serial, expected_drive)
    root = Path(identity['root'])
    if package.drive.casefold() == root.drive.casefold():
        raise ValueError('notes package must be staged on a different volume from the iPod')
    with _exclusive(identity):
        _recover_pending_groups(root, identity)
        return _publish(package, candidate, identity, root, disk_number,
                        expected_disk_serial, expected_volume_serial, expected_drive)


def _publish(package, candidate, identity, root, number, serial, volume, drive):
    return _publish_library(
        package, candidate, identity, root, number, serial, volume, drive,
        kind="notes", label="notes", filename="LIBRARY.NNT", max_file=MAX_FILE,
        summary=dict(notes=candidate['notes']))
