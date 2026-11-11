"""Shared theme, brightness and menu payload in the A/B state records."""
import os
from pathlib import Path
import re
import struct
import uuid

from tools.content.guard import guarded_path
from tools.studio.storage import record
from tools.studio.update_history import _decode_record


def decode_payload(payload):
    if not payload:
        return dict(theme=b'', scores=bytes(36), brightness=100, menus=None)
    if len(payload) < 127 or payload[:4] != b'THMS':
        raise ValueError('Invalid device preferences')
    version = struct.unpack_from('<I', payload, 4)[0]
    if version != 6 or len(payload) > 256:
        raise ValueError('Unsupported device preferences')
    end = len(payload) - 119
    theme = payload[8:end]
    if len(theme) > 80 or payload[end] or (theme and not re.fullmatch(rb'[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+', theme)):
        raise ValueError('Invalid theme preference')
    if not 5 <= payload[end+37] <= 100 or payload[end+38] != 1:
        raise ValueError('Invalid display preferences')
    from .screen_settings import decode
    decode(payload[end+39:end+119])
    return dict(theme=theme, scores=payload[end+1:end+37], brightness=payload[end+37],
                menus=payload[end+39:end+119])


def encode_payload(value):
    if len(value['scores']) != 36 or len(value['menus']) != 80 or not 5 <= value['brightness'] <= 100:
        raise ValueError('Invalid shared device preferences')
    payload = (b'THMS' + struct.pack('<I', 6) + value['theme'] + b'\0' + value['scores']
               + bytes((value['brightness'], 1)) + value['menus'])
    decode_payload(payload)
    return payload


def read(root):
    root = Path(root)
    tokens, valid = [], []
    for index in range(2):
        path = root / '.LINNET' / f'THEME{index}.BIN'
        info = guarded_path(path, root)
        if info is None:
            tokens.append(None)
            continue
        if not path.is_file() or info.st_size != 512:
            raise ValueError('Unsafe device preferences file')
        raw = path.read_bytes()
        tokens.append(raw)
        try:
            generation, payload = _decode_record(raw) if any(raw) else (0, b'')
        except ValueError:
            continue  # Match the firmware's A/B recovery from one damaged copy.
        valid.append((generation, index, payload))
    if not valid:
        if any(token is not None for token in tokens):
            raise ValueError('Both device preference copies are damaged')
        return dict(value=decode_payload(b''), tokens=tokens, generation=0, active=None)
    if len(valid) == 2 and valid[0][0] == valid[1][0] and tokens[0] != tokens[1]:
        raise ValueError('Conflicting device preference copies')
    generation, active, payload = max(valid, key=lambda entry: (entry[0], -entry[1]))
    return dict(value=decode_payload(payload), tokens=tokens, generation=generation, active=active)


def publish(root, snapshot, menus, guard):
    root = Path(root)
    value = dict(snapshot['value'], menus=menus)
    payload = encode_payload(value)
    tokens = list(snapshot['tokens'])

    def unchanged():
        guard()
        for index, expected in enumerate(tokens):
            path = root / '.LINNET' / f'THEME{index}.BIN'
            info = guarded_path(path, root)
            current = path.read_bytes() if info is not None and path.is_file() and info.st_size == 512 else None
            if (info is not None and current is None) or current != expected:
                raise ValueError('Device preferences changed while saving. Reload from iPod and review your edits.')

    def replace(index, data):
        path = root / '.LINNET' / f'THEME{index}.BIN'
        temporary = path.with_name('PREF-' + uuid.uuid4().hex[:8] + '.TMP')
        guarded_path(temporary, root)
        owned = False
        try:
            with temporary.open('xb') as stream:
                owned = True
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            unchanged()
            os.replace(temporary, path)
            tokens[index] = data
            if path.read_bytes() != data:
                raise IOError('Device preferences readback failed')
        finally:
            if owned and temporary.exists():
                guard()
                guarded_path(temporary, root)
                temporary.unlink()

    unchanged()
    # Both files must exist for the firmware's optional transactional store.
    for index in range(2):
        if tokens[index] is None:
            replace(index, record(b''))
    generation = max(snapshot['generation'], 1)
    if generation == 0xffffffff:
        raise ValueError('Device preference generation exhausted')
    inactive = 0 if snapshot['active'] is None else snapshot['active'] ^ 1
    replace(inactive, record(payload, generation + 1))
