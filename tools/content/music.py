"""Deterministic, bounded PODB compiler and host validator.

WAV/AIFF PCM metadata is parsed without decoding audio. Optional ffprobe metadata
does not confer device codec support. Staged copies preserve original files.
"""
from __future__ import annotations

import ctypes
from collections import OrderedDict, deque
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
from contextlib import nullcontext
import os
from typing import Callable
import hashlib
import io
import json
import math
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import struct
import subprocess
import threading
import tempfile
import time
import zlib

from tools.studio.targets import TARGETS
from .host_state import cache_volume_scope, file_stamp

HEADER = 128
TRACK = 132

GROUP = 32
ART = 32
MAX_TRACKS = 100_000
MAX_DATABASE = 128 * 1024 * 1024
MAX_STRING = 255
NO_INDEX = 0xFFFFFFFF
UNKNOWN_GAIN = -0x80000000
CODECS = {"pcm_le": 1, "pcm_be": 2, "float_le": 3, "float_be": 4,
          "mp3": 16, "aac": 17, "alac": 18, "flac": 19,
          "vorbis": 20, "opus": 21, "wavpack": 22, "unknown": 255}
PLAYBACK_RATES = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000}
HIGH_PLAYBACK_RATES = PLAYBACK_RATES | {88200, 96000}
CODEC_NAMES = {number: name for name, number in CODECS.items()}
PLAYABLE_CODECS = frozenset(CODECS[key] for key in ('pcm_le', 'pcm_be', 'mp3', 'aac', 'alac', 'flac', 'vorbis'))


def compatibility_issue(metadata: dict, target: str | None = None) -> str | None:
    """Return a firmware playback limitation without decoding the source again."""
    codec = metadata['codec']
    name = CODEC_NAMES.get(codec, 'unknown')
    if codec not in PLAYABLE_CODECS:
        return f'{name} has no Linnet decoder; convert this unprotected file to AAC, MP3 or PCM WAV'
    channels, rate, bits = metadata['channels'], metadata['rate'], metadata['bits']
    if channels > 2:
        return f'{channels} channels exceed stereo playback; downmix to mono or stereo'
    rates = (HIGH_PLAYBACK_RATES if target is None or
             TARGETS[target].get('audio_codec', 'wm8975') == 'wm8975' else PLAYBACK_RATES)
    if rate not in rates:
        return f'{rate} Hz is unavailable on this target; resample to 44100 or 48000 Hz'
    if codec in (CODECS['pcm_le'], CODECS['pcm_be']):
        container = metadata.get('container')
        if container is None:
            return 'PCM container needs verification before playback'
        if container != ('wav' if codec == CODECS['pcm_le'] else 'aiff'):
            return f'{container} is unavailable in firmware; convert to standard PCM WAV'
    if codec == CODECS['flac']:
        minimum, block, frame = (metadata.get(key) for key in
                                 ('min_blocksize', 'max_blocksize', 'max_framesize'))
        if minimum is None or block is None or frame is None:
            return 'FLAC frame bounds need verification before playback'
        if minimum < 16 or block < minimum or block > 4608 or frame > 65536:
            return 'FLAC frame exceeds firmware buffers; convert to standard PCM WAV'
        if bits and not 8 <= bits <= 24:
            return f'{bits}-bit FLAC exceeds the decoder limit; convert to 16 or 24 bit audio'
    if codec == CODECS['alac'] and bits and bits != 16:
        return f'{bits}-bit ALAC exceeds the decoder limit; convert to 16 bit audio'
    if codec == CODECS['alac'] and not metadata.get('alac_frame_ok'):
        return 'ALAC frame size needs conversion or verification (4096 samples maximum)'
    if codec == CODECS['aac'] and metadata.get('profile') not in ('LC', 'AAC LC'):
        return f"AAC {metadata.get('profile') or 'profile'} needs conversion or verification as AAC LC"
    if codec == CODECS['aac'] and metadata.get('container') == 'mp4' and not metadata.get('aac_config_ok'):
        return 'AAC frame configuration needs conversion or verification (1024 samples required)'
    if codec == CODECS['aac'] and metadata.get('container') == 'adts' and not metadata.get('adts_header_ok'):
        return 'AAC ADTS frame header is unavailable in firmware; convert to standard AAC LC'
    return None
EXTENSIONS = {".wav", ".aif", ".aiff", ".aifc", ".mp3", ".m4a", ".m4b",
              ".aac", ".flac", ".flc", ".ogg", ".opus", ".ops", ".wv"}
COVER_NAMES = ("cover.png", "cover.jpg", "folder.jpg", "Folder.jpg")
COVERFLOW_CACHE_MAX_ENTRIES = 4096  # At most 95 MiB of native tile payloads.
RENDER_CACHE_MAX_ENTRIES = 2048  # At most 310 MiB of complete native playback + Cover Flow renders.
METADATA_WORKERS = 8
NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def _text(value: object) -> str:
    value = str(value).replace("\x00", " ").strip()
    if len(value.encode("utf-8")) > MAX_STRING:
        raise ValueError("metadata string exceeds 255 UTF-8 bytes")
    return value


def _number(value: object) -> int:
    try:
        number = int(str(value).split("/", 1)[0])
        return number if 0 <= number <= 0xFFFFFFFF else 0
    except (ValueError, TypeError):
        return 0


def _embedded_cover(path: Path) -> bytes | None:
    """Read a front cover from native audio tags, without decoding the audio."""
    try:
        import mutagen
    except ImportError:
        return None
    try:
        if path.suffix.lower() == ".mp3":
            # MP3 artwork lives in ID3. Opening the full audio parser also
            # scans MPEG frames on some files, which is costly on iPod disks.
            from mutagen.id3 import ID3
            # ID3v1 has no artwork; skip its extra seek and read at the file tail.
            tags = ID3(path, load_v1=False)
            pictures = tags.getall("APIC")
        else:
            audio = mutagen.File(path)
            if audio is None:
                return None
            tags = audio.tags or {}
            pictures = list(getattr(audio, "pictures", ()))
            if hasattr(tags, "getall"):
                pictures += tags.getall("APIC")
        if pictures:
            picture = next((p for p in pictures if p.type == 3), pictures[0])
            data = picture.data
        elif tags.get("covr"):
            data = bytes(tags["covr"][0])
        elif tags.get("metadata_block_picture"):
            import base64
            from mutagen.flac import Picture
            data = None
            for encoded in tags["metadata_block_picture"]:
                if len(encoded) > 44 * 1024 * 1024:
                    continue
                try:
                    picture = Picture(base64.b64decode(encoded, validate=True))
                except (ValueError, mutagen.MutagenError):
                    continue
                if not 0 < len(picture.data) <= 32 * 1024 * 1024:
                    continue
                if data is None or picture.type == 3:
                    data = picture.data
                if picture.type == 3:
                    break
            if data is None:
                return None
        else:
            return None
        return data if 0 < len(data) <= 32 * 1024 * 1024 else None
    except (mutagen.MutagenError, ValueError, TypeError, AttributeError, KeyError, IndexError, struct.error):
        return None


def _mp102_cover(image):
    from PIL import Image, ImageOps
    gray = image.convert("L")
    # A small contrast lift retains the cover's tone without stretching every
    # shadow/highlight. Half-strength diffusion reduces speckle on the LCD.
    gray = Image.blend(gray, ImageOps.autocontrast(gray, cutoff=1), 0.25)
    width, height = gray.size
    source = list(gray.getdata())
    result = bytearray(width * height)
    errors = [0.0] * (width + 2)
    for y in range(height):
        following = [0.0] * (width + 2)
        step = 1 if y % 2 == 0 else -1
        for x in range(width) if step == 1 else range(width - 1, -1, -1):
            value = max(0.0, min(255.0, source[y * width + x] + errors[x + 1]))
            level = int(value / 85.0 + 0.5) * 85
            result[y * width + x] = level
            error = (value - level) * 0.5
            errors[x + 1 + step] += error * 7 / 16
            following[x + 1 - step] += error * 3 / 16
            following[x + 1] += error * 5 / 16
            following[x + 1 + step] += error / 16
        errors = following
    return Image.frombytes("L", gray.size, bytes(result)).convert("RGB")


def _cover_rgb(image):
    from PIL import Image
    if 'A' in image.getbands() or 'transparency' in image.info:
        rgba = image.convert('RGBA')
        result = Image.new('RGB', rgba.size, (0, 0, 0))
        result.paste(rgba, mask=rgba.getchannel('A'))
        return result
    return image if image.mode == 'RGB' else image.convert('RGB')


def _cover_srgb(image, profile):
    from PIL import ImageCms
    if 'A' in image.getbands() or 'transparency' in image.info:
        image = _cover_rgb(image)
    if profile:
        try:
            return ImageCms.profileToProfile(image, ImageCms.ImageCmsProfile(io.BytesIO(profile)),
                                             ImageCms.createProfile('sRGB'), outputMode='RGB')
        except (OSError, ValueError, ImageCms.PyCMSError):
            pass
    return _cover_rgb(image)


def _cover_normalized(image):
    from PIL import ImageOps
    profile = image.info.get('icc_profile')
    if image.getexif().get(274, 1) != 1:
        image = ImageOps.exif_transpose(image)
    return _cover_srgb(image, profile)


def _cover_pixels_rgb(image, target):
    from PIL import Image, ImageOps
    side = TARGETS[target]["media_art_side"] if target else 64
    original = image
    image = ImageOps.fit(image, (side, side), method=Image.Resampling.LANCZOS)
    if target and TARGETS[target]["lcd_format"] == "gray2-horizontal":
        image = _mp102_cover(image)
    from .photos import _rgb565
    if side == 160:
        # Exact display renditions share one read and CRC. The home pane uses
        # 160px; the standard Now Playing layout copies the appended 112px.
        small = ImageOps.fit(original, (112, 112), method=Image.Resampling.LANCZOS)
        def rounded_rgb565(cover):
            return struct.pack(f'<{cover.width * cover.height}H', *[
                ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
                for r, g, b in cover.getdata()])
        pixels = rounded_rgb565(image) + rounded_rgb565(small)
        if target == 'classic6g-n25':
            pixels += rounded_rgb565(ImageOps.fit(original, (126, 126), method=Image.Resampling.LANCZOS))
        return pixels
    return _rgb565(image)


def _cover_renders(source, target, coverflow_enabled, known_art=()):
    """Decode once, then apply the distinct playback and Cover Flow recipes."""
    from PIL import Image
    try:
        image = Image.open(source)
    except Image.DecompressionBombError as error:
        raise ValueError("cover dimensions exceed bound") from error
    with image:
        if image.width * image.height > 16_000_000:
            raise ValueError("cover dimensions exceed bound")
        if image.format == 'JPEG':
            side = 2 * max(TARGETS[target]["media_art_side"] if target else 64,
                           96 if coverflow_enabled else 0)
            image.draft(image.mode, (side, side))
        image.load()
        normalized = _cover_normalized(image)
        pixels = _cover_pixels_rgb(normalized, target)
        digest = hashlib.sha256(pixels).digest()
        if coverflow_enabled and digest not in known_art:
            from .coverflow import _original_from_rgb
            flow = _original_from_rgb(normalized, 130 if target and TARGETS[target].get('presentation_profile') == 'color320' else 96)
        else:
            flow = None
    return pixels, digest, flow


def _album_artist(tags: dict, artist: str) -> str:
    """Album ownership is independent of the performing track artist."""
    owner = _text(tags.get("album_artist") or tags.get("albumartist") or artist)
    if (str(tags.get("compilation", "")).strip().casefold() in ("1", "true", "yes")
            and owner.casefold() == artist.casefold()):
        return "Various Artists"
    # Guest credits sometimes leak into album-artist tags. Keep the primary
    # artist as owner, while retaining the complete credit on the track.
    owner = re.split(r"\s+(?:feat\.?|ft\.?|featuring)\s+", owner, maxsplit=1,
                     flags=re.IGNORECASE)[0].rstrip()
    return owner or artist


def _stable(value: str) -> int:
    return int.from_bytes(hashlib.sha256(value.encode("utf-8")).digest()[:8], "little")


def _extended(raw: bytes) -> int:
    exponent, mantissa = struct.unpack(">HQ", raw)
    if not 16383 <= exponent <= 16401 or not mantissa & (1 << 63):
        raise ValueError("invalid AIFF sample rate")
    shift = 63 - (exponent - 16383)
    rate = mantissa >> shift
    if mantissa & ((1 << shift) - 1) or not 1 <= rate <= 384000:
        raise ValueError("unsupported AIFF sample rate")
    return rate


