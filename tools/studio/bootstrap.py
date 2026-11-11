"""Verified file-level bootstrap for an already installed file bootloader."""
import hashlib
import json
import os
import struct
from tools.inspect.image import inspect

def digest(data):
    return hashlib.sha256(data).hexdigest()

def ipod_payload(data, expected_model=None, checksum_seed=3):
    if len(data)<40:
        raise ValueError('truncated iPod image')
    checksum,model=struct.unpack_from('>I4s',data)
    payload=data[8:]
    if (expected_model is not None and model != expected_model) or checksum!=(checksum_seed+sum(payload))&0xffffffff:
        raise ValueError('wrong iPod model or wrapper checksum')
    return payload, model

def sync_write(path,data):
    with path.open('xb') as writer:
        writer.write(data);writer.flush();os.fsync(writer.fileno())
    if path.read_bytes()!=data:
        raise OSError('readback verification failed: '+str(path))

def state_write(path,state):
    from tools.content.publish import _plain
    temporary=path.with_suffix('.new')
    _plain(path,path.parent)
    _plain(temporary,path.parent)
    with temporary.open('w',encoding='utf-8',newline='\n') as writer:
        json.dump(state,writer,indent=2);writer.write('\n');writer.flush();os.fsync(writer.fileno())
    temporary.replace(path)

def install(mount, image, envelope, expected_original):
    from tools.content.media_manager import refresh
    from tools.content.publish import _exclusive
    from tools.studio.installer import identity_for_mount
    identity = identity_for_mount(mount)
    with _exclusive(identity):
        refresh(identity)
        return _install(mount, image, envelope, expected_original)

def _install(mount, image, envelope, expected_original):
    mount=mount.resolve(strict=True)
    from tools.content.publish import _plain
    los=envelope.read_bytes();identity=inspect(los)
    from tools.studio.targets import TARGETS
    install_spec = TARGETS[identity['target_id']]['installer']
    target=mount.joinpath(*install_spec['file_loader'].split('/'))
    _plain(target,mount)
    original=target.read_bytes()
    expected_model = install_spec['wrapper_model'].encode('ascii')
    checksum_seed = install_spec['wrapper_checksum_seed']
    _original_payload, original_model = ipod_payload(original, expected_model, checksum_seed)
    candidate=image.read_bytes();payload, candidate_model=ipod_payload(candidate, expected_model, checksum_seed)
    if payload!=los[128:] or candidate_model != original_model:
        raise ValueError('bootstrap requires matching Boot envelope and wrapper')
    report_path=image.with_suffix('.json')
    report=json.loads(report_path.read_text())
    if report.get('kind')!='boot' or report.get('build_id')!=identity['build_id']:
        raise ValueError('matching Boot build report is required')
    recovery=mount/'.LINNET/RECOVERY';_plain(recovery,mount);recovery.mkdir(parents=True,exist_ok=True)
    state_path=recovery/'BOOTSTRAP.JSON'
    backup=recovery/'ORIGINAL.IPOD'
    _plain(state_path,mount);_plain(backup,mount)
    expected_state=dict(target_id=identity['target_id'],build_id=identity['build_id'],
                        entry_mode='existing-file-bootloader',target=install_spec['file_loader'],
                        backup='.LINNET/RECOVERY/ORIGINAL.IPOD',original_sha256=expected_original,
                        candidate_sha256=digest(candidate))
    if state_path.exists():
        state=json.loads(state_path.read_text())
        if any(state.get(key)!=value for key,value in expected_state.items()) or state.get('phase') not in ('backup-verified','candidate-verified','installed','restored'):
            raise ValueError('existing bootstrap transaction differs; inspect or restore it')
        if not backup.is_file() or digest(backup.read_bytes())!=expected_original:
            raise ValueError('existing recovery backup differs; refusing to continue')
        active=digest(original)
        if active not in (expected_original,state['candidate_sha256']):
            raise ValueError('boot file changed during the saved transaction')
        if state['phase']=='installed' and active==state['candidate_sha256']:
            return state
        if state['phase']=='restored':
            if active!=expected_original:
                raise ValueError('restored boot file differs from the saved original')
            state['phase']='backup-verified';state_write(state_path,state)
    else:
        if digest(original)!=expected_original:
            raise ValueError('installed boot file differs from reviewed original hash')
        if backup.exists():
            if backup.read_bytes()!=original:
                raise ValueError('existing recovery backup differs; refusing to overwrite')
        else: sync_write(backup,original)
        state=dict(expected_state,phase='backup-verified')
        state_write(state_path,state)
    temporary=target.with_name('linnet-new.ipod')
    active=digest(target.read_bytes())
    if active != state['candidate_sha256']:
        if temporary.exists():
            if temporary.is_symlink() or digest(temporary.read_bytes())!=state['candidate_sha256']:
                raise ValueError('bootstrap staging file differs from the saved transaction')
        else:
            sync_write(temporary,candidate)
        state['phase']='candidate-verified';state_write(state_path,state)
        if digest(target.read_bytes())!=expected_original:
            raise ValueError('boot file changed during staging; original retained')
        temporary.replace(target)
    if digest(target.read_bytes())!=state['candidate_sha256']:
        raise OSError('published boot file failed readback; restore required')
    state['phase']='installed';state_write(state_path,state)
    return state

