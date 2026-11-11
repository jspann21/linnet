"""Shared desktop identity and icon for Media Manager and its shortcuts."""
import os

from tools.studio.targets import ROOT

APP_USER_MODEL_ID = 'Linnet.MediaManager'
ICON_PATH = ROOT / 'assets/linnet.ico'


def activate_window(window):
    """Request foreground once for the window the user has just launched."""
    window.raise_()
    window.activateWindow()
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        user = ctypes.WinDLL('user32')
        user.SetForegroundWindow.argtypes = (wintypes.HWND,)
        user.SetForegroundWindow.restype = wintypes.BOOL
        user.SetForegroundWindow(int(window.winId()))


def configure_taskbar():
    """Set the shortcut identity before Qt creates any windows."""
    if os.name == 'nt':
        import ctypes
        shell = ctypes.WinDLL('shell32')
        shell.SetCurrentProcessExplicitAppUserModelID.argtypes = (ctypes.c_wchar_p,)
        shell.SetCurrentProcessExplicitAppUserModelID.restype = ctypes.c_long
        result = shell.SetCurrentProcessExplicitAppUserModelID(APP_USER_MODEL_ID)
        if result < 0:
            raise OSError(f'Could not set Linnet taskbar identity ({result:#x}).')
