"""Device-bound preparation and guarded installation of the N25 NOR monitor."""
import hashlib
import json
from pathlib import Path
import struct
import time

from tools.link.identity import decode
from tools.link.protocol import Command
from tools.link.session import Session
from tools.link.transport import SerialTransport
from tools.studio.targets import ROOT

TARGET='classic6g-n25'
START,END=0x8000,0x28000
ARCHIVE=0x17000
ARCHIVE_SECTOR=ARCHIVE&~0xfff

# Plaintext Apple NORBOOT SHA-1 prefixes used by Rockbox's Classic dualboot
# installer (utils/mks5lboot/dualboot/dualboot.c). A relocated image is admitted
# only with one of these identities and device authentication before flashing.
APPLE_SHA1 = frozenset(bytes.fromhex(value) for value in (
    '31d17a3a4b05e4096b58310ab76c2f88', '7cef11d13a96ba1c1c734291f3fcbf20',
    '91e7210dfa4d19f8061eac96c70d4113', '666676dc1d32b246a6c97d5a61d3494c',
    '14c54c28486604252fb6d717952c97ce', '1ef0d9dec27eec027c1576bb5c4f2d95',
    '9c84ada46b38c325b12e32a1d1336d2d', '0685df28e4d7f482c073b05326fcb0fe',
    '60807d33a8def849bbbe0145ff624019'))


def backup_layout(data):
    """Recognize Apple or Rockbox dualboot without searching arbitrary flash."""
    if len(data) != 1048576:
        raise ValueError('Expected a complete Classic NOR snapshot')
    if data[START:START+8] == b'87021.0\x01' and struct.unpack_from('<II', data, START+8) == (0, 129024):
        return dict(previous_boot='apple', apple_offset=START, apple_plaintext=False)
    entry, size = struct.unpack_from('<II', data, START+8)
    if (data[START:START+8] != b'87021.0\x02' or entry or
            not 40 <= size <= 129024 or size % 16):
        raise ValueError('Unsupported Classic bootloader layout; expected Apple or Rockbox dualboot')
    apple = START + ((2048 + size + 4095) & ~4095)
    payload = data[START+2048:START+2048+size]
    if (b'Rockbox' not in payload or apple+2048+129024 > len(data)-512 or
            data[apple:apple+8] != b'87021.0\x02' or
            struct.unpack_from('<II', data, apple+8) != (0, 129024) or
            hashlib.sha1(data[apple+2048:apple+2048+129024]).digest()[:16] not in APPLE_SHA1):
        raise ValueError('Rockbox bootloader needs its intact, recognized Apple dualboot image')
    return dict(previous_boot='rockbox', apple_offset=apple, apple_plaintext=True)


def _sha(data):return hashlib.sha256(data).hexdigest()


def _read_file(path, maximum, *, exact=None):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError('Invalid or oversized NOR recovery artifact')
    data = path.read_bytes()
    if len(data) > maximum or exact is not None and len(data) != exact:
        raise ValueError('NOR recovery artifact size differs')
    return data


def _backup(path, serial):
    path=Path(path);data=_read_file(path,1048576,exact=1048576)
    receipt=json.loads(_read_file(path.with_suffix(path.suffix+'.json'),65536))
    if (not isinstance(receipt,dict) or len(data)!=1048576 or receipt.get('target_id')!=TARGET or receipt.get('serial')!=serial or
        receipt.get('verified') is not True or receipt.get('sha256')!=_sha(data)):
        raise ValueError('Expected a verified complete NOR backup for this device')
    backup_layout(data)
    return data


