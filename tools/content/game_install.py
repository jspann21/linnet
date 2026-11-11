"""Guarded publication of immutable games, retained save slots, and A/B catalogues."""
from __future__ import annotations
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import uuid
import zlib
from tools.content import game_package as fmt
from tools.content.guard import guarded_path, guard_volume
from tools.content.publish import _exclusive
from tools.content import media_manager as media
from tools.studio.storage import record
from tools.studio.update_history import _decode_record

SAVE_STRIDE, SAVE_FILE_BYTES = 4096, 128*4096


class GameCatalogueRecovery(ValueError):
    """An explicitly recoverable catalogue conflict or damaged pair."""


def _read(path, root, maximum, exact=None):
    info=guarded_path(path,root)
    if info is None:return None
    if not path.is_file() or info.st_size>maximum or exact is not None and info.st_size!=exact:
        raise ValueError('Unsafe or incorrectly sized game file: '+path.name)
    with path.open('rb') as reader:data=reader.read(maximum+1)
    if len(data)!=info.st_size:raise ValueError('Game file changed while reading')
    return data


def inventory(root, target):
    root=Path(root);directory=root/'.LINNET/GAMES';slots=[];valid=[]
    for bank in range(2):
        path=directory/f'INDEX{bank}.BIN';info=guarded_path(path,root)
        if info is not None and not path.is_file():raise ValueError('Unsafe game catalogue path')
        if info is not None and info.st_size>36+128*256:
            slots.append(dict(bytes=info.st_size,modified=info.st_mtime_ns));continue
        data=_read(path,root,36+128*256)
        slots.append(data)
        if data is not None:
            try:value=fmt.read_catalogue(data,target)
            except ValueError:continue
            valid.append(dict(bank=bank,data=data,**value))
    if len(valid)==2 and valid[0]['generation']==valid[1]['generation'] and valid[0]['data']!=valid[1]['data']:
        raise GameCatalogueRecovery('Conflicting game catalogues; preserve both copies for recovery')
    if not valid and any(x is not None for x in slots):
        raise GameCatalogueRecovery('Both game catalogues are damaged; recover them before updating games')
    chosen=max(valid,key=lambda x:x['generation']) if valid else None
    return dict(valid=valid,tokens=slots,active=chosen['bank'] if chosen else None,
                generation=chosen['generation'] if chosen else 0,rows=chosen['rows'] if chosen else [])


def save_payload(game_id, schema=1, best=0, progress=b''):
    fmt.identity(game_id)
    if type(schema) is not int or not 0<schema<=0xffffffff or not 0<=best<=0xffffffff or len(progress)>256:
        raise ValueError('Invalid game save payload')
    return fmt._text(game_id,64)+struct.pack('<III',schema,best,len(progress))+progress


def decode_save(raw):
    generation,payload=_decode_record(raw)
    if not 76<=len(payload)<=332:raise ValueError('Invalid game save length')
    game_id=fmt.identity(fmt._read_text(payload[:64]))
    schema,best,size=struct.unpack_from('<III',payload,64)
    if not schema or size>256 or len(payload)!=76+size:raise ValueError('Invalid game save schema/size')
    return dict(id=game_id,schema=schema,best=best,progress=payload[76:],generation=generation)


def saves(root):
    root=Path(root);banks=[_read(root/'.LINNET'/f'GMSAVE{i}.BIN',root,SAVE_FILE_BYTES,SAVE_FILE_BYTES)
                          for i in range(2)]
    result={};used=set()
    for slot in range(fmt.CATALOGUE_MAX):
        records=[b[slot*4096:slot*4096+512] if b is not None else bytes(512) for b in banks]
        valid=[]
        for bank,raw in enumerate(records):
            if not any(raw):continue
            used.add(slot)
            try:value=decode_save(raw)
            except ValueError:continue
            valid.append(dict(bank=bank,raw=raw,slot=slot,**value))
        if len(valid)==2 and (valid[0]['id']!=valid[1]['id'] or
                valid[0]['generation']==valid[1]['generation'] and valid[0]['raw']!=valid[1]['raw']):
            raise ValueError('Conflicting game saves in slot '+str(slot))
        if valid:
            chosen=max(valid,key=lambda x:x['generation'])
            if chosen['id'] in result:raise ValueError('Duplicate retained game save identity')
            result[chosen['id']]=chosen
    return dict(banks=banks,used=used,games=result)


def _stage(data, identity):
    root=Path(identity['root']);directory=root/'.LINNET/GAMES'
    for _ in range(8):
        temporary=directory/('GT'+uuid.uuid4().hex[:6].upper()+'.TMP');owned=False
        try:
            guard_volume(identity);guarded_path(temporary,root)
            try:stream=temporary.open('xb')
            except FileExistsError:continue
            owned=True
            with stream:
                stream.write(data);stream.flush();os.fsync(stream.fileno())
            guard_volume(identity);guarded_path(temporary,root)
            if temporary.read_bytes()!=data:raise IOError('Game staging readback mismatch')
            return temporary
        except BaseException:
            if owned and temporary.exists():
                guard_volume(identity);guarded_path(temporary,root);temporary.unlink()
            raise
    raise FileExistsError('Cannot allocate an owned game staging filename')


