"""Bounded development music controls; mutations are sent once after identity check."""
import struct
from .identity import decode
from .protocol import Command
from tools.studio.targets import TARGETS

STATE_FORMATS = {
    'files-state':(Command.ATA_DIAGNOSTICS,b'FILE',('magic','phase','view','depth','entries','page','indexed','pending','validating','done','load_us','requests','read_bytes','loads','now_us','loading')),
    'audio-state':(Command.AUDIO_STATE,b'AUDS',('magic','reserved','rate','queued','free_frames','consumed','submitted','interrupts','underruns','silence_frames','fault','playing','paused','initialized','last_irq_us','max_irq_us','volume_db_tenths','fifo_empty_irqs','max_completion_interval_us','max_completion_late_us')),
    'player-state':(Command.PLAYER_STATE,b'PLYR',('magic','phase','error','generation','rate','decoded','rendered','underruns','frames_low','frames_high','next_phase','next_error','gapless_joins','missed_joins','position_low','position_high')),
    'player-timing-state':(Command.PLAYER_TIMING_STATE,b'TIME',('magic','reserved','decode_calls','decode_us','decode_max_us','dsp_calls','dsp_frames','dsp_us','dsp_max_us','runtime_calls','runtime_us','runtime_max_us','music_us','storage_us','link_us','clock_hz','cache_control','cache_priority','idle_us','clock_source','decoder_kind','mp3_frame_calls','mp3_frame_us','mp3_frame_max_us','mp3_synth_calls','mp3_synth_us','mp3_synth_max_us','mp3_pcm_calls','mp3_pcm_frames','mp3_pcm_us','mp3_pcm_max_us')),
    'library-state':(Command.LIBRARY_STATE,b'LIBS',('magic','reserved','phase','error','tracks','groups','verified_bytes','revision')),
    'photos-state':(Command.PHOTOS_STATE,b'PHOT',('magic','reserved','phase','error','active','ready','pending','view','file_size','verify_at','data_at','verify_crc','metadata_crc','verified_photos','verified_levels','verified_tiles','photo_index','visible_tiles','revision','navigation','frame_valid','frame_complete','slide_armed','slide_at_us','now_us','view_started_us','frame_ready_us','io_started_us','max_open_us','max_read_us','render_us','jpeg_us')),
    'video-state':(Command.VIDEO_STATE,b'VIDS',('magic','reserved','phase','error','active','pending','paused','next_frame','frame_count','front_frame','read_ops','read_bytes','prepared','presented','late_frames','max_late_samples','max_prepare_us','queued_frames','max_queued_frames','audio_underruns','audio_silence_frames','audio_fault','submitted_samples','revision','max_io_us','max_record_read_us','max_record_crc_us','max_payload_crc_us','max_decode_us','max_frame_crc_us','max_step_us','min_audio_queued')),
}
PLAYER_CONTROLS = {"player-pause": 1, "player-resume": 2, "player-stop": 3, "player-seek": 4}
ACTIONS = (*STATE_FORMATS, "player-open", "video-open", *PLAYER_CONTROLS)

def command(session,identity,action,*,path=None,sample=None):
    if action.endswith('-state'):
        operation,magic,names=STATE_FORMATS[action]
        raw=session.request(operation,struct.pack('<I',19)) if action=='files-state' else session.request(operation)
        if action=='video-state' and TARGETS[identity['target_id']]['media_video_backend']=='bcm-mp4':
            if len(raw)!=128 or struct.unpack_from('<I',raw)[0]!=0x4d250002:
                raise RuntimeError('Malformed native video state reply')
            words=struct.unpack('<32I',raw)
            # The recovery-mode latch stays set in RESTORED (17) and FAILED
            # (18). It does not mean recovery is still running in those states.
            state=dict(backend='bcm2722',phase=words[1],error=words[2],
                       restoring=bool(words[3]) and words[1] not in (0,17,18),
                       display_owned=bool(words[7]),restore_error=words[20],
                       position_ms=words[24],duration_ms=words[25],active=bool(words[26]),paused=bool(words[29]))
            return dict(identity=identity,state=state)
        if action=='video-state':
            if TARGETS[identity['target_id']].get('media_video_backend')=='native-mp4':
                names=('magic','reserved','phase','error','active','pending','paused','next_frame',
                       'frame_count','front_frame','read_ops','read_bytes','decoded','presented',
                       'late_frames','max_late_ms','max_decode_call_us','frame_ready','frame_offered',
                       'decode_wall_us','rgb_us','seek_phase','audio_frames','revision','aac_us',
                       'io_us','dropped_frames','codec_status','codec_fault','codec_detail','position_ms','duration_ms')
        expected=64 if action=='library-state' else len(names)*4
        if len(raw)!=expected or raw[:4]!=magic:raise RuntimeError('Malformed music state reply')
        state=dict(zip(names,struct.unpack('<'+'I'*len(names),raw[:len(names)*4])))
        if state.pop('reserved', 0):raise RuntimeError('Malformed music state reply')
        for key in ('volume_db_tenths','error'):
            if key in state and state[key]>=0x80000000:state[key]-=0x100000000
        if action=='library-state':state['library_sha256']=raw[32:].hex()
        if action=='player-state':state['frames']=state.pop('frames_low')|(state.pop('frames_high')<<32)
        if 'position_low' in state:state['position']=state.pop('position_low')|(state.pop('position_high')<<32)
        return dict(identity=identity,state=state)
    confirmed=decode(session.status())
    if any(confirmed[key]!=identity[key] for key in ('target_id','build_id','generation')):
        raise RuntimeError('Device identity changed during music preflight')
    generation=identity['generation']
    if action=='video-open':
        session.video_open(generation,path)
        return dict(identity=identity,accepted=action)
    if action=='player-open':
        encoded=path.encode('ascii')
        if not encoded.startswith(b'/') or len(encoded)>255 or b'\0' in encoded:
            raise ValueError('Player path must be an absolute ASCII filesystem path of at most255 bytes')
        operation=Command.PLAYER_OPEN;payload=struct.pack('<I',generation)+encoded+b'\0';value=0
    else:
        value=PLAYER_CONTROLS[action]
        operation=Command.PLAYER_CONTROL;payload=struct.pack('<II',generation,value)
        if value==4:
            if sample is None or not 0<=sample<=0xffffffffffffffff:raise ValueError('Sample must be an unsigned64-bit position')
            payload+=struct.pack('<Q',sample)
    reply=session.request(operation,payload)
    if reply!=struct.pack('<II',generation,value):
        session.poisoned = True
        from .session import AmbiguousCommand
        raise AmbiguousCommand('Unexpected music mutation acknowledgement; inspect state before further action')
    return dict(identity=identity,accepted=action)
