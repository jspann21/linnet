"""Host-only lowering of approved theme layouts into bounded native rectangles.

Applications keep their semantic nodes and service bindings. Every theme emits
its own complete scene table; no runtime layout parser or allocation is needed.
"""
from copy import deepcopy
from .tsx import COLOR_ROLES

TOKEN=0x10000
CENTER=8
RIGHT=16
WRAP=32

def color(name): return TOKEN+COLOR_ROLES.index(name)

def place(node,x,y,w,h):
    node.update(x=x,y=y,w=w,h=h)
    return node

def text_node(template,text,x,y,w,h,flags=0,fg='foreground'):
    node=deepcopy(template)
    node.update(kind=3,component='Text',x=x,y=y,w=w,h=h,fg=color(fg),bg=color('background'),
                flags=flags,count=0,resource=0xffffffff,index=0,text=text,items=[],targets=[],item_count=0,deps=4,canvas='')
    return node

def place_video_controls(scene,width,height,theme):
    """Keep video native size while following each theme's type and spacing."""
    if scene['id']!='linnet.system.video-controls':return
    nodes=scene['nodes']
    surface=next(n for n in nodes if n['kind']==16)
    header=next(n for n in nodes if n['kind']==11)
    progress=next(n for n in nodes if n['kind']==10)
    hint=next(n for n in nodes if n['kind']==3)
    top=(21 if theme['layout']=='ipod-photo' else 20) if height==176 else 24
    bottom=145 if height==176 else 201
    inset=max(8,min(12,theme['metrics']['padding']))
    place(surface,0,0,width,height)
    place(header,0,0,width,top);header['flags']=0
    place(progress,inset,bottom+7,width-2*inset,4);progress['flags']=0
    place(hint,inset,bottom+15,width-2*inset,height-bottom-15);hint['flags']=64
    hint['text']='MENU Exit  PLAY Pause  < > Seek'

