"""Apple OS folder availability on validated iPod volumes.

Selections live on the computer. Discovery never changes the iPod. The Apple
database writer runs against a staged mount and installs only a verified DB.
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import re
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import subprocess
import tempfile
import uuid
from contextlib import closing
from fractions import Fraction
from types import SimpleNamespace

from . import media_manager as media
from .apple_process import checkpoint as _checkpoint, run as _run_process
from .host_state import cache_root, cache_data_directory, file_stamp, save_json
from .inventory import file_records
from .music import _device_media_path
from .guard import guarded_path as _plain, guard_volume as _volume_guard, hash_file as _hash
from .publish import _exclusive


# These are libgpod format profiles, not a list of approved physical devices.
# All variants within each supported family share these database formats.
DEVICE_PROFILES = {
    'ipod4g-q21': ('M9282', ('Music',)),
    'photo4g-p98': ('M9585', ('Music', 'Photos')),
    'video5g-m25': ('MA146', ('Music', 'Photos', 'Videos')),
    'classic6g-n25': ('MB147', ('Music', 'Photos', 'Videos')),
}
ROOTS = ('Music', 'Photos', 'Videos', 'Notes')
SELECTABLE_ROOTS = ROOTS[:3]
APPLE_MUSIC_PREFIX = ':iPod_Control:Music:..:..:Music:'


def selectable_roots(identity):
    profile = DEVICE_PROFILES.get(identity.get('target_id'))
    return profile[1] if profile else ()


def _apple_guid(identity):
    """Use Apple's identifier, never a native Disk Mode generated serial."""
    value = ''
    if str(identity.get('usb_vid_pid', '')).upper().startswith('05AC:'):
        value = identity.get('linnet_usb_serial', '')
    else:
        from tools.inspect.retail_device import _sysinfo
        root = Path(identity['root'])
        try:
            _plain(root / 'iPod_Control/Device/SysInfo', root)
            value = _sysinfo(root).get('FirewireGuid', '')
        except (OSError, ValueError, UnicodeError):
            pass
        if not value:
            value = _load(identity).get('apple_guid', '')
    value = str(value).removeprefix('0x').removeprefix('0X').upper()
    return value if re.fullmatch(r'[0-9A-F]{16}', value) and int(value, 16) else None


def _device_profile(identity):
    """Resolve a supported format from a freshly verified mounted iPod."""
    profile = DEVICE_PROFILES.get(identity.get('target_id'))
    if (not profile or identity.get('filesystem') != 'FAT32' or
            identity.get('sector_bytes') not in (512, 2048, 4096)):
        return None
    guid = _apple_guid(identity)
    if identity['target_id'] == 'classic6g-n25' and guid is None:
        return None
    # Earlier families do not checksum iTunesDB with a device identifier.
    return {'model': profile[0], 'apple_guid': guid or '0000000000000000'}


def _eligibility(identity):
    if identity.get('target_id') not in DEVICE_PROFILES:
        return 'Apple library updates are not supported for this iPod model.'
    if identity.get('filesystem') != 'FAT32':
        return 'Apple library updates require a FAT32 iPod volume.'
    if identity.get('sector_bytes') not in (512, 2048, 4096):
        return 'This iPod has an unsupported storage sector size.'
    if identity['target_id'] == 'classic6g-n25' and _apple_guid(identity) is None:
        return ('Reconnect this Classic in Apple Disk Mode so its Apple identifier '
                'can be verified for the library checksum.')
    return 'Apple library updates are supported for this iPod.'


def supported(identity):
    return _device_profile(identity) is not None


def _apple_binding(identity):
    """Bind Apple ownership to a volume, independently of its active USB mode."""
    text_fields = ('volume_unique_id', 'volume_serial', 'target_id', 'filesystem')
    number_fields = ('disk_bytes', 'partition_offset', 'partition_bytes', 'sector_bytes')
    if (any(not isinstance(identity.get(key), str) or not identity[key]
            for key in text_fields) or
            any(type(identity.get(key)) is not int or identity[key] < 0
                for key in number_fields)):
        raise ValueError('The iPod volume identity is incomplete; reconnect it before updating Apple OS.')
    return {**{key: identity[key].casefold() for key in text_fields},
            **{key: identity[key] for key in number_fields}}


