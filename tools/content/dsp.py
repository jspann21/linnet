"""Compile explicit host DSP settings to the 160-byte DSP profile.

RBJ peaking EQ equations: https://www.w3.org/TR/audio-eq-cookbook/ . Coefficients
are normalized by a0, quantized to signedQ2.30, and checked using the same strict
Jury inequalities as the native runtime. Unrepresentable or unstable results are
rejected, never clipped into a different filter. No EQ/crossfeed is enabled by
default; preamp and ReplayGain are unity, channel mode stereo, balance centered.

DSP layout (little endian): magic0;u16reserved0@4;u16size160@6;u32sample_rate@8;
u32flags0@12;reservedzero16..27;u32gainQ16@28;u32ReplayGainQ16@32;i32balance@36;
u32mode@40;u32band_count@44;five20-byte signedQ2.30(b0,b1,b2,a1,a2)@48;
u32crossfeed_amountQ16@148;u32lowpass_coefficientQ16@152;CRC32/ISO-HDLC@156.
Inactive filter records are all zero. CRC covers bytes0..155. Stream sample rate
must match exactly; redesign profiles when sample rate changes.

Input mapping keys: sample_rate(required),preamp_db(default0),balance(-1000..1000),
mode(stereo/mono/left/right/karaoke),bands(list<=5 of frequency_hz/q/gain_db),
crossfeed(optional amount0..0.5 and cutoff_hz). No implicit preset, resampling,
automatic headroom attenuation, or per-track ReplayGain tagging is applied.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

PROFILE_BYTES = 160
Q30 = 1 << 30
MODES = {"stereo": 0, "mono": 1, "left": 2, "right": 3, "karaoke": 4}


def _finite(value: object, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be a finite number")
    try:
        value = float(value)
    except OverflowError as error:
        raise ValueError(f"{name} must be a finite number") from error
    if not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number")
    return value


def _nearest(value: float) -> int:
    return math.floor(value + 0.5) if value >= 0 else -math.floor(-value + 0.5)


def _keys(mapping: object, allowed: set[str], required: set[str], name: str) -> dict:
    if not isinstance(mapping, dict) or set(mapping) - allowed or required - set(mapping):
        raise ValueError(f"invalid {name} keys; allowed: {', '.join(sorted(allowed))}")
    return mapping


def gain_from_db(db: float) -> int:
    """Host exact-math gain, roundedQ16, same saturation bounds as native."""
    value = _finite(db, "gain dB")
    if value <= -96:
        return 1
    if value >= 25:
        return 16 * 65536
    return max(1, min(16 * 65536, _nearest(10 ** (value / 20) * 65536)))


def peaking_eq(sample_rate: int, frequency_hz: float, q: float, gain_db: float) -> tuple[int, ...]:
    if type(sample_rate) is not int or not 8000 <= sample_rate <= 192000:
        raise ValueError("sample rate must be8000..192000Hz")
    frequency, quality, gain = (_finite(frequency_hz, "frequency"), _finite(q, "Q"), _finite(gain_db, "EQ gain"))
    if not 0 < frequency < sample_rate / 2 or not 0.1 <= quality <= 20 or not -24 <= gain <= 24:
        raise ValueError("EQ requires0<f<Nyquist, Q0.1..20, gain-24..24dB")
    omega = 2 * math.pi * frequency / sample_rate
    amplitude = 10 ** (gain / 40)
    alpha = math.sin(omega) / (2 * quality)
    cosine = math.cos(omega)
    a0 = 1 + alpha / amplitude
    values = ((1 + alpha * amplitude) / a0, -2 * cosine / a0,
              (1 - alpha * amplitude) / a0, -2 * cosine / a0, (1 - alpha / amplitude) / a0)
    coefficients = tuple(_nearest(value * Q30) for value in values)
    if any(not -(1 << 31) <= value < 1 << 31 for value in coefficients):
        raise ValueError("EQ coefficients exceed native signedQ2.30 range; adjust gain/Q")
    _jury(coefficients)
    return coefficients


def _jury(coefficients: tuple[int, ...]) -> None:
    _, _, _, a1, a2 = coefficients
    if not -Q30 < a2 < Q30 or Q30 + a1 + a2 <= 0 or Q30 - a1 + a2 <= 0:
        raise ValueError("quantized EQ poles are not strictly stable")


def design_profile(settings: dict) -> tuple[bytes, dict]:
    settings = _keys(settings, {"sample_rate", "preamp_db", "balance", "mode", "bands", "crossfeed"}, {"sample_rate"}, "DSP profile")
    rate = settings["sample_rate"]
    if type(rate) is not int or not 8000 <= rate <= 192000:
        raise ValueError("sample_rate must be an integer8000..192000")
    preamp = _finite(settings.get("preamp_db", 0), "preamp_db")
    if not -96 <= preamp <= 24:
        raise ValueError("preamp must be-96..24dB")
    balance = settings.get("balance", 0)
    mode = settings.get("mode", "stereo")
    if type(balance) is not int or not -1000 <= balance <= 1000 or not isinstance(mode, str) or mode not in MODES:
        raise ValueError("invalid balance/channel mode")
    bands = settings.get("bands", [])
    if not isinstance(bands, list) or len(bands) > 5:
        raise ValueError("at most five peaking EQ bands are supported")
    coefficients = []
    for band in bands:
        band = _keys(band, {"frequency_hz", "q", "gain_db"}, {"frequency_hz", "q", "gain_db"}, "EQ band")
        coefficients.append(peaking_eq(rate, band["frequency_hz"], band["q"], band["gain_db"]))
    crossfeed = _keys(settings.get("crossfeed", {}), {"amount", "cutoff_hz"}, set(), "crossfeed")
    amount = _finite(crossfeed.get("amount", 0), "crossfeed amount")
    if not 0 <= amount <= 0.5:
        raise ValueError("crossfeed amount must be0..0.5")
    amount_q16 = _nearest(amount * 65536)
    coefficient_q16 = 0
    if amount:
        if "cutoff_hz" not in crossfeed:
            raise ValueError("enabled crossfeed requires an explicit cutoff_hz")
        cutoff = _finite(crossfeed["cutoff_hz"], "crossfeed cutoff")
        if not 0 < cutoff < rate / 2:
            raise ValueError("crossfeed cutoff must lie below Nyquist")
        coefficient_q16 = _nearest((1 - math.exp(-2 * math.pi * cutoff / rate)) * 65536)
        if not amount_q16 or not 1 <= coefficient_q16 <= 65535:
            raise ValueError("crossfeed coefficient/amount underflows native precision")
    elif "cutoff_hz" in crossfeed:
        # Still reject invalid dormant settings; do not enable a hidden filter.
        cutoff = _finite(crossfeed["cutoff_hz"], "crossfeed cutoff")
        if not 0 < cutoff < rate / 2:
            raise ValueError("crossfeed cutoff must lie below Nyquist")
    profile = bytearray(PROFILE_BYTES)
    struct.pack_into("<4sHHI", profile, 0, b"LDSP", 0, PROFILE_BYTES, rate)
    struct.pack_into("<IIiII", profile, 28, gain_from_db(preamp), 65536, balance, MODES[mode], len(coefficients))
    for index, band in enumerate(coefficients):
        struct.pack_into("<5i", profile, 48 + index * 20, *band)
    struct.pack_into("<II", profile, 148, amount_q16, coefficient_q16)
    struct.pack_into("<I", profile, 156, zlib.crc32(profile[:156]))
    result = bytes(profile)
    validated = validate_profile(result, rate)
    report = dict(format="LDSP", sample_rate=rate, sha256=hashlib.sha256(result).hexdigest(),
                  bytes=PROFILE_BYTES, settings=settings, native=validated,
                  coefficient_source="https://www.w3.org/TR/audio-eq-cookbook/",
                  replaygain="unity; apply track gain separately before that track produces PCM")
    return result, report


def validate_profile(data: bytes, actual_rate: int) -> dict:
    if len(data) != PROFILE_BYTES:
        raise ValueError("DSP profile must be160 bytes")
    magic, reserved, size, rate = struct.unpack_from("<4sHHI", data)
    if (magic, reserved, size) != (b"LDSP", 0, PROFILE_BYTES) or rate != actual_rate or not 8000 <= rate <= 192000 or any(data[12:28]):
        raise ValueError("DSP header/sample rate mismatch")
    if zlib.crc32(data[:156]) != struct.unpack_from("<I", data, 156)[0]:
        raise ValueError("DSP CRC mismatch")
    gain, replaygain, balance, mode, count = struct.unpack_from("<IIiII", data, 28)
    amount, coefficient = struct.unpack_from("<II", data, 148)
    if (gain > 16 * 65536 or replaygain > 16 * 65536 or not -1000 <= balance <= 1000 or mode > 4 or count > 5 or
        amount > 32768 or coefficient > 65535 or (amount and not coefficient)):
        raise ValueError("DSP configuration bounds")
    bands = []
    for index in range(5):
        band = struct.unpack_from("<5i", data, 48 + index * 20)
        if index < count:
            _jury(band)
            bands.append(band)
        elif any(band):
            raise ValueError("DSP inactive band must be zero")
    return dict(gain_q16=gain, replaygain_q16=replaygain, balance=balance, mode=mode,
                bands=bands, crossfeed_amount_q16=amount, crossfeed_coefficient_q16=coefficient)


def compile_profile(settings: dict, output: Path) -> dict:
    """Write a local DSP binary and adjacent JSON provenance report."""
    data, report = design_profile(settings)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    if output.read_bytes() != data:
        raise OSError("DSP profile readback mismatch")
    report["path"] = str(output.resolve())
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report
