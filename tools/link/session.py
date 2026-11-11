"""Deadline bounded transactions. An uncertain acknowledgement poisons a session."""
from collections import deque
import hashlib
import struct
import time
import zlib
from .protocol import ACK, ERROR, MAX_FRAME, MAX_PAYLOAD, Command, Frame, Parser, ProtocolError
from .transport import Transport


class AmbiguousCommand(RuntimeError):
    """Reconnect and inspect device state before deciding whether to act again."""


class AmbiguousUpdateFinish(AmbiguousCommand):
    """UPDATE_FINISH was attempted; inspect publication state without replay."""


class DeviceError(RuntimeError):
    def __init__(self, message, *, acknowledged_command=None):
        super().__init__(message)
        self.acknowledged_command = acknowledged_command


class Session:
    def video_measurement(self, generation: int, mode: int):
        """Arm one dev-only frame-zero run: 0 cancel, 1 display, 2 no display.

        Requires inactive/drained video. Stops after eight seconds of playback
        (or preparation), preserves resume, and does not open media itself.
        """
        if type(generation) is not int or not 0 <= generation <= 0xffffffff or type(mode) is not int or mode not in (0, 1, 2):
            raise ValueError("invalid generation or video measurement mode")
        raw = self.request(Command.VIDEO_MEASUREMENT, struct.pack('<II', generation, mode))
        if raw != struct.pack('<II', generation, mode):
            self.poisoned = True
            raise AmbiguousCommand("video measurement acknowledgement mismatch; inspect state before another mutation")
        return dict(generation=generation, mode=mode, playback_limit_seconds=8)

    def video_startup_timing(self) -> dict:
        """Read device-side open-to-first-presentation/index/audio latency in us."""
        raw = self.request(Command.VIDEO_STATE, struct.pack('<I', 8))
        if len(raw) != 128:
            raise ProtocolError('invalid video startup timing length')
        words = struct.unpack('<32I', raw)
        if not any(words):
            # Backends without this measurement page (BCM2722) return zeros.
            return dict(available=False)
        if words[:2] != (int.from_bytes(b'VMEA', 'little'), 0) or any(words[8:]):
            raise ProtocolError('invalid video startup timing header')
        return dict(zip(('magic', 'reserved', 'opened_us', 'first_frame_us',
                         'index_ready_us', 'audio_start_us', 'start_ready_frames', 'dropped_frames'), words[:8]))

    def __init__(self, transport: Transport, timeout: float = 2.0):
        if not 0 < timeout <= 60:
            raise ValueError("timeout must be in (0, 60] seconds")
        self.transport = transport
        self.timeout = timeout
        self.sequence = 0
        self.parser = Parser()
        self.pending = deque()
        self.poisoned = False
        self.image_size = 0
        self.handoff_until = 0.0

    def _send(self, requests: list[tuple[Command, bytes]], deadline: float) -> list[Frame]:
        if self.poisoned:
            raise AmbiguousCommand("session requires reconnect and state inspection")
        if self.sequence > 0xFFFFFFFF - len(requests):
            raise ProtocolError("sequence exhausted; open a new session")
        sent, packets = [], []
        for command, payload in requests:
            self.sequence += 1
            frame = Frame(command, self.sequence, payload)
            packets.append(frame.encode())
            sent.append(frame)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("command deadline expired before write")
        # The existing four-frame window shares one transport write; each frame
        # retains its own CRC, sequence and acknowledgement retirement.
        self.transport.write(b''.join(packets), remaining)
        return sent

    def _ack(self, sent: Frame, deadline: float) -> bytes:
        while not self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("acknowledgement deadline expired")
            data = self.transport.read(MAX_FRAME, remaining)
            if len(data) > MAX_FRAME:
                raise ProtocolError("transport exceeded read bound")
            self.pending.extend(self.parser.feed(data))
        reply = self.pending.popleft()
        if reply.sequence != sent.sequence or reply.command != sent.command or not reply.flags & ACK:
            raise ProtocolError("unexpected command or acknowledgement sequence")
        if reply.flags & ERROR:
            raise DeviceError(f"device rejected {Command(sent.command).name}: {reply.payload.hex()}",
                              acknowledged_command=sent.command)
        return reply.payload

    def _batch(self, requests: list[tuple[Command, bytes]], timeout=None) -> list[bytes]:
        if not 1 <= len(requests) <= 4:
            raise ValueError("stream window must contain 1..4 frames")
        duration = self.timeout if timeout is None else timeout
        if not 0 < duration <= 660:
            raise ValueError("transaction timeout must be in (0, 660] seconds")
        deadline = time.monotonic() + duration
        try:
            sent = self._send(requests, deadline)
            replies = [self._ack(frame, deadline) for frame in sent]
            if self.pending or self.parser.buffer:
                raise ProtocolError('unsolicited bytes after the final acknowledgement')
            return replies
        except (KeyboardInterrupt, SystemExit):
            self.poisoned = True
            raise
        except (ProtocolError, DeviceError, OSError, TimeoutError) as exc:
            self.poisoned = True
            if isinstance(exc, DeviceError) and len(requests) == 1:
                raise
            names = ",".join(Command(command).name for command, _ in requests)
            raise AmbiguousCommand(f"{names}: {exc}; command outcome uncertain; reconnect and inspect identity/state; never replay automatically") from exc

    def request(self, command: Command, payload: bytes = b"", timeout=None) -> bytes:
        try:
            return self._batch([(command, payload)], timeout)[0]
        finally:
            if command in (Command.BOOT_RAM, Command.REBOOT_NORMAL, Command.REBOOT_RECOVERY,
                           Command.REBOOT_DISK, Command.CRASH, Command.REBOOT_PERSISTENT):
                # Receiving an acceptance ACK can precede the device retiring
                # its TX descriptor. Closing now drops DTR and could cancel
                # the deferred action. Preserve a bounded handoff interval,
                # including when acknowledgement delivery was ambiguous.
                self.handoff_until = time.monotonic() + 1.0

    def hello(self) -> bytes:
        return self.request(Command.HELLO)

    def info(self) -> bytes:
        return self.request(Command.INFO)

    def status(self, *, timeout=None) -> bytes:
        return self.request(Command.STATUS,timeout=timeout)

    def startup_timing(self, generation=None) -> dict:
        """Read retained startup ticks; missing stages are never reported as zero time."""
        raw = self.request(Command.STARTUP_TIMING)
        if len(raw) != 64:
            raise ProtocolError('invalid startup timing length')
        words = struct.unpack('<16I', raw)
        if not any(words):
            return dict(available=False)
        magic, reserved, observed, size, mask = words[:5]
        if (magic != int.from_bytes(b'STUP', 'little') or reserved or
                not 32 <= size <= 0x800000 or mask > 255 or mask & (mask + 1) or
                any(words[13:]) or (generation is not None and observed != generation)):
            raise ProtocolError('invalid startup timing identity or mask')
        names = ('candidate_crc_start', 'candidate_crc_end', 'copy_dispatch',
                 'os_entry', 'bss_start', 'bss_end', 'iram_end', 'c_entry')
        ticks = {}
        for index, name in enumerate(names):
            if mask & (1 << index):
                ticks[name] = words[5 + index]
            elif words[5 + index]:
                raise ProtocolError('unpublished startup timestamp')
        pairs = {'candidate_crc': (0, 1), 'copy_and_entry': (2, 3),
                 'entry_setup': (3, 4), 'bss': (4, 5), 'iram': (5, 6),
                 'c_entry_setup': (6, 7)}
        intervals = {name: (words[5 + end] - words[5 + start]) & 0xffffffff
                     for name, (start, end) in pairs.items()
                     if mask & (1 << start) and mask & (1 << end)}
        return dict(available=True, generation=observed,
                    payload_bytes=size, valid_mask=mask, ticks_us=ticks,
                    intervals_us=intervals, complete=mask == 255)

    def boot_timeline(self, generation=None) -> dict:
        """Read the boot timeline; intervals require both stamps and use modulo32 ticks."""
        raw = self.request(Command.BOOT_TIMELINE)
        if len(raw) != 192:
            raise ProtocolError('invalid boot timeline length')
        words = struct.unpack('<48I', raw)
        if not any(words):
            return dict(available=False)
        magic, reserved, observed, mask = words[:4]
        if (magic != int.from_bytes(b'BTIM', 'little') or reserved or
                mask >> 28 or words[7] > 3 or words[40] > 31 or
                (not words[46] and any(words[36:40])) or
                (words[46] > 20 or (not words[46] and words[47])) or
                (generation is not None and observed != generation)):
            raise ProtocolError('invalid boot timeline identity or mask')
        names = ('boot_c', 'boot_recovery', 'boot_usb', 'boot_storage',
                 'boot_block', 'boot_fat', 'boot_files', 'boot_proof',
                 'boot_stores', 'boot_validate', 'boot_validated', 'chainload',
                 'os_c', 'os_recovery', 'os_runtime', 'os_clock', 'os_media',
                 'os_frame', 'os_runtime_end', 'os_usb', 'os_storage',
                 'os_block', 'os_fat', 'os_files', 'os_proof', 'os_confirmed',
                 'os_poll', 'os_request')
        ticks = {}
        for index, name in enumerate(names):
            if mask & (1 << index):
                ticks[name] = words[8 + index]
            elif words[8 + index]:
                raise ProtocolError('unpublished boot timestamp')
        pairs = {
            'boot_recovery': (0, 1), 'boot_development_init': (1, 2),
            'boot_ata_init': (3, 4), 'boot_fat_mount': (4, 5),
            'boot_file_discovery': (5, 6), 'boot_extent_proof': (6, 7),
            'boot_store_open': (7, 8), 'boot_reset_history': (8, 9),
            'boot_prepare_and_rejection_history': (9, 10),
            'boot_post_prepare_and_quiesce': (10, 11),
            'chainload_to_os_c': (11, 12), 'os_recovery': (12, 13),
            'os_clock_setup': (14, 15), 'os_media_init': (15, 16),
            'os_input_power_first_frame': (16, 17),
            'os_monitor_cache': (17, 18), 'os_development_init': (18, 19),
            'os_ata_init': (20, 21), 'os_fat_mount': (21, 22),
            'os_file_discovery': (22, 23), 'os_extent_proof': (23, 24),
            'os_confirmation': (24, 25), 'usb_ready_to_host_request': (19, 27),
            'boot_c_to_first_frame': (0, 17), 'boot_c_to_confirmation': (0, 25)}
        intervals = {name: (words[8 + end] - words[8 + start]) & 0xffffffff
                     for name, (start, end) in pairs.items()
                     if mask & (1 << start) and mask & (1 << end)
                     # A saved-theme first frame can finish after runtime setup.
                     # Do not report that reversed span as a timer wrap.
                     and (name != 'os_monitor_cache' or
                          ((words[8 + end] - words[8 + start]) & 0xffffffff) < 0x80000000)}
        counters = {}
        for index, name in enumerate(('image_read_us', 'image_sha_us',
                                      'image_los_validation_us', 'image_bytes',
                                      'image_read_calls')):
            if words[40] & (1 << index):
                counters[name] = words[41 + index]
            elif words[41 + index]:
                raise ProtocolError('unpublished boot counter')
        if words[7] & 1 and (mask & 0xfff) != 0xfff:
            raise ProtocolError('incomplete native boot timeline')
        return dict(available=True, generation=observed,
                    native_boot=bool(words[7] & 1), valid_mask=mask,
                    boot_sha_iram=bool(words[7] & 2),
                    inherited_registers=dict(clock_source=words[4], pll=words[5],
                                             cache_control=words[6]),
                    boot_clock=dict(status=('unobserved', 'skipped', 'attempting',
                                            'verified_80mhz', 'failed', 'skip_recovery_generation',
                                            'skip_audio_idle', 'skip_usb_clock_disabled',
                                            'skip_usb_module_reset', 'skip_usb_run_stuck',
                                            'skip_usb_flush_timeout', 'skip_usb_status_busy',
                                            'skip_settle_timer', 'skip_usb_reset_timeout',
                                            'skip_final_usb_state', 'skip_usb_reset_hold',
                                            'skip_usb_enable_readback', 'skip_usb_reset_release',
                                            'skip_usb_phy_ready', 'skip_usb_phy_settle',
                                            'skip_usb_xmb_readback')[words[46]],
                                    observed_clock_source=words[47],
                                    usb_registers=dict(device_enable=words[36],
                                                        device_reset=words[37],
                                                        device_init2=words[38],
                                                        usb_clock=words[39])),
                    ticks_us=ticks, intervals_us=intervals, counters=counters,
                    saturated_counters=[name for name, value in counters.items()
                                        if value == 0xffffffff])

    def _verify_image_source(self, image: bytes, image_path) -> None:
        from tools.inspect.image import inspect, verify_device_report
        from .identity import decode
        from .transport import SerialTransport

        if image_path is None or not isinstance(self.transport, SerialTransport):
            raise ValueError('Link image transfer requires a device-bound image path')
        port = self.transport.serial.port
        endpoints = [item for item in SerialTransport.discover() if item['port'].casefold() == port.casefold()]
        if len(endpoints) != 1 or not endpoints[0]['serial_number']:
            raise ValueError('Link image transfer requires one stable USB serial identity')
        verify_device_report(image_path, inspect(image), endpoints[0]['serial_number'],
                             decode(self.hello())['target_id'])

    def upload(self, image: bytes, *, image_path=None) -> dict:
        self._verify_image_source(image, image_path)
        return self._upload_unchecked(image)

    def _upload_unchecked(self, image: bytes) -> dict:
        if not image or len(image) > 16 * 1024 * 1024:
            raise ValueError("image must contain 1..16 MiB")
        crc = zlib.crc32(image)
        self.image_size = len(image)
        digest = hashlib.sha256(image).digest()
        self._offset_ack(self.request(Command.UPLOAD_BEGIN, struct.pack("<II", len(image), crc) + digest), 0)
        offset = 0
        while offset < len(image):
            requests, offsets = [], []
            for _ in range(4):
                chunk = image[offset:offset + MAX_PAYLOAD - 4]
                if not chunk:
                    break
                requests.append((Command.UPLOAD_CHUNK, struct.pack("<I", offset) + chunk))
                offset += len(chunk)
                offsets.append(offset)
            for ack, expected in zip(self._batch(requests), offsets):
                self._offset_ack(ack, expected)
        self._offset_ack(self.request(Command.UPLOAD_END, timeout=self._validation_timeout()), len(image))
        return dict(size=len(image), crc32=f"{crc:08x}", sha256=digest.hex())

    def _offset_ack(self, payload: bytes, expected: int) -> None:
        if len(payload) != 4 or struct.unpack("<I", payload)[0] != expected:
            self.poisoned = True
            raise AmbiguousCommand("upload acknowledgement offset mismatch; inspect device state")

    def verify(self) -> bytes:
        return self.request(Command.VERIFY, timeout=self._validation_timeout())

    def _validation_timeout(self):
        # Full-image hashing on uncached ARM7 memory is substantially slower
        # than a frame ACK. A lost validation ACK is still never retried.
        return min(600.0, max(15.0, self.image_size / 8192.0))

    def boot_ram(self) -> bytes:
        return self.request(Command.BOOT_RAM)

    def reboot(self, mode: str = "recovery") -> bytes:
        return self.request({"normal": Command.REBOOT_NORMAL, "recovery": Command.REBOOT_RECOVERY,
                             "disk": Command.REBOOT_DISK}[mode])

    def crash_get(self) -> bytes:
        return self.request(Command.READ_CRASH)

    def platform_state(self) -> tuple[int, ...]:
        payload = self.request(Command.PLATFORM_STATE)
        if len(payload) != 128:
            raise ProtocolError('invalid platform state size')
        return struct.unpack('<32I', payload)

    def power_state(self) -> dict:
        payload = self.request(Command.POWER_STATE)
        if len(payload) != 48 or payload[:4] != b'POWR':
            raise ProtocolError('invalid power state')
        names = ('magic', 'generation', 'sampled_us', 'samples', 'failures',
                 'bus_resets', 'battery_mv', 'battery_raw', 'charge_limit_ma',
                 'flags', 'battery_percent', 'sleeping')
        return dict(zip(names, struct.unpack('<12I', payload)))

    def input_state(self) -> dict:
        payload = self.request(Command.INPUT_STATE)
        if len(payload) != 64:
            raise ProtocolError('invalid input state size')
        words = struct.unpack('<16I', payload)
        result = dict(ready=bool(words[0]), last_packet=words[1], events=words[2],
                      handshakes=words[3], wheel_registers=list(words[4:8]),
                      seen_buttons=words[8], wheel_travel=words[9], remote=None)
        if words[10] >> 16 == 0x524d:
            flags = words[10] & 0xffff
            result['remote'] = dict(present=bool(flags & 1), armed=bool(flags & 2),
                headphones=bool(flags & 4), bus_fault=bool(flags & 8),
                volume_up_held=bool(flags & 16), volume_down_held=bool(flags & 32),
                center_latched=bool(flags & 64), repeat_limited=bool(flags & 128),
                last_action=(flags >> 8) & 7, pending_clicks=(flags >> 11) & 3,
                registers=dict(zip(('mode', 'center', 'volume', 'event'),
                                   struct.pack('<I', words[11]))),
                samples=words[12], failures=words[13], center_presses=words[14],
                volume_up_presses=words[15] & 0xffff, volume_down_presses=words[15] >> 16)
        return result

    def platform_input(self, generation: int, events: list[tuple[int, int, int]]) -> None:
        if type(generation) is not int or not 0 <= generation <= 0xffffffff:
            raise ValueError('invalid synthetic input boot generation')
        if not 1 <= len(events) <= 32:
            raise ValueError('synthetic input request requires 1..32 events')
        previous = 0
        for sequence, kind, value in events:
            if (any(type(item) is not int for item in (sequence, kind, value)) or
                    not previous < sequence <= 0xffffffff or kind not in (1, 2, 3, 4) or
                    (kind == 1 and not -64 <= value <= 64) or
                    (kind == 2 and not 0 <= value <= 31) or
                    (kind >= 3 and value not in (0, 1))):
                raise ValueError('invalid synthetic event or non-increasing input sequence')
            previous = sequence
        payload = struct.pack('<II', generation, len(events))
        payload += b''.join(struct.pack('<IIi', sequence, kind, value) for sequence, kind, value in events)
        reply = self.request(Command.INJECT_INPUT, payload)
        if reply != struct.pack('<II', generation, len(events)):
            self.poisoned = True
            raise AmbiguousCommand('synthetic input acknowledgement mismatch; do not replay')

    def video_open(self, generation: int, path: str) -> None:
        encoded = path.encode('utf-8')
        if not path.startswith('/') or b'\0' in encoded or len(encoded) > 254:
            raise ValueError('video path must be absolute and at most 254 bytes')
        reply = self.request(Command.VIDEO_OPEN, struct.pack('<I', generation) + encoded + b'\0')
        if reply != struct.pack('<II', generation, 0):
            self.poisoned = True
            raise AmbiguousCommand('video open acknowledgement mismatch; do not replay')

    def platform_control(self, generation: int, action: int, value: int = 0) -> None:
        payload = struct.pack('<III', generation, action, value)
        reply = self.request(Command.PLATFORM_CONTROL, payload)
        if reply != payload:
            self.poisoned = True
            raise AmbiguousCommand('platform control acknowledgement mismatch; do not replay')

    def capture_screen(self, generation: int) -> tuple[dict, bytes]:
        descriptor = self.request(Command.CAPTURE_SCREEN, struct.pack('<I', generation))
        if len(descriptor) != 28:
            raise ProtocolError('invalid screenshot descriptor')
        capture, observed, width, height, format_id, size, crc = struct.unpack('<7I', descriptor)
        if (not capture or observed != generation or format_id != 1 or
            not 1<=width<=1024 or not 1<=height<=1024 or width*height>131072 or size != width*height*2):
            raise ProtocolError('invalid screenshot identity or dimensions')
        pixels = bytearray()
        for first in range(0,size,4*1008):
            chunks = [(offset, min(1008,size-offset)) for offset in range(first,min(size,first+4*1008),1008)]
            replies = self._batch([(Command.SCREEN_READ,struct.pack('<IIII',capture,generation,offset,length))
                                   for offset,length in chunks])
            for (offset,length),reply in zip(chunks,replies):
                if len(reply)!=12+length or reply[:12]!=struct.pack('<III',capture,generation,offset):
                    raise ProtocolError('screenshot chunk identity or offset mismatch')
                pixels.extend(reply[12:])
        if zlib.crc32(pixels)!=crc:
            raise ProtocolError('screenshot CRC mismatch')
        return dict(capture_id=capture,generation=generation,width=width,height=height,
                    format='rgb565-le',crc32=f'{crc:08x}'),bytes(pixels)

    def storage_state(self, *, timeout=15) -> tuple[int, ...]:
        reply = self.request(Command.STORAGE_STATE, timeout=timeout)
        if len(reply) != 96 or reply[:4] != b'STOR':
            raise ProtocolError('invalid storage state')
        return struct.unpack('<24I', reply)

    def _diagnostic_words(self, selector: int, count: int, label: str) -> tuple[int, ...]:
        raw = self.request(Command.ATA_DIAGNOSTICS, struct.pack('<I', selector))
        if len(raw) != count * 4:
            raise ProtocolError(f'invalid {label} length')
        return struct.unpack(f'<{count}I', raw)

    def classic_power_state(self) -> dict:
        words = self._diagnostic_words(27, 32, 'Classic power state')
        if words[0] != int.from_bytes(b'PWRC', 'little') or words[1] != 1:
            raise ProtocolError('invalid Classic power state header')
        names = ('magic', 'version', 'clock_hz', 'fast_reason', 'transitions', 'failures',
                 'low_us_low', 'low_us_high', 'normal_us_low', 'normal_us_high',
                 'video_us_low', 'video_us_high', 'low_work_max_us', 'normal_work_max_us',
                 'low_loops', 'normal_loops', 'heavy_fallbacks', 'underrun_fallbacks',
                 'queued_frames', 'rate', 'clock_source', 'clock_dividers', 'pll0',
                 'pll2', 'pll_mode', 'ahb_gates', 'apb_gates', 'i2s_clock', 'i2s_tx',
                 'cpu_voltage_selector', 'input_deadline_us', 'disabled_flags')
        result = dict(zip(names, words))
        for kind in ('low', 'normal', 'video'):
            result[kind + '_us'] = result.pop(kind + '_us_low') | (result.pop(kind + '_us_high') << 32)
        if result['cpu_voltage_selector'] == 0xffffffff:
            result['cpu_voltage_selector'] = None
        result['i2s_gated'] = bool(words[26] & (1 << 7))
        result['dma0_gated'] = bool(words[25] & (1 << 25))
        result['pll2_users'] = (words[31] >> 8) & 3
        result['pll2_owned'] = bool(words[31] & (1 << 16))
        result['disabled_flags'] &= 3
        return result

    def storage_performance(self) -> dict:
        """Read storage performance; no I/O, reset or arming. Timings overlap, not additive.

        New counters saturate (flags bit31). Compare same-identity/epoch idle
        endpoints for the bounded run; pending attempts bias terminal wall averages.
        """
        words = self._diagnostic_words(1, 32, 'storage performance')
        if words[0] != int.from_bytes(b'SIOP', 'little') or words[1] or words[3] & ~0x8000000f:
            raise ProtocolError('invalid storage performance header')
        names = ('magic', 'reserved', 'snapshot_us', 'flags', 'ata_commands', 'ata_sectors_read',
                 'ata_read_commands', 'ata_read_terminal_attempts', 'ata_read_wall_us', 'ata_read_wall_max_us',
                 'ata_sector_service_us', 'ata_sector_service_max_us', 'ata_read_status_polls',
                 'ata_busy_observations', 'ata_no_drq_observations', 'ata_reset_epoch',
                 'block_calls', 'block_service_us', 'block_service_max_us', 'block_read_gaps',
                 'block_read_gap_us', 'block_read_gap_max_us', 'block_cache_hits', 'block_read_ahead_sectors',
                 'block_submitted', 'block_completed', 'block_failed', 'block_retries', 'block_canceled',
                 'block_pending', 'ata_errors', 'ata_timeouts')
        result = dict(zip(names, words))
        result['saturated'] = bool(words[3] & 0x80000000)
        result['idle_endpoint'] = not (words[3] & 15) and words[29] == 0
        return result

    def block_workload(self) -> dict:
        words = self._diagnostic_words(13, 8, 'block workload')
        if words[0] != int.from_bytes(b'BIOQ', 'little'):
            raise ProtocolError('invalid block workload header')
        keys = ('magic', 'read_calls', 'requested_read_sectors', 'requested_write_sectors',
                'cache_hits', 'read_ahead_sectors', 'failed', 'canceled')
        return dict(zip(keys, words))

    def mp4_table_workload(self) -> dict:
        words = self._diagnostic_words(16, 12, 'MP4 table workload')
        if words[0] != int.from_bytes(b'MPTB', 'little') or words[11]:
            raise ProtocolError('invalid MP4 table workload header')
        keys = ('magic', 'attempts', 'completed', 'pending', 'failed', 'bytes',
                'map_reads', 'chunk_reads', 'size_reads', 'sync_reads', 'seeks', 'reserved')
        return dict(zip(keys, words))

    def music_budget_workload(self) -> dict:
        words = self._diagnostic_words(17, 12, 'music budget workload')
        if words[0] != int.from_bytes(b'MBDG', 'little') or words[11]:
            raise ProtocolError('invalid music budget workload header')
        keys = ('magic', 'steps', 'max_step_us', 'photo_priority_windows', 'urgent_windows',
                'metadata_budget_hits', 'metadata_iterations', 'next_arms', 'transitions',
                'low_pcm_windows', 'min_queued', 'reserved')
        return dict(zip(keys, words))

    def executive_workload(self) -> dict:
        words = self._diagnostic_words(14, 24, 'executive workload')
        if words[0] != int.from_bytes(b'EXSV', 'little') or words[1] != 5 or words[2] > 64:
            raise ProtocolError('invalid executive workload header')
        services = ('input', 'power', 'display', 'shell', 'link')
        return {'queued': words[2], 'queue_high': words[3],
                'services': {name: dict(accepted=words[4+i], processed=words[9+i],
                                        dropped=words[14+i], max_latency_us=words[19+i])
                             for i, name in enumerate(services)}}

    def settings_publications(self) -> dict:
        words = self._diagnostic_words(15, 8, 'settings publication')
        if words[0] != int.from_bytes(b'SETP', 'little') or words[6] & ~3 or words[7] & ~3:
            raise ProtocolError('invalid settings publication header')
        keys = ('magic', 'revision', 'record_revision', 'theme_revision',
                'record_publishes', 'theme_publishes', 'dirty_flags', 'loaded_flags')
        return dict(zip(keys, words))

    def storage_poll_timing(self) -> dict:
        """Read inclusive storage-function timing; no reset or I/O.

        Nested means reentry into the same storage function. Callback, FAT,
        progress and poll intervals overlap and must not be added. Deltas need
        identical boot/epoch, overflow_mask=0 at both endpoints, and paired
        idle/ATA-epoch checks. Bracket with host times and snapshot timestamps.
        """
        words = self._diagnostic_words(2, 16, 'storage poll timing')
        if words[0] != int.from_bytes(b'POLL', 'little') or words[1]:
            raise ProtocolError('invalid storage poll timing header')
        names = ('magic', 'reserved', 'epoch', 'overflow_mask', 'poll_calls', 'poll_us',
                 'poll_nested_calls', 'poll_nested_us', 'progress_calls', 'progress_us',
                 'progress_nested_calls', 'progress_nested_us', 'fat_calls', 'fat_us',
                 'runtime_callback_calls', 'runtime_callback_us')
        result = dict(zip(names, words))
        result['counters_usable'] = words[3] == 0
        return result

    def storage_schedule_diagnostics(self) -> dict:
        """Read scheduler opportunity/latency samples.

        State counts are samples at the existing runtime-idle decision. Offered
        wait windows can overlap decoder/render CPU and must not be summed as
        elapsed or predicted idle time. Completion age is DONE-to-take wall time.
        Compare only identical build/epoch endpoints with flags=0.
        """
        words = self._diagnostic_words(5, 24, 'storage schedule diagnostics')
        if (words[0] != int.from_bytes(b'SCHD', 'little') or words[1] or
                not words[2] or words[3] & ~0x80000003 or words[15] > 4 or
                (not (words[3] & 0x80000000) and sum(words[5:10]) != words[4]) or
                words[11] > words[10] or
                words[13] > words[12] or words[18] > words[17] or
                words[20] > words[19] or words[20] > words[21] or
                words[23] > words[22]):
            raise ProtocolError('invalid storage schedule diagnostics header')
        names = ('magic', 'reserved', 'epoch', 'flags', 'samples', 'idle_samples',
                 'fat_runnable_samples', 'completion_runnable_samples',
                 'block_runnable_samples', 'block_wait_samples', 'offered_wait_us',
                 'offered_wait_max_us', 'overdue_us', 'overdue_max_us',
                 'observation_gap_max_us', 'last_state', 'completion_taken',
                 'completion_ready_us', 'completion_ready_max_us', 'cancel_requests',
                 'cancel_terminals', 'cancel_polls', 'cancel_us', 'cancel_max_us')
        result = dict(zip(names, words))
        result['saturated'] = bool(words[3] & 0x80000000)
        result['counters_usable'] = words[3] == 0
        return result

    def runtime_render_timing(self) -> dict:
        """Read dirty-render/publish inclusive wall time, not all runtime.

        Animation-only calls excluded. Match identity/epoch and idle flags=0
        endpoints. Reentry overlaps totals; maxima are lifetime, not deltas.
        """
        w = self._diagnostic_words(4, 16, 'render timing')
        if (w[0] != int.from_bytes(b'RNDR', 'little') or w[1] or not w[2] or
                w[3] & ~0x8000001f or any(w[6:8]) or
                bool(w[3] & 0x80000000) != bool(w[5])):
            raise ProtocolError('invalid render timing header')
        if not w[3] and (w[8] != w[11] or w[14] > w[11] or w[15] > w[8]):
            raise ProtocolError('invalid render timing counts')
        names = ('magic', 'reserved', 'epoch', 'flags', 'snapshot_us', 'active_depth',
                 'reserved0', 'reserved1', 'render_calls', 'render_us', 'render_max_us',
                 'publish_calls', 'publish_us', 'publish_max_us', 'accepted', 'reentry_calls')
        result = dict(zip(names, w))
        result['counters_usable'] = w[3] == 0
        return result

    def ata_capabilities(self) -> dict:
        """Read cached IDENTIFY and passive controller/clock registers."""
        words = self._diagnostic_words(3, 16, 'ATA capabilities')
        # S5L8702 publishes version 1 with media capability bits, without
        # the PP502x IDENTIFY/timing snapshot. Do not interpret its flags
        # as PP502x snapshot-valid bits or fabricate missing register data.
        if (words[0] == int.from_bytes(b'ACAP', 'little') and words[1] == 1 and
                not words[2] and not words[3] & ~31 and not any(words[4:])):
            return dict(magic=words[0], version=1, format='s5l8702', capabilities=words[3],
                        capability_names=[name for bit, name in enumerate(
                            ('lba48', 'flush', 'power_management', 'apm', 'write_cache_enabled'))
                            if words[3] & (1 << bit)],
                        identify_available=False, timing_available=False)
        if (words[0] != int.from_bytes(b'ACAP', 'little') or words[1] or
                words[3] & ~63 or any(value > 65535 for value in words[4:8])):
            raise ProtocolError('invalid ATA capabilities header')
        names = ('magic', 'reserved', 'snapshot_us', 'flags', 'identify_53',
                 'identify_64', 'identify_67', 'identify_68', 'ide_config',
                 'ide_timing0', 'ide_timing1', 'device_enable', 'device_reset',
                 'cpu_clock_source', 'pll_control', 'ata_reset_epoch')
        result = dict(zip(names, words))
        result['identify_valid'] = bool(words[3] & 1)
        result['timing_valid'] = bool(words[3] & 32)
        return result

    def media_info(self) -> dict:
        reply=self.request(Command.MEDIA_INFO,timeout=15)
        if (len(reply)!=192 or reply[:4]!=b'MEDI' or struct.unpack_from('<I',reply,4)[0] or
            any(reply[132:])):
            raise ProtocolError('invalid media telemetry header/size/reserved bytes')
        sectors=struct.unpack_from('<Q',reply,8)[0]
        fields=('media_kind','rotation_rpm','logical_sector_bytes','physical_sector_bytes',
                'capabilities','quirks','power_state','idle_timeout_us','wake_timeout_us',
                'poweroffs','spinups','idlefailures')
        result=dict(zip(fields,struct.unpack_from('<12I',reply,16)))
        if result['capabilities']&~31 or result['power_state']>3 or result['media_kind']>2 or result['quirks']&~31:
            raise ProtocolError('invalid media capability/power flags')
        for key,offset,length in (('model',64,40),('serial',104,20),('firmware',124,8)):
            raw=reply[offset:offset+length]
            meaningful,separator,padding=raw.partition(b'\0')
            if (separator and any(padding)) or any(byte<32 or byte>126 for byte in meaningful):
                raise ProtocolError('invalid media ASCII identity field')
            result[key]=meaningful.decode('ascii').rstrip(' ')
        result.update(format='MEDI',sectors=sectors,
                      power_state_name=('off','active','standby','transition')[result['power_state']],
                      media_kind_name=('unknown','rotating','solid_state')[result['media_kind']],
                      capability_names=[name for bit,name in enumerate(('lba48','flush','power_management','apm','write_cache_enabled'))
                                        if result['capabilities']&(1<<bit)],
                      quirk_names=[name for bit,name in enumerate(('cfa_missing_lba_probe','optional_apm_rejected','cfa_identify_claim','iflash_platform','pata_physical_transfers'))
                                   if result['quirks']&(1<<bit)])
        return result

    def scratch_read(self, offset: int, length: int) -> bytes:
        if not 1 <= length <= 1008 or not 0 <= offset <= 0x400000-length:
            raise ValueError('scratch read outside allocated file')
        reply = self.request(Command.SCRATCH_READ, struct.pack('<II',offset,length), timeout=45)
        if len(reply) != length+4 or reply[:4] != struct.pack('<I',offset):
            raise ProtocolError('scratch read offset/length mismatch')
        return reply[4:]

    def _storage_ack(self, command, generation, payload=b'', end=0, timeout=45):
        reply = self.request(command, struct.pack('<I',generation)+payload, timeout=timeout)
        if reply != struct.pack('<II',generation,end):
            self.poisoned = True
            raise AmbiguousCommand('storage acknowledgement mismatch; inspect state before another mutation')

    def scratch_write(self, generation: int, offset: int, data: bytes):
        if not 1 <= len(data) <= 1008 or not 0 <= offset <= 0x400000-len(data):
            raise ValueError('scratch write outside allocated file')
        self._storage_ack(Command.SCRATCH_WRITE,generation,struct.pack('<I',offset)+data,offset+len(data))

    def settings_get(self, generation: int) -> bytes:
        reply = self.request(Command.SETTINGS_GET, timeout=45)
        if not 4 <= len(reply) <= 484 or reply[:4] != struct.pack('<I',generation):
            raise ProtocolError('settings generation/length mismatch')
        return reply[4:]

    def settings_put(self, generation: int, data: bytes):
        if len(data)>480:
            raise ValueError('settings payload exceeds480 bytes')
        self._storage_ack(Command.SETTINGS_PUT,generation,data,len(data))

    def prepare_storage(self, generation: int, timeout: float = 120):
        """Refresh the read-only FAT proof after media directory discovery.

        Repeat only acknowledged admission checks; an ambiguous reply aborts.
        No device data is written and no cached proof is bypassed.
        """
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.storage_state()
            if state[2] or state[8]:
                raise DeviceError('storage cannot establish a write proof while faulted')
            if state[4] and state[7]:return
            if state[1]==6:
                self._storage_ack(Command.STORAGE_PREPARE,generation)
            time.sleep(.05)
        raise TimeoutError('storage write proof did not become ready')

    def _persistent_update_ack(self, command, generation, payload, end, *,
                               running, confirmed_slot, expected_offset, timeout=45):
        """Re-admit only explicitly rejected, provably untouched update work."""
        if command not in (Command.UPDATE_CHUNK, Command.UPDATE_FINISH):
            raise ValueError('update admission is limited to CHUNK and FINISH')
        deadline = None
        def admission_timeout():
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise DeviceError('persistent update admission deadline expired')
            return remaining
        try:
            while True:
                if deadline is not None:
                    admission_timeout()
                try:
                    return self._storage_ack(command,generation,payload,end,timeout=timeout)
                except DeviceError as rejection:
                    # Generic rejection remains poisoned. This private path can
                    # inspect only a matched single ERROR at a clean boundary.
                    if (rejection.acknowledged_command != command or
                            self.pending or self.parser.buffer):
                        raise
                    if deadline is None:
                        deadline = time.monotonic()+15
                    time.sleep(min(.05,max(0,deadline-time.monotonic())))
                    while True:
                        if time.monotonic() >= deadline:
                            raise DeviceError('persistent update admission deadline expired') from rejection
                        self.poisoned = False
                        from .identity import decode
                        observed = decode(self.status(timeout=admission_timeout()))
                        if any(observed[key] != running[key] for key in
                               ('target_id','build_id','image_sha256','generation')):
                            raise DeviceError('persistent update identity changed') from rejection
                        update = self.update_status(timeout=admission_timeout())
                        if (update[2] != 1 or update[11] != 1 or
                                update[12] != (confirmed_slot^1) or update[13] != expected_offset or
                                update[5] != confirmed_slot or update[6] != 255 or
                                update[7] or update[14] or update[15]):
                            raise DeviceError('rejected update cannot prove untouched work') from rejection
                        storage = self.storage_state(timeout=admission_timeout())
                        if storage[2] or storage[8]:
                            raise DeviceError('persistent update storage fault') from rejection
                        if storage[4] == 1 and storage[7] == 1 and not storage[9] and update[1] == 1:
                            break
                        if storage[1] == 6 and not storage[9] and not storage[4]:
                            # Refresh only the read-only FAT ownership proof.
                            self._storage_ack(Command.STORAGE_PREPARE,generation,timeout=admission_timeout())
                        time.sleep(.05)
                    # The pending hint is not the complete firmware busy gate.
                    # A race can reject again; the next loop must re-prove it.
        except BaseException:
            self.poisoned = True
            raise

    def persistent_install(self, generation: int, image: bytes, publication_stage: int | None = None,
                           *, image_path=None) -> dict:
        from tools.inspect.image import inspect
        from tools.studio.targets import TARGETS
        self._verify_image_source(image, image_path)
        identity=inspect(image)
        from .identity import decode
        running=decode(self.status())
        if running['generation'] != generation:
            raise DeviceError('persistent update generation changed before admission')
        if publication_stage is not None and publication_stage not in range(1,5):
            raise ValueError('publication stage must be1..4')
        if len(image)>TARGETS[identity['target_id']]['os_capacity_bytes']:
            raise ValueError('persistent image exceeds target OS capacity')
        self.prepare_storage(generation)
        storage=self.storage_state()
        update=self.update_status()
        if (storage[4]!=1 or update[1]!=1 or update[2]!=1 or update[7] or
                update[11] or update[14] or update[15]):
            raise DeviceError('persistent update preflight requires prepared storage and a clean idle updater')
        digest=hashlib.sha256(image).digest()
        self._storage_ack(Command.UPDATE_BEGIN,generation,struct.pack('<I',len(image))+digest)
        for offset in range(0,len(image),1008):
            chunk=image[offset:offset+1008]
            self._persistent_update_ack(Command.UPDATE_CHUNK,generation,struct.pack('<I',offset)+chunk,
                                        offset+len(chunk),running=running,confirmed_slot=update[5],
                                        expected_offset=offset)
        if publication_stage is not None:
            self.publication_fault(generation,publication_stage)
        try:
            # A publication cut deliberately resets before acknowledging. Keep
            # its observation bounded; ordinary image validation retains 10 min.
            self._persistent_update_ack(Command.UPDATE_FINISH,generation,b'',0,running=running,
                                        confirmed_slot=update[5],expected_offset=len(image),
                                        timeout=60 if publication_stage is not None else 600)
        except AmbiguousCommand as exc:
            raise AmbiguousUpdateFinish(str(exc)) from exc
        return dict(candidate_published=True,image=identity,sha256=digest.hex(),bytes=len(image))

    def update_status(self, *, timeout=15) -> tuple[int, ...]:
        reply = self.request(Command.UPDATE_STATUS, timeout=timeout)
        if len(reply) != 64 or reply[:4] != b'USTA':
            raise ProtocolError('invalid persistent update status')
        return struct.unpack('<16I',reply)

    def storage_fault(self, generation: int, mode: int):
        if mode not in range(1,6):
            raise ValueError('storage fault mode must be1..5')
        self._storage_ack(Command.STORAGE_FAULT,generation,struct.pack('<I',mode),mode)

    def storage_recover(self, generation: int):
        self._storage_ack(Command.STORAGE_RECOVER,generation)

    def file_resize(self, generation: int, action: int, size: int):
        if action not in (1,2) or not 1<=size<=0x100000:
            raise ValueError('WORK resize requires create1/grow2 and size1..1MiB')
        self._storage_ack(Command.FILE_RESIZE,generation,struct.pack('<II',action,size),size,timeout=120)

    def work_read(self, offset: int, length: int) -> bytes:
        if not 1<=length<=1008 or not 0<=offset<=0x100000-length:
            raise ValueError('WORK read outside scratch-work bounds')
        reply=self.request(Command.WORK_READ,struct.pack('<II',offset,length),timeout=45)
        if len(reply)!=length+4 or reply[:4]!=struct.pack('<I',offset):
            raise ProtocolError('WORK read offset/length mismatch')
        return reply[4:]

    def work_write(self, generation: int, offset: int, data: bytes):
        if not 1<=len(data)<=1008 or not 0<=offset<=0x100000-len(data):
            raise ValueError('WORK write outside scratch-work bounds')
        self._storage_ack(Command.WORK_WRITE,generation,struct.pack('<I',offset)+data,offset+len(data))

    def reboot_persistent(self, generation: int):
        self._storage_ack(Command.REBOOT_PERSISTENT,generation)

    def publication_fault(self, generation: int, stage: int):
        if stage not in range(1,5):
            raise ValueError('publication stage must be1..4')
        self._storage_ack(Command.PUBLICATION_FAULT,generation,struct.pack('<I',stage),stage)

    def close(self) -> None:
        remaining = self.handoff_until - time.monotonic()
        if remaining > 0:
            time.sleep(min(remaining, 1.0))
        self.transport.close()
