"""Deterministic static UI, RGB565 asset and theme compiler (Python stdlib).

compile_system returns paths and hashes for firmware build and desktop oracle.
The only runtime output is native const data. JSON packages are host artifacts.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tomllib
from .tsx import ContentError, compile_tsx, COLOR_ROLES
from .layouts import apply_layout, gate_features, feature_enabled, place_classic_split_home, fit_monochrome_text
import importlib.util

LAYOUTS = ('default','ipod-photo','graphite','aurora','sunset','blueprint','minimal','native','ipod-classic','classic-split')
REFERENCE_WIDTH, REFERENCE_HEIGHT = 220, 176

def canonical(value):
    return (json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True)+'\n').encode('ascii')

def digest(data):
    return hashlib.sha256(data).hexdigest()

def generate_scene_ids(selected, available):
    """Resolve built-in scene names once; omitted scenes cannot match at runtime."""
    indices = {name: index for index, name in enumerate(selected)}
    lines = ['#ifndef LINNET_SCENE_IDS_H', '#define LINNET_SCENE_IDS_H', '#include <stdint.h>']
    prefix = 'linnet.system.'
    for name in sorted(available):
        if not name.startswith(prefix) or '.' in name[len(prefix):]:
            continue
        symbol = 'LN_UI_SCENE_' + name[len(prefix):].replace('-', '_').upper()
        value = f'{indices[name]}u' if name in indices else 'UINT32_MAX'
        lines.append(f'#define {symbol} {value}')
    return '\n'.join([*lines, '#endif', ''])

def bounded_path(root, path):
    candidate=(root/path).resolve()
    if not candidate.is_relative_to(root.resolve()) or not candidate.is_file():
        raise ContentError(f'missing file or path escapes source root: {path}')
    return candidate

def identifier(value):
    if not isinstance(value,str) or len(value)>80 or not re.fullmatch(r'[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+',value):
        raise ContentError(f'invalid Linnet identifier: {value!r}')
    return value

def rgb565(r,g,b):
    return ((r>>3)<<11)|((g>>2)<<5)|(b>>3)

def load_font(data):
    font=json.loads(data)
    required={'format','width','height','advance','first','count','alpha','source_sha256'}
    if not isinstance(font,dict) or not required<=set(font) or set(font)-required-{'advances'} or font['format']!='FONT':
        raise ContentError('invalid baked font')
    if any(type(font[key]) is not int or not 1<=font[key]<=24 for key in ('width','height','advance')) or any(type(font[key]) is not int for key in ('first','count')) or (font['first'],font['count'])!=(32,95):
        raise ContentError('font cell/advance must be 1..24 with 95 printable ASCII glyphs')
    alpha=font['alpha']
    if not isinstance(alpha,list) or len(alpha)!=font['width']*font['height']*95 or any(type(n) is not int or not 0<=n<=255 for n in alpha):
        raise ContentError('invalid font alpha pixels')
    if 'advances' in font and (not isinstance(font['advances'],list) or len(font['advances'])!=95 or any(type(n) is not int or not 1<=n<=24 for n in font['advances'])):
        raise ContentError('font advances must contain 95 values in 1..24')
    if not isinstance(font['source_sha256'],str) or not re.fullmatch(r'[0-9a-f]{64}',font['source_sha256']):
        raise ContentError('font source hash required')
    from .apple_font import FIRMWARE_SHA256
    if font['source_sha256'] == FIRMWARE_SHA256:
        raise ContentError('Use a locally extracted PODIUM.FNT instead of baking Apple glyphs into firmware')
    return font

def crop_font(font):
    """Losslessly pack each glyph's ink rectangle, preserving cell placement."""
    width,height=font['width'],font['height'];alpha=[];glyphs=[]
    for glyph in range(font['count']):
        source=font['alpha'][glyph*width*height:(glyph+1)*width*height]
        points=[(i%width,i//width) for i,value in enumerate(source) if value]
        offset=len(alpha)
        if points:
            left=min(x for x,y in points);top=min(y for x,y in points)
            right=max(x for x,y in points)+1;bottom=max(y for x,y in points)+1
            glyphs.append((offset,left,top,right-left,bottom-top))
            for y in range(top,bottom):alpha.extend(source[y*width+left:y*width+right])
        else:glyphs.append((offset,0,0,0,0))
    return alpha,glyphs

def scale_font(font,x_numerator,x_denominator,y_numerator,y_denominator):
    """Bake logical pixels so one glyph pixel maps to one physical LCD pixel."""
    old_width,old_height=font['width'],font['height']
    width=(old_width*x_numerator+x_denominator-1)//x_denominator
    height=(old_height*y_numerator+y_denominator-1)//y_denominator
    alpha=[]
    for glyph in range(font['count']):
        start=glyph*old_width*old_height
        for y in range(height):
            sy=min(old_height-1,y*y_denominator//y_numerator)
            for x in range(width):
                sx=min(old_width-1,x*x_denominator//x_numerator)
                alpha.append(font['alpha'][start+sy*old_width+sx])
    result=dict(font,width=width,height=height,alpha=alpha,
                advance=max(1,(font['advance']*x_numerator+x_denominator-1)//x_denominator))
    if 'advances' in font:
        result['advances']=[max(1,(value*x_numerator+x_denominator-1)//x_denominator)
                            for value in font['advances']]
    return result

def scale_layout(scenes,width,height):
    """Scale the device-independent authoring space to the target framebuffer."""
    if (width,height)==(REFERENCE_WIDTH,REFERENCE_HEIGHT): return scenes
    for scene in scenes:
        for node in scene['nodes']:
            left=node['x']*width//REFERENCE_WIDTH
            top=node['y']*height//REFERENCE_HEIGHT
            right=(node['x']+node['w'])*width+REFERENCE_WIDTH-1
            bottom=(node['y']+node['h'])*height+REFERENCE_HEIGHT-1
            right=right//REFERENCE_WIDTH;bottom=bottom//REFERENCE_HEIGHT
            node.update(x=min(left,width-1),y=min(top,height-1),
                        w=max(1,min(width,right)-min(left,width-1)),
                        h=max(1,min(height,bottom)-min(top,height-1)))
    return scenes

def ppm(data):
    """Strict bounded P3 RGB image; source does not depend on host image libraries."""
    if len(data)>1024*1024: raise ContentError('asset source exceeds 1 MiB')
    try: source=data.decode('ascii')
    except UnicodeError as exc: raise ContentError('asset must be ASCII P3 PPM') from exc
    tokens=re.sub(r'#[^\n]*','',source).split()
    if len(tokens)<4 or tokens[0]!='P3': raise ContentError('asset must be P3 PPM')
    try: values=[int(token) for token in tokens[1:]]
    except ValueError as exc: raise ContentError('invalid PPM integer') from exc
    width,height,maximum=values[:3]; pixels=values[3:]
    if not 1<=width<=1024 or not 1<=height<=1024 or width*height>131072 or maximum!=255 or len(pixels)!=width*height*3:
        raise ContentError('invalid PPM dimensions/range/length')
    if any(value<0 or value>255 for value in pixels): raise ContentError('PPM sample outside 0..255')
    return dict(width=width,height=height,pixels=[rgb565(*pixels[i:i+3]) for i in range(0,len(pixels),3)])

def resolve_features(config, target_manifest=None):
    from tools.studio.targets import CAPABILITY_DEFAULTS
    features=config.get('features',{})
    names=('photos','video','files','demo','themes','cover_flow','games')
    if not isinstance(features,dict) or any(key not in names or type(value) is not bool for key,value in features.items()):
        raise ContentError('feature flags must use supported names and boolean values')
    features=dict(features)
    if target_manifest is None:
        return features
    capabilities=target_manifest.get('capabilities',{})
    if not isinstance(capabilities,dict) or any(key not in names or type(value) is not bool for key,value in capabilities.items()):
        raise ContentError('invalid target capability flags')
    capabilities=dict(CAPABILITY_DEFAULTS, **capabilities)
    features['cover_flow']=feature_enabled(features,'cover_flow') and capabilities['cover_flow']
    for name,available in capabilities.items():
        if not available:features[name]=False
    features['video_settings']=feature_enabled(features,'video') and target_manifest.get('media_video_backend')=='bcm-mp4'
    return features


def compile_system(root, output, configpath=None, target_override=None):
    root=Path(root).resolve(); output=Path(output).resolve(); sources={}
    def read(path):
        path=bounded_path(root,path)
        with path.open('rb') as reader:
            data=reader.read(1024*1024+1)
        if len(data)>1024*1024: raise ContentError(f'source exceeds 1 MiB: {path}')
        sources[path.relative_to(root).as_posix()]=digest(data)
        return data
    def toml(path):
        return tomllib.loads(read(path).decode('utf-8'))
    for source in ('tools/content/compiler.py','tools/content/tsx.py','tools/content/layouts.py','ui/runtime/ui.h'):
        read(source)
    config=toml(configpath or 'config/system.toml')
    system=config.get('system',{})
    target=target_override if target_override is not None else (system.get('target') if isinstance(system,dict) else None)
    if not isinstance(target,str) or not re.fullmatch(r'[a-z][a-z0-9-]{0,63}',target):
        raise ContentError(f'invalid UI target: {target!r}')
    target_path=f'targets/{target}/target.toml'
    try: target_manifest=toml(target_path)
    except (ContentError,tomllib.TOMLDecodeError) as exc: raise ContentError(f'unsupported UI target: {target}') from exc
    width,height=target_manifest.get('lcd_width'),target_manifest.get('lcd_height')
    if (type(width) is not int or type(height) is not int or
        not 1<=width<=1024 or not 1<=height<=1024 or width*height>131072):
        raise ContentError('UI target dimensions exceed the supported framebuffer bound')
    monochrome=str(target_manifest.get('lcd_format','')).startswith('gray')
    profile_root = None
    profile_layout = None
    scanline = False
    profile_id = target_manifest.get('presentation_profile')
    if profile_id is not None:
        if not isinstance(profile_id, str) or not re.fullmatch(r'[a-z][a-z0-9-]*', profile_id):
            raise ContentError('invalid presentation profile')
        profile_root = root/'ui/profiles'/profile_id
        profile = toml(profile_root/'profile.toml')
        if set(profile) != {'width', 'height', 'coverflow_renderer'} or (profile['width'], profile['height']) != (width, height):
            raise ContentError('presentation profile does not match target display')
        if profile['coverflow_renderer'] not in ('column', 'scanline'):
            raise ContentError('invalid cover-flow renderer')
        scanline = profile['coverflow_renderer'] == 'scanline'
        if scanline and (width, height) != (320, 240):
            raise ContentError('scanline cover-flow requires 320 x 240')
        layout_path = bounded_path(root, profile_root/'layouts.py')
        read(layout_path)
        module_spec = importlib.util.spec_from_file_location('linnet_presentation_' + profile_id, layout_path)
        module = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(module)
        profile_layout = module.apply_layout
    features=resolve_features(config,target_manifest)
    ui=config.get('ui',{})
    if not isinstance(ui,dict): raise ContentError('ui must be a table')
    if set(ui)-{'theme','apps'}: raise ContentError('unknown UI system setting')
    selected=ui.get('apps',[])
    if not isinstance(selected,list) or not 1<=len(selected)<=64:
        raise ContentError('ui.apps must select 1..64 distinct app IDs')
    for app_id in selected: identifier(app_id)
    if len(selected)!=len(set(selected)):
        raise ContentError('ui.apps must select 1..64 distinct app IDs')
    game_apps={'linnet.system.games', 'linnet.system.game-play'}
    chosen_games=game_apps.intersection(selected)
    from tools.studio.targets import resolve_target
    if chosen_games and not resolve_target(target_manifest)['capabilities']['games']:
        raise ContentError(f'{target}: game apps require capabilities.games=true')
    if chosen_games and chosen_games != game_apps:
        raise ContentError('game menu and game play must be selected together')
    if chosen_games:
        from tools.studio.targets import GAME_PRESENTATIONS
        game_presentation=target_manifest.get('game_presentation')
        if (not isinstance(game_presentation, str) or game_presentation not in GAME_PRESENTATIONS or
                (width, height) != GAME_PRESENTATIONS[game_presentation]):
            raise ContentError(f'{target}: game_presentation={game_presentation!r} must match a supported game layout {GAME_PRESENTATIONS}')
    theme_id=identifier(ui.get('theme'))
    theme_manifests={}; seen_theme_ids=set()
    # Target editions replace shared manifests by stable ID; duplicates within
    # either scope remain errors. Keep precedence independent of path sorting.
    theme_paths = sorted((root/'themes').glob('*/theme.toml'))
    if profile_root is not None:
        theme_paths += sorted((profile_root/'themes').glob('*/theme.toml'))
    theme_paths += sorted((root/'targets'/target/'themes').glob('*/theme.toml'))
    for path in theme_paths:
        manifest=tomllib.loads(path.read_text(encoding='utf-8'))
        metadata=manifest.get('theme',{})
        name=identifier(metadata.get('id'))
        scope = path.parent.parent
        if (scope,name) in seen_theme_ids: raise ContentError('duplicate theme ID')
        seen_theme_ids.add((scope,name))
        compatible_targets=metadata.get('targets')
        if compatible_targets is not None:
            if (not isinstance(compatible_targets,list) or not compatible_targets or
                len(compatible_targets)!=len(set(compatible_targets))):
                raise ContentError('theme targets must be a nonempty distinct list')
            for compatible_target in compatible_targets:
                if not isinstance(compatible_target,str) or not re.fullmatch(r'[a-z][a-z0-9-]{0,63}',compatible_target):
                    raise ContentError('invalid theme target ID')
            if target not in compatible_targets:
                continue
        theme_manifests[name]=path
    theme_profile=target_manifest.get('ui')
    if theme_profile is not None:
        if not isinstance(theme_profile,dict) or set(theme_profile)!={'allowed_themes','default_theme'}:
            raise ContentError('target UI profile requires allowed_themes and default_theme')
        allowed=theme_profile['allowed_themes']
        if not isinstance(allowed,list) or not allowed or len(allowed)!=len(set(allowed)):
            raise ContentError('target allowed_themes must be a nonempty distinct list')
        allowed=[identifier(name) for name in allowed]
        default_theme=identifier(theme_profile['default_theme'])
        if default_theme not in allowed or any(name not in theme_manifests for name in allowed):
            raise ContentError('target UI profile references an unavailable theme')
        theme_manifests={name:theme_manifests[name] for name in allowed}
        if theme_id not in allowed: theme_id=default_theme
    if theme_id not in theme_manifests: raise ContentError(f'theme unavailable: {theme_id}')
    preferred=('linnet.theme.default','local.example.midnight','linnet.theme.ipod-photo','linnet.theme.ipod-classic',
               'linnet.theme.graphite','linnet.theme.aurora','linnet.theme.sunset','linnet.theme.blueprint')
    ordered=sorted(theme_manifests,key=lambda name:(preferred.index(name) if name in preferred else len(preferred),name))
    if len(ordered)>16: raise ContentError('at most 16 bundled themes')
    themes=[]; theme_specs={}
    for name in ordered:
        spec=toml(theme_manifests[name]); metadata=spec['theme']
        if set(spec)-{'theme','colors','metrics','assets'} or set(metadata)-{'id','font','font_license','font_source','name','style','layout','targets'}:
            raise ContentError('unknown theme field')
        if metadata.get('font_source') not in (None, 'apple-podium-sans16'):
            raise ContentError('unknown optional font source')
        label=metadata.get('name',{'linnet.theme.default':'Linnet','local.example.midnight':'Midnight'}.get(name,name.rsplit('.',1)[-1]))
        if not isinstance(label,str) or not 1<=len(label)<=24 or any(ord(c)<32 or ord(c)>126 for c in label):
            raise ContentError('theme name must be 1..24 printable ASCII characters')
        style=metadata.get('style','standard')
        if style not in ('standard','monochrome','apple-photo','apple-classic','album-art'): raise ContentError('unknown theme style')
        layout=metadata.get('layout','default')
        if layout not in LAYOUTS: raise ContentError('unknown theme layout')
        if monochrome and layout != 'classic-split': layout='default'
        palette=spec.get('colors',{})
        if not {'foreground','background','accent','muted','panel'}<=set(palette): raise ContentError('theme lacks required color tokens')
        for key,value in palette.items():
            if not re.fullmatch(r'[a-z][a-z0-9_]*',key) or not isinstance(value,str) or not re.fullmatch(r'#[0-9a-fA-F]{6}',value):
                raise ContentError('invalid theme color token')
        for key,fallback in {'selection_fg':'background','selection_bg':'accent','status_fg':'foreground',
                             'status_bg':'panel','border':'muted','artwork_bg':'panel','artwork_fg':'accent','battery':'accent'}.items():
            palette.setdefault(key,palette[fallback])
        if monochrome:
            palette.update(foreground='#000000',background='#ffffff',accent='#000000',muted='#000000',
                           panel='#ffffff',selection_fg='#ffffff',selection_bg='#000000',
                           status_fg='#000000',status_bg='#ffffff',border='#000000',
                           artwork_bg='#ffffff',artwork_fg='#000000',battery='#000000')
        metrics=spec.get('metrics',{})
        if set(metrics)!={'row_height','padding'} or type(metrics['row_height']) is not int or not 16<=metrics['row_height']<=32 or type(metrics['padding']) is not int or not 0<=metrics['padding']<=12:
            raise ContentError('theme metrics require row_height16..32/padding0..12')
        # Physical monochrome targets need binary coverage for every selectable
        # theme.  Retaining a color theme's style flag here made text, status
        # symbols and decorative assets pass through antialias blends before
        # the LCD driver quantized them to four shades, producing ragged gray
        # fringes on the passive-matrix panel.
        flags=1 if monochrome else {'standard':0,'monochrome':1,'apple-photo':2,'album-art':4,'apple-classic':8}[style]
        if not monochrome and metadata.get('font_source') == 'apple-podium-sans16': flags |= 16
        themes.append(dict(id=name,name=label,colors=palette,flags=flags,layout=layout,metrics=metrics))
        theme_specs[name]=spec
    theme_path=theme_manifests[theme_id]; theme=toml(theme_path)
    colors=themes[ordered.index(theme_id)]['colors']
    assets=[]; asset_names={}
    def add_asset(name,path):
        if not re.fullmatch(r'[a-z][a-z0-9_.-]*',name) or name in asset_names: raise ContentError('invalid/duplicate asset name')
        asset=ppm(read(path)); asset['name']=name; asset_names[name]=len(assets); assets.append(asset)
        if len(assets)>64 or sum(a['width']*a['height']*2 for a in assets)>262144: raise ContentError('asset pack exceeds 64 resources /256 KiB')
    for name,path in sorted(theme.get('assets',{}).items()): add_asset(name,theme_path.parent/path)
    manifests={}
    for directory in ('ui/system','apps'):
        for path in sorted((root/directory).glob('*/manifest.toml')):
            spec=tomllib.loads(path.read_text(encoding='utf-8'))
            app_id=identifier(spec.get('app',{}).get('id'))
            if app_id in manifests: raise ContentError('duplicate app ID')
            manifests[app_id]=path
    scenes=[]; callbacks={}; native_sources=[]; native_linked=[]; native_modes={}
    screen_root = root/'targets'/target/'screens'
    if not screen_root.is_dir() and profile_root is not None:
        screen_root = profile_root/'screens'
    native_screens = screen_root.is_dir()
    presentation_path=root/'targets'/target/'presentation.toml'
    presentation=toml(presentation_path) if presentation_path.is_file() else {}
    if set(presentation)-{'clock_font','scale_clock','adjustments'}:
        raise ContentError('unknown target presentation field')
    for app_id in selected:
        if app_id not in manifests: raise ContentError(f'app unavailable: {app_id}')
        path=manifests[app_id]; manifest=toml(path); app=manifest.get('app',{})
        if set(manifest)-{'app','assets','canvas'} or set(app)-{'id','title','entry','capabilities'}:
            raise ContentError('unknown app manifest field')
        capabilities=['ui','canvas'] if manifest.get('canvas') else ['ui']
        if app.get('capabilities')!=capabilities: raise ContentError('app capability list must match ui and any declared canvas')
        title=app.get('title','')
        if not isinstance(title,str) or not 1<=len(title)<=40 or any(ord(c)<32 or ord(c)>126 for c in title):
            raise ContentError('app title must be 1..40 printable ASCII characters')
        for name,resource in sorted(manifest.get('assets',{}).items()): add_asset(app_id+'.'+name,path.parent/resource)
        for name,callback in sorted(manifest.get('canvas',{}).items()):
            if not re.fullmatch(r'[a-z][a-z0-9_]*',name) or not {'source','symbol'}<=set(callback) or set(callback)-{'source','symbol','link'} or type(callback.get('link',False)) is not bool:
                raise ContentError('invalid native canvas declaration')
            symbol=callback['symbol']
            if not isinstance(symbol,str) or not re.fullmatch(r'ln_[A-Za-z_0-9]+',symbol) or symbol in callbacks.values():
                raise ContentError('native callback needs distinct ln_ C symbol')
            source=bounded_path(root,path.parent/callback['source'])
            relative=source.relative_to(root).as_posix()
            if source.suffix!='.c' or not re.fullmatch(r'[A-Za-z0-9_./-]+',relative): raise ContentError('invalid native source path')
            read(source)
            linked=callback.get('link',False)
            if relative in native_modes and native_modes[relative]!=linked:
                raise ContentError('native source cannot be included and separately linked')
            native_modes[relative]=linked
            if linked and relative not in native_linked:native_linked.append(relative)
            if relative not in native_sources: native_sources.append(relative)
            callbacks[app_id+'.'+name]=symbol
            if len(callbacks)>16: raise ContentError('at most16 native callbacks')
        entry = screen_root/app_id/'screen.tsx' if native_screens else path.parent/app['entry']
        nodes,focus=compile_tsx(read(entry).decode('utf-8'),colors,width if native_screens else REFERENCE_WIDTH,height if native_screens else REFERENCE_HEIGHT)
        scenes.append(dict(id=app_id,title=title,nodes=nodes,count=len(nodes),focus_node=focus))
    for scene in scenes:
        for node in scene['nodes']:
            resource=node['resource']
            if resource and resource not in asset_names: raise ContentError(f'unknown asset {resource}')
            node['resource']=asset_names.get(resource,0xffffffff)
            if node['canvas'] and node['canvas'] not in callbacks: raise ContentError('undeclared native canvas callback')
            node['canvas']=callbacks.get(node['canvas'],'')
            if scene['id']=='linnet.system.home' and 'linnet.system.games' in selected:
                for i, target_id in enumerate(node['targets']):
                    if target_id=='linnet.system.ui-demo':
                        node['targets'][i]='linnet.system.games'
                        node['items'][i]='Games'
            # Optional utilities may be omitted to stay within the scene budget.
            if node['targets']:
                optional={'linnet.system.games':'games', 'linnet.system.files':'files', 'linnet.system.ui-demo':'demo'}
                pairs=[(label, target_id) for label, target_id in zip(node['items'], node['targets'])
                       if target_id in selected or target_id not in optional or
                       (feature_enabled(features, optional[target_id]) and target_id != 'linnet.system.games')]
                node['items']=[label for label, _ in pairs]
                node['targets']=[target_id for _, target_id in pairs]
            targets=[]
            for navigation_target in node['targets']:
                actions={'@sync':0xfffffffe,'@shuffle':0xfffffffd,'@repeat':0xfffffffc,'@shuffle-all':0xfffffffb,'@menus':0xfffffffa}
                if navigation_target not in actions and navigation_target not in selected: raise ContentError(f'unknown/unselected target {navigation_target}')
                targets.append(actions[navigation_target] if navigation_target in actions else selected.index(navigation_target))
            node['targets']=targets; node['item_count']=len(node['items'])
    fonts=[]; font_ids={}; licenses={}; font_paths={}
    for variant in themes:
        directory=theme_manifests[variant['id']].parent
        metadata=theme_specs[variant['id']]['theme']
        font_path=directory/metadata.get('font','../../sdk/assets/font.json')
        font_license_path=directory/metadata.get('font_license','../../sdk/assets/FONT-LICENSE.txt')
        font=load_font(read(font_path))
        if font['height']>variant['metrics']['row_height']:
            raise ContentError('theme row height must accommodate its font')
        font_id=digest(canonical(font))
        if font_id not in font_ids:
            font_ids[font_id]=len(fonts);fonts.append(font)
        variant['font_index']=font_ids[font_id]
        variant['font']=font
        license_data=read(font_license_path)
        licenses[variant['font_index']]=license_data
        font_paths[variant['id']]=bounded_path(root,font_license_path).relative_to(root).as_posix()
        if native_screens:
            if profile_layout is not None:
                layouts=profile_layout(scenes,variant)
            else:
                if variant['layout'] != 'native': raise ContentError('native screens require native theme layouts')
                import copy
                layouts=copy.deepcopy(scenes)
        else:
            if variant['layout'] == 'native': raise ContentError('native theme requires target screens')
            layouts=apply_layout(scenes,variant,compact=monochrome)
        for i, scene in enumerate(scenes):
            if scene['id']=='linnet.system.game-play':
                import copy
                layouts[i]=copy.deepcopy(scene)
                for node in layouts[i]['nodes']:
                    if node['kind']==17:node['flags'] |= 64
            if scene['id']=='linnet.system.cover-flow':
                import copy
                layouts[i]=copy.deepcopy(scene)
                for node in layouts[i]['nodes']:
                    if node['kind']==3:node['flags'] |= 8
                    if scanline and node['kind']==17:node['flags'] |= 64
        variant['scenes']=layouts if native_screens else scale_layout(layouts,width,height)
        device_name=target_manifest['name'].removeprefix('Linnet for ')
        model=target_manifest.get('model_alias','').upper()
        if model and model not in device_name.upper():
            device_name+=f' ({model})'
        for scene in variant['scenes']:
            for node in scene['nodes']:
                if node['text']=='System version and device':
                    node['text']=f"{system['version']} / {device_name}"
        for rule in presentation.get('adjustments', []):
            if set(rule)-{'scene','text','kind','x','y','w','h','dy','row_height','replace_text'} or not ({'text','kind'} & set(rule)):
                raise ContentError('invalid target presentation adjustment')
            if 'row_height' in rule and (type(rule['row_height']) is not int or
                                         not font['height'] <= rule['row_height'] <= 32):
                raise ContentError('invalid target presentation row height')
            if 'replace_text' in rule and (not isinstance(rule['replace_text'],str) or
                                           len(rule['replace_text']) > 128):
                raise ContentError('invalid target presentation text')
            for scene in variant['scenes']:
                if 'scene' in rule and rule['scene'] != scene['id']: continue
                for node in scene['nodes']:
                    if any(node[k] != rule[k] for k in ('text','kind') if k in rule): continue
                    for key in ('x','y','w','h'):
                        if key in rule: node[key]=rule[key]
                    if 'dy' in rule: node['y']+=rule['dy']
                    if 'row_height' in rule: node['row_height']=rule['row_height']
                    if 'replace_text' in rule: node['text']=rule['replace_text']

        if not native_screens and (width,height)!=(REFERENCE_WIDTH,REFERENCE_HEIGHT):
            variant['metrics']=dict(
                row_height=max(font['height'],(variant['metrics']['row_height']*height+REFERENCE_HEIGHT//2)//REFERENCE_HEIGHT),
                padding=(variant['metrics']['padding']*width+REFERENCE_WIDTH//2)//REFERENCE_WIDTH)
        if target in ('classic6g-n25', 'video5g-m25', 'photo4g-p98'):
            for scene in variant['scenes']:
                if scene['id'] != 'linnet.system.photos-grid': continue
                top = 21 if target == 'photo4g-p98' else 24
                columns, rows = (5, 3) if target == 'classic6g-n25' else (6, 5) if target == 'video5g-m25' else (5, 5)
                grid_top = top + (24 if target == 'classic6g-n25' else 0)
                row_height = (height - grid_top) // rows
                for node in scene['nodes']:
                    if node['kind'] == 11:
                        node.update(x=0, y=0, w=width, h=top)
                    elif node['kind'] == 8:
                        node.update(x=0, y=grid_top, w=width-10, h=row_height*rows,
                                    grid_columns=columns, row_height=row_height)
                    elif node['kind'] == 9:
                        node.update(x=width-10, y=grid_top, w=10, h=row_height*rows)
                    elif node['text'] == 'Photo count':
                        node.update(x=8, y=top, w=width-16, h=24)
                        if target != 'classic6g-n25': node['flags'] |= 2
                    elif node['text'] == 'Photo count background':
                        # The full-width count band covers theme side chrome.
                        node.update(x=0, y=top, w=width, h=24)
                        if target == 'classic6g-n25': node['flags'] &= ~2
        if variant['layout']=='classic-split':
            place_classic_split_home(variant['scenes'],width,height)
        if monochrome:
            fit_monochrome_text(variant['scenes'],font,height)
        # Display also hosts the native menu editor; reserve a full scrolling
        # body and two independent footer lines on every target and theme.
        for scene in variant['scenes']:
            if scene['id'] != 'linnet.system.display':
                continue
            listing = next(n for n in scene['nodes'] if n['kind'] == 6)
            texts = [n for n in scene['nodes'] if n['kind'] == 3]
            bottom = min(height - 4, max(n['y'] + n['h'] for n in texts))
            row = max(18, font['height'] + 5)
            footer = 2 * (font['height'] + 4)
            listing['h'] = max(row, ((bottom - footer - listing['y']) // row) * row)
            listing['row_height'] = row
            for i, node in enumerate(texts[:2]):
                node.update(text='Display hint' if i == 0 else 'Display status', x=listing['x'],
                            y=bottom-footer+i*(font['height']+4), w=listing['w'], h=font['height'], flags=64)
        enabled_scenes=gate_features(variant['scenes'],features)
        if not enabled_scenes&1:
            raise ContentError('the first UI scene must be enabled')
        if [scene['id'] for scene in variant['scenes']]!=selected:
            raise ContentError('theme layout must retain ordered scene IDs')
        for scene in variant['scenes']:
            if not 1<=len(scene['nodes'])<=256 or scene['count']!=len(scene['nodes']):
                raise ContentError('theme layout node count invalid')
            if scene['focus_node']!=0xffffffff and not 0<=scene['focus_node']<scene['count']:
                raise ContentError('theme layout focus invalid')
            for node in scene['nodes']:
                if any(type(node[key]) is not int for key in ('x','y','w','h')) or node['x']<0 or node['y']<0 or node['w']<1 or node['h']<1 or node['x']+node['w']>width or node['y']+node['h']>height:
                    raise ContentError(f'theme layout rectangle escapes target: {variant["id"]}/{scene["id"]}')
    chosen=themes[ordered.index(theme_id)]
    if monochrome:
        # Native bitmap targets use the same pixel-grid face for supporting
        # text. Resampling a second antialiased face recreates the fringe that
        # the target-specific font is intended to avoid.
        hint_index=chosen['font_index']
    else:
        # Use a small face hinted directly onto the LCD pixel grid. Scaling the
        # regular UI face made the low-contrast corner hints visibly soft.
        hint=load_font(read(root/'sdk/assets/font-hint11.json'))
        hint_index=len(fonts);fonts.append(hint)
    # The prominent clock uses a separately hinted native-size face. Scaling
    # each target here keeps runtime glyph pixels aligned to its LCD grid.
    clock=load_font(read(bounded_path(root,presentation.get('clock_font','sdk/assets/font-clock24.json'))))
    if presentation.get('scale_clock',not native_screens) and (width,height)!=(REFERENCE_WIDTH,REFERENCE_HEIGHT):
        clock=scale_font(clock,width,REFERENCE_WIDTH,height,REFERENCE_HEIGHT)
    clock_index=len(fonts);fonts.append(clock)
    font=chosen['font'];scenes=chosen['scenes'];license_data=licenses[chosen['font_index']]
    document=dict(format='LNUI',enabled_scenes=enabled_scenes,target=target,width=width,height=height,theme=theme,themes=themes,default_theme=ordered.index(theme_id),scenes=scenes,assets=assets,font=font,fonts=fonts,native_sources=native_sources,native_linked=native_linked)
    document['hint_font_index']=hint_index
    document['clock_font_index']=clock_index
    document['coverflow_scanline']=scanline
    files={'scenes.json':canonical(document),'font-license.txt':license_data,
           'font-license-clock.txt':read(root/'sdk/assets/FONT-LICENSE.txt')}
    for index,data in licenses.items(): files[f'font-license-{index}.txt']=data
    packed_fonts=[crop_font(font) for font in fonts]
    files['linnet_ui.c']=generate_c(document,packed_fonts).encode('ascii')
    files['linnet_scene_ids.h']=generate_scene_ids(selected,manifests).encode('ascii')
    files[theme_id+'.ltheme']=canonical(dict(format='THME',theme=theme,assets=assets,font=font,font_license=license_data.decode('utf-8')))
    for scene in scenes:
        files[scene['id']+'.lapp']=canonical(dict(format='APPL',scene=scene))
    identity=digest(canonical(dict(sources=sources,document_sha256=digest(files['scenes.json']))))
    manifest=dict(format='CNTN',target=target,content_id=identity,
                  source_hashes=sources,files={name:dict(bytes=len(data),sha256=digest(data)) for name,data in sorted(files.items())},
                  themes=len(themes),scenes=len(scenes),nodes=sum(len(s['nodes']) for s in scenes),assets=len(assets),
                  layout_nodes=sum(len(scene['nodes']) for variant in themes for scene in variant['scenes']),fonts=len(fonts),
                  resource_bytes=sum(a['width']*a['height']*2 for a in assets)+sum(len(packed[0])+font['count']*8+len(font.get('advances',[])) for font,packed in zip(fonts,packed_fonts)),
                  font_storage='lossless-glyph-rectangles',
                  runtime='native-static-c',font_license=font_paths[theme_id],font_licenses=font_paths)
    files['content-manifest.json']=canonical(manifest)
    output.mkdir(parents=True,exist_ok=True)
    for name,data in files.items(): (output/name).write_bytes(data)
    return dict(content_id=identity,source=str(output/'linnet_ui.c'),scenes=str(output/'scenes.json'),
                manifest=str(output/'content-manifest.json'),report=manifest,features=dict(features),native_linked=native_linked)

def geometry_flags(node, theme, font):
    """Pack native list geometry without changing source flags or node layout.

    Low six flag bits retain their existing meaning. Bit 31 marks geometry:
    row height [6:12], columns [13:15], rows [16:19], cell width [20:27].
    Unknown flags or unrepresentable geometry retain the runtime fallback.
    """
    flags = node['flags']
    if node['kind'] not in (6, 7, 8) or flags & ~63:
        return flags
    layout = theme['layout']
    if node['kind'] == 8:
        columns = node.get('grid_columns', 5 if layout == 'ipod-photo' else 2 if layout == 'aurora' else 3)
    else:
        columns = 2 if layout == 'blueprint' and node['kind'] == 7 and node['h'] >= 90 else 1
    if node['text'] == 'Clock fields':
        height = 24 if layout == 'graphite' else 22 if layout == 'ipod-photo' else max(18, font['height'] + 3)
    elif node['kind'] == 6:
        height = node.get('row_height', max(18, font['height'] + 5))
    elif node['kind'] == 8:
        height = node.get('row_height', 31 if layout == 'ipod-photo' else max(1, node['h'] // 2))
    else:
        height = 46 if columns == 2 else theme['metrics']['row_height']
    if height <= 0 or node['w'] <= 0:
        return flags
    rows = max(1, node['h'] // height) if node['h'] > 0 else 1
    width = node['w'] // columns
    if not (1 <= height <= 127 and 1 <= columns <= 7 and 1 <= rows <= 15 and 1 <= width <= 255):
        return flags
    return flags | (1 << 31) | (height << 6) | (columns << 13) | (rows << 16) | (width << 20)


def _coverflow_settled_spans(widths, rows, canvas_width):
    """Bake first-owner visibility for every settled library edge case."""
    offsets, tokens, shared = [], [], {}
    for left_depth in range(4):
        for right_depth in range(4):
            case = []
            for y in range(len(rows[0])):
                owners = [7] * canvas_width
                for delta in (0, 1, -1, 2, -2, 3, -3):
                    if not -left_depth <= delta <= right_depth:
                        continue
                    pose = 0 if delta == 0 else 32
                    width = widths[pose]
                    line_left, line_width = rows[pose][y]
                    distance = abs(delta) * 256
                    shift = distance * 69 // 256 if distance <= 256 else 69 + (distance - 256) * 25 // 256
                    # C applies signed division after scaling: divide the
                    # positive magnitude first, then restore its sign.
                    x = canvas_width // 2 + (-1 if delta < 0 else 1) * (shift * canvas_width // 220) - width // 2
                    left = x + (line_left if delta < 0 else width - line_left - line_width)
                    for column in range(max(0, left), min(canvas_width, left + line_width)):
                        if owners[column] == 7:
                            owners[column] = delta + 3
                spans = []
                for column in range(1, canvas_width + 1):
                    if column == canvas_width or owners[column] != owners[column - 1]:
                        spans.append((column << 3) | owners[column - 1])
                assert 1 <= len(spans) <= 15 and spans[-1] >> 3 == canvas_width
                assert all((token & 7) == 7 or -left_depth <= (token & 7) - 3 <= right_depth for token in spans)
                key = tuple(spans)
                if key not in shared:
                    shared[key] = len(tokens)
                    tokens.extend(spans)
                case.append(shared[key])
            offsets.append(case)
    if len(tokens) > 65535:
        raise ValueError('Cover Flow settled span offsets exceed the 16-bit table bound')
    return offsets, tokens


def generate_c(doc, packed_fonts=None):
    if packed_fonts is None:
        packed_fonts=[crop_font(font) for font in doc['fonts']]
    out=['/* Generated by Linnet content compiler. Do not edit. */','#include "ui/runtime/ui.h"']
    out.extend('#include "'+source+'"' for source in doc['native_sources'] if source not in doc.get('native_linked',[]))
    for scene in doc['scenes']:
        for node in scene['nodes']:
            symbol=node.get('canvas')
            if symbol:out.append('extern void '+symbol+'(const struct ln_ui_canvas_context *);')
    if any(scene['id']=='linnet.system.cover-flow' and doc['enabled_scenes']&(1<<i) for i,scene in enumerate(doc['scenes'])):
        cover_size=96*doc['height']//176
        widths=[cover_size-round(pose*57*cover_size/(32*96)) for pose in range(33)]
        out.append('#if defined(LINNET_COVERFLOW)')
        out.append('const uint8_t ln_coverflow_ease[8][8]={'+','.join(
            '{'+','.join(str(round(256*(1-(step+1)/frames)**3*(1+.6*(step+1)/frames))) if step<frames else '0'
                         for step in range(8))+'}' for frames in range(1,9))+'};')
        gw,gh=doc['width'],doc['height']*140//176
        x_falloff=[((x-(gw-1)/2)/(gw*116/220))**2 for x in range(gw)]
        y_falloff=[((y-gh*62/140)/(gh*84/140))**2 for y in range(gh)]
        glow=[round(63*max(0,1-x_falloff[x]-y_falloff[y])**2)*4+((0,2),(3,1))[y&1][x&1] for x in range(gw) for y in range(gh)]
        scanline=doc.get('coverflow_scanline',False)
        texture_side,texture_height=(130,174) if scanline else (96,128)
        if scanline:
            for name,count,source in (('preview_x',130,24),('preview_y',174,32)):
                out.append(f'const uint8_t ln_coverflow_{name}[{count}]={{'+','.join(str(i*source//count) for i in range(count))+'};')
            pairs=[[],[]];indices=[];palette_indices=[{},{}]
            for y in range(gh):
                row=[];palette=pairs[y&1];lookup=palette_indices[y&1]
                for x in range(0,gw,2):
                    pair=(glow[x*gh+y],glow[(x+1)*gh+y])
                    if pair not in lookup:
                        lookup[pair]=len(palette);palette.append(pair)
                    row.append(lookup[pair])
                indices.append('{'+','.join(map(str,row))+'}')
            assert all(len(palette)<=192 for palette in pairs)
            out.append(f'const uint8_t ln_coverflow_glow_pairs[{gh}][{gw//2}]={{'+','.join(indices)+'};')
            out.append('const uint8_t ln_coverflow_glow_colors[2][192][2]={'+','.join('{'+','.join('{'+f'{a},{b}'+'}' for a,b in palette)+'}' for palette in pairs)+'};')
        else:
            out.append(f'const uint8_t ln_coverflow_glow[{gw}][{gh}]={{'+','.join('{'+','.join(map(str,glow[x*gh:(x+1)*gh]))+'}' for x in range(gw))+'};')
        out.append('const uint8_t ln_coverflow_widths[33]={'+','.join(map(str,widths))+'};')
        if not scanline:out.append(f'const struct ln_coverflow_column ln_coverflow_columns[33][{cover_size}]={{')
        scanlines,samples,sources,mirrors,row_geometry=[],[],[],[],[]
        source_rows,mirror_rows=[],[]
        for pose,width in enumerate(widths):
            tilt=pose*8
            columns=[];geometry=[]
            for x in range(width):
                height=round((96-tilt*15/256+(x+.5)*tilt*13/(width*256))*cover_size/96)
                values=((texture_side<<16)//height,(2*x+1)*texture_side//(2*width),24*doc['height']//176+(cover_size-height)//2,height,height*texture_height//texture_side)
                columns.append('{'+','.join(map(str,values))+'}');geometry.append(values)
            if not scanline:out.append('{'+','.join(columns)+'},')
            else:
                source_row=bytes(c[1] for c in geometry)
                mirror_row=bytes(texture_side-1-geometry[width-1-x][1] for x in range(width))
                source_rows.append(source_row);mirror_rows.append(mirror_row)
                sources.append('{'+','.join(map(str,source_row))+'}')
                mirrors.append('{'+','.join(map(str,mirror_row))+'}')
                rows=[];spans=[]
                for y in range(gh):
                    visible=[x for x,c in enumerate(geometry) if c[2]<=y<c[2]+c[4]]
                    left=visible[0] if visible else 0
                    assert not visible or visible==list(range(left,left+len(visible)))
                    spans.append((left,len(visible)))
                    start=len(samples)
                    for x in visible:
                        sy=((geometry[x][0]>>1)+(y-geometry[x][2])*geometry[x][0])>>16
                        if x==left or samples[-1]!=sy:samples.extend((x+1,sy))
                        else:samples[-2]=x+1
                    rows.append((start,left,len(visible),(len(samples)-start)//2))
                scanlines.append(rows)
                row_geometry.append(spans)
        if scanline:
            if len(samples)>65535:
                raise ValueError('Cover Flow scanline offsets exceed the 16-bit table bound')
            source_map=bytearray();source_offsets=[];source_lookup={}
            for rows in (source_rows,mirror_rows):
                offsets=[]
                for width,row in zip(widths,rows):
                    assert len(row)==width and all(value<texture_side for value in row)
                    if row not in source_lookup:
                        source_lookup[row]=len(source_map);source_map.extend(row)
                    offset=source_lookup[row]
                    assert offset<=65535 and offset+width<=len(source_map)
                    offsets.append(offset)
                source_offsets.append(offsets)
            assert len(source_map)<=65536
            out.append('#if defined(LINNET_SOC_PP5022)')
            out.append('const uint8_t ln_coverflow_source_map[]={'+','.join(map(str,source_map))+'};')
            out.append('const uint16_t ln_coverflow_source_offsets[2][33]={'+','.join('{'+','.join(map(str,row))+'}' for row in source_offsets)+'};')
            out.append('#else')
            out.append(f'const uint8_t ln_coverflow_sources[33][{cover_size}]={{'+','.join(sources)+'};')
            out.append('#if defined(LINNET_SOC_S5L8702)')
            out.append(f'const uint8_t ln_coverflow_mirror_sources[33][{cover_size}]={{'+','.join(mirrors)+'};')
            out.append('#endif')
            out.append('#endif')
            span_ends=[gh]*gh;next_end=gh
            for y in range(gh-1,-1,-1):
                if y+1<gh and any(rows[y]!=rows[y+1] for rows in row_geometry):
                    next_end=y+1
                span_ends[y]=next_end
            out.append('#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)')
            out.append(f'_Static_assert({gh}u <= UINT8_MAX, "Cover Flow span bound");')
            out.append('#define LN_COVERFLOW_SPAN_END(value) (value)')
            out.append('#else')
            out.append('#define LN_COVERFLOW_SPAN_END(value) 0')
            out.append('#endif')
            scanline_rows_32=scanlines[32]
            scanlines=['{'+','.join('{'+','.join(map(str,row))+f',LN_COVERFLOW_SPAN_END({span_ends[y]})'+'}'
                                  for y,row in enumerate(rows))+'}' for rows in scanlines]
            out.append(f'const struct ln_coverflow_scanline ln_coverflow_rows[33][{gh}]={{'+','.join(scanlines)+'};')
            out.append('#undef LN_COVERFLOW_SPAN_END')
            out.append('const uint8_t ln_coverflow_samples[]={'+','.join(map(str,samples))+'};')
            # Side covers use the fixed fully tilted pose during both motion
            # and rest. Resolve its perspective runs and mirrored coordinates
            # on the host, leaving one bounded source-index lookup per pixel.
            tilted=[]
            width=widths[32]
            for mirror in (False,True):
                source=mirror_rows[32] if mirror else source_rows[32]
                for start,left,visible,runs in scanline_rows_32:
                    source_y=[None]*width
                    column=left
                    for run in range(runs):
                        end,sy=samples[start+run*2:start+run*2+2]
                        assert column<=end<=left+visible and sy<texture_height
                        source_y[column:end]=[sy]*(end-column);column=end
                    assert column==left+visible
                    for x in range(width):
                        sy=source_y[width-1-x if mirror else x]
                        index=0 if sy is None else sy*texture_side+source[x]
                        assert 0<=index<texture_side*texture_height
                        tilted.append(index)
            out.append('#if defined(LINNET_SOC_PP5022)')
            out.append('const uint16_t ln_coverflow_tilted_samples[]={'+','.join(map(str,tilted))+'};')
            out.append('#endif')
            settled_offsets,settled_tokens=_coverflow_settled_spans(widths,row_geometry,gw)
            out.append('#if defined(LINNET_SOC_PP5022) || defined(LINNET_SOC_S5L8702)')
            out.append(f'const uint16_t ln_coverflow_settled_offsets[16][{gh}]={{'+','.join('{'+','.join(map(str,row))+'}' for row in settled_offsets)+'};')
            out.append('const uint16_t ln_coverflow_settled_spans[]={'+','.join(map(str,settled_tokens))+'};')
            out.append('#endif')
        else:out.append('};')
        out.append('#endif')
    def array(type_,name,values):
        out.append(f'static const {type_} {name}[] = {{')
        for i in range(0,len(values),24): out.append(','.join(str(v) for v in values[i:i+24])+',')
        out.append('};')
    for i,asset in enumerate(doc['assets']): array('uint16_t',f'ln_asset_{i}',asset['pixels'])
    out.append('const struct ln_ui_asset ln_ui_assets[] = {')
    for i,asset in enumerate(doc['assets']): out.append('{%du,%du,ln_asset_%d},'%(asset['width'],asset['height'],i))
    if not doc['assets']: out.append('{0,0,0},')
    out.extend(['};',f'const uint32_t ln_ui_asset_count = {len(doc["assets"])}u;'])
    quote=lambda value:json.dumps(value,ensure_ascii=True).replace('?',r'\?')
    font_values=[]
    for index,font in enumerate(doc['fonts']):
        pixels,glyphs=packed_fonts[index]
        array('uint8_t',f'ln_font_alpha_{index}',pixels or [0])
        out.append(f'static const struct ln_ui_glyph ln_font_glyphs_{index}[] = {{')
        for glyph in glyphs:out.append('{'+','.join(str(value)+'u' for value in glyph)+'},')
        out.append('};')
        if 'advances' in font: array('uint8_t',f'ln_font_advances_{index}',font['advances'])
        values=[f'ln_font_alpha_{index}']+[str(font[key])+'u' for key in ('width','height','advance','first','count')]
        values.append(f'ln_font_advances_{index}' if 'advances' in font else '0')
        values.append(f'ln_font_glyphs_{index}')
        font_values.append('{'+','.join(values)+'}')
    out.append('static const struct ln_ui_font ln_ui_fonts[] = {'+','.join(font_values)+'};')
    default=doc['default_theme']
    out.append('struct ln_ui_font ln_ui_font = '+font_values[doc['themes'][default]['font_index']]+';')
    out.append('const struct ln_ui_font ln_ui_hint_font = '+font_values[doc['hint_font_index']]+';')
    out.append('const struct ln_ui_font ln_ui_clock_font = '+font_values[doc['clock_font_index']]+';')
    # Share immutable arrays by their complete emitted contents. First occurrence
    # owns the symbol, retaining deterministic traversal and all scene identities.
    item_arrays,target_arrays,node_arrays,scene_arrays={},{},{},{}
    theme_scenes=[]
    for theme_index,theme in enumerate(doc['themes']):
        scene_nodes=[]
        scene_counts=[]
        scene_focus=[]
        for i,scene in enumerate(theme['scenes']):
            # Keep disabled scene identities addressable without retaining their
            # unreachable native nodes. Host packages still contain full scenes.
            if not doc['enabled_scenes'] & (1 << i):
                scene_nodes.append('0')
                scene_counts.append(0)
                scene_focus.append(0xffffffff)
                continue
            initializers=[]
            focus=scene['focus_node']
            preserve=focus < len(scene['nodes']) and scene['nodes'][focus]['kind']==6
            native_focus=0xffffffff
            for j,node in enumerate(scene['nodes']):
                if not preserve and j!=focus and node['flags']&2 and not node['deps'] and node['kind'] in (3,4,5,14):
                    continue
                if j==focus:native_focus=len(initializers)
                suffix=f'{theme_index}_{i}_{j}'
                items=targets='0'
                if node['items']:
                    key=tuple(node['items'])
                    if key not in item_arrays:
                        item_arrays[key]=f'ln_items_{suffix}'
                        out.append(f'static const char *const {item_arrays[key]}[] = '+'{'+','.join(quote(v) for v in key)+'};')
                    items=item_arrays[key]
                if node['targets']:
                    key=tuple(node['targets'])
                    if key not in target_arrays:
                        target_arrays[key]=f'ln_targets_{suffix}'
                        array('uint32_t',target_arrays[key],[str(v)+'u' for v in key])
                    targets=target_arrays[key]
                native_flags=geometry_flags(node,theme,doc['fonts'][theme['font_index']])
                values=[str(native_flags if k == 'flags' else node[k])+('' if k in ('x','y','w','h') else 'u') for k in ('kind','x','y','w','h','fg','bg','flags','count','resource','index')]
                values += [quote(node['text']),items,targets,str(node['item_count'])+'u',str(node['deps'])+'u',node['canvas'] or '0']
                initializers.append('{'+','.join(values)+'},')
            key=tuple(initializers)
            if key not in node_arrays:
                node_arrays[key]=f'ln_nodes_{theme_index}_{i}'
                out.append(f'static const struct ln_ui_node {node_arrays[key]}[] = {{')
                out.extend(initializers)
                out.append('};')
            scene_nodes.append(node_arrays[key])
            scene_counts.append(len(initializers))
            scene_focus.append(native_focus)
        initializers=[]
        for i,scene in enumerate(theme['scenes']):
            initializers.append('{'+','.join([quote(scene['id']),quote(scene['title']),scene_nodes[i],str(scene_counts[i])+'u',str(scene_focus[i])+'u'])+'},')
        key=tuple(initializers)
        if key not in scene_arrays:
            scene_arrays[key]=f'ln_scenes_{theme_index}'
            out.append(f'static const struct ln_ui_scene {scene_arrays[key]}[] = {{')
            out.extend(initializers)
            out.append('};')
        theme_scenes.append(scene_arrays[key])
    out.append('const struct ln_ui_theme ln_ui_themes[] = {')
    for index,theme in enumerate(doc['themes']):
        values=[rgb565(*(int(theme['colors'][role][i:i+2],16) for i in (1,3,5))) for role in COLOR_ROLES]
        descriptor=[quote(theme['id']),quote(theme['name']),'{'+','.join(str(v)+'u' for v in values)+'}',str(theme['flags'])+'u',
                    theme_scenes[index],f'&ln_ui_fonts[{theme["font_index"]}]',str(theme['metrics']['row_height'])+'u',
                    str(theme['metrics']['padding'])+'u',str(LAYOUTS.index(theme['layout']))+'u']
        out.append('{'+','.join(descriptor)+'},')
    out.extend(['};',f'const uint32_t ln_ui_theme_count = {len(doc["themes"])}u;',
                f'const uint32_t ln_ui_default_theme = {default}u;',
                f'const struct ln_ui_scene *ln_ui_scenes = {theme_scenes[default]};',
                f'const uint32_t ln_ui_scene_count = {len(doc["scenes"])}u;',
                f'const uint64_t ln_ui_enabled_scenes = UINT64_C({doc["enabled_scenes"]});',''])
    return '\n'.join(out)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--config',type=Path)
    args=parser.parse_args()
    print(json.dumps(compile_system(args.root,args.output,args.config),sort_keys=True,indent=2))

if __name__=='__main__': main()
