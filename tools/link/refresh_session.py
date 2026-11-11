"""Finite, plan-bound Windows directory-refresh session; ordinary UAC bootstrap.

The approved bootstrap copies hash-pinned source/data into an administrator-only
directory before starting the server. The pipe accepts no paths or payloads.
"""
import argparse
import base64
import ctypes
from ctypes import wintypes as w
from datetime import datetime, timezone
import hashlib
import json
import locale  # noqa: F401 - preload subprocess text decoding before elevated readiness
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
DATA = (Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) /
        'Linnet' / 'Refresh')
STATE = DATA / 'state.json'
# Discover data files without importing workspace code in the isolated bootstrap.
# Keep the entry script first: prepare uses SOURCES[0] for the UAC command.
def _source_closure():
    import tomllib
    paths = {'tools/studio/native_bootstrap.py', 'tools/inspect/image.py', 'tools/studio/targets.py'}
    for manifest in (ROOT/'targets').glob('*/target.toml'):
        spec = tomllib.loads(manifest.read_text(encoding='utf-8'))
        paths.add(manifest.relative_to(ROOT).as_posix())
        if spec.get('presentation_profile'):
            paths.add('ui/profiles/' + spec['presentation_profile'] + '/profile.toml')
        if not spec.get('build_enabled', True):
            continue
        board = manifest.parent
        hal = ROOT/'hal'/spec.get('hal_family',spec['soc'])
        display = board/'display.c'
        if not display.is_file(): display = hal/'display.c'
        device = board/'device.c'
        if not device.is_file(): device = board/'board.c'
        power = board/'power.c'
        if not power.is_file(): power = hal/'power.c'
        video = {'ipvf': ROOT/'services/video/ipvf/player.c',
                 'native-mp4': ROOT/'services/video/native/player.c',
                 'bcm-mp4': board/'video.c', 'none': None}[spec['media_video_backend']]
        if video is not None: paths.add(video.relative_to(ROOT).as_posix())
        for optional in (board/'coprocessor.c', board/'display.S'):
            if optional.is_file(): paths.add(optional.relative_to(ROOT).as_posix())
        for path in (manifest,hal/'startup.S',hal/'memory.ld',hal/'platform.c',
                     board/'lcd.c',device,power,board/'hardware.h',display,
                     ROOT/'hal'/'codecs'/(spec.get('audio_codec','wm8975')+'.c')):
            paths.add(path.resolve().relative_to(ROOT.resolve()).as_posix())
    return ('tools/link/refresh_session.py', *sorted(paths))
SOURCES = _source_closure()
MAX_MESSAGE = 65536


def _json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def _pack(value):
    if isinstance(value, bytes):
        return {'$bytes': base64.b64encode(value).decode('ascii')}
    if isinstance(value, (list, tuple)):
        return [_pack(item) for item in value]
    if isinstance(value, dict):
        return {key: _pack(item) for key, item in value.items()}
    return value


