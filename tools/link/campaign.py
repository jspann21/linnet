"""Real device cycles with exact identity checks and failure receipts."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import time
from tools.inspect.image import inspect, verify_device_report
from .identity import decode, decode_crash, CAP_UPLOAD, CAP_RAM_BOOT, CAP_CRASH
from .protocol import Command
from .session import Session, AmbiguousCommand
from .transport import SerialTransport

def connect(port=None, serial_number=None, deadline_seconds=15):
    """Probe only a selected port or identified Linnet USB endpoints."""
    deadline = time.monotonic() + deadline_seconds
    errors = []
    while time.monotonic() < deadline:
        inventory = SerialTransport.discover()
        candidates = [p for p in inventory
                      if (not port or p['port'] == port)
                      and (not serial_number or p['serial_number'] == serial_number)
                      and (port or serial_number or (p.get('vid'), p.get('pid')) == (0x1209, 0x504a)
                           or 'Linnet' in (p['description'] or ''))]
        if serial_number and len(candidates) > 1:
            raise RuntimeError("multiple USB endpoints have the requested serial identity")
        if not port and not serial_number and len(candidates) > 1:
            raise RuntimeError("multiple Linnet endpoints found; select a port or USB serial identity")
        for candidate in candidates:
            session = None
            try:
                session = Session(SerialTransport(candidate['port']), timeout=2)
                identity = decode(session.hello())
                if not candidate['serial_number']:
                    raise RuntimeError("Linnet USB endpoint has no stable hardware serial identity")
                return session, candidate, identity
            except (ValueError, RuntimeError, OSError) as exc:
                errors.append(str(exc))
                errors = errors[-4:]
                if session:
                    session.close()
            except BaseException:
                if session:
                    session.close()
                raise
        time.sleep(min(0.2, max(0, deadline-time.monotonic())))
    raise TimeoutError("no responding Linnet endpoint before deadline" + (": " + '; '.join(errors) if errors else ""))

def _save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    for attempt in range(8):
        try:
            temporary.replace(path)
            return
        except PermissionError as exc:
            # Windows readers and scanners can briefly hold the old receipt
            # without delete sharing. Retry only this local atomic rename;
            # no device command or temporary-file write is repeated.
            if getattr(exc, 'winerror', None) not in (5, 32, 33) or attempt == 7:
                raise
            time.sleep(.025 * (attempt + 1))


def wait_ready(serial_number, *, expected=None, generation=None, library=False, timeout=60):
    """Wait using read-only requests; never replay the action that caused a transition."""
    if not serial_number or not 0 < timeout <= 600:
        raise ValueError('readiness requires a USB serial and a timeout in (0, 600] seconds')
    if expected and expected['profile'] == 'release':
        raise ValueError('release images do not expose Linnet Link readiness')
    started = time.monotonic()
    deadline = started + timeout
    session = None
    last = 'no responding endpoint'
    try:
        while time.monotonic() < deadline:
            try:
                if session is None:
                    session, device, _ = connect(serial_number=serial_number,
                        deadline_seconds=min(5, deadline-time.monotonic()))
                session.timeout = min(2, max(.001, deadline-time.monotonic()))
                identity = decode(session.status())
                media = None
                if library and identity['image_kind'] == 'os' and identity['healthy']:
                    from .music import command
                    media = command(session, identity, 'library-state')['state']
                    if decode(session.status()) != identity:
                        raise RuntimeError('identity changed while reading library readiness')
            except (ValueError, RuntimeError, OSError) as exc:
                last = str(exc)
                if session is not None:
                    session.close()
                    session = None
            else:
                if expected and identity['target_id'] != expected['target_id']:
                    raise ValueError('device target differs from the expected image')
                matches = not expected or (identity['profile'] == expected['profile'] and
                    identity['build_id'] == expected['build_id'] and identity['image_sha256'] == expected['sha256'])
                matches = matches and (generation is None or identity['generation'] == generation)
                last = f"{identity['image_kind']} {identity['build_id'][:12]}, generation {identity['generation']}, healthy={identity['healthy']}"
                if media is not None:
                    last += f", library phase={media['phase']}, error={media['error']}"
                    if matches and media['error']:
                        raise RuntimeError(f'OS is running but library initialization failed: {last}')
                if (time.monotonic() < deadline and matches and identity['image_kind'] == 'os' and
                        identity['healthy'] and (not library or (media and media['phase'] == 3))):
                    return dict(ready=True, device=device, identity=identity, library=media,
                                elapsed_seconds=round(time.monotonic()-started, 3))
            time.sleep(min(.1, max(0, deadline-time.monotonic())))
    finally:
        if session is not None:
            session.close()
    raise TimeoutError(f'OS readiness was not confirmed within {timeout:g} seconds; last observation: {last}')


def wait_upload_ready(session, device, identity, timeout=180):
    """Resolve cold Boot's automatic handoff before sending any upload bytes.

    Only read-only discovery/HELLO/STATUS is repeated. Return the new session
    and identity if the device automatically enters its persistent OS.
    """
    if identity.get('healthy'):
        return session, device, identity
    serial = device.get('serial_number')
    target = identity['target_id']
    if not serial:
        session.close()
        raise RuntimeError('Waiting for startup requires a stable USB serial')
    deadline = time.monotonic() + timeout
    session.close()
    while time.monotonic() < deadline:
        candidate = None
        ready = False
        try:
            candidate, observed_device, observed = connect(serial_number=serial,
                deadline_seconds=min(5, max(.1, deadline-time.monotonic())))
            confirmed = decode(candidate.status())
            if observed['target_id'] != target or confirmed['target_id'] != target:
                raise ValueError('Device target changed while waiting for startup')
            if (observed['build_id'], observed['generation']) == (confirmed['build_id'], confirmed['generation']) and confirmed['healthy']:
                ready = True
                return candidate, observed_device, confirmed
        except (RuntimeError, OSError):
            pass
        finally:
            if candidate is not None and not ready:
                candidate.close()
        time.sleep(min(.2, max(0, deadline-time.monotonic())))
    raise TimeoutError('Startup did not settle; no upload was sent')

def _run_directory(root, identity):
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    root.mkdir(parents=True, exist_ok=True)
    for seq in range(1, 10000):
        run_id = f"{stamp}-{identity['target_id']}-{identity['build_id'][:12]}-{seq:04d}"
        directory = root / run_id
        try:
            directory.mkdir()
            return run_id, directory
        except FileExistsError:
            continue
    raise RuntimeError('run sequence exhausted')

def _resume_evidence(root, source, image_id, suite, crash_id, hang_id):
    path = Path(source)
    if not path.exists():
        path = root / path
    if path.is_dir():
        path /= 'receipt.json'
    raw = path.read_bytes()
    if len(raw) > 5 * 1024 * 1024:
        raise ValueError('resume receipt exceeds size limit')
    previous = json.loads(raw)
    if previous.get('passed') or previous.get('resume'):
        raise ValueError('resume requires an original interrupted healthy-cycle run')
    if previous.get('image') != image_id or previous.get('crash_image') != crash_id or previous.get('hang_image') != hang_id:
        raise ValueError('resume image identities differ from the original run')
    old_suite = previous.get('scenario', {})
    if ({k:v for k,v in old_suite.items() if k != 'healthy_cycles'} !=
            {k:v for k,v in suite.items() if k != 'healthy_cycles'}):
        raise ValueError('resume scenario changed beyond its cycle requirement')
    if type(old_suite.get('healthy_cycles')) is not int or not suite['healthy_cycles'] <= old_suite['healthy_cycles'] <= 500:
        raise ValueError('resume requires the same or a reduced cycle requirement')
    anchor = previous.get('before', {})
    if anchor.get('image_kind') != 'boot' or anchor.get('target_id') != image_id['target_id'] or anchor.get('profile') == 'release':
        raise ValueError('resume source lacks a matching Boot anchor')
    generation = anchor.get('generation')
    if type(generation) is not int or not 0 <= generation <= 0xffffffff:
        raise ValueError('invalid original Boot generation')
    healthy, metrics, elapsed = [], {}, -1
    for step in previous.get('steps', []):
        if step.get('name') not in ('handshake', 'healthy-cycle', 'ambiguous-boot-ack'):
            raise ValueError('resume source already entered fault/reboot checks')
        stamp = step.get('elapsed_seconds')
        if not isinstance(stamp, (int, float)) or not elapsed < stamp < 86400:
            raise ValueError('resume timestamps are not strictly increasing')
        elapsed = stamp
        if step['name'] != 'healthy-cycle':
            continue
        index = len(healthy) + 1
        identity = step.get('identity', {})
        if type(step.get('index')) is not int or step['index'] != index:
            raise ValueError('resume indices are not contiguous')
        _check_healthy_identity(identity, image_id)
        if type(identity.get('generation')) is not int or identity['generation'] != generation + index:
            raise ValueError('resume generations are not consecutive')
        name = f'cycle-{index:04d}-metrics.bin'
        metrics[name] = (path.parent / name).read_bytes()
        if len(metrics[name]) != 32:
            raise ValueError('invalid resume native metrics length')
        healthy.append(step)
    if not healthy or previous.get('phase') not in (f'healthy-cycle-{len(healthy)}', f'healthy-cycle-{len(healthy)+1}'):
        raise ValueError('resume source is not a healthy-cycle interruption')
    serial_number = previous.get('device', {}).get('serial_number')
    if not isinstance(serial_number, str) or not serial_number:
        raise ValueError('resume source has no physical serial identity')
    return dict(path=path.resolve(), raw=raw, previous=previous, healthy=healthy,
                metrics=metrics, serial_number=serial_number, anchor=anchor)


def _check_healthy_identity(identity, image_id):
    if (identity.get('image_kind') != 'os' or identity.get('healthy') is not True or
            identity.get('health_flags') != 31 or identity.get('build_id') != image_id['build_id'] or
            identity.get('image_sha256') != image_id['sha256'] or
            any(identity.get(key) != image_id[key] for key in ('target_id', 'profile'))):
        raise ValueError('resume healthy identity, hash or health mismatch')


def _resume_current(evidence, current, device, image_id):
    _check_healthy_identity(current, image_id)
    last = evidence['healthy'][-1]['identity']['generation']
    allowed = (last, last + 1) if evidence['previous']['phase'] == f"healthy-cycle-{len(evidence['healthy'])+1}" else (last,)
    if (device.get('serial_number') != evidence['serial_number'] or
            type(current.get('generation')) is not int or current['generation'] not in allowed):
        raise ValueError('current physical device or generation differs from resume evidence')
    return current['generation'] - last


def cycle(root, image_path, scenario_path, port=None, crash_image=None, progress=None,
          release_image=None, release_symbols=None, hang_image=None, resume_run=None):
    image = image_path.read_bytes()
    image_id = inspect(image)
    if image_id['profile'] == 'release':
        raise ValueError('device campaign requires a development transport image')
    scenario_bytes = scenario_path.read_bytes()
    suite = json.loads(scenario_bytes)
    if (not isinstance(suite, dict) or
            set(suite) != {'target_id', 'healthy_cycles', 'required_invariants'}):
        raise ValueError('invalid recovery scenario structure')
    count = suite.get('healthy_cycles')
    if suite.get('target_id') != image_id['target_id'] or type(count) is not int or not 1 <= count <= 500:
        raise ValueError('invalid recovery scenario or cycle count')
    crash_bytes = crash_image.read_bytes() if crash_image else None
    crash_id = inspect(crash_bytes) if crash_bytes else None
    hang_bytes = hang_image.read_bytes() if hang_image else None
    hang_id = inspect(hang_bytes) if hang_bytes else None
    for name, payload, identity in [('OS', image, image_id), ('crash', crash_bytes, crash_id), ('hang', hang_bytes, hang_id)]:
        if identity and (identity['profile'] == 'release' or identity['target_id'] != image_id['target_id']):
            raise ValueError(f'{name} image must be a matching development image')
        if identity and (len(payload) > 0x800000 or identity['load_address'] != 0 or identity['payload_bytes'] < 32):
            raise ValueError(f'{name} image does not fit the fixed staging and entry policy')
    identities = [item['build_id'] for item in (image_id, crash_id, hang_id) if item]
    if len(set(identities)) != len(identities):
        raise ValueError('healthy, crash and watchdog probes must have distinct build identities')
    release_result = None
    if release_image and release_symbols:
        from tools.inspect.image import verify_release
        release_result = verify_release(release_image.read_bytes(), release_symbols.read_text())
    evidence = _resume_evidence(root, resume_run, image_id, suite, crash_id, hang_id) if resume_run else None
    run_id, directory = _run_directory(root, image_id)
    receipt = dict(run_id=run_id, image=image_id, scenario=suite,
                   scenario_sha256=hashlib.sha256(scenario_bytes).hexdigest(),
                   crash_image=crash_id, hang_image=hang_id,
                   passed=False, started_at=datetime.now(timezone.utc).isoformat(),
                   phase='discovery', steps=[], device_write_attempted=False)
    completed = len(evidence['healthy']) if evidence else 0
    receipt['healthy_cycles_required'] = count
    receipt['healthy_cycles_completed'] = completed
    if evidence:
        (directory / 'resume-source-receipt.json').write_bytes(evidence['raw'])
        for name, data in evidence['metrics'].items():
            (directory / name).write_bytes(data)
        receipt['resume'] = dict(source_run_id=evidence['previous']['run_id'],
            source_receipt=str(evidence['path']), source_receipt_sha256=hashlib.sha256(evidence['raw']).hexdigest(),
            preserved_receipt='resume-source-receipt.json',
            metrics={name:hashlib.sha256(data).hexdigest() for name,data in evidence['metrics'].items()},
            source_scenario=evidence['previous']['scenario'],
            source_scenario_sha256=evidence['previous']['scenario_sha256'],
            recorded_healthy_cycles=completed, requirement_reduced=count < evidence['previous']['scenario']['healthy_cycles'])
    receipt_path = directory / 'receipt.json'
    _save(receipt_path, receipt)
    session = None
    serial_number = None
    anchor = None

    def record(name, **details):
        receipt['steps'].append(dict(name=name, elapsed_seconds=round(time.monotonic()-started, 6), **details))
        _save(receipt_path, receipt)
        if progress:
            progress(dict(run_id=run_id, phase=name, **details))

    def reconnect(expected=None, require_kind=None, previous_generation=None, generation_steps=1,
                  require_healthy=False, timeout_seconds=20):
        nonlocal session
        if session:
            session.close()
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            try:
                session, device, identity = connect(serial_number=serial_number, deadline_seconds=min(3, max(.1, deadline-time.monotonic())))
            except TimeoutError:
                continue
            expected_generation = None if previous_generation is None else (previous_generation + generation_steps) & 0xffffffff
            if expected_generation is not None and identity['generation'] != expected_generation:
                session.close()
                session = None
                time.sleep(.2)
                continue
            if expected and (identity['build_id'] != expected['build_id'] or identity['image_sha256'] != expected['sha256']):
                session.close()
                session = None
                time.sleep(.2)
                continue
            if require_kind and identity['image_kind'] != require_kind:
                session.close()
                session = None
                time.sleep(.2)
                continue
            if require_kind == 'boot' and anchor and identity['build_id'] != anchor['build_id']:
                raise RuntimeError('recovery anchor build identity changed')
            if require_healthy and not identity['healthy']:
                # Read-only readiness observation stays in the established
                # session; do not reset its sequence by reopening the same port.
                while time.monotonic() < deadline and not identity['healthy']:
                    time.sleep(.05)
                    identity = decode(session.status())
                    if expected and (identity['build_id'] != expected['build_id'] or identity['image_sha256'] != expected['sha256']):
                        raise RuntimeError('running identity changed while waiting for healthy marker')
                    if identity['image_kind'] != require_kind or identity['generation'] != expected_generation:
                        raise RuntimeError('boot generation changed while waiting for healthy marker')
                if not identity['healthy']:
                    raise TimeoutError('boot did not establish all required health conditions')
            return identity
        raise TimeoutError('re-enumerated device did not report expected image identity')

    def boot_bytes(payload, expected):
        previous = decode(session.status())['generation']
        receipt['device_write_attempted'] = True
        _save(receipt_path, receipt)
        session.upload(payload,image_path=image_path)
        verified = session.verify()
        if verified != hashlib.sha256(payload).digest():
            raise RuntimeError('device verified a different staged image')
        try:
            session.boot_ram()
        except AmbiguousCommand as exc:
            record('ambiguous-boot-ack', resolution='inspect after reconnect; command not repeated', detail=str(exc))
        # BOOT_RAM acknowledges before the final native hash/copy operation.
        # Its re-enumeration deadline must scale like upload verification.
        transition_timeout = min(600, max(20, len(payload) / 8192))
        return reconnect(expected, require_kind='os', previous_generation=previous,
                         require_healthy=True, timeout_seconds=transition_timeout)

    def capture_crash(name, expected, generation, reason):
        raw = session.crash_get()
        (directory / f'{name}.bin').write_bytes(raw)
        result = decode_crash(raw)
        if result['build_id'] != expected['build_id'] or result['generation'] != generation or result['reason'] != reason:
            raise RuntimeError(f'{name} crash build, boot generation or reason mismatch')
        return result

    def fault_probe(name, payload, expected, reason, source):
        receipt['phase'] = name
        _save(receipt_path, receipt)
        previous = decode(session.status())['generation']
        session.upload(payload,image_path=source)
        if session.verify() != hashlib.sha256(payload).digest():
            raise RuntimeError(f'{name} verification identity mismatch')
        try:
            session.boot_ram()
        except AmbiguousCommand as exc:
            record(f'ambiguous-{name}-boot-ack', detail=str(exc), resolution='reconnect without replay')
        recovered = reconnect(require_kind='boot', previous_generation=previous, generation_steps=2,
                              timeout_seconds=min(600, max(20, len(payload) / 8192)))
        crash = capture_crash(name, expected, (previous + 1) & 0xffffffff, reason)
        record(f'{name}-recovered', identity=recovered, crash=crash)
        return recovered

    started = time.monotonic()
    try:
        session, device, before = connect(port=port, serial_number=evidence['serial_number'] if evidence else None)
        for path, image_identity in ((image_path,image_id),(crash_image,crash_id),(hang_image,hang_id)):
            if path:verify_device_report(path,image_identity,device['serial_number'],before['target_id'])
        serial_number = device['serial_number']
        receipt['device'] = device
        receipt['before'] = before
        if not evidence and before['image_kind'] != 'boot':
            raise RuntimeError('start the campaign in Linnet Boot so the recovery anchor identity can be pinned')
        if before['profile'] == 'release':
            raise RuntimeError('release firmware must not expose the development campaign endpoint')
        anchor = evidence['anchor'] if evidence else before
        receipt['anchor'] = anchor
        current = before
        if evidence:
            advance = _resume_current(evidence, before, device, image_id)
            record('resume-validated', recorded_healthy_cycles=completed, required_healthy_cycles=count,
                   identity=before, unrecorded_generation_advance=advance, unrecorded_cycle_counted=False)
        if before['capabilities'] & (CAP_UPLOAD | CAP_RAM_BOOT) != CAP_UPLOAD | CAP_RAM_BOOT:
            raise RuntimeError('connected image lacks upload/RAM-boot capabilities')
        record('handshake', identity=before)
        for index in range(completed, count):
            receipt['phase'] = f'healthy-cycle-{index+1}'
            current = boot_bytes(image, image_id)
            if not current['healthy']:
                raise RuntimeError('boot did not establish all required health conditions')
            metrics = session.request(Command.READ_METRICS)
            if len(metrics) != 32:
                raise RuntimeError('invalid native metrics response length')
            (directory / f'cycle-{index+1:04d}-metrics.bin').write_bytes(metrics)
            receipt['healthy_cycles_completed'] = index + 1
            record('healthy-cycle', index=index+1, identity=current)
        receipt['phase'] = 'crash-after-healthy'
        _save(receipt_path, receipt)
        if not current['capabilities'] & CAP_CRASH:
            raise RuntimeError('development crash command is unavailable')
        try:
            session.request(Command.CRASH, struct.pack('<I', 0))
        except AmbiguousCommand as exc:
            record('ambiguous-crash-ack', detail=str(exc), resolution='reconnect without replay')
        recovered = reconnect(require_kind='boot', previous_generation=current['generation'])
        crash = capture_crash('crash-after-healthy', image_id, current['generation'], 1)
        record('crash-after-healthy-recovered', identity=recovered, crash=crash)
        if not crash_bytes:
            raise RuntimeError('before-healthy crash image is required; supply --crash-image')
        fault_probe('crash-before-healthy', crash_bytes, crash_id, 1, crash_image)
        if not hang_bytes:
            raise RuntimeError('watchdog hang image is required; supply --hang-image')
        fault_probe('watchdog-hang-before-healthy', hang_bytes, hang_id, 9, hang_image)
        receipt['phase'] = 'reboot-modes'
        current = boot_bytes(image, image_id)
        previous = current['generation']
        try:
            session.reboot('normal')
        except AmbiguousCommand as exc:
            record('ambiguous-reboot-os-ack', detail=str(exc), resolution='inspect without replay')
        current = reconnect(image_id, require_kind='os', previous_generation=previous, require_healthy=True)
        if not current['healthy']:
            raise RuntimeError('normal reboot did not report healthy OS')
        record('reboot-os', identity=current)
        previous = current['generation']
        try:
            session.reboot('recovery')
        except AmbiguousCommand as exc:
            record('ambiguous-reboot-boot-ack', detail=str(exc), resolution='inspect without replay')
        current = reconnect(require_kind='boot', previous_generation=previous)
        record('reboot-boot', identity=current)
        # A disk-mode reboot must produce a real host mass-storage observation.
        # It is deliberately last because Apple disk mode leaves Linnet Link.
        if release_result is None:
            raise RuntimeError('release image and symbol report are required for complete verification')
        record('release-exclusion', result=release_result)
        from .storage_observation import disks, wait_for_disk
        existing_disks = [disk for disk in disks() if serial_number.casefold() in
                          ((disk.get('SerialNumber') or '') + ' ' + (disk.get('PNPDeviceID') or '')).casefold()]
        if existing_disks:
            raise RuntimeError('matching disk already present before reboot; cannot prove a new disk-mode transition')
        record('disk-baseline', matching_disks=existing_disks)
        try:
            session.reboot('disk')
        except AmbiguousCommand as exc:
            record('ambiguous-reboot-disk-ack', detail=str(exc), resolution='observe storage without replay')
        session.close()
        session = None
        disk = wait_for_disk(serial_number, 30)
        record('reboot-disk', disk=disk)
        receipt['phase'] = 'complete'
        receipt['passed'] = True
    except (ValueError, OSError, RuntimeError) as exc:
        receipt['failure'] = str(exc)
        record('failed', detail=str(exc))
    finally:
        if session:
            session.close()
        receipt['finished_at'] = datetime.now(timezone.utc).isoformat()
        _save(receipt_path, receipt)
    return receipt
