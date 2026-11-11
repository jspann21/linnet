"""Media Manager's device-bound first installation for an Apple-mode Classic.

The only manual transition is entry into Apple's DFU mode. Once the temporary
monitor runs, Link owns diagnostics, backup, flash, and the native disk handoff.
All artifacts and receipts remain on the host if a stage cannot complete.
"""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import time
import traceback
import uuid

TARGET = 'classic6g-n25'
FORMAT = 'linnet-classic-setup-v1'


class Cancelled(RuntimeError):
    pass


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _scope(serial):
    from tools.studio.classic_recovery import _scope as recovery_scope
    return recovery_scope(serial).parent / 'setup'


def _load(path):
    from tools.link.classic_nor import _read_file
    value=json.loads(_read_file(path,1024*1024))
    if not isinstance(value,dict) or value.get('format')!=FORMAT:
        raise ValueError('Invalid Classic setup record')
    from tools.studio.classic_recovery import _serial
    _serial(value.get('serial'))
    if value.get('target_id')!=TARGET or value.get('build') not in ('user','dev'):
        raise ValueError('Invalid Classic setup identity')
    expected=_scope(value['serial']).resolve()
    if Path(path).resolve().parent.parent!=expected:
        raise ValueError('Classic setup record is outside its device directory')
    return value


def product_identity(serial):
    """Recover display metadata from the setup bound to this live logic board.

    Native USB exposes the board ID instead of Apple's product serial. Never
    guess the revision from that hexadecimal ID or from disk capacity.
    """
    from tools.inspect.ipod_models import identify
    from tools.studio.classic_recovery import _serial
    _serial(serial)
    candidates = []
    for path in _scope(serial).glob('*/setup.json'):
        try:
            state = _load(path)
            original = state['identity']
            if (state['serial'] != serial or original.get('linnet_usb_serial') != serial or
                    original.get('target_id') != TARGET):
                continue
            product = identify(product_serial=original.get('disk_serial'), target=TARGET)
            if product.get('model_identity_status') == 'identified':
                candidates.append(product)
        except (OSError, ValueError, KeyError, TypeError):
            continue
    if not candidates or len({item['model_number'] for item in candidates}) != 1:
        return None
    return dict(candidates[0], model_identity_source='saved-live-product-serial')


def attest_hardware(diagnostic, identity):
    """Record the observed hardware gates; model metadata never passes them."""
    from tools.studio.targets import TARGETS
    spec = TARGETS[TARGET]
    board = dict(line.split('=', 1) for line in diagnostic['board'].splitlines() if '=' in line)
    pmu = int(board.get('pmu_identity', '0'), 16)
    panel = int(board.get('panel_type', 'ffffffff'), 16)
    disk = diagnostic['disk']
    checks = dict(
        target=diagnostic['identity'].get('target_id') == identity['target_id'] == TARGET,
        ram=int(board.get('ram_ready', '0'), 16) == 1 and int(board.get('ram_failure', '1'), 16) == 0,
        pmu=(pmu >> 24) == 1 and (pmu & 0xffff) == 0x0312,
        panel=panel in (0, 1, 2, 3) and diagnostic['display'].get('frame_transfer_complete') is True,
        pata=disk.get('native_storage_compatible') is True,
        disk_capacity=disk.get('sectors', 0) * 512 >= identity['disk_bytes'],
        partition_table=disk.get('mbr_signature') == '55aa',
    )
    return dict(format='linnet-classic-hardware-attestation-v1', passed=all(checks.values()),
                checks=checks, target_id=TARGET, model_number=identity.get('model_number'),
                generation=identity.get('generation'), model_identity_source=identity.get('model_identity_source'),
                linnet_usb_serial=identity['linnet_usb_serial'],
                ram_bytes=spec['sdram_bytes'], panel_type=panel, pmu_version=pmu & 255,
                pmu_variant=(pmu >> 8) & 255, disk=disk,
                physical_output_verified=diagnostic['display'].get('physical_output_verified', False),
                nor=dict(status='pending', requirement='Verified JEDEC identity and two matching full original reads'))