def preserve_apple(device_plan, backup, mount, *, prepared=None, expected_plan_sha=None):
    """Copy the exact original N25 IM3 body to Linnet's verified fallback files."""
    device=json.loads(Path(device_plan).read_text())
    if device.get('target_id')!=TARGET or not device.get('serial'):
        raise ValueError('Expected an N25 device plan')
    root=Path(mount).resolve(strict=True)
    from tools.content.media_manager import discover
    matches=[item for item in discover() if Path(item['root']).resolve()==root]
    if len(matches)!=1:
        raise ValueError('Mounted iPod identity differs from the reviewed N25 device plan')
    mounted=matches[0]
    # Linnet's native disk mode intentionally exposes the immutable logic-board
    # serial as both the USB and USBSTOR serial.  Retail disk mode exposes the
    # underlying drive serial saved in the bootstrap plan instead.  Bind either
    # representation to the same target, logic-board serial, and physical port.
    from tools.link.s5l_bootstrap import location
    disk_serial=mounted.get('disk_serial')
    valid_disk_serial=disk_serial in (device.get('disk_serial'),device['serial'])
    if (mounted.get('target_id')!=TARGET or
        mounted.get('linnet_usb_serial')!=device['serial'] or not valid_disk_serial or
        location(mounted.get('usb_pnp_id',''))!=sorted(device.get('usb_location',[]))):
        raise ValueError('Mounted iPod identity differs from the reviewed N25 device plan')
    original=_backup(backup,device['serial'])
    layout=backup_layout(original)
    at=layout['apple_offset']+2048
    payload=original[at:at+129024]
    if layout['apple_plaintext']:
        if prepared is None or expected_plan_sha is None:
            raise ValueError('Rockbox Apple fallback requires its authenticated NOR plan')
        plan,saved,_,_,_,_=load_prepared(prepared,expected_plan_sha)
        if saved!=original or plan['serial']!=device['serial']:
            raise ValueError('Apple fallback plan differs from this backup or device')
        payload=_read_file(plan['apple_payload_path'],129024,exact=129024)
        if _sha(payload)!=plan['apple_payload_sha256']:
            raise ValueError('Prepared Apple fallback digest differs')
    from tools.studio.apple_firmware import publish
    result=publish(root,TARGET,payload,_sha(payload))
    result.update(serial=device['serial'],source_nor_sha256=_sha(original))
    from tools.studio import classic_recovery
    if classic_recovery.find(mounted):
        result['recovery'] = classic_recovery.preserve(mounted)
    return result


def _connect(plan):
    if plan.get('target_id')!=TARGET:raise ValueError('Expected an N25 device plan')
    session=Session(SerialTransport.reconnect(plan['serial'],timeout=15))
    try:
        identity=decode(session.hello())
        if identity['target_id']!=TARGET or identity['image_kind']!='boot':
            raise ValueError('Expected the N25 SRAM monitor')
        return session,identity
    except Exception:session.close();raise


def _operation(session, operation, argument=0, data=b'', size=None):
    raw=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',operation,argument)+data,
                        timeout=5 if operation==52 else None)
    if len(raw)<12 or struct.unpack_from('<III',raw)!=(0x4252504c,operation,1) or (size is not None and len(raw)!=12+size):
        raise RuntimeError(f'NOR operation {operation} failed: {raw[:16].hex()}')
    return raw[12:]


def _sign(session, data):
    digest=hashlib.sha1(data).digest()[:16]
    signature=_operation(session,49,2,digest,16)
    if _operation(session,50,2,signature,16)!=digest:raise RuntimeError('Boot authentication round trip failed')
    return signature


def _authenticate_backup(session, data):
    layout=backup_layout(data)
    for offset in dict.fromkeys((START,layout['apple_offset'])):
        if _sign(session,data[offset:offset+64])!=data[offset+64:offset+80]:
            raise ValueError('Original NOR authentication does not match this device')
        if data[offset+7]==2:
            size=struct.unpack_from('<I',data,offset+12)[0]
            if _sign(session,data[offset+2048:offset+2048+size])!=data[offset+16:offset+32]:
                raise ValueError('Original plaintext NOR body authentication failed')
    return layout