def _apple_cache(identity):
    binding = _apple_binding(identity)
    key = hashlib.sha256(json.dumps(binding, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:32]
    return cache_root(identity).parent / 'apple-os' / key


def _state_path(identity):
    return _apple_cache(identity) / 'apple-os.json'


def _read_apple_state(path):
    if path.is_symlink() or path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('The saved Apple library state is invalid.')
    value = json.loads(path.read_text(encoding='utf-8'))
    if (not isinstance(value, dict) or not isinstance(value.get('folders', []), list) or
            any(not isinstance(folder, str) for folder in value.get('folders', [])) or
            (value.get('published') is not None and not isinstance(value.get('published'), dict))):
        raise ValueError('The saved Apple library state is invalid.')
    return value


def _load(identity):
    serial = identity.get('media_cache_disk_serial') or identity['disk_serial']
    binding = {'device_serial': serial, 'volume_serial': identity['volume_serial'],
               'target_id': identity['target_id'], 'volume_binding': _apple_binding(identity)}
    try:
        value = _read_apple_state(_state_path(identity))
    except FileNotFoundError:
        pass
    else:
        if value.get('volume_binding') != binding['volume_binding']:
            raise ValueError('The saved Apple library belongs to a different volume; review it before updating.')
        value.update(binding)
        return value
    return dict(binding, folders=[], configured=False, published=None)


def _valid_folder(root, value, allowed=SELECTABLE_ROOTS):
    if not isinstance(value, str) or not value or '\\' in value:
        raise ValueError('Invalid Apple OS folder selection')
    relative = Path(value)
    if (relative.is_absolute() or relative.drive or '..' in relative.parts or
            not relative.parts or relative.parts[0] not in allowed or
            relative.as_posix() != value):
        raise ValueError('Apple OS folder is outside the media folders')
    folder = root / relative
    _plain(folder, root)
    if not folder.is_dir() or not folder.resolve(strict=True).is_relative_to(root):
        raise ValueError(f'Apple OS folder is unavailable: {value}')
    return folder


def _normalize(root, folders, allowed=SELECTABLE_ROOTS):
    values = sorted(set(folders), key=lambda x: (x.casefold(), x))
    result = []
    for value in values:
        _valid_folder(root, value, allowed)
        if not any(value.casefold() == parent.casefold() or
                   value.casefold().startswith(parent.casefold() + '/') for parent in result):
            result.append(value)
    return result


def _walk_folders(root, kinds=ROOTS):
    result = []
    for kind in kinds:
        start = root / kind
        if not start.is_dir():
            continue
        stack = [start]
        while stack:
            folder = stack.pop()
            _plain(folder, root)
            relative = folder.relative_to(root).as_posix()
            result.append({'path': relative, 'kind': kind})
            if len(result) > 10000:
                raise ValueError('Too many media folders for Apple OS selection')
            children = []
            with os.scandir(folder) as entries:
                for entry in entries:
                    if entry.is_symlink():
                        raise ValueError(f'Linked media folder is unsupported: {entry.path}')
                    if entry.is_dir(follow_symlinks=False):
                        children.append(Path(entry.path))
            stack.extend(sorted(children, key=lambda p: p.name.casefold(), reverse=True))
    return result


def _folder_rows(root, records_by_kind, kinds=ROOTS):
    paths = set()
    for kind in kinds:
        kind_root = root / kind
        if kind_root.is_dir():
            paths.add(kind)
        last_parent = None
        for path, _ in records_by_kind[kind]:
            parent = path.parent
            parent_key = str(parent)
            if parent != root and parent_key == last_parent:
                continue
            last_parent = parent_key if parent != root else None
            if not path.is_relative_to(kind_root):
                continue
            while parent.is_relative_to(kind_root):
                relative = parent.relative_to(root).as_posix()
                if relative in paths:
                    break
                paths.add(relative)
                parent = parent.parent
    return [{'path': value, 'kind': value.split('/')[0]}
            for value in sorted(paths, key=lambda x: (kinds.index(x.split('/')[0]),
                                                      len(Path(x).parts), x.casefold()))]


def _source_info(path, info, *, inode=None, volume_dev=None):
    if os.name == 'nt' and not info.st_ino:
        if inode and volume_dev is not None:
            # A shared read-only inventory already collected the file ID and
            # rejected links. Reuse it instead of issuing another USB query.
            return SimpleNamespace(st_dev=volume_dev, st_ino=inode,
                                   st_size=info.st_size, st_mtime_ns=info.st_mtime_ns,
                                   st_ctime_ns=info.st_ctime_ns)
        # Windows directory records omit the file ID needed to recognize a
        # replacement that preserves size and modification time.
        current = path.lstat()
        if (not stat.S_ISREG(current.st_mode) or
                getattr(current, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400) or
                (current.st_size, current.st_mtime_ns, current.st_ctime_ns) !=
                (info.st_size, info.st_mtime_ns, info.st_ctime_ns)):
            raise ValueError(f'Media changed while reading Apple OS sources: {path}')
        return current
    return info


def _files(root, folders, kind, extensions, records_by_kind=None):
    found = {}
    selected = [_valid_folder(root, value) for value in folders
                if value.split('/')[0] == kind]
    if not selected:
        return []
    if records_by_kind is not None:
        selected_dirs = set(selected)
        kind_root = root / kind
        whole_kind = kind_root in selected_dirs
        last_parent, relative_prefix, included = None, None, False
        for path, info in records_by_kind[kind]:
            if path.suffix.lower() not in extensions:
                continue
            parent = path.parent
            parent_key = str(parent)
            if parent == root or parent_key != last_parent:
                included = ((whole_kind and path.is_relative_to(kind_root)) or
                            any(folder in selected_dirs for folder in path.parents))
                last_parent = parent_key if parent != root else None
                relative_prefix = None
            if included:
                if relative_prefix is None:
                    relative = path.relative_to(root).as_posix()
                    relative_prefix = relative[:relative.rfind('/') + 1]
                else:
                    relative = relative_prefix + path.name
                found[relative.casefold()] = (path, info)
    else:
        for folder in selected:
            for path, info in file_records(folder, allowed=extensions):
                found[path.relative_to(root).as_posix().casefold()] = (path, info)
    inodes = getattr(records_by_kind, 'inodes', {})
    volume_dev = root.stat().st_dev if found and inodes else None
    return [(path, _source_info(path, info, inode=inodes.get(path), volume_dev=volume_dev))
            for _, (path, info) in sorted(found.items())]


def _signature(root, files):
    rows = [(path.relative_to(root).as_posix(), *file_stamp(info))
            for path, info in files]
    return hashlib.sha256(json.dumps(rows, ensure_ascii=False).encode()).hexdigest()


def _apple_music_path(path, root, aliases):
    if not path.is_relative_to(root / 'Music'):
        raise ValueError('Apple music must be under /Music')
    if path.suffix.lower() != '.mp3':
        raise ValueError(f'Apple music currently supports MP3 only: {path.relative_to(root)}')
    short = _device_media_path(path, root, aliases)
    if not short.startswith('/MUSIC/'):
        raise ValueError(f'Cannot resolve Apple music path: {path}')
    relative = short[len('/MUSIC/'):].replace('/', ':')
    if len(relative) > 23:
        raise ValueError(f'Apple path needs a shorter FAT alias: {path.relative_to(root)}')
    result = APPLE_MUSIC_PREFIX + relative[:-4] + '.mp3'
    if len(result.encode('ascii')) > 55:
        raise ValueError(f'Apple path is too long: {path.relative_to(root)}')
    return result


def _apple_video_path(path, root, aliases):
    if path.suffix.lower() not in ('.mp4', '.m4v', '.mov') or not path.is_relative_to(root / 'Videos'):
        raise ValueError('Apple video needs an MP4, M4V or MOV file under /Videos')
    short = _device_media_path(path, root, aliases)
    if not short.startswith('/VIDEOS/'):
        raise ValueError(f'Cannot resolve Apple video path: {path}')
    relative = short[len('/VIDEOS/'):].replace('/', ':')
    if len(relative) > 22:
        raise ValueError(f'Apple video path needs a shorter FAT alias: {path.relative_to(root)}')
    prefix = ':iPod_Control:Music:..:..:Videos:'
    result = prefix + relative[:-len(path.suffix)] + path.suffix.lower()
    if len(result.encode('ascii')) > 55:
        raise ValueError(f'Apple video path is too long: {path.relative_to(root)}')
    return result


def _video_probe(path, ffprobe, *, target_id='video5g-m25', cancelled=lambda: False):
    if target_id not in ('video5g-m25', 'classic6g-n25'):
        raise ValueError('Apple video playback is unavailable on this iPod model.')
    model = 'iPod Video' if target_id == 'video5g-m25' else 'iPod Classic'
    def incompatible(reason):
        return ValueError(f'Needs conversion for {model}: {reason} ({path.name})')
    if not ffprobe:
        raise ValueError('FFprobe is needed to check video for Apple OS.')
    try:
        result = _run_process([ffprobe, '-v', 'error', '-show_entries',
                                 'format=duration,bit_rate:format_tags=title,artist:stream=codec_type,codec_name,profile,width,height,level,r_frame_rate,sample_rate,channels,bit_rate',
                                 '-of', 'json', str(path)], timeout=30, cancelled=cancelled)
    except subprocess.TimeoutExpired:
        raise ValueError(f'Video inspection timed out: {path.name}') from None
    if result.returncode:
        raise ValueError(f'Could not inspect video: {path.name}')
    data = json.loads(result.stdout)
    video = next((item for item in data.get('streams', []) if item.get('codec_type') == 'video'), None)
    audio = next((item for item in data.get('streams', []) if item.get('codec_type') == 'audio'), None)
    if not video or not audio:
        raise incompatible('video and AAC audio are required')
    width, height = int(video.get('width', 0)), int(video.get('height', 0))
    if width <= 0 or height <= 0 or width > 640 or height > 480:
        raise incompatible('use dimensions no larger than 640×480')
    rate = Fraction(video.get('r_frame_rate', '0/1'))
    if rate <= 0 or rate > 30:
        raise incompatible('use 30 frames per second or less')
    codec, profile = video.get('codec_name'), video.get('profile', '')
    maximum_bitrate = 2_500_000
    if codec == 'h264':
        maximum_level = 13 if target_id == 'video5g-m25' else 30
        if profile not in ('Baseline', 'Constrained Baseline') or int(video.get('level', 99)) > maximum_level:
            raise incompatible(f'use H.264 Baseline Level {maximum_level / 10:.1f} or lower')
        if target_id == 'video5g-m25':
            # FFprobe cannot establish the later firmware's proprietary
            # 480p Low-Complexity constraints. Use the documented base profile.
            if width > 320 or height > 240:
                raise incompatible('use H.264 at 320×240 or smaller')
            maximum_bitrate = 768_000
    elif codec == 'mpeg4':
        if profile != 'Simple Profile':
            raise incompatible('use MPEG-4 Simple Profile')
    else:
        raise incompatible(f'unsupported video codec {codec}')
    bitrate = int(video.get('bit_rate') or data.get('format', {}).get('bit_rate') or 0)
    if bitrate <= 0 or bitrate > maximum_bitrate:
        raise incompatible(f'use a known video bitrate of {maximum_bitrate // 1000:,} kbps or less')
    if audio.get('codec_name') != 'aac' or audio.get('profile') != 'LC':
        raise incompatible('use AAC-LC audio')
    sample_rate, channels = int(audio.get('sample_rate', 0)), int(audio.get('channels', 0))
    if sample_rate <= 0 or sample_rate > 48000 or channels not in (1, 2):
        raise incompatible('use mono or stereo AAC at 48 kHz or less')
    # Encoder bitrates are often rounded just above the nominal 160 kbps cap.
    audio_rate = int(audio.get('bit_rate') or 0)
    if audio_rate <= 0 or audio_rate > 165_000:
        raise incompatible('use AAC audio at 160 kbps or less')
    duration = float(data.get('format', {}).get('duration', 0))
    if not math.isfinite(duration) or duration <= 0 or duration * 1000 > 2_147_483_647:
        raise ValueError(f'Apple video duration is invalid: {path.name}')
    tags = data.get('format', {}).get('tags') or {}
    return {'duration_ms': round(duration * 1000), 'bitrate_kbps': round(bitrate / 1000),
            'sample_rate': sample_rate, 'title': str(tags.get('title') or path.stem),
            'artist': str(tags.get('artist') or 'Unknown Artist')}


def inspect(identity, *, records_by_kind=None, retail_tracks=None):
    """Return folder choices and readiness without writing the device."""
    root = Path(identity['root']).resolve(strict=True)
    allowed = selectable_roots(identity)
    folder_kinds = (*allowed, 'Notes')
    if not supported(identity):
        database = root / 'iPod_Control/iTunes/iTunesDB'
        if database.is_file():
            try:
                from .retail_media import tracks
                count = len(retail_tracks) if retail_tracks is not None else len(tracks(root))
                library = f'Apple library lists {count:,} item' + ('' if count == 1 else 's') + '. '
            except (OSError, ValueError, UnicodeError, struct.error):
                library = 'Apple music database could not be read. '
        else:
            library = 'No Apple music database found. '
        return {'supported': False, 'folders': [], 'selected': [],
                'status': library.strip(), 'eligibility': _eligibility(identity),
                'details': []}
    folders = (_folder_rows(root, records_by_kind, folder_kinds) if records_by_kind is not None else
               _walk_folders(root, folder_kinds))
    library_issue = None
    if retail_tracks is None:
        from .retail_media import tracks
        try:
            retail_tracks = tracks(root,
                                   known_records=records_by_kind['Music'] if records_by_kind is not None else None,
                                   known_inodes=getattr(records_by_kind, 'inodes', None))
        except (OSError, ValueError, UnicodeError, struct.error):
            retail_tracks = {}
            library_issue = 'The existing Apple library could not be read. Review it before updating.'
    library_count = len(retail_tracks)
    from .retail_media import media_kind
    library_counts = dict(Music=0, Videos=0, Other=0)
    for track in retail_tracks.values():
        library_counts[media_kind(track)] += 1
    library_status = ' · '.join(
        f'{library_counts[kind]:,} {singular if library_counts[kind] == 1 else plural}'
        for kind, singular, plural in (('Music', 'audio track', 'audio tracks'),
                                      ('Videos', 'video', 'videos'), ('Other', 'other item', 'other items'))
        if library_counts[kind]) + ' listed in Apple OS'
    state = _load(identity)
    selected = [value for value in state.get('folders', [])
                if isinstance(value, str) and value.split('/')[0] in allowed and (root / value).is_dir()]
    music = _files(root, selected, 'Music', media.extensions('Music'), records_by_kind)
    published = state.get('published') if isinstance(state.get('published'), dict) else {}
    db = root / 'iPod_Control/iTunes/iTunesDB'
    source_stamp = _signature(root, music)
    digest = _hash(db) if (selected or published) and db.is_file() else None
    # A path listed in Apple's database does not prove its metadata matches
    # today's file. Only an explicit publication establishes source freshness.
    verified_music = isinstance(published.get('managed_paths'), list)
    dependencies_ready = verified_music and library_outputs_ready(identity, published)
    if (published.get('library_outputs') and published.get('database_sha256') == digest and
            not dependencies_ready):
        library_issue = ('Apple artwork files changed or are missing. Restore the Apple library '
                         'from its backup or sync it again before updating.')
    source_matches = published.get('music_source_signature') == source_stamp
    same_published = (verified_music and source_matches and
                      published.get('database_sha256') == digest and
                      isinstance(published.get('music_count'), int))
    if same_published:
        # A verified, unchanged selection needs no thousands of FAT alias lookups.
        issues = published.get('music_issues', [])
        playable_count = published['music_count']
        stamp = published.get('music_signature')
    else:
        aliases, issues, playable_music = {}, [], []
        for path, info in music:
            try:
                if info.st_size <= 0 or info.st_size > 0xffffffff:
                    raise ValueError(f'Apple music file size is unsupported: {path.relative_to(root)}')
                _apple_music_path(path, root, aliases)
                playable_music.append((path, info))
            except (OSError, ValueError) as error:
                if len(issues) < 20:
                    issues.append(str(error))
        playable_count = len(playable_music)
        stamp = _signature(root, playable_music)
    if issues and same_published:
        music_status = f'{len(music) - playable_count} music paths need attention'
    elif issues:
        music_status = 'Needs update'
    elif not any(value.split('/')[0] == 'Music' for value in selected) and published.get('music_count', 0):
        music_status = 'Needs update'
    elif not any(value.split('/')[0] == 'Music' for value in selected):
        music_status = 'No music folders selected'
    else:
        if same_published:
            signature_matches = published.get('music_signature') == stamp
        elif (published.get('music_signature') == published.get('music_source_signature') and
              playable_count == len(music) and stamp == source_stamp):
            # The playable subset is the full source list, so the first
            # signature comparison already checked these same files.
            signature_matches = source_matches
        else:
            signature_matches = published.get('music_signature') == stamp
        music_status = ('Ready' if verified_music and dependencies_ready and signature_matches and
                        published.get('database_sha256') == digest else 'Needs update')
    details = [{'kind': 'Music', 'count': len(music), 'status': music_status}]
    if library_count:
        details.insert(0, {'kind': 'Apple library', 'count': library_count,
                           'status': library_status})
    photos = _files(root, selected, 'Photos', media.extensions('Photos'), records_by_kind)
    photo_db = root / 'Photos/Photo Database'
    photo_digest = (_hash(photo_db) if (photos or published.get('photo_count', 0)) and
                    photo_db.is_file() else None)
    if photos:
        if photo_digest and published.get('photo_database_sha256') not in (None, photo_digest):
            photo_status = 'Apple photo library changed outside Linnet'
        elif photo_digest and not published.get('photo_database_sha256'):
            photo_status = 'Existing Apple photo library needs review'
        elif (published.get('photo_signature') == _signature(root, photos) and
              published.get('photo_database_sha256') == photo_digest and
              photo_outputs_ready(identity, published)):
            photo_status = 'Ready'
        else:
            photo_status = 'Needs update'
        details.append({'kind': 'Photos', 'count': len(photos), 'status': photo_status})
    elif published.get('photo_count', 0):
        photo_status = 'Needs update'
        details.append({'kind': 'Photos', 'count': 0, 'status': photo_status})
    else:
        photo_status = 'No photo folders selected'
    note_issues = 0
    if (root / 'Notes').is_dir():
        notes = (records_by_kind['Notes'] if records_by_kind is not None else
                 file_records(root / 'Notes'))
        notes = [(path, info) for path, info in notes
                 if path.is_relative_to(root / 'Notes') and path.name.casefold() != 'instructions']
        native = [(path, info) for path, info in notes
                  if path.suffix.lower() == '.txt' and info.st_size <= 4096]
        note_issues = len(notes) - len(native)
        note_status = ('Apple Notes reads .txt files here automatically' if len(native) == len(notes) else
                       f'{len(notes) - len(native)} notes need a short plain-text file')
        details.append({'kind': 'Notes', 'count': len(notes), 'status': note_status})
    videos = _files(root, selected, 'Videos', media.extensions('Videos'), records_by_kind)
    video_stamp = _signature(root, videos)
    video_matches = published.get('video_source_signature') == video_stamp
    saved_video_issues = (published.get('video_issues', [])
                          if video_matches and
                          isinstance(published.get('video_issues'), list) else [])
    if videos or published.get('video_count', 0):
        if not verified_music or not dependencies_ready or not video_matches or published.get('database_sha256') != digest:
            video_status = 'Needs update'
        elif saved_video_issues:
            video_status = f'{len(saved_video_issues)} videos need attention'
        else:
            video_status = 'Ready'
        details.append({'kind': 'Videos', 'count': len(videos), 'status': video_status})
        issues.extend(str(value) for value in saved_video_issues[:max(0, 20 - len(issues))])
    else:
        video_status = 'No video folders selected'
    photo_conflict = photo_status.startswith(('Apple photo library', 'Existing Apple photo'))
    recovery_issue = pending_recovery(identity)
    can_update = not photo_conflict and not library_issue and not recovery_issue and (any(value == 'Needs update' for value in
                                             (music_status, photo_status, video_status)) or
                                         bool(issues))
    if recovery_issue:
        issues.insert(0, recovery_issue)
        status = 'Needs attention'
    elif library_issue:
        issues.insert(0, library_issue)
        status = 'Needs attention'
    elif photo_conflict:
        status = 'Needs attention'
    elif any(value == 'Needs update' for value in (music_status, photo_status, video_status)):
        status = 'Apple library needs updating'
    elif issues or note_issues:
        status = 'Needs attention'
    elif selected:
        status = 'Ready in Apple OS'
    elif library_count:
        status = library_status
    else:
        status = 'Choose folders to make available in Apple OS'
    return {'supported': True, 'folders': folders, 'selected': selected,
            'status': status, 'details': details, 'issues': issues,
             'can_update': can_update, 'eligibility': _eligibility(identity),
             'selectable_roots': allowed}


def save_selection(identity, folders):
    identity = media.refresh(identity)
    if not supported(identity):
        raise ValueError(_eligibility(identity))
    root = Path(identity['root']).resolve(strict=True)
    selected = _normalize(root, folders, selectable_roots(identity))
    state = _load(identity)
    state['folders'] = selected
    state['configured'] = True
    guid = _apple_guid(identity)
    if guid:
        state['apple_guid'] = guid
    save_json(_state_path(identity), state)
    return selected


def _run_writer(arguments, *, timeout=120, cancelled=lambda: False):
    result = _run_process(arguments, timeout=timeout, cancelled=cancelled)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or 'Apple database tool failed')
    return result