def pending(identity=None):
    """Find unfinished host transactions; no device reads or writes."""
    from tools.studio.installer import cache_root
    if identity is not None and identity.get('target_id')!=TARGET:return []
    parent=cache_root(dict(target_id=TARGET,disk_serial='scope')).parent
    paths=(_scope(identity['linnet_usb_serial']).glob('*/setup.json') if identity is not None else
           parent.glob(TARGET+'-*/setup/*/setup.json'))
    found=[]
    for path in paths:
        try:
            state=_load(path)
            if state.get('phase') not in ('complete','cancelled'):
                found.append(dict(state,path=str(path),modified=path.stat().st_mtime_ns))
        except (OSError,ValueError,KeyError,TypeError):
            continue
    return sorted(found,key=lambda item:item['modified'],reverse=True)


def _artifacts(state):
    from tools.studio.installer import _validate_artifacts
    artifacts=state['artifacts']
    _validate_artifacts(state['identity'],artifacts)
    for name,digest in state['artifact_hashes'].items():
        if _sha(artifacts[name])!=digest:raise ValueError('Saved Classic firmware changed: '+name)
    return artifacts


def prepare(identity, *, firmware=None, from_source=False, build='user', progress=lambda _message:None):
    from tools.content.media_manager import refresh
    from tools.studio import installer
    from tools.link import s5l_bootstrap as link
    identity=refresh(identity)
    if identity['target_id']!=TARGET or identity.get('native_target_id')==TARGET:
        raise ValueError('First-time Classic setup requires the selected Apple-mode iPod')
    if installer.installed_state(identity['root'],TARGET)['state']!='absent':
        raise ValueError('Inspect the existing Linnet installation before changing Classic Boot')
    if pending(identity):raise ValueError('Resume the existing Classic installation before starting another')
    directory=_scope(identity['linnet_usb_serial'])/uuid.uuid4().hex
    directory.mkdir(parents=True,exist_ok=False)
    if from_source:
        progress('Building Linnet for the connected Classic…')
        artifacts=installer.build_artifacts(identity,progress,build,'native-preserve',boot_profile='release')
    else:
        if firmware is None:raise ValueError('Choose a Linnet firmware ZIP or build from source')
        from tools.studio.firmware_bundle import load
        progress('Checking the selected Classic firmware ZIP…')
        artifacts=load(firmware,identity,'native-preserve',build=build)
    persisted=installer._persist_artifacts(artifacts,directory)
    bootstrap=directory/'bootstrap'
    from tools.studio import s5l_bootstrap
    if from_source:s5l_bootstrap.build(identity['linnet_usb_serial'],bootstrap)
    else:s5l_bootstrap.bind(artifacts['bootstrap_template'],identity['linnet_usb_serial'],bootstrap)
    device=link.prepare(identity['root'],output=directory/'device.json',artifacts=bootstrap)
    state=dict(format=FORMAT,target_id=TARGET,serial=identity['linnet_usb_serial'],identity=identity,
               build=build,artifacts=persisted,phase='prepared',persistent_writes=False,
               device_plan=device['prepared'],device_plan_sha256=_sha(device['prepared']),
               artifact_hashes={name:_sha(persisted[name]) for name in
                   ('boot_image','boot_report','boot_wrapper','os_image','os_report')},
               bootstrap_hashes={name:_sha(bootstrap/name) for name in
                   ('manifest.json','monitor.bin','rom_transfer.bin','probe.bin','sram_bridge.bin','monitor.lnos','monitor-bridge.lnos')})
    installer._save(directory/'setup.json',state,durable=True)
    return directory/'setup.json'


def _mounted(state):
    from tools.content.media_manager import discover
    from tools.link.s5l_bootstrap import location
    from tools.studio.classic_recovery import _same_volume
    device=json.loads(Path(state['device_plan']).read_text())
    matches=[item for item in discover(detailed=True)['devices'] if item.get('native_target_id')==TARGET and
             item.get('linnet_usb_serial')==state['serial']]
    if len(matches)>1:raise ValueError('More than one native disk matches this Classic')
    if not matches:return None
    current=matches[0]
    if location(current['usb_pnp_id'])!=device['usb_location']:
        raise ValueError('The Classic reconnected at a different USB port')
    _same_volume(current,state['identity'])
    return current


