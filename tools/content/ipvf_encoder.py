#!/usr/bin/env python3
"""Encode video and audio for Linnet's target-native IPVF viewer.

IPVF stores target-sized RGB565 little-endian pixels, lossless bounding-rectangle
deltas, and one independent
44.1 kHz silence, mono IMA, or stereo IMA payload after each frame's video.
Video and matching audio remain together in one sector-aligned disk read.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
from operator import sub
import struct
import subprocess
import zlib
from array import array
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from fractions import Fraction
from functools import lru_cache
from pathlib import Path
from typing import BinaryIO

from . import ipvf_host

W, H = 220, 176
FRAME_BYTES = W * H * 2
HEADER_SIZE = 80
DATA_OFFSET = 512
RECORD_SECTOR_SIZE = 512
RECORD_HEADER_SIZE = 16
MAX_RECORD_SECTORS = 192
MOTION_MIN_SECTOR_SAVING = 10
MAGIC = b"IPVF"
FLAG_RGB565LE = 1
FLAG_SECTOR_RECORDS = 2
FLAG_IMA_ADPCM = 8
FLAG_TEMPORAL_XOR = 16
FLAGS = FLAG_RGB565LE | FLAG_SECTOR_RECORDS | FLAG_IMA_ADPCM
TYPE_KEY = 0
TYPE_RECTS = 1
TYPE_REPEAT = 2
TYPE_KEY_LZ4 = 3
TYPE_RECTS_LZ4 = 4
TYPE_XOR_LZ4 = 5
TYPE_MOTION_LZ4 = 6
AUDIO_FORMAT_IMA_ADPCM = 2
AUDIO_CHANNELS = 2
AUDIO_BITS_PER_SAMPLE = 16
AUDIO_SAMPLE_RATE = 44_100
AUDIO_FRAME_BYTES = AUDIO_CHANNELS * AUDIO_BITS_PER_SAMPLE // 8
MIN_FPS = 4
MAX_FPS = 240
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
DEVICE_FILE_SIZE_MAX = 0x7FFFFFFF
INDEX_ENTRY_SIZE = 16
INDEX_FLAG_KEY_LZ4 = 1
METADATA_OFFSET = HEADER_SIZE
METADATA_CAPACITY = DATA_OFFSET - METADATA_OFFSET
METADATA_TAGS = {"title": 1, "artist": 2, "album": 3}
NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

def configure_geometry(width: int, height: int, max_record_sectors: int = 192) -> None:
    """Select the target media geometry before encoding any records."""
    global W, H, FRAME_BYTES, MAX_RECORD_SECTORS
    if (type(width) is not int or type(height) is not int or
            not 1 <= width <= 1024 or not 1 <= height <= 1024 or
            width * height > 131072 or width * height % 2 or
            type(max_record_sectors) is not int or
            not 8 <= max_record_sectors <= 1024):
        raise ValueError("invalid IPVF target geometry")
    W, H = width, height
    FRAME_BYTES = width * height * 2
    MAX_RECORD_SECTORS = max_record_sectors

COLOR_FILTERS = {
    "rgb565": None,
    "gray4": (
        "format=gray,lut=y='round(val*3/255)*255/3',format=rgb24"
    ),
    "rgb555": (
        "format=rgb24,lutrgb=r='floor(val/8)*8':"
        "g='floor(val/8)*8':b='floor(val/8)*8',format=bgr0"
    ),
    "rgb454": (
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/8)*8':b='floor(val/16)*16',format=bgr0"
    ),
    "rgb444": (
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/16)*16':b='floor(val/16)*16',format=bgr0"
    ),
}

DEFAULT_LZ4_MODE = "balanced"


class OfficialLZ4:
    """Official raw-block LZ4/LZ4HC encoder loaded from the host library."""

    def __init__(self) -> None:
        # The Windows wheel includes liblz4 without exposing a separate DLL.
        try:
            import lz4
            from lz4.block import compress
        except ImportError:
            self.block_compress = None
        else:
            self.block_compress = compress
            self.block_version = lz4.library_version_string()
            return
        name = ctypes.util.find_library("lz4")
        if not name:
            raise RuntimeError("official liblz4 was not found")
        self.lib = ctypes.CDLL(name)
        self.lib.LZ4_compressBound.argtypes = [ctypes.c_int]
        self.lib.LZ4_compressBound.restype = ctypes.c_int
        self.lib.LZ4_compress_default.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ]
        self.lib.LZ4_compress_default.restype = ctypes.c_int
        self.lib.LZ4_compress_HC.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_int,
        ]
        self.lib.LZ4_compress_HC.restype = ctypes.c_int
        self.lib.LZ4_versionString.argtypes = []
        self.lib.LZ4_versionString.restype = ctypes.c_char_p

    @property
    def version(self) -> str:
        if self.block_compress is not None:
            return self.block_version
        value = self.lib.LZ4_versionString()
        return value.decode("ascii") if value else "unknown"

    def compress(self, source: bytes, level: int = 0) -> bytes:
        if not source or len(source) > 0x7FFFFFFF:
            raise ValueError("invalid LZ4 input size")
        if self.block_compress is not None:
            return self.block_compress(source, store_size=False,
                                       mode="high_compression" if level else "default",
                                       compression=level)
        bound = self.lib.LZ4_compressBound(len(source))
        destination = ctypes.create_string_buffer(bound)
        source_buffer = ctypes.create_string_buffer(source, len(source))
        if level:
            written = self.lib.LZ4_compress_HC(
                source_buffer, destination, len(source), bound, level
            )
        else:
            written = self.lib.LZ4_compress_default(
                source_buffer, destination, len(source), bound
            )
        if written <= 0:
            raise RuntimeError("official LZ4 compression failed")
        return destination.raw[:written]


_official_lz4: OfficialLZ4 | None | bool = None


def official_lz4() -> OfficialLZ4 | None:
    """Return the optional host compressor, caching an unavailable result."""
    global _official_lz4
    if _official_lz4 is None:
        try:
            _official_lz4 = OfficialLZ4()
        except (RuntimeError, OSError):
            _official_lz4 = False
    return _official_lz4 if isinstance(_official_lz4, OfficialLZ4) else None

IMA_INDEX_TABLE = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

_REVERSE_BITS = bytes(int(f"{value:08b}"[::-1], 2) for value in range(256))


def ipvf_crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """IPVF CRC-32: polynomial 0x04c11db7, MSB-first, no final XOR."""
    # Conjugate zlib's reflected CRC with bit reversal, including the seed
    # and final state, so chunked media identities retain the exact contract.
    seed = int.from_bytes(crc.to_bytes(4, "little").translate(_REVERSE_BITS), "big")
    value = zlib.crc32(data.translate(_REVERSE_BITS), seed ^ 0xFFFFFFFF) ^ 0xFFFFFFFF
    return int.from_bytes(value.to_bytes(4, "little").translate(_REVERSE_BITS), "big")


def ipvf_crc32_file_range(
    stream: BinaryIO,
    start: int,
    end: int,
    chunk_size: int = 1024 * 1024,
) -> int:
    """CRC one bounded file range without retaining it in memory."""
    if start < 0 or end < start or chunk_size <= 0:
        raise ValueError("invalid CRC file range")
    stream.flush()
    stream.seek(start)
    remaining = end - start
    crc = 0xFFFFFFFF
    while remaining:
        block = stream.read(min(remaining, chunk_size))
        if not block:
            raise RuntimeError("short read while calculating media identity")
        crc = ipvf_crc32(block, crc)
        remaining -= len(block)
    return crc


def encode_metadata(metadata: dict[str, str] | None = None) -> bytes:
    """Encode the bounded metadata TLV region in stable tag order."""
    if metadata is None:
        return b""
    unknown = set(metadata) - set(METADATA_TAGS)
    if unknown:
        raise ValueError(
            "unsupported metadata tag(s): " + ", ".join(sorted(unknown))
        )

    encoded = bytearray()
    for name, tag in METADATA_TAGS.items():
        value = metadata.get(name)
        if value is None:
            continue
        if not isinstance(value, str):
            raise ValueError(f"{name} metadata must be text")
        try:
            raw = value.encode("utf-8", "strict")
        except UnicodeEncodeError as error:
            raise ValueError(f"{name} metadata is not valid UTF-8") from error
        if not raw or len(raw) > 255:
            raise ValueError(
                f"{name} metadata length must be 1..255 UTF-8 bytes"
            )
        if len(encoded) + 2 + len(raw) > METADATA_CAPACITY:
            raise ValueError("metadata exceeds the superblock capacity")
        encoded.extend((tag, len(raw)))
        encoded.extend(raw)
    return bytes(encoded)


def index_flags_for_kind(kind: int) -> int:
    """Return the index flags for a keyframe record kind."""
    if kind == TYPE_KEY:
        return 0
    if kind == TYPE_KEY_LZ4:
        return INDEX_FLAG_KEY_LZ4
    raise ValueError(f"record kind {kind} is not a keyframe")


def _ima_encode_tables():
    # Eight quantizer intervals per step, built in bulk on the host. Only
    # differences below the final interval need a byte entry (616 KiB total).
    tables = []
    for index, step in enumerate(IMA_STEP_TABLE):
        half, quarter = step >> 1, step >> 2
        thresholds = (0, quarter, half, half + quarter, step,
                      step + quarter, step + half, step + half + quarter)
        levels = b"".join(bytes([code]) * (thresholds[code + 1] - thresholds[code])
                          for code in range(7))
        transitions = []
        for code in range(16):
            delta = ((step >> 3) + (quarter if code & 1 else 0) +
                     (half if code & 2 else 0) + (step if code & 4 else 0))
            transitions.append((-delta if code & 8 else delta,
                                max(0, min(88, index + IMA_INDEX_TABLE[code]))))
        tables.append((levels, len(levels), tuple(transitions)))
    return tuple(tables)


_IMA_ENCODE_TABLES = _ima_encode_tables()


def _ima_encode_step(predictor: int, target: int, index: int) -> tuple[int, int, int]:
    # Block entry points validate the index. Quantization and reconstruction
    # use exactly the original thresholds, deltas and saturated index changes.
    levels, cap, transitions = _IMA_ENCODE_TABLES[index]
    diff = target - predictor
    if diff < 0:
        diff = -diff
        code = (levels[diff] if diff < cap else 7) | 8
    else:
        code = levels[diff] if diff < cap else 7
    delta, index = transitions[code]
    predictor += delta
    if predictor < -32768:
        predictor = -32768
    elif predictor > 32767:
        predictor = 32767
    return code, predictor, index


def encode_ima_adpcm_stateful(
    pcm: bytes,
    frames: int,
    left_index: int,
    right_index: int,
) -> tuple[bytes, int, int]:
    """Encode one anchored block and return its final channel indices."""
    if frames <= 0:
        raise ValueError("IMA ADPCM blocks require at least one frame")
    if not 0 <= left_index <= 88 or not 0 <= right_index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    expected = frames * AUDIO_FRAME_BYTES
    if len(pcm) != expected:
        raise ValueError("PCM does not contain exactly the requested frames")
    left, right = struct.unpack_from("<hh", pcm)
    out = bytearray(struct.pack(
        "<hBBhBB", left, left_index, 0, right, right_index, 0
    ))
    lp = left
    rp = right
    li = left_index
    ri = right_index
    for target_l, target_r in struct.iter_unpack("<hh", memoryview(pcm)[AUDIO_FRAME_BYTES:]):
        code_l, lp, li = _ima_encode_step(lp, target_l, li)
        code_r, rp, ri = _ima_encode_step(rp, target_r, ri)
        out.append(code_l | (code_r << 4))
    return bytes(out), li, ri


def encode_ima_mono_stateful(
    pcm: bytes, frames: int, index: int
) -> tuple[bytes, int]:
    """Encode exact dual-mono stereo PCM as one independently anchored channel."""
    if frames <= 0 or len(pcm) != frames * AUDIO_FRAME_BYTES:
        raise ValueError("PCM does not contain exactly the requested frames")
    if not 0 <= index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    predictor, right = struct.unpack_from("<hh", pcm)
    if predictor != right:
        raise ValueError("PCM is not dual mono")
    out = bytearray(struct.pack("<hBB", predictor, index, 0))
    pending = 0
    for frame, (target, right) in enumerate(
            struct.iter_unpack("<hh", memoryview(pcm)[AUDIO_FRAME_BYTES:]), 1):
        if target != right:
            raise ValueError("PCM is not dual mono")
        code, predictor, index = _ima_encode_step(predictor, target, index)
        if (frame - 1) & 1:
            out.append(pending | (code << 4))
        else:
            pending = code
    if (frames - 1) & 1:
        out.append(pending)
    return bytes(out), index


def encode_adaptive_ima(
    pcm: bytes, frames: int, left_index: int, right_index: int
) -> tuple[bytes, int, int, str]:
    """Choose exact silence, exact dual mono, or ordinary stereo per record."""
    if frames <= 0 or len(pcm) != frames * AUDIO_FRAME_BYTES:
        raise ValueError("PCM does not contain exactly the requested frames")
    if not 0 <= left_index <= 88 or not 0 <= right_index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    native = ipvf_host.encode_audio(pcm, frames, left_index, right_index)
    if native is not None:
        return native
    if pcm == bytes(len(pcm)):
        return b"", left_index, right_index, "silence"
    dual_mono = pcm[0::4] == pcm[2::4] and pcm[1::4] == pcm[3::4]
    if dual_mono:
        payload, index = encode_ima_mono_stateful(pcm, frames, left_index)
        return payload, index, index, "mono"
    payload, left_index, right_index = encode_ima_adpcm_stateful(
        pcm, frames, left_index, right_index
    )
    return payload, left_index, right_index, "stereo"


def lz4_compress(data: bytes) -> bytes:
    """Compress an independent raw LZ4 block using a bounded hash chain."""
    n = len(data)
    if n == 0:
        raise ValueError("cannot encode an empty LZ4 block")

    # Search enough prior candidates to improve compression without making a
    # high-motion 77 KiB frame expensive to scan.  The chain is deterministic.
    max_chain = 32
    heads: dict[int, int] = {}
    # Unpack all overlapping four-byte keys in four C passes. Every source
    # position uses the same exact key as before, without allocating slices
    # and hashing their bytes in the insertion loop.
    keys = [0] * max(0, n - 3)
    for alignment in range(min(4, len(keys))):
        count = (n - alignment) // 4
        keys[alignment::4] = struct.unpack_from(f"={count}I", data, alignment)
    # Each link depends only on earlier positions, so build the complete
    # history once instead of updating it inside the matching hot loop.
    previous = []
    for position, key in enumerate(keys):
        previous.append(heads.get(key, -1))
        heads[key] = position
    del keys, heads
    out = bytearray()
    anchor = 0
    i = 0
    deferred_match = None

    def emit_length(length: int) -> None:
        if length < 15:
            return
        length -= 15
        while length >= 255:
            out.append(255)
            length -= 255
        out.append(length)

    def best_match(position: int) -> tuple[int, int]:
        # Standard LZ4 blocks reserve five terminal literals.  A match may
        # only start at least twelve bytes before the source end.
        if position + 4 > n - 5:
            return 0, -1
        ref = previous[position]
        short_length = 0
        short_ref = -1
        friendly_length = 0
        friendly_ref = -1
        limit = n - 5 - position
        # Chains use exact four-byte keys, so every candidate already matches
        # the first four bytes (there are no hash-bucket collisions to check).
        for _ in range(max_chain):
            if ref < 0:
                break
            offset = position - ref
            if offset > 65535:
                break
            # A candidate must improve at least one retained match. A
            # mismatch at that length proves it cannot do so, without
            # scanning the common prefix again.
            threshold = friendly_length if offset >= 8 else short_length
            if (threshold >= 4 and
                    data[ref + threshold] != data[position + threshold]):
                ref = previous[ref]
                continue
            length = 4
            if threshold >= 8:
                if (data[ref + 4:ref + threshold] !=
                        data[position + 4:position + threshold]):
                    ref = previous[ref]
                    continue
                # The prefix and its following byte both match. Continue
                # after them instead of scanning them a second time.
                length = threshold + 1
            while (length < limit and length < 12 and
                   data[ref + length] == data[position + length]):
                length += 1
            if length >= 12:
                # Long equal runs use C byte comparisons; binary search
                # finds the exact first difference in the final span.
                span = 16
                while length < limit:
                    count = min(span, limit - length)
                    if (data[ref + length:ref + length + count] ==
                            data[position + length:position + length + count]):
                        length += count
                        span *= 2
                        continue
                    low, high = 0, count
                    while low + 1 < high:
                        middle = (low + high) // 2
                        if (data[ref + length:ref + length + middle] ==
                                data[position + length:position + length + middle]):
                            low = middle
                        else:
                            high = middle
                    length += low
                    break
            # References are visited newest first: short offsets come before
            # all friendly offsets. Keep their winners separately, then apply
            # the same one-byte preference at the end of the search.
            if offset >= 8:
                if length > friendly_length:
                    friendly_length, friendly_ref = length, ref
            elif length > short_length:
                short_length, short_ref = length, ref
            if length == limit:
                break
            ref = previous[ref]

        # Small offsets require more overlapping copy steps in the target
        # decoder. Prefer an offset >= 8 when it costs at most one byte.
        if friendly_ref >= 0 and friendly_length + 1 >= short_length:
            return friendly_length, friendly_ref
        return short_length, short_ref

    while i + 12 <= n:
        if previous[i] < 0:
            i += 1
            continue
        if deferred_match is None:
            match_len, ref = best_match(i)
        else:
            # Lazy matching already searched this position in the same history.
            match_len, ref = deferred_match
            deferred_match = None
        if match_len < 4:
            i += 1
            continue

        # Lazy matching avoids committing to a locally shorter match.
        if i + 1 + 12 <= n and previous[i + 1] >= 0:
            next_len, next_ref = best_match(i + 1)
            if next_len > match_len + 1:
                deferred_match = next_len, next_ref
                i += 1
                continue

        literal_len = i - anchor
        encoded_match_len = match_len - 4
        out.append(((literal_len if literal_len < 15 else 15) << 4) |
                   (encoded_match_len if encoded_match_len < 15 else 15))
        if literal_len >= 15:
            emit_length(literal_len)
        out.extend(data[anchor:i])
        offset = i - ref
        out.append(offset & 255)
        out.append(offset >> 8)
        if encoded_match_len >= 15:
            emit_length(encoded_match_len)

        i += match_len
        anchor = i

    literal_len = n - anchor
    out.append(min(literal_len, 15) << 4)
    emit_length(literal_len)
    out.extend(data[anchor:])
    return bytes(out)


def lz4_decompress(data: bytes, expected_size: int | None = None) -> bytes:
    """Decompress a raw independent LZ4 block with strict bounds checking."""
    if not data:
        raise ValueError("empty LZ4 block")
    if expected_size is not None and expected_size < 0:
        raise ValueError("negative expected LZ4 size")
    out = bytearray()
    pos = 0
    last_match_start: int | None = None
    while pos < len(data):
        token = data[pos]
        pos += 1
        literal_len = token >> 4
        if literal_len == 15:
            while True:
                if pos >= len(data):
                    raise ValueError("truncated LZ4 literal length")
                extra = data[pos]
                pos += 1
                literal_len += extra
                if extra != 255:
                    break
        if pos + literal_len > len(data) or (
                expected_size is not None and
                literal_len > expected_size - len(out)):
            raise ValueError("truncated LZ4 literals")
        out.extend(data[pos:pos + literal_len])
        pos += literal_len
        if pos == len(data):
            if literal_len == 0 or (literal_len < 5 and len(out) >= 5):
                raise ValueError("LZ4 block lacks canonical terminal literals")
            break
        if pos + 2 > len(data):
            raise ValueError("truncated LZ4 offset")
        offset = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        if offset == 0 or offset > len(out):
            raise ValueError("invalid LZ4 offset")
        match_len = token & 0x0F
        if match_len == 15:
            while True:
                if pos >= len(data):
                    raise ValueError("truncated LZ4 match length")
                extra = data[pos]
                pos += 1
                match_len += extra
                if extra != 255:
                    break
        match_len += 4
        if expected_size is not None and match_len > expected_size - len(out):
            raise ValueError("LZ4 decoded size exceeds expected size")
        last_match_start = len(out)
        match_source = len(out) - offset
        while match_len:
            # Slice from all bytes currently available at the match source.
            # For overlapping matches this naturally grows geometrically,
            # while preserving the byte-at-a-time LZ4 copy semantics.
            count = min(match_len, len(out) - match_source)
            out.extend(out[match_source:match_source + count])
            match_len -= count
    if expected_size is not None and len(out) != expected_size:
        raise ValueError("LZ4 decoded size mismatch")
    if last_match_start is not None and len(out) - last_match_start < 12:
        raise ValueError("LZ4 final match starts too near block end")
    return bytes(out)


def bbox_diff(prev: bytes, cur: bytes):
    native = ipvf_host.frame_bounds(prev, cur, W, H)
    if native is not None:
        return native or None
    minx, miny = W, H
    maxx = maxy = -1
    for y in range(H):
        row = y * W * 2
        # A row-sized integer compares the same RGB565 bits in C. Its first
        # and last set bits locate the changed pixels without 220 Python
        # slice comparisons; the rectangle still uses native pixel pairs.
        difference = (int.from_bytes(prev[row:row + W * 2], "big") ^
                      int.from_bytes(cur[row:row + W * 2], "big"))
        if difference:
            minx = min(minx, (W * 16 - difference.bit_length()) // 16)
            last_bit = (difference & -difference).bit_length() - 1
            maxx = max(maxx, W - 1 - last_bit // 16)
            miny = min(miny, y)
            maxy = y
    if maxx < 0:
        return None
    # The LCD2 RGB data port consumes two pixels per 32-bit write. Align delta
    # rectangles to complete pixel pairs so a staging slot can be transferred
    # directly while the CPU reads the following frame.
    minx &= ~1
    maxx = min(W - 1, maxx | 1)
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def rect_payload(frame: bytes, rect):
    x, y, w, h = rect
    pix = bytearray(w * h * 2)
    pos = 0
    for row in range(y, y + h):
        a = (row * W + x) * 2
        b = a + w * 2
        pix[pos:pos + w * 2] = frame[a:b]
        pos += w * 2
    return struct.pack("<BBBBI", x, y, w, h, len(pix)) + pix


def rects_payload(frame: bytes, rects) -> bytes:
    """Build one native-aligned payload containing all rectangles."""
    return b"".join(rect_payload(frame, rect) for rect in rects)


def multi_rect_diff(
    prev: bytes,
    cur: bytes,
    max_rects: int = 8,
    tile_pairs: int = 4,
    tile_rows: int = 4,
):
    """Return a bounded rectangle cover for spatially separate changes.

    The working grid is made of four two-pixel LCD words by four rows.  It
    keeps component discovery cheap, preserves the LCD's two-pixel alignment,
    and prevents a noisy frame from creating hundreds of tiny transfers.
    Components are merged whenever doing so saves payload bytes, then the
    cheapest remaining merges are used to enforce ``max_rects``.
    """
    if len(prev) != FRAME_BYTES or len(cur) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    if max_rects <= 0 or max_rects > 255:
        raise ValueError("max_rects must be 1..255")
    if tile_pairs <= 0 or tile_rows <= 0:
        raise ValueError("tile dimensions must be positive")

    native = ipvf_host.rectangles(prev, cur, W, H, max_rects, tile_pairs, tile_rows)
    if native is not None:
        return native

    pair_columns = W // 2
    changed = set()
    # Combine changed bits across each tile band with bulk integer operations,
    # then inspect one mask per tile instead of slicing every tile row.
    tile_mask = (1 << (min(tile_pairs, pair_columns) * 32)) - 1
    for y in range(0, H, tile_rows):
        difference = 0
        for row in range(y, min(H, y + tile_rows)):
            offset = row * W * 2
            end = offset + pair_columns * 4
            difference |= (int.from_bytes(prev[offset:end], "little") ^
                           int.from_bytes(cur[offset:end], "little"))
        for pair in range(0, pair_columns, tile_pairs):
            if difference & tile_mask:
                changed.add((pair // tile_pairs, y // tile_rows))
            difference >>= tile_pairs * 32
            if not difference:
                break
    if not changed:
        return []

    if W % 2 == 0:
        xs, ys = zip(*changed)
        if len(changed) == (max(xs) - min(xs) + 1) * (max(ys) - min(ys) + 1):
            # A filled tile rectangle is already one connected component.
            # Its tightened bounds are exactly the whole-frame difference.
            return [bbox_diff(prev, cur)]

    components = []
    remaining = set(changed)
    while remaining:
        seed = min(remaining)
        remaining.remove(seed)
        stack = [seed]
        cells = [seed]
        while stack:
            tx, ty = stack.pop()
            for neighbour in ((tx - 1, ty), (tx + 1, ty),
                              (tx, ty - 1), (tx, ty + 1)):
                if neighbour in remaining:
                    remaining.remove(neighbour)
                    stack.append(neighbour)
                    cells.append(neighbour)
        min_tx = min(cell[0] for cell in cells)
        max_tx = max(cell[0] for cell in cells)
        min_ty = min(cell[1] for cell in cells)
        max_ty = max(cell[1] for cell in cells)

        # Tighten the tile component to the actual changed LCD words.
        min_pair = pair_columns
        max_pair = -1
        min_y = H
        max_y = -1
        for y in range(min_ty * tile_rows,
                       min(H, (max_ty + 1) * tile_rows)):
            row = y * W * 2
            first = min_tx * tile_pairs
            end = min(pair_columns, (max_tx + 1) * tile_pairs)
            start_byte, end_byte = row + first * 4, row + end * 4
            # The first and last set bits locate the changed four-byte words.
            difference = (int.from_bytes(prev[start_byte:end_byte], "big") ^
                          int.from_bytes(cur[start_byte:end_byte], "big"))
            if difference:
                min_pair = min(min_pair, end - (difference.bit_length() + 31) // 32)
                last_bit = (difference & -difference).bit_length() - 1
                max_pair = max(max_pair, end - 1 - last_bit // 32)
                min_y = min(min_y, y)
                max_y = y
        if max_pair >= 0:
            components.append((min_pair * 2, min_y,
                               (max_pair - min_pair + 1) * 2,
                               max_y - min_y + 1))

    def payload_cost(rect) -> int:
        return 8 + rect[2] * rect[3] * 2

    def merged(a, b):
        x0 = min(a[0], b[0])
        y0 = min(a[1], b[1])
        x1 = max(a[0] + a[2], b[0] + b[2])
        y1 = max(a[1] + a[3], b[1] + b[3])
        return x0, y0, x1 - x0, y1 - y0

    # A pathological sparse frame can have many isolated components.  A
    # single bounding rectangle is a safe bounded fallback for host memory and
    # encode time; normal multi-object footage stays far below this limit.
    if len(components) > 128:
        return [bbox_diff(prev, cur)]

    while len(components) > 1:
        best = None
        for i in range(len(components) - 1):
            for j in range(i + 1, len(components)):
                union = merged(components[i], components[j])
                extra = (payload_cost(union) -
                         payload_cost(components[i]) -
                         payload_cost(components[j]))
                candidate = (extra, i, j, union)
                if best is None or candidate < best:
                    best = candidate
        assert best is not None
        if len(components) <= max_rects and best[0] > 0:
            break
        _, i, j, union = best
        components[i] = union
        del components[j]

    return sorted(components, key=lambda rect: (rect[1], rect[0]))


def xor_frames(prev: bytes, cur: bytes) -> bytes:
    if len(prev) != FRAME_BYTES or len(cur) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    return (int.from_bytes(prev, "little") ^
            int.from_bytes(cur, "little")).to_bytes(FRAME_BYTES, "little")


def translate_frame(previous: bytes, dx: int, dy: int) -> bytes:
    """Shift one RGB565 frame losslessly into a black prediction canvas."""
    if len(previous) != FRAME_BYTES:
        raise ValueError("frame must match the native IPVF geometry")
    if abs(dx) >= W or abs(dy) >= H:
        raise ValueError("translation leaves no overlapping pixels")
    predicted = bytearray(FRAME_BYTES)
    source_x = max(0, -dx)
    target_x = max(0, dx)
    source_y = max(0, -dy)
    target_y = max(0, dy)
    width = W - abs(dx)
    height = H - abs(dy)
    row_bytes = width * 2
    for row in range(height):
        source = ((source_y + row) * W + source_x) * 2
        target = ((target_y + row) * W + target_x) * 2
        predicted[target:target + row_bytes] = \
            previous[source:source + row_bytes]
    return bytes(predicted)


def estimate_translation(
    previous: bytes, current: bytes, max_shift: int = 16,
    sample_step: int = 12,
) -> tuple[int, int]:
    """Estimate a bounded integer translation with a coarse/refined RGB565 SAD."""
    if len(previous) != FRAME_BYTES or len(current) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    if not 0 <= max_shift < min(W, H) or sample_step <= 0:
        raise ValueError("invalid translation search bounds")

    native = ipvf_host.estimate_translation(previous, current, W, H, max_shift, sample_step)
    if native is not None:
        return native

    score_cache: dict[tuple[int, int], tuple[int, int]] = {}
    current_samples = {}
    previous_samples = {}

    def score(dx: int, dy: int, ceiling=None) -> tuple[int, int]:
        cached = score_cache.get((dx, dy))
        if cached is not None:
            return cached
        x0 = max(0, dx)
        x1 = min(W, W + dx)
        y0 = max(0, dy)
        y1 = min(H, H + dy)
        total = 0
        columns = len(range(x0, x1, sample_step))
        count = columns * len(range(y0, y1, sample_step))
        stride = sample_step * 2
        current_key = (x0, x1)
        previous_key = (x0 - dx, x1 - dx)
        current_rows = current_samples.get(current_key)
        if current_rows is None:
            current_rows = current_samples[current_key] = [None] * H
        previous_rows = previous_samples.get(previous_key)
        if previous_rows is None:
            previous_rows = previous_samples[previous_key] = [None] * H
        for y in range(y0, y1, sample_step):
            a = current_rows[y]
            if a is None:
                current_offset = (y * W + x0) * 2
                current_end = current_offset + (x1 - x0) * 2
                a = current_rows[y] = (
                    current[current_offset:current_end:stride] +
                    current[current_offset + 1:current_end:stride])
            b = previous_rows[y - dy]
            if b is None:
                previous_offset = ((y - dy) * W + x0 - dx) * 2
                previous_end = previous_offset + (x1 - x0) * 2
                b = previous_rows[y - dy] = (
                    previous[previous_offset:previous_end:stride] +
                    previous[previous_offset + 1:previous_end:stride])
            total += sum(map(abs, map(sub, a, b)))
            if ceiling is not None and total * ceiling[1] > ceiling[0] * count:
                # Remaining terms are nonnegative. This lower bound already
                # loses, and every future best score can only get smaller.
                # It is safe to cache it; a winning score is always complete.
                break
        result = total, count
        score_cache[(dx, dy)] = result
        return result

    def better(candidate: tuple[int, int], best: tuple[int, int]) -> bool:
        best_score, best_count = score(*best)
        candidate_score, candidate_count = score(
            *candidate, ceiling=(best_score, best_count))
        left = candidate_score * best_count
        right = best_score * candidate_count
        return (left, abs(candidate[0]) + abs(candidate[1]), candidate) < \
               (right, abs(best[0]) + abs(best[1]), best)

    best = (0, 0)
    coarse = range(-max_shift, max_shift + 1, 2)
    for dy in coarse:
        for dx in coarse:
            if better((dx, dy), best):
                best = dx, dy
    coarse_best = best
    for dy in range(max(-max_shift, coarse_best[1] - 2),
                    min(max_shift, coarse_best[1] + 2) + 1):
        for dx in range(max(-max_shift, coarse_best[0] - 2),
                        min(max_shift, coarse_best[0] + 2) + 1):
            if better((dx, dy), best):
                best = dx, dy
    return best


def frame_rate(fps: int | Fraction) -> Fraction:
    """Normalize and bound one exact IPVF rational frame rate."""
    rate = Fraction(fps)
    if (rate < MIN_FPS or rate > MAX_FPS or
            rate.numerator > 0xFFFF or rate.denominator > 0xFFFF):
        raise ValueError("frame rate is outside IPVF rational bounds")
    return rate


def audio_boundary(frame: int, fps: int | Fraction) -> int:
    """Nearest 44.1 kHz sample-frame boundary for an exact video frame."""
    rate = frame_rate(fps)
    numerator = frame * AUDIO_SAMPLE_RATE * rate.denominator
    return (numerator + rate.numerator // 2) // rate.numerator


def record_sectors(video_payload_size: int, audio_size: int) -> int:
    sectors = (
        RECORD_HEADER_SIZE + video_payload_size + audio_size +
        RECORD_SECTOR_SIZE - 1
    ) // RECORD_SECTOR_SIZE
    if not 1 <= sectors <= MAX_RECORD_SECTORS:
        raise RuntimeError(
            f"record needs invalid sector count {sectors}; "
            f"IPVF requires --fps >= {MIN_FPS}"
        )
    return sectors


def write_record(
    f: BinaryIO,
    kind: int,
    rects: int,
    video_payload: bytes,
    audio_payload: bytes,
    next_sectors: int,
    decoded_video_bytes: int,
) -> tuple[int, int]:
    sectors = record_sectors(len(video_payload), len(audio_payload))
    record_bytes = sectors * RECORD_SECTOR_SIZE
    f.write(struct.pack("<BBHIII", kind, rects, next_sectors,
                        len(video_payload), decoded_video_bytes,
                        len(audio_payload)))
    f.write(video_payload)
    f.write(audio_payload)
    padding = (record_bytes - RECORD_HEADER_SIZE - len(video_payload) -
               len(audio_payload))
    f.write(bytes(padding))
    return sectors, padding


def write_header(
    f: BinaryIO,
    fps: int | Fraction,
    frames: int,
    first_record_sectors: int,
    flags: int,
    media_end_offset: int,
    index_count: int,
    index_crc: int,
    media_id: int,
    metadata: dict[str, str] | None = None,
) -> None:
    rate = frame_rate(fps)
    metadata_bytes = encode_metadata(metadata)
    if not 0 <= media_end_offset <= UINT64_MAX:
        raise ValueError("media end offset does not fit in the IPVF header")
    if not 0 <= index_count <= UINT32_MAX:
        raise ValueError("index entry count does not fit in the IPVF header")
    if not 0 <= index_crc <= UINT32_MAX:
        raise ValueError("index CRC does not fit in the IPVF header")
    if not 0 <= media_id <= UINT32_MAX:
        raise ValueError("media identity does not fit in the IPVF header")
    h = bytearray(DATA_OFFSET)
    h[0:4] = MAGIC
    struct.pack_into(
        "<HHHHHHIII",
        h,
        4,
        HEADER_SIZE,
        W,
        H,
        rate.numerator,
        rate.denominator,
        first_record_sectors,
        frames,
        flags,
        DATA_OFFSET,
    )
    total_audio_frames = audio_boundary(frames, fps)
    if total_audio_frames > UINT32_MAX:
        raise RuntimeError("audio duration exceeds IPVF limits")
    struct.pack_into(
        "<HHHII",
        h,
        28,
        AUDIO_FORMAT_IMA_ADPCM,
        AUDIO_CHANNELS,
        AUDIO_BITS_PER_SAMPLE,
        AUDIO_SAMPLE_RATE,
        total_audio_frames,
    )
    struct.pack_into("<H", h, 42, 0)
    struct.pack_into("<Q", h, 44, media_end_offset)
    struct.pack_into("<Q", h, 52, media_end_offset)
    struct.pack_into("<I", h, 60, index_count)
    struct.pack_into("<H", h, 64, INDEX_ENTRY_SIZE)
    struct.pack_into("<H", h, 66, len(metadata_bytes))
    struct.pack_into("<I", h, 68, METADATA_OFFSET)
    struct.pack_into("<I", h, 72, index_crc)
    struct.pack_into("<I", h, 76, media_id)
    h[METADATA_OFFSET:METADATA_OFFSET + len(metadata_bytes)] = metadata_bytes
    f.seek(0)
    f.write(h)


def metadata_from_probe(payload: dict) -> dict[str, str]:
    """Select supported tags from an existing FFprobe result."""
    metadata: dict[str, str] = {}
    candidates = payload.get("streams", [])
    if isinstance(candidates, list):
        candidates = [
            stream.get("tags", {}) for stream in candidates
            if isinstance(stream, dict)
        ]
    else:
        candidates = []
    format_info = payload.get("format", {})
    if isinstance(format_info, dict):
        candidates.append(format_info.get("tags", {}))
    for tags in candidates:
        if not isinstance(tags, dict):
            continue
        normalized = {
            str(key).lower(): value for key, value in tags.items()
        }
        for name in METADATA_TAGS:
            if name in metadata or name not in normalized:
                continue
            value = normalized[name]
            if isinstance(value, str) and value:
                metadata[name] = value
    return metadata


@lru_cache(maxsize=1)
def _gray4_pixels() -> tuple[bytes, ...]:
    # Evaluate the existing integer luma rounding once for every RGB565 value.
    palette = (b"\x00\x00", b"\x52\xaa", b"\xad\x55", b"\xff\xff")
    pixels = []
    for color in range(65536):
        r, g, b = (color >> 11) & 31, (color >> 5) & 63, color & 31
        luma = (r * 77 * 255 // 31 + g * 150 * 255 // 63 +
                b * 29 * 255 // 31) >> 8
        pixels.append(palette[(luma * 3 + 127) // 255])
    return tuple(pixels)


def ffmpeg_frames(
    source: Path,
    fps: int | Fraction,
    ffmpeg: str,
    color_depth: str = "rgb565",
):
    rate = frame_rate(fps)
    if color_depth not in COLOR_FILTERS:
        raise ValueError(f"unknown color depth: {color_depth}")
    filters = [
        f"fps={rate.numerator}/{rate.denominator}:start_time=0,"
        f"scale={W}:{H}:force_original_aspect_ratio=decrease:"
        f"flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black"
    ]
    if COLOR_FILTERS[color_depth] is not None:
        filters.append(COLOR_FILTERS[color_depth])
    vf = ",".join(filters)
    cmd = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(source),
        "-an",
        "-vf",
        vf,
        "-pix_fmt",
        "rgb565be",
        "-f",
        "rawvideo",
        "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, creationflags=NO_WINDOW)
    assert proc.stdout is not None
    exhausted = False
    try:
        while True:
            chunks = bytearray()
            while len(chunks) < FRAME_BYTES:
                chunk = proc.stdout.read(FRAME_BYTES - len(chunks))
                if not chunk:
                    break
                chunks.extend(chunk)
            if not chunks:
                exhausted = True
                break
            if len(chunks) != FRAME_BYTES:
                raise RuntimeError("ffmpeg ended with a partial frame")
            frame = bytes(chunks)
            if color_depth == "gray4":
                # Make the compressed pixels exactly match Q21's four physical
                # shades. FFmpeg's gray-to-RGB conversion can otherwise leave
                # several adjacent RGB565 values for the same visible shade.
                colors = struct.unpack(f">{W * H}H", frame)
                frame = b"".join(map(_gray4_pixels().__getitem__, colors))
            # Keep FFmpeg's established RGB565 quantization, then prepare
            # native pixel words on the host instead of swapping on the iPod.
            pixels = array("H")
            pixels.frombytes(frame)
            pixels.byteswap()
            yield pixels.tobytes()
    finally:
        if not exhausted and proc.poll() is None:
            proc.kill()
        proc.stdout.close()
        rc = proc.wait()
        if exhausted and rc:
            raise RuntimeError(f"video ffmpeg exited with status {rc}")


def decode_audio(source: Path, ffmpeg: str) -> subprocess.Popen:
    cmd = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(source),
        "-map",
        "0:a:0",
        "-vn",
        "-af",
        "aresample=async=1:first_pts=0",
        "-ac",
        str(AUDIO_CHANNELS),
        "-ar",
        str(AUDIO_SAMPLE_RATE),
        "-c:a",
        "pcm_s16le",
        "-f",
        "s16le",
        "-",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, creationflags=NO_WINDOW)


def read_audio_slice(audio: BinaryIO, size: int) -> tuple[bytes, int]:
    data = audio.read(size)
    if len(data) % AUDIO_FRAME_BYTES:
        raise RuntimeError("ffmpeg produced a partial PCM sample frame")
    missing = size - len(data)
    if missing:
        data += bytes(missing)
    return data, missing


def compress_lz4(data: bytes, mode: str = DEFAULT_LZ4_MODE) -> bytes:
    """Choose a raw LZ4 block without changing the device bitstream."""
    if mode not in ("builtin", "balanced", "best", "official-hc12"):
        raise ValueError(
            "lz4 mode must be builtin, balanced, best, or official-hc12"
        )
    host_lz4 = official_lz4()
    if mode == "balanced":
        return (host_lz4.compress(data, 9)
                if host_lz4 is not None else lz4_compress(data))
    if mode == "builtin":
        return lz4_compress(data)
    if mode == "official-hc12":
        if host_lz4 is None:
            raise RuntimeError("official-hc12 requires host liblz4")
        return host_lz4.compress(data, 12)

    candidates = [lz4_compress(data)]
    if host_lz4 is not None:
        candidates.append(host_lz4.compress(data, 12))
    if not candidates:  # Defensive: the built-in candidate is always present.
        raise RuntimeError("official-hc12 requires host liblz4")
    return min(candidates, key=len)


def _compress_record_if_smaller(
    kind: int,
    rect_count: int,
    payload: bytes,
    audio_size: int,
    lz4_mode: str = DEFAULT_LZ4_MODE,
):
    """Return a raw or LZ4 record, requiring a whole-sector saving."""
    decoded_size = len(payload)
    if kind not in (TYPE_KEY, TYPE_RECTS):
        return kind, rect_count, payload, decoded_size
    compressed = compress_lz4(payload, lz4_mode)
    if (len(compressed) < decoded_size and
            record_sectors(len(compressed), audio_size) <
            record_sectors(decoded_size, audio_size)):
        return kind + 3, rect_count, compressed, decoded_size
    return kind, rect_count, payload, decoded_size


def _coalesce_lz4_matches(packed: bytes, raw: bytes, minimum: int = 8) -> bytes:
    """Replace short matches with exact literals, retaining longer match offsets."""
    native = ipvf_host.coalesce(packed, raw, minimum)
    if native is not None:
        return native
    output = bytearray()
    cursor = position = anchor = 0
    packed_size = len(packed)

    def length(value: int) -> None:
        if value >= 15:
            value -= 15
            while value >= 255:
                output.append(255)
                value -= 255
            output.append(value)

    # The caller supplies the exact raw payload for this bounded block.
    while cursor < packed_size:
        token = packed[cursor]
        cursor += 1
        literals = token >> 4
        if literals == 15:
            while True:
                extension = packed[cursor]
                cursor += 1
                literals += extension
                if extension != 255:
                    break
        cursor += literals
        position += literals
        if cursor == packed_size:
            break
        match_at = cursor
        cursor += 2
        match = (token & 15) + 4
        if token & 15 == 15:
            while True:
                extension = packed[cursor]
                cursor += 1
                match += extension
                if extension != 255:
                    break
        if match >= minimum:
            literals = position - anchor
            output.append(((literals if literals < 15 else 15) << 4) | (token & 15))
            if literals >= 15:
                length(literals)
            output.extend(raw[anchor:position])
            # Offset and match-length extension bytes remain valid: only
            # the preceding literal run changed, not this match or its position.
            output.extend(packed[match_at:cursor])
            anchor = position + match
        position += match
    literals = position - anchor
    output.append((literals if literals < 15 else 15) << 4)
    if literals >= 15:
        length(literals)
    output.extend(raw[anchor:])
    return bytes(output)


def _realtime_record(record, raw=None):
    kind, rectangles, payload, decoded = record
    if kind < TYPE_KEY_LZ4:
        return record
    prefix = 6 if kind == TYPE_MOTION_LZ4 else 4 if kind == TYPE_XOR_LZ4 else 0
    if raw is None:
        raw = lz4_decompress(payload[prefix:], decoded)
    coalesced = _coalesce_lz4_matches(payload[prefix:], raw)
    # Compressed payloads, including their temporal prefix, must remain
    # smaller than the decoded payload required by the format.
    if len(coalesced) + prefix >= decoded:
        return record
    header = (payload[:2] if kind == TYPE_MOTION_LZ4 else b"")
    if prefix:
        header += struct.pack("<I", ipvf_crc32(coalesced))
    return kind, rectangles, header + coalesced, decoded


def choose_video_record(
    prev: bytes | None,
    cur: bytes,
    audio_size: int,
    force_key: bool,
    video_mode: str,
    max_rects: int,
    lz4_mode: str = DEFAULT_LZ4_MODE,
):
    """Choose a video record by final sector count, with bounded tie costs."""
    if video_mode not in ("current", "spatial", "balanced", "motion", "auto", "realtime"):
        raise ValueError(
            "video_mode must be current, spatial, balanced, motion, auto, or realtime"
        )
    realtime = video_mode == "realtime"
    if realtime:
        video_mode = "balanced"
    if prev is None or force_key:
        selected = _compress_record_if_smaller(
            TYPE_KEY, 0, cur, audio_size, lz4_mode
        )
        return _realtime_record(selected, cur) if realtime else selected

    if prev == cur:
        return TYPE_REPEAT, 0, b"", 0
    rect = bbox_diff(prev, cur)
    if rect is None:
        return TYPE_REPEAT, 0, b"", 0
    byte_geometry = W <= 255 and H <= 255
    selected_raw = cur
    if byte_geometry:
        delta = rect_payload(cur, rect)
        if len(delta) < FRAME_BYTES:
            selected_raw = delta
            selected = _compress_record_if_smaller(
                TYPE_RECTS, 1, delta, audio_size, lz4_mode
            )
        else:
            selected = _compress_record_if_smaller(
                TYPE_KEY, 0, cur, audio_size, lz4_mode
            )
    else:
        selected = _compress_record_if_smaller(
            TYPE_KEY, 0, cur, audio_size, lz4_mode
        )
    selected_sectors = record_sectors(len(selected[2]), audio_size)

    if byte_geometry and video_mode in ("spatial", "balanced", "motion", "auto"):
        rectangles = multi_rect_diff(prev, cur, max_rects=max_rects)
        if len(rectangles) > 1:
            multi_payload = rects_payload(cur, rectangles)
            if len(multi_payload) < FRAME_BYTES:
                candidate = _compress_record_if_smaller(
                    TYPE_RECTS, len(rectangles), multi_payload, audio_size,
                    lz4_mode,
                )
                candidate_sectors = record_sectors(
                    len(candidate[2]), audio_size
                )
                if candidate_sectors < selected_sectors:
                    selected = candidate
                    selected_raw = multi_payload
                    selected_sectors = candidate_sectors

    minimum_saving = (MOTION_MIN_SECTOR_SAVING
                      if video_mode == "balanced" else 1)
    # Even an empty residual needs its six-byte motion header. If that
    # lower bound cannot win, prediction and compression cannot be selected.
    if (video_mode in ("balanced", "motion", "auto") and
            record_sectors(6, audio_size) + minimum_saving <= selected_sectors):
        dx, dy = estimate_translation(prev, cur)
        prediction = translate_frame(prev, dx, dy)
        residual_raw = xor_frames(prediction, cur)
        residual = compress_lz4(residual_raw, lz4_mode)
        motion_kind = TYPE_MOTION_LZ4 if dx or dy else TYPE_XOR_LZ4
        motion_payload = (struct.pack("<bb", dx, dy) if dx or dy else b"") + \
            struct.pack("<I", ipvf_crc32(residual)) + residual
        motion_sectors = record_sectors(len(motion_payload), audio_size)
        if ((video_mode in ("balanced", "motion") or (dx, dy) != (0, 0)) and
                len(motion_payload) < FRAME_BYTES and
                motion_sectors + minimum_saving <=
                selected_sectors):
            selected = motion_kind, 0, motion_payload, FRAME_BYTES
            selected_raw = residual_raw
            selected_sectors = motion_sectors

    if (video_mode == "auto" and
            record_sectors(4, audio_size) < selected_sectors):
        temporal = compress_lz4(xor_frames(prev, cur), lz4_mode)
        temporal_payload = struct.pack(
            "<I", ipvf_crc32(temporal)
        ) + temporal
        temporal_sectors = record_sectors(len(temporal_payload), audio_size)
        if (len(temporal_payload) < FRAME_BYTES and
                temporal_sectors < selected_sectors):
            selected = TYPE_XOR_LZ4, 0, temporal_payload, FRAME_BYTES

    if realtime:
        selected = _realtime_record(selected, selected_raw)
        # Stationary backgrounds can predict better than a translated frame.
        # Require a useful disk saving before paying the full-frame XOR cost.
        if (selected[0] in (TYPE_KEY, TYPE_RECTS, TYPE_KEY_LZ4, TYPE_RECTS_LZ4) and
                record_sectors(4, audio_size) + 4 <= record_sectors(len(selected[2]), audio_size)):
            residual_raw = xor_frames(prev, cur)
            residual = compress_lz4(residual_raw, lz4_mode)
            if len(residual) + 4 < FRAME_BYTES:
                candidate = _realtime_record((TYPE_XOR_LZ4, 0,
                    struct.pack("<I", ipvf_crc32(residual)) + residual, FRAME_BYTES), residual_raw)
                if record_sectors(len(candidate[2]), audio_size) + 4 <= record_sectors(len(selected[2]), audio_size):
                    selected = candidate
    return selected


def _configure_video_worker(width, height, sectors, host_library):
    configure_geometry(width, height, sectors)
    ipvf_host.configure_worker(host_library)


def _encoded_records(source, fps, keyint, ffmpeg, video_mode, max_rects,
                     lz4_mode, color_depth, audio_file, audio_process, worker_limit):
    # Each video record depends only on two source frames. Keep audio state
    # sequential and retire bounded host jobs in source order, so scheduling
    # cannot change the bitstream or its audio/video timing.
    workers = min(worker_limit, os.cpu_count() or 1)
    pending = deque()
    previous = None
    left_index = right_index = 0

    def retire():
        future, audio, kind, missing = pending.popleft()
        video_kind, rects, payload, decoded = future.result()
        return (video_kind, rects, payload, audio, decoded), kind, missing

    with ProcessPoolExecutor(max_workers=workers, initializer=_configure_video_worker,
                             initargs=(W, H, MAX_RECORD_SECTORS, ipvf_host.worker_path())) as pool:
        for frame, current in enumerate(ffmpeg_frames(source, fps, ffmpeg, color_depth)):
            samples = audio_boundary(frame + 1, fps) - audio_boundary(frame, fps)
            size = samples * AUDIO_FRAME_BYTES
            if audio_file is None:
                pcm, missing = bytes(size), size
            else:
                pcm, missing = read_audio_slice(audio_file, size)
                if missing and audio_process.wait():
                    raise RuntimeError("ffmpeg could not decode the first audio stream")
            audio, left_index, right_index, kind = encode_adaptive_ima(
                pcm, samples, left_index, right_index)
            future = pool.submit(choose_video_record, previous, current, len(audio),
                                 previous is None or (keyint > 0 and frame % keyint == 0),
                                 video_mode, max_rects, lz4_mode)
            pending.append((future, audio, kind, missing))
            previous = current
            if len(pending) >= workers * 2:
                yield retire()
        while pending:
            yield retire()


def encode(
    source: Path,
    output: Path,
    fps: int | Fraction,
    keyint: int,
    ffmpeg: str,
    video_mode: str = "spatial",
    max_rects: int = 8,
    lz4_mode: str = DEFAULT_LZ4_MODE,
    color_depth: str = "rgb565",
    *,
    metadata: dict[str, str] | None = None,
    source_has_audio: bool | None = None,
    worker_limit: int = 4,
) -> None:
    fps = frame_rate(fps)
    if type(worker_limit) is not int or not 1 <= worker_limit <= 12:
        raise ValueError("IPVF worker limit must be between one and twelve")
    # Validate metadata before creating temporary PCM or walking any video
    # frames. Header finalization must not be the first point where a bad tag
    # can reject an otherwise complete long encode.
    encode_metadata(metadata)
    if video_mode in ("balanced", "motion", "auto", "realtime") and keyint == 0:
        raise ValueError("temporal mode requires a bounded keyframe interval")
    if video_mode in ("balanced", "motion", "auto", "realtime") and fps > 30:
        raise ValueError("temporal mode is supported only at <=30 fps")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    audio_process: subprocess.Popen | None = None
    audio_file: BinaryIO | None = None
    counts = {
        TYPE_KEY: 0, TYPE_RECTS: 0, TYPE_REPEAT: 0,
        TYPE_KEY_LZ4: 0, TYPE_RECTS_LZ4: 0, TYPE_XOR_LZ4: 0,
        TYPE_MOTION_LZ4: 0,
    }
    audio_counts = {"silence": 0, "mono": 0, "stereo": 0}
    video_payload_total = 0
    audio_payload_total = 0
    audio_padding_total = 0
    record_total = 0
    padding_total = 0
    pending = None
    pending_frame: int | None = None
    index_entries: list[tuple[int, int, int, int]] = []
    first_record_sectors = 0
    n = 0

    try:
        if source_has_audio is not False:
            audio_process = decode_audio(source, ffmpeg)
            audio_file = audio_process.stdout

        with temporary.open("wb+") as f:
            f.write(bytes(DATA_OFFSET))
            for current, audio_kind, missing in _encoded_records(
                    source, fps, keyint, ffmpeg, video_mode, max_rects,
                    lz4_mode, color_depth, audio_file, audio_process, worker_limit):
                audio_counts[audio_kind] += 1
                audio_padding_total += missing
                kind, rects, video_payload, audio_payload, decoded_video_bytes = current
                current_sectors = record_sectors(
                    len(video_payload), len(audio_payload)
                )
                if f.tell() + current_sectors * RECORD_SECTOR_SIZE + len(index_entries) * INDEX_ENTRY_SIZE + MAX_RECORD_SECTORS * RECORD_SECTOR_SIZE > DEVICE_FILE_SIZE_MAX:
                    raise RuntimeError("IPVF exceeds the device's 2-GiB file limit; split the source video")
                if pending is None:
                    first_record_sectors = current_sectors
                    pending_frame = n
                else:
                    record_offset = f.tell()
                    sectors, padding = write_record(
                        f,
                        pending[0],
                        pending[1],
                        pending[2],
                        pending[3],
                        current_sectors,
                        pending[4],
                    )
                    assert pending_frame is not None
                    if pending[0] in (TYPE_KEY, TYPE_KEY_LZ4):
                        index_entries.append((
                            pending_frame,
                            record_offset,
                            sectors,
                            index_flags_for_kind(pending[0]),
                        ))
                    record_total += sectors * RECORD_SECTOR_SIZE
                    padding_total += padding
                    pending_frame = n
                pending = current
                counts[kind] += 1
                video_payload_total += len(video_payload)
                audio_payload_total += len(audio_payload)
                n += 1
                if n % 24 == 0:
                    print(f"IPVF_PROGRESS:{n}", flush=True)

            if n == 0:
                raise RuntimeError("ffmpeg produced no frames")
            if audio_process is not None and audio_process.poll():
                raise RuntimeError("ffmpeg could not decode the first audio stream")
            assert pending is not None
            assert pending_frame is not None
            record_offset = f.tell()
            sectors, padding = write_record(
                f, pending[0], pending[1], pending[2], pending[3], 0,
                pending[4]
            )
            if pending[0] in (TYPE_KEY, TYPE_KEY_LZ4):
                index_entries.append((
                    pending_frame,
                    record_offset,
                    sectors,
                    index_flags_for_kind(pending[0]),
                ))
            record_total += sectors * RECORD_SECTOR_SIZE
            padding_total += padding
            media_end_offset = f.tell()
            media_id = ipvf_crc32_file_range(
                f, DATA_OFFSET, media_end_offset
            )
            f.seek(media_end_offset)
            if not index_entries or index_entries[0][0] != 0:
                raise RuntimeError("IPVF index must begin with frame 0")
            index_data = b"".join(
                struct.pack("<IQHH", frame, offset, record_sectors, flags)
                for frame, offset, record_sectors, flags in index_entries
            )
            index_offset = f.tell()
            if index_offset != media_end_offset:
                raise RuntimeError("IPVF index offset does not follow media")
            if media_end_offset + len(index_data) > DEVICE_FILE_SIZE_MAX:
                raise RuntimeError(
                    "IPVF exceeds the supported 2-GiB file limit; "
                    "transparent segmentation is required"
                )
            f.write(index_data)
            flags = FLAGS
            if counts[TYPE_XOR_LZ4] != 0 or counts[TYPE_MOTION_LZ4] != 0:
                flags |= FLAG_TEMPORAL_XOR
            write_header(
                f,
                fps,
                n,
                first_record_sectors,
                flags,
                media_end_offset,
                len(index_entries),
                ipvf_crc32(index_data),
                media_id,
                metadata,
            )
        os.replace(temporary, output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    finally:
        if audio_file is not None:
            audio_file.close()
        if audio_process is not None:
            if audio_process.poll() is None:
                audio_process.kill()
            audio_process.wait()

    raw = n * FRAME_BYTES
    ratio = video_payload_total / raw if raw else 0
    print(f"{output}: {n} frames @ {fps.numerator}/{fps.denominator} fps")
    print(
        f"  key={counts[TYPE_KEY]} delta={counts[TYPE_RECTS]} "
        f"repeat={counts[TYPE_REPEAT]} key-lz4={counts[TYPE_KEY_LZ4]} "
        f"delta-lz4={counts[TYPE_RECTS_LZ4]} "
        f"xor-lz4={counts[TYPE_XOR_LZ4]} "
        f"motion-lz4={counts[TYPE_MOTION_LZ4]} mode={video_mode} "
        f"lz4={lz4_mode} color={color_depth}"
    )
    print(
        f"  video={video_payload_total:,} bytes "
        f"({ratio:.1%} of raw RGB565)"
    )
    print(
        f"  audio={audio_payload_total:,} bytes, modes={audio_counts}, "
        f"silence-pad={audio_padding_total:,} bytes "
        f"({AUDIO_SAMPLE_RATE} Hz adaptive IMA ADPCM)"
    )
    print(
        f"  records={record_total:,} bytes, padding={padding_total:,} bytes "
        f"({padding_total / record_total:.1%} of records)"
    )
    print(
        f"  index={len(index_entries)} keyframes, "
        f"metadata={len(encode_metadata(metadata))} bytes"
    )
