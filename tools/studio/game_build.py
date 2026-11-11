"""Ahead-of-time native game builds; installed games never invoke this toolchain."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import struct
import tomllib
from tools.content import game_package as fmt
from tools.studio.build import ROOT, run, toolchain
from tools.studio.targets import target_spec, presentation_defines


def _attributes(data, maximum):
    # AAELF32/addenda32: aeabi file attributes, ULEB128 values and NTBS names.
    if not data or data[0]!=65:
        raise ValueError('Missing ARM build attributes')
    at=1;attributes={}
    while at<len(data):
        start=at;length=struct.unpack_from('<I',data,at)[0];at+=4
        end=start+length
        if end>len(data) or length<11:
            raise ValueError('Invalid ARM attribute section')
        stop=data.index(0,at,end);vendor=data[at:stop];at=stop+1
        if vendor!=b'aeabi':
            raise ValueError('Unsupported ARM attribute vendor')
        while at<end:
            tag=data[at];size=struct.unpack_from('<I',data,at+1)[0];limit=at+size;at+=5
            if tag!=1 or limit>end or size<5:
                raise ValueError('Only file-level ARM attributes are supported')
            def uleb():
                nonlocal at
                value=shift=0
                while at<limit and shift<35:
                    byte=data[at];at+=1;value|=(byte&127)<<shift
                    if not byte&128:return value
                    shift+=7
                raise ValueError('Invalid ARM attribute integer')
            while at<limit:
                key=uleb()
                if key in (4,5,67):
                    at=data.index(0,at,limit)+1
                elif key in (32,65) or key>64:
                    raise ValueError('Unsupported ARM compatibility attributes')
                else:
                    value=uleb()
                    if key in attributes and attributes[key]!=value:
                        raise ValueError('Conflicting ARM attributes')
                    attributes[key]=value
        at=end
    if (not 1<=attributes.get(6,0)<=maximum or attributes.get(7,0)!=0
            or attributes.get(8)!=1 or attributes.get(9,0)>1
            or any(attributes.get(k,0) for k in (10,11,12,27,28))):
        raise ValueError('Game requires unsupported CPU, instruction mode or floating-point ABI')
    return attributes


def _elf(data, spec, relocatable=False):
    if data[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<HH',data,16) != (1 if relocatable else 2,40):
        raise ValueError('Expected a linked little-endian ARM executable')
    flags=struct.unpack_from('<I',data,36)[0]
    if flags>>24!=5 or flags&0x400:
        raise ValueError('Game requires ARM EABI5 with soft-float calling conventions')
    offset=struct.unpack_from('<I',data,32)[0]
    stride,count,strings=struct.unpack_from('<HHH',data,46)
    sections=[struct.unpack_from('<10I',data,offset+i*stride) for i in range(count)]
    names=sections[strings];names=data[names[4]:names[4]+names[5]]
    result=[];attributes=None
    for item in sections:
        name=names[item[0]:].split(b'\0',1)[0].decode('ascii')
        if name=='.ARM.attributes':
            attributes=_attributes(data[item[4]:item[4]+item[5]],4 if spec['architecture']=='armv5te' else 2)
        if item[1]==2:
            table=sections[item[6]];strings=data[table[4]:table[4]+table[5]]
            for off in range(item[4],item[4]+item[5],item[9]):
                index,value,_,kind,_,_=struct.unpack_from('<IIIBBH',data,off)
                symbol=strings[index:].split(b'\0',1)[0]
                if symbol.startswith(b'$t') or kind&15==2 and value&1:
                    raise ValueError('Thumb code is unsupported in ARM-only game packages')
        if not relocatable and item[1] in (4,9) and item[5]:
            raise ValueError('Runtime relocations are not supported')
        if item[2]&2:
            allowed=(name.startswith(('.text','.rodata','.data','.bss','.ARM.exidx')) if relocatable
                     else name in ('.text','.rodata','.data','.bss'))
            if not allowed:
                raise ValueError('Unexpected allocated game section: '+name)
            result.append(dict(name=name,address=item[3],bytes=item[5],stored=item[1]!=8))
    if attributes is None:
        raise ValueError('Game has no target compatibility attributes')
    return result


def build(manifest, target, output):
    manifest=Path(manifest).resolve(strict=True);project=manifest.parent
    config=tomllib.loads(manifest.read_text(encoding='utf-8'))
    if set(config) != {'game','build'}:
        raise ValueError('Game manifest requires [game] and [build] only')
    game=config['game'];settings=config['build']
    required={'id','title','description','author','license','version','schema','targets','capabilities','stack_bytes'}
    if set(game)!=required or set(settings)-{'sources','builtin','assembly_stack_bytes','assets','tables'} or not settings.get('sources'):
        raise ValueError('Unexpected or missing game manifest fields')
    fmt.identity(game['id'])
    if not isinstance(game['author'],str) or not game['author'].strip() or not isinstance(game['license'],str) or not game['license'].strip():
        raise ValueError('Game author and license are required')
    builtin=settings.get('builtin',0xffffffff);fmt._builtin(game['id'],builtin)
    if target not in game['targets'] or set(game['capabilities'])-{'music'}:
        raise ValueError('Game does not support the requested target/capabilities')
    spec=target_spec(target)
    if not spec['capabilities'].get('games',False):
        raise ValueError('Games are unsupported on this target')
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=True)
    from tools.content.game_assets import prepare
    prepared,asset_report=prepare(project,settings,output/'prepared')
    sources=[];assembly=settings.get('assembly_stack_bytes',{})
    for name in settings['sources']:
        source=(project/name).resolve(strict=True)
        if not source.is_relative_to(project) or source.suffix not in ('.c','.S'):
            raise ValueError('Game sources must stay inside their project')
        if source.suffix=='.S' and (name not in assembly or type(assembly[name]) is not int or not 0<=assembly[name]<=game['stack_bytes']):
            raise ValueError('Handwritten assembly requires an audited fixed stack bound')
        sources.append(source)
    if asset_report:sources.append(prepared)
    chain=toolchain();address=spec['load_address']+fmt.SLOT_OFFSET
    flags=['--target=arm-none-eabi',f'-mcpu={spec["cpu"]}',f'-march={spec["architecture"]}',
           '-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector',
           '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections',
           '-fdata-sections','-fstack-usage','-O2','-Wall','-Wextra','-Werror',
           '-I'+str(ROOT/'sdk/include'),'-I'+str(project),'-I'+str(output/'prepared'),
           f'-DLINNET_UI_WIDTH={spec["lcd_width"]}u',f'-DLINNET_UI_HEIGHT={spec["lcd_height"]}u']
    flags.extend(f'-D{name}={value}' for name,value in presentation_defines(spec).items())
    if builtin!=0xffffffff:
        flags+=['-I'+str(ROOT),f'-DLN_GAME_PACKAGE_ID={builtin}',
                '-DLINNET_AUDIO=1','-DLINNET_STORAGE=1','-Wno-unused-function','-Wno-unused-variable']
    # The same optimized EABI memory helpers as firmware; linked only if used.
    sources.extend((ROOT/'kernel/memory.S',ROOT/'services/audio/vorbis/division.c'))
    objects=[];stack_reports=[]
    for index,source in enumerate(sources):
        obj=output/f'part-{index}.o'
        usage=obj.with_suffix('.su')
        if usage.exists():usage.unlink()
        run([chain['compiler']['path'],*flags,'-c',source,'-o',obj])
        _elf(obj.read_bytes(),spec,relocatable=True)
        objects.append(obj)
        if usage.exists():stack_reports.append(usage)
    elf=output/'game.elf';mapfile=output/'game.map'
    run([chain['linker']['path'],'-m','armelf','-T',ROOT/'sdk/game/module.ld',
         '--defsym=GAME_ADDRESS='+str(address),'--gc-sections','--fatal-warnings',
         '-Map='+str(mapfile),'-o',elf,*objects])
    symbols=run([chain['nm']['path'],'-S','--size-sort',elf])
    if run([chain['nm']['path'],'--undefined-only',elf]):
        raise ValueError('Native game has unresolved symbols')
    sections=_elf(elf.read_bytes(),spec)
    for section in sections:
        if section['address']<address or section['address']-address+section['bytes']>fmt.SLOT_BYTES:
            raise ValueError('Game section exceeds its slot')
    raw=output/'game.bin';run([chain['objcopy']['path'],'-O','binary',elf,raw])
    payload=raw.read_bytes()
    bss=next((s for s in sections if s['name']=='.bss'),dict(address=address+len(payload),bytes=0))
    if bss['address'] != address+len(payload):
        raise ValueError('Game BSS must directly follow the initialized image')
    text=next(s for s in sections if s['name']=='.text')
    entry=struct.unpack_from('<I',elf.read_bytes(),24)[0]-address
    data=fmt.package(payload,target=target,game_id=game['id'],title=game['title'],
                     description=game['description'],bss=bss['bytes'],entry=entry,
                     capabilities=int('music' in game['capabilities']),builtin=builtin,
                     stack=game['stack_bytes'],text=text['bytes'],schema=game['schema'],version=game['version'])
    details=fmt.inspect(data,target)
    digest=hashlib.sha256(data).hexdigest();filename=digest[:8].upper()+'.LGM'
    package=output/filename
    frames=[]
    for usage in stack_reports:
        for line in usage.read_text().splitlines():
            fields=line.split('\t')
            if len(fields)>=3:
                size=int(fields[1]);frames.append(dict(function=fields[0],bytes=size,kind=fields[2]))
                if fields[2]!='static' or size>game['stack_bytes']:
                    raise ValueError('Game stack frame exceeds declared stack budget')
    if package.exists() and package.read_bytes()!=data:
        raise ValueError('Short package filename collision')
    package.write_bytes(data)
    report=dict(**details,author=game['author'],license=game['license'],prepared_assets=asset_report,package=str(package),filename=filename,file_crc=__import__('zlib').crc32(data),
                sha256=digest,sections=sections,stack_frames=frames,
                assembly_stack_bytes=assembly,
                stack_note='Static C frames only; SDK memory assembly uses at most32 bytes. Author assembly needs audited bounds; cumulative/indirect calls require hardware qualification',
                symbols=symbols,toolchain=chain)
    (output/'game-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report


def build_library(target, output):
    """Release-time built-ins; players consume these files without a compiler."""
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=True)
    games=[];manifests=sorted((ROOT/'games').glob('linnet.*/game.toml'))
    identities=[tomllib.loads(p.read_text(encoding='utf-8'))['game']['id'] for p in manifests]
    if len(identities)!=len(fmt.BUILTIN_IDS) or set(identities)!={'linnet.'+name for name in fmt.BUILTIN_IDS}:
        raise ValueError('Built-in game library must contain every reserved builtin identity exactly once')
    for manifest in manifests:
        report=build(manifest,target,output/'build'/manifest.parent.name)
        source=Path(report['package']);destination=output/report['filename'];data=source.read_bytes()
        if destination.exists() and destination.read_bytes()!=data:
            raise ValueError('Prepared game filename collision')
        destination.write_bytes(data)
        games.append({key:report[key] for key in ('id','filename','sha256','author','license','builtin')})
    metadata=dict(format='LNGAMES',version=1,target=target,games=games)
    (output/'library.json').write_text(json.dumps(metadata,sort_keys=True,indent=2)+'\n',encoding='utf-8')
    write_library_notices(output)
    return dict(directory=str(output),games=games)


def write_library_notices(directory):
    directory=Path(directory)
    (directory/'LICENSE.txt').write_bytes((ROOT/'LICENSE').read_bytes())
    (directory/'NOTICE.txt').write_text(
        'Linnet native games\nCopyright (C)2026 jspann21 and Linnet contributors.\n'
        'Licensed under GPL-3.0-or-later; see LICENSE.txt.\n'
        "Solitaire court art: Austin Gabriel's Public Domain Playing Cards (CC0).\n"
        'https://github.com/AustinGabriel/Public-Domain-and-CC0-Playing-Cards\n'
        'Matching game source, SDK and preparation tools are in the Linnet source release.\n',encoding='utf-8')


def library_files(directory,target):
    directory=Path(directory);packages=library(directory,target)
    return [*packages.values(),directory/'library.json',directory/'LICENSE.txt',directory/'NOTICE.txt']


def library(directory,target):
    directory=Path(directory).resolve(strict=True);manifest=directory/'library.json'
    if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size>65536:
        raise ValueError('Invalid prepared game library')
    value=json.loads(manifest.read_text(encoding='utf-8'))
    if (not isinstance(value,dict) or set(value)!={'format','version','target','games'}
            or value['format']!='LNGAMES' or value['version']!=1 or value['target']!=target
            or not isinstance(value['games'],list) or len(value['games'])>128):
        raise ValueError('Incompatible prepared game library')
    for name in ('LICENSE.txt','NOTICE.txt'):
        notice=directory/name
        if notice.is_symlink() or not notice.is_file() or not 0<notice.stat().st_size<=65536:
            raise ValueError('Prepared game library notice is missing or unsafe')
    result={};filenames=set()
    for item in value['games']:
        if not isinstance(item,dict) or set(item)!={'id','filename','sha256','author','license','builtin'}:
            raise ValueError('Invalid prepared game manifest')
        filename=item['filename']
        if not isinstance(filename,str) or not fmt.FILE_PATTERN.fullmatch(filename) or filename in filenames:
            raise ValueError('Invalid prepared game filename')
        path=directory/filename
        if path.is_symlink() or not path.is_file() or path.stat().st_size>256+fmt.SLOT_BYTES:
            raise ValueError('Missing or unsafe prepared game')
        data=path.read_bytes();header=fmt.inspect(data,target)
        if (header['id']!=item['id'] or header['builtin']!=item['builtin']
                or hashlib.sha256(data).hexdigest()!=item['sha256'] or item['id'] in result):
            raise ValueError('Prepared game checksum/identity mismatch')
        filenames.add(filename);result[item['id']]=path
    return result


def build_preview(manifest,target,output):
    """Compile the same source for the desktop ABI, after validating its ARM build."""
    import os
    manifest=Path(manifest).resolve(strict=True);output=Path(output).resolve()
    report=build(manifest,target,output/'arm')
    config=tomllib.loads(manifest.read_text(encoding='utf-8'))
    settings=config['build'];spec=target_spec(target);project=manifest.parent
    if any(Path(name).suffix!='.c' for name in settings['sources']):
        raise ValueError('Desktop preview requires portable C sources; qualify ARM assembly on hardware')
    prepared,assets=__import__('tools.content.game_assets',fromlist=['prepare']).prepare(project,settings,output/'prepared')
    sources=[project/name for name in settings['sources']]
    if assets:sources.append(prepared)
    destination=output/('game.dll' if os.name=='nt' else 'game.so')
    flags=['-std=c11','-shared','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'sdk/include'),
           '-I'+str(project),'-I'+str(output/'prepared'),
           f'-DLINNET_UI_WIDTH={spec["lcd_width"]}u',f'-DLINNET_UI_HEIGHT={spec["lcd_height"]}u']
    if os.name!='nt':flags+=['-fPIC']
    flags.extend(f'-D{name}={value}' for name,value in presentation_defines(spec).items())
    if settings.get('builtin',0xffffffff)!=0xffffffff:
        flags+=['-I'+str(ROOT),f'-DLN_GAME_PACKAGE_ID={settings["builtin"]}',
                '-DLINNET_AUDIO=1','-DLINNET_STORAGE=1','-Wno-unused-function','-Wno-unused-variable']
    run([toolchain()['compiler']['path'],*flags,*sources,'-o',destination])
    return dict(path=str(destination),id=report['id'],title=report['title'],description=report['description'],schema=report['schema'],capabilities=report['capabilities'])