def _unpack(value):
    if isinstance(value, dict):
        if set(value) == {'$bytes'}:
            return base64.b64decode(value['$bytes'], validate=True)
        return {key: _unpack(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_unpack(item) for item in value]
    return value


class Windows:
    def __init__(self):
        if os.name != 'nt':
            raise RuntimeError('refresh sessions require Windows')
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.a = ctypes.WinDLL('advapi32', use_last_error=True)
        signatures = {
            'CloseHandle': ([w.HANDLE], w.BOOL),
            'LocalFree': ([ctypes.c_void_p], ctypes.c_void_p),
            'GetCurrentProcess': ([], w.HANDLE),
            'GetWindowsDirectoryW': ([w.LPWSTR, w.UINT], w.UINT),
            'CreateDirectoryW': ([w.LPCWSTR, ctypes.c_void_p], w.BOOL),
            'CreateNamedPipeW': ([w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, ctypes.c_void_p], w.HANDLE),
            'ConnectNamedPipe': ([w.HANDLE, ctypes.c_void_p], w.BOOL),
            'DisconnectNamedPipe': ([w.HANDLE], w.BOOL),
            'CreateFileW': ([w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE], w.HANDLE),
            'ReadFile': ([w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p], w.BOOL),
            'WriteFile': ([w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p], w.BOOL),
            'PeekNamedPipe': ([w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.c_void_p, ctypes.POINTER(w.DWORD), ctypes.c_void_p], w.BOOL),
            'GetNamedPipeServerProcessId': ([w.HANDLE, ctypes.POINTER(w.ULONG)], w.BOOL),
            'OpenProcess': ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.k, name)
            fn.argtypes, fn.restype = args, result
        self.a.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
        self.a.OpenProcessToken.restype = w.BOOL
        self.a.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)]
        self.a.GetTokenInformation.restype = w.BOOL
        self.a.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(w.LPWSTR)]
        self.a.ConvertSidToStringSidW.restype = w.BOOL
        self.a.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [w.LPCWSTR, w.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
        self.a.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = w.BOOL

    def check(self, value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    def token(self, process=None):
        token = w.HANDLE()
        self.check(self.a.OpenProcessToken(process or self.k.GetCurrentProcess(), 8, ctypes.byref(token)))
        try:
            size = w.DWORD()
            self.a.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
            data = ctypes.create_string_buffer(size.value)
            self.check(self.a.GetTokenInformation(token, 1, data, size, ctypes.byref(size)))
            sid = w.LPWSTR()
            self.check(self.a.ConvertSidToStringSidW(ctypes.c_void_p.from_buffer(data), ctypes.byref(sid)))
            try:
                text = sid.value
            finally:
                self.k.LocalFree(sid)
            elevated = w.DWORD()
            self.check(self.a.GetTokenInformation(token, 20, ctypes.byref(elevated), 4, ctypes.byref(size)))
            return text, bool(elevated.value)
        finally:
            self.k.CloseHandle(token)

    def security(self, sid, *, directory=False):
        if not re.fullmatch(r'S-1-5-(?:\d+-)*\d+', sid):
            raise ValueError('invalid owner SID')
        flags = 'OICI' if directory else ''
        access = 'GR' if directory else 'GRGW'
        sddl = f'O:BAG:BAD:P(A;{flags};FA;;;SY)(A;{flags};FA;;;BA)(A;{flags};{access};;;{sid})'
        descriptor = ctypes.c_void_p()
        self.check(self.a.ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, ctypes.byref(descriptor), None))
        class Attributes(ctypes.Structure):
            _fields_ = [('size', w.DWORD), ('descriptor', ctypes.c_void_p), ('inherit', w.BOOL)]
        return Attributes(ctypes.sizeof(Attributes), descriptor, False), descriptor

    def read(self, handle, timeout):
        deadline = time.monotonic() + timeout
        available = w.DWORD()
        while time.monotonic() < deadline:
            self.check(self.k.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None))
            if available.value:
                if available.value > MAX_MESSAGE:
                    raise ValueError('oversize pipe message')
                data, count = ctypes.create_string_buffer(MAX_MESSAGE), w.DWORD()
                self.check(self.k.ReadFile(handle, data, MAX_MESSAGE, ctypes.byref(count), None))
                return json.loads(data.raw[:count.value])
            time.sleep(.025)
        raise TimeoutError('session response timeout; do not replay refresh')

    def write(self, handle, value):
        data = json.dumps(value, separators=(',', ':')).encode('utf-8')
        if len(data) > MAX_MESSAGE:
            raise ValueError('oversize pipe message')
        count = w.DWORD()
        self.check(self.k.WriteFile(handle, data, len(data), ctypes.byref(count), None))
        if count.value != len(data):
            raise OSError('incomplete pipe response')


def _usb_nodes():
    # Fixed query: client requests cannot supply PowerShell or instance paths.
    script = r"""$ErrorActionPreference='Stop'; @(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'USB\VID_*' -and $_.InstanceId -notlike '*&MI_*'} | ForEach-Object { $p=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_LocationPaths -ErrorAction SilentlyContinue; $parent=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue; $address=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Address -ErrorAction SilentlyContinue; [pscustomobject]@{id=$_.InstanceId; locations=@($p.Data); status=$_.Status; parent=$parent.Data; port=$address.Data} }) | ConvertTo-Json -Compress"""
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                            capture_output=True, text=True, timeout=30, check=True,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    data = json.loads(result.stdout or '[]')
    return data if isinstance(data, list) else [data]


