"""Authored 320 x 240 layouts for color displays.

Use native scene bindings, with independent body, media and text geometry.
These rectangles never pass through the Photo layout or its scaling step.
"""
from copy import deepcopy
from tools.content.layouts import place, place_video_controls, text_node, CENTER, RIGHT, WRAP
from tools.content.tsx import ContentError


def mark_control_footer(scene, footer=None):
    sid = scene['id'].rsplit('.', 1)[-1]
    if sid in ('display', 'battery', 'shutdown'):
        hints = [node for node in scene['nodes'] if node['kind'] == 3][0 if sid == 'display' else 1:]
        if len(hints) != 2:
            return
        hints[0]['text'] += '  ' + hints[1]['text']
        hints[0]['flags'] = 64
        hints[1]['flags'] |= 2
        if footer:
            place(hints[0], *footer)
        return
    if sid not in ('music', 'music-queue', 'music-sound', 'music-search',
                   'video-settings', 'video-output', 'files'):
        return
    for node in scene['nodes']:
        if node['kind'] == 3 and (sid != 'music-search' or node['text'] == 'Search controls'):
            node['flags'] = (node['flags'] & 2) | 64


def apply_layout(scenes, theme):
    # The game callback paints every pixel, including Solitaire's chrome.
    # Skip redundant screen/canvas clears on the full-size arcade surface.
    scenes = deepcopy(scenes)
    for scene in scenes:
        if scene['id'] == 'linnet.system.game-play':
            for node in scene['nodes']:
                if node['kind'] == 17 and (node['x'], node['y'], node['w'], node['h']) == (0, 0, 320, 240):
                    node['flags'] |= 64
    style = theme['layout']
    if style == 'native':
        result = deepcopy(scenes)
        for scene in result:
            place_video_controls(scene, 320, 240, theme)
            mark_control_footer(scene, (8, 212, 304, 24))
        return result
    if style in ('ipod-classic', 'classic-split'):
        result = apply_ipod_classic(scenes, theme)
        if style == 'classic-split':
            for scene in result:
                if scene['id'] == 'linnet.system.home':
                    next(n for n in scene['nodes'] if n['kind'] == 11)['text'] = 'Linnet'
        return result
    # Graphite/Sunset reserve the bottom 30 pixels for transport chrome;
    # Aurora reserves 38 pixels at the left. Blueprint puts status below content.
    shells = {
        'default': ((12, 36, 296, 196), (0, 0, 320, 28)),
        'minimal': ((12, 40, 296, 192), (12, 6, 296, 26)),
        'graphite': ((16, 42, 288, 160), (0, 0, 320, 32)),
        'aurora': ((48, 40, 260, 192), (48, 6, 260, 28)),
        'sunset': ((16, 44, 288, 158), (0, 0, 320, 34)),
        'blueprint': ((12, 10, 296, 188), (0, 212, 320, 28)),
    }
    if style not in shells:
        raise ContentError('unsupported M25 theme layout')
    body, header = shells[style]
    x, y, w, h = body
    bottom = y + h
    fh = theme['font']['height']
    row = max(18, fh + 5)
    result = deepcopy(scenes)
    for scene in result:
        sid = scene['id'].rsplit('.', 1)[-1]
        nodes = scene['nodes']
        texts = [n for n in nodes if n['kind'] == 3]
        listing = next((n for n in nodes if n['kind'] in (6, 7, 8)), None)
        bars = [n for n in nodes if n['kind'] == 9]
        for node in nodes:
            if node['kind'] == 11:
                place(node, *header)
            elif style == 'minimal' and node['kind'] in (4, 5, 14):
                node['flags'] |= 2
        if listing:
            place(listing, x, y, w - (12 if bars else 0), h)
        # Fullscreen media owns its full native surface. Controls use their own
        # overlay header even when the theme normally has a bottom status bar.
        if sid in ('cover-flow', 'video-player'):
            continue
        if sid == 'video-controls':
            place_video_controls(scene, 320, 240, theme)
            continue
        if sid == 'photos-viewer':
            surface = next(n for n in nodes if n['kind'] == 15)
            place(surface, 0, 0 if style == 'blueprint' else 28, 320, 212)
            for node in nodes:
                if node['kind'] == 11:
                    place(node, 0, 212 if style == 'blueprint' else 0, 320, 28)
            continue
        if sid in ('music', 'music-queue', 'music-sound', 'themes', 'sleep-timer', 'clock-edit', 'video-settings', 'files'):
            footer = 28 if sid in ('sleep-timer', 'clock-edit') else 14
            field_row = 24 if style == 'graphite' else max(18, fh + 3)
            step = field_row if sid == 'clock-edit' else row
            listing['h'] = ((h - footer - 4) // step) * step
            for node in texts:
                place(node, x, bottom - footer, w, footer)
                node['flags'] = WRAP
        elif sid == 'games':
            # Menu rows use the theme metric, unlike VirtualList rows.
            step = 46 if style == 'blueprint' else theme['metrics']['row_height']
            line = max(14, fh + 1)
            listing['h'] = ((h - 2 * line - 4) // step) * step
            for node in texts:
                if node['text'] == 'Game description':
                    place(node, x, bottom - line, w, line); node['flags'] = 64
                elif node['text'] == 'Game music':
                    place(node, x, bottom - 2 * line, w, line); node['flags'] = 64
                else:
                    node['flags'] |= 2
        elif sid == 'music-search':
            place(texts[0], x, y, w, 22)
            place(listing, x, y + 26, w - 12, ((h - 44) // row) * row)
            place(texts[1], x, bottom - 14, w, 14)
            texts[1]['flags'] = WRAP
        elif sid in ('display', 'battery', 'shutdown'):
            place(listing, x, y, w, row * (3 if sid == 'shutdown' else 2 if sid == 'display' else 1))
            positions = ([bottom - 46, bottom - 22] if sid == 'display' else
                         [y + listing['h'] + 12, bottom - 46, bottom - 22])
            for node, top in zip(texts, positions):
                place(node, x, top, w, 22)
        elif sid == 'clock':
            place(texts[0], x, y, w, 38)
            texts[0]['flags'] = CENTER
            place(listing, x, y + 46, w - 12, 2 * row)
            place(texts[1], x, bottom - 38, w, 38)
            texts[1]['flags'] = WRAP
        elif sid == 'update':
            for i, node in enumerate(texts):
                place(node, x, y + i * 23, w, 21)
            place(listing, x, bottom - 32, w, 32)
        elif sid == 'now-playing':
            art = next(n for n in nodes if n['kind'] == 14)
            size = 80 if style in ('graphite', 'sunset') else 112
            place(art, x, y, size, size)
            tx = x + size + 12
            place(texts[0], tx, y, w - size - 12, 34)
            place(texts[1], tx, y + 40, w - size - 12, 34)
            texts[0]['flags'] = texts[1]['flags'] = WRAP
            place(texts[2], tx, y + 80, w - size - 12, 18)
            if style == 'aurora':
                place(art, x + (w - 48) // 2, y, 48, 48)
                for node, top, height in zip(texts, (y + 54, y + 92, y + 112), (34, 18, 18)):
                    place(node, x, top, w, height)
                    node['flags'] = CENTER | WRAP
            elif style == 'minimal':
                for node, top, height in zip(texts, (y + 8, y + 56, y + 104), (34, 34, 18)):
                    place(node, x, top, w, height)
            progress = next(n for n in nodes if n['kind'] == 10)
            place(progress, x, bottom - 38, w, 6)
            place(texts[3], x, bottom - 62, w, 18)
            texts[3]['deps'] |= 16
            nodes.extend([
                text_node(texts[0], 'Track elapsed', x, bottom - 22, (w - 12) // 2, 20),
                text_node(texts[0], 'Track remaining', x + (w + 12) // 2, bottom - 22, (w - 12) // 2, 20, RIGHT),
            ])
        elif sid == 'about':
            art = next(n for n in nodes if n['kind'] == 17)
            ah = min(40, max(32, h // 5))
            aw = ah * 68 // 104
            place(art, x + 4, y + 2, aw, ah)
            place(texts[0], x + aw + 16, y + (ah - fh) // 2, w - aw - 20, fh + 2)
            texts[1]['flags'] |= 2
            place(next(n for n in nodes if n['kind'] == 10), x + 4, y + ah + 7, w - 8, 7)
            list_y = y + ah + 18
            place(listing, x, list_y, w - 12, max(row, ((bottom - 18 - list_y) // row) * row))
            place(texts[2], x + 4, bottom - 14, w - 8, 14); texts[2]['flags'] = 64
            if bars: place(bars[0], x + w - 8, list_y, 8, listing['h'])
        elif sid == 'photos-grid':
            for node in texts:
                place(node, x + 8, y + h // 2 - 22, w - 24, 44)
                node['flags'] = CENTER | WRAP
        elif sid == 'video-output':
            place(listing, x, y + 12, w, row * 2)
            place(texts[0], x, bottom - 30, w, 24)
        elif scene['id'] == 'linnet.system.ui-demo':
            place(texts[0], x, y, w, 22)
            half = (w - 12) // 2
            for node in nodes:
                if node['kind'] == 17:
                    place(node, x, y + 30, half, h - 114)
                elif node['kind'] == 15:
                    place(node, x + half + 12, y + 30, half, h - 114)
                elif node['kind'] == 12:
                    place(node, x, bottom - 72, w, 34)
                elif node['kind'] == 13:
                    place(node, x, bottom - 30, w, 30)
        for bar in bars:
            if listing:
                place(bar, x + w - 4, listing['y'], 4, listing['h'])
        mark_control_footer(scene, (x, bottom - 14, w, 14))
        scene['count'] = len(nodes)
    return result


def apply_ipod_classic(scenes, theme):
    """Apple's 2007 Classic proportions, with Linnet services kept native."""
    result = deepcopy(scenes)
    fh = theme['font']['height']
    row = theme['metrics']['row_height']
    for scene in result:
        sid = scene['id'].rsplit('.', 1)[-1]
        nodes = scene['nodes']
        texts = [n for n in nodes if n['kind'] == 3]
        listing = next((n for n in nodes if n['kind'] in (6, 7, 8)), None)
        scroll = next((n for n in nodes if n['kind'] == 9), None)
        header = next((n for n in nodes if n['kind'] == 11), None)
        if header:
            place(header, 0, 0, 320, 24)
            names = {'home': 'iPod', 'about': 'About', 'display': 'Display',
                     'music-sound': 'Sound', 'music-options': 'Playback Options',
                     'clock-edit': 'Date & Time', 'photos-options': 'Photo Options'}
            if sid in names:
                header['text'] = names[sid]
        if sid in ('cover-flow', 'video-player'):
            continue
        if sid == 'video-controls':
            place_video_controls(scene, 320, 240, theme)
            continue
        if sid == 'photos-viewer':
            surface = next(n for n in nodes if n['kind'] == 15)
            place(surface, 0, 24, 320, 216)
            continue
        if listing:
            place(listing, 0, 24, 312 if scroll else 320, 216)
            if listing['kind'] == 8:
                place(listing, 0, 24, 312 if scroll else 320, 216)
            if scroll:
                place(scroll, 312, 24, 8, 216)
        if sid == 'home' and listing:
            place(listing, 0, 24, 160, 216)
            if scroll:
                scroll['flags'] |= 2
        elif sid in ('music', 'music-queue', 'music-sound', 'files', 'video-settings') and listing:
            place(listing, 0, 24, 312 if scroll else 320, 189)
            if scroll:place(scroll, 312, 24, 8, 189)
            for n in texts:
                place(n, 8, 216, 304, 22); n['flags'] = 64
        elif sid == 'music-search' and listing:
            prefix = next(n for n in texts if n['text'] == 'Search prefix')
            footer = next(n for n in texts if n['text'] == 'Search controls')
            place(prefix, 8, 28, 304, fh)
            place(listing, 0, 49, 312, 162)
            place(scroll, 312, 49, 8, 162)
            place(footer, 8, 216, 304, fh)
        elif sid == 'now-playing':
            art = next(n for n in nodes if n['kind'] == 14)
            title = next(n for n in texts if n['text'] == 'Sample track')
            artist = next(n for n in texts if n['text'] == 'Audio pending')
            album = next(n for n in texts if n['text'] == 'Track album')
            notice = next(n for n in texts if n['text'] == 'Native playback shell')
            progress = next(n for n in nodes if n['kind'] == 10)
            place(art, 18, 39, 126, 126)
            place(title, 153, 47, 153, fh); title['flags'] = 0
            place(artist, 153, 72, 153, fh); artist['flags'] = 0
            place(album, 153, 96, 153, fh); album['flags'] = 0
            place(notice, 18, 171, 136, fh); notice['flags'] = 0
            place(progress, 18, 197, 284, 9)
            nodes.extend([
                text_node(title, 'Track position', 171, 171, 131, fh, RIGHT),
                text_node(title, 'Track elapsed', 18, 211, 132, fh),
                text_node(title, 'Track remaining', 170, 211, 132, fh, RIGHT),
            ])
        elif sid == 'settings' and listing:
            labels = {'About Linnet': 'About', 'Display': 'Display',
                      'Clock': 'Date & Time', 'Video': 'Video Settings',
                      'Update / Recovery': 'Update'}
            listing['items'] = [labels.get(label, label) for label in listing['items']]
        elif sid == 'themes' and listing:
            place(listing, 0, 24, 312, 189)
            place(scroll, 312, 24, 8, 189)
            for n in texts:
                place(n, 8, 217, 304, fh); n['flags'] = 0
        elif sid == 'games' and listing:
            list_h = ((216 - 2 * fh - 10) // row) * row
            place(listing, 0, 24, 312, list_h)
            place(scroll, 312, 24, 8, list_h)
            for n in texts:
                if n['text'] == 'Game description':
                    place(n, 8, 240 - fh - 4, 304, fh); n['flags'] = 64
                elif n['text'] == 'Game music':
                    place(n, 8, 240 - 2 * fh - 6, 304, fh); n['flags'] = 64
                else:
                    n['flags'] |= 2
        elif sid == 'clock' and listing:
            clock = next(n for n in texts if n['text'] == 'System clock')
            source = next(n for n in texts if n['text'] == 'Clock source')
            place(clock, 8, 34, 304, 44); clock['flags'] = CENTER
            place(listing, 0, 88, 312, 2 * row)
            place(scroll, 312, 88, 8, 2 * row)
            place(source, 8, 184, 304, 2 * fh); source['flags'] = WRAP
        elif sid == 'clock-edit' and listing:
            place(listing, 0, 24, 312, 6 * row)
            place(scroll, 312, 24, 8, 6 * row)
            for n in texts:
                place(n, 8, 200, 304, 2 * fh); n['flags'] = WRAP
        elif sid in ('display', 'battery', 'shutdown') and listing:
            list_rows = 3 if sid == 'shutdown' else 2 if sid == 'display' else 1
            place(listing, 0, 24, 320, list_rows * row)
            for i, n in enumerate(texts):
                place(n, 8, 24 + list_rows * row + 18 + i * 28, 304, fh)
        elif sid == 'sleep-timer' and listing:
            place(listing, 0, 24, 312, 4 * row)
            place(scroll, 312, 24, 8, 4 * row)
            for n in texts:
                place(n, 8, 204, 304, 2 * fh); n['flags'] = WRAP
        elif sid == 'about':
            art = next(n for n in nodes if n['kind'] == 17)
            place(art, 12, 30, 46, 70)
            place(texts[0], 70, 48, 238, fh + 2)
            place(texts[1], 70, 76, 238, fh)
            place(next(n for n in nodes if n['kind'] == 10), 12, 106, 296, 8)
            place(listing, 0, 121, 312, 94)
            place(scroll, 312, 121, 8, 94)
            place(texts[2], 8, 218, 304, fh)
        elif sid == 'update' and listing:
            for i, n in enumerate(texts):
                place(n, 8, 30 + i * 26, 304, fh)
            place(listing, 0, 204, 320, 27)
        elif sid == 'photos-grid' and listing:
            for n in texts:
                place(n, 16, 104, 288, 2 * fh); n['flags'] = CENTER | WRAP
        elif sid == 'video-output' and listing:
            place(listing, 0, 48, 320, 2 * row)
            for n in texts:
                place(n, 8, 190, 304, fh); n['flags'] = CENTER
        for n in nodes:
            n['w'] = max(1, min(n['w'], 320 - n['x']))
            n['h'] = max(1, min(n['h'], 240 - n['y']))
        mark_control_footer(scene, (8, 216, 304, 22))
        scene['count'] = len(nodes)
    return result
