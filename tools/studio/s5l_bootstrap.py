"""Build the Classic SRAM monitor and its temporary BootROM transfer stub."""
import hashlib
import json
from pathlib import Path
import re
from tools.studio.build import ROOT, run, toolchain
from tools.inspect.image import pack


TEMPLATE_SERIAL = '0000000000000000'
PAYLOADS = ('monitor.bin', 'rom_transfer.bin', 'probe.bin', 'sram_bridge.bin')


def build(serial, output=None):
    if not re.fullmatch(r'[0-9A-F]{16}',serial):
        raise ValueError('Bootstrap requires the observed Classic USB serial')
    chain=toolchain(); out=Path(output) if output is not None else ROOT/'out/dev/classic6g-n25/bootstrap';out.mkdir(parents=True,exist_ok=True)
    board=ROOT/'targets/classic6g-n25/bootstrap'
    sources=[board.parent/'power.c',ROOT/'hal/s5l8702/core.c',ROOT/'hal/s5l8702/watchdog.c',ROOT/'hal/s5l8702/aes.c',ROOT/'hal/s5l8702/audio.c',ROOT/'hal/arm926/mmu.c',ROOT/'hal/s5l8702/timer.c',ROOT/'hal/s5l8702/irq.c',ROOT/'hal/s5l8702/irq.S',board/'start.S',board/'monitor.c',board/'persistent.c',board/'enter.S',board/'control.c',ROOT/'hal/codecs/cs42l55.c',ROOT/'hal/pmu/pcf50635.c',ROOT/'hal/arm926/cache.c',ROOT/'hal/s5l8702/pcm_dma.c',ROOT/'hal/s5l8702/ata.c',ROOT/'hal/s5l8702/input.c',board.parent/'panel.c',board.parent/'flash.c',ROOT/'hal/s5l8702/spi.c',board.parent/'display.c',ROOT/'hal/s5l8702/lcd_bus.c',board/'storage.c',ROOT/'hal/s5l8702/ata_pio.c',board.parent/'board.c',ROOT/'hal/s5l8702/i2c.c',ROOT/'hal/s5l8702/usb.c',ROOT/'hal/link/link.c',ROOT/'hal/link/dispatch.c',ROOT/'boot/src/linnet_boot.c',ROOT/'boot/src/loader.c',ROOT/'boot/src/sha256.c',ROOT/'kernel/memory.S']
    sources.append(board.parent/'lcd.c')
    probe_sources=[board/'probe.S',board/'probe.c',board/'storage.c',board/'filesystem.c',board.parent/'board.c',ROOT/'hal/s5l8702/i2c.c',ROOT/'hal/s5l8702/ata_pio.c',ROOT/'services/storage/fat32.c',ROOT/'kernel/memory.S']
    inputs=[*sources,*probe_sources,ROOT/'hal/api/power.h',ROOT/'hal/s5l8702/watchdog.h',ROOT/'hal/s5l8702/aes.h',ROOT/'hal/api/audio.h',ROOT/'hal/arm926/mmu.h',ROOT/'hal/s5l8702/usb.h',ROOT/'hal/s5l8702/boot_state.h',ROOT/'hal/s5l8702/timer.h',ROOT/'hal/s5l8702/irq.h',board.parent/'flash.h',ROOT/'hal/s5l8702/spi.h',ROOT/'hal/arm926/cache.h',ROOT/'hal/pmu/pcf50635.h',ROOT/'hal/s5l8702/pcm_dma.h',board.parent/'board.h',ROOT/'hal/api/input.h',ROOT/'hal/api/display.h',ROOT/'hal/api/ata.h',ROOT/'hal/api/core.h',ROOT/'hal/codecs/codec.h',ROOT/'hal/s5l8702/board.h',board.parent/'panel.h',ROOT/'hal/s5l8702/lcd_bus.h',ROOT/'hal/s5l8702/i2c.h',ROOT/'hal/s5l8702/ata_pio.h',ROOT/'services/storage/fat32.h',board/'memory.ld',board/'rom_transfer.S',board/'sram_bridge.S',ROOT/'hal/s5l8702/registers.h',
            ROOT/'hal/api/usb.h',ROOT/'hal/link/link.h',ROOT/'boot/include/linnet_bytes.h',
            ROOT/'boot/include/linnet_boot.h',ROOT/'boot/include/loader.h',ROOT/'boot/include/sha256.h',ROOT/'hal/link/dispatch.h',ROOT/'boot/src/crc32.inc',Path(__file__)]
    inputs.extend([ROOT/'hal/api/platform.h',ROOT/'hal/api/click_pcm.h',ROOT/'ui/profiles/color320/boot_font.h'])
    digest=hashlib.sha256(serial.encode())
    for path in inputs:digest.update(path.read_bytes())
    digest.update(json.dumps({k:v['sha256'] for k,v in chain.items()},sort_keys=True).encode())
    build_id=digest.hexdigest()
    (out/'bootstrap_build.h').write_text('#define LINNET_BOOTSTRAP_SERIAL "'+serial+'"\n#define LINNET_BOOTSTRAP_BUILD_BYTES {'+','.join('0x'+build_id[n:n+2] for n in range(0,64,2))+'}\n')
    (out/'linnet_target.h').write_text('#define LINNET_UI_WIDTH 320u\n#define LINNET_UI_HEIGHT 240u\n#define LINNET_DISPLAY_PIPELINE 0\n#define LINNET_STORAGE_MAX_READ_SECTORS 384u\n')
    flags=['-DLINNET_SRAM_MONITOR=1','--target=arm-none-eabi','-mcpu=arm926ej-s','-marm','-Oz','-ffreestanding','-fno-builtin',
           '-fdata-sections','-ffunction-sections','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
           '-Wall','-Wextra','-Werror','-I'+str(ROOT),'-I'+str(ROOT/'boot/include'),'-I'+str(out)]
    # Full LTO keeps the shared audio/control code inside the fixed NOR slot.
    # Assembly retains its explicit ABI; the same SRAM and payload bounds apply.
    objects=[]
    for index,source in enumerate(sources):
        obj=out/f'{index}.o';run([chain['compiler']['path'],*flags,*(['-flto'] if source.suffix=='.c' else []),'-c',source,'-o',obj]);objects.append(obj)
    run([chain['linker']['path'], '-m', 'armelf','-T',board/'memory.ld','--gc-sections','-o',out/'monitor.elf',*objects])
    symbols=run([chain['nm']['path'],'-n',out/'monitor.elf'])
    bss_end=re.search(r'^([0-9a-fA-F]+) B __bss_end$',symbols,re.M)
    if not bss_end or int(bss_end[1],16)>0x22016000:
        raise ValueError('Monitor leaves insufficient bounded SRAM upload staging')
    run([chain['objcopy']['path'],'-O','binary',out/'monitor.elf',out/'monitor.bin'])
    run([chain['compiler']['path'],*flags,'-DLINNET_BOOTSTRAP_MONITOR_PATH="'+(out/'monitor.bin').resolve().as_posix()+'"',
         '-c',board/'sram_bridge.S','-o',out/'sram_bridge.o'])
    run([chain['linker']['path'], '-m', 'armelf','-Ttext=0x08000000','-e','ln_sram_bridge','-o',out/'sram_bridge.elf',out/'sram_bridge.o'])
    run([chain['objcopy']['path'],'-O','binary',out/'sram_bridge.elf',out/'sram_bridge.bin'])
    run([chain['compiler']['path'],*flags,'-c',board/'rom_transfer.S','-o',out/'rom_transfer.o'])
    run([chain['linker']['path'], '-m', 'armelf','-Ttext=0x220284a8','-e','ln_rom_transfer','-o',out/'rom_transfer.elf',out/'rom_transfer.o'])
    run([chain['objcopy']['path'],'-O','binary',out/'rom_transfer.elf',out/'rom_transfer.bin'])
    probe_objects=[]
    for index,source in enumerate(probe_sources):
        obj=out/f'probe-{index}.o';run([chain['compiler']['path'],*flags,*(['-flto'] if source.suffix=='.c' else []),'-DLINNET_DFU_PROBE=1','-c',source,'-o',obj]);probe_objects.append(obj)
    run([chain['linker']['path'], '-m', 'armelf','-T',board/'memory.ld','--gc-sections','-o',out/'probe.elf',*probe_objects])
    run([chain['objcopy']['path'],'-O','binary',out/'probe.elf',out/'probe.bin'])
    probe=(out/'probe.bin').read_bytes()
    payload=(out/'monitor.bin').read_bytes();bridge=(out/'sram_bridge.bin').read_bytes();transfer=(out/'rom_transfer.bin').read_bytes()
    if not 32<=len(payload)<=0xe800:
        raise ValueError(f'Bootstrap monitor is {len(payload)} bytes; the NOR reservation allows 32..{0xe800} bytes')
    if len(transfer)>0x178:
        raise ValueError(f'Bootstrap DFU transfer is {len(transfer)} bytes; the limit is {0x178} bytes')
    (out/'monitor.lnos').write_bytes(pack(payload,'dev',build_id,0x22000000,0x22000000,'classic6g-n25'))
    bridge_image=pack(bridge,'dev',build_id,0x08000000,0x08000000,'classic6g-n25')
    (out/'monitor-bridge.lnos').write_bytes(bridge_image)
    report=dict(target_id='classic6g-n25',serial=serial,build_id=build_id,load_address=0x22000000,
                link_time_optimization='full',monitor_bytes=len(payload),monitor_sha256=hashlib.sha256(payload).hexdigest(),
                bridge_bytes=len(bridge),bridge_sha256=hashlib.sha256(bridge_image).hexdigest(),
                transfer_sha256=hashlib.sha256(transfer).hexdigest(),probe_sha256=hashlib.sha256(probe).hexdigest(),hardware_verified=False)
    def offset(name, expected):
        match=re.search(r'^([0-9a-fA-F]+) [rRtTdD] '+name+r'$',symbols,re.M)
        if not match:raise ValueError('Bootstrap binding symbol missing: '+name)
        at=int(match[1],16)-0x22000000
        if at<0 or payload[at:at+len(expected)]!=expected:raise ValueError('Bootstrap binding symbol differs: '+name)
        return at
    report.update(serial_offset=offset('ln_bootstrap_serial',serial.encode()+b'\0'),
                  build_id_offset=offset('build_id',bytes.fromhex(build_id)),
                  bridge_payload_sha256=hashlib.sha256(bridge).hexdigest())
    (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def bind(directory, serial, output):
    """Bind a verified precompiled monitor template without a local compiler."""
    if not re.fullmatch(r'[0-9A-F]{16}',serial) or serial==TEMPLATE_SERIAL:
        raise ValueError('Expected the observed Classic USB serial')
    directory=Path(directory);output=Path(output)
    manifest=directory/'manifest.json'
    if manifest.is_symlink() or manifest.stat().st_size>65536:raise ValueError('Invalid bootstrap manifest')
    report=json.loads(manifest.read_text())
    if report.get('target_id')!='classic6g-n25' or report.get('serial')!=TEMPLATE_SERIAL:
        raise ValueError('Expected a portable Classic bootstrap template')
    data={}
    for name,key,limit in (('monitor.bin','monitor_sha256',0x10000),
                           ('rom_transfer.bin','transfer_sha256',0x178),
                           ('probe.bin','probe_sha256',0x10000),
                           ('sram_bridge.bin','bridge_payload_sha256',0x10000)):
        path=directory/name
        if path.is_symlink() or not path.is_file() or not 1<=path.stat().st_size<=limit:
            raise ValueError('Invalid bootstrap payload: '+name)
        data[name]=path.read_bytes()
        if hashlib.sha256(data[name]).hexdigest()!=report.get(key):raise ValueError('Bootstrap checksum mismatch: '+name)
    original=data['monitor.bin'];payload=bytearray(original);old_build=bytes.fromhex(report['build_id'])
    at,build_at=report['serial_offset'],report['build_id_offset']
    if (len(old_build)!=32 or type(at)is not int or type(build_at)is not int or
        not 0<=at<=len(payload)-17 or not 0<=build_at<=len(payload)-32 or
        payload[at:at+17]!=TEMPLATE_SERIAL.encode()+b'\0' or payload[build_at:build_at+32]!=old_build or
        not (at+17<=build_at or build_at+32<=at)):
        raise ValueError('Invalid bootstrap template binding offsets')
    build_id=hashlib.sha256(old_build+serial.encode()).hexdigest()
    payload[at:at+16]=serial.encode();payload[build_at:build_at+32]=bytes.fromhex(build_id)
    data['monitor.bin']=bytes(payload)
    bridge=data['sram_bridge.bin']
    if bridge.count(original)!=1:raise ValueError('Bootstrap bridge does not contain exactly one monitor')
    data['sram_bridge.bin']=bridge.replace(original,bytes(payload))
    data['monitor.lnos']=pack(payload,'dev',build_id,0x22000000,0x22000000,'classic6g-n25')
    data['monitor-bridge.lnos']=pack(data['sram_bridge.bin'],'dev',build_id,0x08000000,0x08000000,'classic6g-n25')
    report.update(serial=serial,build_id=build_id,template_build_id=old_build.hex(),
                  monitor_sha256=hashlib.sha256(payload).hexdigest(),
                  bridge_payload_sha256=hashlib.sha256(data['sram_bridge.bin']).hexdigest(),
                  bridge_sha256=hashlib.sha256(data['monitor-bridge.lnos']).hexdigest())
    output.mkdir(parents=True,exist_ok=False)
    for name,content in data.items():(output/name).write_bytes(content)
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