def _usb_pin(serial, recovery_pin=None):
    if not isinstance(serial, str) or not re.fullmatch('[0-9A-Fa-f]{16}', serial):
        raise ValueError('USB serial must contain 16 hexadecimal digits')
    nodes = _usb_nodes()
    matches = [n for n in nodes if n['id'].upper().endswith('\\' + serial.upper())
               and n['id'].upper().startswith(('USB\\VID_1209&PID_504A', 'USB\\VID_05AC&PID_1209'))]
    if not matches and recovery_pin and recovery_pin['serial'] == serial.upper():
        matches = [n for n in nodes if n['id'].upper().startswith('USB\\VID_0000&PID_0002\\')
                   and n['status'] == 'Error' and set(n['locations']) & set(recovery_pin['locations'])]
    if len(matches) != 1 or not matches[0]['locations']:
        raise RuntimeError('expected one present iPod with a physical location')
    node = matches[0]
    if type(node['port']) is not int or not 1 <= node['port'] <= 255 or not isinstance(node['parent'], str) or not node['parent'].upper().startswith('USB\\'):
        raise RuntimeError('invalid USB parent or port')
    locations = [p for p in node['locations'] if isinstance(p, str) and p.startswith('ACPI(')] or node['locations']
    return dict(serial=serial.upper(), locations=locations, parent=node['parent'], port=node['port'], port_cycle=True)


