"""Author-time sprite/table preparation. Device packages contain native data only."""
from pathlib import Path
import json
import re


def _source(project,name):
    path=(project/name).resolve(strict=True)
    if not path.is_relative_to(project):raise ValueError('Game asset escapes its project')
    return path


def prepare(project, settings, output):
    project=Path(project).resolve();output=Path(output);output.mkdir(parents=True,exist_ok=True)
    declarations=['#ifndef LINNET_PREPARED_GAME_ASSETS_H','#define LINNET_PREPARED_GAME_ASSETS_H','#include <linnet/draw.h>']
    source=['#include "game_assets.h"'];seen=set();report=[]
    assets=settings.get('assets',[]);tables=settings.get('tables',[])
    if not isinstance(assets,list) or not isinstance(tables,list) or len(assets)+len(tables)>64:
        raise ValueError('Game asset/table count exceeds64')
    def name(item):
        value=item.get('name')
        if not isinstance(value,str) or not re.fullmatch('[a-z][a-z0-9_]{0,47}',value) or value in seen:
            raise ValueError('Invalid or duplicate prepared asset name')
        seen.add(value);return value
    for item in assets:
        if not isinstance(item,dict) or set(item)-{'name','source','size','crop','background'} or not {'name','source'}<=set(item):
            raise ValueError('Invalid sprite preparation fields')
        symbol=name(item);path=_source(project,item['source'])
        from PIL import Image
        with Image.open(path) as original:
            if original.width*original.height>32000000:raise ValueError('Source image exceeds host preparation limit')
            image=original.convert('RGBA')
        if 'crop' in item:
            crop=item['crop']
            if (not isinstance(crop,list) or len(crop)!=4 or any(type(v) is not int for v in crop)
                    or min(crop)<0 or not crop[2] or not crop[3] or crop[0]+crop[2]>image.width or crop[1]+crop[3]>image.height):
                raise ValueError('Sprite crop exceeds source bounds')
            x,y,w,h=crop;image=image.crop((x,y,x+w,y+h))
        if 'size' in item:
            size=item['size']
            if not isinstance(size,list) or len(size)!=2 or any(type(v) is not int or not 1<=v<=1024 for v in size):
                raise ValueError('Invalid prepared sprite dimensions')
            image=image.resize(tuple(size),Image.Resampling.LANCZOS)
        w,h=image.size
        if w>1024 or h>1024 or w*h>131072:raise ValueError('Sprite exceeds native slot budget')
        if 'background' in item:
            color=item['background']
            if not isinstance(color,list) or len(color)!=3 or any(type(v) is not int or not 0<=v<=255 for v in color):
                raise ValueError('Invalid sprite background')
            background=Image.new('RGBA',image.size,tuple(color)+(255,));background.alpha_composite(image);image=background
        pixels=list(image.getdata());opaque=all(a==255 for _,_,_,a in pixels)
        if any(a not in (0,255) for _,_,_,a in pixels):raise ValueError('Partial alpha requires an explicit host-flattened background')
        spans=[];packed=[]
        def rgb(pixel):r,g,b,_=pixel;return ((r>>3)<<11)|((g>>2)<<5)|(b>>3)
        if opaque:packed=[rgb(p) for p in pixels]
        else:
            for y in range(h):
                x=0
                while x<w:
                    if not pixels[y*w+x][3]:x+=1;continue
                    start=x;offset=len(packed)
                    while x<w and pixels[y*w+x][3]:packed.append(rgb(pixels[y*w+x]));x+=1
                    spans.append((start,y,x-start,0,offset))
        if packed:source.append('static const uint16_t '+symbol+'_pixels[]={'+','.join(map(str,packed))+'};')
        if spans:source.append('static const struct ln_game_span '+symbol+'_spans[]={'+','.join('{'+','.join(map(str,s))+'}' for s in spans)+'};')
        declarations.append('extern const struct ln_game_sprite '+symbol+';')
        source.append('const struct ln_game_sprite '+symbol+'={'+f'{w},{h},{len(spans)},'+(symbol+'_spans' if spans else '0')+','+(symbol+'_pixels' if packed else '0')+'};')
        report.append(dict(name=symbol,kind='rgb565' if opaque else 'visible-spans',source=str(path),width=w,height=h,prepared_bytes=len(packed)*2+len(spans)*12))
    for item in tables:
        if not isinstance(item,dict) or set(item)!={'name','source','type'} or item['type'] not in ('u8','u16','s16'):
            raise ValueError('Invalid prepared table')
        symbol=name(item);path=_source(project,item['source'])
        if path.stat().st_size>1024*1024:raise ValueError('Prepared table source exceeds limit')
        values=json.loads(path.read_text(encoding='utf-8'));flat=[];kind=item['type']
        def flatten(value,depth=0):
            if depth>8 or len(flat)>131072:raise ValueError('Table exceeds dimensions/count limits')
            if isinstance(value,list):
                for child in value:flatten(child,depth+1)
            else:
                minimum=-32768 if kind=='s16' else 0;maximum=255 if kind=='u8' else 32767 if kind=='s16' else 65535
                if type(value) is not int or not minimum<=value<=maximum:raise ValueError('Table value exceeds its native type')
                flat.append(value)
        flatten(values)
        if not flat or len(flat)*(1 if kind=='u8' else 2)>262144:raise ValueError('Prepared table exceeds native slot')
        ctype={'u8':'uint8_t','u16':'uint16_t','s16':'int16_t'}[kind]
        declarations.append(f'extern const {ctype} {symbol}[{len(flat)}];')
        source.append(f'const {ctype} {symbol}[]={{'+','.join(map(str,flat))+'};')
        report.append(dict(name=symbol,kind=kind,source=str(path),prepared_bytes=len(flat)*(1 if kind=='u8' else 2)))
    declarations.append('#endif')
    (output/'game_assets.h').write_text('\n'.join(declarations)+'\n',encoding='utf-8')
    (output/'game_assets.c').write_text('\n'.join(source)+'\n',encoding='utf-8')
    return output/'game_assets.c',report
