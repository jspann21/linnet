"""Explicit host font baking; normal content builds use the checked-in FONT pack.

Requires Pillow. Supply a redistributable font and retain its license alongside
the output. Default baking retains the original fixed 8x14 glyph cell;
proportional packs store bounded ASCII glyph cells and individual advances.
Monochrome baking asks FreeType to hint directly onto the target pixel grid.
"""
import argparse
import hashlib
import json
from pathlib import Path

def bake(font_path, output, size=11, proportional=False, height=14, monochrome=False):
    from PIL import Image, ImageDraw, ImageFont
    import io
    if type(size) is not int or not 1 <= size <= 24 or type(height) is not int or not 1 <= height <= 24:
        raise ValueError('font size and cell height must be integers in 1..24')
    source=Path(font_path).read_bytes()
    font=ImageFont.truetype(io.BytesIO(source),size)
    glyphs=[chr(code) for code in range(32,127)]
    if proportional:
        bounds=[font.getbbox(char) for char in glyphs]
        left=min(0,min(box[0] for box in bounds))
        top=min(box[1] for box in bounds if box[3]>box[1])
        bottom=max(box[3] for box in bounds)
        cell_width=max(box[2] for box in bounds)-left
        advances=[max(1,int(font.getlength(char)+0.5)) for char in glyphs]
        if not 1 <= cell_width <= 24 or max(advances)>24 or bottom-top>height:
            raise ValueError('font glyphs exceed bounded cell geometry')
        y=(height-(bottom-top))//2-top
        alpha=[]
        for char in glyphs:
            cell=Image.new('L',(cell_width,height))
            draw=ImageDraw.Draw(cell)
            if monochrome: draw.fontmode='1'
            draw.text((-left,y),char,font=font,fill=255)
            alpha.extend(cell.tobytes())
        value=dict(format='FONT',width=cell_width,height=height,advance=max(advances),
                   advances=advances,first=32,count=95,alpha=alpha,
                   source_sha256=hashlib.sha256(source).hexdigest())
    else:
        value=_fixed(font,height,source,monochrome)
    Path(output).parent.mkdir(parents=True,exist_ok=True)
    Path(output).write_text(json.dumps(value,sort_keys=True,separators=(',',':'))+'\n',encoding='ascii')
    return value


def _fixed(font,height,source,monochrome=False):
    from PIL import Image, ImageDraw
    alpha=[]
    for code in range(32,127):
        width=max(8,int(font.getlength(chr(code))+1))
        glyph=Image.new('L',(width,height))
        draw=ImageDraw.Draw(glyph)
        if monochrome: draw.fontmode='1'
        draw.text((0,0),chr(code),font=font,fill=255)
        if width>8: glyph=glyph.resize((8,height),Image.Resampling.LANCZOS)
        alpha.extend(glyph.tobytes())
    return dict(format='FONT',width=8,height=height,advance=8,first=32,count=95,
               alpha=alpha,source_sha256=hashlib.sha256(source).hexdigest())

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--size',type=int,default=11)
    parser.add_argument('--height',type=int,default=14)
    parser.add_argument('--proportional',action='store_true')
    parser.add_argument('--monochrome',action='store_true')
    args=parser.parse_args(); bake(args.font,args.output,args.size,args.proportional,args.height,args.monochrome)