def _barrier(path, data, identity):
    root=Path(identity['root']);guard_volume(identity);guarded_path(path,root)
    with path.open('r+b') as stream:
        stream.flush();os.fsync(stream.fileno())
        if stream.read(len(data)+1)!=data:raise IOError('Game publication readback mismatch')
    guard_volume(identity);guarded_path(path,root)


def _create_file(path, data, identity):
    root=Path(identity['root']);temporary=_stage(data,identity)
    try:
        guard_volume(identity);guarded_path(path,root)
        # Windows rename refuses an existing destination. All publication is
        # on the identified FAT volume, including the two root save-bank files.
        temporary.rename(path)
        _barrier(path,data,identity)
    finally:
        if temporary.exists():guard_volume(identity);guarded_path(temporary,root);temporary.unlink()


def _replace_file(path, data, identity):
    root=Path(identity['root']);temporary=_stage(data,identity)
    try:
        guard_volume(identity);guarded_path(path,root);os.replace(temporary,path)
        _barrier(path,data,identity)
    finally:
        if temporary.exists():guard_volume(identity);guarded_path(temporary,root);temporary.unlink()


def _recover(directory, identity, snapshot):
    root=Path(identity['root']);path=directory/'INSTALL.JSON'
    # GTxxxxxx.TMP is reserved exclusively for this publisher in GAMES.
    # A killed writer may leave a partial temporary, never a partial final file.
    for temporary in directory.iterdir():
        if re.fullmatch(r'GT[0-9A-F]{6}\.TMP',temporary.name):
            guarded_path(temporary,root);guard_volume(identity);temporary.unlink()
    raw=_read(path,root,65536)
    if raw is None:return
    value=json.loads(raw)
    if not isinstance(value,dict) or set(value)!={'format','files'} or value['format']!=1 or not isinstance(value['files'],dict) or len(value['files'])>3*128:
        raise ValueError('Invalid game publication receipt')
    keep={row['filename'] for v in snapshot['valid'] for row in v['rows']}
    for filename,digest in value['files'].items():
        if not fmt.FILE_PATTERN.fullmatch(filename) or not isinstance(digest,str) or not re.fullmatch('[0-9a-f]{64}',digest):
            raise ValueError('Invalid game receipt path/digest')
        candidate=directory/filename
        data=_read(candidate,root,256+fmt.SLOT_BYTES)
        if data is not None and filename not in keep:
            if hashlib.sha256(data).hexdigest()!=digest:continue  # Preserve changed files; never reclaim by filename alone.
            guard_volume(identity);guarded_path(candidate,root);candidate.unlink()
    guard_volume(identity);guarded_path(path,root);path.unlink()


