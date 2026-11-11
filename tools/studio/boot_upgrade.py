"""Upgrade an installed native Boot while retaining its Apple recovery path."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import uuid

from tools.studio import native_bootstrap as native
from tools.studio.targets import TARGETS


def _eligible(review):
    identity = review['identity']
    marker = review.get('recovery') or {}
    return (identity.get('native_target_id') == identity['target_id'] and
            TARGETS[identity['target_id']]['installer']['boot_layout'] == 'ipod-directory' and
            review['linnet']['state'] == 'installed' and
            marker.get('strategy') == 'native-directory' and
            marker.get('phase') == 'installed' and
            marker.get('linnet_usb_serial') == identity.get('linnet_usb_serial') and
            not review['rockbox'].get('supersedes_linnet_recovery'))


def available(review):
    if not _eligible(review):
        return False
    try:
        _saved_plan(review)
        return True
    except (KeyError, OSError, ValueError, RuntimeError):
        return False


def pending(identity):
    from tools.studio import installer
    from tools.content.publish import _plain

    root = Path(identity['root']).resolve(strict=True)
    path = root / installer.JOURNAL
    _plain(path, root)
    if not path.is_file() or path.is_symlink() or path.stat().st_size > 65536:
        return None
    value = json.loads(path.read_text(encoding='utf-8'))
    if (value.get('phase') == 'boot-upgrade-pending' and
            value.get('target_id') == identity['target_id'] and
            value.get('linnet_usb_serial') == identity['linnet_usb_serial'] and
            isinstance(value.get('upgrade'), dict)):
        installer._saved_previous_boot(value)
        return value
    return None


def _durable_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as writer:
        writer.write(data)
        writer.flush()
        os.fsync(writer.fileno())
    if path.read_bytes() != data:
        raise OSError('Boot upgrade backup failed readback')


def _flush_recovery(transaction):
    """Persist every local recovery input before publishing the pending marker."""
    paths = [transaction / 'backup/firmware.bin', transaction / 'backup/mbr.bin',
             transaction / 'device.json']
    for directory in ('prior-plan-native', 'native-plan', 'apple-recovery-plan'):
        paths.extend(transaction / directory / name for name in (*native.ARTIFACTS, 'plan.json'))
    for path in paths:
        with path.open('r+b') as saved:
            os.fsync(saved.fileno())


def _saved_plan(review):
    from tools.studio import installer

    identity, marker = review['identity'], review['recovery']
    transaction = Path(marker['transaction']).resolve(strict=True)
    path = Path(marker['plan']).resolve(strict=True)
    original_identity = dict(identity, disk_serial=marker['device_id'])
    if (path not in (transaction / 'native-plan/plan.json',
                     transaction / 'apple-recovery-plan/plan.json') or
            transaction.parent.parent != installer.cache_root(original_identity)):
        raise ValueError('Saved Boot plan is outside this iPod’s recovery records')
    manifest, artifacts, firmware = native.load_plan(path, marker['plan_sha256'])
    saved_device = json.loads(artifacts['device.json'])['storage']
    if (manifest['target_id'] != identity['target_id'] or
            manifest['image']['build_id'] != marker['boot_build_id'] or
            manifest['expected_storage_serial'] != marker['device_id'] or
            manifest['expected_linnet_usb_serial'] != identity['linnet_usb_serial'] or
            manifest['expected_device_bytes'] != identity['disk_bytes'] or
            manifest.get('sector_bytes', native.SECTOR) != identity['sector_bytes'] or
            saved_device['partition_offset'] != identity['partition_offset'] or
            saved_device['partition_bytes'] != identity['partition_bytes'] or
            native.sha(firmware) != marker['firmware_backup_sha256']):
        raise ValueError('Saved Boot plan does not match this iPod')
    return path, marker['plan_sha256'], manifest, artifacts, firmware


def _rebind_plan(path, digest, output, *, identity, expected_serial, new_serial):
    """Copy an exact reviewed plan with only its live storage serial changed."""
    manifest, _artifacts, _backup = native.load_plan(path, digest)
    if (manifest['target_id'] != identity['target_id'] or
            manifest['expected_storage_serial'] != expected_serial or
            manifest['expected_linnet_usb_serial'] != identity['linnet_usb_serial'] or
            manifest['expected_device_bytes'] != identity['disk_bytes'] or
            manifest.get('sector_bytes', native.SECTOR) != identity['sector_bytes']):
        raise ValueError('Saved boot plan does not match this iPod')
    output.mkdir(parents=True, exist_ok=False)
    for name in native.ARTIFACTS:
        shutil.copyfile(Path(path).parent / name, output / name)
    rebound = dict(manifest, expected_storage_serial=new_serial)
    native.save_json(output / 'plan.json', rebound)
    result_path = output / 'plan.json'
    result_hash = native.sha(result_path.read_bytes())
    native.load_plan(result_path, result_hash)
    return result_path, result_hash


def _apple_recovery_plan(path, digest, output, *, identity, apple_serial):
    manifest, _artifacts, _backup = native.load_plan(path, digest)
    if (manifest['target_id'] != identity['target_id'] or
            manifest['expected_storage_serial'] != identity['disk_serial'] or
            manifest['expected_linnet_usb_serial'] != identity['linnet_usb_serial']):
        raise ValueError('New boot plan does not match this iPod')
    return _rebind_plan(path, digest, output, identity=identity,
                        expected_serial=identity['disk_serial'], new_serial=apple_serial)


def upgrade(identity, *, build=None, firmware=None, from_source=False,
            progress=lambda _message: None):
    from tools.studio import installer
    from tools.content.media_manager import refresh

    current = refresh(identity)
    resume = pending(current)
    if resume:
        transaction = Path(resume['upgrade']['transaction']).resolve(strict=True)
        result_path = transaction / ('upgrade-resume-' + uuid.uuid4().hex + '.result.json')
        job = dict(mode='upgrade-boot', identity=current, marker=resume,
                   transaction=str(transaction), result=str(result_path))
        job_path = transaction / ('worker-' + uuid.uuid4().hex + '.json')
        installer._save(job_path, job, replace=False)
        progress('Checking the interrupted startup upgrade…')
        return dict(passed=True, result=installer._run_elevated(job_path),
                    requires_safe_eject=True)
    reviewed = installer.assess(current, _validated_identity=True)
    from tools.studio.firmware_bundle import selected_build
    build = selected_build(firmware, build)
    if not available(reviewed):
        raise ValueError('This iPod does not have a verified native Boot upgrade path')
    if build not in ('user', 'dev') or (from_source and firmware is not None):
        raise ValueError('Choose a supported Linnet firmware source')
    method = reviewed['recovery'].get('installation_method')
    if method not in ('native-preserve', 'native-selector'):
        raise ValueError('The installed Boot method cannot be upgraded here')
    current = reviewed['identity']
    progress('Preparing the new startup firmware…')
    if from_source:
        artifacts = installer.build_artifacts(current, progress=progress, build=build,
                                              installation_method=method)
    else:
        if firmware is None:
            raise ValueError('Choose a Linnet firmware ZIP before upgrading Boot')
        from tools.studio.firmware_bundle import load
        artifacts = load(firmware, current, method, build=build)
    if installer.inspect_image(Path(artifacts['boot_image']).read_bytes()) == _saved_plan(reviewed)[2]['image']:
        raise ValueError('The selected firmware already has the installed startup selector')
    recovery_identity = dict(current, disk_serial=reviewed['recovery']['device_id'])
    transaction = installer._transaction(recovery_identity, artifacts)
    transaction.mkdir(parents=True, exist_ok=False)
    artifacts = installer._persist_artifacts(artifacts, transaction)
    installer._validate_artifacts(current, artifacts)
    result_path = transaction / ('upgrade-' + uuid.uuid4().hex + '.result.json')
    job = dict(mode='upgrade-boot', identity=current, marker=reviewed['recovery'],
               artifacts=artifacts, transaction=str(transaction), result=str(result_path))
    job_path = transaction / ('worker-' + uuid.uuid4().hex + '.json')
    installer._save(job_path, job, replace=False)
    progress('Requesting administrator access to upgrade the startup selector…')
    result = installer._run_elevated(job_path)
    return dict(passed=True, result=result, requires_safe_eject=True)


def _guard_plan(path, digest, disk_number):
    manifest, artifacts, firmware = native.load_plan(path, digest)
    native.identify_disk(disk_number, manifest)
    with open(r'\\.\PhysicalDrive' + str(disk_number), 'rb', buffering=0) as device:
        state = native.guard_device(device, manifest, artifacts, firmware)
    return manifest, artifacts, firmware, state


def _finish_pending(expected, pending_marker, transaction):
    from tools.studio import installer

    upgrade = pending_marker['upgrade']
    original_identity = dict(expected, disk_serial=pending_marker['device_id'])
    if (transaction.parent.parent != installer.cache_root(original_identity) or
            Path(upgrade['transaction']).resolve(strict=True) != transaction):
        raise ValueError('Pending Boot upgrade is outside this iPod’s recovery records')
    paths = {}
    for key, directory in (('prior', 'prior-plan-native'),
                           ('native', 'native-plan'), ('apple', 'apple-recovery-plan')):
        path = Path(upgrade[key + '_plan']).resolve(strict=True)
        if path != transaction / directory / 'plan.json':
            raise ValueError('Pending Boot upgrade plan path changed')
        paths[key] = (path, upgrade[key + '_sha256'])
    old, _old_files, _old_backup = native.load_plan(*paths['prior'])
    new, _new_files, _new_backup = native.load_plan(*paths['native'])
    apple, _apple_files, _apple_backup = native.load_plan(*paths['apple'])
    old_transaction = Path(pending_marker['transaction']).resolve(strict=True)
    prior_apple = (Path(pending_marker['plan']).resolve(strict=True),
                   pending_marker['plan_sha256'])
    if (prior_apple[0] not in (old_transaction / 'native-plan/plan.json',
                               old_transaction / 'apple-recovery-plan/plan.json') or
            old_transaction.parent.parent != installer.cache_root(original_identity)):
        raise ValueError('Original Apple recovery plan path changed')
    original, _original_files, _original_backup = native.load_plan(*prior_apple)
    if (old['expected_storage_serial'] != upgrade['native_serial'] or
            new['expected_storage_serial'] != upgrade['native_serial'] or
            apple['expected_storage_serial'] != pending_marker['device_id'] or
            original['expected_storage_serial'] != pending_marker['device_id'] or
            any(item['expected_linnet_usb_serial'] != expected['linnet_usb_serial'] or
                item['target_id'] != expected['target_id'] or
                item['expected_device_bytes'] != expected['disk_bytes'] for item in (old, new, apple, original)) or
            expected['disk_serial'] not in (upgrade['native_serial'], pending_marker['device_id']) or
            new['image']['build_id'] != upgrade['boot_build_id'] or
            new['image'] != apple['image'] or
            new['firmware_backup_sha256'] != apple['firmware_backup_sha256'] or
            original['image'] != old['image']):
        raise ValueError('Pending Boot upgrade plans differ from this iPod')
    live_prior = paths['prior'] if expected['disk_serial'] == upgrade['native_serial'] else prior_apple
    live_new = paths['native'] if expected['disk_serial'] == upgrade['native_serial'] else paths['apple']
    prior_state = None
    try:
        prior_state = _guard_plan(*live_prior, expected['disk_number'])[3]
    except RuntimeError:
        pass
    if prior_state is not None:
        restored = native.execute('restore', *live_prior, expected['disk_number'])
        if not restored.get('passed'):
            raise RuntimeError(restored.get('failure', 'Could not restore Apple startup'))
    else:
        restored = None
    try:
        _guard_plan(*live_new, expected['disk_number'])
    except RuntimeError as error:
        raise RuntimeError('Boot upgrade remains pending; inspect the raw write receipt before retrying') from error
    applied = native.execute('apply', *live_new, expected['disk_number'])
    if not applied.get('passed'):
        raise RuntimeError(applied.get('failure', 'Could not publish the new Boot'))
    updated = dict(pending_marker, phase='installed', installed_at=installer._now(),
                   boot_build_id=upgrade['boot_build_id'], startup_default_format=1,
                   transaction=str(transaction), plan=str(paths['apple'][0]),
                   plan_sha256=paths['apple'][1],
                   firmware_backup_sha256=new['firmware_backup_sha256'])
    updated.pop('upgrade', None)
    updated.pop('restored_at', None)
    installer._save(Path(expected['root']) / installer.JOURNAL, updated, durable=True)
    return dict(passed=True, restored=restored, applied=applied,
                recovery=updated, requires_safe_eject=True)


def worker(job):
    """Run inside the installer's finite elevated worker."""
    from tools.inspect.retail_device import detect
    from tools.studio import installer
    from tools.studio.apple_firmware import available as apple_available

    expected, marker = job['identity'], job['marker']
    transaction = Path(job['transaction']).resolve(strict=True)
    observed = detect(expected['root'])
    live = dict(target_id=observed['target_id'], disk_number=observed['storage']['disk_number'],
                disk_serial=observed['storage']['disk_serial'],
                disk_bytes=observed['storage']['disk_bytes'],
                partition_offset=observed['storage']['partition_offset'],
                partition_bytes=observed['storage']['partition_bytes'],
                linnet_usb_serial=observed['linnet_usb_serial'])
    installer._same_device(expected, live)
    root = Path(expected['root']).resolve(strict=True)
    if pending(expected):
        if pending(expected) != marker:
            raise RuntimeError('The pending Boot upgrade changed after review')
        return _finish_pending(expected, marker, transaction)
    review = installer.assess(expected)
    if not available(review) or review['recovery'] != marker:
        raise RuntimeError('The installed Boot changed after upgrade review')
    installer._validate_artifacts(expected, job['artifacts'])
    if not apple_available(root, expected['target_id']):
        raise ValueError('The preserved Apple firmware is unavailable')
    old_path, old_sha, old_manifest, old_artifacts, old_firmware = _saved_plan(review)
    rebound_path, rebound_sha = _rebind_plan(
        old_path, old_sha, transaction / 'prior-plan-native', identity=expected,
        expected_serial=marker['device_id'], new_serial=expected['disk_serial'])
    _manifest, _artifacts, _firmware, before = _guard_plan(
        rebound_path, rebound_sha, expected['disk_number'])
    if before.get('compatible_residue') or before['staging'] not in ('zero', 'original', 'candidate'):
        raise ValueError('Installed Boot contains an unsupported firmware residue')
    simulated = bytearray(old_firmware)
    if before['staging'] == 'candidate':
        start = old_manifest['candidate_partition_offset']
        simulated[start:start + old_manifest['candidate_write_bytes']] = old_artifacts['candidate.bin']
    backup = transaction / 'backup'
    firmware_path, mbr_path = backup / 'firmware.bin', backup / 'mbr.bin'
    _durable_bytes(firmware_path, bytes(simulated))
    _durable_bytes(mbr_path, old_artifacts['mbr.bin'])
    device_path = transaction / 'device.json'
    installer._save(device_path, observed, replace=False)
    planned = native.plan(firmware_path, mbr_path, device_path,
                          Path(job['artifacts']['boot_image']), transaction / 'native-plan')
    recovery_path, recovery_sha = _apple_recovery_plan(
        planned['plan'], planned['plan_sha256'], transaction / 'apple-recovery-plan',
        identity=expected, apple_serial=marker['device_id'])
    upgrade = dict(transaction=str(transaction),
                   native_serial=expected['disk_serial'],
                   prior_plan=str(rebound_path), prior_sha256=rebound_sha,
                   native_plan=planned['plan'], native_sha256=planned['plan_sha256'],
                   apple_plan=str(recovery_path), apple_sha256=recovery_sha,
                   boot_build_id=job['artifacts']['boot_build_id'])
    pending_marker = dict(marker, phase='boot-upgrade-pending', upgrade=upgrade)
    _flush_recovery(transaction)
    installer._save(root / installer.JOURNAL, pending_marker, durable=True)
    return _finish_pending(expected, pending_marker, transaction)
