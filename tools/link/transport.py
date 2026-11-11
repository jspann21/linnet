"""Optional serial adapter. No USB identity or physical driver is invented."""
from typing import Protocol
import time


class Transport(Protocol):
    def read(self, size: int, timeout: float) -> bytes: ...
    def write(self, data: bytes, timeout: float) -> None: ...
    def close(self) -> None: ...


class SerialTransport:
    OPEN_SETTLE_SECONDS = 1.0
    DTR_RESET_SECONDS = 0.5

    @staticmethod
    def discover() -> list[dict]:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise RuntimeError("Serial discovery requires optional dependency pyserial==3.5") from exc
        return [dict(port=p.device, description=p.description, serial_number=p.serial_number,
                     vid=p.vid, pid=p.pid, location=p.location)
                for p in sorted(list_ports.comports(), key=lambda p: p.device)]

    @classmethod
    def reconnect(cls, serial_number: str, timeout: float = 10.0):
        if not serial_number:
            raise ValueError("reconnect requires a stable USB serial identity")
        deadline = time.monotonic() + timeout
        last_open_error = None
        while time.monotonic() < deadline:
            matches = [p for p in cls.discover() if p["serial_number"] == serial_number]
            if len(matches) > 1:
                raise RuntimeError("USB serial identity is ambiguous")
            if matches:
                try:
                    return cls(matches[0]["port"])
                except OSError as exc:
                    # Enumeration can precede the COM driver's readiness.
                    # Retry opening only; never send or repeat a command here.
                    last_open_error = exc
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
        raise TimeoutError("device did not become available before reconnect deadline") from last_open_error

    def __init__(self, port: str, baudrate: int = 115200):
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("Serial transport requires optional dependency pyserial==3.5") from exc
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0, write_timeout=2)
        try:
            # Explicitly establish a fresh sequence space before any command.
            # These are connection setup operations, never transaction retries.
            self.serial.dtr = False
            time.sleep(self.DTR_RESET_SECONDS)
            self.serial.reset_input_buffer()
            self.serial.dtr = True
            time.sleep(self.OPEN_SETTLE_SECONDS)
        except BaseException:
            self.serial.close()
            raise

    def read(self, size: int, timeout: float) -> bytes:
        if size <= 0:
            return b""
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            available = min(size, self.serial.in_waiting)
            if available:
                return self.serial.read(available)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return b""
            time.sleep(min(0.001, remaining))

    def write(self, data: bytes, timeout: float) -> None:
        # PySerial timeout setters reconfigure the Windows DCB and emit CDC
        # control traffic. Keep both timeouts immutable after opening. The
        # serial write itself has a fixed two-second ceiling; enforce the
        # caller's transaction deadline again before accepting completion.
        if timeout <= 0:
            raise TimeoutError("serial write deadline expired")
        deadline = time.monotonic() + timeout
        if self.serial.write(data) != len(data):
            raise OSError("short serial write")
        if time.monotonic() > deadline:
            raise TimeoutError("serial write exceeded transaction deadline")

    def close(self) -> None:
        self.serial.close()