def _encrypt_apple(session, payload):
    """Keep the legacy encrypted Apple fallback format, with continuous CBC.

    Each bounded hardware AES request starts at IV zero. Chaining its first
    block with the preceding ciphertext preserves the original full stream.
    Verify every block by independently decrypting it before retaining bytes.
    """
    result=bytearray();previous=bytes(16)
    for offset in range(0,len(payload),512):
        plain=payload[offset:offset+512]
        chained=bytes(a^b for a,b in zip(plain[:16],previous))+plain[16:]
        encrypted=_operation(session,49,2,chained,len(chained))
        if _operation(session,50,2,encrypted,len(encrypted))!=chained:
            raise ValueError('Apple fallback encryption round trip failed')
        result.extend(encrypted);previous=encrypted[-16:]
    return bytes(result)


def _read(session):
    if _operation(session,32,size=3)!=bytes.fromhex('bf258e'):raise ValueError('Unexpected NOR part')
    return b''.join(_operation(session,33,offset,size=512) for offset in range(0,1048576,512))


def _validate_base(session, current, original):
    if len(current)!=len(original) or current[:START]!=original[:START] or current[END:]!=original[END:]:
        raise ValueError('NOR contents outside the boot slot differ from the original backup')
    if current==original:return
    image=current[START:END]
    entry,size=struct.unpack_from('<II',image,8)
    if image[:8]!=b'87021.0\x02' or entry or not size or size%16 or size>0x10000:
        raise ValueError('Current NOR boot slot is not a supported Linnet monitor')
    if _sign(session,image[:64])!=image[64:80] or _sign(session,image[2048:2048+size])!=image[16:32]:
        raise ValueError('Current NOR monitor authentication failed')


def prepare(device_plan, backup, output, boot_image=None):
    device=json.loads(Path(device_plan).read_text());output=Path(output).resolve()
    image_path=output.with_suffix('.im3')
    before_path=output.with_suffix('.before.bin')
    if output.exists() or image_path.exists() or before_path.exists():raise ValueError('NOR preparation output already exists')
    original=_backup(backup,device['serial'])
    from tools.link.s5l_bootstrap import artifacts_directory
    base=artifacts_directory(device)
    manifest=json.loads((base/'manifest.json').read_text());payload=(base/'monitor.bin').read_bytes()
    if manifest['serial']!=device['serial'] or manifest['target_id']!=TARGET or _sha(payload)!=manifest['monitor_sha256']:
        raise ValueError('Monitor artifact identity/hash mismatch')
    payload+=bytes((-len(payload))%16)
    if len(payload)>0x10000:raise ValueError('Monitor exceeds its verified SRAM window')
    archive=b'';boot_identity=None
    if boot_image is not None:
        from tools.inspect.image import inspect
        from tools.inspect.lz4 import pack_hc,unpack
        boot_image=Path(boot_image);boot=boot_image.read_bytes();boot_identity=inspect(boot)
        report=json.loads(boot_image.with_suffix('.json').read_text())
        if (report.get('kind')!='boot' or report.get('image')!=boot_identity or
                report.get('startup_default_format')!=1 or
                boot_identity['target_id']!=TARGET or
                report.get('device_serial')!=('' if boot_identity['profile']=='release' else device['serial']) or
                boot_identity['load_address']!=0x08000000 or len(boot)>0x80000):
            raise ValueError('Expected a matching device-bound native Boot build')
        compressed=pack_hc(boot)
        if unpack(compressed,len(boot))!=boot:
            raise ValueError('Packed Boot archive does not match the verified Boot image')
        header=bytearray(64)
        struct.pack_into('<4sIII',header,0,b'LNB1',1,len(compressed),len(boot))
        header[16:48]=hashlib.sha256(boot).digest()
        import zlib
        struct.pack_into('<I',header,60,zlib.crc32(header[:60]))
        archive=bytes(header)+compressed
        if START+2048+len(payload)>ARCHIVE or len(archive)>END-ARCHIVE:
            raise ValueError('Monitor and compressed Boot do not fit the verified NOR boot region')
    session,identity=_connect(device)
    try:
        if identity['build_id']!=manifest['build_id']:raise ValueError('RAM-verify this monitor before preparing NOR')
        layout=_authenticate_backup(session,original)
        current=_read(session)
        _validate_base(session,current,original)
        header=bytearray(2048);header[:8]=b'87021.0\x02'
        struct.pack_into('<II',header,8,0,len(payload));header[16:32]=_sign(session,payload)
        header[64:80]=_sign(session,header[:64])
        image=bytes(header)+payload
        if archive:image+=b'\xff'*(ARCHIVE-START-len(image))+archive
        image+=b'\xff'*((-len(image))%4096)
        if START+len(image)>END:raise ValueError('Boot image exceeds the verified NOR boot region')
        expected=current[:START]+image+current[START+len(image):]
        pages=[offset for offset in range(START,START+len(image),4096) if current[offset:offset+4096]!=expected[offset:offset+4096]]
        pages=[offset for offset in pages if offset not in (START,ARCHIVE_SECTOR)]+([ARCHIVE_SECTOR] if ARCHIVE_SECTOR in pages else [])+([START] if START in pages else [])
        plan=dict(target_id=TARGET,serial=device['serial'],build_id=manifest['build_id'],
                  original_path=str(Path(backup).resolve()),original_sha256=_sha(original),
                  base_path=str(before_path),base_sha256=_sha(current),
                  image_path=str(image_path),image_sha256=_sha(image),result_sha256=_sha(expected),
                  pages=pages,boot_offset=START,boot_limit=END,ram_identity=identity)
        if boot_identity is not None:
            plan.update(boot_image=boot_identity,startup_default_format=report['startup_default_format'])
        if layout['apple_plaintext']:
            apple_path=output.with_suffix('.apple.bin')
            if apple_path.exists():raise ValueError('Apple fallback output already exists')
            at=layout['apple_offset']+2048
            apple=_encrypt_apple(session,original[at:at+129024])
            output.parent.mkdir(parents=True,exist_ok=True)
            with apple_path.open('xb') as destination:destination.write(apple)
            plan.update(apple_payload_path=str(apple_path),apple_payload_sha256=_sha(apple))
        output.parent.mkdir(parents=True,exist_ok=True)
        with before_path.open('xb') as destination:destination.write(current)
        with image_path.open('xb') as destination:destination.write(image)
        with output.open('x') as destination:json.dump(plan,destination,indent=2)
        return dict(plan,prepared=str(output),plan_sha256=_sha(output.read_bytes()),persistent_writes=False)
    finally:session.close()


