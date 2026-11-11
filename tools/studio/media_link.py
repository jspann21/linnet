"""Recognize and open an identified Linnet Link connection from Media Manager."""
import time
from contextlib import ExitStack


def _classic_record(connection):
    from tools.studio.classic_recovery import _scope, load
    for path in _scope(connection['serial']).glob('*/NOR.JSON'):
        try:
            record, _, _ = load(path)
            if (record['monitor_build_id'] == connection['identity']['build_id'] and
                    record.get('startup_default_format') == 1):
                return record
        except (OSError, ValueError):
            continue
    return None


def discover():
    from tools.link.transport import SerialTransport
    from tools.link.session import Session
    from tools.link.identity import decode
    from tools.studio.targets import TARGETS
    connections = []
    for port in SerialTransport.discover():
        if (port['vid'], port['pid']) != (0x1209, 0x504a) or not port['serial_number']:
            continue
        session = None
        try:
            session = Session(SerialTransport(port['port']), timeout=2)
            identity = decode(session.hello())
            connection = dict(serial=port['serial_number'], identity=identity,
                              target_name=TARGETS[identity['target_id']]['name'])
            if identity['target_id'] == 'classic6g-n25':
                from .classic_setup import product_identity
                product = product_identity(port['serial_number'])
                if product:
                    connection['product'] = {key: value for key, value in product.items() if key != 'target_id'}
                    connection['target_name'] = 'Linnet for ' + product['product_name']
            connection['can_open_disk'] = bool(identity['capabilities'] & 32 or
                identity['target_id'] == 'classic6g-n25' and identity['image_kind'] == 'boot' and
                _classic_record(connection))
            connections.append(connection)
        except (OSError, RuntimeError, ValueError):
            continue
        finally:
            if session is not None:
                session.close()
    return connections


def _saved_name(connection):
    """Use a saved name only when its target and board USB identity both match."""
    import json
    from tools.content.guard import guarded_path
    from tools.content.host_state import media_state_root

    root = media_state_root()
    candidates = []
    for path in root.glob('*/session-*.json'):
        try:
            info = guarded_path(path, root)
            if info is not None and info.st_size <= 1048576:
                candidates.append((info.st_mtime_ns, path))
        except (OSError, ValueError):
            continue
    for _, path in sorted(candidates, reverse=True)[:256]:
        try:
            with path.open('rb') as reader:
                raw = reader.read(1048577)
            if len(raw) > 1048576:
                continue
            record = json.loads(raw)
            identity = record.get('identity', {})
            name = identity.get('volume_label')
            if (identity.get('target_id') == connection['identity']['target_id'] and
                    identity.get('linnet_usb_serial') == connection['serial'] and
                    isinstance(name, str) and name.strip() and len(name) <= 128 and name.isprintable()):
                return name.strip()
        except (OSError, ValueError, AttributeError, TypeError):
            continue
    return None


def read_details(connection):
    """Read live Link telemetry without mounting storage or changing boot mode."""
    from tools.link.campaign import connect
    from tools.link.identity import decode
    from tools.studio.targets import TARGETS

    fields = ('target_id', 'image_kind', 'profile', 'build_id', 'image_sha256', 'generation')

    def verify(current):
        if any(current[key] != connection['identity'][key] for key in fields):
            raise ValueError('The connected software changed. Choose Refresh before reading device details.')

    session, _, current = connect(serial_number=connection['serial'], deadline_seconds=10)
    try:
        verify(current)

        def reconnect_if_needed():
            nonlocal session
            if session.poisoned:
                # A failed optional read poisons the session. Continue only
                # on a fresh, verified connection.
                session.close()
                session, _, observed = connect(serial_number=connection['serial'], deadline_seconds=10)
                verify(observed)

        spec = TARGETS[current['target_id']]
        product = connection.get('product', {})
        family = product.get('product_name') or spec['name'].removeprefix('Linnet for ')
        saved_name = _saved_name(connection)
        name = saved_name or family
        models = product.get('model_number') or ' / '.join(spec['retail']['regulatory_models'])
        rows = [f'Device  {family}', f'Model  {models}', 'Connection  Linnet Link',
                f"USB serial  {connection['serial']}",
                'Software  ' + ('Linnet OS' if current['image_kind'] == 'os' else 'Linnet recovery'),
                'Build  ' + {'release': 'User', 'dev': 'Developer', 'trace': 'Trace'}[current['profile']],
                'Health  ' + ('Ready' if current['healthy'] else 'Checks incomplete'),
                f"Build ID  {current['build_id']}"]
        if current['image_kind'] == 'os':
            try:
                media = session.media_info()
            except (OSError, RuntimeError, ValueError):
                rows.append('Storage details unavailable')
                reconnect_if_needed()
            else:
                rows += [f"Drive  {media['model']}", f"Drive firmware  {media['firmware']}",
                         f"Drive serial  {media['serial'].strip()}",
                         f"Disk size  {media['sectors'] * media['logical_sector_bytes'] / 1_000_000_000:.1f} GB",
                         f"Logical sector  {media['logical_sector_bytes']} B",
                         f"Physical sector  {media['physical_sector_bytes']} B",
                         f"Storage power  {media['power_state_name']}"]
            try:
                power = session.power_state()
            except (OSError, RuntimeError, ValueError):
                rows.append('Battery details unavailable')
                reconnect_if_needed()
            else:
                if power['flags'] & 8:
                    rows.append(f"Battery  {power['battery_percent']}% ({power['battery_mv']} mV)")
                if power['flags'] & 1:
                    rows.append(f"USB charge limit  {power['charge_limit_ma']} mA")
            if current['target_id'] == 'video5g-m25':
                try:
                    from tools.link.m25_hardware import read as read_hardware
                    hardware = read_hardware(session, current)
                except (OSError, RuntimeError, ValueError):
                    rows.append('Board and memory details unavailable')
                    reconnect_if_needed()
                else:
                    rows += [f"Board revision  {hardware['board_revision']}",
                             f"Usable RAM  {hardware['usable_ram_bytes'] // (1024 * 1024)} MiB"]
        verify(decode(session.status()))
        return dict(name=name, name_saved=bool(saved_name), rows=rows, used_percent=None, link_connection=connection,
                    subtitle='Live details over Linnet Link.')
    finally:
        session.close()


