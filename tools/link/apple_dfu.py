"""Apple Windows DFU transport, using the installed Apple driver (no replacement).

The driver exposes USB setup packets through PIPE0. All transfers are bounded
overlapped I/O, and devices must be selected by an enumerated interface path.
"""
import ctypes as c
from ctypes import wintypes as w
import os
import re
import struct
import time
import uuid


class GUID(c.Structure):
    _fields_ = [('data', c.c_ubyte * 16)]


class Interface(c.Structure):
    _fields_ = [('size', w.DWORD), ('guid', GUID), ('flags', w.DWORD), ('reserved', c.c_size_t)]


class Overlapped(c.Structure):
    _fields_ = [('internal', c.c_size_t), ('internal_high', c.c_size_t),
                ('offset', w.DWORD), ('offset_high', w.DWORD), ('event', w.HANDLE)]


def interfaces():
    if os.name != 'nt':
        raise RuntimeError('Apple DFU transport requires Windows')
    api = c.WinDLL('setupapi', use_last_error=True)
    guid = GUID((c.c_ubyte * 16).from_buffer_copy(uuid.UUID('b8085869-feb9-404b-8cb1-1e5c14fa8c54').bytes_le))
    api.SetupDiGetClassDevsW.argtypes = [c.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
    api.SetupDiGetClassDevsW.restype = w.HANDLE
    api.SetupDiEnumDeviceInterfaces.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(GUID), w.DWORD, c.POINTER(Interface)]
    api.SetupDiGetDeviceInterfaceDetailW.argtypes = [w.HANDLE, c.POINTER(Interface), c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
    api.SetupDiDestroyDeviceInfoList.argtypes = [w.HANDLE]
    handle = api.SetupDiGetClassDevsW(c.byref(guid), None, None, 0x12)
    if handle == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    result = []
    try:
        for index in range(64):
            item = Interface(); item.size = c.sizeof(item)
            if not api.SetupDiEnumDeviceInterfaces(handle, None, c.byref(guid), index, c.byref(item)):
                if c.get_last_error() == 259:
                    return result
                raise c.WinError(c.get_last_error())
            size = w.DWORD()
            api.SetupDiGetDeviceInterfaceDetailW(handle, c.byref(item), None, 0, c.byref(size), None)
            if not 8 <= size.value <= 65536:
                raise RuntimeError('Invalid DFU interface path length')
            data = c.create_string_buffer(size.value)
            c.cast(data, c.POINTER(w.DWORD))[0] = 8 if c.sizeof(c.c_void_p) == 8 else 6
            if not api.SetupDiGetDeviceInterfaceDetailW(handle, c.byref(item), data, size.value, None, None):
                raise c.WinError(c.get_last_error())
            path = c.wstring_at(c.addressof(data) + 4)
            match = re.search(r'vid_05ac&pid_([0-9a-f]{4})', path, re.I)
            if match and int(match[1], 16) in (0x1223, 0x1250):
                result.append(dict(path=path, vid=0x05ac, pid=int(match[1], 16)))
        raise RuntimeError('DFU interface enumeration exceeded its bound')
    finally:
        api.SetupDiDestroyDeviceInfoList(handle)


class AppleDFU:
    def __init__(self, path):
        if path not in [item['path'] for item in interfaces()]:
            raise ValueError('Select a currently enumerated Apple DFU interface')
        self.api = k = c.WinDLL('kernel32', use_last_error=True)
        k.CreateFileW.argtypes = [w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE]
        k.CreateFileW.restype = w.HANDLE
        k.CreateEventW.argtypes = [c.c_void_p,w.BOOL,w.BOOL,w.LPCWSTR]; k.CreateEventW.restype = w.HANDLE
        for name in ('ReadFile', 'WriteFile'):
            getattr(k,name).argtypes = [w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.POINTER(Overlapped)]
        k.GetOverlappedResult.argtypes = [w.HANDLE,c.POINTER(Overlapped),c.POINTER(w.DWORD),w.BOOL]
        k.CancelIoEx.argtypes = [w.HANDLE,c.POINTER(Overlapped)]
        k.WaitForSingleObject.argtypes = [w.HANDLE,w.DWORD]
        k.CloseHandle.argtypes = [w.HANDLE]
        self.handle = k.CreateFileW(path + '\\PIPE0', 0xc0000000, 3, None, 3, 0x40000000, None)
        if self.handle == c.c_void_p(-1).value:
            raise c.WinError(c.get_last_error())
        self.poisoned = False

    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle); self.handle = None

    def __enter__(self): return self
    def __exit__(self, *_): self.close()

    def control(self, request_type, request, value=0, index=0, data=b'', length=None, timeout_ms=3000):
        if self.poisoned or self.handle is None:
            raise RuntimeError('DFU transport closed or uncertain; reconnect before continuing')
        incoming = bool(request_type & 0x80)
        size = length if incoming else len(data)
        if type(size) is not int or not 0 <= size <= 2048 or not 1 <= timeout_ms <= 10000:
            raise ValueError('DFU control transfer exceeds its bound')
        packet = struct.pack('<BBHHH', request_type, request, value, index, size)
        buffer = c.create_string_buffer(packet + (bytes(size) if incoming else data), 8+size)
        operation = Overlapped(); operation.event = self.api.CreateEventW(None, True, False, None)
        if not operation.event: raise c.WinError(c.get_last_error())
        count = w.DWORD()
        try:
            method = self.api.ReadFile if incoming else self.api.WriteFile
            if not method(self.handle, buffer, len(buffer), c.byref(count), c.byref(operation)):
                if c.get_last_error() != 997: raise c.WinError(c.get_last_error())
                if self.api.WaitForSingleObject(operation.event, timeout_ms) != 0:
                    self.api.CancelIoEx(self.handle, c.byref(operation))
                    # Cancellation must retire before the native buffer is released.
                    self.api.GetOverlappedResult(self.handle, c.byref(operation), c.byref(count), True)
                    self.poisoned = True
                    raise TimeoutError('DFU transfer timed out; acceptance is unknown')
                if not self.api.GetOverlappedResult(self.handle, c.byref(operation), c.byref(count), False):
                    raise c.WinError(c.get_last_error())
            if count.value != 8+size:
                raise OSError(f'Short Apple DFU transfer: {count.value}/{8+size}')
            return bytes(buffer[8:]) if incoming else b''
        except OSError:
            self.poisoned = True
            raise
        finally:
            self.api.CloseHandle(operation.event)

    def state(self): return self.control(0xa1,5,length=1)[0]

    def clear(self):
        self.control(0x21,4)
        if self.state() != 2: raise RuntimeError('BootROM did not return to DFU idle')

    def status(self):
        data = self.control(0xa1,3,length=6)
        if data[0]: raise RuntimeError(f'DFU device reported error {data[0]}')
        return data[4], int.from_bytes(data[1:4],'little')

    def wait_state(self, wanted, seconds=5):
        deadline = time.monotonic()+seconds
        while time.monotonic()<deadline:
            state, delay = self.status()
            if state in wanted: return state
            if state not in (3,4,6,7): raise RuntimeError(f'Unexpected DFU state {state}')
            time.sleep(min(max(delay/1000, .001), .1))
        raise TimeoutError('DFU state transition timed out')