def _pcm(path: Path, cancelled=lambda: False) -> dict:
    size = path.stat().st_size
    with path.open("rb") as stream:
        top = stream.read(12)
        wave = top[:4] == b"RIFF" and top[8:] == b"WAVE"
        aiff = top[:4] == b"FORM" and top[8:] in (b"AIFF", b"AIFC")
        if not wave and not aiff:
            raise ValueError("not a supported RIFF WAVE or AIFF container")
        endian = "<" if wave else ">"
        end = struct.unpack(endian + "I", top[4:8])[0] + 8
        if end > size or end < 12:
            raise ValueError("truncated audio container")
        result = {"tags": {}, "data_offset": 0, "data_bytes": 0, "frames": 0}
        position = 12
        chunks = 0
        have_data = False
        while position < end:
            _check_cancel(cancelled)
            chunks += 1
            if chunks > 10000 or end - position < 8:
                raise ValueError("audio chunk bounds")
            stream.seek(position)
            kind, length = struct.unpack(endian + "4sI", stream.read(8))
            start = position + 8
            if length + (length & 1) > end - start:
                raise ValueError("truncated audio chunk")
            next_position = start + length + (length & 1)
            if wave and kind == b"fmt ":
                if length < 16 or length > 65536 or "codec" in result:
                    raise ValueError("invalid WAV format chunk")
                raw = stream.read(length)
                encoding, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", raw)
                extensible = encoding == 0xFFFE
                if extensible:
                    if length < 40 or raw[26:40] != bytes.fromhex("000000001000800000aa00389b71"):
                        raise ValueError("unsupported WAV extensible format")
                    encoding = struct.unpack_from("<H", raw, 24)[0]
                    if struct.unpack_from("<H", raw, 18)[0] != bits:
                        raise ValueError("packed valid bits are not supported")
                if encoding not in (1, 3) or bits not in (8, 16, 24, 32) or not channels or not rate:
                    raise ValueError("unsupported WAV PCM encoding")
                if align != channels * ((bits + 7) // 8) or byte_rate != rate * align:
                    raise ValueError("inconsistent WAV sample layout")
                if encoding == 3 and bits != 32:
                    raise ValueError("unsupported WAV float width")
                result.update(codec=CODECS["pcm_le" if encoding == 1 else "float_le"],
                              channels=channels, rate=rate, bits=bits, align=align,
                              container='wav-extensible' if extensible else 'wav')
            elif aiff and kind == b"COMM":
                if length < 18 or length > 65536 or "codec" in result:
                    raise ValueError("invalid AIFF common chunk")
                raw = stream.read(length)
                channels, frames, bits = struct.unpack_from(">HIH", raw)
                compression = b"NONE" if top[8:] == b"AIFF" else raw[18:22]
                codec = {b"NONE": "pcm_be", b"twos": "pcm_be", b"sowt": "pcm_le",
                         b"fl32": "float_be", b"FL32": "float_be"}.get(compression)
                if codec is None or bits not in (8, 16, 24, 32) or not channels:
                    raise ValueError("unsupported AIFF PCM encoding")
                if codec == "float_be" and bits != 32:
                    raise ValueError("unsupported AIFF float width")
                result.update(codec=CODECS[codec], channels=channels, frames=frames,
                              rate=_extended(raw[8:18]), aiff_rate_exact=True,
                              bits=bits, align=channels * ((bits + 7) // 8),
                              container='aifc' if top[8:] == b'AIFC' else 'aiff')
            elif (wave and kind == b"data") or (aiff and kind == b"SSND"):
                if have_data:
                    raise ValueError("multiple audio data chunks")
                have_data = True
                if aiff:
                    if length < 8:
                        raise ValueError("short AIFF sound chunk")
                    offset, _block = struct.unpack(">II", stream.read(8))
                    if offset > length - 8:
                        raise ValueError("AIFF sound offset")
                    start += 8 + offset
                    length -= 8 + offset
                result.update(data_offset=start, data_bytes=length)
            elif aiff and kind in (b"NAME", b"AUTH") and length <= 4096:
                key = "title" if kind == b"NAME" else "artist"
                result["tags"][key] = stream.read(length).decode("latin-1").rstrip("\x00")
            elif wave and kind == b"LIST" and length <= 1024 * 1024:
                raw = stream.read(length)
                if raw[:4] == b"INFO":
                    cursor = 4
                    tags = {b"INAM": "title", b"IART": "artist", b"IPRD": "album",
                            b"IGNR": "genre", b"ICRD": "date", b"ITRK": "track",
                            b"IAMP": "album_artist", b"ICMP": "composer", b"IDIS": "disc",
                            b"IGAN": "replaygain_track_gain", b"IGAL": "replaygain_album_gain",
                            b"ICPL": "compilation", b"IHS1": "source_sha256",
                            b"IHC1": "pcm_sha256"}
                    while cursor + 8 <= len(raw):
                        tag, count = struct.unpack_from("<4sI", raw, cursor)
                        cursor += 8
                        if count > len(raw) - cursor:
                            raise ValueError("truncated WAV INFO tag")
                        if tag in tags:
                            result["tags"][tags[tag]] = raw[cursor:cursor+count].rstrip(b"\0").decode("utf-8", "replace")
                        cursor += count + (count & 1)
            position = next_position
        if "codec" not in result or not have_data:
            raise ValueError("audio format or data missing")
        if result["data_bytes"] % result["align"]:
            raise ValueError("incomplete PCM frame")
        frames = result["data_bytes"] // result["align"]
        if aiff and result["frames"] != frames:
            raise ValueError("AIFF frame count disagrees with sound bytes")
        result["frames"] = frames
        if result["channels"] > 8 or result["rate"] > 384000:
            raise ValueError("audio channel/sample-rate bound")
        return result


def _flac_bounds(path: Path) -> dict:
    with path.open('rb') as stream:
        header = stream.read(18)
    if (len(header) != 18 or header[:4] != b'fLaC' or header[4] & 0x7f or
            header[5:8] != b'\x00\x00\x22'):
        raise ValueError('FLAC STREAMINFO is not at the start of the file')
    return dict(min_blocksize=int.from_bytes(header[8:10], 'big'),
                max_blocksize=int.from_bytes(header[10:12], 'big'),
                max_framesize=int.from_bytes(header[15:18], 'big'))


def _adts_header_ok(path: Path) -> bool:
    with path.open('rb') as stream:
        b = stream.read(7)
    if len(b) != 7:
        return False
    channels = ((b[2] & 1) << 2) | (b[3] >> 6)
    frame_bytes = ((b[3] & 3) << 11) | (b[4] << 3) | (b[5] >> 5)
    return (b[0] == 255 and b[1] & 0xf6 == 0xf0 and b[1] & 1 == 1 and
            b[2] >> 6 == 1 and (b[2] >> 2 & 15) < 13 and 1 <= channels <= 2 and
            b[6] & 3 == 0 and 8 <= frame_bytes <= path.stat().st_size)


def _decoded_frames(path: Path, executable: str, *, recover: bool = False,
                    codec: int | None = None, cancelled=lambda: False) -> int:
    """Count decoded samples, never estimate duration from bitrate/container time.

    No PCM is emitted. Decoding has a deadline; probe output is capped at 1 MiB.
    Decode errors reject staged imports. Existing tracks may recover damaged
    packets, matching playback; count only samples actually decoded. Missing
    sample counts, fatal failures and exceeded budgets always reject the import.
    """
    # Existing device files can arrive over slow USB while other metadata reads
    # share the disk. Keep a bounded I/O allowance as well as the decode budget.
    budget = max(30, min(600, 30 + path.stat().st_size // 65536)) if recover else 30
    indexed_bytes = None
    if codec == CODECS["mp3"]:
        from .mp3_index import original_size
        indexed_bytes = original_size(path, checkpoint=lambda: _check_cancel(cancelled))
    if indexed_bytes is not None:
        with path.open('rb') as reader:
            reader.seek(-64, os.SEEK_END)
            footer = reader.read(64)
        indexed_tail = struct.unpack_from('<I', footer, 36)[0]
        indexed_total = struct.unpack_from('<Q', footer, 40)[0]
    command = [executable, "-v", "fatal" if recover else "error"]
    # Tags can fool FFprobe's format sniffing (including MP3 misdetected as
    # WAV). Use the MP3 identity already established by the metadata reader.
    if codec == CODECS["mp3"]:
        command += ["-f", "mp3"]
    command += ["-select_streams", "a:0",
               "-show_entries", "frame=nb_samples:frame_side_data=",
               "-of", "csv=p=0", 'pipe:0' if indexed_bytes is not None else str(path)]
    # Recovery counts decoded audio only; unrelated embedded-art diagnostics
    # must not become sample-count rows. Fatal exits are still rejected below.
    with subprocess.Popen(command, stdin=subprocess.PIPE if indexed_bytes is not None else subprocess.DEVNULL,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL if recover else subprocess.STDOUT,
                          creationflags=NO_WINDOW) as process:
        feed_error = []
        def feed_original():
            try:
                with path.open('rb') as reader:
                    remaining = indexed_bytes
                    while remaining:
                        _check_cancel(cancelled)
                        chunk = reader.read(min(remaining, 1024 * 1024))
                        if not chunk:
                            raise OSError('Indexed MP3 changed during verification')
                        process.stdin.write(chunk)
                        remaining -= len(chunk)
            except Exception as error:
                feed_error.append(error)
                if process.poll() is None:
                    process.kill()
            finally:
                try:
                    process.stdin.close()
                except OSError as error:
                    if not feed_error:
                        feed_error.append(error)
        feeder = None
        if indexed_bytes is not None:
            feeder = threading.Thread(target=feed_original, name='music-mp3-prefix', daemon=True)
            feeder.start()
        stop_watch, was_cancelled, watcher = _watch_cancellation(process, cancelled)
        timed_out = threading.Event()
        def expire():
            if process.poll() is None:
                timed_out.set()
                process.kill()
        timer = threading.Timer(budget, expire)
        timer.daemon = True
        timer.start()
        output = bytearray()
        try:
            while chunk := process.stdout.read(4096):
                output.extend(chunk)
                if len(output) > 1024 * 1024:
                    raise ValueError(f"{path}: decoded sample-count output exceeds limit")
            returncode = process.wait()
            _check_cancel(cancelled, was_cancelled)
            if timed_out.is_set():
                # A deadline says nothing about file validity. Stop preparation
                # without permanently adding this track to the source skip list.
                raise TimeoutError(f"{path}: decoded sample-count probe exceeded {budget} seconds")
            if returncode:
                raise ValueError(f"{path}: decoded sample-count probe failed (exit {returncode})")
        finally:
            stop_watch.set()
            timer.cancel()
            if process.poll() is None:
                process.kill()
            process.wait()
            watcher.join()
            if feeder is not None:
                feeder.join(timeout=5)
                if feeder.is_alive():
                    raise TimeoutError(f"{path}: MP3 verification reader did not stop")
        if feed_error:
            raise feed_error[0]
    frames = 0
    for line in output.splitlines():
        if not line:
            continue
        if not re.fullmatch(rb"[1-9][0-9]{0,6},?", line):
            raise ValueError(f"{path}: invalid decoded sample count or audio decode error")
        samples = int(line.rstrip(b","))
        if samples > 1048576:
            raise ValueError(f"{path}: decoded audio frame exceeds sample bound")
        frames += samples
    if not 0 < frames <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError(f"{path}: missing or oversized decoded sample count")
    if indexed_bytes is not None:
        # A pipe cannot seek to the LAME footer, so FFprobe emits the encoder's
        # trailing padding. The indexed metadata was verified above.
        if frames < indexed_tail or frames - indexed_tail != indexed_total:
            raise ValueError(f"{path}: prepared MP3 audio sample count disagrees with its checkpoint")
        frames -= indexed_tail
    return frames


def _check_cancel(cancelled, observed=None):
    if (observed is not None and observed.is_set()) or cancelled():
        from .media_manager import Cancelled
        raise Cancelled('Stopped while reading music metadata')


def _watch_cancellation(process, cancelled):
    stop = threading.Event()
    observed = threading.Event()

    def watch():
        while not stop.wait(0.05):
            if cancelled():
                observed.set()
                if process.poll() is None:
                    try:
                        process.kill()
                    except OSError:
                        pass  # The process exited between poll and kill.
                return

    watcher = threading.Thread(target=watch, name='music-probe-cancel', daemon=True)
    watcher.start()
    return stop, observed, watcher


def _probe(path: Path, executable: str, *, exact_frames: bool = True,
           cancelled=lambda: False, allow_unknown=False) -> dict:
    # ffprobe is optional host metadata tooling, never a native decoder substitute.
    command = [executable, "-v", "error", "-show_entries",
               "stream=codec_name,codec_type,profile,sample_rate,channels,bits_per_raw_sample:format_tags:stream_tags",
               "-of", "json"]
    if not exact_frames:
        command += ["-probesize", "262144", "-analyzeduration", "1000000"]
    command.append(str(path))
    _check_cancel(cancelled)
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          creationflags=NO_WINDOW) as process:
        stop_watch, was_cancelled, watcher = _watch_cancellation(process, cancelled)
        try:
            try:
                stdout, stderr = process.communicate(timeout=30)
            except subprocess.TimeoutExpired as error:
                process.kill()
                process.communicate()
                _check_cancel(cancelled, was_cancelled)
                raise TimeoutError(f"{path}: metadata probe exceeded 30 seconds") from error
            _check_cancel(cancelled, was_cancelled)
            if process.returncode:
                raise subprocess.CalledProcessError(process.returncode, command, stdout, stderr)
        finally:
            stop_watch.set()
            if process.poll() is None:
                process.kill()
            process.wait()
            watcher.join()
    if len(stdout) > 1024 * 1024:
        raise ValueError("ffprobe metadata exceeds limit")
    info = json.loads(stdout)
    streams = [item for item in info.get("streams", []) if item.get("codec_type") == "audio"]
    if len(streams) != 1:
        raise ValueError("exactly one audio stream required")
    audio = streams[0]
    codec = CODECS.get(audio.get("codec_name"), CODECS["unknown"])
    if codec == CODECS["unknown"] and not allow_unknown:
        raise ValueError("unrecognized compressed audio codec")
    tags = {str(k).lower(): v for k, v in info.get("format", {}).get("tags", {}).items()}
    tags.update({str(k).lower(): v for k, v in audio.get("tags", {}).items()})
    rate, channels = _number(audio.get("sample_rate")), _number(audio.get("channels"))
    if not 1 <= rate <= 384000 or not 1 <= channels <= 8:
        raise ValueError("audio channel/sample-rate bound")
    bits = _number(audio.get("bits_per_raw_sample"))
    if bits > 64:
        raise ValueError("compressed source bit-depth bound")
    mp4 = codec in (CODECS['aac'], CODECS['alac']) and path.suffix.lower() in ('.m4a', '.m4b')
    verified = _quick_metadata(path, cancelled) if mp4 else None
    config = ({key: verified[key] for key in ('container', 'profile', 'aac_config_ok', 'alac_frame_ok', 'mp4_config_version')
               if key in verified} if verified and verified['codec'] == codec else
              {'container': 'mp4'} if mp4 else {})
    if codec == CODECS['aac'] and path.suffix.lower() == '.aac':
        config.update(container='adts', adts_header_ok=_adts_header_ok(path))
    profile = config.pop('profile', audio.get('profile'))
    frames = _decoded_frames(path, executable, codec=codec, cancelled=cancelled) if exact_frames else 0
    if verified:
        frames = verified['frames']
    return dict(codec=codec, channels=channels, rate=rate, bits=bits, profile=profile,
                **config,
                **(_flac_bounds(path) if codec == CODECS['flac'] else {}),
                frames=frames, data_offset=0,
                 data_bytes=path.stat().st_size, tags=tags)


def _mp3_header_frames(path: Path, info, cancelled=lambda: False, stream=None) -> int:
    """Read an exact Xing/Info or VBRI sample count without scanning the MP3."""
    rate = int(info.sample_rate)
    # Mutagen only assigns a known bitrate mode after finding a Xing/Info or
    # VBRI header. Headerless MP3s retain the full FFprobe scan fallback.
    if not int(getattr(info, "bitrate_mode", 0)):
        return 0
    if getattr(info, "encoder_info", "") == "FhG":
        return round(float(info.length) * rate)
    if int(getattr(info, "layer", 0)) != 3:
        return 0
    version, mode = float(info.version), int(info.mode)
    side = (21 if mode == 3 else 36) if version == 1 else (13 if mode == 3 else 21)
    with path.open("rb") if stream is None else nullcontext(stream) as raw_stream:
        stream = _CancellableReader(raw_stream, cancelled)
        stream.seek(int(info.frame_offset) + side)
        marker = stream.read(4)
        if marker not in (b"Xing", b"Info"):
            return 0
        flags_raw = stream.read(4)
        if len(flags_raw) != 4:
            return 0
        flags = int.from_bytes(flags_raw, "big")
        if not flags & 1:
            return 0
        frames_raw = stream.read(4)
        if len(frames_raw) != 4:
            return 0
        frame_count = int.from_bytes(frames_raw, "big")
        byte_count = None
        if flags & 2:
            raw = stream.read(4)
            if len(raw) != 4:
                return 0
            byte_count = int.from_bytes(raw, "big")
        if flags & 4:
            if len(stream.read(100)) != 100:
                return 0
        if flags & 8:
            if len(stream.read(4)) != 4:
                return 0
        encoder, extension = stream.read(9), stream.read(27)
    samples_per_frame = 1152 if version == 1 else 576
    samples = frame_count * samples_per_frame
    # A LAME-compatible extension carries gapless delay and padding. Validate
    # its declared audio length before trusting those fields; FFmpeg's Lavc
    # encoder writes the same extension with a different encoder label.
    if (len(encoder) == 9 and len(extension) == 27 and extension[0] >> 4 == 0
            and byte_count and int.from_bytes(extension[19:23], "big") == byte_count):
        delay = (extension[12] << 4) | (extension[13] >> 4)
        padding = ((extension[13] & 15) << 8) | extension[14]
        if delay + padding < samples:
            samples -= delay + padding
    return samples if 0 < samples <= 0xFFFFFFFFFFFFFFFF else 0


def _quick_frames(path: Path, info, codec_name: str, rate: int,
                  cancelled=lambda: False, stream=None) -> int:
    """Return exact container/header samples, or zero to request full probing."""
    if codec_name == "flac":
        frames = int(getattr(info, "total_samples", 0))
    elif codec_name == "mp3":
        frames = _mp3_header_frames(path, info, cancelled, stream)
    else:
        # Ogg uses the final granule position and WavPack uses its total-sample
        # header (or block indices), all of which Mutagen exposes as length.
        frames = round(float(getattr(info, "length", 0)) * rate)
    return frames if 0 < frames <= 0xFFFFFFFFFFFFFFFF else 0


class _CancellableReader:
    """Check Stop between the bounded reads and seeks made by tag parsers."""
    def __init__(self, stream, cancelled):
        self.stream = stream
        self.cancelled = cancelled

    def __getattr__(self, name):
        return getattr(self.stream, name)

    def read(self, *args):
        _check_cancel(self.cancelled)
        return self.stream.read(*args)

    def seek(self, *args):
        _check_cancel(self.cancelled)
        return self.stream.seek(*args)

    def readline(self, *args):
        _check_cancel(self.cancelled)
        return self.stream.readline(*args)

    def readinto(self, *args):
        _check_cancel(self.cancelled)
        return self.stream.readinto(*args)


def _mp4_audio_config(track, reader, codec: str) -> dict:
    """Read bounded sample-entry fields that Mutagen does not expose."""
    flag = 'aac_config_ok' if codec == 'aac' else 'alac_frame_ok'
    unsupported = {flag: False}
    atom = track[b'mdia', b'minf', b'stbl', b'stsd']
    if atom.datalength < 52:
        return unsupported
    start = atom.offset + atom.length - atom.datalength
    reader.seek(start)
    data = reader.read(44)
    if len(data) != 44 or struct.unpack_from('>II', data) != (0, 1):
        return unsupported
    entry_size, entry_type = struct.unpack_from('>I4s', data, 8)
    if (entry_size != atom.datalength - 8 or
            entry_type != (b'mp4a' if codec == 'aac' else b'alac') or
            struct.unpack_from('>HH', data, 22) != (1, 0)):
        return unsupported
    wanted = b'esds' if codec == 'aac' else b'alac'
    body = None
    at, end = start + 44, start + atom.datalength
    for _ in range(4096):
        if at == end:
            break
        reader.seek(at)
        header = reader.read(min(16, end - at))
        if len(header) < 8:
            return unsupported
        size, kind = struct.unpack_from('>I4s', header)
        head = 16 if size == 1 else 8
        if size == 1:
            if len(header) < 16:
                return unsupported
            size = struct.unpack_from('>Q', header, 8)[0]
        elif size == 0:
            size = end - at
        if size < head or size > end - at:
            return unsupported
        if kind == wanted:
            if body is not None or size - head > 4096:
                return unsupported
            reader.seek(at + head)
            body = reader.read(size - head)
            if len(body) != size - head:
                return unsupported
        at += size
    if body is None or at != end:
        return unsupported
    if codec == 'alac':
        frames = int.from_bytes(body[4:8], 'big') if len(body) >= 8 else 0
        return {'alac_frame_ok': (len(body) == 28 and body[:4] == bytes(4) and
                                 0 < frames <= 4096 and body[8] == 0 and body[9] == 16 and
                                 body[12] <= 14 and 1 <= body[13] <= 2 and
                                 struct.unpack_from('>HH', data, 32) == (body[13], 16))}
    if len(body) < 4 or body[:4] != b'\0\0\0\0':
        return unsupported
    data, at = body, 4
    def descriptor(expected, at):
        if at >= len(data) or data[at] != expected:
            raise ValueError('unexpected MP4 audio descriptor')
        at += 1
        length = 0
        for _ in range(4):
            if at >= len(data):
                raise ValueError('truncated MP4 audio descriptor')
            byte = data[at]; at += 1
            length = (length << 7) | (byte & 127)
            if not byte & 128:
                break
        else:
            raise ValueError('oversized MP4 audio descriptor')
        if length > len(data) - at:
            raise ValueError('truncated MP4 audio descriptor')
        return at, at + length
    try:
        es, es_end = descriptor(3, at)
        flags = data[es + 2]
        at = es + 3 + (2 if flags & 128 else 0)
        if flags & 64:
            at += 1 + data[at]
        if flags & 32:
            at += 2
        config, config_end = descriptor(4, at)
        if config_end - config < 13 or data[config] != 0x40 or data[config + 1] >> 2 & 63 != 5:
            return unsupported
        asc, end = descriptor(5, config + 13)
        if config_end > es_end or end > config_end:
            return unsupported
        raw = data[asc:end]
    except (IndexError, ValueError):
        return unsupported
    return {'aac_config_ok': (2 <= len(raw) <= 64 and raw[0] >> 3 == 2 and
                              ((raw[0] & 7) << 1 | raw[1] >> 7) < 13 and
                              1 <= (raw[1] >> 3 & 15) <= 2 and raw[1] & 7 == 0)}


def _mp4_frames(track, reader, codec, tags, atoms, rate):
    """Use the decoder's sample timeline, including Apple gapless trimming."""
    atom = track[b'mdia', b'minf', b'stbl', b'stts']
    limit = 1_000_000 if codec == 'aac' else 128
    if not 16 <= atom.datalength <= 8 + limit * 8:
        raise ValueError('MP4 sample timing exceeds decoder bounds')
    reader.seek(atom.offset + atom.length - atom.datalength)
    header = reader.read(8)
    runs = (atom.datalength - 8) // 8
    if len(header) != 8 or struct.unpack('>II', header) != (0, runs) or atom.datalength % 8:
        raise ValueError('Invalid MP4 sample timing')
    frames = packets = 0
    regular, last = True, (0, 1024)
    for start in range(0, runs, 512):
        size = min(512, runs - start) * 8
        data = reader.read(size)
        if len(data) != size:
            raise ValueError('Truncated MP4 sample timing')
        for count, delta in struct.iter_unpack('>II', data):
            if not count or packets + count > 1_000_000 or not 0 < delta <= 4096:
                raise ValueError('MP4 sample timing exceeds decoder bounds')
            regular &= last[1] == 1024
            packets += count
            frames += count * delta
            last = count, delta
    if codec == 'aac' and not (regular and last[0] == 1 and last[1] < 1024):
        nominal = packets * 1024
        if abs(nominal - frames) >= 1024:
            raise ValueError('AAC sample timing disagrees with its packets')
        frames = nominal
    lead = tail = 0
    try:
        atom = track[b'edts',]
    except KeyError:
        atom = None
    if atom is not None:
        if atom.datalength > 4096:
            raise ValueError('Unsupported MP4 edit list')
        ok, data = atom.read(reader)
        if not ok:
            raise ValueError('Unsupported MP4 edit list')
        from mutagen.mp4 import Atoms
        edits = io.BytesIO(data)
        children = Atoms(edits).atoms
        if sum(child.length for child in children) != len(data):
            raise ValueError('Invalid MP4 edit list')
        entries = [child for child in children if child.name == b'elst']
        if len(entries) > 1:
            raise ValueError('Unsupported MP4 edit list')
        if entries:
            if entries[0].datalength != 20:
                raise ValueError('Unsupported MP4 edit list')
            ok, edit = entries[0].read(edits)
            if not ok or struct.unpack_from('>II', edit) != (0, 1) or int.from_bytes(edit[16:20], 'big') != 65536:
                raise ValueError('Unsupported MP4 edit list')
            lead = int.from_bytes(edit[12:16], 'big', signed=True)
            movie = atoms[b'moov', b'mvhd']
            reader.seek(movie.offset + movie.length - movie.datalength)
            header = reader.read(20) if movie.datalength >= 20 else b''
            if len(header) != 20 or header[:4] != bytes(4):
                raise ValueError('Unsupported MP4 movie timing')
            scale = int.from_bytes(header[12:16], 'big')
            if not 0 < scale <= 1_000_000:
                raise ValueError('Unsupported MP4 movie timescale')
            difference = (frames - lead) * scale - int.from_bytes(edit[8:12], 'big') * rate
            if abs(difference) >= rate:
                if codec != 'aac' or not 0 < difference < 1024 * scale:
                    raise ValueError('MP4 edit duration disagrees with sample timing')
                tail = difference // scale
    if not 0 <= lead < frames or not 0 <= tail < frames - lead or (codec == 'alac' and lead):
        raise ValueError('Invalid MP4 lead trim')
    gapless = tags.get('----:com.apple.iTunes:iTunSMPB') if codec == 'aac' else None
    if gapless is not None:
        if len(gapless) != 1 or len(gapless[0]) > 248 or getattr(gapless[0], 'dataformat', 1) != 1:
            raise ValueError('Invalid Apple gapless metadata')
        fields = [field for field in bytes(gapless[0]).split(b' ') if field][:4]
        if len(fields) != 4 or any(re.fullmatch(rb'[0-9a-fA-F]{1,16}', field) is None for field in fields):
            raise ValueError('Invalid Apple gapless metadata')
        reserved, delay, padding, total = (int(field, 16) for field in fields)
        if reserved or max(delay, padding) > 0xffffffff or not total or total + delay + padding != frames or (lead and lead != delay):
            raise ValueError('Apple gapless metadata disagrees with sample timing')
        return total
    return frames - lead - tail


def _quick_metadata(path: Path, cancelled=lambda: False, *, include_cover=False,
                    allow_unknown=False) -> dict | None:
    """Use optional in-process tag readers for well identified containers."""
    try:
        import mutagen
    except ImportError:
        return None
    # Raw AAC still needs probing; MP4's sample entry identifies its codec.
    if path.suffix.lower() not in (".mp3", ".m4a", ".m4b", ".flac", ".flc", ".ogg", ".opus", ".ops", ".wv"):
        return None
    stream = None
    try:
        stream = path.open('rb')
        reader = _CancellableReader(stream, cancelled)
        audio = mutagen.File(reader, easy=path.suffix.lower() not in ('.m4a', '.m4b'))
        _check_cancel(cancelled)
        mp3_cover = None
        if include_cover and type(audio).__name__ == 'EasyMP3' and audio.tags is not None:
            # EasyID3 already holds the loaded ID3 frames. Its instance-only
            # fallback exposes APIC without changing the normal tag mappings.
            easy_tags = audio.tags
            previous_fallback = easy_tags.GetFallback
            def picture_frames(id3, key):
                if key == 'linnet_apic':
                    return id3.getall('APIC')
                if previous_fallback is not None:
                    return previous_fallback(id3, key)
                raise KeyError(key)
            easy_tags.GetFallback = picture_frames
            pictures = easy_tags['linnet_apic']
            if pictures:
                picture = next((p for p in pictures if p.type == 3), pictures[0])
                data = picture.data
                mp3_cover = data if 0 < len(data) <= 32 * 1024 * 1024 else None
        codec_name = {"MP3": "mp3", "EasyMP3": "mp3", "FLAC": "flac", "OggVorbis": "vorbis",
                      "OggOpus": "opus", "WavPack": "wavpack"}.get(type(audio).__name__)
        if type(audio).__name__ == "MP4":
            codec_name = {"mp4a.40.2": "aac", "mp4a": "aac", "alac": "alac"}.get(audio.info.codec)
            from mutagen.mp4 import Atoms
            atoms = Atoms(reader)
            tracks = list(atoms[b'moov'].findall(b'trak'))
            audio_tracks = 0
            audio_track = None
            for track in tracks:
                ok, handler = track[b'mdia', b'hdlr'].read(reader)
                if not ok:
                    return None
                if handler[8:12] == b'soun':
                    audio_tracks += 1
                    audio_track = track
            if audio_tracks != 1:
                return None
        if codec_name is None and allow_unknown:
            codec_name = 'unknown'
        if codec_name is None:
            return None
        info = audio.info
        rate = int(getattr(info, "sample_rate", 48000 if codec_name == "opus" else 0))
        channels = int(info.channels)
        bits = int(getattr(info, "bits_per_sample", 0))
        if not 1 <= rate <= 384000 or not 1 <= channels <= 8 or not 0 <= bits <= 64:
            return None
        if type(audio).__name__ == 'MP4':
            raw = audio.tags or {}
            tags = {key: str(raw[atom][0]) for atom, key in {
                '\xa9nam': 'title', '\xa9ART': 'artist', '\xa9alb': 'album',
                'aART': 'album_artist', '\xa9gen': 'genre', '\xa9day': 'date',
                '\xa9wrt': 'composer'}.items() if raw.get(atom)}
            tags['compilation'] = '1' if raw.get('cpil') else '0'
            for atom, key in (('trkn', 'track'), ('disk', 'disc')):
                if raw.get(atom):
                    tags[key] = str(raw[atom][0][0])
        else:
            tags = {str(k).lower(): str(v[0]) if isinstance(v, list) and v else str(v)
                    for k, v in (audio.tags or {}).items()}
        for key in ("track", "disc"):
            if key + "number" in tags:
                tags[key] = tags[key + "number"]
        mp4_config = {}
        if codec_name in ('aac', 'alac'):
            mp4_config = {'container': 'mp4', 'mp4_config_version': 2,
                          **_mp4_audio_config(audio_track, reader, codec_name)}
        result = dict(codec=CODECS[codec_name], channels=channels, rate=rate, bits=bits,
                      **(_flac_bounds(path) if codec_name == 'flac' else {}),
                      **({'profile': 'AAC LC' if mp4_config.get('aac_config_ok') else
                          getattr(info, 'codec_description', None)} if codec_name == 'aac' else {}),
                      **mp4_config,
                      frames=(_mp4_frames(audio_track, reader, codec_name, audio.tags or {}, atoms, rate)
                              if codec_name in ('aac', 'alac') else
                              _quick_frames(path, info, codec_name, rate, cancelled, stream)), data_offset=0,
                      data_bytes=path.stat().st_size, tags=tags)
        if include_cover and type(audio).__name__ == 'EasyMP3':
            result['_embedded_cover'] = mp3_cover
        elif include_cover and type(audio).__name__ == 'MP4':
            covers = (audio.tags or {}).get('covr') or ()
            cover = bytes(covers[0]) if covers else None
            result['_embedded_cover'] = cover if cover is not None and 0 < len(cover) <= 32 * 1024 * 1024 else None
        return result
    except (mutagen.MutagenError, ValueError, AttributeError, TypeError, KeyError, struct.error):
        return None
    finally:
        if stream is not None:
            stream.close()


_EMBEDDED_PREFETCH_BYTES = 4 * 1024 * 1024
_EMBEDDED_FALLBACK = object()


class _MetadataCache:
    """Disposable acceleration only; unreadable caches never block an import."""
    def __init__(self, path: Path, *, source_root=None, source_scope=None):
        self.connection = None
        # A guarded receipt can bind Apple's and native USB's volume names.
        # Each cache hit still requires the complete current file stamp.
        self.source_keys = ({str(source_root): source_scope}
                            if source_root is not None and source_scope else {})
        self.pending_writes = 0
        self.coverflow_writes = 0
        self.render_writes = 0
        self.artwork_bytes = None
        self.render_ready = False
        self.status = "enabled"
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.connection = sqlite3.connect(path, timeout=1)
            self.connection.execute("CREATE TABLE IF NOT EXISTS metadata "
                "(source TEXT, relative TEXT, size INTEGER, mtime INTEGER, parser TEXT, "
                "value TEXT, PRIMARY KEY(source, relative, parser))")
            self.connection.execute("CREATE TABLE IF NOT EXISTS coverflow "
                "(key BLOB PRIMARY KEY, value BLOB, digest BLOB)")
            self._trim_coverflow()
            self.connection.commit()
        except (OSError, sqlite3.Error):
            self._disable()

    def _disable(self):
        self.status = "disabled: cache unavailable; metadata read directly"
        if self.connection is not None:
            try:
                self.connection.rollback()
            except sqlite3.Error:
                pass
        self.close()

    def _commit(self):
        if self.coverflow_writes:
            self._trim_coverflow()
        if self.render_writes:
            self._trim_renders()
        self.connection.commit()
        self.pending_writes = 0
        self.coverflow_writes = 0
        self.render_writes = 0

    def _written(self, *, coverflow=False, render=False):
        self.pending_writes += 1
        if coverflow:
            self.coverflow_writes += 1
        if render:
            self.render_writes += 1
        if self.pending_writes >= 128:
            self._commit()

    @staticmethod
    def _valid(value, relative, stat):
        if not isinstance(value, dict):
            return False
        limits = {"codec": (1, 255), "channels": (1, 8), "rate": (1, 384000),
                  "bits": (0, 64), "frames": (0, 0xFFFFFFFFFFFFFFFF),
                  "data_offset": (0, stat.st_size), "data_bytes": (0, stat.st_size)}
        for key, (minimum, maximum) in limits.items():
            item = value.get(key)
            if type(item) is not int or not minimum <= item <= maximum:
                return False
        if value["codec"] not in CODECS.values() or value["codec"] == CODECS["unknown"]:
            return False
        if value["data_offset"] + value["data_bytes"] > stat.st_size:
            return False
        suffix = Path(relative).suffix.lower()
        pcm = suffix in (".wav", ".aif", ".aiff", ".aifc")
        if pcm:
            if value.get('container') not in ('wav', 'wav-extensible', 'aiff', 'aifc'):
                return False
            # AIFF/AIFC metadata must preserve the exact sample rate.
            if suffix != '.wav' and value.get('aiff_rate_exact') is not True:
                return False
            if value["codec"] not in (1, 2, 3, 4) or value["bits"] not in (8, 16, 24, 32):
                return False
            if value["codec"] in (3, 4) and value["bits"] != 32:
                return False
            align = value["channels"] * (value["bits"] // 8)
            if value["frames"] * align != value["data_bytes"]:
                return False
        elif (value["codec"] < 16 or value["data_offset"] != 0 or
              value["data_bytes"] != stat.st_size or value["frames"] == 0):
            return False
        if value['codec'] == CODECS['aac'] and not isinstance(value.get('profile'), str):
            return False
        if value['codec'] == CODECS['aac'] and value.get('container') == 'mp4' and type(value.get('aac_config_ok')) is not bool:
            return False
        if value['codec'] in (CODECS['aac'], CODECS['alac']) and value.get('container') == 'mp4' and value.get('mp4_config_version') != 2:
            return False
        if value['codec'] == CODECS['aac'] and value.get('container') == 'adts' and type(value.get('adts_header_ok')) is not bool:
            return False
        if value['codec'] == CODECS['alac'] and type(value.get('alac_frame_ok')) is not bool:
            return False
        if value['codec'] == CODECS['flac'] and not all(
                type(value.get(key)) is int and 0 <= value[key] <= 0xFFFFFF
                for key in ('min_blocksize', 'max_blocksize', 'max_framesize')):
            return False
        tags = value.get("tags")
        return (isinstance(tags, dict) and all(isinstance(k, str) and isinstance(v, str)
                                             for k, v in tags.items()))

    def _source_key(self, source):
        name = str(source)
        if name not in self.source_keys:
            self.source_keys[name] = cache_volume_scope(source)
        return self.source_keys[name]

    def _embedded_source_key(self, path):
        name = str(path)
        drive, relative = os.path.splitdrive(name)
        if not drive or not relative.startswith(('\\', '/')):
            return name
        relative = relative.lstrip('\\/').replace('\\', '/')
        return f'{self._source_key(Path(drive + os.sep))}:{relative}'

    def prefetch(self, source, relatives):
        """Fetch a bounded window without trusting cached file timestamps."""
        if self.connection is None or not relatives:
            return {}
        if len(relatives) > 128:
            raise ValueError("metadata cache window exceeds 128 tracks")
        try:
            placeholders = ",".join("?" for _ in relatives)
            return {relative: (size, mtime, value) for relative, size, mtime, value in
                    self.connection.execute(
                        "SELECT relative,size,mtime,value FROM metadata WHERE source=? "
                        f"AND parser=? AND relative IN ({placeholders})",
                        (self._source_key(source), "existing", *relatives))}
        except sqlite3.Error:
            self._disable()
            return {}

    def get(self, source, relative, stat, *, prefetched=None):
        if self.connection is None:
            return None
        try:
            if prefetched is None:
                row = self.connection.execute("SELECT value FROM metadata WHERE source=? "
                    "AND relative=? AND size=? AND mtime=? AND parser=?",
                    (self._source_key(source), relative, stat.st_size, stat.st_mtime_ns, "existing")).fetchone()
            else:
                saved = prefetched.get(relative)
                row = (saved[2],) if saved and saved[:2] == (stat.st_size, stat.st_mtime_ns) else None
            if row is None:
                return None
            value = json.loads(row[0])
            if not isinstance(value, dict) or value.pop('_source_stamp', None) != list(file_stamp(stat)):
                return None
            return value if self._valid(value, relative, stat) else None
        except (ValueError, TypeError, OverflowError):
            return None
        except sqlite3.Error:
            self._disable()
            return None

    def put(self, source, relative, stat, metadata):
        if self.connection is None:
            return
        try:
            self.connection.execute("INSERT OR REPLACE INTO metadata VALUES (?,?,?,?,?,?)",
                (self._source_key(source), relative, stat.st_size, stat.st_mtime_ns, "existing",
                 json.dumps(dict(metadata, _source_stamp=file_stamp(stat)))))
            # The cache is disposable; amortize durable writes across tracks.
            # close() also commits the final partial group on cancellation.
            self._written()
        except OverflowError:
            # NTFS timestamps can exceed SQLite's signed 64-bit nanoseconds.
            return
        except sqlite3.Error:
            self._disable()

    def _prepare_embedded(self, now):
        # Keep original encoded bytes so all target artwork recipes remain exact.
        # Equal covers share one blob; source stamps expire after one hour even
        # when repeated imports keep using them. A refresh always rereads tags.
        if self.connection is not None and self.artwork_bytes is None:
            # Library browsing uses this metadata cache too. Defer artwork
            # maintenance until preparation actually needs embedded covers.
            try:
                self.connection.execute("CREATE TABLE IF NOT EXISTS embedded_sources "
                    "(path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, checked REAL, digest BLOB, stamp TEXT)")
                self.connection.execute("CREATE TABLE IF NOT EXISTS embedded_images "
                    "(digest BLOB PRIMARY KEY, value BLOB)")
                self.connection.execute("DELETE FROM embedded_sources WHERE stamp IS NULL OR checked < ? OR checked > ?",
                                        (now - 3600, now))
                self.connection.execute("DELETE FROM embedded_images WHERE digest NOT IN "
                                        "(SELECT digest FROM embedded_sources)")
                self.artwork_bytes = self.connection.execute(
                    "SELECT COALESCE(SUM(length(value)),0) FROM embedded_images").fetchone()[0]
                self._commit()
            except sqlite3.Error:
                self._disable()

    def prefetch_embedded(self, paths):
        """Share bounded artwork blobs across the next small source window."""
        if not paths:
            return {}
        self._prepare_embedded(time.time())
        if self.connection is None:
            return {}
        keys = [self._embedded_source_key(path) for path in paths]
        try:
            placeholders = ",".join("?" for _ in keys)
            rows = list(self.connection.execute(
                "SELECT s.path,s.size,s.mtime,s.checked,s.digest,length(i.value),s.stamp "
                "FROM embedded_sources s JOIN embedded_images i ON i.digest=s.digest "
                f"WHERE s.path IN ({placeholders}) AND length(i.value)<=?",
                (*keys, 32 * 1024 * 1024)))
            chosen = {}
            held = 0
            for _, _, _, _, digest, size, _ in rows:
                if digest not in chosen and isinstance(size, int) and size >= 0 and \
                        held + size <= _EMBEDDED_PREFETCH_BYTES:
                    chosen[digest] = size
                    held += size
            images = {}
            if chosen:
                # Recheck lengths in SQL: another cache writer may replace a
                # blob between the size query and this bounded materialization.
                wanted = ",".join("(?,?)" for _ in chosen)
                arguments = tuple(part for pair in chosen.items() for part in pair)
                for digest, image in self.connection.execute(
                        f"WITH wanted(digest,size) AS (VALUES {wanted}) "
                        "SELECT i.digest,i.value FROM wanted w JOIN embedded_images i "
                        "ON i.digest=w.digest WHERE length(i.value)=w.size", arguments):
                    if isinstance(image, bytes) and len(image) == chosen[digest]:
                        images[digest] = image
            result = dict.fromkeys(keys)
            for key, size, mtime, checked, digest, _, stamp in rows:
                value = images.get(digest, _EMBEDDED_FALLBACK)
                result[key] = ((size, mtime, value, digest, checked, stamp)
                               if value is not _EMBEDDED_FALLBACK else _EMBEDDED_FALLBACK)
            return result
        except sqlite3.Error:
            self._disable()
            return {}

    def embedded_cover_lookup(self, path, before, *, bypass=False, prefetched=None):
        now = time.time()
        self._prepare_embedded(now)
        if self.connection is not None and not bypass:
            try:
                if prefetched is None:
                    saved = _EMBEDDED_FALLBACK
                else:
                    saved = prefetched.get(self._embedded_source_key(path), _EMBEDDED_FALLBACK)
                if saved is _EMBEDDED_FALLBACK:
                    row = self.connection.execute(
                        "SELECT i.value,i.digest,s.checked,s.stamp FROM embedded_sources s "
                        "JOIN embedded_images i ON i.digest=s.digest "
                        "WHERE s.path=? AND s.size=? AND s.mtime=? AND length(i.value)<=?",
                        (self._embedded_source_key(path), before.st_size, before.st_mtime_ns,
                         32 * 1024 * 1024)).fetchone()
                else:
                    row = saved[2:] if saved and saved[:2] == (before.st_size, before.st_mtime_ns) else None
                if (row and row[3] == json.dumps(file_stamp(before)) and
                        type(row[2]) in (int, float) and 0 <= now - row[2] <= 3600 and
                        isinstance(row[0], bytes) and hashlib.sha256(row[0]).digest() == row[1]):
                    return True, row[0] or None
            except OverflowError:
                pass
            except sqlite3.Error:
                self._disable()
        return False, None

    def embedded_cover_store(self, path, before, cover):
        now = time.time()
        after = path.stat()
        if file_stamp(before) != file_stamp(after):
            raise ValueError("audio source changed during artwork extraction")
        if self.connection is not None:
            try:
                value = cover or b""
                digest = hashlib.sha256(value).digest()
                exists = self.connection.execute(
                    "SELECT length(value) FROM embedded_images WHERE digest=?", (digest,)).fetchone()
                if exists or self.artwork_bytes + len(value) <= 128 * 1024 * 1024:
                    self.connection.execute("INSERT INTO embedded_images VALUES (?,?) "
                        "ON CONFLICT(digest) DO UPDATE SET value=excluded.value "
                        "WHERE embedded_images.value IS NOT excluded.value", (digest, value))
                    self.artwork_bytes += len(value) - (exists[0] or 0) if exists else len(value)
                    self.connection.execute("INSERT OR REPLACE INTO embedded_sources VALUES (?,?,?,?,?,?)",
                        (self._embedded_source_key(path), before.st_size, before.st_mtime_ns,
                         now, digest, json.dumps(file_stamp(before))))
                    self._written()
            except OverflowError:
                pass
            except sqlite3.Error:
                self._disable()
        return cover

    def embedded_cover(self, path, before, *, bypass=False):
        hit, cover = self.embedded_cover_lookup(path, before, bypass=bypass)
        return cover if hit else self.embedded_cover_store(path, before, _embedded_cover(path))

    def coverflow_get(self, key, value_bytes=14080):
        if self.connection is None:
            return None
        try:
            row = self.connection.execute("SELECT value, digest FROM coverflow WHERE key=?", (key,)).fetchone()
            if (row and isinstance(row[0], bytes) and
                    (len(row[0]) == value_bytes if value_bytes is not None else 0 < len(row[0]) <= 23660) and
                    hashlib.sha256(row[0]).digest() == row[1]):
                return row[0]
        except sqlite3.Error:
            self._disable()
        return None

    def _trim_coverflow(self):
        count = self.connection.execute("SELECT COUNT(*) FROM coverflow").fetchone()[0]
        if count > COVERFLOW_CACHE_MAX_ENTRIES:
            self.connection.execute("DELETE FROM coverflow WHERE rowid IN "
                "(SELECT rowid FROM coverflow ORDER BY rowid LIMIT ?)",
                (count - COVERFLOW_CACHE_MAX_ENTRIES,))

    def coverflow_put(self, key, value):
        if self.connection is None:
            return
        try:
            self.connection.execute("INSERT OR REPLACE INTO coverflow VALUES (?,?,?)",
                (key, value, hashlib.sha256(value).digest()))
            self._written(coverflow=True)
        except sqlite3.Error:
            self._disable()

    def _render_table(self):
        if self.connection is None:
            return False
        if not self.render_ready:
            try:
                self.connection.execute("CREATE TABLE IF NOT EXISTS artwork_renders "
                    "(key BLOB PRIMARY KEY, pixels BLOB, flow BLOB, digest BLOB)")
                self.render_ready = True
            except sqlite3.Error:
                self._disable()
        return self.render_ready

    def render_get(self, key, pixel_bytes, with_flow, flow_side=96):
        if not self._render_table():
            return None
        try:
            row = self.connection.execute(
                "SELECT pixels,flow,digest FROM artwork_renders WHERE key=? "
                "AND typeof(pixels)='blob' AND length(pixels)=? "
                "AND typeof(flow)='blob' AND length(flow)=? "
                "AND typeof(digest)='blob' AND length(digest)=32",
                (key, pixel_bytes, flow_side * flow_side * 3 if with_flow else 0)).fetchone()
            if (row and isinstance(row[0], bytes) and len(row[0]) == pixel_bytes and
                    isinstance(row[1], bytes) and len(row[1]) == (flow_side * flow_side * 3 if with_flow else 0) and
                    hashlib.sha256(row[0] + row[1]).digest() == row[2]):
                return row[0], row[1]
        except sqlite3.Error:
            self._disable()
        return None

    def _trim_renders(self):
        count = self.connection.execute("SELECT COUNT(*) FROM artwork_renders").fetchone()[0]
        if count > RENDER_CACHE_MAX_ENTRIES:
            self.connection.execute("DELETE FROM artwork_renders WHERE rowid IN "
                "(SELECT rowid FROM artwork_renders ORDER BY rowid LIMIT ?)",
                (count - RENDER_CACHE_MAX_ENTRIES,))

    def render_put(self, key, pixels, flow):
        if not self._render_table():
            return
        try:
            self.connection.execute("INSERT OR REPLACE INTO artwork_renders VALUES (?,?,?,?)",
                (key, pixels, flow, hashlib.sha256(pixels + flow).digest()))
            self._written(render=True)
        except sqlite3.Error:
            self._disable()

    def close(self):
        if self.connection is not None:
            try:
                if self.status == "enabled":
                    self._commit()
            except sqlite3.Error:
                pass
            finally:
                try:
                    self.connection.close()
                except sqlite3.Error:
                    pass
            self.connection = None


_SHORT_MEDIA_COMPONENT = r"[A-Z0-9_~!#$%&'()@^`{}-]{1,8}(?:\.[A-Z0-9_~!#$%&'()@^`{}-]{1,3})?"
_SAFE_MEDIA_PATH = re.compile(
    rf"/(?:\.LINNET|{_SHORT_MEDIA_COMPONENT})(?:/{_SHORT_MEDIA_COMPONENT}){{0,15}}")


def safe_media_path(path: str) -> bool:
    """Match the firmware's bounded FAT short-name path contract."""
    return len(path) <= 255 and _SAFE_MEDIA_PATH.fullmatch(path) is not None


class UnsupportedMediaPath(ValueError):
    """A resolved existing filename cannot be opened by firmware."""


def device_media_path(path: Path, device_root: Path) -> str:
    """Resolve the real FAT alias; never invent short names for existing files."""
    root = Path(device_root).resolve(strict=True)
    return _device_media_path(path, root)


if os.name == "nt":
    from ctypes import wintypes

    class _FindData(ctypes.Structure):
        _fields_ = [
            ("attributes", wintypes.DWORD),
            ("creation", wintypes.FILETIME),
            ("access", wintypes.FILETIME),
            ("write", wintypes.FILETIME),
            ("size_high", wintypes.DWORD),
            ("size_low", wintypes.DWORD),
            ("reserved0", wintypes.DWORD),
            ("reserved1", wintypes.DWORD),
            ("name", ctypes.c_wchar * 260),
            ("alternate", ctypes.c_wchar * 14),
            ("file_type", wintypes.DWORD),
            ("creator_type", wintypes.DWORD),
            ("finder_flags", wintypes.WORD),
        ]

    _find_kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    _find_kernel.FindFirstFileW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(_FindData))
    _find_kernel.FindFirstFileW.restype = wintypes.HANDLE
    _find_kernel.FindNextFileW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_FindData))
    _find_kernel.FindNextFileW.restype = wintypes.BOOL
    _find_kernel.FindClose.argtypes = (wintypes.HANDLE,)
    _find_kernel.FindClose.restype = wintypes.BOOL


def _actual_short_component(path: Path) -> str:
    """Get the real alternate name even when the long name is 8.3-shaped."""
    data = _FindData()
    handle = _find_kernel.FindFirstFileW(str(path), ctypes.byref(data))
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return data.alternate or data.name
    finally:
        if not _find_kernel.FindClose(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def _short_names_in(folder: Path, aliases: dict[Path, str]) -> None:
    """Read every FAT alias in one directory query for the music compiler."""
    data = _FindData()
    handle = _find_kernel.FindFirstFileW(str(folder / '*'), ctypes.byref(data))
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        while True:
            if data.name not in ('.', '..'):
                aliases[folder / data.name] = data.alternate or data.name
            if not _find_kernel.FindNextFileW(handle, ctypes.byref(data)):
                if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise ctypes.WinError(ctypes.get_last_error())
                break
    finally:
        if not _find_kernel.FindClose(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def _device_media_path(path: Path, root: Path, alias_cache: dict[Path, str] | None = None,
                       *, canonical: Path | None = None, parents=None) -> str:
    """Resolve one file against the compiler's already-resolved device root."""
    from .publish import _plain
    path = Path(path).absolute()
    _plain(path, root, parents=parents)
    path = Path(canonical) if canonical is not None else path.resolve(strict=True)
    path.relative_to(root)
    if os.name == "nt" and alias_cache is not None:
        actual = []
        cursor = root
        for part in path.relative_to(root).parts:
            parent = cursor
            cursor /= part
            if cursor not in alias_cache:
                _short_names_in(parent, alias_cache)
            if cursor not in alias_cache:
                raise ValueError(f"Cannot resolve the iPod FAT short name: {cursor}")
            actual.append(alias_cache[cursor])
        short_path = root.joinpath(*actual)
        if short_path != path:
            _plain(short_path, root, parents=parents)
            if short_path.resolve(strict=True) != path:
                raise ValueError(f"FAT short name changed during compilation: {path}")
        path = short_path
    elif os.name == "nt":
        get_short = ctypes.windll.kernel32.GetShortPathNameW
        get_short.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
        get_short.restype = ctypes.c_uint32
        buffer = ctypes.create_unicode_buffer(32768)
        length = get_short(str(path), buffer, len(buffer))
        if not length or length >= len(buffer):
            raise ValueError(f"Cannot resolve the iPod FAT short name: {path}")
        long_parts = path.relative_to(root).parts
        short_parts = Path(buffer.value).relative_to(root).parts
        if len(long_parts) != len(short_parts):
            raise ValueError(f"FAT short path changed its component count: {path}")
        actual = []
        cursor = root
        for long_part, short_part in zip(long_parts, short_parts):
            cursor /= long_part
            if long_part.casefold() == short_part.casefold():
                # GetShortPathNameW may retain an 8.3-shaped long name despite
                # a different primary FAT alias. The firmware opens aliases.
                if alias_cache is not None and cursor in alias_cache:
                    short_part = alias_cache[cursor]
                else:
                    short_part = _actual_short_component(cursor)
                    if alias_cache is not None:
                        alias_cache[cursor] = short_part
            actual.append(short_part)
        path = root.joinpath(*actual)
    relative = "/" + path.relative_to(root).as_posix().upper()
    if not safe_media_path(relative):
        raise UnsupportedMediaPath(f"Existing file has no firmware-compatible FAT short path: {path}")
    return relative


def compile_music(source: Path, output: Path, *, ffprobe: str | None = None,
                  artwork: bool = True, playlists: dict[str, list[str]] | None = None,
                  target: str | None = None,
                  device_root: Path | None = None, progress: Callable[[dict], None] | None = None,
                  verification: str | None = None, cache_path: Path | None = None,
                  cache_scope: str | None = None,
                  bypass_cache: bool = False,
                  include_roots: list[Path] | tuple[Path, ...] | None = None,
                  excluded: set[str] | tuple[str, ...] | None = None,
                  cancelled: Callable[[], bool] = lambda: False) -> dict:
    """Prepare and validate a complete stage before replacing the old directory.

    An interrupted directory swap leaves a sibling backup, restored on the next
    import if the output is absent. Concurrent imports into one output are unsupported.
    """
    from .publish import validate_stage

    if target is not None and target not in TARGETS:
        raise ValueError("unsupported artwork target")
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_dir() or output == source or source in output.parents or output in source.parents:
        raise ValueError("output must be outside the source music directory")
    roots = None
    if include_roots is not None:
        roots = []
        for value in include_roots:
            folder = Path(value).resolve(strict=True)
            if not folder.is_dir() or not (folder == source or source in folder.parents):
                raise ValueError("included music root must be a directory inside the source")
            if folder not in roots:
                roots.append(folder)
        if not roots:
            raise ValueError("at least one included music root is required")
    output.parent.mkdir(parents=True, exist_ok=True)
    verification = verification or ("metadata" if device_root is not None else "full")
    if verification not in ("metadata", "full") or (device_root is None and verification != "full"):
        raise ValueError("verification must be full for staged files, metadata or full for existing music")
    cache = None
    backup = output.with_name(output.name + ".music-previous")
    if backup.exists() and not output.exists():
        backup.rename(output)
    if backup.exists():
        raise ValueError(f"previous music stage retained at {backup}; restore or archive it before importing")
    prepared = Path(tempfile.mkdtemp(prefix=output.name + ".music-prepare-", dir=output.parent))
    moved_previous = False
    try:
        if device_root is not None:
            cache_file = (Path(cache_path) if cache_path is not None else
                          output.with_name(output.name + ".metadata.sqlite3")).resolve()
            if any(cache_file == folder or folder in cache_file.parents or cache_file in folder.parents
                   for folder in (source, output, Path(device_root).resolve())):
                raise ValueError("metadata cache must be outside the iPod, source and staged output")
            cache = _MetadataCache(cache_file, source_root=source, source_scope=cache_scope)
        if output.exists():
            # Preserve unrelated staged content without copying obsolete music.
            managed = {"MUSIC", "ART", "COVERS.CFL", "BROWSE.BIN", "LIBRARY.PDB", "LIBRARY.TMP", "music-manifest.json"}
            shutil.copytree(output, prepared, dirs_exist_ok=True, symlinks=True,
                            ignore=lambda directory, names: [name for name in names
                                if Path(directory) == output and name in managed])
        previous_database = (Path(device_root) / '.LINNET' / 'LIBRARY.PDB') if device_root is not None else output / 'LIBRARY.PDB'
        recent_ids = _recent_track_ids(previous_database)
        stop_metadata = threading.Event()
        def stop_requested():
            return stop_metadata.is_set() or cancelled()
        with ThreadPoolExecutor(max_workers=METADATA_WORKERS, thread_name_prefix="music-metadata") as metadata_pool:
            try:
                report = _compile_music_stage(source, prepared, metadata_pool=metadata_pool, ffprobe=ffprobe,
                                          artwork=artwork, playlists=playlists, target=target, device_root=device_root, progress=progress,
                                           verification=verification, cache=cache, bypass_cache=bypass_cache,
                                           include_roots=roots, excluded=excluded, recent_ids=recent_ids,
                                           cancelled=stop_requested)
            finally:
                stop_metadata.set()
        report["database"] = str(output / "LIBRARY.PDB")
        (prepared / "music-manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        _check_cancel(cancelled)
        validate_stage(prepared, device_root=device_root)
        _check_cancel(cancelled)
        try:
            if output.exists():
                output.rename(backup)
                moved_previous = True
            prepared.rename(output)
        except BaseException:
            if backup.exists() and not output.exists():
                try:
                    backup.rename(output)
                except OSError as recovery_error:
                    raise RuntimeError(f"music stage publication failed; restore previous stage from {backup}") from recovery_error
            raise
        # Publication has committed. Cleanup failure must not turn a successful
        # replacement into a failed-import result; the backup remains recoverable.
        if moved_previous:
            shutil.rmtree(backup, ignore_errors=True)
        return report
    finally:
        if cache is not None:
            cache.close()
        if prepared.exists():
            shutil.rmtree(prepared, ignore_errors=True)


def _read_metadata(path: Path, ffprobe: str | None, existing: bool, cancelled=lambda: False,
                   *, include_cover=False) -> dict:
    _check_cancel(cancelled)
    if path.suffix.lower() in (".wav", ".aif", ".aiff", ".aifc"):
        try:
            metadata = _pcm(path, cancelled)
        except (ValueError, struct.error):
            if not ffprobe:
                raise
            metadata = _probe(path, ffprobe, cancelled=cancelled, allow_unknown=True)
        if include_cover:
            metadata['_embedded_cover'] = _embedded_cover(path)
            try:
                import mutagen
                tagged = mutagen.File(path)
                id3 = getattr(tagged, 'tags', None)
                if id3 and hasattr(id3, 'getall'):
                    for frame, key in (('TIT2', 'title'), ('TPE1', 'artist'), ('TALB', 'album'),
                                       ('TPE2', 'album_artist'), ('TCON', 'genre'), ('TDRC', 'date'),
                                       ('TRCK', 'track'), ('TPOS', 'disc'), ('TCMP', 'compilation'),
                                       ('TCOM', 'composer')):
                        values = id3.getall(frame)
                        if values and getattr(values[0], 'text', None):
                            metadata['tags'][key] = str(values[0].text[0])
                    for value in id3.getall('TXXX'):
                        key = value.desc.lower()
                        if key in ('replaygain_track_gain', 'replaygain_album_gain') and value.text:
                            metadata['tags'][key] = str(value.text[0])
            except Exception:
                pass
        return metadata
    metadata = _quick_metadata(path, cancelled, include_cover=include_cover) if existing else None
    _check_cancel(cancelled)
    if metadata is None and ffprobe:
        metadata = _probe(path, ffprobe, exact_frames=not existing, cancelled=cancelled)
    if metadata is None:
        raise ValueError(f"{path}: compressed metadata requires explicit ffprobe executable")
    if not metadata["frames"]:
        if not ffprobe:
            raise ValueError(f"{path}: track duration requires explicit ffprobe executable")
        metadata["frames"] = _decoded_frames(path, ffprobe, recover=existing,
                                               codec=metadata["codec"], cancelled=cancelled)
    if include_cover and '_embedded_cover' not in metadata:
        metadata['_embedded_cover'] = _embedded_cover(path)
    _check_cancel(cancelled)
    return metadata


_METADATA_COVER = object()


def _metadata_rows(paths, source, ffprobe, existing, cache, bypass_cache, pool, progress,
                   cancelled, prefetch_cover, needs_embedded):
    # Keep SQLite and progress callbacks on the caller's thread. Bound both
    # concurrent device reads and queued results, including on cancellation.
    pending = deque()
    iterator = iter(enumerate(paths))
    cache_rows = {}
    cache_relatives = []
    cover_rows = {}
    def enqueue():
        nonlocal cache_rows, cache_relatives, cover_rows
        _check_cancel(cancelled)
        item = next(iterator, None)
        if item is None:
            return
        number, path = item
        if cache and not bypass_cache and number % 128 == 0:
            cache_relatives = [p.relative_to(source).as_posix()
                               for p in paths[number:number+128]]
            cache_rows = cache.prefetch(source, cache_relatives)
        if cache and not bypass_cache and number % METADATA_WORKERS == 0:
            cover_rows = cache.prefetch_embedded(
                [p for p in paths[number:number+METADATA_WORKERS] if needs_embedded(p)])
        before = path.lstat()
        resolved = path.resolve()
        if stat.S_ISLNK(before.st_mode) or source not in resolved.parents:
            raise ValueError("audio paths must remain in the source directory")
        relative = (cache_relatives[number % 128] if cache and not bypass_cache
                    else path.relative_to(source).as_posix())
        metadata = cache.get(source, relative, before, prefetched=cache_rows) if cache and not bypass_cache else None
        cover = prefetch_cover(path, before, pool, prefetched=cover_rows,
                               defer=metadata is None and path.suffix.lower() in ('.m4a', '.m4b', '.mp3'))
        future = (pool.submit(_read_metadata, path, ffprobe, existing, cancelled,
                              include_cover=cover is _METADATA_COVER)
                  if metadata is None else None)
        pending.append((number, path, resolved, before, metadata, future, cover))
    try:
        for _ in range(METADATA_WORKERS):
            enqueue()
        while pending:
            number, path, resolved, before, metadata, future, cover = pending[0]
            cached = metadata is not None
            if progress:
                progress(dict(phase="music", current=number, total=len(paths), file=str(path), cached=cached))
            try:
                if not cached:
                    while True:
                        _check_cancel(cancelled)
                        try:
                            metadata = future.result(timeout=0.1)
                            break
                        except FutureTimeout:
                            if future.done():
                                metadata = future.result()
                                break
                            continue
                    if cover is _METADATA_COVER:
                        embedded = metadata.pop('_embedded_cover')
                        cover = (True, cache.embedded_cover_store(path, before, embedded)
                                 if cache else embedded)
            except (ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                from . import SourceError
                raise SourceError(f"{path}: {error}", path) from error
            pending.popleft()
            enqueue()
            yield number, path, resolved, before, metadata, cached, cover
    finally:
        for _, _, _, _, _, future, cover in pending:
            if future is not None:
                future.cancel()
            if cover is not None and cover is not _METADATA_COVER and not isinstance(cover, tuple):
                cover.cancel()


def _compile_music_stage(source: Path, output: Path, *, metadata_pool, ffprobe: str | None = None,
                         artwork: bool = True, playlists: dict[str, list[str]] | None = None,
                  target: str | None = None,
                  device_root: Path | None = None, progress: Callable[[dict], None] | None = None,
                   verification: str = "full", cache: _MetadataCache | None = None,
                   bypass_cache: bool = False,
                   include_roots: list[Path] | None = None,
                   excluded: set[str] | tuple[str, ...] | None = None,
                   recent_ids: list[int] | None = None,
                   cancelled: Callable[[], bool] = lambda: False) -> dict:
    """Stage source audio under MUSIC and write LIBRARY.PDB.

    Install MUSIC at /Music, ART below /.LINNET and LIBRARY.PDB at /.LINNET/LIBRARY.PDB.
    Source-relative playlist paths are exact; unsupported sources fail explicitly.
    """
    source, output = source.resolve(), output.resolve()
    if not source.is_dir() or output == source or source in output.parents or output in source.parents:
        raise ValueError("output must be outside the source music directory")
    if device_root is not None:
        device_root = Path(device_root).resolve(strict=True)
        source.relative_to(device_root)
    excluded = {Path(value).as_posix().casefold() for value in (excluded or ())}
    paths = []
    directories = list(include_roots or [source])
    while directories:
        _check_cancel(cancelled)
        with os.scandir(directories.pop()) as entries:
            for entry in entries:
                _check_cancel(cancelled)
                if entry.is_dir(follow_symlinks=False):
                    # Never recurse into symlinks or Windows directory junctions.
                    if not entry.is_symlink() and (os.name != 'nt' or
                            entry.stat(follow_symlinks=False).st_reparse_tag != stat.IO_REPARSE_TAG_MOUNT_POINT):
                        directories.append(entry.path)
                elif Path(entry.name).suffix.lower() in EXTENSIONS and entry.is_file():
                    path = Path(entry.path)
                    if path.relative_to(source).as_posix().casefold() in excluded:
                        continue
                    paths.append(path)
                    if len(paths) > MAX_TRACKS:
                        raise ValueError("library exceeds 100000 tracks")
    paths.sort(key=lambda p: p.relative_to(source).as_posix())
    output.mkdir(parents=True, exist_ok=True)
    if device_root is None:
        (output / "MUSIC").mkdir(exist_ok=True)
    pool = bytearray(b"\0")
    strings = {("", None): 0}

    def string(value: object, album_owner: str | None = None) -> int:
        value = _text(value)
        # Firmware compares album offsets at the sleep-timer boundary. Equal
        # titles from different album artists must retain distinct identities.
        key = (value, album_owner)
        if key not in strings:
            strings[key] = len(pool)
            pool.extend(value.encode("utf-8") + b"\0")
        return strings[key]

    records, entries, artwork_records = [], [], []
    coverflow_originals = {}
    coverflow_enabled = bool(target and TARGETS[target].get('capabilities', {}).get('cover_flow', False))
    art_cache: dict[Path, int] = {}
    from .retail_media import available_tracks, metadata as retail_metadata
    retail = available_tracks(device_root) if device_root is not None else {}
    retail_artwork = None
    folder_covers: dict[Path, Path | None] = {}
    file_covers: dict[Path, tuple[tuple[int, int, int, int], int]] = {}
    art_indices: dict[bytes, int] = {}
    art_directory = output / "ART"
    art_directory_ready = False
    embedded_cache: OrderedDict[bytes, int] = OrderedDict()
    embedded_cache_bytes = 0
    filenames: set[str] = set()
    filename_suffixes: dict[tuple[str, str], int] = {}
    stable_ids: set[int] = set()
    groups: dict[tuple[int, str, str], list[int]] = {}

    def needs_embedded(path: Path):
        if not artwork or path.parent in art_cache:
            return False
        folder = path.parent
        if folder not in folder_covers:
            folder_covers[folder] = next((p for p in (folder / name for name in COVER_NAMES)
                                          if p.is_file()), None)
        return folder_covers[folder] is None

    def prefetch_cover(path: Path, before, executor, *, prefetched=None, defer=False):
        if not needs_embedded(path):
            return None
        if cache is not None:
            hit, value = cache.embedded_cover_lookup(path, before, bypass=bypass_cache,
                                                     prefetched=prefetched)
            if hit:
                return (True, value)
        if defer:
            return _METADATA_COVER
        return executor.submit(_embedded_cover, path)

    def art_for(path: Path, before, prefetched_cover) -> int:
        nonlocal art_directory_ready, embedded_cache_bytes, retail_artwork
        if not artwork:
            return NO_INDEX
        folder = path.parent
        if folder in art_cache:
            return art_cache[folder]
        # Most MP3 albums carry embedded art. Search sidecar names once per
        # folder, but still inspect each track's own embedded cover.
        if folder not in folder_covers:
            candidates = [folder / name for name in COVER_NAMES]
            folder_covers[folder] = next((p for p in candidates if p.is_file()), None)
        cover = folder_covers[folder]
        sidecar = cover is not None
        embedded = None
        if cover is None:
            if prefetched_cover is None:
                embedded = (cache.embedded_cover(path, before, bypass=bypass_cache)
                            if cache else _embedded_cover(path))
            elif isinstance(prefetched_cover, tuple):
                embedded = prefetched_cover[1]
            else:
                while True:
                    _check_cancel(cancelled)
                    try:
                        embedded = prefetched_cover.result(timeout=0.1)
                        break
                    except FutureTimeout:
                        if prefetched_cover.done():
                            embedded = prefetched_cover.result()
                            break
                        continue
                if cache is not None:
                    embedded = cache.embedded_cover_store(path, before, embedded)
        if cover is None and embedded is None:
            key = str(path).casefold()
            if key in retail and retail_artwork is None:
                retail_artwork = retail_metadata(device_root)
            saved_cover = (retail_artwork or {}).get(key, {}).get('artwork')
            if saved_cover:
                cover = Path(saved_cover)
        if cover is None and embedded is None:
            return NO_INDEX
        # Compare the actual tag bytes; reuse conversion only within this build.
        if embedded is not None and embedded in embedded_cache:
            embedded_cache.move_to_end(embedded)
            return embedded_cache[embedded]
        cover_stamp = None
        if cover is not None:
            info = cover.stat()
            if info.st_size > 32 * 1024 * 1024:
                raise ValueError("cover source exceeds 32 MiB")
            cover_stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
            saved = file_covers.get(cover)
            # Apple and downloaded covers can serve many tracks or folders.
            # Reuse their conversion only within this build and this file stamp.
            if saved is not None and saved[0] == cover_stamp:
                if sidecar:
                    art_cache[folder] = saved[1]
                return saved[1]
        cover_bytes = None
        if (cover is not None and cache is not None and cache.connection is not None and
                not bypass_cache and cover_stamp[0] <= 4 * 1024 * 1024):
            with cover.open('rb') as reader:
                cover_bytes = reader.read(cover_stamp[0] + 1)
            after = cover.stat()
            if len(cover_bytes) != cover_stamp[0] or (
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino) != cover_stamp):
                raise ValueError('cover source changed during conversion')
        render_key = None
        rendered = None
        if (embedded is not None or cover_bytes is not None) and cache is not None and cache.connection is not None:
            try:
                from PIL import __version__ as pillow_version
            except ImportError as exc:
                raise ValueError("cover conversion requires Pillow, or artwork=False") from exc
            # Bump the recipe when playback or Cover Flow image processing changes.
            recipe = ("embedded-render-v4" if cover_bytes is None else "file-render-v4",
                      target, TARGETS[target]["media_art_side"] if target else 64,
                      TARGETS[target]["lcd_format"] if target else None,
                      coverflow_enabled, pillow_version)
            render_key = hashlib.sha256(repr(recipe).encode("utf-8") +
                                        hashlib.sha256(embedded if cover_bytes is None else cover_bytes).digest()).digest()
            if not bypass_cache:
                side = TARGETS[target]["media_art_side"] if target else 64
                pixel_bytes = side * side * 2 + (112 * 112 * 2 if side == 160 else 0) + (126 * 126 * 2 if target == 'classic6g-n25' else 0)
                rendered = cache.render_get(render_key, pixel_bytes, coverflow_enabled,
                                            130 if target and TARGETS[target].get('presentation_profile') == 'color320' else 96)
        try:
            if rendered is None:
                pixels, digest, flow = _cover_renders(io.BytesIO(cover_bytes) if cover_bytes is not None else
                                                       cover if cover is not None else io.BytesIO(embedded),
                                                       target, coverflow_enabled,
                                                       () if render_key is not None else art_indices)
                if render_key is not None:
                    cache.render_put(render_key, pixels, flow.tobytes() if flow is not None else b"")
            else:
                pixels, flow_bytes = rendered
                digest = hashlib.sha256(pixels).digest()
                if coverflow_enabled:
                    from PIL import Image
                    flow_side = 130 if target and TARGETS[target].get('presentation_profile') == 'color320' else 96
                    flow = Image.frombytes("RGB", (flow_side, flow_side), flow_bytes)
                else:
                    flow = None
        except ImportError as exc:
            raise ValueError("cover conversion requires Pillow, or artwork=False") from exc
        except (OSError, ValueError):
            if cover is not None:
                raise
            return NO_INDEX
        def remember_cover(index):
            nonlocal embedded_cache_bytes
            if cover is not None:
                file_covers[cover] = (cover_stamp, index)
            if sidecar:
                art_cache[folder] = index
            if embedded is not None:
                while embedded_cache and embedded_cache_bytes + len(embedded) > 32 * 1024 * 1024:
                    old, _ = embedded_cache.popitem(last=False)
                    embedded_cache_bytes -= len(old)
                embedded_cache[embedded] = index
                embedded_cache_bytes += len(embedded)

        if digest in art_indices:
            remember_cover(art_indices[digest])
            return art_indices[digest]
        name = digest[:4].hex().upper() + ".RGB"
        if not art_directory_ready:
            art_directory.mkdir(exist_ok=True)
            art_directory_ready = True
        destination = art_directory / name
        if destination.exists() and destination.read_bytes() != pixels:
            raise ValueError("art filename collision")
        destination.write_bytes(pixels)
        index = len(artwork_records)
        if coverflow_enabled:
            coverflow_originals[index] = flow
        side = TARGETS[target]["media_art_side"] if target else 64
        artwork_records.append(struct.pack("<QIHHIIII", int.from_bytes(digest[:8], "little"),
            string("/.LINNET/ART/" + name), side, side, 2 if side == 160 else 1, len(pixels), zlib.crc32(pixels), 0))
        art_indices[digest] = index
        remember_cover(index)
        return index

    alias_cache: dict[Path, str] = {}
    media_parents = {}
    cache_hits = 0
    from .media_manager import _music_import_name
    for number, path, resolved, before, metadata, cached, prefetched_cover in _metadata_rows(
            paths, source, ffprobe, device_root is not None, cache, bypass_cache,
            metadata_pool, progress, cancelled, prefetch_cover, needs_embedded):
        issue = compatibility_issue(metadata, target)
        if issue:
            from . import SourceError
            raise SourceError(f'{path}: {issue}', path)
        location = None
        if device_root is not None:
            location = _device_media_path(path, device_root, alias_cache, canonical=resolved,
                                          parents=media_parents)
        stage_audio = device_root is None
        hash_audio = verification == "full" or stage_audio
        relative = path.relative_to(source).as_posix()
        cache_hits += int(cached)
        identity = _stable("track:" + relative)
        extension = {".aiff": "AIF", ".aifc": "AIF", ".flac": "FLC", ".opus": "OPS"}.get(path.suffix.lower(), path.suffix[1:].upper())
        stem = re.sub(r"[^A-Z0-9_-]", "", _music_import_name(path.stem).upper()) or "TRACK"
        name = f"{stem[:8]}.{extension}"
        # Occupied names only accumulate, so earlier suffixes for this exact
        # stem and extension never need to be searched again.
        suffix_key = (stem, extension)
        suffix = filename_suffixes.get(suffix_key, 1)
        while stage_audio and name in filenames:
            tail = str(suffix)
            name = f"{stem[:8-len(tail)-1]}-{tail}.{extension}"
            suffix += 1
        if identity in stable_ids:
            raise ValueError("stable track identifier collision")
        stable_ids.add(identity)
        if stage_audio:
            filenames.add(name)
            filename_suffixes[suffix_key] = suffix
        hasher = hashlib.sha256()
        temporary = output / "MUSIC" / (name + ".tmp")
        if hash_audio:
            with path.open("rb") as original:
                staged = temporary.open("wb") if stage_audio else None
                try:
                    while True:
                        _check_cancel(cancelled)
                        chunk = original.read(1024 * 1024)
                        _check_cancel(cancelled)
                        if not chunk:
                            break
                        hasher.update(chunk)
                        if staged:
                            staged.write(chunk)
                finally:
                    if staged:
                        staged.close()
        after = path.stat()
        if file_stamp(before) != file_stamp(after):
            temporary.unlink(missing_ok=True)
            raise ValueError("audio source changed during compilation")
        if cache and not cached:
            cache.put(source, relative, before, metadata)
        if stage_audio:
            temporary.replace(output / "MUSIC" / name)
        location = location or "/MUSIC/" + name
        saved_tags = retail.get(str(path).casefold(), {})
        tags = {**{key: str(value) for key, value in saved_tags.items() if value},
                **{key: value for key, value in metadata["tags"].items() if value}}
        if device_root is not None:
            tags = {key: value.encode('utf-8')[:MAX_STRING].decode('utf-8', errors='ignore')
                    for key, value in tags.items()}
        try:
            title = _text(tags.get("title") or path.stem)
            artist = _text(tags.get("artist") or "Unknown Artist")
            album = _text(tags.get("album") or "Unknown Album")
            album_artist = _album_artist(tags, artist)
            genre, composer = _text(tags.get("genre", "")), _text(tags.get("composer", ""))
        except ValueError as error:
            from . import SourceError
            raise SourceError(f"{path}: {error}", path) from error
        year = _number(str(tags.get("date", tags.get("year", "")))[:4])
        track, disc = _number(tags.get("track", "")), _number(tags.get("disc", ""))
        def replaygain(key):
            try:
                db = float(str(tags.get(key, "")).lower().replace("db", "").strip())
                return round(db * 65536) if math.isfinite(db) and -60 <= db <= 60 else UNKNOWN_GAIN
            except ValueError:
                return UNKNOWN_GAIN
        gain = replaygain("replaygain_track_gain")
        record = bytearray(TRACK)
        struct.pack_into("<QQQQQIHH", record, 0, identity, before.st_size,
            metadata["data_offset"], metadata["data_bytes"], metadata["frames"],
            metadata["rate"], metadata["channels"], metadata["bits"])
        struct.pack_into("<IIIIIIIIIIIi", record, 48, metadata["codec"], string(title), string(artist),
            string(album, album_artist), string(location), string(genre), string(composer),
            year, track, disc, art_for(path, before, prefetched_cover), gain)
        record[96:128] = hasher.digest() if hash_audio else bytes(32)
        struct.pack_into("<i", record, 128, replaygain("replaygain_album_gain"))
        index = len(records)
        records.append(record)
        for kind, label, parent in ((1, artist, ""), (2, album, album_artist), (3, genre, ""),
                                    (4, composer, ""), (5, str(year) if year else "", ""),
                                    (6, path.parent.relative_to(source).as_posix(), "")):
            if label:
                groups.setdefault((kind, label, parent), []).append(index)
        if album_artist != artist:
            groups.setdefault((1, album_artist, ""), []).append(index)
        entries.append(dict(source=relative, id=f"{identity:016x}", path=location, storage="staged" if stage_audio else "existing",
                            title=title, artist=artist, album=album, track=track, disc=disc,
                            codec=metadata["codec"], integrity="sha256" if hash_audio else "metadata", bytes=before.st_size,
                            mtime_ns=before.st_mtime_ns,
                            sha256=hasher.hexdigest() if hash_audio else "0" * 64))
    if media_parents:
        from .publish import _recheck_parents
        _recheck_parents(media_parents, device_root)
    # All Tracks reads record ordinals directly. Sort once on the host and
    # remap every existing group reference before constructing playlists/search.
    order = sorted(range(len(entries)), key=lambda i: (
        entries[i]["title"].casefold(), entries[i]["artist"].casefold(),
        entries[i]["album"].casefold(), entries[i]["disc"], entries[i]["track"],
        entries[i]["source"]))
    ordinals = {old: new for new, old in enumerate(order)}
    records = [records[i] for i in order]
    entries = [entries[i] for i in order]
    groups = {key: [ordinals[i] for i in indices] for key, indices in groups.items()}
    lookup = {entry["source"]: index for index, entry in enumerate(entries)}
    for name, members in (playlists or {}).items():
        if len(members) > MAX_TRACKS:
            raise ValueError("playlist exceeds track limit")
        try:
            groups[(7, _text(name), "")] = [lookup[member] for member in members]
        except KeyError as exc:
            raise ValueError(f"playlist contains unknown source: {exc}") from exc
    by_id = {int(entry['id'], 16): index for index, entry in enumerate(entries)}
    old_ids = set(recent_ids or ())
    recent = [i for identity, i in by_id.items() if identity not in old_ids]
    recent.extend(by_id[identity] for identity in (recent_ids or ()) if identity in by_id)
    if recent:
        groups[(8, 'Recently Added', '')] = recent
    keys = sorted(groups, key=lambda key: (key[0], key[1].casefold(), key[2].casefold(), key))
    group_indices = {key: index for index, key in enumerate(keys)}
    group_records, members, group_ids = [], [], set()
    member_spans = {}
    track_order = [(entry["disc"], entry["track"], entry["title"].casefold(), i)
                   for i, entry in enumerate(entries)]
    for key in keys:
        kind, name, parent = key
        indices = groups[key]
        if kind not in (7, 8):
            indices = sorted(indices, key=track_order.__getitem__)
        parent_index = group_indices[(1, parent, "")] if kind == 2 else NO_INDEX
        identity = _stable("group:" + json.dumps([kind, parent, name], ensure_ascii=False, separators=(",", ":")))
        if identity in group_ids:
            raise ValueError("stable group identifier collision")
        group_ids.add(identity)
        span = tuple(indices)
        if span not in member_spans:
            member_spans[span] = len(members)
            members.extend(indices)
        group_records.append(struct.pack("<IIIIIIQ", kind, string(name, parent if kind == 2 else None), parent_index,
            member_spans[span], len(indices), 0, identity))
    search = [(entry["title"].casefold(), i) for i, entry in enumerate(entries)]
    search += [(key[1].casefold(), 0x80000000 | i) for i, key in enumerate(keys) if key[0] in (1, 2)]
    search.sort()
    search_records = [struct.pack("<II", reference, string(key)) for key, reference in search]
    # Pack browse labels in group order, then titles and search keys. Previously
    # album names were scattered among each source file's paths and metadata,
    # forcing unrelated page reads while traversing a sorted album list.
    old_strings = {offset: key[0] for key, offset in strings.items()}
    offsets = [0]
    offsets.extend(struct.unpack_from("<I", row, 4)[0] for row in group_records)
    offsets.extend(struct.unpack_from("<I", row, 52)[0] for row in records)
    offsets.extend(struct.unpack_from("<I", row, 4)[0] for row in search_records)
    offsets.extend(old_strings)
    remap, pool = {}, bytearray()
    for old in offsets:
        if old not in remap:
            remap[old] = len(pool)
            pool.extend(old_strings[old].encode("utf-8") + b"\0")
    for table, fields in ((records, range(52, 76, 4)), (group_records, (4,)),
                          (search_records, (4,)), (artwork_records, (8,))):
        for index, packed in enumerate(table):
            row = bytearray(packed)
            for field in fields:
                struct.pack_into("<I", row, field, remap[struct.unpack_from("<I", row, field)[0]])
            table[index] = row
    track_offset = HEADER
    group_offset = track_offset + TRACK * len(records)
    member_offset = group_offset + GROUP * len(group_records)
    search_offset = member_offset + 4 * len(members)
    art_offset = search_offset + 8 * len(search)
    string_offset = art_offset + ART * len(artwork_records)
    if string_offset + len(pool) > MAX_DATABASE:
        raise ValueError("database exceeds 128 MiB")
    payload = bytearray().join(records)
    payload += b"".join(group_records)
    payload += struct.pack(f"<{len(members)}I", *members)
    payload += b"".join(search_records)
    payload += b"".join(artwork_records)
    payload += pool
    header = bytearray(HEADER)
    struct.pack_into("<4sHH14I", header, 0, b"PODB", 1, HEADER, HEADER + len(payload), 0,
        len(records), len(group_records), len(members), len(search), track_offset, group_offset,
        member_offset, search_offset, string_offset, len(pool), len(artwork_records), art_offset)
    header[64:96] = hashlib.sha256(payload).digest()
    struct.pack_into("<I", header, 124, zlib.crc32(header[:124]))
    database = bytes(header) + payload
    from .browse import prepare as prepare_browse
    browse_data = prepare_browse(database)
    temporary = output / "LIBRARY.TMP"
    temporary.write_bytes(database)
    temporary.replace(output / "LIBRARY.PDB")
    (output / "BROWSE.BIN").write_bytes(browse_data)
    if coverflow_enabled:
        from .coverflow import prepare
        prepare(database, output, output / 'COVERS.CFL', originals=coverflow_originals,
                cache=cache, bypass_cache=bypass_cache,
                packed=target == 'video5g-m25',
                row_major=TARGETS[target].get('presentation_profile') == 'color320')
    report = dict(format="PODB", integrity="sha256" if verification == "full" else "metadata", tracks=len(records), groups=len(groups),
                  artwork=len(artwork_records), bytes=len(database), sha256=hashlib.sha256(database).hexdigest(),
                  database=str(output / "LIBRARY.PDB"), entries=entries,
                  cache=dict(hits=cache_hits, misses=len(records) - cache_hits,
                             status=cache.status if cache else "unused"),
                  limitations=[
                               "Compressed sample counts use bounded host decoding and are cached for existing media. Native decoder counts take precedence. Gapless trim metadata and seek tables are not emitted.",
                               "No cached FAT extents; paths are resolved by the device."])
    return report


def remove_database_tracks(data: bytes, removed) -> bytes:
    """Remove track ordinals using only the validated index, preserving media and artwork."""
    validate_database(data)
    _, _, tracks, groups, members, searches, to, go, mo, so, po, ps, arts, ao = struct.unpack_from('<14I', data, 8)
    removed = set(removed)
    if any(index < 0 or index >= tracks for index in removed):
        raise ValueError('Removed track index out of range')
    if not removed:
        return data
    track_map = {old: new for new, old in enumerate(i for i in range(tracks) if i not in removed)}
    width = TRACK
    records = [data[to + i * width:to + (i + 1) * width] for i in track_map]
    group_rows, kept_members = [], {}
    for i in range(groups):
        row = list(struct.unpack_from('<IIIIIIQ', data, go + i * GROUP))
        group_rows.append(row)
        member_data = memoryview(data)[mo + row[3]*4:mo + (row[3]+row[4])*4]
        kept_members[i] = [track_map[j] for (j,) in struct.iter_unpack('<I', member_data) if j in track_map]
    kept_groups = {i for i in range(groups) if kept_members[i]}
    kept_groups.update(group_rows[i][2] for i in tuple(kept_groups) if group_rows[i][0] == 2)
    group_map = {old: new for new, old in enumerate(sorted(kept_groups))}
    packed_groups, packed_members = [], []
    member_spans = {}
    for i in group_map:
        row = group_rows[i]
        if row[0] == 2:
            row[2] = group_map[row[2]]
        span = tuple(kept_members[i])
        if span not in member_spans:
            member_spans[span] = len(packed_members)
            packed_members.extend(span)
        row[3], row[4] = member_spans[span], len(span)
        packed_groups.append(struct.pack('<IIIIIIQ', *row))
    packed_search = []
    for i in range(searches):
        reference, key = struct.unpack_from('<II', data, so + i * 8)
        mapping = group_map if reference & 0x80000000 else track_map
        old = reference & 0x7fffffff
        if old in mapping:
            packed_search.append(struct.pack('<II', mapping[old] | (reference & 0x80000000), key))
    go = HEADER + len(records) * width
    mo = go + len(packed_groups) * GROUP
    so = mo + len(packed_members) * 4
    new_ao = so + len(packed_search) * 8
    new_po = new_ao + arts * ART
    payload = bytearray().join(records)
    payload += b''.join(packed_groups)
    payload += struct.pack(f'<{len(packed_members)}I', *packed_members)
    payload += b''.join(packed_search)
    payload += memoryview(data)[ao:]
    header = bytearray(HEADER)
    struct.pack_into('<4sHH14I', header, 0, b'PODB', 1, HEADER, HEADER + len(payload), 0,
                     len(records), len(packed_groups), len(packed_members), len(packed_search),
                     HEADER, go, mo, so, new_po, ps, arts, new_ao)
    header[64:96] = hashlib.sha256(payload).digest()
    struct.pack_into('<I', header, 124, zlib.crc32(header[:124]))
    result = bytes(header) + payload
    del payload
    validate_database(result)
    return result


def validate_database(data: bytes) -> dict:
    """Strict full host validation; firmware may validate by bounded reads."""
    if len(data) < HEADER or len(data) > MAX_DATABASE:
        raise ValueError("database size")
    magic, reserved, header_size, *fields = struct.unpack_from("<4sHH14I", data)
    size, flags, tracks, groups, members, searches, to, go, mo, so, po, ps, arts, ao = fields
    if reserved != 1 or (magic, header_size, size, flags) != (b"PODB", HEADER, len(data), 0):
        raise ValueError("database header")
    if tracks > MAX_TRACKS or not tracks <= searches <= tracks + groups or any(data[96:124]):
        raise ValueError("database counts/reserved")
    width = TRACK
    if (to != HEADER or go != to + tracks * width or mo != go + groups * GROUP or
        so != mo + members * 4 or ao != so + searches * 8 or po != ao + arts * ART or po + ps != size or not ps):
        raise ValueError("database section bounds")
    if zlib.crc32(data[:124]) != struct.unpack_from("<I", data, 124)[0]:
        raise ValueError("database header CRC")
    if hashlib.sha256(memoryview(data)[HEADER:]).digest() != data[64:96]:
        raise ValueError("database payload SHA256")
    if data[po] or data[-1]:
        raise ValueError("string pool sentinel")
    strings = {}
    cursor = po
    while cursor < len(data):
        end = data.find(b"\0", cursor)
        if end < 0 or end - cursor > MAX_STRING:
            raise ValueError("string length")
        strings[cursor - po] = data[cursor:end].decode("utf-8")
        cursor = end + 1

    def get_string(offset: int) -> str:
        if offset not in strings:
            raise ValueError("string reference")
        return strings[offset]

    titles, ids = [], set()
    for index in range(tracks):
        off = to + index * width
        identity, file_size, start, count, frames, rate, channels, bits = struct.unpack_from("<QQQQQIHH", data, off)
        codec, title, artist, album, path, genre, composer, year, track, disc, art, gain = struct.unpack_from("<IIIIIIIIIIIi", data, off + 48)
        if identity in ids or start > file_size or count > file_size - start or not 1 <= rate <= 384000 or not 1 <= channels <= 8:
            raise ValueError("track identity/range")
        ids.add(identity)
        for offset in (title, artist, album, path, genre, composer):
            get_string(offset)
        location = get_string(path)
        if not safe_media_path(location):
            raise ValueError("track path")
        if codec not in CODECS.values() or codec == 255 or (art != NO_INDEX and art >= arts):
            raise ValueError("track codec/art")
        if codec <= 4 and (bits not in (8, 16, 24, 32) or count != frames * channels * ((bits + 7) // 8)):
            raise ValueError("PCM frame layout")
        if codec >= 16 and (start or count != file_size):
            raise ValueError("compressed track must cover the complete file")
        if codec in (3, 4) and bits != 32:
            raise ValueError("float sample width")
        if gain != UNKNOWN_GAIN and not -60 * 65536 <= gain <= 60 * 65536:
            raise ValueError("ReplayGain bound")
        album_gain = struct.unpack_from("<i", data, off + 128)[0]
        if album_gain != UNKNOWN_GAIN and not -60 * 65536 <= album_gain <= 60 * 65536:
            raise ValueError("Album ReplayGain bound")
        titles.append(get_string(title))
    group_ids = set()
    expected_search = [(title.casefold(), i) for i, title in enumerate(titles)]
    for index in range(groups):
        kind, name, parent, first, count, reserved, identity = struct.unpack_from("<IIIIIIQ", data, go + index * GROUP)
        get_string(name)
        if kind in (1, 2):
            expected_search.append((get_string(name).casefold(), 0x80000000 | index))
        if kind not in range(1, 9) or reserved or first > members or count > members - first:
            raise ValueError("group bounds")
        if identity in group_ids:
            raise ValueError("group identifier collision")
        group_ids.add(identity)
        if kind == 2:
            if parent >= groups or struct.unpack_from("<I", data, go + parent * GROUP)[0] != 1:
                raise ValueError("album artist parent")
        elif parent != NO_INDEX:
            raise ValueError("unexpected group parent")
    for (member,) in struct.iter_unpack("<I", memoryview(data)[mo:so]):
        if member >= tracks:
            raise ValueError("member track index")
    expected_search.sort()
    if searches != len(expected_search) or any(
            (get_string(key), reference) != expected
            for (reference, key), expected in zip(
                struct.iter_unpack("<II", memoryview(data)[so:ao]), expected_search)):
        raise ValueError("search permutation/order")
    for index in range(arts):
        identity, path, width, height, encoding, count, crc, reserved = struct.unpack_from("<QIHHIIII", data, ao + index * ART)
        if (not re.fullmatch(r"/\.LINNET/ART/[0-9A-F]{8}\.RGB", get_string(path)) or
                width not in (64, 128, 160) or height != width or reserved or
                not ((encoding == 1 and count == width * width * 2) or
                     (encoding == 2 and width == 160 and count in (76288, 108040)))):
            raise ValueError("art record")
    return dict(tracks=tracks, groups=groups, members=members, artwork=arts, bytes=size)


def _recent_track_ids(path: Path) -> list[int]:
    """Preserve import order by stable ID across syncs and metadata reordering."""
    from .publish import _bounded
    if not path.exists():
        return []
    if path.is_symlink() or path.stat().st_size > MAX_DATABASE:
        raise ValueError('unsafe previous library')
    data = _bounded(path, MAX_DATABASE)
    validate_database(data)
    tracks, groups = struct.unpack_from('<II', data, 16)
    to, go, mo = struct.unpack_from('<III', data, 32)
    width = TRACK
    ids = [struct.unpack_from('<Q', data, to + i * width)[0] for i in range(tracks)]
    for i in range(groups):
        kind, _, _, first, count = struct.unpack_from('<5I', data, go + i * GROUP)
        if kind == 8:
            return [ids[struct.unpack_from('<I', data, mo + (first+j)*4)[0]] for j in range(count)]
    if tracks:
        raise ValueError("Music library is missing its Recently Added index")
    return []
