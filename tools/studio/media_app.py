"""Platform and dependency checks for the native Media Manager."""
import os
from contextlib import contextmanager


class SetupInProgressError(RuntimeError):
    pass


@contextmanager
def _installer_guard():
    """Serialize packaged process startup against setup's file changes."""
    if os.name != 'nt':
        yield
        return
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.OpenMutexW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.OpenMutexW.restype = wintypes.HANDLE
    handle = kernel.CreateMutexW(None, False, 'Local\\Linnet.MediaManager.Running')
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        # Publish Running before checking Changing. Setup does the reverse,
        # so simultaneous starts cannot both admit work.
        changing = kernel.OpenMutexW(0x00100000, False, 'Local\\Linnet.MediaManager.Changing')
        if changing:
            kernel.CloseHandle(changing)
            raise SetupInProgressError('Finish installing or uninstalling Linnet, then open it again.')
        if ctypes.get_last_error() != 2:  # ERROR_FILE_NOT_FOUND
            raise ctypes.WinError(ctypes.get_last_error())
        yield
    finally:
        kernel.CloseHandle(handle)


def launch():
    if os.name != "nt":
        raise RuntimeError("The desktop media manager currently supports Windows Disk Mode.")
    try:
        from .media_desktop import launch as launch_desktop
    except ModuleNotFoundError as error:
        missing = error.name or ''
        if missing in ('PySide6', 'shiboken6') or missing.startswith(('PySide6.', 'shiboken6.')):
            raise RuntimeError('Install the Media Manager with: python -m pip install -r tools/studio/requirements.txt') from error
        raise
    launch_desktop()