def install(identity, packages, expected=None):
    """Replace the installed selection. Callers include custom games to retain."""
    # Validate prepared artifacts completely before touching the device.
    desired=[];seen=set()
    for path in packages:
        path=Path(path)
        if not path.is_file() or not 288<=path.stat().st_size<=256+fmt.SLOT_BYTES:
            raise ValueError('Invalid prepared game package size')
        data=path.read_bytes();value=fmt.inspect(data,identity['target_id'])
        if value['id'] in seen:raise ValueError('Duplicate selected game identity')
        seen.add(value['id']);digest=hashlib.sha256(data).hexdigest()
        desired.append(dict(value,data=data,filename=digest[:8].upper()+'.LGM',file_crc=zlib.crc32(data),digest=digest))
    if len(desired)>fmt.CATALOGUE_MAX:raise ValueError('Too many installed games')
    identity=media.refresh(identity)
    with _exclusive(identity):
        root=Path(identity['root']);directory=root/'.LINNET/GAMES'
        guarded_path(directory,root);guard_volume(identity);directory.mkdir(exist_ok=True)
        snapshot=inventory(root,identity['target_id'])
        if expected is not None and snapshot['tokens']!=expected:
            raise ValueError('Game selection changed; reload before saving')
        _recover(directory,identity,snapshot)
        owned={}
        for value in desired:
            existing=_read(directory/value['filename'],root,256+fmt.SLOT_BYTES)
            if existing is None:owned[value['filename']]=value['digest']
            elif existing!=value['data']:raise ValueError('Short game filename collision; preserving existing package')
        # Journal verified prior references too: replacing a catalogue can
        # otherwise erase the only list of packages pending garbage collection.
        for old in (r for v in snapshot['valid'] for r in v['rows']):
            content=_read(directory/old['filename'],root,256+fmt.SLOT_BYTES)
            if content is None or len(content)!=old['bytes'] or zlib.crc32(content)!=old['file_crc']:continue
            try:metadata=fmt.inspect(content,identity['target_id'])
            except ValueError:continue
            if metadata['id']==old['id']:owned[old['filename']]=hashlib.sha256(content).hexdigest()
        retained=saves(root)
        # Blank preallocated files are optional on firmware; never replace existing banks.
        for bank,data in enumerate(retained['banks']):
            if data is None:_create_file(root/'.LINNET'/f'GMSAVE{bank}.BIN',bytes(SAVE_FILE_BYTES),identity)
        slots=dict(retained['games']);used=set(retained['used'])
        # A retained catalogue can reserve a damaged save slot even if its identity is unreadable.
        references={};identities={}
        for value in snapshot['valid']:
            for row in value['rows']:
                if row['save_slot'] in references and references[row['save_slot']]!=row['id']:
                    raise ValueError('Retained catalogues disagree about save ownership')
                if row['id'] in identities and identities[row['id']]!=row['save_slot']:
                    raise ValueError('Retained game identity changed save slots')
                identities[row['id']]=row['save_slot']
                references[row['save_slot']]=row['id'];used.add(row['save_slot'])
        for game_id,saved in slots.items():
            if saved['slot'] in references and references[saved['slot']]!=game_id:
                raise ValueError('Save identity disagrees with retained catalogue slot')
            if game_id in identities and identities[game_id]!=saved['slot']:
                raise ValueError('Retained game save moved to a different slot')
        assigned=[]
        for value in desired:
            saved=slots.get(value['id'])
            if saved is not None:
                slot=saved['slot']
                if saved['schema']>value['schema']:raise ValueError('Game save schema is newer than this package')
                if slot in references and references[slot]!=value['id']:raise ValueError('Game save/catalogue identity mismatch')
            else:
                reserved=[slot for slot,game_id in references.items() if game_id==value['id']]
                if reserved:
                    slot=reserved[0]
                    # Do not reset damaged retained saves during a package update.
                    saved=dict(slot=slot)
                else:
                    free=next((i for i in range(fmt.CATALOGUE_MAX) if i not in used),None)
                    if free is None:raise ValueError('Retained game save store is full; no saves were removed')
                    slot=free;used.add(slot)
                    assigned.append((slot,record(save_payload(value['id'],value['schema']),1)))
            value['save_slot']=slot
        generation=snapshot['generation']+1
        data=fmt.catalogue(desired,generation=generation,target=identity['target_id'])
        # Record immutable ownership before creating package files; recovery retains all valid catalogue references.
        receipt=json.dumps(dict(format=1,files=owned),sort_keys=True).encode()
        _replace_file(directory/'INSTALL.JSON',receipt,identity)
        for slot,initial in assigned:
            for bank in range(2):
                path=root/'.LINNET'/f'GMSAVE{bank}.BIN';guard_volume(identity);guarded_path(path,root)
                with path.open('r+b') as stream:
                    stream.seek(slot*4096);old=stream.read(512)
                    if any(old):raise ValueError('New game save slot changed during installation')
                    stream.seek(slot*4096);stream.write(initial);stream.flush();os.fsync(stream.fileno())
                    stream.seek(slot*4096)
                    if stream.read(512)!=initial:raise IOError('Game save allocation readback failed')
        for value in desired:
            path=directory/value['filename'];existing=_read(path,root,256+fmt.SLOT_BYTES)
            if existing is None:_create_file(path,value['data'],identity)
            elif existing!=value['data']:raise ValueError('Short game filename collision; preserving existing package')
        # Confirm catalogue tokens were not replaced by another writer while preparing.
        current=inventory(root,identity['target_id'])
        if current['tokens']!=snapshot['tokens']:raise ValueError('Game catalogue changed during installation')
        inactive=0 if snapshot['active'] is None else snapshot['active']^1
        _replace_file(directory/f'INDEX{inactive}.BIN',data,identity)
        published=inventory(root,identity['target_id'])
        _recover(directory,identity,published)
        return published['rows']


def recover_catalogues(identity):
    """Explicit repair keeps damaged metadata, packages, and all save banks."""
    identity=media.refresh(identity)
    with _exclusive(identity):
        root=Path(identity['root']);directory=root/'.LINNET/GAMES'
        try:
            current=inventory(root,identity['target_id'])
        except GameCatalogueRecovery:
            current=None
        if current and current['valid']:
            raise ValueError('Game catalogues are readable now; reload before choosing recovery')
        archive=directory/'RECOVER';guarded_path(archive,root);guard_volume(identity)
        archive.mkdir(parents=True,exist_ok=True)
        for name in ('INDEX0.BIN','INDEX1.BIN','INSTALL.JSON'):
            path=directory/name;info=guarded_path(path,root)
            if info is None:continue
            if not path.is_file():raise ValueError('Unsafe game metadata recovery path')
            backup=archive/('R'+uuid.uuid4().hex[:7].upper()+'.BIN')
            guard_volume(identity);guarded_path(backup,root)
            if backup.exists():raise FileExistsError('Game recovery archive name collision')
            # A same-volume rename preserves even oversized corrupt metadata
            # without reading it into memory or publishing a partial backup.
            path.rename(backup)
            guard_volume(identity);saved=guarded_path(backup,root)
            if saved.st_size!=info.st_size or saved.st_mtime_ns!=info.st_mtime_ns:
                raise IOError('Game metadata changed during recovery')
