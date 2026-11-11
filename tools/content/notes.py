"""Prepare read-only Notes for the device's bounded ASCII display."""
from pathlib import Path
from itertools import islice
from functools import lru_cache
import hashlib
import importlib.util
import struct
import json
import tomllib
import unicodedata
import os
import tempfile
from . import SourceError
from .host_state import file_stamp

MAX_NOTES = 256
TITLE = 64
LINES = 256
LINE = 32
RECORD = 4 + LINES * LINE
MAX_FILE = 16 + MAX_NOTES * (TITLE + RECORD)
EXTENSIONS = {'.txt', '.md', '.text'}


@lru_cache(maxsize=8)
def _display_recipe(target, scene_id='linnet.system.notes'):
    """Use the narrowest reading list and widest glyphs allowed by this target."""
    from .compiler import load_font, scale_layout
    from .layouts import apply_layout
    from .tsx import compile_tsx
    root = Path(__file__).resolve().parents[2]
    manifest = tomllib.loads((root / 'targets' / target / 'target.toml').read_text(encoding='utf-8'))
    width, height = manifest['lcd_width'], manifest['lcd_height']
    profile = manifest.get('presentation_profile')
    profile_root = root / 'ui/profiles' / profile if profile else None
    screen_root = root / 'targets' / target / 'screens'
    if not screen_root.is_dir() and profile_root is not None:
        screen_root = profile_root / 'screens'
    native = screen_root.is_dir()
    screen = (screen_root / scene_id / 'screen.tsx' if native else
              root / 'ui/system' / scene_id.rsplit('.', 1)[-1] / 'screen.tsx')
    colors = {name: '#000000' for name in ('foreground', 'background', 'accent',
                                           'muted', 'panel', 'selection_fg', 'selection_bg')}
    nodes, focus = compile_tsx(screen.read_text(encoding='utf-8'), colors,
                               width if native else 220, height if native else 176)
    scene = dict(id=scene_id, title='Reading', nodes=nodes,
                 count=len(nodes), focus_node=focus)
    layouts = None
    if profile_root is not None and native:
        layout_path = profile_root / 'layouts.py'
        module_spec = importlib.util.spec_from_file_location('linnet_notes_' + profile, layout_path)
        layouts = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(layouts)
    paths = {}
    scopes = [root / 'themes']
    if profile_root is not None:
        scopes.append(profile_root / 'themes')
    scopes.append(root / 'targets' / target / 'themes')
    for scope in scopes:
        for path in sorted(scope.glob('*/theme.toml')):
            spec = tomllib.loads(path.read_text(encoding='utf-8'))
            metadata = spec['theme']
            if target in metadata.get('targets', [target]):
                paths[metadata['id']] = (path, spec)
    allowed = manifest.get('ui', {}).get('allowed_themes', list(paths))
    if not allowed or any(name not in paths for name in allowed):
        raise ValueError(f'{target}: Notes themes are unavailable')
    presentation_path = root / 'targets' / target / 'presentation.toml'
    presentation = (tomllib.loads(presentation_path.read_text(encoding='utf-8'))
                    if presentation_path.is_file() else {})
    widths = [0] * 95
    pixel_limit = width
    for name in allowed:
        path, spec = paths[name]
        metadata = spec['theme']
        font_path = path.parent / metadata.get('font', '../../sdk/assets/font.json')
        font = load_font(font_path.read_bytes())
        advances = font.get('advances', [font['advance']] * 95)
        # Optional Podium Sans uses advances up to 14 pixels. Prepare Notes
        # conservatively so changing fonts never requires rewriting the library.
        if metadata.get('font_source') == 'apple-podium-sans16':
            advances = [max(14, value) for value in advances]
        widths = [max(a, b) for a, b in zip(widths, advances)]
        layout = ('default' if manifest['lcd_format'].startswith('gray') and metadata.get('layout') != 'classic-split' else
                  metadata.get('layout', 'default'))
        theme = dict(layout=layout, font=font,
                     metrics=spec['metrics'])
        if native:
            if layouts is not None:
                variant = layouts.apply_layout([scene], theme)
            else:
                from copy import deepcopy
                variant = deepcopy([scene])
        else:
            variant = scale_layout(apply_layout([scene], theme,
                                                compact=manifest['lcd_format'].startswith('gray')),
                                   width, height)
        for rule in presentation.get('adjustments', []):
            if rule.get('scene', scene['id']) != scene['id']:
                continue
            for node in variant[0]['nodes']:
                if all(node.get(key) == rule[key] for key in ('kind', 'text') if key in rule):
                    for key in ('x', 'y', 'w', 'h'):
                        if key in rule:
                            node[key] = rule[key]
        listing = next(node for node in variant[0]['nodes'] if node['kind'] == 6)
        padding = spec['metrics']['padding']
        if not native and width != 220:
            padding = (padding * width + 110) // 220
        pixel_limit = min(pixel_limit, listing['w'] - padding - 18)
    if pixel_limit < max(widths):
        raise ValueError(f'{target}: Notes list is too narrow for its selected fonts')
    recipe = hashlib.sha256(json.dumps([target, pixel_limit, widths], separators=(',', ':')).encode()).hexdigest()[:16]
    return tuple(widths), pixel_limit, recipe


def _display_lines(text, widths, pixel_limit, columns=31):
    for paragraph in text.split('\n'):
        if not paragraph:
            yield ''
            continue
        start, stop = 0, len(paragraph)
        while start < stop:
            length, pixels = 0, 0
            for char in paragraph[start:start+columns]:
                advance = widths[ord(char)-32]
                if pixels + advance > pixel_limit:
                    break
                pixels += advance
                length += 1
            end = start + length
            if end < stop:
                space = paragraph.rfind(' ', start, end+1)
                if space > start:
                    end = space
            yield paragraph[start:end]
            start = end
            while start < stop and paragraph[start] == ' ':
                start += 1