def _writer(cache, kind='music'):
    from tools.studio.apple_tools import writer
    return writer(kind, cache_data_directory(cache))


def _revalidate_device(identity, *paths):
    """Check the full disk identity and mounted volume before a device mutation."""
    media.refresh(identity)
    _volume_guard(identity)
    root = Path(identity['root'])
    for path in paths:
        _plain(path, root)


def _publication_checkpoint(cancelled):
    if cancelled():
        raise media.Cancelled('Stopped preparing Apple OS media.')


def _verify_sources(root, files, cancelled):
    for path, original in files:
        _publication_checkpoint(cancelled)
        _plain(path, root)
        current = path.lstat()
        if not stat.S_ISREG(current.st_mode) or file_stamp(current) != file_stamp(original):
            raise ValueError(f'Media changed during Apple OS preparation: {path.relative_to(root)}')


def _copy_contents(source, destination, cancelled):
    with source.open('rb') as reader:
        while True:
            _publication_checkpoint(cancelled)
            block = reader.read(1024 * 1024)
            if not block:
                break
            destination.write(block)
    destination.flush()
    os.fsync(destination.fileno())


def _durable_copy(source, target, expected):
    """Create an owned copy, persist its bytes, then verify its complete contents."""
    with target.open('xb') as destination:
        _copy_contents(source, destination, lambda: False)
    if _hash(target) != expected:
        raise OSError(f'Apple media copy failed readback: {target}')