def install(prepared, expected_plan_sha, *, progress=print, reboot_mode='recovery'):
    if reboot_mode not in ('recovery','disk'):raise ValueError('Invalid Classic installation reboot mode')
    prepared=Path(prepared).resolve()
    plan, original, before, image, expected, pages = load_prepared(prepared, expected_plan_sha)
    if reboot_mode=='disk' and not plan.get('boot_image'):
        raise ValueError('Native Disk Mode requires the prepared Boot archive')
    size=struct.unpack_from('<I',image,12)[0]
    from tools.studio.classic_recovery import save_install
    # Retain recovery independently of disposable build output before any flash write.
    recovery = save_install(plan, original, expected, installed=False)
    receipt=dict(target_id=TARGET,serial=plan['serial'],build_id=plan['build_id'],phase='validating',
                 persistent_writes=False,completed=[],recovery=str(recovery))
    receipt_path=prepared.with_suffix('.receipt.json')
    from tools.studio.installer import _save
    def record():_save(receipt_path,receipt,durable=True)
    session=None;record()
    try:
        session,identity=_connect(plan)
        if identity['build_id']!=plan['build_id']:raise ValueError('The prepared monitor must be running before installation')
        _authenticate_backup(session,original)
        _validate_base(session,before,original)
        if _sign(session,image[:64])!=image[64:80] or _sign(session,image[2048:2048+size])!=image[16:32]:
            raise ValueError('Candidate boot authentication failed')
        progress('Verifying the complete current NOR before writing the boot slot')
        current=_read(session)
        # An explicit resumed invocation may encounter fully completed sectors.
        for offset in range(0,len(current),4096):
            actual=current[offset:offset+4096]
            allowed=(before[offset:offset+4096],expected[offset:offset+4096]) if offset in pages else (before[offset:offset+4096],)
            if actual not in allowed:raise ValueError(f'Unexpected flash contents at {offset:#x}; no further writes')
        receipt['phase']='programming';record()
        for offset in pages:
            desired=expected[offset:offset+4096]
            if current[offset:offset+4096]!=desired:
                receipt.update(persistent_writes=True,pending_sector=offset);record()
                _replace_sector(session, offset, current[offset:offset+4096], desired)
            receipt['completed'].append(offset);receipt.pop('pending_sector',None);record()
            progress(f'Verified boot sector {offset:#07x}')
        receipt['phase']='verifying';record();actual=_read(session)
        if actual!=expected:raise RuntimeError('Complete NOR readback differs from the planned result')
        receipt.update(result_sha256=_sha(actual),phase='reboot-requested');record()
        if reboot_mode=='disk':
            save_install(plan, original, expected, installed=True)
            receipt['phase']='flash-verified';record()
            _operation(session,67,0x52454254,size=0)
            receipt['phase']='disk-requested';record()
            return receipt
        _operation(session,53,0x52454254,size=0)
        session.close();session=None;time.sleep(1)
        session,identity=_connect(plan)
        if identity['build_id']!=plan['build_id']:raise RuntimeError('Persistent boot returned a different build')
        receipt.update(phase='persistent-link-verified',identity=identity);record()
        save_install(plan, original, expected, installed=True)
        return receipt
    except Exception as error:
        receipt['error']=str(error);record();raise
    finally:
        if session is not None:session.close()


