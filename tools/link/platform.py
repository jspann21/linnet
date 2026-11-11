"""Named platform reports, verified screen encoding and bounded input completion."""
import struct
import time
import zlib
from .identity import decode
from .protocol import Command, ProtocolError

FIELDS = ('magic','generation','flags','selected','scene','events_accepted','events_processed',
          'event_drops','max_latency_us','queue_high','deadline_high','arena_high','frames_submitted',
          'frames_completed','lcd_max_us','cop_head','cop_tail','cop_resets','sleep_count','wake_count',
          'idle_us','idle_entries','physical_buttons','physical_hold','physical_headphones',
          'synthetic_flags','last_injection','animation_frame','clock_hz','battery_mv','charge_flags','irq_max_us')
MAGIC = int.from_bytes(b'PLAT','little')
BUTTONS = dict(menu=1, play=2, next=4, previous=8, select=16, release=0)


def state(session, generation):
    return _state(session.request(Command.PLATFORM_STATE), generation)


def _state(raw, generation):
    if len(raw) != 128:
        raise ProtocolError('invalid platform state length')
    result = dict(zip(FIELDS, struct.unpack('<32I', raw)))
    if result['magic'] != MAGIC or result['generation'] != generation:
        raise ProtocolError('platform state protocol or boot generation changed')
    return result


def ui_state(session, generation):
    return _ui_state(session.request(Command.UI_STATE), generation)


def _ui_state(raw, generation):
    if len(raw) != 64 or raw[:8] != b'UIST\0\0\0\0':
        raise ProtocolError('invalid native UI report')
    names = ('magic','reserved','scene','selected','scene_epoch','transitions','paints',
             'painted_pixels','visible_items','dirty','action','arena_used','arena_high',
             'arena_generation','generation','scenes')
    result = dict(zip(names, struct.unpack('<16I', raw)))
    if result['generation'] != generation:
        raise ProtocolError('UI boot generation changed')
    return result


def send_input(session, identity, events, timeout=5):
    if not 0 < timeout <= 60:
        raise ValueError('input completion timeout must be in (0, 60] seconds')
    # These independent reads share the existing Link window. Separate
    # round trips let a full codec turn run between every small query.
    identity_raw, platform_raw = session._batch([
        (Command.STATUS, b''), (Command.PLATFORM_STATE, b'')])
    confirmed = decode(identity_raw)
    if identity['image_kind'] != 'os' or any(confirmed[key] != identity[key] for key in
            ('target_id', 'profile', 'image_kind', 'build_id', 'image_sha256', 'generation')):
        raise ValueError('input requires the same running OS identity')
    generation = identity['generation']
    before = _state(platform_raw, generation)
    sequence = before['last_injection']
    session.platform_input(generation, [(sequence + index + 1, kind, value)
                                       for index, (kind, value) in enumerate(events)])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        platform_raw, ui_raw = session._batch([
            (Command.PLATFORM_STATE, b''), (Command.UI_STATE, b'')])
        current = _state(platform_raw, generation)
        ui = _ui_state(ui_raw, generation)
        if current['event_drops'] != before['event_drops']:
            raise RuntimeError('Input was accepted but events were dropped; inspect state, do not replay')
        if (current['events_processed'] == current['events_accepted'] and
                current['frames_completed'] == current['frames_submitted'] and not ui['dirty'] and
                (current['events_accepted'] != 0xffffffff or not session.executive_workload()['queued'])):
            return dict(accepted=True, settled=True, platform=current, ui=ui)
        time.sleep(.005)
    raise TimeoutError('Input was accepted but presentation did not settle; inspect state, do not replay')


def png(pixels, width, height):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            value = struct.unpack_from('<H', pixels, (y*width+x)*2)[0]
            rows.extend(((value>>11)*255//31, ((value>>5)&63)*255//63, (value&31)*255//31))
    def chunk(kind, payload):
        return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+
            chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