def open_disk(connection, *, progress=lambda _message: None):
    from tools.content.publish import _exclusive
    with ExitStack() as locks:
        if connection['identity']['target_id'] == 'classic6g-n25':
            locks.enter_context(_exclusive(dict(volume_unique_id='classic-setup:'+connection['serial'])))
        return _open_disk(connection, progress=progress)


def open_setup(connection, *, section, progress=lambda _message: None):
    """Connect storage and read setup for the user's selected task."""
    if section not in ('startup', 'build', 'remove'):
        raise ValueError('Unknown iPod setup section')
    from tools.studio.installer import assess
    result = open_disk(connection, progress=progress)
    identity = result['reconnected_identity']
    progress('Reading installed builds and startup choices…')
    # Retain the mounted identity if assessment fails: storage has already
    # changed mode and the old Link connection is no longer usable.
    try:
        from tools.content.publish import pending_publications
        groups = pending_publications(identity)
        if groups:
            return dict(result, section=section, publication_recovery_pending=True,
                        identity=identity, groups=groups)
        review = assess(identity)
    except (OSError, RuntimeError, ValueError) as error:
        return dict(result, section=section, review=None, setup_error=str(error))
    return dict(result, section=section, review=review)


def _open_disk(connection, *, progress):
    from tools.link.campaign import connect
    from tools.content.media_manager import discover as disks
    session, _, current = connect(serial_number=connection['serial'])
    try:
        if any(current[key] != connection['identity'][key] for key in
               ('target_id', 'image_kind', 'build_id', 'generation')):
            raise ValueError('The connected software changed. Choose Refresh before opening Disk Mode.')
        progress('Opening Disk Mode automatically. Keep your iPod connected…')
        if current['capabilities'] & 32:
            session.reboot('disk')
        elif current['target_id'] == 'classic6g-n25' and current['image_kind'] == 'boot' and _classic_record(connection):
            from tools.link.classic_nor import _operation
            _operation(session, 67, 0x52454254, size=0)
        else:
            raise ValueError('Resume Classic installation to finish setting up this iPod.')
    finally:
        session.close()
    deadline = time.monotonic() + 90
    last_error = ''
    while time.monotonic() < deadline:
        try:
            result = disks(detailed=True)
        except (OSError, RuntimeError, ValueError) as error:
            last_error = str(error)
            time.sleep(.5)
            continue
        last_error = '; '.join(result['errors'])
        matches = [item for item in result['devices'] if item.get('linnet_usb_serial') == connection['serial'] and
                   item.get('native_target_id') == current['target_id']]
        if len(matches) > 1:
            raise ValueError('More than one mounted iPod matches this Link connection.')
        if matches:
            return dict(passed=True, reconnected_identity=matches[0], summary='Your iPod is ready for media management.')
        time.sleep(.5)
    raise TimeoutError('Disk Mode has not reconnected. Keep USB connected and choose Refresh.' +
                       (' '+last_error if last_error else ''))