def load_prepared(prepared, expected_plan_sha):
    """Reconstruct a NOR installation from its bounded, hash-checked artifacts."""
    prepared=Path(prepared);raw=_read_file(prepared,65536)
    if _sha(raw)!=expected_plan_sha:raise ValueError('NOR plan hash mismatch')
    plan=json.loads(raw)
    if (not isinstance(plan,dict) or not isinstance(plan.get('base_path'),str) or
            not isinstance(plan.get('base_sha256'),str)):
        raise ValueError('NOR plan lacks its transaction base snapshot')
    original=_backup(plan['original_path'],plan['serial'])
    if backup_layout(original)['apple_plaintext']:
        apple=_read_file(plan['apple_payload_path'],129024,exact=129024)
        if _sha(apple)!=plan['apple_payload_sha256']:
            raise ValueError('Prepared Apple fallback digest differs')
    before=_read_file(plan['base_path'],1048576,exact=1048576)
    if (len(before)!=len(original) or _sha(before)!=plan['base_sha256'] or
        before[:START]!=original[:START] or before[END:]!=original[END:]):
        raise ValueError('NOR transaction base differs from its verified snapshot')
    image=_read_file(plan['image_path'],END-START)
    if (plan.get('target_id')!=TARGET or plan.get('boot_offset')!=START or plan.get('boot_limit')!=END or
        plan.get('original_sha256')!=_sha(original) or plan.get('image_sha256')!=_sha(image) or
        not image or len(image)%4096 or START+len(image)>END or image[:8]!=b'87021.0\x02'):
        raise ValueError('Invalid NOR transaction artifacts')
    size=struct.unpack_from('<I',image,12)[0]
    if struct.unpack_from('<I',image,8)[0]!=0 or not size or size%16 or size>0x10000 or 2048+size>len(image):
        raise ValueError('Invalid NOR image entry or size')
    expected=before[:START]+image+before[START+len(image):]
    if _sha(expected)!=plan['result_sha256']:raise ValueError('Result image hash mismatch')
    pages=[offset for offset in range(START,START+len(image),4096) if before[offset:offset+4096]!=expected[offset:offset+4096]]
    pages=[offset for offset in pages if offset not in (START,ARCHIVE_SECTOR)]+([ARCHIVE_SECTOR] if ARCHIVE_SECTOR in pages else [])+([START] if START in pages else [])
    if pages!=plan['pages']:raise ValueError('NOR sector plan mismatch')
    return plan, original, before, image, expected, pages


