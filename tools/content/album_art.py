"""Optional album-cover lookup; originals stay untouched and scans stay offline."""
from collections import Counter
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import threading
import time
import unicodedata
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import uuid

from .host_state import load_json_object

USER_AGENT = 'Linnet/0.1 (https://github.com/jspann21/linnet)'
_lock = threading.Lock()
_last_request = 0.0


def _normal(value):
    value = unicodedata.normalize('NFKD', str(value)).casefold()
    return ''.join(c for c in value if c.isalnum())


def _artist(value):
    return re.split(r'\s+(?:feat\.?|ft\.?|featuring)\s+', value, flags=re.I)[0].strip()


def _title(value):
    return re.sub(r'\s*[\[(](?:disc\s*\d+|book (?:one|two)|explicit|edited|bonus tracks)[\])]\s*',
                  '', value, flags=re.I).strip()


def _identity(row):
    artist = row.get('album_artist') or ('Various Artists' if row.get('compilation') else _artist(row.get('artist', '')))
    return artist, row.get('album', '')


def _base(row, *, cache=None):
    artist, album = _identity(row)
    return _base_identity(artist, album, cache=cache)


def _base_identity(artist, album, *, cache=None):
    if not artist or not album:
        return None
    key = hashlib.sha256((_normal(artist) + '\0' + _normal(album)).encode()).hexdigest()
    from .host_cache import cache_base
    return (cache if cache is not None else cache_base()) / 'album-art' / key


def cached(row, *, cache=None):
    base = _base(row, cache=cache)
    path = base.with_suffix('.png') if base else None
    return str(path) if path and path.is_file() else ''


def signature(rows):
    # One library may contain many tracks per album. Check each cached cover
    # only once instead of issuing a host filesystem lookup for every track.
    identities = {_identity(row) for row in rows}
    from .host_cache import cache_base
    cache = cache_base() if any(artist and album for artist, album in identities) else None
    bases = {_base_identity(*identity, cache=cache) for identity in identities}
    stamps = []
    for base in bases:
        if base is None:
            continue
        path = base.with_suffix('.png')
        try:
            info = path.stat()
        except OSError:
            continue
        if stat.S_ISREG(info.st_mode):
            stamps.append((str(path), info.st_size, info.st_mtime_ns))
    stamps.sort()
    return hashlib.sha256(json.dumps(stamps).encode()).hexdigest()


def _request(url, checkpoint, *, maximum=2 * 1024 * 1024):
    global _last_request
    with _lock:
        while time.monotonic() - _last_request < 1.1:
            checkpoint()
            time.sleep(0.1)
        request = Request(url, headers={'User-Agent': USER_AGENT})
        for attempt in range(3):
            checkpoint()
            _last_request = time.monotonic()
            try:
                with urlopen(request, timeout=20) as response:
                    data = response.read(maximum + 1)
                break
            except HTTPError as error:
                if error.code not in (429, 502, 503, 504) or attempt == 2:
                    raise
                delay = error.headers.get('Retry-After', '')
                delay = min(30, max(2, int(delay))) if delay.isdigit() else 5 * (attempt + 1)
                deadline = time.monotonic() + delay
                while time.monotonic() < deadline:
                    checkpoint()
                    time.sleep(0.1)
        if len(data) > maximum:
            raise ValueError('Album artwork response exceeds the size limit')
        return data


