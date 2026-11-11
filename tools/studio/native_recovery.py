"""Bind original firmware recovery records to the same iPod in Linnet Disk Mode."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import uuid

from tools.studio import native_bootstrap as native
from tools.studio.targets import TARGETS


def eligible(identity):
    return (identity.get('native_target_id') == identity.get('target_id') and
            TARGETS[identity['target_id']]['installer']['boot_layout'] == 'ipod-directory')


def _directory_target(identity):
    spec = TARGETS[identity['target_id']]['installer']
    return spec['enabled'] and spec['boot_layout'] == 'ipod-directory'


def _upgrade_volume_identity(identity, transaction, path, digest, manifest, artifacts):
    """Verify the FAT identity recorded by a completed native Boot upgrade."""
    storage = json.loads(artifacts['device.json'])['storage']
    if (path != transaction / 'apple-recovery-plan/plan.json' or
            storage['disk_serial'] != identity['linnet_usb_serial']):
        return False
    for worker_path in transaction.glob('worker-*.json'):
        try:
            job = json.loads(worker_path.read_text(encoding='utf-8'))
            recorded = job['identity']
            result_path = Path(job['result']).resolve(strict=True)
            if (job['mode'] != 'upgrade-boot' or
                    Path(job['transaction']).resolve(strict=True) != transaction or
                    result_path.parent != transaction or
                    recorded['native_target_id'] != identity['target_id'] or
                    recorded['disk_serial'] != storage['disk_serial'] or
                    recorded['volume_unique_id'] != storage['volume_id'] or
                    any(recorded[key] != identity[key] for key in
                        ('target_id', 'linnet_usb_serial', 'volume_serial', 'disk_bytes',
                         'sector_bytes', 'partition_offset', 'partition_bytes'))):
                continue
            result = json.loads(result_path.read_text(encoding='utf-8'))
            recovery, applied = result['recovery'], result['applied']
            if (result['operation'] != 'upgrade-boot' or result['passed'] is not True or
                    recovery['strategy'] != 'native-directory' or recovery['phase'] != 'installed' or
                    Path(recovery['transaction']).resolve(strict=True) != transaction or
                    Path(recovery['plan']).resolve(strict=True) != path or recovery['plan_sha256'] != digest or
                    recovery['device_id'] != manifest['expected_storage_serial'] or
                    recovery['target_id'] != identity['target_id'] or
                    recovery['linnet_usb_serial'] != identity['linnet_usb_serial'] or
                    recovery['boot_build_id'] != manifest['image']['build_id'] or
                    recovery['firmware_backup_sha256'] != manifest['firmware_backup_sha256'] or
                    applied['operation'] != 'apply' or applied['passed'] is not True or
                    applied['after']['directory'] != 'candidate' or
                    applied['after']['original_payload_intact'] is not True or
                    Path(applied['plan']).resolve(strict=True) != transaction / 'native-plan/plan.json'):
                continue
            native_manifest, native_artifacts, _firmware = native.load_plan(
                applied['plan'], applied['reviewed_plan_sha256'])
            # Upgrade plans retain the native device.json; only the execution
            # serial differs in the Apple recovery descriptor. Verify all bytes.
            if (native_manifest == dict(manifest, expected_storage_serial=storage['disk_serial']) and
                    native_artifacts == artifacts):
                return True
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    return False


def _inputs(identity, transaction, path, digest, device_id, *, cache_scope=None):
    from tools.studio import installer

    transaction = Path(transaction).resolve(strict=True)
    path = Path(path).resolve(strict=True)
    original_identity = dict(identity, disk_serial=device_id)
    expected_scope = installer.cache_root(original_identity)
    scope = Path(cache_scope).resolve(strict=True) if cache_scope is not None else expected_scope
    if (scope.name != expected_scope.name or scope.parent.name != expected_scope.parent.name or
            not _directory_target(identity) or transaction.parent.parent != scope or
            path not in (transaction / 'native-plan/plan.json', transaction / 'apple-recovery-plan/plan.json')):
        raise ValueError('Native recovery record is outside this iPod’s original transaction')
    manifest, artifacts, firmware = native.load_plan(path, digest)
    saved = json.loads(artifacts['device.json'])
    storage = saved['storage']
    if (manifest['target_id'] != identity['target_id'] or
            saved['target_id'] != identity['target_id'] or
            manifest['expected_storage_serial'] != device_id or
            storage['disk_serial'] not in (device_id, identity['linnet_usb_serial']) or
            manifest['expected_linnet_usb_serial'] != identity['linnet_usb_serial'] or
            saved['linnet_usb_serial'] != identity['linnet_usb_serial'] or
            identity['disk_serial'] != (identity['linnet_usb_serial'] if eligible(identity) else device_id) or
            Path(manifest['firmware_backup']).resolve(strict=True) != transaction / 'backup/firmware.bin' or
            manifest['expected_device_bytes'] != identity['disk_bytes'] or
            manifest.get('sector_bytes', native.SECTOR) != identity['sector_bytes'] or
            any(storage[key] != identity[key] for key in ('disk_bytes', 'sector_bytes', 'partition_offset', 'partition_bytes'))):
        raise ValueError('Original firmware plan does not match this native iPod')
    # Bind the FAT serial through the original install receipt, independently
    # of Windows' mode-specific volume GUID.
    originals = []
    for receipt_path in transaction.glob('install-*.json'):
        if receipt_path.name.endswith('.result.json'):
            continue
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        original = receipt.get('identity', {})
        if (receipt.get('operation') == 'install-linnet' and
                original.get('disk_serial') == device_id and
                original.get('volume_unique_id') == storage.get('volume_id') and
                original.get('volume_serial') == identity['volume_serial'] and
                all(original.get(key) == identity[key] for key in
                    ('target_id', 'linnet_usb_serial', 'disk_bytes', 'sector_bytes', 'partition_offset', 'partition_bytes'))):
            originals.append(original)
    if not originals and not _upgrade_volume_identity(identity, transaction, path, digest, manifest, artifacts):
        raise ValueError('The original installation has no matching FAT volume identity')
    return manifest, artifacts, firmware


def _context(identity, linnet, transaction, *, cache_scope=None):
    from tools.studio import installer
    from tools.studio.storage import SCRATCH_BYTES
    from tools.studio.update_history import _read_record
    from tools.content.publish import _plain

    transaction = Path(transaction).resolve(strict=True)
    root = Path(identity['root']).resolve(strict=True)
    capacity = TARGETS[identity['target_id']]['os_capacity_bytes']
    for name in installer.CORE_FILES:
        path = root / '.LINNET' / name
        _plain(path, root)
        size = capacity if name.startswith('OS') else SCRATCH_BYTES if name == 'SCRATCH.BIN' else 512
        if not path.is_file() or path.is_symlink() or path.stat().st_size != size:
            raise ValueError('Installed system file has an invalid type or capacity: ' + name)
    states = [_read_record(root / '.LINNET' / name) for name in ('STATE0.BIN', 'STATE1.BIN')]
    if states[0][1] == states[1][1] and states[0][0] != states[1][0]:
        raise ValueError('Conflicting installed state records at the same generation')
    context = json.loads((transaction / 'context.json').read_text(encoding='utf-8'))
    if (context.get('target_id') != identity['target_id'] or
            context.get('strategy') != 'native-directory' or context.get('build') not in ('user', 'dev') or
            linnet.get('state') != 'installed' or linnet.get('candidate_pending')):
        raise ValueError('An interrupted native installation cannot be verified in its current state')
    installer._saved_previous_boot(context)
    method = installer._saved_method(context)
    if method is None or installer.INSTALLATION_METHODS[method]['strategy'] != 'native-directory':
        raise ValueError('The saved installation does not use native firmware recovery')
    artifacts = dict(context['artifacts'])
    if artifacts.get('installation_method') != method:
        raise ValueError('The saved firmware does not match the installation method')
    installer._validate_artifacts(identity, artifacts)
    path = transaction / 'native-plan/plan.json'
    digest = (transaction / 'native-plan.sha256').read_text(encoding='ascii').strip()
    manifest, plan_artifacts, _firmware = _inputs(identity, transaction, path, digest, context['device_id'],
                                               cache_scope=cache_scope)
    if (artifacts['build'] != context['build'] or linnet.get('build') != context['build'] or
            linnet.get('build_id') != artifacts['os_build_id'] or
            linnet.get('image_sha256') != installer.inspect_image(Path(artifacts['os_image']).read_bytes())['sha256'] or
            manifest['image']['build_id'] != artifacts['boot_build_id'] or
            Path(artifacts['boot_image']).read_bytes() != plan_artifacts['boot.los']):
        raise ValueError('Installed software differs from the saved native installation')
    return dict(context, artifacts=artifacts, transaction=str(transaction), installation_method=method,
                previous_boot=native.original_boot(manifest['original_osos'],_firmware),
                initialized_resume=True)


def pending(identity, linnet):
    from tools.studio import installer

    if not _directory_target(identity) or linnet.get('state') != 'installed':
        return None
    base = installer.cache_root(identity).parent
    matches = []
    for transaction in base.glob(identity['target_id'] + '-*/transactions/*'):
        try:
            matches.append(_context(identity, linnet, transaction))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    if len(matches) > 1:
        raise ValueError('Multiple original installations match this iPod; resolve their recovery records first')
    return matches[0] if matches else None


def available(identity, marker, *, cache_scope=None):
    if not eligible(identity) or not marker or marker.get('strategy') != 'native-directory':
        return False
    try:
        manifest, _artifacts, _firmware = _inputs(identity, marker['transaction'], marker['plan'],
                                                marker['plan_sha256'], marker['device_id'], cache_scope=cache_scope)
        return (manifest['image']['build_id'] == marker['boot_build_id'] and
                manifest['firmware_backup_sha256'] == marker['firmware_backup_sha256'])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def bind(identity, transaction, path, digest, device_id, *, cache_scope=None):
    """Derive an execution descriptor; retain original plan and every raw byte."""
    from tools.inspect.retail_device import detect
    from tools.studio import installer
    from tools.content.media_manager import refresh

    current = refresh(identity)
    installer._same_device(identity, current)
    manifest, artifacts, _firmware = _inputs(current, transaction, path, digest, device_id,
                                           cache_scope=cache_scope)
    detected = detect(current['root'])
    storage = detected['storage']
    if (detected['target_id'] != current['target_id'] or
            detected['linnet_usb_serial'] != current['linnet_usb_serial'] or
            storage['volume_id'] != current['volume_unique_id'] or
            any(storage[key] != current[key] for key in
                ('disk_number', 'disk_serial', 'disk_bytes', 'sector_bytes', 'partition_offset', 'partition_bytes'))):
        raise ValueError('Native storage changed before recovery binding')
    output = Path(transaction) / ('native-binding-' + uuid.uuid4().hex)
    output.mkdir(exist_ok=False)
    for name in native.ARTIFACTS:
        shutil.copyfile(Path(path).parent / name, output / name)
    native.save_json(output / 'device.json', detected)
    data = (output / 'device.json').read_bytes()
    entries = dict(manifest['artifacts'])
    entries['device.json'] = dict(bytes=len(data), sha256=native.sha(data))
    rebound = dict(manifest, expected_storage_serial=current['disk_serial'], artifacts=entries)
    native.save_json(output / 'plan.json', rebound)
    result_path = output / 'plan.json'
    result_hash = native.sha(result_path.read_bytes())
    _new, new_artifacts, _backup = native.load_plan(result_path, result_hash)
    if any(new_artifacts[name] != artifacts[name] for name in native.ARTIFACTS if name != 'device.json'):
        raise ValueError('Native binding changed a firmware recovery artifact')
    return result_path, result_hash


def finish(job):
    from tools.studio import installer

    identity = job['identity']
    root = Path(identity['root'])
    if (root / installer.JOURNAL).exists():
        raise ValueError('Installation recovery changed before verification')
    cache_scope = job['recovery_cache_root']
    context = _context(identity, installer.installed_state(root, identity['target_id']), job['transaction'],
                       cache_scope=cache_scope)
    if (context['artifacts'] != job['artifacts'] or context['installation_method'] != job['installation_method'] or
            installer._saved_previous_boot(context) != job['previous_boot']):
        raise ValueError('The saved installation changed before verification')
    transaction = Path(context['transaction'])
    path = transaction / 'native-plan/plan.json'
    digest = (transaction / 'native-plan.sha256').read_text(encoding='ascii').strip()
    bound_path, bound_sha = bind(identity, transaction, path, digest, context['device_id'], cache_scope=cache_scope)
    applied = native.execute('apply', bound_path, bound_sha, identity['disk_number'])
    if not applied.get('passed'):
        raise RuntimeError(applied.get('failure', 'Native installation verification failed'))
    # Initial provisioning must not overwrite metadata updated by a successful
    # first boot. Validate the current confirmed image again instead.
    _context(identity, installer.installed_state(root, identity['target_id']), transaction, cache_scope=cache_scope)
    apple = installer._install_apple_firmware(identity, path, digest)
    manifest, _artifacts, firmware = native.load_plan(path, digest)
    if identity['target_id'] == 'video5g-m25':
        from tools.studio.apple_firmware import publish_video_resources
        saved = json.loads(_artifacts['device.json'])
        parsed = native.parse_backup(firmware, _artifacts['mbr.bin'], saved)
        apple['video_resources'] = publish_video_resources(identity, firmware, parsed['entries'])
    selector = (installer._install_selector_config(identity)
                if context['installation_method'] == 'native-selector' else None)
    marker = installer._journal(dict(identity, disk_serial=context['device_id']), context['artifacts'],
        'native-directory', transaction, dict(plan=str(path), plan_sha256=digest,
        firmware_backup_sha256=manifest['firmware_backup_sha256'],
        installation_method=context['installation_method'], previous_boot=installer._saved_previous_boot(context),
        apple_firmware=apple, selector=selector))
    return dict(passed=True, strategy='native-directory', boot=applied, recovery=marker,
                requires_safe_eject=True, resumed_through_native_disk=True)