def apply_layout(scenes,theme,compact=False):
    if theme['layout']=='classic-split':
        base=dict(theme,layout='default' if compact else 'ipod-photo')
        return apply_layout(scenes,base,compact)
    result=deepcopy(scenes)
    style=theme['layout']
    if style in ('default','minimal'):
        body=(0,20,220,156) if compact else (4,26 if style=='minimal' else 24,212,146 if style=='minimal' else 148)
        for scene in result:
            refine_scene(scene,theme,body)
            if style=='minimal':refine_minimal(scene,theme)
            refine_footers(scene,theme)
            align_scrollbars(scene,theme)
            place_video_controls(scene,220,176,theme)
        return result
    font_h=theme['font']['height']
    # Each body rectangle is its own layout, not a recolor of the default shell.
    bodies={'ipod-photo':(0,22,220,154),'graphite':(10,28,200,124),
            'aurora':(34,38,178,132),'sunset':(12,34,196,118),'blueprint':(8,8,204,142)}
    headers={'ipod-photo':(0,0,220,22),'graphite':(0,0,220,26),
             'aurora':(34,7,178,25),'sunset':(0,0,220,30),'blueprint':(0,154,220,22)}
    bx,by,bw,bh=bodies[style]
    for scene,source in zip(result,scenes):
        original=source['nodes']
        for node,old in zip(scene['nodes'],original):
            kind=node['kind']
            if kind==1: continue
            if kind==11:
                place(node,*headers[style])
                if style=='ipod-photo' and scene['id']=='linnet.system.home':node['text']='iPod'
                continue
            if kind==16 and old['x']==0 and old['y']==0 and old['w']==220 and old['h']==176:
                # Actual fullscreen video still owns the complete display.
                continue
            x=bx+(old['x']*bw)//220
            y=by+(max(0,old['y']-24)*bh)//152
            w=max(1,(old['w']*bw)//220)
            h=max(1,(old['h']*bh)//152)
            if kind in (3,12,13):h=max(h,font_h+(8 if kind in (12,13) else 0))
            place(node,x,min(y,176-h),min(w,220-x),min(h,176-y))
            if kind in (6,7,8):
                # Lists use all of their allotted rows; static footers retain space.
                if old['y']==24:
                    node['y']=by
                    node['h']=bh if old['h']>=148 else max(theme['metrics']['row_height'],bh-24)
                if style=='ipod-photo':
                    node['x']=0;node['w']=210 if any(n['kind']==9 for n in original) else 220
                    if old['y']==24:node['h']=154 if old['h']>=148 else 132
                elif style=='graphite':node['x']=10;node['w']=194 if any(n['kind']==9 for n in original) else 200
                elif style=='aurora':node['x']=34;node['w']=172 if any(n['kind']==9 for n in original) else 178
                elif style=='sunset':node['x']=12;node['w']=190 if any(n['kind']==9 for n in original) else 196
                elif style=='blueprint':node['x']=8;node['w']=196 if any(n['kind']==9 for n in original) else 204
            if kind==9:
                node['x']=210 if style=='ipod-photo' else bx+bw-4
                node['w']=10 if style=='ipod-photo' else 4
                listing=next((n for n in scene['nodes'] if n['kind'] in (6,7,8)),None)
                if listing:node['y']=listing['y'];node['h']=listing['h']
            if kind==3:node['flags']|=WRAP
        nodes=scene['nodes']
        if scene['id']=='linnet.system.update' and style=='graphite':
            lines=[n for n in nodes if n['kind']==3]
            for i,node in enumerate(lines):place(node,12,36+i*18,196,18);node['flags']=0
            menu=next(n for n in nodes if n['kind']==7);place(menu,10,126,200,26)
        if scene['id']=='linnet.system.home':
            menu=next(n for n in nodes if n['kind']==7)
            if style=='blueprint':place(menu,8,8,204,138)
        refine_scene(scene,theme,(bx,by,bw,bh))
        refine_footers(scene,theme)
        if style=='ipod-photo':refine_ipod_photo(scene,theme)
        align_scrollbars(scene,theme)
        # Fixed clipping bounds remain a compiler invariant for every variant.
        for node in nodes:
            node['w']=max(1,min(node['w'],220-node['x']))
            node['h']=max(1,min(node['h'],176-node['y']))
        place_video_controls(scene,220,176,theme)
        scene['count']=len(nodes)
    return result


def place_classic_split_home(scenes,width,height):
    """Final-pixel Home geometry; smaller panels keep useful label width."""
    header,left,row={320:(24,160,27),220:(22,132,22),160:(18,104,18)}[width]
    for scene in scenes:
        if scene['id']!='linnet.system.home':continue
        for node in scene['nodes']:
            if node['kind']==11:
                place(node,0,0,width,header);node['text']='Linnet'
            elif node['kind']==7:
                place(node,0,header,left,height-header)
                node['row_height']=row
            elif node['kind']==9:
                node['flags']|=2


def fit_monochrome_text(scenes,font,height):
    """Reserve native font lines after the authoring rectangles are scaled."""
    fh=font['height']
    rows={'about':1,'themes':1,'music-sound':1,'sleep-timer':1,
          'music-search':3,'shutdown':3}
    for scene in scenes:
        sid=scene['id'].rsplit('.',1)[-1]
        texts=[n for n in scene['nodes'] if n['kind']==3 and not n['flags']&2]
        listing=next((n for n in scene['nodes'] if n['kind'] in (6,7) and n['flags']&1),None)
        for node in texts:
            if node['flags']&64:
                bottom=min(height-2,node['y']+node['h'])
                node['h']=max(node['h'],rows.get(sid,2)*(fh+1)-1)
                node['y']=bottom-node['h']
                if listing:
                    listing['h']=min(listing['h'],node['y']-3-listing['y'])
            elif node['text'] in ('Search prefix','LINNET OS'):
                node['h']=max(node['h'],fh)
        if sid=='music-search' and listing:
            prefix=next(n for n in texts if n['text']=='Search prefix')
            header=next(n for n in scene['nodes'] if n['kind']==11)
            prefix['y']=header['y']+header['h']+2
            bottom=listing['y']+listing['h']
            listing['y']=prefix['y']+prefix['h']+2
            listing['h']=bottom-listing['y']
        if listing:
            for node in scene['nodes']:
                if node['kind']==9:node['y']=listing['y'];node['h']=listing['h']


def align_scrollbars(scene,theme):
    listing=next((n for n in scene['nodes'] if n['kind'] in (6,7,8) and n['flags']&1),None)
    if not listing:return
    for bar in scene['nodes']:
        if bar['kind']!=9:continue
        if theme['layout']=='ipod-photo':
            place(bar,210,listing['y'],10,listing['h'])
            continue
        bar['y']=listing['y']
        bar['h']=listing['h']
        for node in scene['nodes']:
            if node['kind']==3:node['w']=min(node['w'],bar['x']-node['x']-4)
        listing['w']=min(listing['w'],bar['x']-listing['x']-2)


def refine_footers(scene,theme):
    """Keep supporting text on one bottom grid, above each theme's chrome."""
    sid=scene['id'].rsplit('.',1)[-1]
    keys={'themes':{'Theme status'},'clock':{'Clock source'},
          'clock-edit':{'Clock edit hint'},'sleep-timer':{'Sleep timer status'},'music-search':{'Search controls'}}
    texts=[n for n in scene['nodes'] if n['kind']==3]
    if sid in keys:foot=[n for n in texts if n['text'] in keys[sid]]
    elif sid in ('display','battery','shutdown'):foot=texts[0 if sid=='display' else 1:]
    elif sid in ('music','music-queue','music-sound','video-settings','video-output','files'):foot=texts
    else:return
    if not foot:return
    style=theme['layout']
    x,w,bottom={'aurora':(38,170,172),'graphite':(14,192,152),
                'sunset':(16,188,152),'blueprint':(8,204,150)}.get(style,(6,208,172))
    if len(foot)>1:
        foot[0]['text']='  '.join(n['text'] for n in foot)
        for n in foot[1:]:n['flags']|=2
    height=36 if sid in ('clock','display','battery','shutdown') or (sid=='music-search' and style=='aurora') else 24 if sid=='clock-edit' else 12
    place(foot[0],x,bottom-height,w,height);foot[0]['flags']=64
    for n in scene['nodes']:
        if n['kind'] in (6,7) and n['y']<bottom-height and n['y']+n['h']>bottom-height-4:
            n['h']=max(18,bottom-height-4-n['y'])
    listing=next((n for n in scene['nodes'] if n['kind'] in (6,7)),None)
    if listing:
        for n in scene['nodes']:
            if n['kind']==9:n['h']=min(n['h'],listing['h'])


def refine_scene(scene,theme,body):
    """Place service text and media explicitly rather than compressing baselines."""
    style=theme['layout'];x,y,w,h=body;bottom=y+h
    fh=theme['font']['height'];rh=theme['metrics']['row_height']
    nodes=scene['nodes'];sid=scene['id'].rsplit('.',1)[-1]
    texts=[n for n in nodes if n['kind']==3]
    listing=next((n for n in nodes if n['kind'] in (6,7,8)),None)
    if listing and listing['kind']==6:rh=max(18,fh+5)
    if scene['id']=='linnet.system.ui-demo':
        top_h=2*fh if style=='aurora' else fh
        for n in texts:place(n,x+4,y,w-8,top_h);n['flags']=WRAP
        toast_h=fh+6;dialog_h=fh+10;toast_y=bottom-toast_h;dialog_y=toast_y-4-dialog_h
        visual_y=y+top_h+5;visual_h=max(8,dialog_y-5-visual_y);half=(w-12)//2
        for n in nodes:
            if n['kind']==17:place(n,x+4,visual_y,half,visual_h)
            elif n['kind']==15:place(n,x+8+half,visual_y,half,visual_h)
            elif n['kind']==12:place(n,x if style=='aurora' else x+4,dialog_y,w if style=='aurora' else w-8,dialog_h)
            elif n['kind']==13:place(n,x+4,toast_y,w-8,toast_h)
    elif sid=='about':
        # Keep the logo visible above a scrollable, host-prepared fact list.
        art_h=min(28,max(22,h//6));art_w=art_h*68//104
        art=next(n for n in nodes if n['kind']==17)
        place(art,x+4,y+2,art_w,art_h)
        place(texts[0],x+art_w+12,y+(art_h-fh)//2,w-art_w-16,fh+2)
        texts[1]['flags']|=2
        texts[0]['flags']=0
        progress=next(n for n in nodes if n['kind']==10)
        place(progress,x+4,y+art_h+3,w-8,4)
        list_y=y+art_h+10
        list_h=max(rh,((bottom-14-list_y)//rh)*rh)
        place(listing,x,list_y,w-10,list_h)
        place(texts[2],x+4,bottom-12,w-8,12);texts[2]['flags']=64
    elif sid=='update' and style=='sunset':
        for i,n in enumerate(texts):place(n,12,40+i*17,196,17);n['flags']=0
        place(listing,12,126,196,26)
    elif sid=='games':
        footer_h=24 if style=='aurora' else 12
        music_h=max(12,fh+1)
        if style=='aurora':y=34;bottom=172;h=bottom-y
        place(listing,x,y,w-10,max(rh,h-footer_h-music_h-4))
        for n in texts:
            if n['text']=='Game music':place(n,x,bottom-footer_h-music_h,w,music_h)
            else:place(n,x,bottom-footer_h,w,footer_h)
            n['flags']=64
    elif sid=='clock':
        clock_h=2*fh+4
        row_h=max(20,fh+6)
        place(listing,x,y+clock_h+8,w,2*row_h)
        for n in texts:
            if n['text']=='System clock':place(n,x+4,y,w-8,clock_h);n['flags']=CENTER
            elif n['text']=='Clock source':place(n,x+4,bottom-2*fh,w-8,2*fh);n['flags']=WRAP
    elif sid=='clock-edit':
        field_h=24 if style=='graphite' else 22 if style=='ipod-photo' else max(18,fh+3)
        list_h=max(field_h,((h-28)//field_h)*field_h)
        place(listing,x,y,w-8,list_h)
        for n in texts:place(n,x+4,bottom-2*fh,w-8,2*fh);n['flags']=WRAP
    elif sid=='sleep-timer':
        place(listing,listing['x'],y,listing['w'],max(rh,((h-32)//rh)*rh))
        for n in texts:place(n,x+4,bottom-26,w-8,26);n['flags']=WRAP
    elif sid in ('display','battery','shutdown'):
        if sid=='shutdown' and style=='sunset':y=42
        list_h=max(50,(3 if sid=='shutdown' else 2)*rh) if sid in ('display','shutdown') else rh
        if sid=='shutdown' and theme['flags']&1:list_h=max(list_h,80)
        place(listing,x,y,w,list_h)
        note_gap=4 if sid=='shutdown' else max(50,2*rh)-2*rh+8
        for i,n in enumerate(texts):
            top=y+list_h+note_gap if i==0 and sid!='display' else bottom-(len(texts)-i)*(fh+5)
            place(n,x+4,top,w-8,fh);n['flags']=0
    elif sid in ('music','music-sound','music-queue','themes','files'):
        footer_h=12
        place(listing,listing['x'],y,listing['w'],max(rh,((h-footer_h-8)//rh)*rh))
        for n in texts:place(n,x+4,bottom-footer_h,w-8,footer_h);n['flags']=WRAP
    elif sid=='music-search':
        prefix=next(n for n in texts if n['text']=='Search prefix')
        footer=next(n for n in texts if n['text']=='Search controls')
        prefix_y=y
        place(prefix,x+4,prefix_y,w-8,fh);prefix['flags']=0
        top=prefix_y+fh+3;foot=bottom-12
        place(listing,listing['x'],top,listing['w'],max(rh,((foot-top-4)//rh)*rh))
        place(footer,x+4,foot,w-8,2*fh);footer['flags']=WRAP
    elif sid=='photos-grid':
        place(listing,x,y,w,h)
        for n in texts:place(n,x+8,y+(h-2*fh)//2,w-16,2*fh);n['flags']=CENTER
    elif sid=='video-controls':
        surface=next(n for n in nodes if n['kind']==16)
        bar=next(n for n in nodes if n['kind']==10)
        # Keep the full authoring canvas behind the controls. Scaling this surface
        # resamples every video frame on ARM7 and defeats direct presentation.
        place(surface,0,0,220,176)
        place(bar,x,bottom-fh-13,w,5)
        for n in texts:place(n,x,bottom-fh,w,fh);n['flags']=0
    elif sid=='now-playing':
        title=next(n for n in texts if n['text']=='Sample track')
        artist=next(n for n in texts if n['text']=='Audio pending')
        status=next(n for n in texts if n['text']=='Native playback shell')
        art=next(n for n in nodes if n['kind']==14)
        bar=next(n for n in nodes if n['kind']==10)
        header=next(n for n in nodes if n['kind']==11);header['text']='Now Playing'
        status['deps']|=16
        # Metadata owns stable lines; notices never displace it or disappear.
        inset=6 if style=='ipod-photo' else 4
        left=x+inset;width=w-2*inset;top=y+(2 if h<120 else 4)
        if style=='ipod-photo':
            # Photo-era composition: position above art, metadata to its right.
            header=next(n for n in nodes if n['kind']==11);header['text']='Now Playing'
            left=10;width=200
            place(art,10,46,70,70)
            nodes.append(text_node(title,'Track position',10,29,200,fh))
            place(title,90,50,120,2*fh);title['flags']=WRAP
            place(artist,90,86,120,fh);artist['flags']=0;artist['fg']=color('foreground')
            nodes.append(text_node(title,'Track album',90,106,120,fh))
        elif style=='aurora':
            place(art,left+(width-32)//2,top,32,32)
            place(title,left,top+36,width,2*fh);title['flags']=WRAP
            place(artist,left,top+38+2*fh,width,fh);artist['flags']=CENTER
        else:
            size=56 if h<120 else 64
            lines=2 if h<120 else 3
            place(art,left,top,size,size)
            place(title,left+size+8,top,width-size-8,lines*fh);title['flags']=WRAP
            place(artist,left+size+8,top+lines*fh+4,width-size-8,2*fh);artist['flags']=WRAP
            if h>=138:
                nodes.append(text_node(title,'Track album',left,top+size+8,width,fh,0,'muted'))
        place(status,left,bottom-2*fh-14,width,fh);status['flags']=0;status['fg']=color('muted')
        place(bar,left,bottom-fh-10,width,4)
        if style=='ipod-photo':
            place(status,left,124,width,fh)
            place(bar,left,142,width,10)
        nodes.extend([text_node(title,'Track elapsed',left,bottom-fh-2,(width-12)//2,fh),
                      text_node(title,'Track remaining',left+(width+12)//2,bottom-fh-2,(width-12)//2,fh,RIGHT)])
    if listing:
        for n in nodes:
            if n['kind']==9:n['y']=listing['y'];n['h']=listing['h']
    scene['count']=len(nodes)


def refine_minimal(scene,theme):
    nodes=scene['nodes'];fh=theme['font']['height']
    for n in nodes:
        if n['kind']==11:place(n,0,2,220,20)
        elif n['kind'] in (4,5,14):n['flags']|=2
        elif n['kind']==2:n['bg']=color('background');n['flags']&=~4
    if scene['id']=='linnet.system.now-playing':
        for n in nodes:
            if n['text']=='Sample track':place(n,8,34,204,2*fh);n['flags']=WRAP
            elif n['text']=='Audio pending':place(n,8,68,204,2*fh);n['flags']=WRAP
            elif n['text']=='Track album':place(n,8,102,204,fh)
    scene['count']=len(nodes)


def refine_ipod_photo(scene,theme):
    """Photo-style chrome and content grid in the authoring coordinate space."""
    nodes=scene['nodes'];sid=scene['id'].rsplit('.',1)[-1]
    fh=theme['font']['height'];rh=22
    texts=[n for n in nodes if n['kind']==3]
    listing=next((n for n in nodes if n['kind'] in (6,7,8)),None)
    scroll=next((n for n in nodes if n['kind']==9),None)
    header=next((n for n in nodes if n['kind']==11),None)
    if header:
        place(header,0,0,220,21)
        names={'home':'iPod','about':'About','display':'Display',
               'photos-options':'Photo Options','music-sound':'Sound',
               'clock-edit':'Date & Time','music-options':'Playback Options'}
        if sid in names:header['text']=names[sid]
    if listing and sid!='about':
        # White edge at the right keeps the thumb and chevrons inside the LCD.
        top=21 if listing['y']<=24 else listing['y']
        bottom=min(176,listing['y']+listing['h']+1)
        place(listing,0,top,220,max(rh,((bottom-top)//rh)*rh))
        if listing['kind']==8:place(listing,0,21,210,155)
    if sid=='settings' and listing:
        labels={'About Linnet':'About','Display':'Display','Clock':'Date & Time'}
        listing['items']=[labels.get(label,label) for label in listing['items']]
    elif sid=='now-playing':
        title=next(n for n in texts if n['text']=='Sample track')
        artist=next(n for n in texts if n['text']=='Audio pending')
        album=next(n for n in texts if n['text']=='Track album')
        # A single baseline for each field matches the original player layout.
        for n,y in ((title,47),(artist,72),(album,97)):
            place(n,10,y,200,fh);n['flags']=0;n['fg']=color('foreground')
        position=next(n for n in texts if n['text']=='Track position');place(position,10,29,200,fh);position['deps']|=16
        art=next(n for n in nodes if n['kind']==14);place(art,10,46,70,70)
        notice=next(n for n in texts if n['text']=='Native playback shell');place(notice,10,115,200,fh)
        bar=next(n for n in nodes if n['kind']==10);place(bar,11,134,198,11)
        for n in texts:
            if n['text']=='Track elapsed':place(n,10,150,94,fh)
            elif n['text']=='Track remaining':place(n,116,150,94,fh)
    elif sid=='themes' and listing:
        place(listing,0,21,220,132)
        for n in texts:place(n,6,155,208,17)
    elif sid in ('display','battery','shutdown','music','music-queue','music-sound','music-search','clock-edit','clock'):
        for n in texts:
            n['x']=6;n['w']=208
            n['h']=min(n['h'],172-n['y'])
            if n['h']<fh:n['y']=172-fh;n['h']=fh
    elif sid=='photos-viewer':
        surface=next(n for n in nodes if n['kind']==15);place(surface,0,21,220,155)
    elif sid=='cover-flow':
        surface=next(n for n in nodes if n['kind']==17);place(surface,0,0,220,140)
        for n in texts:
            place(n,4,141 if n['text']=='Cover Flow album' else 158,212,fh)
            n['flags']=0
    if sid in ('music','music-queue','music-sound') and listing:
        place(listing,0,21,220,132)
        for n in texts:place(n,6,155,208,17);n['flags']=64
    if scroll and listing:
        place(scroll,210,listing['y'],10,listing['h'])
    scene['count']=len(nodes)


def feature_enabled(features, name):
    return features.get(name, name != 'demo')


def gate_features(scenes,features):
    """Keep stable scene slots while removing links into disabled services."""
    groups={
        'cover_flow':{'linnet.system.cover-flow'},
        'files':{'linnet.system.files'},
        'demo':{'linnet.system.ui-demo'},
        'photos':{'linnet.system.photos','linnet.system.photos-grid','linnet.system.photos-viewer','linnet.system.photos-options'},
        'video':{'linnet.system.videos','linnet.system.video-player','linnet.system.video-controls'},
        'video_settings':{'linnet.system.video-settings','linnet.system.video-output'},
        'themes':{'linnet.system.themes'},
    }
    disabled={sid for feature,ids in groups.items() if not feature_enabled(features,feature) for sid in ids}
    enabled=[scene['id'] not in disabled for scene in scenes]
    for scene in scenes:
        for node in scene['nodes']:
            if not node['targets']:continue
            keep=[i for i,target in enumerate(node['targets']) if target>=len(scenes) or enabled[target]]
            node['items']=[node['items'][i] for i in keep]
            node['targets']=[node['targets'][i] for i in keep]
            node['count']=node['item_count']=len(keep)
            node['index']=min(node['index'],max(0,len(keep)-1))
    return sum(1<<i for i,available in enumerate(enabled) if available)