def _replace_sector(session, offset, before, desired):
    for index in range(8):
        result=session.request(Command.BOOTSTRAP_CONTROL,struct.pack('<II',51,index)+desired[index*512:(index+1)*512])
        if len(result)!=12 or struct.unpack('<III',result)!=(0x4252504c,51,(1<<(index+1))-1):
            raise RuntimeError('NOR sector staging failed')
    result=_operation(session,52,offset,hashlib.sha256(before).digest()+hashlib.sha256(desired).digest(),36)
    if struct.unpack_from('<I',result)[0] or result[4:]!=hashlib.sha256(desired).digest():
        raise RuntimeError('NOR sector readback failed')


def restore(recovery, expected_sha, receipt_path, *, progress=print, mount_identity=None):
    """Restore the original flash using the serial-bound SRAM monitor, then reboot."""
    from tools.studio.classic_recovery import load
    from tools.studio.installer import _save

    record, original, installed = load(recovery, expected_sha)
    receipt_path=Path(receipt_path)
    if receipt_path.exists():
        previous = json.loads(_read_file(receipt_path, 65536))
        if (previous.get('operation') != 'restore-classic-nor' or
                previous.get('serial') != record['serial'] or
                previous.get('recovery_sha256') != expected_sha):
            raise ValueError('The restore receipt belongs to a different NOR recovery')
        if previous.get('passed') is True and previous.get('result_sha256') == _sha(original):
            return previous
        if mount_identity is None:
            mount_identity = previous.get('mount_identity')
    receipt=dict(operation='restore-classic-nor',target_id=TARGET,serial=record['serial'],
                 recovery=str(Path(recovery).resolve()),recovery_sha256=expected_sha,
                 original_sha256=_sha(original),installed_sha256=_sha(installed),
                 passed=False,persistent_writes=False,completed=[],phase='validating')
    if mount_identity is not None:
        receipt['mount_identity'] = mount_identity
    def record_receipt():_save(receipt_path,receipt,durable=True)
    session=None;record_receipt()
    try:
        session,identity=_connect(record)
        if identity['build_id'] != record['monitor_build_id']:
            raise ValueError('The recovery monitor differs from the verified Classic installation')
        receipt['identity']=identity
        _authenticate_backup(session,original)
        _validate_base(session,installed,original)
        progress('Reading and checking the complete Classic boot flash…')
        current=_read(session)
        # Exact complete snapshots allow an explicit retry after completed sectors;
        # unexpected bytes (including a partially programmed sector) stop all writes.
        for offset in range(0,len(current),4096):
            actual=current[offset:offset+4096]
            if actual not in (original[offset:offset+4096],installed[offset:offset+4096]):
                raise ValueError(f'Unexpected flash contents at {offset:#x}; no writes made')
        pages=[offset for offset in range(START,END,4096) if current[offset:offset+4096]!=original[offset:offset+4096]]
        # Publish the original authenticated entry sector after restoring its body.
        pages=[offset for offset in pages if offset!=START]+([START] if START in pages else [])
        receipt.update(phase='programming',pages=pages);record_receipt()
        for offset in pages:
            receipt.update(persistent_writes=True,pending_sector=offset);record_receipt()
            _replace_sector(session,offset,current[offset:offset+4096],original[offset:offset+4096])
            receipt['completed'].append(offset);receipt.pop('pending_sector',None);record_receipt()
            progress(f'Restored and verified original boot sector {offset:#07x}')
        receipt['phase']='verifying';record_receipt()
        actual=_read(session)
        if actual!=original:raise RuntimeError('Complete NOR readback differs from the original boot backup')
        receipt.update(passed=True,result_sha256=_sha(actual),phase='restored');record_receipt()
        progress('Original boot flash verified. Restarting the iPod…')
        _operation(session,53,0x52454254,size=0)
        receipt['phase']='reboot-requested';record_receipt()
        return receipt
    except Exception as error:
        receipt['error']=str(error);record_receipt();raise
    finally:
        if session is not None:session.close()
