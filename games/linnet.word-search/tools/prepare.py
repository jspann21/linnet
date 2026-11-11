"""Regenerate validated Word Search boards on the host (stable authoring seed)."""
from pathlib import Path
import json
import random

WORDS=(('MUSIC','CLICK','PLAY','MENU','SONG'),
       ('PODCAST','ALBUM','ARTIST','SHUFFLE','TRACK','PHOTO','VIDEO'),
       ('PLAYLIST','HEADPHONE','CLASSIC','LIBRARY','COVERFLOW','BATTERY','SCREEN','WHEEL','LINNET'))


def prepare():
    rng=random.Random(0x4c4e4757);boards=[]
    for level,words in enumerate(WORDS):
        width,height=((8,8),(10,10),(12,10))[level];variants=[]
        for variant in range(8):
            grid=[0]*120;begin=[];end=[]
            for word in words:
                options=[]
                for y in range(height):
                    for x in range(width):
                        for dx,dy in ((1,0),(1,1),(0,1),(-1,1),(-1,0),(-1,-1),(0,-1),(1,-1)):
                            ex,ey=x+dx*(len(word)-1),y+dy*(len(word)-1)
                            if not (0<=ex<width and 0<=ey<height):continue
                            cells=[(y+dy*i)*width+x+dx*i for i in range(len(word))]
                            if all(grid[at] in (0,ord(c)) for at,c in zip(cells,word)):options.append(cells)
                if not options:raise ValueError('No valid placement; change authoring seed')
                cells=rng.choice(options)
                for at,c in zip(cells,word):grid[at]=ord(c)
                begin.append(cells[0]);end.append(cells[-1])
            for at in range(width*height):
                if not grid[at]:grid[at]=ord('A')+rng.randrange(26)
            begin+=[0]*(9-len(begin));end+=[0]*(9-len(end))
            for index,word in enumerate(words):
                start,finish=begin[index],end[index]
                dx=(finish%width>start%width)-(finish%width<start%width)
                dy=(finish//width>start//width)-(finish//width<start//width)
                assert ''.join(chr(grid[(start//width+dy*i)*width+start%width+dx*i]) for i in range(len(word)))==word
            variants.append(grid+begin+end)
        boards.append(variants)
    output=Path(__file__).resolve().parents[1]/'levels/boards.json'
    output.parent.mkdir(exist_ok=True)
    output.write_text(json.dumps(boards,separators=(',',':'))+'\n',encoding='utf-8')


if __name__=='__main__':prepare()