def _wait_disk(state, progress):
    progress('Waiting for the Classic to reconnect automatically in Linnet Disk Mode…')
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        current=_mounted(state)
        if current:return current
        time.sleep(.5)
    raise RuntimeError('Linnet Boot was installed, but its disk did not reconnect. Keep this iPod connected and use Resume Classic installation.')


def _rebind_usb_location(state, path, usb_location, progress):
    """Move a pre-flash plan only after the board identity has been verified."""
    from tools.studio import installer
    device = json.loads(Path(state['device_plan']).read_text())
    if device['usb_location'] == usb_location:
        return
    if (state['persistent_writes'] or state.get('nor_plan') or
            device.get('target_id') != TARGET or device.get('serial') != state['serial'] or
            device.get('disk_serial') != state['identity']['disk_serial']):
        raise ValueError('The Classic moved to a different USB port after its flash plan was prepared')
    destination = Path(path).parent / ('device-' + uuid.uuid4().hex + '.json')
    installer._save(destination, dict(device, usb_location=usb_location), replace=False, durable=True)
    state.setdefault('usb_relocations', []).append(dict(previous_plan=state['device_plan'],
        previous_sha256=state['device_plan_sha256'], usb_location=usb_location))
    state.update(device_plan=str(destination), device_plan_sha256=_sha(destination))
    installer._save(path, state, durable=True)
    progress('iPod verified on the new USB port. Resuming installation…')