def _verify_published(path, expected):
    with path.open('r+b') as saved:
        os.fsync(saved.fileno())
    if _hash(path) != expected:
        raise OSError(f'Apple media publication failed readback: {path}')


def _outputs_ready(identity, outputs, *, cache_name, prefixes, required,
                   cancelled=lambda: False):
    """Verify complete output receipts, rehashing only files whose stamps changed."""
    if not isinstance(outputs, list) or not outputs or len(outputs) > 10000:
        return False
    root = Path(identity['root']).resolve(strict=True)
    cache_path = cache_data_directory(_apple_cache(identity)) / cache_name
    scope = [str(root), identity.get('volume_unique_id'), identity.get('target_id')]
    cached = {}
    try:
        if cache_path.stat().st_size <= 8 * 1024 * 1024:
            value = json.loads(cache_path.read_text(encoding='utf-8'))
            if (isinstance(value, dict) and value.get('version') == 1 and
                    value.get('scope') == scope and isinstance(value.get('outputs'), dict)):
                cached = value['outputs']
    except (OSError, ValueError):
        pass
    verified = {}
    changed = False
    seen = set()
    try:
        for row in outputs:
            _publication_checkpoint(cancelled)
            if not isinstance(row, dict) or set(row) != {'path', 'bytes', 'sha256'}:
                return False
            name, length, expected = row['path'], row['bytes'], row['sha256']
            if (not isinstance(name, str) or '\\' in name or ':' in name or
                    type(length) is not int or length < 0 or
                    not isinstance(expected, str) or len(expected) != 64 or
                    any(char not in '0123456789abcdef' for char in expected)):
                return False
            relative = Path(name)
            if (relative.is_absolute() or relative.as_posix() != name or
                    not any(relative.parts[:len(prefix)] == prefix for prefix in prefixes) or
                    any(part in ('.', '..') for part in relative.parts) or
                    name.casefold() in seen):
                return False
            seen.add(name.casefold())
            path = root / relative
            _plain(path, root)
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_size != length:
                return False
            stamp = list(file_stamp(info))
            saved = cached.get(name)
            if (not isinstance(saved, dict) or saved.get('stamp') != stamp or
                    not isinstance(saved.get('sha256'), str) or len(saved['sha256']) != 64 or
                    any(char not in '0123456789abcdef' for char in saved['sha256'])):
                digest = _hash(path)
                if list(file_stamp(path.lstat())) != stamp:
                    return False
                changed = True
            else:
                digest = saved['sha256']
            verified[name] = {'stamp': stamp, 'sha256': digest}
            if digest != expected:
                return False
        return required.casefold() in seen
    except (OSError, ValueError, TypeError):
        return False
    finally:
        if changed:
            try:
                save_json(cache_path, {'version': 1, 'scope': scope, 'outputs': verified})
            except OSError:
                pass  # A host cache failure never establishes device readiness.


def photo_outputs_ready(identity, published, *, cancelled=lambda: False):
    return _outputs_ready(identity, published.get('photo_outputs'),
                          cache_name='apple-photo-verified.json', prefixes=(('Photos',),),
                          required='Photos/Photo Database', cancelled=cancelled)


def library_outputs_ready(identity, published, *, cancelled=lambda: False):
    outputs = published.get('library_outputs')
    if not isinstance(outputs, list) or not outputs or len(outputs) > 10000:
        return False
    root = Path(identity['root']).resolve(strict=True)
    artwork = root / 'iPod_Control/Artwork'
    try:
        _plain(artwork, root)
        present = set()
        if artwork.exists():
            if not artwork.is_dir():
                return False
            for path in artwork.iterdir():
                _publication_checkpoint(cancelled)
                _plain(path, root)
                if not path.is_file():
                    return False
                present.add(path.relative_to(root).as_posix().casefold())
                if len(present) > 10000:
                    return False
        recorded = {row['path'].casefold() for row in outputs
                    if isinstance(row, dict) and isinstance(row.get('path'), str) and
                    row['path'].casefold().startswith('ipod_control/artwork/')}
        if present != recorded:
            return False
    except (OSError, ValueError):
        return False
    return _outputs_ready(identity, outputs,
                          cache_name='apple-library-verified.json',
                          prefixes=(('iPod_Control', 'iTunes'), ('iPod_Control', 'Artwork')),
                          required='iPod_Control/iTunes/iTunesDB', cancelled=cancelled)


def _library_output_receipts(inputs, prepared, removed):
    """Include unchanged artwork dependencies as well as newly written files."""
    remaining = {name: value for name, value in inputs.items() if name not in removed}
    remaining.update(prepared)
    return [dict(path=name, bytes=value['bytes'], sha256=value['sha256'])
            for name, value in sorted(remaining.items())
            if name.casefold() == 'ipod_control/itunes/itunesdb' or
            name.casefold().startswith('ipod_control/artwork/')]


def _owned_music_paths(published):
    paths = published.get('managed_paths', [])
    prefixes = (APPLE_MUSIC_PREFIX, ':iPod_Control:Music:..:..:Videos:')
    if (not isinstance(paths, list) or len(paths) > 100000 or
            any(not isinstance(path, str) or not path.startswith(prefixes) or
                len(path) > 55 or not path.isascii() or
                any(char in path for char in '\r\n\t\\') for path in paths)):
        raise ValueError('Saved Apple media ownership record is invalid.')
    return paths


