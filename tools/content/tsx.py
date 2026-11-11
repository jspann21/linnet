"""A complete, deliberately small TSX grammar; never evaluates JavaScript.

Accepted module: named imports from @linnet/ui followed by one exported default
function, with one return containing one JSX Screen. Attributes are literals;
literal arrays supply menu labels and destination scene IDs. Nothing following
the closing module is ignored. Coordinates are resolved relative to the parent.
"""
import html
import re

KINDS = ('Screen','View','Text','Icon','Image','VirtualList','Menu','Grid',
         'Scrollbar','ProgressBar','StatusBar','Dialog','Toast','AlbumArt',
         'PhotoSurface','VideoSurface','CanvasSurface')
COLOR_ROLES = ('foreground','background','accent','muted','panel',
               'selection_fg','selection_bg','status_fg','status_bg','border',
               'artwork_bg','artwork_fg','battery')
COLOR_TOKEN = 0x10000
PROPS = {'x','y','w','h','fg','bg','flags','count','resource','index','text',
         'items','targets','deps','focus','canvas'}
ID = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')

class ContentError(ValueError):
    pass

class Parser:
    def __init__(self, source, colors, width=220, height=176):
        if type(width) is not int or type(height) is not int or not 1 <= width <= 1024 or not 1 <= height <= 1024 or width*height > 131072:
            raise ContentError("invalid authoring canvas")
        self.width, self.height = width, height
        if len(source.encode('utf-8')) > 262144:
            raise ContentError('TSX source exceeds 256 KiB')
        self.s, self.p, self.colors, self.nodes = source, 0, colors, []
        self.imports = set()

    def fail(self, message):
        raise ContentError(f'TSX line {self.s.count(chr(10),0,self.p)+1}: {message}')

    def ws(self):
        while self.p<len(self.s) and self.s[self.p].isspace(): self.p+=1

    def take(self, token):
        self.ws()
        if not self.s.startswith(token,self.p): self.fail(f'expected {token!r}')
        self.p+=len(token)
        if token[-1].isalpha() and self.p<len(self.s) and (self.s[self.p].isalnum() or self.s[self.p]=='_'):
            self.fail('keyword requires a token boundary')

    def ident(self):
        self.ws(); m=ID.match(self.s,self.p)
        if not m: self.fail('expected identifier')
        self.p=m.end(); return m.group()

    def string(self):
        self.ws()
        if self.p>=len(self.s) or self.s[self.p] not in "\"'": self.fail('expected string literal')
        quote=self.s[self.p]; self.p+=1; out=''
        while self.p<len(self.s):
            char=self.s[self.p]; self.p+=1
            if char==quote: return out
            if char=='\\':
                if self.p>=len(self.s): self.fail('unfinished string escape')
                char=self.s[self.p]; self.p+=1
                if char not in ('\\',quote): self.fail('only quote/backslash escapes are supported')
            if ord(char)<32 or ord(char)>126: self.fail('strings must be printable ASCII')
            out+=char
            if len(out)>160: self.fail('string exceeds 160 characters')
        self.fail('unterminated string')

    def literal(self):
        self.ws()
        if self.p<len(self.s) and self.s[self.p] in "\"'": return self.string()
        if self.s.startswith('[',self.p):
            self.take('['); values=[]; self.ws()
            if not self.s.startswith(']',self.p):
                while True:
                    values.append(self.string())
                    if len(values)>64: self.fail('at most 64 static items')
                    self.ws()
                    if not self.s.startswith(',',self.p): break
                    self.take(',')
            self.take(']'); return values
        m=re.match(r'(?:-?[0-9]+|true|false)(?![A-Za-z_0-9.])',self.s[self.p:])
        if not m: self.fail('only string/integer/boolean/string-array literals are supported')
        self.p+=len(m.group()); value=m.group()
        return value=='true' if value in ('true','false') else int(value)

    def integer(self, props, key, default, low, high):
        value=props.get(key,default)
        if type(value) is not int or not low<=value<=high:
            self.fail(f'{key} must be an integer in {low}..{high}')
        return value

    def color(self, value):
        if not isinstance(value,str): self.fail('colors must be token or hex strings')
        if value.startswith('theme.'):
            token=value[6:]
            if token in COLOR_ROLES and token in self.colors: return COLOR_TOKEN+COLOR_ROLES.index(token)
            value=self.colors.get(token,'')
        if not re.fullmatch(r'#[0-9a-fA-F]{6}',value): self.fail('unknown color token or invalid #RRGGBB color')
        rgb=int(value[1:],16)
        return ((rgb>>19)<<11)|(((rgb>>10)&63)<<5)|((rgb>>3)&31)

    def element(self, parent=None, depth=0):
        if depth>16: self.fail('nesting exceeds 16 levels')
        self.take('<'); name=self.ident()
        if name not in self.imports or name not in KINDS: self.fail(f'unknown/unimported component {name}')
        if parent is None and name!='Screen': self.fail('root must be Screen')
        if parent is not None and name=='Screen': self.fail('Screen must be the root')
        props={}
        while True:
            self.ws()
            if self.s.startswith(('>','/>'),self.p): break
            key=self.ident()
            if key not in PROPS or key in props: self.fail(f'unknown/duplicate property {key}')
            self.take('='); self.ws()
            if self.s.startswith('{',self.p):
                self.take('{'); value=self.literal(); self.take('}')
            else: value=self.string()
            props[key]=value
        for key in ('items','targets'):
            if key in props and not isinstance(props[key],list): self.fail(f'{key} must be a literal string array')
        parent_x,parent_y,parent_w,parent_h=(0,0,self.width,self.height) if parent is None else (parent[k] for k in ('x','y','w','h'))
        x=self.integer(props,'x',0,0,self.width-1); y=self.integer(props,'y',0,0,self.height-1)
        w=self.integer(props,'w',parent_w-x,1,self.width); h=self.integer(props,'h',parent_h-y,1,self.height)
        if x+w>parent_w or y+h>parent_h: self.fail('rectangle escapes parent bounds')
        if parent is None and (x,y,w,h)!=(0,0,self.width,self.height):
            self.fail('Screen must cover the authoring canvas')
        node=dict(kind=KINDS.index(name)+1,component=name,x=x+parent_x,y=y+parent_y,w=w,h=h,
                  fg=self.color(props.get('fg','theme.foreground')),bg=self.color(props.get('bg','theme.background')),
                  flags=self.integer(props,'flags',1 if name in ('Menu','VirtualList','Grid') else 0,0,7),
                  count=self.integer(props,'count',len(props.get('items',[])),0,1000000),
                  resource=props.get('resource',''),index=self.integer(props,'index',0,0,999999),
                  text=props.get('text',''),items=props.get('items',[]),targets=props.get('targets',[]),
                  deps=self.integer(props,'deps',3 if name in ('Menu','VirtualList','Grid','Scrollbar') else 0,0,31),
                  focus=props.get('focus',False),canvas=props.get('canvas',''))
        if parent is not None: node['flags']|=parent['flags']&2
        if type(node['focus']) is not bool: self.fail('focus must be boolean')
        for key in ('text','resource','canvas'):
            if not isinstance(node[key],str): self.fail(f'{key} must be a string')
        if len(node['targets']) not in (0,len(node['items'])): self.fail('targets must match static item count')
        if node['items'] and node['count']!=len(node['items']): self.fail('static items must exactly match count')
        if node['index']>=max(node['count'],1): self.fail('index outside item count')
        if node['focus'] and not node['flags']&1: self.fail('focused component must be focusable')
        if node['focus'] and node['flags']&2: self.fail('focused component must be visible')
        if node['canvas'] and name!='CanvasSurface': self.fail('canvas callback requires CanvasSurface')
        if name=='CanvasSurface' and not node['canvas']: self.fail('CanvasSurface requires a declared native callback')
        if len(self.nodes)>=256: self.fail('scene exceeds 256 nodes')
        self.nodes.append(node)
        self.ws()
        if self.s.startswith('/>',self.p): self.take('/>'); return node
        self.take('>'); child_text=''
        while True:
            self.ws()
            if self.s.startswith('</',self.p): break
            if self.p>=len(self.s): self.fail(f'unclosed {name}')
            if self.s[self.p]=='<': self.element(node,depth+1)
            else:
                end=self.s.find('<',self.p)
                if end<0: self.fail('unclosed text')
                text=html.unescape(self.s[self.p:end])
                if '{' in text or '}' in text or any(ord(c)>126 or (ord(c)<32 and not c.isspace()) for c in text):
                    self.fail('child expressions/non-ASCII text are not supported')
                child_text+=' '.join(text.split()); self.p=end
        self.take('</'); close=self.ident(); self.take('>')
        if close!=name: self.fail(f'closing {close} does not match {name}')
        if child_text:
            if name!='Text' or node['text'] or len(child_text)>160: self.fail('text children require Text without text property (max 160)')
            node['text']=child_text
        return node

    def parse(self):
        self.ws()
        while self.s.startswith('import',self.p):
            self.take('import'); self.take('{')
            while True:
                name=self.ident()
                if name not in KINDS or name in self.imports: self.fail('unknown/duplicate Linnet UI import')
                self.imports.add(name); self.ws()
                if not self.s.startswith(',',self.p): break
                self.take(',')
            self.take('}'); self.take('from')
            if self.string()!='@linnet/ui': self.fail('only @linnet/ui imports are supported')
            self.take(';'); self.ws()
        self.take('export'); self.take('default'); self.take('function'); self.ident()
        self.take('('); self.take(')'); self.take('{'); self.take('return'); self.take('(')
        self.element(); self.take(')'); self.take(';'); self.take('}'); self.ws()
        if self.p!=len(self.s): self.fail('unexpected trailing source')
        focus=[i for i,node in enumerate(self.nodes) if node.pop('focus')]
        if len(focus)>1: self.fail('at most one initial focus node')
        return self.nodes,focus[0] if focus else next((i for i,n in enumerate(self.nodes) if n['flags']&1 and not n['flags']&2),0xffffffff)

def compile_tsx(source, colors, width=220, height=176):
    return Parser(source,colors,width,height).parse()