def _match(rows, checkpoint):
    owner, album = _identity(rows[0])
    title = _title(album)
    artists = Counter(_artist(row.get('artist', '')) for row in rows if row.get('artist'))
    names = [name for name, _ in artists.most_common(3)]
    if _normal(owner) != 'variousartists' or not artists or artists.most_common(1)[0][1] * 2 <= len(rows):
        names.insert(0, owner)
    names = list(dict.fromkeys(names))
    def quote(value):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    query = 'releasegroup:' + quote(title) + ' AND (' + ' OR '.join('artist:' + quote(n) for n in names) + ')'
    url = 'https://musicbrainz.org/ws/2/release-group/?' + urlencode(dict(query=query, fmt='json', limit=20))
    response = json.loads(_request(url, checkpoint))
    candidates = response.get('release-groups') if isinstance(response, dict) else None
    if not isinstance(candidates, list):
        raise ValueError('Invalid album artwork search response')
    matches = []
    for candidate in candidates:
        if not isinstance(candidate, dict) or not isinstance(candidate.get('artist-credit', []), list):
            raise ValueError('Invalid album artwork search response')
        credits = set()
        for credit in candidate.get('artist-credit', []):
            if not isinstance(credit, dict):
                continue
            artist = credit.get('artist', {})
            if not isinstance(artist, dict):
                raise ValueError('Invalid album artwork search response')
            credits.update(_normal(name) for name in (credit.get('name', ''), artist.get('name', '')) if name)
        if (_normal(candidate.get('title', '')) == _normal(title) and
                credits.intersection(map(_normal, names))):
            matches.append(candidate)
    return matches[0] if len(matches) == 1 else None


def _save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(uuid.uuid4().hex + '.tmp')
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def fetch(root, *, progress=lambda text: None, checkpoint=lambda: None, retry_missing=False):
    from PIL import Image, ImageOps
    from .retail_media import metadata
    existing = list(metadata(root).values())
    groups = {}
    for row in existing:
        base = _base(row)
        if base and not row.get('artwork'):
            groups.setdefault(base, []).append(row)
    result = dict(downloaded=0, cached=len({row['artwork'] for row in existing if row.get('artwork')}), unmatched=0, errors=[])
    for number, (base, rows) in enumerate(groups.items(), 1):
        checkpoint()
        state_path = base.with_suffix('.json')
        previous = load_json_object(state_path)
        checked, now = previous.get('checked'), time.time()
        if (not retry_missing and previous.get('matcher') == 1 and
                previous.get('status') == 'unmatched' and type(checked) in (int, float) and
                now - 7 * 86400 < checked <= now):
            result['unmatched'] += 1
            continue
        owner, album = _identity(rows[0])
        progress(f'Finding album covers {number}/{len(groups)}: {album}')
        try:
            match = _match(rows, checkpoint)
            source = ''
            if match:
                if not isinstance(match.get('id'), str):
                    raise ValueError('Invalid album artwork release-group identifier')
                identifier = str(uuid.UUID(match['id']))
                source = f'https://coverartarchive.org/release-group/{identifier}/front-500'
                try:
                    raw = _request(source, checkpoint, maximum=8 * 1024 * 1024)
                except HTTPError as error:
                    if error.code != 404:
                        raise
                    match = None
            if match:
                with Image.open(io.BytesIO(raw)) as image:
                    if image.width * image.height > 20_000_000 or getattr(image, 'n_frames', 1) != 1:
                        raise ValueError('Unsupported album cover dimensions')
                    image = ImageOps.exif_transpose(image).convert('RGB')
                    image.thumbnail((600, 600))
                    output = io.BytesIO()
                    image.save(output, format='PNG')
                checkpoint()
                _save(base.with_suffix('.png'), output.getvalue())
                result['downloaded'] += 1
            else:
                result['unmatched'] += 1
            _save(state_path, json.dumps(dict(artist=owner, album=album, source=source, matcher=1,
                release_group=match['id'] if match else None, checked=time.time(),
                status='matched' if match else 'unmatched')).encode())
        except (OSError, URLError, ValueError, KeyError, Image.DecompressionBombError) as error:
            result['errors'].append(str(error))
            progress(f'Album cover lookup unavailable: {error}. Continuing preparation with available artwork.')
            break
    progress(f"Album covers: {result['downloaded']} downloaded; {result['cached']} reused; {result['unmatched']} without a confident match.")
    return result