def pending_recovery(identity):
    """Surface interrupted transactions before another writer reads mixed files."""
    cache = _apple_cache(identity)
    caches = {cache, cache_root(identity)}
    count = 0
    for directory in (directory / folder for directory in sorted(caches)
                      for folder in ('apple-database-recovery', 'apple-photo-recovery')):
        if not directory.is_dir():
            continue
        for path in directory.glob('*/recovery.json'):
            count += 1
            if count > 4096:
                return f'Apple library recovery records need review at {directory} before updating.'
            try:
                if path.is_symlink() or path.stat().st_size > 16 * 1024 * 1024:
                    return f'Apple library recovery information needs review at {path} before updating.'
                record = json.loads(path.read_text(encoding='utf-8'))
                if (not isinstance(record, dict) or
                        record.get('volume_serial') != identity['volume_serial'] or
                        record.get('target_id', identity['target_id']) != identity['target_id']):
                    continue
                saved_binding = record.get('volume_binding')
                if saved_binding is not None and saved_binding != _apple_binding(identity):
                    continue
                if record.get('status') in ('in_progress', 'needs_manual_recovery', 'published_pending_state',
                                             'published_without_host_state'):
                    return f'An interrupted Apple library update needs recovery. Review {path} before updating Apple OS.'
            except (OSError, ValueError):
                return f'Apple library recovery information could not be read at {path}. Review it before updating.'
    return None


def _publish_library(identity, inputs, prepared, removed, verify_inputs, cache, cancelled):
    """Publish a complete staged library with durable originals and rollback."""
    root = Path(identity['root']).resolve(strict=True)
    database = 'iPod_Control/iTunes/iTunesDB'
    names = [name for name, row in prepared.items()
             if inputs.get(name, {}).get('sha256') != row['sha256']]
    names.sort(key=lambda name: (name.casefold() == database.casefold(), name))
    names.extend(name for name in removed if name not in names)
    backup = cache / 'apple-database-recovery' / uuid.uuid4().hex
    backup.mkdir(parents=True)
    recovery = backup / 'recovery.json'
    record = dict(kind='Apple library', target_id=identity['target_id'],
                  disk_serial=identity['disk_serial'],
                  volume_serial=identity['volume_serial'],
                  volume_unique_id=identity['volume_unique_id'],
                  volume_binding=_apple_binding(identity),
                  device_root=str(root), status='in_progress', files=[])
    for name in names:
        relative = Path(name)
        if (relative.is_absolute() or relative.as_posix() != name or
                any(part in ('.', '..') for part in relative.parts) or
                relative.parts[:2] not in (('iPod_Control', 'iTunes'), ('iPod_Control', 'Artwork'))):
            raise ValueError('Prepared Apple library has an unsafe output path.')
        previous = inputs.get(name)
        record['files'].append(dict(path=name,
                                   previous_sha256=previous['sha256'] if previous else None,
                                   prepared_sha256=prepared.get(name, {}).get('sha256'),
                                   backup_path=str(backup / relative)))
    needed = sum(inputs[name]['bytes'] for name in names if name in inputs)
    if needed + 1024 * 1024 > shutil.disk_usage(cache).free:
        raise ValueError('Not enough computer space to retain the previous Apple library.')
    copies = [prepared[name]['bytes'] for name in names if name in prepared]
    new_bytes = sum(prepared[name]['bytes'] for name in names
                    if name in prepared and name not in inputs)
    if new_bytes + max(copies, default=0) + 1024 * 1024 > shutil.disk_usage(root).free:
        raise ValueError('Not enough iPod space to publish the Apple library.')
    changed = []
    temporary = None
    temporary_created = False
    with _exclusive(identity):
        _publication_checkpoint(cancelled)
        _revalidate_device(identity, root / database)
        verify_inputs()
        # Every backup must be durable before replacing the first device file.
        for row in record['files']:
            _publication_checkpoint(cancelled)
            if row['previous_sha256'] is not None:
                target, prior = root / row['path'], Path(row['backup_path'])
                prior.parent.mkdir(parents=True, exist_ok=True)
                _durable_copy(target, prior, row['previous_sha256'])
        verify_inputs()
        save_json(recovery, record)
        try:
            for row in record['files']:
                _publication_checkpoint(cancelled)
                name, expected, old_hash = row['path'], row['prepared_sha256'], row['previous_sha256']
                target = root / name
                _revalidate_device(identity, target)
                actual = _hash(target) if target.is_file() else None
                if actual != old_hash or target.exists() and not target.is_file():
                    raise ValueError(f'Apple library changed during publication: {name}')
                if expected is None:
                    target.unlink()
                    changed.append(row)
                    continue
                _revalidate_device(identity, target.parent)
                target.parent.mkdir(parents=True, exist_ok=True)
                temporary = target.with_name(target.name + '.linnet-tmp')
                _revalidate_device(identity, temporary)
                if temporary.exists():
                    raise ValueError(f'An interrupted Apple library update needs attention: {temporary}')
                with temporary.open('xb') as destination:
                    temporary_created = True
                    _copy_contents(prepared[name]['path'], destination, cancelled)
                if _hash(temporary) != expected:
                    raise OSError(f'Apple library copy failed readback: {name}')
                _revalidate_device(identity, target, temporary)
                _publication_checkpoint(cancelled)
                if (_hash(target) if target.is_file() else None) != old_hash:
                    raise ValueError(f'Apple library changed during publication: {name}')
                temporary.replace(target)
                temporary_created = False
                changed.append(row)
                _verify_published(target, expected)
            record['status'] = 'published_pending_state'
            save_json(recovery, record)
        except Exception as failure:
            try:
                if temporary_created:
                    _revalidate_device(identity, temporary)
                    temporary.unlink(missing_ok=True)
                # Restore database dependencies and consumed sidecars first.
                for row in sorted(changed, key=lambda item: item['path'].casefold() == database.casefold()):
                    target = root / row['path']
                    _revalidate_device(identity, target)
                    current = _hash(target) if target.is_file() else None
                    if current != row['prepared_sha256']:
                        raise RuntimeError(f'Apple library changed after publication: {target}')
                    if row['previous_sha256'] is None:
                        target.unlink()
                        continue
                    prior = Path(row['backup_path'])
                    if not prior.is_file() or _hash(prior) != row['previous_sha256']:
                        raise RuntimeError(f'Apple library backup is incomplete: {prior}')
                    rollback = target.with_name(target.name + '.linnet-rollback-tmp')
                    _revalidate_device(identity, rollback)
                    if rollback.exists():
                        raise RuntimeError(f'An earlier rollback needs attention: {rollback}')
                    _durable_copy(prior, rollback, row['previous_sha256'])
                    _revalidate_device(identity, target, rollback)
                    rollback.replace(target)
                    _verify_published(target, row['previous_sha256'])
            except Exception as stopped:
                record['status'] = 'needs_manual_recovery'
                try:
                    save_json(recovery, record)
                except OSError:
                    pass
                raise _recovery_error('library', failure, recovery, stopped) from failure
            record['status'] = 'rolled_back'
            save_json(recovery, record)
            raise
    return recovery, record


def _recovery_error(kind, failure, recovery, stopped):
    return RuntimeError(
        f'Apple {kind} update failed ({failure}); automatic recovery stopped ({stopped}). '
        f'Recovery information is at {recovery}. Reconnect the original iPod, inspect the '
        'listed device paths and saved originals, then restore or retry only after verifying its identity.'
    )