def _usb_cycle_port(pin):
    # Resolve the current hub interface for the approved parent. Only its
    # approved connection index is cycled; no hub-wide reset is issued.
    cm = ctypes.WinDLL('cfgmgr32')
    guid = ctypes.create_string_buffer(uuid.UUID('f18a0e88-c30c-11d0-8815-00a0c906bed8').bytes_le)
    cm.CM_Get_Device_Interface_List_SizeW.argtypes = [ctypes.POINTER(w.ULONG), ctypes.c_void_p, w.LPCWSTR, w.ULONG]
    cm.CM_Get_Device_Interface_ListW.argtypes = [ctypes.c_void_p, w.LPCWSTR, w.LPWSTR, w.ULONG, w.ULONG]
    size = w.ULONG()
    if cm.CM_Get_Device_Interface_List_SizeW(ctypes.byref(size), guid, pin['parent'], 0):
        raise RuntimeError('cannot resolve approved USB hub')
    buf = ctypes.create_unicode_buffer(size.value)
    if cm.CM_Get_Device_Interface_ListW(guid, pin['parent'], buf, size, 0):
        raise RuntimeError('cannot resolve approved USB hub interface')
    paths = [x for x in buf[:size.value].split('\0') if x]
    if len(paths) != 1:
        raise RuntimeError('approved USB hub interface is ambiguous')
    api = Windows()
    handle = api.k.CreateFileW(paths[0], 0x40000000, 3, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        api.k.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD,
                                         ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
        params = (w.ULONG * 2)(pin['port'], 0)
        count = w.DWORD()
        api.check(api.k.DeviceIoControl(handle, 0x220444, params, 8, params, 8, ctypes.byref(count), None))
        if params[1]:
            raise RuntimeError('USB port cycle returned status ' + hex(params[1]))
        return dict(passed=True, parent=pin['parent'], port=pin['port'], status=params[1])
    finally:
        api.k.CloseHandle(handle)


class UsbDiagnostics:
    def __init__(self, config, directory):
        self.pin = config.get('usb_diagnostics')
        self.directory = directory
        self.name = 'LinnetUSB-' + config['session_id']
        self.trace = None
        self.deadline = 0

    def run(self, command):
        if not self.pin:
            raise ValueError('USB diagnostics were not approved for this session')
        if command in ('usb-restart', 'usb-cycle'):
            matches = [n for n in _usb_nodes() if set(n['locations']) & set(self.pin['locations'])]
            if len(matches) != 1:
                raise RuntimeError('pinned USB port does not contain exactly one device')
            node = matches[0]
            identity = node['id'].upper()
            known = identity.endswith('\\' + self.pin['serial']) and identity.startswith(
                ('USB\\VID_1209&PID_504A', 'USB\\VID_05AC&PID_1209'))
            failed = identity.startswith('USB\\VID_0000&PID_0002\\') and node['status'] == 'Error'
            if not (known or failed):
                raise RuntimeError('pinned port contains an unexpected device')
            if command == 'usb-cycle':
                if not self.pin.get('port_cycle') or (node['parent'], node['port']) != (self.pin['parent'], self.pin['port']):
                    raise RuntimeError('USB parent or port differs from approval')
                return _usb_cycle_port(self.pin)
            result = subprocess.run(['pnputil.exe', '/restart-device', node['id']],
                                    capture_output=True, text=True, timeout=40,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            return dict(passed=result.returncode == 0, device=node, output=result.stdout,
                        error=result.stderr, returncode=result.returncode)
        if command == 'trace-start':
            if self.trace:
                return dict(passed=True, trace=str(self.trace), already_running=True)
            providers = self.directory / 'usb-providers.txt'
            providers.write_text(''.join(name + ' 0x8065f 5\n' for name in (
                'Microsoft-Windows-USB-USBHUB3', 'Microsoft-Windows-USB-USBXHCI',
                'Microsoft-Windows-USB-UCX')), encoding='ascii')
            path = self.directory / ('usb-' + uuid.uuid4().hex + '.etl')
            subprocess.run(['logman.exe', 'create', 'trace', self.name, '-o', str(path),
                            '-pf', str(providers), '-f', 'bincirc', '-max', '128', '-ets'],
                           capture_output=True, text=True, check=True, timeout=20,
                           creationflags=subprocess.CREATE_NO_WINDOW)
            self.trace, self.deadline = path, time.monotonic() + 900
            return dict(passed=True, trace=str(path), maximum_seconds=900)
        if command == 'trace-stop':
            path = self.trace
            if path:
                subprocess.run(['logman.exe', 'stop', self.name, '-ets'], capture_output=True,
                               text=True, check=True, timeout=20,
                               creationflags=subprocess.CREATE_NO_WINDOW)
                self.trace = None
            return dict(passed=True, trace=str(path) if path else None)
        raise ValueError('unsupported USB diagnostic command')

    def tick(self):
        if self.trace and time.monotonic() >= self.deadline:
            self.run('trace-stop')


def prepare(plan_path, plan_sha256, *, hours=8, install_reviewed_boot=False, usb_serial=None):
    """Read-only device preparation; creates local immutable-content snapshot."""
    from tools.studio.native_bootstrap import load_plan
    if type(hours) is not int or not 1 <= hours <= 24:
        raise ValueError('session duration must be 1..24 hours')
    plan_path = Path(plan_path).resolve()
    manifest, artifacts, original = load_plan(plan_path, plan_sha256)
    prior_pin = None
    if usb_serial and STATE.is_file():
        prior_state = json.loads(STATE.read_text())
        if (prior_state.get('plan'), prior_state.get('plan_sha256')) == (str(plan_path), plan_sha256):
            prior_pin = request(prior_state).get('usb_diagnostics')
    usb_pin = _usb_pin(usb_serial, prior_pin) if usb_serial else None
    artifacts=dict(artifacts)
    sid, _ = Windows().token()
    session_id = uuid.uuid4().hex
    directory = DATA / 'sessions' / session_id
    directory.mkdir(parents=True)
    config = dict(session_id=session_id, owner_sid=sid, seconds=hours * 3600,
                  usb_diagnostics=usb_pin, install_reviewed_boot=bool(install_reviewed_boot),
                  plan=str(plan_path), plan_sha256=plan_sha256,
                  interpreter=str(Path(sys.executable).resolve()),
                  interpreter_sha256=hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
                  manifest=manifest, artifacts=_pack(artifacts), original=_pack(original))
    sources = {}
    for relative in SOURCES:
        data = (ROOT / relative).read_bytes()
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        sources[relative] = hashlib.sha256(data).hexdigest()
    config['sources'] = sources
    path = directory / 'snapshot.json'
    _json(path, config)
    prepared = dict(session_id=session_id, plan=str(plan_path), plan_sha256=plan_sha256,
                    snapshot=str(path), snapshot_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    script=str(directory / SOURCES[0]), interpreter=config['interpreter'],
                    interpreter_sha256=config['interpreter_sha256'], sources=sources,
                    owner_sid=sid, hours=hours, usb_diagnostics=usb_pin, install_reviewed_boot=bool(install_reviewed_boot))
    _json(directory / 'prepared.json', prepared)
    return prepared


def bootstrap(snapshot, expected_hash):
    """The ordinary reviewed script is the UAC trust boundary, like an installer."""
    win = Windows()
    if not win.token()[1]:
        raise RuntimeError('bootstrap needs normal UAC elevation')
    data = Path(snapshot).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected_hash:
        raise ValueError('snapshot hash differs from approved preparation')
    config = json.loads(data)
    if str(Path(sys.executable).resolve()) != config['interpreter'] or hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest() != config['interpreter_sha256']:
        raise ValueError('interpreter differs from approved preparation')
    if set(config['sources']) != set(SOURCES):
        raise ValueError('unexpected source closure')
    if config.get('usb_diagnostics') and _usb_pin(config['usb_diagnostics']['serial'], config['usb_diagnostics']) != config['usb_diagnostics']:
        raise RuntimeError('USB topology changed since approval preparation')
    source_bytes = {}
    for relative in SOURCES:
        source = (Path(snapshot).parent / relative).read_bytes()
        if hashlib.sha256(source).hexdigest() != config['sources'][relative]:
            raise ValueError('reviewed source hash mismatch')
        source_bytes[relative] = source
    windows = ctypes.create_unicode_buffer(32768)
    win.check(win.k.GetWindowsDirectoryW(windows, len(windows)))
    directory = Path(windows.value) / 'Temp' / ('LinnetRefresh-' + uuid.uuid4().hex)
    attributes, descriptor = win.security(config['owner_sid'], directory=True)
    try:
        win.check(win.k.CreateDirectoryW(str(directory), ctypes.byref(attributes)))
    finally:
        win.k.LocalFree(descriptor)
    # Parent is a system-owned directory; creation must be new and atomic. Its
    # protected DACL grants the caller read only, including inherited child ACLs.
    for relative, source in source_bytes.items():
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source)
    protected_config = directory / 'snapshot.json'
    protected_config.write_bytes(data)
    child = subprocess.Popen([sys.executable, '-I', str(directory / SOURCES[0]),
                              '--serve', str(protected_config)], cwd=str(directory),
                             creationflags=subprocess.CREATE_NO_WINDOW,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if (directory / 'ready.json').is_file():
            return
        if child.poll() is not None:
            raise RuntimeError(f'protected refresh server exited ({child.returncode})')
        time.sleep(.1)
    raise TimeoutError('protected refresh server did not publish readiness')


def serve(snapshot):
    win = Windows()
    if not win.token()[1]:
        raise RuntimeError('server requires the elevated bootstrap')
    config = json.loads(Path(snapshot).read_text(encoding='utf-8'))
    # These imports are from the protected closure, before the pipe exists.
    sys.path.insert(0, str(ROOT))
    from tools.studio import native_bootstrap as native
    manifest, artifacts, original = config['manifest'], _unpack(config['artifacts']), _unpack(config['original'])
    device = json.loads(artifacts['device.json'])
    derived, candidate, old_directory, new_directory = native.derive(
        original, artifacts['mbr.bin'], artifacts['boot.los'], device)
    if (candidate, old_directory, new_directory) != (artifacts['candidate.bin'], artifacts['directory-original.bin'], artifacts['directory-candidate.bin']):
        raise ValueError('snapshot candidate derivation mismatch')
    sector = manifest.get('sector_bytes', 512)
    if (sector not in (512, 2048, 4096) or len(new_directory) != sector or manifest['directory_disk_offset'] !=
            manifest['partition_lba'] * sector + manifest['directory_partition_offset']):
        raise ValueError('snapshot directory bounds mismatch')
    pipe = r'\\.\pipe\LinnetRefresh-' + config['session_id']
    attributes, descriptor = win.security(config['owner_sid'])
    try:
        # Duplex, first instance, message mode, nonblocking, reject remote clients.
        handle = win.k.CreateNamedPipeW(pipe, 3 | 0x80000, 4 | 2 | 1 | 8, 1, MAX_MESSAGE, MAX_MESSAGE, 0, ctypes.byref(attributes))
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        win.k.LocalFree(descriptor)
    directory = Path(snapshot).parent
    expires = time.monotonic() + config['seconds']
    status = dict(session_id=config['session_id'], plan=config['plan'], plan_sha256=config['plan_sha256'],
                  install_reviewed_boot=config.get('install_reviewed_boot') is True,
                  pid=os.getpid(), owner_sid=config['owner_sid'], poisoned=False,
                  started_at=datetime.now(timezone.utc).isoformat(), seconds=config['seconds'])
    _json(directory / 'ready.json', status)
    diagnostics = UsbDiagnostics(config, directory)
    status['usb_diagnostics'] = config.get('usb_diagnostics')
    receipts = {}
    stop = False
    try:
        while time.monotonic() < expires and not stop:
            diagnostics.tick()
            connected = win.k.ConnectNamedPipe(handle, None)
            if not connected and ctypes.get_last_error() not in (535,):
                if ctypes.get_last_error() == 536:  # no client yet
                    time.sleep(.05)
                    continue
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                response = None
                request = win.read(handle, 5)
                if not isinstance(request, dict) or request.get('session_id') != config['session_id']:
                    raise ValueError('session identity mismatch')
                command = request.get('command')
                if command in ('status', 'stop') and set(request) == {'session_id', 'command'}:
                    stop = command == 'stop'
                    response = dict(status, remaining_seconds=max(0, int(expires-time.monotonic())), stopping=stop)
                elif command in ('trace-start', 'trace-stop', 'usb-restart', 'usb-cycle') and set(request) == {'session_id', 'command'}:
                    response = dict(diagnostics.run(command), session_id=config['session_id'], plan_sha256=config['plan_sha256'])
                    _json(directory / (command + '-' + uuid.uuid4().hex + '.json'), response)
                elif command in ('refresh','install','verify') and set(request) == {'session_id', 'command', 'request_id', 'disk_number'}:
                    if command=='install' and config.get('install_reviewed_boot') is not True:
                        raise ValueError('approved session permits refresh only')
                    request_id, number = request['request_id'], request['disk_number']
                    if not isinstance(request_id, str) or not re.fullmatch('[0-9a-f]{32}', request_id) or type(number) is not int or not 0 <= number <= 1024:
                        raise ValueError('invalid refresh identity')
                    if request_id in receipts:
                        prior = receipts[request_id]
                        if prior.get('requested_disk_number') != number or prior.get('command') != command:
                            raise ValueError('request ID reused with different disk')
                        response = prior
                    elif (status['poisoned'] and command != 'verify') or len(receipts) >= 512:
                        raise RuntimeError('session writes revoked; inspect its receipts')
                    else:
                        response = dict(operation='verify-installed-directory' if command=='verify' else 'install-reviewed-boot' if command=='install' else 'refresh-installed-directory', command=command, plan=config['plan'], plan_sha256=config['plan_sha256'],
                                        passed=False, device_write_attempted=False, writes=[], phase='identify-device',
                                        started_at=datetime.now(timezone.utc).isoformat(), requested_disk_number=number,
                                        request_id=request_id, session_id=config['session_id'])
                        receipts[request_id] = response
                        receipt_path = directory / (request_id + '.json')
                        try:
                            # Pin executable lookup: the native utility uses a fixed
                            # integer-only, read-only Get-Disk query. System PATH and
                            # protected cwd exclude workspace executables.
                            response['device'] = native.identify_disk(number, manifest)
                            with open(r'\\.\PhysicalDrive' + str(number), 'rb' if command=='verify' else 'r+b', buffering=0) as device:
                                response['phase'] = 'verify-before-write' if command!='verify' else 'verify-read-only'
                                response['before'] = native.guard_device(device, manifest, artifacts, original)
                                if command=='install':
                                    if response['before']['staging']!='candidate':
                                        native.write_verified(device,manifest['candidate_disk_offset'],artifacts['candidate.bin'],response,receipt_path,'install-reviewed-payload',sector)
                                    response['phase'] = 'verify-staged-payload'
                                    native.guard_device(device,manifest,artifacts,original)
                                elif response['before']['directory'] != 'candidate':
                                    raise RuntimeError('refresh requires exact installed candidate')
                                if command != 'verify':
                                    native.write_verified(device, manifest['directory_disk_offset'], new_directory, response, receipt_path, 'refresh-identical-directory',sector)
                                    response['phase'] = 'verify-after-write'
                                    response['after'] = native.guard_device(device, manifest, artifacts, original)
                                else:
                                    response['after'] = dict(response['before'])
                                if command=='install' and (response['after']['directory']!='candidate' or response['after']['staging']!='candidate'):
                                    raise RuntimeError('installed Boot failed full guard')
                                if command=='refresh' and response['before'] != response['after']:
                                    raise RuntimeError('directory refresh changed firmware state')
                            response.update(passed=True, phase='complete')
                            if command in ('refresh','verify'):response['bytes_changed']=0
                        except Exception as exc:
                            response['failure'] = f'{type(exc).__name__}: {exc}'
                            if response['device_write_attempted']:
                                status['poisoned'] = True
                        response['finished_at'] = datetime.now(timezone.utc).isoformat()
                        _json(receipt_path, response)
                else:
                    raise ValueError('unsupported command or fields')
                win.write(handle, response)
                if command in ('refresh','install','verify'):
                    if win.read(handle, 5) != {'ack': request_id}:
                        raise RuntimeError('refresh completion was not acknowledged')
                # Give the client a bounded interval to consume the message before
                # DisconnectNamedPipe discards unread data.
                deadline = time.monotonic() + 5
                available = w.DWORD()
                while time.monotonic() < deadline and win.k.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None):
                    time.sleep(.025)
            except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as exc:
                if response and response.get('device_write_attempted'):
                    status['poisoned'] = True
                    _json(directory / 'revoked.json', dict(status, reason='refresh response was not acknowledged'))
                elif response is None:
                    try:
                        win.write(handle, dict(session_id=config['session_id'], plan_sha256=config['plan_sha256'],
                                               error=f'{type(exc).__name__}: {exc}'))
                        time.sleep(.1)
                    except OSError:
                        pass
            finally:
                win.k.DisconnectNamedPipe(handle)
    finally:
        if diagnostics.trace:
            diagnostics.run('trace-stop')
        win.k.CloseHandle(handle)
        _json(directory / 'closed.json', dict(status, closed_at=datetime.now(timezone.utc).isoformat()))


def request(state, command='status', *, disk_number=None, request_id=None):
    win = Windows()
    session_id = state.get('session_id', '')
    if not re.fullmatch('[0-9a-f]{32}', session_id):
        raise ValueError('invalid session ID')
    deadline = time.monotonic() + 5
    while True:
        handle = win.k.CreateFileW(r'\\.\pipe\LinnetRefresh-' + session_id, 0xC0000000, 0, None, 3, 0, None)
        if handle != ctypes.c_void_p(-1).value:
            break
        error = ctypes.get_last_error()
        if error != 231 or time.monotonic() >= deadline:
            raise ctypes.WinError(error)
        time.sleep(.025)  # connection discovery only; no request has been sent
    try:
        pid = w.ULONG()
        win.check(win.k.GetNamedPipeServerProcessId(handle, ctypes.byref(pid)))
        process = win.check(win.k.OpenProcess(0x1000, False, pid.value))
        try:
            sid, elevated = win.token(process)
            if not elevated or sid != state['owner_sid']:
                raise RuntimeError('pipe server is not the approved elevated owner')
        finally:
            win.k.CloseHandle(process)
        message = dict(session_id=session_id, command=command)
        if command in ('refresh','install','verify'):
            message.update(disk_number=disk_number, request_id=request_id or uuid.uuid4().hex)
        win.write(handle, message)
        response = win.read(handle, 60)
        if response.get('session_id') != session_id or response.get('plan_sha256') != state['plan_sha256']:
            raise RuntimeError('session response identity mismatch')
        if response.get('error'):
            raise RuntimeError(response['error'])
        if command in ('refresh','install','verify'):
            if response.get('request_id') != message['request_id']:
                raise RuntimeError('refresh request identity mismatch')
            win.write(handle, {'ack': message['request_id']})
        return response
    finally:
        win.k.CloseHandle(handle)


def active(plan_path=None, plan_sha256=None):
    try:
        state = json.loads(STATE.read_text(encoding='utf-8'))
        if plan_path is not None and (state.get('plan'), state.get('plan_sha256')) != (str(Path(plan_path).resolve()), plan_sha256):
            return None
        status = request(state)
        if status.get('poisoned') or status.get('remaining_seconds', 0) <= 0:
            return None
        return state
    except (OSError, ValueError, RuntimeError, TimeoutError, KeyError):
        return None


def start(prepared, *, allow_prompt=False):
    """Launch a reviewed preparation once; later clients reuse the live pipe."""
    from tools.link.eject import _elevated_refresh
    existing = active(prepared['plan'], prepared['plan_sha256'])
    if existing:
        status=request(existing)
        if prepared.get('usb_diagnostics') != status.get('usb_diagnostics'):
            raise RuntimeError('active session has different USB capabilities; stop it before approving replacement')
        if prepared.get('install_reviewed_boot') and not status.get('install_reviewed_boot'):
            raise RuntimeError('active session permits refresh only; stop it before approving installation capability')
        return status
    arguments = subprocess.list2cmdline(['-I', prepared['script'], '--bootstrap', prepared['snapshot'],
                                        '--snapshot-sha256', prepared['snapshot_sha256']])
    _elevated_refresh(prepared['interpreter'], arguments, str(Path(prepared['snapshot']).parent),
                      allow_prompt=allow_prompt)
    status = request(prepared)
    if status.get('poisoned') or status.get('remaining_seconds', 0) <= 0:
        raise RuntimeError('new session is not ready')
    STATE.parent.mkdir(parents=True, exist_ok=True)
    _json(STATE, {key: prepared[key] for key in ('session_id', 'plan', 'plan_sha256', 'owner_sid')})
    return status


def refresh(plan_path, plan_sha256, disk_number, receipt_path):
    if not STATE.is_file():
        return False
    state = json.loads(STATE.read_text(encoding='utf-8'))
    if (state.get('plan'), state.get('plan_sha256')) != (str(Path(plan_path).resolve()), plan_sha256):
        return False
    try:
        status = request(state)
    except OSError as exc:
        if getattr(exc, 'winerror', None) == 2:  # pipe no longer exists
            return False
        raise  # a busy/uncertain live session must never cause a second writer
    if status.get('poisoned') or status.get('remaining_seconds', 0) <= 0:
        raise RuntimeError('session writes revoked or expired; inspect its receipts')
    request_id = uuid.uuid4().hex
    # Persist request identity before IPC so a caller can investigate uncertainty.
    _json(Path(receipt_path), dict(operation='refresh-installed-directory', passed=False,
                                 phase='request-session', session_id=state['session_id'],
                                 request_id=request_id, plan=str(Path(plan_path).resolve()),
                                 plan_sha256=plan_sha256))
    result = request(state, 'refresh', disk_number=disk_number, request_id=request_id)
    _json(Path(receipt_path), result)
    return True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bootstrap', type=Path)
    parser.add_argument('--snapshot-sha256')
    parser.add_argument('--serve', type=Path)
    args = parser.parse_args()
    if args.bootstrap and args.snapshot_sha256 and not args.serve:
        bootstrap(args.bootstrap, args.snapshot_sha256)
    elif args.serve and not args.bootstrap:
        # Pin executable search before importing the protected native utility.
        api = Windows()
        windows = ctypes.create_unicode_buffer(32768)
        api.check(api.k.GetWindowsDirectoryW(windows, len(windows)))
        system = Path(windows.value) / 'System32'
        os.environ['PATH'] = str(system) + os.pathsep + str(system / 'WindowsPowerShell/v1.0')
        serve(args.serve)
    else:
        parser.error('use the reviewed bootstrap or its protected server')
