"""Retail model metadata, separate from device/firmware admission checks.

Model numbers describe the original product, never its current drive, RAM,
panel, or flash. Unknown and ambiguous identifiers deliberately stay unknown.
"""
import re

from .ipod_model_data import MODELS, SERIAL_MODELS


GENERATIONS = {
    'FIRST': ('iPod (1st generation)', None),
    'SECOND': ('iPod (2nd generation)', None),
    'THIRD': ('iPod (3rd generation)', None),
    'FOURTH': ('iPod (4th generation, monochrome)', 'ipod4g-q21'),
    'PHOTO': ('iPod Photo / Color', 'photo4g-p98'),
    'VIDEO_1': ('iPod Video (5th generation)', 'video5g-m25'),
    'VIDEO_2': ('iPod Video (5th generation, Late 2006 / 5.5G)', 'video5g-m25'),
    'CLASSIC_1': ('iPod classic (2007 / 6th generation)', 'classic6g-n25'),
    'CLASSIC_2': ('iPod classic (2008 / 6.5th generation)', 'classic6g-n25'),
    'CLASSIC_3': ('iPod classic (Late 2009 / 7th generation)', 'classic6g-n25'),
}


def normalize_model(value):
    """Accept base, regional order number, and libgpod's leading-M shorthand."""
    if not isinstance(value, str):
        return None
    value = value.strip().upper()
    if value in MODELS:
        return value
    # Only known bases may lose a regional suffix. A1238 is a shared regulatory
    # number and must never identify a generation; neither may arbitrary text.
    for base in sorted(MODELS, key=len, reverse=True):
        if re.fullmatch(re.escape(base) + r'[A-Z]{1,3}(?:/A)?', value):
            return base
    if re.fullmatch(r'[A-C][0-9]{3}', value) or re.fullmatch(r'[0-9]{4}', value):
        base = 'M' + value
        return base if base in MODELS else None
    return None


def model_info(model):
    base = normalize_model(model)
    if base is None:
        return None
    generation, capacity, color = MODELS[base]
    name, target = GENERATIONS.get(generation, (None, None))
    if name is None:
        family, _, number = generation.partition('_')
        name = f'iPod {family.lower()} (generation {number})'
    return dict(model_number=base, product_name=name, generation=generation,
                original_capacity_gb=capacity, color=color, target_id=target)


def identify(model=None, *, product_serial=None, target=None):
    """Prefer a live product serial; retain conflicts as diagnostic metadata.

    The caller supplies a product serial from the currently bound Apple disk,
    never the hexadecimal USB board ID or a saved serial on transplanted media.
    Only legacy 11-character product serials use this three-character table.
    """
    saved = model_info(model)
    candidates = set()
    if isinstance(product_serial, str) and re.fullmatch(r'[A-Z0-9]{11}', product_serial.upper()):
        candidates = set(SERIAL_MODELS.get(product_serial.upper()[-3:], ()))
    live = model_info(next(iter(candidates))) if len(candidates) == 1 else None
    selected = live or saved
    if selected is None:
        return dict(model_identity_status='unknown', model_identity_source='unavailable')
    if target is not None and selected['target_id'] != target:
        return dict(model_identity_status='conflict', model_identity_source='live-product-serial'
                    if live else 'sysinfo')
    conflict = bool(live and saved and live['model_number'] != saved['model_number'])
    return dict(selected, model_identity_status='identified',
                model_identity_source='live-product-serial' if live else 'sysinfo',
                model_identity_conflict=conflict)
