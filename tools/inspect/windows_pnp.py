"""Resolve a disk device's USB ancestor through the Windows device tree."""
from functools import lru_cache
import ctypes
from ctypes import wintypes
import re


@lru_cache(maxsize=1)
def _api():
    cfg = ctypes.WinDLL("cfgmgr32", use_last_error=True)
    locate = cfg.CM_Locate_DevNodeW
    locate.argtypes = [ctypes.POINTER(wintypes.ULONG), wintypes.LPWSTR, wintypes.ULONG]
    locate.restype = wintypes.ULONG
    parent = cfg.CM_Get_Parent
    parent.argtypes = [ctypes.POINTER(wintypes.ULONG), wintypes.ULONG, wintypes.ULONG]
    parent.restype = wintypes.ULONG
    size = cfg.CM_Get_Device_ID_Size
    size.argtypes = [ctypes.POINTER(wintypes.ULONG), wintypes.ULONG, wintypes.ULONG]
    size.restype = wintypes.ULONG
    device_id = cfg.CM_Get_Device_IDW
    device_id.argtypes = [wintypes.ULONG, wintypes.LPWSTR, wintypes.ULONG, wintypes.ULONG]
    device_id.restype = wintypes.ULONG
    return locate, parent, size, device_id


def present_devices(device_filter: str, *, setup_class=False) -> list[str]:
    """List present devices by setup class GUID or by enumerator name."""
    cfg = ctypes.WinDLL("cfgmgr32", use_last_error=True)
    get_size = cfg.CM_Get_Device_ID_List_SizeW
    get_size.argtypes = [ctypes.POINTER(wintypes.ULONG), wintypes.LPCWSTR, wintypes.ULONG]
    get_size.restype = wintypes.ULONG
    get_list = cfg.CM_Get_Device_ID_ListW
    get_list.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.ULONG, wintypes.ULONG]
    get_list.restype = wintypes.ULONG
    # CM_GETIDLIST_FILTER_PRESENT plus CLASS or ENUMERATOR.
    flags = 0x100 | (0x200 if setup_class else 0x1)
    for _ in range(2):
        length = wintypes.ULONG()
        if get_size(ctypes.byref(length), device_filter, flags) or not length.value:
            break
        buffer = ctypes.create_unicode_buffer(length.value)
        result = get_list(device_filter, buffer, length.value, flags)
        if result == 0:
            return buffer[:].rstrip("\0").split("\0") if buffer[0] else []
        if result != 0x1A:  # CR_BUFFER_SMALL, a device may have arrived mid-query.
            break
    raise RuntimeError("Could not list present Windows devices")


def usb_parent(instance_id: str) -> str:
    """Return the first USB VID/PID ancestor, following at most five parent links."""
    if not instance_id:
        raise RuntimeError("Disk PnP identity is unavailable")
    locate, parent, size, device_id = _api()
    node = wintypes.ULONG()
    if locate(ctypes.byref(node), instance_id, 0):
        raise RuntimeError("Disk PnP device is unavailable")
    for _ in range(5):
        ancestor = wintypes.ULONG()
        if parent(ctypes.byref(ancestor), node, 0):
            break
        length = wintypes.ULONG()
        if size(ctypes.byref(length), ancestor, 0) or not 0 < length.value < 1024:
            break
        name = ctypes.create_unicode_buffer(length.value + 1)
        if device_id(ancestor, name, len(name), 0):
            break
        if re.match(r"^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}\\", name.value, re.I):
            return name.value
        node = ancestor
    raise RuntimeError("Could not resolve the disk USB identity")