def run(path, *, progress=lambda _message:None, instruction=lambda _message:None,
        cancelled=lambda:False):
    """Run or explicitly resume the exact saved installation, with no write retries."""
    from tools.studio import installer, classic_recovery
    from tools.content.publish import _exclusive
    from tools.link import s5l_bootstrap as link, classic_nor as nor
    from tools.link.transport import SerialTransport
    from tools.link.campaign import connect
    from tools.link.eject import eject
    path=Path(path);state=_load(path)
    if state['phase']=='complete':raise ValueError('This Classic installation is already complete')
    if _sha(state['device_plan'])!=state['device_plan_sha256']:
        raise ValueError('Classic device plan changed after preparation')
    bootstrap=link.artifacts_directory(json.loads(Path(state['device_plan']).read_text()))
    for name,digest in state['bootstrap_hashes'].items():
        if _sha(bootstrap/name)!=digest:raise ValueError('Saved Classic bootstrap changed: '+name)
    artifacts=_artifacts(state)
    def save():installer._save(path,state,durable=True)
    def guide(message, action=None):
        instruction(dict(message=message,can_stop=not state['persistent_writes'],action=action))
    def check_cancel():
        if cancelled() and not state['persistent_writes']:
            state['phase']='cancelled';save();raise Cancelled('Classic setup stopped before bootloader installation. No flash was written.')
    # A board-level mutex survives all storage mode transitions. Volume writers
    # also participate in their usual volume-specific exclusion at publication.
    with ExitStack() as locks:
        locks.enter_context(_exclusive(dict(volume_unique_id='classic-setup:'+state['serial'])))
        native=None;disk_started=None
        try:
            native=_mounted(state) if state.get('nor_plan') else None
            if native is None:
                check_cancel()
                ports=[p for p in SerialTransport.discover() if p['vid']==0x1209 and p['pid']==0x504a and
                       p['serial_number']==state['serial']]
                if len(ports)>1:raise ValueError('Classic Link identity is ambiguous')
                if not ports:
                    from tools.content.media_manager import discover, refresh
                    retail=[item for item in discover(detailed=True)['devices'] if item.get('target_id')==TARGET and
                            item.get('linnet_usb_serial')==state['serial'] and not item.get('native_target_id')]
                    if len(retail)>1:raise ValueError('Classic Apple storage identity is ambiguous')
                    if retail:
                        current=refresh(retail[0])
                        classic_recovery._same_volume(state['identity'],current)
                        _rebind_usb_location(state, path, link.location(current['usb_pnp_id']), progress)
                        locks.enter_context(_exclusive(current))
                        progress('Safely ejecting the Classic before bootloader setup…')
                        eject(current['disk_number'],current['disk_serial'])
                    state['phase']='waiting-dfu';save()
                    guide('Turn the Hold switch off. Hold Menu + Select (the center button) together for about 12 seconds, '
                                'through the restart, until this screen tells you to release them. Keep USB connected.',action='hold')
                    deadline=time.monotonic()+180
                    device=json.loads(Path(state['device_plan']).read_text())
                    while True:
                        check_cancel()
                        matches=[]
                        for item in link.interfaces():
                            instance='\\'.join(item['path'][4:].split('#')[:3])
                            if link.location(instance)==device['usb_location']:matches.append(item)
                        if len(matches)>1:raise ValueError('Classic DFU identity is ambiguous')
                        if matches:
                            if matches[0]['pid']!=0x1223:raise ValueError('This Classic has an unsupported BootROM')
                            break
                        if time.monotonic()>=deadline:raise TimeoutError('DFU mode was not detected. Use Resume Classic installation to try the button step again.')
                        time.sleep(.2)
                    guide('Release Menu + Select now. Your iPod’s screen will stay black briefly. Media Manager will continue automatically.',action='release')
                    # A continued button hold resets DFU again after eight seconds.
                    # Allow release/reset to settle before loading or writing flash.
                    until=time.monotonic()+10
                    while time.monotonic()<until:check_cancel();time.sleep(.1)
                    progress('Loading temporary Linnet Link; your original bootloader is unchanged…')
                    link.load(state['device_plan'],progress=progress)
                else:
                    session,_,running=connect(serial_number=state['serial'],deadline_seconds=15)
                    session.close()
                    manifest=json.loads((link.artifacts_directory(json.loads(Path(state['device_plan']).read_text()))/'manifest.json').read_text())
                    if (running['target_id']!=TARGET or running['image_kind']!='boot' or
                            running['build_id']!=manifest['build_id'] or
                            running['image_sha256'] not in (_sha(bootstrap/'monitor.lnos'),'0'*64)):
                        raise ValueError('The connected Link image differs from this installation’s monitor')
                    _rebind_usb_location(state, path,
                        link.location('USB\\VID_1209&PID_504A\\' + state['serial']), progress)
                state['phase']='monitor';save()
                guide('Linnet Link is connected. Checking this iPod’s hardware and saving recovery. No more buttons are needed.')
                progress('Checking Classic RAM, display, disk identity, and partition layout…')
                diagnostic=link.diagnose(state['device_plan'],display=True,setup_screen=True)
                diagnostic_path=path.parent/'hardware.json'
                installer._save(diagnostic_path,diagnostic,durable=True)
                state['hardware']=str(diagnostic_path);save()
                attestation=attest_hardware(diagnostic,state['identity'])
                attestation_path=path.parent/'hardware-attestation.json'
                installer._save(attestation_path,attestation,durable=True)
                state['hardware_attestation']=str(attestation_path);save()
                if not attestation['passed']:
                    raise ValueError('This Classic did not pass its hardware checks; the original bootloader has not been changed')
                backup=Path(state.get('original_backup',path.parent/'original-nor.bin'))
                verified=False
                if backup.exists() or backup.with_suffix(backup.suffix+'.json').exists():
                    try:
                        nor._backup(backup,state['serial']);verified=True
                    except (OSError,ValueError):
                        # Never replace an artifact referenced by a flash plan.
                        # An incomplete read before planning gets a new name on
                        # explicit resume, retaining the failed read and receipt.
                        if state.get('nor_plan'):raise
                        backup=path.parent/('original-nor-'+uuid.uuid4().hex+'.bin')
                state['original_backup']=str(backup);save()
                if not verified:
                    progress('Saving and independently checking the complete original boot flash…')
                    link.backup_nor(state['device_plan'],backup)
                if not state.get('nor_plan'):
                    progress('Preparing the device-authenticated Linnet bootloader…')
                    # Keep an interrupted preparation's artifacts intact. A new
                    # attempt gets its own stem until its complete plan is saved.
                    planned=nor.prepare(state['device_plan'],backup,path.parent/('nor-'+uuid.uuid4().hex+'.json'),boot_image=artifacts['boot_image'])
                    state.update(nor_plan=planned['prepared'],nor_plan_sha256=planned['plan_sha256'])
                    save()
                original=nor._backup(backup,state['serial'])
                attestation['nor']=dict(status='original-and-plan-verified', jedec='bf258e',
                    bytes=len(original), original_sha256=_sha(backup), plan_sha256=state['nor_plan_sha256'])
                installer._save(attestation_path,attestation,durable=True)
                check_cancel()
                # From this point the guarded flash transaction must finish.
                state.update(phase='installing-boot',persistent_writes=True);save()
                guide('Installing and verifying Linnet Boot. Keep the iPod connected; Media Manager will restart it automatically.')
                nor.install(state['nor_plan'],state['nor_plan_sha256'],progress=progress,reboot_mode='disk')
                attestation['nor']['status']='installed-and-readback-verified'
                installer._save(attestation_path,attestation,durable=True)
                state['phase']='waiting-native-disk';save()
                native=_wait_disk(state,progress)
            locks.enter_context(_exclusive(native))
            from tools.content.media_manager import refresh
            native=refresh(native)
            disk_started=time.time()
            state['phase']='installing-os';save()
            guide('Linnet Disk Mode is connected. Installing the OS and preserving Apple startup and recovery files…')
            # Establish the directory before portable recovery discovery; a
            # completed host NOR record is authoritative only for this serial.
            from tools.content.guard import guarded_path, guard_volume
            system=Path(native['root'])/'.LINNET'
            guarded_path(system,Path(native['root']));guard_volume(native);system.mkdir(exist_ok=True)
            plan,_,_,_,_,_=nor.load_prepared(state['nor_plan'],state['nor_plan_sha256'])
            selected_record=classic_recovery._scope(state['serial'])/plan['result_sha256']/'NOR.JSON'
            classic_recovery.preserve(native,record_path=selected_record)
            nor.preserve_apple(state['device_plan'],state['original_backup'],native['root'],
                               prepared=state['nor_plan'],expected_plan_sha=state['nor_plan_sha256'])
            if installer._selector_rockbox(Path(native['root'])/'.rockbox/rockbox.ipod',
                                           installer.TARGETS[TARGET]):
                installer._install_selector_config(native)
            from tools.studio.native_update import publish
            if artifacts.get('game_library'):
                from tools.content import games
                games.save(native,games.read(native),library=artifacts['game_library'])
            existing=installer.installed_state(native['root'],TARGET)
            if existing['state']=='installed':
                if (existing.get('build_id')!=artifacts['os_build_id'] or existing.get('candidate_pending') or
                        existing.get('image_sha256')!=installer.inspect_image(Path(artifacts['os_image']).read_bytes())['sha256']):
                    raise ValueError('The installed OS differs from this saved Classic setup; inspect it before continuing')
                result=dict(passed=True,initial_installation=True,requires_safe_eject=True)
            else:
                result=publish(native,artifacts['os_image'])
            if not result.get('passed'):raise RuntimeError('OS installation did not verify')
            classic_recovery.preserve(native)
            state.update(phase='complete',result=result)
            state.pop('error',None);state.pop('error_traceback',None)
            state.pop('disk_error_evidence',None);save()
            guide('Linnet is installed. Open Preparation to add media, or choose Eject safely to start your iPod.')
            return dict(passed=True,reconnected_identity=native,summary='Linnet Boot and OS installed and verified. Add media in Preparation, or eject safely to start Linnet.',
                        setup_receipt=str(path),hardware=state.get('hardware'),
                        hardware_attestation=state.get('hardware_attestation'))
        except Exception as error:
            message=str(error)
            if isinstance(error,OSError) and disk_started is not None:
                from tools.studio.disk_errors import installation_error
                message,evidence=installation_error(error,native,disk_started)
                state.pop('disk_error_evidence',None)
                if evidence:state['disk_error_evidence']=evidence
            state['error']=message
            state['error_traceback']=traceback.format_exc();save()
            if message!=str(error):raise RuntimeError(message) from error
            raise
