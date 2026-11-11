"""Supported internal entry points for media path and device identity guards."""

from pathlib import Path

from .publish import (
    _hash as hash_file,
    _plain as guarded_path,
    _same_target as require_same_target,
    _volume_guard as guard_volume,
    inspect_target,
)


class LinnetInstallationRequired(ValueError):
    pass


def require_linnet(identity):
    """Require a complete installation on the caller's refreshed device."""
    from tools.studio.installer import installed_state

    root = Path(identity['root'])
    guarded_path(root / '.LINNET', root)
    state = installed_state(root, identity['target_id'])['state']
    if state == 'installed':
        return
    message = ('Install Linnet on this iPod first.' if state == 'absent' else
               'Finish installing Linnet on this iPod first.' if state == 'incomplete' else
               'The Linnet installation on this iPod needs attention.')
    raise LinnetInstallationRequired(message + ' Open iPod setup, then try again.')


__all__ = ("hash_file", "guarded_path", "require_same_target", "guard_volume", "inspect_target",
           "require_linnet", "LinnetInstallationRequired")