def _ascii(text):
    if text.isascii():
        return text
    text = text.translate(str.maketrans({'“': '"', '”': '"', '’': "'", '‘': "'", '—': '-', '–': '-', '…': '...'}))
    normalized = ''.join(c for c in unicodedata.normalize('NFKD', text) if not unicodedata.combining(c))
    return normalized.encode('ascii', 'replace').decode('ascii')


def compile_notes(source, output, *, target, excluded=None, progress=None, checkpoint=lambda: None):
    from .publish import _plain
    checkpoint()
    original_source = Path(os.path.abspath(source))
    _plain(original_source, Path(original_source.anchor))
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_dir():
        raise ValueError('Notes source must be a folder')
    if output.is_relative_to(source):
        raise ValueError('Stage the Notes package outside the source folder')
    excluded = {Path(value).as_posix().casefold() for value in (excluded or ())}
    files = []
    for path in source.rglob('*'):
        checkpoint()
        if (path.suffix.lower() in EXTENSIONS and
                path.relative_to(source).as_posix().casefold() not in excluded and path.is_file()):
            files.append(path)
            if len(files) > MAX_NOTES:
                raise ValueError(f'Notes supports at most {MAX_NOTES} documents; found more than {MAX_NOTES}')
    checkpoint()
    files.sort(key=lambda p: p.as_posix().casefold())
    titles, records, changed = bytearray(), bytearray(), []
    widths, pixel_limit, recipe = _display_recipe(target)
    for index, path in enumerate(files):
        checkpoint()
        if progress:
            progress(dict(phase='notes', current=index, total=len(files), file=str(path)))
        _plain(path, source)
        if path.is_symlink() or not path.resolve().is_relative_to(source):
            raise ValueError(f'Note links cannot escape or alias the source: {path}')
        before = path.stat()
        if before.st_size > 1024 * 1024:
            raise SourceError(f'Note is too large: {path}', path)
        with path.open('rb') as reader:
            raw = reader.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise SourceError(f'Note is too large: {path}', path)
        if len(raw) != before.st_size or file_stamp(before) != file_stamp(path.stat()):
            raise SourceError(f'Note changed while preparing: {path}', path)
        try:
            original = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')
        except UnicodeError as exc:
            raise SourceError(f'Note must be UTF-8 or BOM-marked UTF-16: {path}', path) from exc
        text = _ascii(original).replace('\r\n', '\n').replace('\r', '\n').expandtabs(4)
        text = ''.join(c if c == '\n' or 32 <= ord(c) <= 126 else '?' for c in text)
        title = _ascii(path.relative_to(source).with_suffix('').as_posix())
        title = ''.join(c if 32 <= ord(c) <= 126 else '?' for c in title)
        if len(title) > 63:
            raise SourceError(f'Note title exceeds 63 display characters: {path}', path)
        lines = list(islice(_display_lines(text, widths, pixel_limit), LINES + 1))
        if len(lines) > LINES:
            raise SourceError(f'Note exceeds {LINES} display lines; split it into smaller documents: {path}', path)
        if not original.isascii() or not path.relative_to(source).as_posix().isascii():
            changed.append(path.relative_to(source).as_posix())
        titles += title.encode('ascii').ljust(TITLE, b'\0')
        record = struct.pack('<I', len(lines)) + b''.join(line.encode('ascii').ljust(LINE, b'\0') for line in lines)
        records += record.ljust(RECORD, b'\0')
    data = struct.pack('<4sIII', b'NOTE', 0, len(files), RECORD) + titles + records
    checkpoint()
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=output.name+'.', suffix='.tmp', dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, 'wb') as writer:
            writer.write(data)
            writer.flush()
            os.fsync(writer.fileno())
        report = inspect(temporary)
        checkpoint()
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    report['package'] = str(output)
    report.update(source=str(source), ascii_converted=changed, target=target,
                  wrapping_pixels=pixel_limit, wrapping_recipe=recipe,
                  note='Prepared ASCII display copies; original documents are unchanged.')
    return report


def inspect(path):
    path = Path(path)
    with path.open('rb') as reader:
        size = os.fstat(reader.fileno()).st_size
        if size > MAX_FILE:
            raise ValueError('Notes package is too large')
        data = reader.read(size + 1)
        if len(data) != size:
            raise ValueError('Notes package changed while reading')
    if len(data) < 16:
        raise ValueError('Notes header is missing')
    magic, reserved, count, record = struct.unpack_from('<4sIII', data)
    if magic != b'NOTE' or reserved or count > MAX_NOTES or record != RECORD or len(data) != 16 + count * (TITLE + RECORD):
        raise ValueError('Invalid Notes package')
    def string(raw):
        if 0 not in raw or any(c and not 32 <= c <= 126 for c in raw):
            raise ValueError('Invalid Notes display text')
    for index in range(count):
        string(data[16+index*TITLE:16+(index+1)*TITLE])
        offset = 16 + count*TITLE + index*RECORD
        lines = struct.unpack_from('<I', data, offset)[0]
        if not 1 <= lines <= LINES:
            raise ValueError('Invalid Notes line count')
        for line in range(LINES):
            string(data[offset+4+line*LINE:offset+4+(line+1)*LINE])
    return dict(package=str(path), notes=count, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
