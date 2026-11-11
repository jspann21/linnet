"""Measured storage scratch/state/fault campaign; no implicit OS installation."""
from datetime import datetime, timezone
import hashlib
import json
import random
import struct
import time
from tools.inspect.image import inspect, verify_device_report
from .campaign import connect, _run_directory, _save
from .identity import decode
from .protocol import Command
from .session import AmbiguousCommand, AmbiguousUpdateFinish, DeviceError

FIELDS=('magic','phase','error','files','prepared','sectors_low','sectors_high','ready',
        'quarantined','pending','completed','submitted','retries','failed','canceled',
        'coalesced','cache_hits','read_ahead_sectors','max_service_us','partition_lba',
        'partition_sectors','clusters','epoch','work_capacity')


def _wait_healthy_os(session, observed, expected, generation=None, deadline=None):
    """Refresh startup readiness on one session, with no mutation or reconnect."""
    bound = dict(target_id=expected['target_id'],
                 profile=expected['profile'], image_kind='os', build_id=expected['build_id'],
                 image_sha256=expected['sha256'],
                 generation=observed['generation'] if generation is None else generation)
    def check(identity):
        if any(identity.get(key) != value for key, value in bound.items()):
            raise RuntimeError('OS identity changed or differs from campaign image during readiness wait')
    check(observed)
    if deadline is None:deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        current=decode(session.status())
        check(current)
        if current.get('healthy') is True and current.get('health_flags')==31:return current
        time.sleep(min(.05,max(0,deadline-time.monotonic())))
    raise TimeoutError('OS did not establish all startup health conditions before deadline')


def _finalize_receipt(session, receipt, path):
    """Persist operation evidence even when transport cleanup fails."""
    close_error=None
    try:
        if session:session.close()
    except Exception as exc:
        close_error=exc
        receipt['close_failure']=f'{type(exc).__name__}: {exc}'
        receipt['automated_passed']=False
    receipt['finished_at']=datetime.now(timezone.utc).isoformat()
    try:
        _save(path,receipt)
    except Exception as exc:
        context='; '.join(f'{key}: {receipt[key]}' for key in
                          ('failure','restore_failure','close_failure') if key in receipt)
        raise RuntimeError(f'final receipt save failed at {path}: {type(exc).__name__}: {exc}' +
                           ('; '+context if context else '')) from exc
    if close_error is not None and 'failure' not in receipt:
        raise RuntimeError(f"transport close failed; outcome saved at {path}: {receipt['close_failure']}") from close_error


