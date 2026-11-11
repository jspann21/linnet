"""Load Linnet's SRAM monitor through the S5L8702 BootROM USB entry.

DFU writes are confined to bootstrap SRAM and verified before execution.
The native monitor initializes external RAM and supports verified RAM updates.
NOR and disk contents are not written by these commands.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
import zlib

from tools.link.apple_dfu import AppleDFU, interfaces
from tools.studio.targets import ROOT


def location(instance):
    quoted = "'" + instance.replace("'", "''") + "'"
    script = ("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
              "ConvertTo-Json -Compress -InputObject @((Get-PnpDeviceProperty -InstanceId " + quoted +
              " -KeyName DEVPKEY_Device_LocationPaths).Data)")
    result = subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',script],
                            capture_output=True,text=True,encoding='utf-8',timeout=15,check=True,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    paths=json.loads(result.stdout)
    if not paths or not all(isinstance(x,str) and x for x in paths):
        raise ValueError('Physical USB location unavailable')
    return sorted(x.upper() for x in paths)


def artifacts_directory(plan):
    return Path(plan.get('bootstrap_artifacts', ROOT/'out/dev/classic6g-n25/bootstrap'))


def prepare(mount, *, output=None, artifacts=None):
    from tools.inspect.retail_device import detect
    device=detect(mount)
    if device['target_id']!='classic6g-n25':raise ValueError('Expected a live Classic in Disk Mode')
    usb=device.get('retail_usb') or device.get('native_usb')
    if not usb:raise ValueError('Expected a Classic USB Disk Mode endpoint')
    usb_location=location(usb['pnp_id'])
    destination=Path(output) if output is not None else ROOT/'out/classic6g-n25/bootstrap-device.json'
    disk_serial=device['device_id']
    if 'native_usb' in device and destination.exists():
        previous=json.loads(destination.read_text())
        if (previous.get('target_id')==device['target_id'] and
            previous.get('serial')==device['linnet_usb_serial'] and
            sorted(previous.get('usb_location',[]))==usb_location):
            disk_serial=previous.get('disk_serial') or disk_serial
    plan=dict(target_id=device['target_id'],serial=device['linnet_usb_serial'],
              disk_serial=disk_serial,usb_location=usb_location)
    if artifacts is not None:plan['bootstrap_artifacts']=str(Path(artifacts).resolve())
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_text(json.dumps(plan,indent=2)+'\n')
    return dict(plan,prepared=str(destination))


def update(prepared):
    """Enter or replace the SRAM monitor through ordinary Linnet Link."""
    from tools.inspect.image import pack
    from tools.link.transport import SerialTransport
    from tools.link.campaign import connect
    plan=json.loads(Path(prepared).read_text())
    base=artifacts_directory(plan)
    report=json.loads((base/'manifest.json').read_text())
    payload=(base/'monitor.bin').read_bytes();image=(base/'monitor.lnos').read_bytes()
    bridge=(base/'monitor-bridge.lnos').read_bytes()
    if (plan.get('target_id')!='classic6g-n25' or report.get('target_id')!=plan['target_id'] or
        report.get('serial')!=plan['serial'] or hashlib.sha256(payload).hexdigest()!=report['monitor_sha256'] or
        image!=pack(payload,'dev',report['build_id'],0x22000000,0x22000000,'classic6g-n25') or
        hashlib.sha256(bridge).hexdigest()!=report.get('bridge_sha256')):
        raise ValueError('Bootstrap update identity/hash mismatch')
    ports=[p for p in SerialTransport.discover() if p['vid']==0x1209 and p['pid']==0x504a and p['serial_number']==plan['serial']]
    if len(ports)!=1:raise RuntimeError('Expected one Linnet Link endpoint for the recorded Classic')
    session=None
    receipt=dict(target_id=plan['target_id'],serial=plan['serial'],build_id=report['build_id'],
                 persistent_writes=False,phase='opening')
    destination=base/'update-receipt.json'
    def record():destination.write_text(json.dumps(receipt,indent=2)+'\n')
    record()
    try:
        session,_,before=connect(serial_number=plan['serial'],deadline_seconds=15)
        if before['target_id']!=plan['target_id'] or before['capabilities']&3!=3:
            raise RuntimeError('Running monitor does not support verified RAM updates')
        for stage in range(2):
            transfer=image if before['image_kind']=='boot' else bridge
            receipt.update(phase='uploading',previous_generation=before['generation'],stage=stage+1);record()
            # The monitor package is bound to the reviewed device plan above;
            # it has no ordinary OS build report.
            session._upload_unchecked(transfer)
            digest=hashlib.sha256(transfer).digest()
            if session.verify()!=digest:raise RuntimeError('Device upload digest mismatch')
            receipt['phase']='handoff-requested';record()
            session.boot_ram();session.close();session=None
            time.sleep(.3)
            session, _, after=connect(serial_number=plan['serial'],deadline_seconds=20)
            if (after['target_id']==plan['target_id'] and after['build_id']==report['build_id'] and
                after['image_sha256']==digest.hex() and after['generation']==before['generation']+1):
                receipt.update(phase='link-verified',identity=after);record();return receipt
            # The OS bridge can enter the previously installed NOR monitor.
            # Upload the verified new monitor from that boot endpoint once.
            if (stage==0 and before['image_kind']!='boot' and after['target_id']==plan['target_id'] and
                after['image_kind']=='boot' and after['capabilities']&3==3):
                before=after
                continue
            raise RuntimeError('RAM update did not return the expected image and generation')
    except Exception as error:
        receipt['error']=str(error);record();raise
    finally:
        if session is not None:session.close()


def backup_nor(prepared, output):
    """Preserve the entire identified flash, then compare a second device read."""
    from tools.link.transport import SerialTransport
    from tools.link.session import Session
    from tools.link.identity import decode
    from tools.link.protocol import Command
    plan=json.loads(Path(prepared).read_text())
    if plan.get('target_id')!='classic6g-n25':raise ValueError('Expected a Classic bootstrap plan')
    output=Path(output).resolve()
    receipt_path=output.with_suffix(output.suffix+'.json')
    if output.exists() or receipt_path.exists():raise ValueError('Backup destination already exists')
    output.parent.mkdir(parents=True,exist_ok=True)
    session=Session(SerialTransport.reconnect(plan['serial'],timeout=10))
    receipt=dict(target_id=plan['target_id'],serial=plan['serial'],persistent_writes=False,verified=False)
    try:
        identity=decode(session.hello())
        if identity['target_id']!=plan['target_id'] or identity['image_kind']!='boot':
            raise RuntimeError('Expected the Classic SRAM monitor')
        receipt['identity']=identity
        def read(operation, argument, size):
            raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',operation,argument))
            if len(raw)!=12+size or struct.unpack_from('<III',raw)!=(0x4252504c,operation,1):
                raise RuntimeError(f'Flash read failed at {argument:#x}')
            return raw[12:]
        jedec=read(32,0,3)
        # Microchip SST25VF080B DS20005045D: BF/25/8E is 8 Mbit.
        if jedec!=bytes.fromhex('bf258e'):raise ValueError(f'Unsupported NOR identity {jedec.hex()}')
        receipt.update(jedec=jedec.hex(),part='SST25VF080B',bytes=1048576,path=str(output))
        digest=hashlib.sha256()
        with output.open('xb') as destination:
            for offset in range(0,receipt['bytes'],512):
                block=read(33,offset,512);destination.write(block);digest.update(block)
        receipt['sha256']=digest.hexdigest()
        with output.open('rb') as original:
            for offset in range(0,receipt['bytes'],512):
                if read(33,offset,512)!=original.read(512):
                    raise RuntimeError(f'Independent NOR read differs at {offset:#x}')
        if hashlib.sha256(output.read_bytes()).hexdigest()!=receipt['sha256']:
            raise RuntimeError('Saved NOR backup digest changed')
        receipt['verified']=True
        return receipt
    except Exception as error:
        receipt['error']=str(error);raise
    finally:
        session.close()
        with receipt_path.open('x') as destination:json.dump(receipt,destination,indent=2)


def diagnose(prepared, *, display=False, setup_screen=False):
    """Read board status, ATA identity and sector zero through native Link."""
    if setup_screen and not display:
        raise ValueError('The setup screen requires a display check')
    from tools.link.transport import SerialTransport
    from tools.link.session import Session
    from tools.link.identity import decode
    from tools.link.protocol import Command
    plan=json.loads(Path(prepared).read_text())
    if plan.get('target_id')!='classic6g-n25':raise ValueError('Expected a Classic bootstrap plan')
    ports=[p for p in SerialTransport.discover() if p['vid']==0x1209 and p['pid']==0x504a and p['serial_number']==plan['serial']]
    if len(ports)!=1:raise RuntimeError('Expected one Linnet Link endpoint for the recorded Classic')
    session=Session(SerialTransport.reconnect(plan['serial'],timeout=10))
    try:
        identity=decode(session.hello())
        if identity['target_id']!=plan['target_id'] or identity['image_kind']!='boot':
            raise RuntimeError('Expected the Classic SRAM monitor')
        result=dict(identity=identity,persistent_writes=False,
                    board=session.request(Command.READ_LOG).decode('ascii'))
        if display and setup_screen:
            # HELLO presents the monitor's Linnet setup screen before replying.
            # Verify that transfer without resetting the panel or overwriting
            # the user's setup screen with an engineering test pattern.
            raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',22,0))
            deadline=time.monotonic()+20
            while True:
                if len(raw)!=24:raise RuntimeError('Invalid setup display progress response')
                magic,op,ok,busy,completed,crc=struct.unpack('<6I',raw)
                if magic!=0x4252504c or op!=22 or ok!=1:
                    raise RuntimeError('Setup display transfer failed')
                if not busy:
                    if not completed:raise RuntimeError('Setup display did not complete a frame')
                    break
                if time.monotonic()>=deadline:raise TimeoutError('Setup display deadline expired')
                raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',22,0))
            result['display']=dict(screen='setup',frame_transfer_complete=True,
                                   frame_crc32=f'{crc:08x}',physical_output_verified=False)
        elif display:
            raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',8,0))
            if len(raw)!=20 or struct.unpack_from('<3I',raw)!=(0x4252504c,8,1):
                raise RuntimeError('LCD command interface did not respond')
            result['display']=dict(read_id=raw[16:].hex(),controller_status=struct.unpack_from('<I',raw,12)[0])
            raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',9,0))
            deadline=time.monotonic()+20
            while True:
                if len(raw)!=12:raise RuntimeError('Invalid panel progress response')
                magic,op,phase=struct.unpack('<3I',raw)
                if magic!=0x4252504c or op not in (9,10):raise RuntimeError('Invalid panel progress marker')
                if phase==3:break
                # The 16-bit panel variants use phases 6–10 for their power
                # sequence before joining the shared frame transfer (phase 2).
                if phase not in (1,2,5,6,7,8,9,10):raise RuntimeError(f'Panel startup failed: {phase:#x}')
                if time.monotonic()>=deadline:raise TimeoutError('Panel startup deadline expired')
                raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',10,0))
            result['display'].update(frame_transfer_complete=True,physical_output_verified=False)
        def operation(number,lba=0):
            raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',number,lba))
            deadline=time.monotonic()+18
            while True:
                if len(raw)<24:raise RuntimeError('Truncated bootstrap diagnostic')
                magic,op,phase,error,words,status=struct.unpack_from('<6I',raw)
                if magic!=0x4252504c or op not in (number,3):raise RuntimeError('Invalid bootstrap diagnostic')
                if error:raise RuntimeError(f'ATA probe failed: phase={phase} error={error} status={status:#x}')
                if phase==8:
                    if words!=256 or len(raw)!=536:raise RuntimeError('Incomplete ATA data')
                    return raw[24:]
                if time.monotonic()>=deadline:raise TimeoutError('ATA diagnostic deadline expired')
                time.sleep(.02)
                raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',3,0))
        raw=operation(2)
        words=struct.unpack('<256H',raw)
        def ata_string(first,count):
            return b''.join(struct.pack('>H',word) for word in words[first:first+count]).decode('ascii',errors='replace').strip()
        lba48=(words[83]&0xc400)==0x4400
        sectors=sum(words[100+n]<<(16*n) for n in range(4)) if lba48 else words[60]|words[61]<<16
        model,firmware=ata_string(27,20),ata_string(23,4)
        iflash=(model=='iFlash-Platform iPod Adapters' and firmware=='iFlash85' and (words[83]&0xc004)==0x4004)
        logical_512=not ((words[106]&0xd000)==0x5000 and (words[117]|words[118]<<16)!=256)
        flush=(words[83]&0xd000)==0x5000
        compatible=bool((words[49]&512 or iflash) and logical_512 and flush and 0<sectors<=1<<48)
        result['disk']=dict(model=model,serial=ata_string(10,10),firmware=firmware,
                            sectors=sectors,lba48=lba48,native_storage_compatible=compatible,
                            logical_512=logical_512,can_flush=flush,identify_sha256=hashlib.sha256(raw).hexdigest())
        sector=operation(4)
        result['disk'].update(mbr_signature=sector[510:512].hex(),sector0_sha256=hashlib.sha256(sector).hexdigest(),
            partitions=[dict(type=sector[446+n*16+4],start_lba=struct.unpack_from('<I',sector,454+n*16)[0],
                             sectors=struct.unpack_from('<I',sector,458+n*16)[0]) for n in range(4)])
        base=artifacts_directory(plan)
        (base/'link-diagnostics.json').write_text(json.dumps(result,indent=2)+'\n')
        return result
    finally:session.close()


def _execute(dfu,stub,operation,address=0,length=0,data=b''):
    if len(data)>512 or operation not in (0,1,2):raise ValueError('Invalid BootROM operation')
    if operation==0 and not (0x20000000<=address<=0x2000ffc0 or 0x22000000<=address<=0x22017fc0
                            or address in (0x3c500000,0x3c500040)):
        raise ValueError('Read outside BootROM or bootstrap SRAM')
    if operation==1 and not (length==len(data) and 0<length<=512 and 0x22000000<=address<address+length<=0x22018000):
        raise ValueError('Write outside bootstrap SRAM')
    parameters=struct.pack('<III',operation,address,length)+data
    payload=parameters.ljust(0x288,b'\0')+stub
    if len(payload)>1024:raise ValueError('BootROM USB payload exceeds its buffer')
    dfu.clear()
    dfu.control(0x21,1,data=payload)
    dfu.clear()
    dfu.control(0xa1,2,length=64)
    return dfu.control(0xa0,0xfe,0xeaff,6,length=64)


def load(prepared, *, progress=print, wait_seconds=0, inspect_only=False, probe_operation=None, after_reset=False):
    if not 0 <= wait_seconds <= 300:
        raise ValueError('DFU wait must be between zero and 300 seconds')
    if probe_operation not in (None,0,1,2) or (inspect_only and probe_operation is not None):
        raise ValueError('Invalid board probe operation')
    plan=json.loads(Path(prepared).read_text())
    if plan.get('target_id')!='classic6g-n25':raise ValueError('Invalid bootstrap device plan')
    base=artifacts_directory(plan)
    report=json.loads((base/'manifest.json').read_text())
    kind='monitor' if probe_operation is None else 'probe'
    payload=(base/f'{kind}.bin').read_bytes();stub=(base/'rom_transfer.bin').read_bytes()
    if (report.get('target_id')!=plan['target_id'] or report.get('serial')!=plan['serial'] or
            hashlib.sha256(payload).hexdigest()!=report[f'{kind}_sha256'] or
            hashlib.sha256(stub).hexdigest()!=report['transfer_sha256']):
        raise ValueError('Bootstrap artifact identity/hash mismatch')
    matches=[]
    deadline=time.monotonic()+wait_seconds
    if after_reset:
        if not wait_seconds:raise ValueError('Waiting for reset requires --wait-seconds')
        while interfaces():
            if time.monotonic()>=deadline:raise TimeoutError('Device did not leave DFU before the reset deadline')
            time.sleep(.1)
    while True:
        for item in interfaces():
            instance='\\'.join(item['path'][4:].split('#')[:3])
            if location(instance)==plan['usb_location']:matches.append(item)
        if matches or time.monotonic()>=deadline:break
        time.sleep(.1)
    if len(matches)!=1:raise RuntimeError('Expected one Apple DFU device at the recorded Classic USB location')
    if matches[0]['pid']!=0x1223:raise ValueError('This bootstrap currently requires S5L8702 DFU PID 1223')
    receipt=dict(target_id=plan['target_id'],serial=plan['serial'],build_id=report['build_id'],
                 phase='opening',written_bytes=0,verified_bytes=0,persistent_writes=False)
    receipt_path=base/('inspect-receipt.json' if inspect_only else f'{kind}-receipt.json')
    def record():receipt_path.write_text(json.dumps(receipt,indent=2)+'\n')
    record()
    try:
        with AppleDFU(matches[0]['path']) as dfu:
            probe=_execute(dfu,stub,0,0x20000000)
            if probe==bytes(64) or probe==b'\xff'*64:raise RuntimeError('BootROM entry did not return plausible ROM bytes')
            receipt['bootrom_vector_sha256']=hashlib.sha256(probe).hexdigest()
            (base/'bootrom-vectors.bin').write_bytes(probe)
            receipt['previous_usb_trace']=list(struct.unpack('<32I',
                _execute(dfu,stub,0,0x22017f00)+_execute(dfu,stub,0,0x22017f40)))
            receipt['bootrom_registers']={}
            for address in (0x3c500000,0x3c500040):
                receipt['bootrom_registers'][f'{address:08x}']=list(struct.unpack('<16I',_execute(dfu,stub,0,address)))
            if inspect_only:
                receipt['phase']='inspected';record();return receipt
            receipt['phase']='loading';record()
            for offset in range(0,len(payload),512):
                part=payload[offset:offset+512]
                returned=_execute(dfu,stub,1,0x22000000+offset,len(part),part)
                receipt['written_bytes']=offset+len(part);record()
                if returned[:min(64,len(part))]!=part[:64]:raise RuntimeError('Bootstrap write/readback mismatch')
                progress(f'Bootstrap SRAM: {offset+len(part)}/{len(payload)} bytes')
            receipt['phase']='verifying';record()
            for offset in range(0,len(payload),64):
                expected=payload[offset:offset+64]
                returned=_execute(dfu,stub,0,0x22000000+offset)
                if returned[:len(expected)]!=expected:raise RuntimeError(f'Bootstrap verification failed at {offset:#x}')
                receipt['verified_bytes']=offset+len(expected)
            receipt['phase']='execution-requested';record()
            if probe_operation is not None:
                returned=_execute(dfu,stub,2,data=struct.pack('<I',probe_operation))
                words=struct.unpack('<16I',returned)
                if words[0]!=0x4252504c or words[1]!=probe_operation or words[15]!=0x454e4f44:
                    raise RuntimeError('Board probe did not return its completion markers')
                if probe_operation==2:
                    deadline=time.monotonic()+18
                    while words[3]<8 and not words[4] and time.monotonic()<deadline:
                        time.sleep(.02)
                        words=struct.unpack('<16I',_execute(dfu,stub,2,data=struct.pack('<I',3)))
                        if words[0]!=0x4252504c or words[1]!=3 or words[15]!=0x454e4f44:
                            raise RuntimeError('Storage probe lost completion markers')
                    if words[3]==8 and not words[4] and words[5]==256:
                        raw=b''.join(_execute(dfu,stub,0,words[7]+offset) for offset in range(0,512,64))
                        identify=struct.unpack('<256H',raw)
                        def ata_string(first,count):
                            return b''.join(struct.pack('>H',word) for word in identify[first:first+count]).decode('ascii',errors='replace').strip()
                        receipt['disk']=dict(model=ata_string(27,20),serial=ata_string(10,10),firmware=ata_string(23,4),
                            lba28_sectors=identify[60]|identify[61]<<16,lba48=bool(identify[83]&1024),
                            identify_sha256=hashlib.sha256(raw).hexdigest())
                        (base/'ata-identify.bin').write_bytes(raw)
                        words=struct.unpack('<16I',_execute(dfu,stub,2,data=struct.pack('<I',4)))
                        deadline=time.monotonic()+4
                        while words[3]<8 and not words[4] and time.monotonic()<deadline:
                            time.sleep(.01)
                            words=struct.unpack('<16I',_execute(dfu,stub,2,data=struct.pack('<I',3)))
                            if words[0]!=0x4252504c or words[1]!=3 or words[15]!=0x454e4f44:
                                raise RuntimeError('Sector read lost completion markers')
                        if words[3]==8 and not words[4] and words[5]==256:
                            sector=b''.join(_execute(dfu,stub,0,words[7]+offset) for offset in range(0,512,64))
                            (base/'ata-sector0.bin').write_bytes(sector)
                            receipt['disk']['sector0_sha256']=hashlib.sha256(sector).hexdigest()
                            receipt['disk']['mbr_signature']=sector[510:512].hex()
                            receipt['disk']['partitions']=[dict(type=sector[446+n*16+4],start_lba=struct.unpack_from('<I',sector,454+n*16)[0],sectors=struct.unpack_from('<I',sector,458+n*16)[0]) for n in range(4)]
                receipt.update(phase='probe-returned',probe_operation=probe_operation,result=list(words))
                record();return receipt
            # A DFU RAM launch must enter the monitor itself. An old SRAM boot
            # request can otherwise chainload the installed NOR image before
            # Link is available for bring-up or recovery.
            prior=_execute(dfu,stub,0,0x22017b00)
            receipt['previous_boot_request']=prior[:12].hex()
            request=struct.pack('<II',0x54424e4c,0)
            request+=struct.pack('<I',zlib.crc32(request))
            written=_execute(dfu,stub,1,0x22017b00,len(request),request)
            if written[:len(request)]!=request or _execute(dfu,stub,0,0x22017b00)[:len(request)]!=request:
                raise RuntimeError('Recovery boot request did not verify in SRAM')
            receipt['recovery_boot_request_verified']=True;record()
            try:_execute(dfu,stub,2)
            except (OSError,TimeoutError) as error:receipt['handoff_transport']=str(error)
        # A DFU disconnect is not evidence of a successful boot. Confirm Link.
        from tools.link.transport import SerialTransport
        from tools.link.session import Session
        from tools.link.identity import decode
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            ports=[p for p in SerialTransport.discover() if p['vid']==0x1209 and p['pid']==0x504a and p['serial_number']==plan['serial']]
            if len(ports)==1:
                session=Session(SerialTransport.reconnect(plan['serial'],timeout=10))
                try:identity=decode(session.hello())
                finally:session.close()
                if identity['target_id']!=plan['target_id'] or identity['build_id']!=report['build_id']:
                    raise RuntimeError('Unexpected image on bootstrap Link')
                receipt.update(phase='link-verified',port=ports[0]['port'],identity=identity);record();return receipt
            time.sleep(.25)
        raise TimeoutError('Setup connection failed. Connect this iPod directly to the computer, '
                           'then choose Resume Classic installation. Boot flash was not changed.')
    except Exception as error:
        receipt['error']=str(error);record();raise