def _update_photos(identity, state, selected, cache, progress, *, cancelled=lambda: False):
    _publication_checkpoint(cancelled)
    device = _device_profile(identity)
    if device is None:
        raise ValueError(_eligibility(identity))
    root = Path(identity['root']).resolve(strict=True)
    photos = _files(root, selected, 'Photos', media.extensions('Photos'))
    published = state.get('published') or {}
    if not photos and not published.get('photo_count', 0):
        return
    photo_db = root / 'Photos/Photo Database'
    _plain(photo_db, root)
    old_digest = _hash(photo_db) if photo_db.is_file() else None
    if old_digest and old_digest != published.get('photo_database_sha256'):
        raise ValueError('The Apple photo library changed outside Linnet. Review it before updating Photos.')
    if (published.get('photo_signature') == _signature(root, photos) and
            published.get('photo_database_sha256') == old_digest and
            photo_outputs_ready(identity, published, cancelled=cancelled)):
        return
    source_bytes = max((info.st_size for _, info in photos), default=0)
    work_bytes = max(64 * 1024 * 1024, len(photos) * 256 * 1024) if photos else 1024 * 1024
    data_cache = cache_data_directory(cache)
    data_cache.mkdir(parents=True, exist_ok=True)
    if source_bytes + work_bytes > min(shutil.disk_usage(cache).free, shutil.disk_usage(data_cache).free):
        raise ValueError('Not enough computer space to prepare these Apple photos.')
    progress(f'Preparing {len(photos):,} photos for Apple OS…')
    writer = _writer(cache, 'photos')
    with tempfile.TemporaryDirectory(prefix='linnet-apple-photos-', dir=data_cache) as directory:
        stage = Path(directory)
        from .apple_library import stage_sysinfo
        stage_sysinfo(root, stage, device['model'], '0x' + device['apple_guid'])
        source = stage / 'source'
        source.mkdir()
        from .apple_photo_cache import NormalizedPhotos
        lines = []
        with NormalizedPhotos(cache_data_directory(cache), cancelled=cancelled) as normalized:
            for index, (path, info) in enumerate(photos, 1):
                _publication_checkpoint(cancelled)
                if index % 32 == 1 or index == len(photos):
                    progress(f'Preparing Apple OS photo {index:,} of {len(photos):,}…')
                destination = source / f'{index:06}.png'
                normalized.prepare(path, info, root, destination, work_bytes=work_bytes)
                lines.append(str(destination))
        manifest = stage / 'photos.txt'
        manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        _run_writer([writer, stage, manifest, device['model'], '0x' + device['apple_guid']],
                    timeout=max(300, len(photos) * 2), cancelled=cancelled)
        prepared = stage / 'Photos/Photo Database'
        if not prepared.is_file() or prepared.stat().st_size < 128:
            raise ValueError('Apple photo writer produced no valid database.')
        from .retail_media import image_count
        visible = image_count(stage)
        if visible != len(photos):
            raise ValueError(f'Apple photo database contains {visible} images, expected {len(photos)}.')
        outputs = [p for p in (stage / 'Photos').rglob('*') if p.is_file() and p != prepared]
        if photos and not outputs:
            raise ValueError('Apple photo thumbnails are missing.')
        outputs.append(prepared)  # Publish the database last.
        backup = cache / 'apple-photo-recovery' / uuid.uuid4().hex
        backup.mkdir(parents=True)
        recovery = backup / 'recovery.json'
        prepared_hashes = {source_file.relative_to(stage): _hash(source_file)
                           for source_file in outputs}
        output_receipts = [dict(path=source_file.relative_to(stage).as_posix(),
                                bytes=source_file.stat().st_size,
                                sha256=prepared_hashes[source_file.relative_to(stage)])
                           for source_file in outputs]
        changed = []
        temporary = None
        temporary_created = False
        with _exclusive(identity):
            _publication_checkpoint(cancelled)
            _revalidate_device(identity, photo_db)
            _verify_sources(root, photos, cancelled)
            current_photo_digest = _hash(photo_db) if photo_db.is_file() else None
            if current_photo_digest != old_digest:
                raise ValueError('The Apple photo library changed during preparation. Review it before retrying.')
            previous_hashes = {}
            for relative in prepared_hashes:
                target = root / relative
                _plain(target, root)
                if target.exists() and not target.is_file():
                    raise ValueError(f'An Apple photo library path is occupied by a folder: {target}')
                previous_hashes[relative] = _hash(target) if target.is_file() else None
            if previous_hashes[photo_db.relative_to(root)] != old_digest:
                raise ValueError('The Apple photo library changed during preparation. Review it before retrying.')
            replacements = [source_file for source_file in outputs
                            if previous_hashes[source_file.relative_to(stage)] !=
                            prepared_hashes[source_file.relative_to(stage)]]
            existing = [(source_file, root / source_file.relative_to(stage)) for source_file in replacements]
            backup_bytes = sum(target.stat().st_size for _, target in existing if target.is_file())
            if backup_bytes + 1024 * 1024 > shutil.disk_usage(cache).free:
                raise ValueError('Not enough computer space to retain the previous Apple photo library.')
            new_bytes = sum(source_file.stat().st_size for source_file, target in existing if not target.exists())
            largest_replacement = max((source_file.stat().st_size for source_file, target in existing
                                       if target.exists()), default=0)
            if new_bytes + largest_replacement + 1024 * 1024 > shutil.disk_usage(root).free:
                raise ValueError('Not enough iPod space for the Apple photo library.')
            record = {'kind': 'Apple photos', 'target_id': identity['target_id'],
                      'disk_serial': identity['disk_serial'],
                      'volume_serial': identity['volume_serial'],
                      'volume_unique_id': identity['volume_unique_id'],
                      'volume_binding': _apple_binding(identity),
                      'device_root': str(root), 'status': 'in_progress',
                      'files': [{'path': relative.as_posix(),
                                 'previous_exists': previous_hashes[relative] is not None,
                                 'previous_sha256': previous_hashes[relative],
                                 'prepared_sha256': digest,
                                 'backup_path': str(backup / relative)}
                                for relative, digest in prepared_hashes.items()
                                if previous_hashes[relative] != digest]}
            save_json(recovery, record)
            try:
                for source_file in replacements:
                    _publication_checkpoint(cancelled)
                    relative = source_file.relative_to(stage)
                    target = root / relative
                    _plain(target, root)
                    _revalidate_device(identity, target.parent)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    prior = backup / relative
                    old_hash = previous_hashes[relative]
                    if old_hash is not None:
                        if not target.is_file() or _hash(target) != old_hash:
                            raise ValueError(f'Apple photo changed during preparation: {relative}')
                        prior.parent.mkdir(parents=True, exist_ok=True)
                        _durable_copy(target, prior, old_hash)
                        if _hash(target) != old_hash:
                            raise ValueError(f'Apple photo changed while backing up: {relative}')
                    elif target.exists():
                        raise ValueError(f'Apple photo appeared during preparation: {relative}')
                    temporary = target.with_name(target.name + '.linnet-tmp')
                    if temporary.exists():
                        raise ValueError('An interrupted Apple photo update needs attention.')
                    expected = prepared_hashes[relative]
                    _revalidate_device(identity, temporary)
                    with temporary.open('xb') as destination:
                        temporary_created = True
                        _copy_contents(source_file, destination, cancelled)
                    if _hash(temporary) != expected:
                        raise ValueError(f'Apple photo readback failed: {relative}')
                    _revalidate_device(identity, target, temporary)
                    if old_hash is None:
                        if target.exists():
                            raise ValueError(f'Apple photo appeared during publication: {relative}')
                    elif not target.is_file() or _hash(target) != old_hash:
                        raise ValueError(f'Apple photo changed during publication: {relative}')
                    _revalidate_device(identity, target, temporary)
                    _publication_checkpoint(cancelled)
                    if target == photo_db:
                        _verify_sources(root, photos, cancelled)
                    temporary.replace(target)
                    temporary_created = False
                    changed.append((target, prior, old_hash, expected))
                    _verify_published(target, expected)
                _verify_sources(root, photos, cancelled)
                if not photo_outputs_ready(identity, {'photo_outputs': output_receipts}, cancelled=cancelled):
                    raise OSError('Apple photo outputs changed during publication.')
                record['status'] = 'published_pending_state'
                save_json(recovery, record)
            except Exception as failure:
                try:
                    if temporary_created:
                        _revalidate_device(identity, temporary)
                        temporary.unlink(missing_ok=True)
                    # Restore thumbnails before publishing the old database.
                    for target, prior, old_hash, expected in sorted(
                            changed, key=lambda item: item[0] == photo_db):
                        _revalidate_device(identity, target)
                        if not target.is_file() or _hash(target) != expected:
                            raise RuntimeError(f'Apple photo changed after publication: {target}')
                        if old_hash is None:
                            _revalidate_device(identity, target)
                            target.unlink()
                        else:
                            if not prior.is_file() or _hash(prior) != old_hash:
                                raise RuntimeError(f'Apple photo backup is incomplete: {prior}')
                            rollback_temp = target.with_name(target.name + '.linnet-rollback-tmp')
                            _plain(rollback_temp, root)
                            if rollback_temp.exists():
                                raise RuntimeError(f'An earlier rollback needs attention: {rollback_temp}')
                            _revalidate_device(identity, rollback_temp)
                            _durable_copy(prior, rollback_temp, old_hash)
                            _revalidate_device(identity, target, rollback_temp)
                            rollback_temp.replace(target)
                            _verify_published(target, old_hash)
                except Exception as stopped:
                    record['status'] = 'needs_manual_recovery'
                    try:
                        save_json(recovery, record)
                    except OSError:
                        pass
                    raise _recovery_error('photo', failure, recovery, stopped) from failure
                record['status'] = 'rolled_back'
                save_json(recovery, record)
                raise
        published.update(photo_signature=_signature(root, photos),
                         photo_database_sha256=_hash(prepared),
                         photo_count=len(photos),
                         photo_outputs=output_receipts)
        state['published'] = published
        try:
            save_json(_state_path(identity), state)
            record['status'] = 'published'
            save_json(recovery, record)
        except OSError as error:
            record['status'] = 'published_without_host_state'
            try:
                save_json(recovery, record)
            except OSError:
                pass
            raise RuntimeError(f'Apple photos were published, but their host state could not be saved. '
                               f'Inspect recovery information at {recovery} before retrying.') from error


