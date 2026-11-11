"""Target media capabilities, preparation recipes, and shared operation values."""
from __future__ import annotations

from .inventory import KINDS, VIDEO_EXTENSIONS


_RECENT_OUTPUT_NS = 5_000_000_000


def supported_kinds(identity):
    """Return the media services declared by the resolved device target."""
    profile = identity.get("media_profile", {})
    return tuple(kind for kind in KINDS
                 if (kind != "Videos" or profile.get("video_enabled", True))
                 and (kind != "Photos" or profile.get("photo_enabled", True)))


def native_video_extensions(identity=None):
    profile = (identity or {}).get("media_profile", {})
    return set(profile.get("video_extensions", [".ipvf"])) if profile.get("video_enabled", True) else set()


def video_source_extensions(identity):
    profile = (identity or {}).get("media_profile", {})
    if not profile.get("video_enabled", True):
        return set()
    allowed = set(VIDEO_EXTENSIONS)
    if profile.get("video_backend") in ("bcm-mp4", "native-mp4"):
        allowed.discard(".ipvf")
    return allowed


def target_args(identity):
    return dict(disk_number=identity["disk_number"], expected_disk_serial=identity["disk_serial"],
                expected_volume_serial=identity["volume_serial"], expected_drive=identity["drive"])


def preparation_option(kind, identity, photo_dimension=640, music_verification='metadata'):
    """Shared preparation version for compilation and library readiness."""
    if kind == 'Music':
        from .aac_index import PREPARATION_VERSION
        from tools.studio.targets import TARGETS
        target = TARGETS[identity['target_id']]
        art_version = ('art-native130-playback160-exact-np-bridge1' if target.get('presentation_profile') == 'color320' else
                       'art9' if target['media_art_side'] == 128 else
                       'art8' if target['lcd_format'] == 'gray2-horizontal' else 'art7')
        if identity['target_id'] == 'video5g-m25':
            art_version += '-resident-lz4-1-index1'
        return f"{music_verification}-aac-seek{PREPARATION_VERSION}-duration-search-prefix2-browse-id-names-album-owner2-title-order1-retail2-recent-album-gain-icc-srgb1-audio-compat1-{art_version}-{identity['target_id']}"
    if kind == 'Notes':
        from .notes import _display_recipe
        return f"target-wrap2-{_display_recipe(identity['target_id'])[2]}"
    if kind == 'Photos':
        profile = identity['media_profile']
        layout = '-pack32-sequential8' if identity['target_id'] in ('video5g-m25', 'classic6g-n25') else ''
        return f"lpic-hc-photos-native-grid{layout}-jpeg-fallback2-{identity['target_id']}-{photo_dimension}-{profile['photo_width']}x{profile['photo_height']}"
    return 'default'


def format_size(value):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024 or unit == "TB":
            return f"{value:,.1f} {unit}"
        value /= 1024


class Cancelled(RuntimeError):
    pass