def status(mount):
    mount=mount.resolve(strict=True)
    from tools.content.publish import _plain
    _plain(mount/'.LINNET/RECOVERY/BOOTSTRAP.JSON',mount)
    state=json.loads((mount/'.LINNET/RECOVERY/BOOTSTRAP.JSON').read_text())
    # Paths in external journal are never used to select write destinations.
    if not isinstance(state.get('target_id'),str):
        raise ValueError('unsupported bootstrap journal')
    from tools.studio.targets import TARGETS
    if state['target_id'] not in TARGETS:
        raise ValueError('bootstrap target is no longer supported')
    relative=TARGETS[state['target_id']]['installer']['file_loader']
    if state.get('target')!=relative:
        raise ValueError('bootstrap journal target differs from the target manifest')
    target=mount.joinpath(*relative.split('/'));_plain(target,mount)
    _plain(mount/'.LINNET/RECOVERY/ORIGINAL.IPOD',mount)
    result=dict(state)
    result['active_sha256']=digest(target.read_bytes())
    result['backup_valid']=digest((mount/'.LINNET/RECOVERY/ORIGINAL.IPOD').read_bytes())==state['original_sha256']
    return result

def restore(mount):
    from tools.content.media_manager import refresh
    from tools.content.publish import _exclusive
    from tools.studio.installer import identity_for_mount
    identity = identity_for_mount(mount)
    with _exclusive(identity):
        refresh(identity)
        return _restore(mount)

def _restore(mount):
    mount=mount.resolve(strict=True);state=status(mount)
    from tools.content.publish import _plain
    _plain(mount/'.LINNET/RECOVERY/ORIGINAL.IPOD',mount)
    if not state['backup_valid']:
        raise ValueError('recovery backup hash mismatch')
    if state['active_sha256'] not in (state['original_sha256'],state['candidate_sha256']):
        raise ValueError('boot file was changed independently; refusing to overwrite')
    original=(mount/'.LINNET/RECOVERY/ORIGINAL.IPOD').read_bytes()
    from tools.studio.targets import TARGETS
    relative=TARGETS[state['target_id']]['installer']['file_loader']
    target=mount.joinpath(*relative.split('/'));_plain(target,mount)
    temporary=target.with_name('linnet-restore.ipod');_plain(temporary,mount)
    if temporary.exists():
        if not temporary.is_file() or temporary.stat().st_size > len(original):
            raise ValueError('restore staging differs from the saved original')
        staged=temporary.read_bytes()
        if staged != original:
            if not original.startswith(staged):
                raise ValueError('restore staging differs from the saved original')
            temporary.unlink()  # An interrupted write left only a verified prefix.
        elif state['active_sha256']==state['original_sha256']:
            temporary.unlink()  # A previous restore finished before cleanup.
    if state['active_sha256']!=state['original_sha256']:
        if not temporary.exists():
            sync_write(temporary,original)
        with temporary.open('r+b') as staged_file:
            os.fsync(staged_file.fileno())
        if digest(temporary.read_bytes())!=state['original_sha256']:
            raise OSError('restore staging failed verification')
        temporary.replace(target)
    with target.open('r+b') as restored_file:
        os.fsync(restored_file.fileno())
    if digest(target.read_bytes())!=state['original_sha256']:
        raise OSError('restored boot file failed verification')
    state['phase']='restored';state['active_sha256']=state['original_sha256']
    state_write(mount/'.LINNET/RECOVERY/BOOTSTRAP.JSON',state)
    return state