def _metadata(identity, files, *, ffprobe=None, cancelled=lambda: False,
              progress=lambda text: None):
    from .music import _MetadataCache, _read_metadata, CODECS
    _checkpoint(cancelled)
    root = Path(identity['root']).resolve(strict=True)
    database = cache_data_directory(cache_root(identity)) / 'music-metadata.sqlite3'
    result = []
    aliases = {}
    probe = ffprobe or shutil.which('ffprobe')
    with closing(_MetadataCache(database, source_root=root,
                                source_scope=identity.get('media_cache_volume_scope'))) as cache:
        for index, (path, info) in enumerate(files):
            _checkpoint(cancelled)
            if index % 128 == 0:
                window = [item.relative_to(root).as_posix() for item, _ in files[index:index + 128]]
                prefetched = cache.prefetch(root, window)
            relative = path.relative_to(root).as_posix()
            current = path.lstat()
            if file_stamp(current) != file_stamp(info):
                raise ValueError(f'Music changed during Apple OS preparation: {relative}')
            details = cache.get(root, relative, current, prefetched=prefetched)
            cached = details is not None
            if index % 32 == 0 or not cached or index + 1 == len(files):
                progress(f'Checking Apple OS song {index + 1:,} of {len(files):,}…')
            if not cached:
                # Existing readers use MP3 headers/tags and probe only when
                # needed. This also warms the ordinary Linnet metadata cache.
                details = _read_metadata(path, probe, True, cancelled)
                if file_stamp(path.lstat()) != file_stamp(current):
                    raise ValueError(f'Music changed during Apple OS inspection: {relative}')
                cache.put(root, relative, current, details)
            if (details.get('codec') != CODECS['mp3'] or details.get('channels') not in (1, 2) or
                    not 0 < details.get('rate', 0) <= 48000 or details.get('frames', 0) <= 0):
                raise ValueError(f'Apple music requires a valid mono or stereo MP3: {relative}')
            tags = details.get('tags') or {}
            def tag(name, fallback=''):
                return str(tags.get(name) or fallback).replace('\x00', '').strip()
            def number(value):
                first = str(value or '').split('/')[0]
                return int(first) if first.isdecimal() else 0
            def encoded(value):
                return base64.b64encode(value.encode('utf-8')).decode('ascii')
            artist = tag('artist', 'Unknown Artist')
            result.append([_apple_music_path(path, root, aliases), info.st_size,
                           round(details['frames'] * 1000 / details['rate']),
                           round(details['data_bytes'] * 8 * details['rate'] / details['frames'] / 1000),
                           details['rate'], number(tags.get('tracknumber') or tags.get('track')),
                           number(tags.get('discnumber') or tags.get('disc')),
                           encoded(tag('title', path.stem)), encoded(artist),
                           encoded(tag('album', 'Unknown Album')),
                           encoded(tag('albumartist', tags.get('album_artist') or artist)), encoded(tag('genre'))])
    return result


def _video_rows(identity, files, ffprobe, *, cancelled=lambda: False,
                progress=lambda text: None):
    _checkpoint(cancelled)
    if not files:
        return [], []
    root = Path(identity['root']).resolve(strict=True)
    rows, issues = [], []
    aliases = {}
    cache_path = cache_data_directory(_apple_cache(identity)) / 'apple-video-probes.json'
    cached = {}
    try:
        if cache_path.stat().st_size <= 4 * 1024 * 1024:
            saved = json.loads(cache_path.read_text(encoding='utf-8'))
            if saved.get('version') == 2 and isinstance(saved.get('entries'), dict):
                cached = dict(list(saved['entries'].items())[-2048:])
    except (OSError, ValueError, AttributeError):
        pass
    # The probe policy and exact tool are part of each key. Successful results
    # can be reused when music changes without reading every video again.
    tool_key = None
    if ffprobe:
        try:
            tool = Path(shutil.which(str(ffprobe)) or ffprobe).resolve(strict=True)
            tool_key = [str(tool), *file_stamp(tool.stat())]
        except OSError:
            pass
    changed = False
    def encoded(value):
        return base64.b64encode(str(value).encode('utf-8')).decode('ascii')
    for index, (path, info) in enumerate(files, 1):
        _checkpoint(cancelled)
        progress(f'Checking Apple OS video {index:,} of {len(files):,}…')
        try:
            if info.st_size <= 0 or info.st_size > 0xffffffff:
                raise ValueError(f'Apple video file size is unsupported: {path.relative_to(root)}')
            location = _apple_video_path(path, root, aliases)
            key = hashlib.sha256(json.dumps([2, identity.get('target_id'),
                identity.get('model_number'), path.relative_to(root).as_posix().casefold(),
                file_stamp(info), tool_key]).encode()).hexdigest() if tool_key else None
            details = cached.get(key)
            if (not isinstance(details, dict) or
                    set(details) != {'duration_ms', 'bitrate_kbps', 'sample_rate', 'title', 'artist'} or
                    any(type(details.get(field)) is not int or not 0 < details[field] <= limit
                        for field, limit in (('duration_ms', 2_147_483_647),
                                             ('bitrate_kbps', 2500), ('sample_rate', 48000))) or
                    any(not isinstance(details.get(field), str) for field in ('title', 'artist'))):
                details = _video_probe(path, ffprobe, target_id=identity['target_id'], cancelled=cancelled)
                if file_stamp(path.lstat()) != file_stamp(info):
                    raise ValueError(f'Video changed during Apple OS inspection: {path.name}')
                if key:
                    cached[key] = details
                    changed = True
            rows.append([location, info.st_size, details['duration_ms'],
                         details['bitrate_kbps'], details['sample_rate'], 0, 0,
                         encoded(details['title']), encoded(details['artist']),
                         encoded('Movies'), encoded(details['artist']), encoded('Video'),
                         'movie'])
        except (OSError, ValueError, TypeError, OverflowError, ZeroDivisionError) as error:
            issues.append(str(error))
    _checkpoint(cancelled)
    if changed:
        try:
            save_json(cache_path, {'version': 2, 'entries': dict(list(cached.items())[-2048:])})
        except OSError:
            pass  # A host cache failure does not invalidate inspected media.
    return rows, issues


