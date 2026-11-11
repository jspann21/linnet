"""Read-only M25 board observations from the target's bounded diagnostics."""
import struct

from .identity import decode
from .protocol import Command, ProtocolError


def read(session, identity):
    """Report live board/RAM facts, without deriving hardware from retail models."""
    if identity['target_id'] != 'video5g-m25' or identity['image_kind'] != 'os':
        raise ValueError('M25 hardware observations require a running Video OS')
    fields = ('target_id', 'image_kind', 'profile', 'build_id', 'image_sha256', 'generation')

    def same_image():
        observed = decode(session.status())
        if any(observed[key] != identity[key] for key in fields):
            raise RuntimeError('Runtime identity changed during hardware observation')

    def words(selector, count):
        payload = session.request(Command.ATA_DIAGNOSTICS, struct.pack('<I', selector))
        if len(payload) != count * 4:
            raise ProtocolError('Invalid M25 hardware diagnostic length')
        return struct.unpack('<' + 'I' * count, payload)

    same_image()
    memory = words(0x4d250027, 12)
    board = words(0x4d250026, 32)
    same_image()
    base = 32 * 1024 * 1024
    if (memory[0] != 0x4d250027 or memory[3] not in (0, base) or
            memory[1] != base + memory[3] or
            memory[2] != (0x12000000 if memory[3] else 0) or
            board[18] != 0x53797349 or board[16] >> 16 != 0xb):
        raise ProtocolError('Invalid M25 board or memory observation')
    # This is OS-admitted memory, not a physical chip inventory. An unadmitted
    # upper bank keeps the 32 MiB baseline regardless of original disk capacity.
    # Copy only hardware facts. Other words include runtime addresses and media
    # state, which do not belong in a shareable hardware inventory.
    return dict(board_revision=f'0x{board[16]:08X}', usable_ram_bytes=memory[1],
                extended_ram_bytes=memory[3])
