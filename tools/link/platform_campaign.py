"""Platform campaign: synthetic automation plus separately pending physical checks."""
from datetime import datetime, timezone
import hashlib
import json
import time
from tools.inspect.image import inspect, verify_device_report
from tools.studio.targets import TARGETS
from .campaign import connect, _run_directory, _save
from .session import DeviceError
from .platform import state as _state, ui_state as _ui_state, png as _png


def cycle(root, image_path, scenario_path, port, progress=None):
    image = image_path.read_bytes()
    image_id = inspect(image)
    raw_scenario = scenario_path.read_bytes()
    suite = json.loads(raw_scenario)
    required = {'target_id', 'navigation_events', 'animation_frames',
                'idle_observe_ms', 'sleep_ms'}
    if (not isinstance(suite, dict) or set(suite) != required or
            image_id['profile'] == 'release' or
            suite.get('target_id') != image_id['target_id']):
        raise ValueError('platform campaign requires a matching development image and scenario')
    limits = {'navigation_events':(32,4096),'animation_frames':(17,120),
              'idle_observe_ms':(100,5000),'sleep_ms':(50,1000)}
    for key,(low,high) in limits.items():
        if type(suite.get(key)) is not int or not low <= suite[key] <= high:
            raise ValueError(f'invalid platform scenario {key}')
    if suite['navigation_events'] % 2:
        raise ValueError('navigation storm requires an even event count for balanced wheel pairs')
    run_id,directory = _run_directory(root,image_id)
    receipt = dict(run_id=run_id,image=image_id,scenario=suite,
                   scenario_sha256=hashlib.sha256(raw_scenario).hexdigest(),
                   started_at=datetime.now(timezone.utc).isoformat(),steps=[],automated_passed=False,
                   physical_passed=False,device_write_attempted=False,
                   physical_checks_pending=['buttons','wheel','hold-switch','headphone-detection',
                                            'backlight-visibility','sleep-wake','screen-artifacts'])
    receipt_path = directory/'receipt.json'
    _save(receipt_path,receipt)
    started = time.monotonic()
    session = None
    def record(name, **details):
        event=dict(name=name,elapsed_seconds=round(time.monotonic()-started,6),**details)
        receipt['steps'].append(event); _save(receipt_path,receipt)
        if progress: progress(dict(run_id=run_id,**event))
    try:
        session,device,identity = connect(port=port)
        verify_device_report(image_path,image_id,device['serial_number'],identity['target_id'])
        receipt['device']=device; receipt['before']=identity
        if identity['target_id'] != image_id['target_id']:
            raise RuntimeError('connected target does not match platform image')
        if identity['build_id'] != image_id['build_id'] or identity['image_sha256'] != image_id['sha256']:
            previous=identity['generation']; receipt['device_write_attempted']=True; _save(receipt_path,receipt)
            session.upload(image,image_path=image_path)
            if session.verify()!=hashlib.sha256(image).digest(): raise RuntimeError('platform image verification mismatch')
            session.boot_ram(); session.close(); session=None
            serial_number=device['serial_number']
            deadline=time.monotonic()+min(600,max(30,len(image)/8192))
            while time.monotonic()<deadline:
                try:
                    session,device,identity=connect(serial_number=serial_number,deadline_seconds=min(3,deadline-time.monotonic()))
                except TimeoutError:
                    continue
                if (identity['generation']==previous+1 and identity['build_id']==image_id['build_id'] and
                    identity['image_sha256']==image_id['sha256'] and identity['image_kind']=='os'):
                    break
                session.close(); session=None
            if session is None: raise TimeoutError('platform image did not reconnect with exact identity/generation')
        if identity['image_kind']!='os' or identity['build_id']!=image_id['build_id'] or identity['image_sha256']!=image_id['sha256']:
            raise RuntimeError('platform OS identity mismatch after reconnect')
        generation=identity['generation']
        state=_state(session,generation)
        sequence=state['last_injection']
        record('handshake',identity=identity,state=state)
        def control(action,value=0):
            receipt['device_write_attempted']=True; _save(receipt_path,receipt)
            session.platform_control(generation,action,value)
        def inject(events):
            nonlocal sequence
            triples=[]
            for kind,value in events:
                sequence+=1
                if sequence>0xffffffff: raise RuntimeError('synthetic sequence exhausted')
                triples.append((sequence,kind,value))
            receipt['device_write_attempted']=True; _save(receipt_path,receipt)
            session.platform_input(generation,triples)
        def wait(predicate,seconds=5):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                current=_state(session,generation)
                if predicate(current): return current
                time.sleep(.02)
            raise TimeoutError('platform state did not reach expected condition')
        def settled():
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                before=_state(session,generation)
                if (before['frames_submitted']==before['frames_completed'] and
                        before['events_accepted']==before['events_processed'] and
                        not _ui_state(session,generation)['dirty']):
                    # Reset/control ACKs precede presentation. Return a fresh
                    # drained sample after the clean UI, never its older metrics.
                    after=_state(session,generation)
                    if (after['frames_submitted']==after['frames_completed']==before['frames_submitted'] and
                            after['events_accepted']==after['events_processed']==before['events_processed']):
                        return after
                time.sleep(.02)
            raise TimeoutError('platform presentation did not settle')
        # HELLO can become healthy before FAT discovery and the first idle turn.
        # A navigation event queued during that work measures startup latency,
        # not the platform's steady-state input path.
        wait_started=time.monotonic()
        deadline=wait_started+30
        stable_since=None
        while time.monotonic()<deadline:
            state=_state(session,generation)
            storage=session.storage_state()
            quiet=(storage[1] in (6,9) and storage[9]==0 and state['idle_entries']>0 and
                   state['frames_submitted']==state['frames_completed'])
            if quiet:
                if stable_since is None:stable_since=time.monotonic()
                if time.monotonic()-stable_since>=.5:break
            else:stable_since=None
            time.sleep(.02)
        else:raise TimeoutError('platform startup did not settle before input campaign')
        record('startup-settled',state=state,storage_phase=storage[1],startup_wait_seconds=round(time.monotonic()-wait_started,6))
        def capture(name):
            info,pixels=session.capture_screen(generation)
            (directory/f'{name}.rgb565').write_bytes(pixels)
            (directory/f'{name}.png').write_bytes(_png(pixels,info['width'],info['height']))
            record(name,screenshot=info,sha256=hashlib.sha256(pixels).hexdigest())
            return info['crc32']

        control(6); control(7); settled()
        before=_state(session,generation); initial_crc=capture('menu-initial')
        inject([(1,1)]); changed=wait(lambda s:s['selected']!=before['selected'])
        settled(); selected_crc=capture('menu-selected')
        if selected_crc==initial_crc: raise RuntimeError('menu navigation did not change framebuffer')
        record('menu-navigation',synthetic=True,state=changed)
        menu=settled(); inject([(2,16),(2,0)])
        entered=wait(lambda s:s['scene']!=menu['scene']); settled(); capture('selected-screen')
        inject([(2,1),(2,0)]); returned=wait(lambda s:s['scene']==menu['scene'])
        record('button-navigation',synthetic=True,entered=entered,returned=returned)

        base=settled()
        remaining=suite['navigation_events']
        while remaining:
            size=min(16,remaining)
            inject([(1,1 if (i%2)==0 else -1) for i in range(size)])
            settled(); remaining-=size
        storm=settled()
        if storm['events_processed']-base['events_processed'] < suite['navigation_events'] or storm['event_drops']!=base['event_drops']:
            raise RuntimeError('navigation storm lost events')
        if storm['selected']!=base['selected']:
            raise RuntimeError('balanced wheel storm changed the final selection')
        if storm['max_latency_us']>35000:
            raise RuntimeError('input event latency exceeded 35000 microseconds')
        record('navigation-storm',synthetic=True,events=suite['navigation_events'],before=base,after=storm)

        for kind,field in ((3,'hold'),(4,'headphones')):
            base=settled(); inject([(kind,1)]); on=settled()
            inject([(kind,0)]); off=settled()
            if on['events_processed']<=base['events_processed'] or off['events_processed']<=on['events_processed'] or on['synthetic_flags']==off['synthetic_flags']:
                raise RuntimeError(f'synthetic {field} transition was not observed')
            record(f'{field}-transitions',synthetic=True,on=on,off=off)
        base=settled(); inject([(3,1),(1,1)]); held=settled()
        if held['selected']!=base['selected']:
            raise RuntimeError('hold override failed to suppress wheel navigation')
        inject([(3,0),(1,1)]); released=settled()
        if released['selected']==held['selected']:
            raise RuntimeError('wheel navigation did not resume after hold release')
        record('hold-suppresses-navigation',synthetic=True,held=held,released=released)
        control(6)

        for index in range(3):
            control(3,25); low=_state(session,generation)
            control(3,100); high=_state(session,generation)
            if ((low['flags']>>8)&127)!=25 or ((high['flags']>>8)&127)!=100:
                raise RuntimeError('backlight state did not acknowledge applied brightness')
            record('backlight-levels',index=index+1,low=low,high=high,physical_visibility_verified=False)

        base=settled()
        cop_display=TARGETS[image_id['target_id']]['cop_display']
        if cop_display and not base['flags']&4:
            raise RuntimeError('COP is unavailable; CPU fallback cannot exercise the COP queue')
        control(5,suite['animation_frames'])
        # Native scenes without animation effects consume timer ticks without
        # submitting frames, including on COP targets. Exercise queue wrap with
        # visible selection changes and wait for each actual presentation.
        wait(lambda s:s['animation_frame']>=base['animation_frame']+suite['animation_frames'],15)
        display_base=settled()
        for index in range(suite['animation_frames']):
            prior=settled(); inject([(1,1 if index%2==0 else -1)])
            wait(lambda s:s['frames_completed']>prior['frames_completed'])
        animated=settled()
        if animated['frames_completed']<display_base['frames_completed']+suite['animation_frames']:
            raise RuntimeError('native display did not complete every changed selection')
        if cop_display and not animated['flags']&4:
            raise RuntimeError('COP was not active during the queue-wrap observation')
        settled(); capture('animation-final')
        record('animation-and-cop-wrap' if cop_display else 'animation-and-display-completion',before=base,after=animated,completed_delta=animated['frames_completed']-base['frames_completed'])
        if cop_display:
            base=settled(); control(4); reset=wait(lambda s:s['cop_resets']>base['cop_resets'] and s['flags']&4)
            record('cop-queue-reset',before=base,after=reset)

        for index in range(3):
            base=settled(); admission_deadline=time.monotonic()+15; retries=0
            while True:
                try:
                    sleep_started=time.monotonic()
                    control(1,suite['sleep_ms']);break
                except DeviceError:
                    session.close();session=None
                    if time.monotonic()>=admission_deadline:raise
                    session,observed_device,observed_identity=connect(
                        port=port,serial_number=device['serial_number'],
                        deadline_seconds=min(3,admission_deadline-time.monotonic()))
                    if (observed_device['serial_number']!=device['serial_number'] or
                            any(observed_identity[key]!=identity[key] for key in
                                ('target_id','build_id','image_sha256','generation'))):
                        raise RuntimeError('platform identity changed after sleep rejection')
                    current=_state(session,generation)
                    if (current['sleep_count']!=base['sleep_count'] or current['flags']&2 or
                            time.monotonic()>=admission_deadline):
                        raise
                    retries+=1;time.sleep(.05)
            asleep=_state(session,generation)
            if not asleep['flags']&2 or asleep['sleep_count']!=base['sleep_count']+1 or asleep['wake_count']!=base['wake_count']:
                raise RuntimeError('system did not remain asleep after acceptance')
            awake=wait(lambda s:s['sleep_count']>base['sleep_count'] and s['wake_count']>base['wake_count'] and not s['flags']&2,5)
            elapsed_ms=(time.monotonic()-sleep_started)*1000
            if elapsed_ms<suite['sleep_ms']-min(20,suite['sleep_ms']/10):
                raise RuntimeError('system woke before the requested sleep interval')
            record('sleep-auto-wake',index=index+1,before=base,asleep=asleep,after=awake,elapsed_ms=elapsed_ms,admission_retries=retries,physical_wake_verified=False)
        control(2); control(7); base=settled()
        time.sleep(suite['idle_observe_ms']/1000)
        idle=_state(session,generation)
        if idle['frames_submitted']!=base['frames_submitted'] or idle['events_processed']!=base['events_processed']:
            record('idle-observation-work',before=base,after=idle,observation_ms=suite['idle_observe_ms'])
            raise RuntimeError('idle observation performed redraw or event work')
        # S5L8702 still services USB and clickwheel through bounded foreground
        # pollers between its 1 ms IRQ waits, so roughly 40% measured IRQ-wait
        # time is normal even when the executive has no runnable work.
        idle_ratio = 250 if TARGETS[image_id['target_id']].get('hal_family') == 's5l8702' else 500
        if ((idle['idle_us']-base['idle_us'])&0xffffffff)<suite['idle_observe_ms']*idle_ratio:
            raise RuntimeError('idle time did not advance; executive may be polling')
        record('idle-no-redraw',before=base,after=idle,observation_ms=suite['idle_observe_ms'])
        capture('platform-final')
        receipt['automated_passed']=True
        record('physical-confirmation-required',checks=receipt['physical_checks_pending'])
    except (ValueError,OSError,RuntimeError) as exc:
        receipt['failure']=str(exc); record('failed',detail=str(exc))
    finally:
        if session:
            session.close()
        receipt['finished_at']=datetime.now(timezone.utc).isoformat(); _save(receipt_path,receipt)
    return receipt