def update(identity, *, ffprobe=None, progress=lambda text: None, cancelled=lambda: False):
    """Update selected Apple media while preserving unrelated Apple entries."""
    _publication_checkpoint(cancelled)
    identity = media.refresh(identity)
    device = _device_profile(identity)
    if device is None:
        raise ValueError(_eligibility(identity))
    recovery_problem = pending_recovery(identity)
    if recovery_problem:
        raise ValueError(recovery_problem)
    root = Path(identity['root']).resolve(strict=True)
    state = _load(identity)
    guid = _apple_guid(identity)
    if guid:
        state['apple_guid'] = guid
    selected = _normalize(root, state.get('folders', []), selectable_roots(identity))
    files = _files(root, selected, 'Music', media.extensions('Music'))
    videos = _files(root, selected, 'Videos', media.extensions('Videos'))
    cache = _apple_cache(identity)
    cache.mkdir(parents=True, exist_ok=True)
    source_db = root / 'iPod_Control/iTunes/iTunesDB'
    _plain(source_db, root)
    from . import apple_library
    published = state.get('published') if isinstance(state.get('published'), dict) else {}
    if (published.get('library_outputs') and source_db.is_file() and
            published.get('database_sha256') == _hash(source_db) and
            not library_outputs_ready(identity, published, cancelled=cancelled)):
        raise ValueError('Apple artwork files changed or are missing. Restore the Apple library '
                         'from its backup or sync it again before updating.')
    # Resolve a photo ownership conflict before changing the music database.
    if _files(root, selected, 'Photos', media.extensions('Photos')) or published.get('photo_count', 0):
        photo_db = root / 'Photos/Photo Database'
        _plain(photo_db, root)
        photo_digest = _hash(photo_db) if photo_db.is_file() else None
        if photo_digest and photo_digest != published.get('photo_database_sha256'):
            raise ValueError('The Apple photo library changed outside Linnet. Review it before updating Photos.')
    if (not files and not videos and not _owned_music_paths(published) and
            not published.get('music_count', 0) and not published.get('video_count', 0)):
        _update_photos(identity, state, selected, cache, progress, cancelled=cancelled)
        save_json(_state_path(identity), state)
        count = (state.get('published') or {}).get('photo_count', 0)
        return {'summary': f'{count:,} selected photos are ready in Apple OS. '
                           'Eject safely before switching systems.'}
    if (published.get('music_source_signature') == _signature(root, files) and
            published.get('video_source_signature') == _signature(root, videos) and
            isinstance(published.get('managed_paths'), list) and
            not published.get('music_issues') and not published.get('video_issues') and
            source_db.is_file() and published.get('database_sha256') == _hash(source_db) and
            library_outputs_ready(identity, published, cancelled=cancelled) and
            not apple_library.pending(root)):
        _update_photos(identity, state, selected, cache, progress, cancelled=cancelled)
        save_json(_state_path(identity), state)
        counts = state['published']
        attention = len(counts.get('music_issues', [])) + len(counts.get('video_issues', []))
        summary = (f'{counts.get("music_count", 0):,} songs, '
                   f'{counts.get("video_ready_count", 0):,} videos and '
                   f'{counts.get("photo_count", 0):,} photos are ready in Apple OS.')
        if attention:
            summary += f' {attention:,} files need attention.'
        return {'summary': summary + ' Eject safely before switching systems.'}
    playable_music, music_issues, aliases = [], [], {}
    for path, info in files:
        _publication_checkpoint(cancelled)
        try:
            if info.st_size <= 0 or info.st_size > 0xffffffff:
                raise ValueError(f'Apple music file size is unsupported: {path.relative_to(root)}')
            _apple_music_path(path, root, aliases)
            playable_music.append((path, info))
        except (OSError, ValueError) as error:
            music_issues.append(str(error))
    manifest = (_metadata(identity, playable_music, ffprobe=ffprobe, cancelled=cancelled,
                          progress=progress) if playable_music else [])
    video_tool = ffprobe or shutil.which('ffprobe')
    if videos and not video_tool:
        raise ValueError('Video inspection could not start. ' +
            ('Reinstall Linnet and try again.' if getattr(sys, 'frozen', False) else
             'Choose FFprobe in Settings → Advanced preparation.'))
    video_rows, video_issues = _video_rows(identity, videos, video_tool,
                                         cancelled=cancelled, progress=progress)
    manifest.extend(video_rows)
    prior_owned = _owned_music_paths(published)
    # An inspection failure must not remove a previously published track or
    # its playlist memberships. Keep ownership so a later valid update can
    # still remove the track when its folder is deliberately deselected.
    held_owned = [path for path in prior_owned if
                  (music_issues and path.startswith(APPLE_MUSIC_PREFIX)) or
                  (video_issues and path.startswith(':iPod_Control:Music:..:..:Videos:'))]
    held_set = set(held_owned)
    removable_owned = [path for path in prior_owned if path not in held_set]
    writer = _writer(cache)
    progress(f'Preparing Apple OS library for {len(playable_music):,} songs and {len(video_rows):,} videos…')
    data_cache = cache_data_directory(cache)
    data_cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='linnet-apple-', dir=data_cache) as directory:
        stage = Path(directory)
        stage_db = stage / 'iPod_Control/iTunes/iTunesDB'
        inputs = apple_library.snapshot(root, stage, device['model'],
                                        '0x' + device['apple_guid'], cancelled=cancelled,
                                        artwork_cache=cache_data_directory(cache) / 'artwork-sources',
                                        cache_binding=_apple_binding(identity))
        manifest_path = stage / 'manifest.tsv'
        with manifest_path.open('w', encoding='ascii', newline='\n') as output:
            for row in manifest:
                output.write('\t'.join(map(str, row)) + '\n')
        owned_path = stage / 'managed-paths.txt'
        owned_path.write_text('\n'.join(removable_owned), encoding='ascii')
        _run_writer([writer, stage, manifest_path, device['model'],
                     '0x' + device['apple_guid'], owned_path], timeout=300,
                    cancelled=cancelled)
        if not stage_db.is_file() or stage_db.stat().st_size < 128:
            raise ValueError('Apple database writer produced no valid database.')
        from .retail_media import tracks, _path as apple_path
        parsed = tracks(stage)
        expected = {str(apple_path(stage, row[0]).resolve()).casefold():
                    2 if len(row) == 13 else 1 for row in manifest}
        # Existing Apple entries may also point into Music or Videos. Verify
        # the selected entries without counting those unrelated tracks.
        if len(expected) != len(manifest) or any(
                parsed.get(path, {}).get('media_type') != kind for path, kind in expected.items()):
            raise ValueError('Apple database did not retain the selected songs and videos.')
        digest = _hash(stage_db)
        prepared, removed = apple_library.outputs(stage, inputs, cancelled=cancelled)
        output_receipts = _library_output_receipts(inputs, prepared, removed)
        def verify_inputs():
            apple_library.verify(root, inputs, cancelled=cancelled)
            _verify_sources(root, (*files, *videos), cancelled)
        recovery, record = _publish_library(
            identity, inputs, prepared, removed, verify_inputs, cache, cancelled)
        published = state.get('published') if isinstance(state.get('published'), dict) else {}
        published.update(music_signature=_signature(root, playable_music),
                         music_source_signature=_signature(root, files),
                         video_source_signature=_signature(root, videos),
                         video_count=len(videos), video_ready_count=len(video_rows),
                         video_issues=video_issues, music_issues=music_issues,
                         database_sha256=digest, music_count=len(playable_music),
                         managed_paths=sorted(held_set | {row[0] for row in manifest}),
                         library_outputs=output_receipts,
                         preserved_issue_count=len(held_set))
        state['published'] = published
        try:
            save_json(_state_path(identity), state)
            record['status'] = 'published'
            save_json(recovery, record)
        except OSError as error:
            record['status'] = 'published_without_host_state'
            try:
                save_json(recovery, record)
            except OSError:
                pass
            raise RuntimeError(f'Apple database was published, but its host state could not be saved. '
                               f'Inspect recovery information at {recovery} before retrying.') from error
    _update_photos(identity, state, selected, cache, progress, cancelled=cancelled)
    summary = (f'{len(playable_music):,} songs, {len(video_rows):,} videos and '
               f'{state["published"].get("photo_count", 0):,} photos are ready in Apple OS.')
    if music_issues or video_issues:
        summary += f' {len(music_issues) + len(video_issues):,} files need attention.'
    if held_owned:
        summary += ' Earlier Apple entries were kept until these issues are resolved.'
    return {'summary': summary + ' Eject safely before switching systems.'}