def cycle(root, image_path, scenario_path, port, progress=None):
    image=image_path.read_bytes(); identity=inspect(image)
    scenario_raw=scenario_path.read_bytes(); suite=json.loads(scenario_raw)
    required={'target_id','scratch_bytes','random_reads','seed','fault_modes'}
    if (not isinstance(suite,dict) or set(suite)!=required or
            identity['profile']=='release' or suite.get('target_id')!=identity['target_id']):
        raise ValueError('storage campaign requires a matching development image and scenario')
    for key,low,high in [('scratch_bytes',65536,0x400000),('random_reads',32,4096),('seed',0,0xffffffff)]:
        if type(suite.get(key)) is not int or not low<=suite[key]<=high:
            raise ValueError('invalid storage scenario '+key)
    faults=suite.get('fault_modes',[1,2,3,4,5])
    if faults!=[1,2,3,4,5]:
        raise ValueError('storage campaign requires all five ordered fault modes')
    run_id,directory=_run_directory(root,identity)
    receipt=dict(run_id=run_id,image=identity,scenario=suite,
        scenario_sha256=hashlib.sha256(scenario_raw).hexdigest(),steps=[],automated_passed=False,settings_restored=False,device_write_attempted=False,
        settings_restoration=dict(attempted=False,last_attempt_verified=False,last_attempt_uncertain=False),
        started_at=datetime.now(timezone.utc).isoformat(),
        pending_gates=['reset-at-every-metadata-publication-stage','persistent-candidate-success-and-crash-rollback',
                       'host-USB-mount-write-sync-eject-reconnect'])
    receipt_path=directory/'receipt.json';_save(receipt_path,receipt)
    session=None; original=None; changed=False; generation=None; serial=None; admitted=False
    def record(name,**details):
        event=dict(name=name,**details);receipt['steps'].append(event);_save(receipt_path,receipt)
        if progress:progress(dict(run_id=run_id,**event))
    def check_identity(observed):
        nonlocal admitted
        admitted=False
        current=_wait_healthy_os(session,observed,identity,generation=generation)
        admitted=True
        return current
    def mounted():
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            state=dict(zip(FIELDS,session.storage_state()))
            if state['prepared'] and state['ready'] and not state['quarantined']:
                return state
            if state['phase']==9:raise RuntimeError('storage mount failed: '+str(state))
            time.sleep(.05)
        raise TimeoutError('storage did not become prepared before deadline')
    try:
        session,port_info,observed=connect(port=port);observed=check_identity(observed)
        verify_device_report(image_path,identity,port_info['serial_number'],observed['target_id'])
        generation=observed['generation'];serial=port_info['serial_number']
        session.prepare_storage(generation)
        record('before',identity=observed,state=mounted())
        original=session.settings_get(generation);(directory/'settings-original.bin').write_bytes(original)
        n=suite['scratch_bytes']; seed=suite['seed']
        pattern=b''.join(hashlib.sha256(f'{seed}:{i}'.encode()).digest() for i in range((n+31)//32))[:n]
        receipt['device_write_attempted']=True;_save(receipt_path,receipt)
        for offset in range(0,n,1008):
            session.scratch_write(generation,offset,pattern[offset:offset+1008])
            if offset%64512==0:record('scratch-progress',bytes=min(offset+1008,n))
        actual=hashlib.sha256()
        for offset in range(0,n,1008):actual.update(session.scratch_read(offset,min(1008,n-offset)))
        expected=hashlib.sha256(pattern).hexdigest()
        if actual.hexdigest()!=expected:raise RuntimeError('full scratch readback hash mismatch')
        record('scratch-full-readback',bytes=n,sha256=expected)
        rng=random.Random(seed)
        for i in range(suite['random_reads']):
            if i%2: length=rng.randint(1,1008);offset=rng.randrange(0,n-length+1)
            else: length=512;offset=rng.randrange(n//512)*512
            if session.scratch_read(offset,length)!=pattern[offset:offset+length]:
                raise RuntimeError(f'random scratch read mismatch at{offset}+{length}')
        record('random-readback',count=suite['random_reads'],seed=seed)
        replacement=b'Linnet storage check '+hashlib.sha256(pattern).digest()
        changed=True;session.settings_put(generation,replacement)
        session.close();session=None
        session,_,observed=connect(serial_number=serial);observed=check_identity(observed)
        if session.settings_get(generation)!=replacement:raise RuntimeError('settings did not survive transport reconnect')
        receipt['settings_restoration'].update(attempted=True,last_attempt_verified=False,last_attempt_uncertain=True)
        session.settings_put(generation,original)
        if session.settings_get(generation)!=original:raise RuntimeError('settings restoration mismatch')
        receipt['settings_restoration'].update(last_attempt_verified=True,last_attempt_uncertain=False)
        changed=False;receipt['settings_restored']=True;record('settings-publish-and-restore',original_bytes=len(original))
        work=mounted()
        created=False
        if not work['files']&128:
            session.file_resize(generation,1,8192);work=mounted()
            created=True
            if work['work_capacity']!=8192:raise RuntimeError('WORK creation capacity mismatch')
            for offset in range(0,8192,1008):session.work_write(generation,offset,pattern[offset:min(offset+1008,8192)])
        old_size=work['work_capacity']
        if old_size>0x100000-8192:raise RuntimeError('WORK lacks space within1MiB campaign growth bound')
        previous=b''.join(session.work_read(offset,min(1008,old_size-offset)) for offset in range(0,old_size,1008))
        if created and previous!=pattern[:8192]:raise RuntimeError('new WORK file write/readback mismatch')
        new_size=old_size+8192
        session.file_resize(generation,2,new_size);grown=mounted()
        if grown['work_capacity']!=new_size:raise RuntimeError('WORK growth capacity mismatch')
        expected_bytes=previous+bytes(8192)
        for offset in range(0,new_size,1008):
            length=min(1008,new_size-offset)
            expected=expected_bytes[offset:offset+length]
            if session.work_read(offset,length)!=expected:raise RuntimeError('WORK growth changed old data or exposed nonzero extension')
        record('file-create-grow',old_bytes=old_size,new_bytes=new_size,previous_sha256=hashlib.sha256(previous).hexdigest(),zero_extension_bytes=8192)
        for mode in faults:
            before=dict(zip(FIELDS,session.storage_state()))
            session.storage_fault(generation,mode)
            rejected=False
            try:
                # Writes bypass read cache; mode5 is retained until its flush.
                session.scratch_write(generation,0,bytes([mode])*512)
            except (DeviceError,AmbiguousCommand):rejected=True
            if not rejected:raise RuntimeError(f'injected fault{mode} failed to reject scratch transaction')
            session.close();session=None
            session,_,observed=connect(serial_number=serial);observed=check_identity(observed)
            failed=dict(zip(FIELDS,session.storage_state()))
            counter='canceled' if mode==4 else 'failed'
            if failed[counter]<=before[counter]:raise RuntimeError('fault did not increment '+counter+' counter')
            session.storage_recover(generation);recovered=mounted()
            # New explicit write after recovery establishes known scratch data.
            session.scratch_write(generation,0,pattern[:512])
            if session.scratch_read(0,512)!=pattern[:512]:raise RuntimeError('scratch failed after recovery')
            record('injected-fault-recovery',mode=mode,before=before,failed=failed,recovered=recovered)
        observed=check_identity(observed)
        record('after',state=mounted(),update_status=list(session.update_status()))
        receipt['automated_passed']=True
    except (ValueError,RuntimeError,OSError) as exc:
        receipt['failure']=f'{type(exc).__name__}: {exc}'
    finally:
        if changed and original is not None:
            if session and admitted and not session.poisoned:
                try:
                    receipt['settings_restoration'].update(attempted=True,last_attempt_verified=False,last_attempt_uncertain=True)
                    session.settings_put(generation,original)
                    receipt['settings_restored']=session.settings_get(generation)==original
                    receipt['settings_restoration'].update(last_attempt_verified=receipt['settings_restored'],
                        last_attempt_uncertain=not receipt['settings_restored'])
                except (ValueError,RuntimeError,OSError) as exc:receipt['restore_failure']=str(exc)
            if not receipt['settings_restored']:
                receipt['settings_restore_pending']=str(directory/'settings-original.bin')
        _finalize_receipt(session,receipt,receipt_path)
    return receipt


def update_cycle(root, image_path, crash_path, port, progress=None):
    """Install good/bad candidates and reset at each settings publication boundary."""
    from .identity import decode_crash
    good=image_path.read_bytes();bad=crash_path.read_bytes()
    identity=inspect(good);crash_identity=inspect(bad)
    if any(i['profile']=='release' for i in (identity,crash_identity)) or identity['target_id']!=crash_identity['target_id']:
        raise ValueError('update campaign requires matching development OS images')
    good_sha=hashlib.sha256(good).hexdigest();bad_sha=hashlib.sha256(bad).hexdigest()
    if good_sha==bad_sha:raise ValueError('crash image must differ from healthy image')
    run_id,directory=_run_directory(root,identity);path=directory/'receipt.json'
    receipt=dict(run_id=run_id,campaign='persistent-update',image=identity,crash_image=crash_identity,
        good_sha256=good_sha,crash_sha256=bad_sha,steps=[],automated_passed=False,
        device_write_attempted=False,settings_restored=False,started_at=datetime.now(timezone.utc).isoformat(),
        settings_restoration=dict(attempted=False,last_attempt_verified=False,last_attempt_uncertain=False))
    _save(path,receipt);session=None;original=None;observed=None;serial=None
    def record(name,**details):
        event=dict(name=name,**details);receipt['steps'].append(event);_save(path,receipt)
        if progress:progress(dict(run_id=run_id,**event))
    def ready():
        deadline=time.monotonic()+120
        settled=None
        while time.monotonic()<deadline:
            state=session.storage_state()
            if state[4] and state[7] and not state[8] and not state[9]:
                # Healthy startup can precede the asynchronous music scan.
                # Wait before admitting a mutation; never replay a rejection.
                if observed['image_kind']=='os':
                    library=session.request(Command.LIBRARY_STATE)
                    if struct.unpack_from('<I',library,8)[0] in (1,2,5):
                        settled=None;time.sleep(.05);continue
                    if (not session.storage_performance()['idle_endpoint'] or
                            session.storage_schedule_diagnostics()['last_state']):
                        settled=None;time.sleep(.05);continue
                update=session.update_status()
                if (update[1] and update[2] and not update[7] and not update[11] and not update[14] and
                        (not state[2] and not update[15])):
                    # Health flags precede persistent confirmation and the
                    # remaining startup readers. Require settled storage too.
                    if settled is None:settled=time.monotonic()
                    if time.monotonic()-settled>=1:
                        return update
                    time.sleep(.05);continue
            settled=None
            time.sleep(.05)
        raise TimeoutError('persistent metadata unavailable')
    def attempt_baseline(update):
        if update[8]==0xffffffff:
            raise RuntimeError("saturated boot attempt counter cannot prove another reset")
        return update
    def reset_ready(before):
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            update=ready()
            if update[8]>before[8] and update[6]==255 and not update[7]:return update
            time.sleep(.05)
        raise TimeoutError("publication reset lacks a newer settled boot attempt")
    def reconnect(expected_kind,expected_sha=None):
        nonlocal session,observed
        deadline=time.monotonic()+120
        classic_recovery_requested=False
        while time.monotonic()<deadline:
            try:session,_,observed=connect(serial_number=serial,deadline_seconds=min(15,max(.1,deadline-time.monotonic())))
            except TimeoutError:continue
            if expected_kind=='os' and observed['image_kind']=='os':
                observed=_wait_healthy_os(session,observed,identity,deadline=deadline)
                return
            if (expected_kind=='boot' and identity['target_id']=='classic6g-n25' and
                    observed['image_kind']=='os'):
                # Native Classic Boot automatically returns to its confirmed
                # OS after rejecting a crashed candidate. The retained crash
                # and slot/attempt checks below still have to prove rollback.
                observed=_wait_healthy_os(session,observed,identity,deadline=deadline)
                return
            if (expected_kind=='os' and identity['target_id']=='classic6g-n25' and
                    observed['target_id']==identity['target_id'] and observed['image_kind']=='boot' and
                    not classic_recovery_requested):
                # An intentional publication cut leaves Classic in its SRAM
                # recovery monitor. Admit one explicit restart only after its
                # identity matches the verified installed NOR recovery record.
                from tools.studio.media_link import _classic_record
                from tools.link.classic_nor import _operation
                if not _classic_record(dict(serial=serial,identity=observed)):
                    raise RuntimeError('Classic cut returned an unverified recovery monitor')
                record('classic-publication-recovery',identity=observed)
                classic_recovery_requested=True
                _operation(session,66,0x52454254,size=0)
                session.close();session=None
                continue
            if (observed['image_kind']==expected_kind and (expected_kind!='boot' or observed['generation']>0) and
                (expected_sha is None or observed['image_sha256']==expected_sha)):
                return
            session.close();session=None;time.sleep(.1)
        raise TimeoutError('expected persistent identity did not reconnect: '+str(observed))
    def restart():
        nonlocal session
        # A timeout is inspected through reconnect, never resent.
        attempt_baseline(ready())
        try:session.reboot_persistent(observed['generation'])
        except AmbiguousCommand:record('restart-ack-uncertain')
        finally:session.close();session=None
    try:
        session,endpoint,observed=connect(port=port);serial=endpoint['serial_number']
        verify_device_report(image_path,identity,serial,observed['target_id'])
        verify_device_report(crash_path,crash_identity,serial,observed['target_id'])
        observed=_wait_healthy_os(session,observed,identity)
        before=attempt_baseline(ready());original=session.settings_get(observed['generation'])
        (directory/'settings-original.bin').write_bytes(original)
        record('before',identity=observed,update=list(before))
        receipt['device_write_attempted']=True;_save(path,receipt)
        session.persistent_install(observed['generation'],good,image_path=image_path)
        offered=session.update_status()
        if offered[6]!=(before[5]^1) or offered[5]!=before[5]:raise RuntimeError('candidate publication changed confirmed slot')
        record('good-candidate-published',update=list(offered));restart();reconnect('os',good_sha)
        confirmed=ready();deadline=time.monotonic()+120
        while (confirmed[6]!=255 or confirmed[7]) and time.monotonic()<deadline:
            time.sleep(.05);confirmed=session.update_status()
        if confirmed[5]!=offered[6] or confirmed[6]!=255 or confirmed[7]:raise RuntimeError('healthy candidate was not confirmed')
        record('good-candidate-confirmed',identity=observed,update=list(confirmed))
        attempt_baseline(confirmed)
        session.persistent_install(observed['generation'],bad,image_path=crash_path)
        bad_offer=session.update_status();record('crash-candidate-published',update=list(bad_offer))
        restart();reconnect('boot');rollback=ready()
        crash=decode_crash(session.crash_get())
        if (crash['build_id']!=crash_identity['build_id'] or rollback[5]!=confirmed[5] or
                rollback[6]!=255 or rollback[7] or rollback[8]<=confirmed[8]):
            raise RuntimeError('crash candidate rollback identity/state mismatch')
        record('crash-rollback-observed',crash=crash,identity=observed,update=list(rollback),
               recovery_endpoint=observed['image_kind'])
        # Fresh explicit recovery action, after inspecting the crashed candidate.
        restart();reconnect('os',good_sha);ready()
        record('last-known-good-boot',identity=observed)
        for stage in range(1,5):
            before_cut=attempt_baseline(ready())
            old=session.settings_get(observed['generation'])
            candidate=b'publication-cut-'+bytes([stage])+hashlib.sha256(old).digest()
            session.publication_fault(observed['generation'],stage)
            interrupted=False
            try:session.settings_put(observed['generation'],candidate)
            except AmbiguousCommand:interrupted=True
            if not interrupted:raise RuntimeError('publication fault did not interrupt acknowledgement')
            session.close();session=None;reconnect('os',good_sha);after_cut=reset_ready(before_cut)
            actual=session.settings_get(observed['generation'])
            if actual not in (old,candidate):raise RuntimeError('publication reset exposed a torn settings record')
            if stage==1 and actual!=old:raise RuntimeError('before-write reset unexpectedly published new settings')
            if stage>=3 and actual!=candidate:raise RuntimeError('durably flushed settings were lost after reset')
            record('publication-reset',stage=stage,result='old' if actual==old else 'new',identity=observed,
                   before_attempt=before_cut[8],after_attempt=after_cut[8])
            receipt['settings_restoration'].update(attempted=True,last_attempt_verified=False,last_attempt_uncertain=True)
            session.settings_put(observed['generation'],original)
            if session.settings_get(observed['generation'])!=original:raise RuntimeError('settings restoration mismatch')
            receipt['settings_restoration'].update(last_attempt_verified=True,last_attempt_uncertain=False)
        for stage in range(1,5):
            before_cut=attempt_baseline(ready())
            interrupted=False
            try:session.persistent_install(observed['generation'],good,publication_stage=stage,image_path=image_path)
            except AmbiguousUpdateFinish:interrupted=True
            if not interrupted:raise RuntimeError('candidate publication cut did not interrupt acknowledgement')
            session.close();session=None;reconnect('os',good_sha);after_cut=reset_ready(before_cut)
            if after_cut[6]!=255 or after_cut[7] or after_cut[5] not in (before_cut[5],before_cut[5]^1):
                raise RuntimeError('candidate publication reset left unresolved slot state')
            if stage==1 and after_cut[5]!=before_cut[5]:
                raise RuntimeError('unpublished candidate became confirmed')
            if stage>=3 and after_cut[5]!=(before_cut[5]^1):
                raise RuntimeError('durably published candidate was lost')
            record('candidate-publication-reset',stage=stage,before=list(before_cut),after=list(after_cut),identity=observed)
        receipt['settings_restored']=True;receipt['automated_passed']=True
    except (ValueError,RuntimeError,OSError) as exc:receipt['failure']=f'{type(exc).__name__}: {exc}'
    finally:
        if original is not None and not receipt['settings_restored']:
            receipt['settings_restore_pending']=str(directory/'settings-original.bin')
        _finalize_receipt(session,receipt,path)
    return receipt
