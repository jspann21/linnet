"""Wire framing shared by serial sessions and the simulator."""
from dataclasses import dataclass
from enum import IntEnum
import struct
import zlib

MAGIC = b"LINK"
HEADER = struct.Struct("<4sBBHHHII")
MAX_PAYLOAD = 1024
MAX_FRAME = HEADER.size + MAX_PAYLOAD + 4
ACK = 1
ERROR = 2


class Command(IntEnum):
    HELLO = 1
    INFO = 2
    STATUS = 3
    UPLOAD_BEGIN = 16
    UPLOAD_CHUNK = 17
    UPLOAD_END = 18
    VERIFY = 19
    BOOT_RAM = 20
    REBOOT_NORMAL = 32
    REBOOT_RECOVERY = 33
    REBOOT_DISK = 34
    READ_LOG = 48
    READ_METRICS = 49
    READ_CRASH = 50
    CLEAR_CRASH = 51
    CRASH = 52
    STARTUP_TIMING = 53
    BOOT_TIMELINE = 54
    PLATFORM_STATE = 64
    INJECT_INPUT = 65
    CAPTURE_SCREEN = 66
    SCREEN_READ = 67
    PLATFORM_CONTROL = 68
    UI_STATE = 69
    INPUT_STATE = 70
    POWER_STATE = 71
    AUDIO_STATE = 112
    AUDIO_CONFIG = 113
    AUDIO_PCM = 114
    AUDIO_CONTROL = 115
    PLAYER_STATE = 116
    PLAYER_OPEN = 117
    PLAYER_CONTROL = 118
    LIBRARY_STATE = 119
    PLAYER_TIMING_STATE = 120
    PHOTOS_STATE = 121
    VIDEO_STATE = 122
    VIDEO_MEASUREMENT = 123
    VIDEO_OPEN = 124
    STORAGE_STATE = 80
    SCRATCH_READ = 81
    SCRATCH_WRITE = 82
    SETTINGS_GET = 83
    SETTINGS_PUT = 84
    UPDATE_BEGIN = 85
    UPDATE_CHUNK = 86
    UPDATE_FINISH = 87
    UPDATE_STATUS = 88
    STORAGE_FAULT = 89
    STORAGE_PREPARE = 90
    STORAGE_RECOVER = 91
    REBOOT_PERSISTENT = 92
    PUBLICATION_FAULT = 93
    MEDIA_INFO = 95
    ATA_DIAGNOSTICS = 94
    FILE_RESIZE = 96
    WORK_READ = 97
    WORK_WRITE = 98
    BOOTSTRAP_CONTROL = 0x7f00


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Frame:
    command: int
    sequence: int
    payload: bytes = b""
    flags: int = 0

    def encode(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ProtocolError("payload exceeds 1024 bytes")
        if not 1 <= self.sequence <= 0xFFFFFFFF:
            raise ProtocolError("sequence outside 1..UINT32_MAX")
        if self.flags not in (0, ACK, ACK | ERROR):
            raise ProtocolError("invalid frame flags")
        packet = HEADER.pack(MAGIC, 0, 0, self.flags, self.command, 0,
                             self.sequence, len(self.payload)) + self.payload
        return packet + struct.pack("<I", zlib.crc32(packet))


class Parser:
    """Incremental parser with at most one frame buffered; rejects corruption."""

    def __init__(self):
        self.buffer = bytearray()
        self.expected = HEADER.size

    def feed(self, data: bytes) -> list[Frame]:
        frames = []
        try:
            incoming = memoryview(data)
            offset = 0
            while offset < len(incoming):
                count = min(self.expected - len(self.buffer), len(incoming) - offset)
                self.buffer.extend(incoming[offset:offset + count])
                offset += count
                if len(self.buffer) == HEADER.size:
                    magic, reserved8a, reserved8b, flags, command, reserved, seq, length = HEADER.unpack(self.buffer)
                    if (magic != MAGIC or reserved8a or reserved8b or reserved
                            or flags not in (0, ACK, ACK | ERROR) or not seq
                            or length > MAX_PAYLOAD):
                        raise ProtocolError("invalid Linnet Link frame header")
                    self.expected = HEADER.size + length + 4
                if len(self.buffer) == self.expected:
                    raw = self.buffer
                    if zlib.crc32(raw[:-4]) != struct.unpack_from("<I", raw, len(raw) - 4)[0]:
                        raise ProtocolError("Linnet Link CRC mismatch")
                    _, _, _, flags, command, _, seq, _ = HEADER.unpack_from(raw)
                    frames.append(Frame(command, seq, bytes(raw[HEADER.size:-4]), flags))
                    self.buffer = bytearray()
                    self.expected = HEADER.size
        except ProtocolError:
            self.buffer.clear()
            self.expected = HEADER.size
            raise
        return frames
