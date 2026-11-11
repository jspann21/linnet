"""Desktop execution and pixel oracle for Linnet's generated native UI."""
import hashlib
import html
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib
from tools.content.compiler import compile_system
from tools.studio.build import toolchain
from tools.studio.targets import TARGET_NUMBERS, presentation_defines, target_spec
from tools.studio.menu_definition import write_firmware_header

def _c_string(value):
    return json.dumps(value).replace('?', r'\?')

def png(raw,width,height):
    rows=[]
    for y in range(height):
        row=bytearray([0])
        for x in range(width):
            value=struct.unpack_from('<H',raw,(y*width+x)*2)[0]
            row.extend((((value>>11)&31)*255//31,((value>>5)&63)*255//63,(value&31)*255//31))
        rows.append(row)
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),9))+chunk(b'IEND',b'')

def simulate(root,output,tape=None,system=None,target=None,all_scenes=False,apple_font=None,menus=None,game_manifest=None):
    root=Path(root).resolve();output=Path(output).resolve();output.mkdir(parents=True,exist_ok=True)
    if all_scenes and tape is not None:raise ValueError('All-scenes capture cannot use an input tape')
    if system is None and target is not None:
        target_system=root/'targets'/target/'system.toml'
        if target_system.is_file():system=target_system
    content=compile_system(root,output/'generated',system,target_override=target)
    scenes=json.loads(Path(content['scenes']).read_text())
    width,height=scenes['width'],scenes['height']
    spec=target_spec(scenes['target'])
    font_hash=None
    if apple_font is not None:
        from tools.content.apple_font import BYTES, decode, TARGETS
        font_path=Path(apple_font)
        if scenes['target'] not in TARGETS or font_path.stat().st_size!=BYTES:
            raise ValueError('Optional Apple font is unsupported for this target or has an invalid size')
        font_data=font_path.read_bytes();decode(font_data)
        (output/'PODIUM.FNT').write_bytes(font_data)
        font_hash=hashlib.sha256(font_data).hexdigest()
    if (width, height) != (spec['lcd_width'], spec['lcd_height']):
        raise ValueError('simulator scene dimensions differ from the target manifest')
    (output/'generated/linnet_target.h').write_text(
        f'#include "targets/{scenes["target"]}/hardware.h"\n' +
        f'#define LINNET_TARGET_NUMBER {TARGET_NUMBERS[scenes["target"]]}u\n' +
        f'#define LINNET_EXTENDED_SOUND {int(spec.get("runtime_policy", {}).get("extended_sound", False))}\n' +
        ('#define LINNET_VIDEO 1\n' if spec.get('capabilities', {}).get('video', False) else '') +
        ''.join(f'#define {name} {value}\n' for name, value in presentation_defines(spec).items()))
    write_firmware_header(output/'generated')
    menu_hash=None
    if menus is not None:
        from tools.studio.screen_settings import decode
        menu_path=Path(menus)
        if menu_path.stat().st_size!=80:
            raise ValueError('Simulator menus must contain exactly 80 bytes')
        menu_data=menu_path.read_bytes();decode(menu_data)
        (output/'MENUS.BIN').write_bytes(menu_data)
        menu_hash=hashlib.sha256(menu_data).hexdigest()
    executable=output/('linnet-simulator.exe' if os.name == 'nt' else 'linnet-simulator')
    chain=toolchain()
    game_sources=[]
    if content.get('native_linked'):
        from tools.studio.game_build import build_preview
        import shutil
        manifests=[Path(game_manifest)] if game_manifest else sorted((root/'games').glob('linnet.*/game.toml'))
        entries=[]
        for index,manifest in enumerate(manifests):
            preview=build_preview(manifest,scenes['target'],output/'game-build'/str(index))
            name=f'game-{index}'+Path(preview['path']).suffix
            shutil.copyfile(preview['path'],output/name)
            entries.append('{'+','.join(_c_string(preview[key]) for key in ('title','description'))+','+_c_string(name)+f', {preview["schema"]}u, {preview["capabilities"]}u'+'}')
        (output/'generated/game_previews.h').write_text(
            '#define LN_GAME_PREVIEWS 1\n#define GAME_PREVIEW_COUNT '+str(len(entries))+'u\n'
            'static const struct {const char *title,*description,*file;unsigned schema,capabilities;} game_previews[]={'+','.join(entries)+'};\n',encoding='utf-8')
        game_sources=[str(root/'tools/simulator/game_host.c')]
    command=[chain['compiler']['path'],'-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(output/'generated'),'-I'+str(root),
             *(['-DLN_GAME_PREVIEWS=1'] if game_sources else []),*game_sources,
             str(root/'tools/simulator/native.c'),str(root/'ui/runtime/ui.c'),str(root/'services/display/clock.c'),str(root/'services/display/preferences.c'),str(output/'generated/linnet_ui.c'),'-o',str(executable)]
    if os.name!='nt' and game_sources:command+=['-ldl']
    compiled=subprocess.run(command,capture_output=True,text=True)
    if compiled.returncode:raise RuntimeError('Simulator compilation failed:\n'+compiled.stdout+compiled.stderr)
    events=json.loads(Path(tape).read_text()) if tape else []
    if not isinstance(events,list) or len(events)>10000:raise ValueError('Input tape must be a bounded JSON list')
    lines=[];total=captures=0
    for event in events:
        if not isinstance(event,dict) or set(event)-{'kind','value','repeat','capture'}:raise ValueError('Invalid input event')
        kind=event.get('kind',0);value=event.get('value',0);repeat=event.get('repeat',1);capture=event.get('capture',True)
        if any(type(v) is not int for v in (kind,value,repeat)) or type(capture) is not bool:raise ValueError('Invalid input types')
        if kind not in range(13) or not -(2**31)<=value<2**31 or not 1<=repeat<=100000:raise ValueError('Input out of bounds')
        if kind==2 and not 0<=value<=31 or kind in (3,4) and value not in (0,1):raise ValueError('Invalid button/status value')
        if kind==5 and value!=-1 and (value<0 or value>1023 or (value&255)>100):raise ValueError('Invalid battery state')
        if kind==6 and not -1<=value<86400:raise ValueError('Invalid clock state')
        if kind==7 and value not in (0,1,2):raise ValueError('Invalid playback state')
        if kind==8 and value not in range(5):raise ValueError('Invalid simulated clock result')
        if kind in (9,10) and value not in range(3):raise ValueError('Invalid simulated calendar/admission state')
        if kind==11 and value not in range(6):raise ValueError('Invalid playback modes')
        if kind==12 and value not in (0,1):raise ValueError('Invalid artwork presence')
        total+=repeat;captures+=capture
        if total>100000 or captures>512:raise ValueError('Input tape exceeds event/capture budget')
        lines.append(f'{kind} {value} {repeat} {int(capture)}\n')
    run=subprocess.run([str(executable),str(output)]+(['--all-scenes'] if all_scenes else [])+(['--apple-font'] if apple_font is not None else [])+(['--menus'] if menus is not None else [])+(['--game'] if game_manifest is not None else []),
                       input=''.join(lines),capture_output=True,text=True,timeout=120)
    if run.returncode:raise RuntimeError('Simulator execution failed:\n'+run.stdout+run.stderr)
    frames=[json.loads(line) for line in run.stdout.splitlines()]
    cards=[]
    for frame in frames:
        name=f"frame-{frame['frame']:05d}"
        raw=(output/(name+'.rgb565')).read_bytes()
        if len(raw)!=width*height*2 or zlib.crc32(raw)!=frame['crc32']:raise RuntimeError('Native framebuffer integrity mismatch')
        (output/(name+'.png')).write_bytes(png(raw,width,height))
        frame['sha256']=hashlib.sha256(raw).hexdigest()
        frame['theme_id']=scenes['themes'][frame['theme']]['id']
        frame['theme_name']=scenes['themes'][frame['theme']]['name']
        cards.append(f'<figure><img src="{name}.png" width="{width*2}" height="{height*2}"><figcaption>{html.escape(frame["theme_name"])} / {html.escape(scenes["themes"][frame["theme"]]["scenes"][frame["scene"]]["title"])} / selection {frame["selected"]}</figcaption></figure>')
    page='<!doctype html><meta charset="utf-8"><title>Linnet Simulator</title><style>body{font:15px system-ui;background:#e9ecef;color:#263238;margin:32px}main{display:flex;flex-wrap:wrap;gap:24px}figure{margin:0;background:white;padding:16px;border-radius:12px}img{image-rendering:pixelated}figcaption{padding-top:12px}</style><h1>Linnet Simulator</h1><p>Native UI frames from the input tape.</p><main>'+''.join(cards)+'</main>'
    (output/'index.html').write_text(page)
    report=dict(target_id=content['report']['target'],content=content,events=total,frames=frames,preview=str(output/'index.html'),
                native_oracle=True,
                apple_font_sha256=font_hash,menu_sha256=menu_hash,
                native_source_hashes={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in ('tools/simulator/native.c','ui/runtime/ui.c','ui/runtime/ui.h','tools/simulator/runner.py','tools/studio/menu_definition.py','services/music/menu.h','services/display/clock.c','services/display/clock.h','services/display/preferences.c','services/display/preferences.h')})
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
