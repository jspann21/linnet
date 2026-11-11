#include "services/music/player.h"
#include "services/music/decoder.h"
#include "services/storage/storage.h"
#include "services/audio/dsp.h"
#include "hal/api/audio.h"
#include "hal/api/ata.h"
#include "hal/api/platform.h"
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
#define PLAYER_COMPRESSED_CACHE_BYTES 0x300000u
#define PLAYER_COMPRESSED_CACHE(window) ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x100000u+(window)*PLAYER_COMPRESSED_CACHE_BYTES))
#define PLAYER_TABLE_CACHE_BYTES 0x80000u
#define PLAYER_TABLE_CACHE(table) ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+(table)*PLAYER_TABLE_CACHE_BYTES))
_Static_assert(2u*PLAYER_TABLE_CACHE_BYTES<=0x100000u,"table cache overlaps compressed windows");
_Static_assert(0x100000u+2u*PLAYER_COMPRESSED_CACHE_BYTES+
    (LINNET_AUDIO_RING_FRAMES+1152u)*4u<=LINNET_STAGE_CAPACITY,
    "compressed read cache overlaps PCM staging");
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#include "kernel/development.h"
#endif
#define PLAYER_DECODE_FRAMES 1152u
/* Music, video and firmware upload have exclusive ownership of STAGE. */
#define RAW_PCM ((int16_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+LINNET_STAGE_CAPACITY-LINNET_AUDIO_RING_FRAMES*4u))
#define EFFECT_PCM (RAW_PCM-PLAYER_DECODE_FRAMES*2u)
_Static_assert((LINNET_AUDIO_RING_FRAMES+PLAYER_DECODE_FRAMES)*4u<=LINNET_STAGE_CAPACITY,"PCM cache exceeds staging");
static uint32_t raw_head,effect_cursor;
static bool effect_pending;
static bool effect_paused_full;
static uint32_t output_headroom,pending_headroom,headroom_boundary;
static bool headroom_pending,gain_waiting;
static bool (*gain_sync)(void);
enum slot_phase {SLOT_EMPTY,SLOT_OPEN,SLOT_DECODE,SLOT_EOF,SLOT_FAILED};
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
struct player_cache_window {uint32_t start,valid,target;bool filling;};
struct player_table_cache {uint32_t start,valid,target;};
#endif
struct player_slot {
    struct ln_decoder decoder;struct ln_dsp dsp;struct ln_fat32_file media;
    char path[LN_FAT32_PATH_MAX+1];int16_t pcm[PLAYER_DECODE_FRAMES*2u];
    uint64_t expected,initial,position_base,submitted;
    uint32_t phase,generation,token,buffered,rate,config_rate;bool seek_initial;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    struct player_cache_window cache[2];
    struct player_table_cache tables[2];
    uint32_t cache_current;
    bool cache_enabled,cache_initial,cache_pending,cache_pause_canceling,cache_codec_checked,cache_demand_pending;
#endif
};
static struct player_slot slots[LINNET_DECODER_SLOTS];
static bool (*file_configure)(uint32_t,struct ln_dsp_config *);
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
static uint32_t video_firmware_generation;
static bool video_firmware_owned;
uint8_t *ln_player_video_firmware_memory(uint32_t *generation){
    if(!generation||video_firmware_owned||ln_player_busy()||ln_decoder_workspace_bytes()<0xa0000u)return 0;
    video_firmware_owned=true;
    *generation=video_firmware_generation;return (uint8_t *)ln_decoder_workspace_at(1);
}
void ln_player_video_firmware_release(void){video_firmware_owned=false;}
#define DECODER_WORKSPACE_CHANGED() (++video_firmware_generation)
#else
#define DECODER_WORKSPACE_CHANGED() ((void)0)
#endif
static struct ln_player_status state;
#if !defined(LINNET_PROFILE_RELEASE)
static struct ln_player_timing timing;
static void timing_add(uint32_t *v,uint32_t n){*v=n>UINT32_MAX-*v?UINT32_MAX:*v+n;}
void ln_player_timing_snapshot(struct ln_player_timing *out){if(out)*out=timing;}
void ln_player_timing_runtime(uint32_t elapsed){timing_add(&timing.runtime_calls,1);timing_add(&timing.runtime_us,elapsed);if(elapsed>timing.runtime_max_us)timing.runtime_max_us=elapsed;}
void ln_player_timing_stage(uint32_t stage,uint32_t elapsed){if(stage==0)timing_add(&timing.music_us,elapsed);else if(stage==1)timing_add(&timing.storage_us,elapsed);else if(stage==2)timing_add(&timing.link_us,elapsed);}
#endif
static struct ln_player_transition transition;
static struct player_slot *io_owner;
static uint32_t io_offset,io_length;static uint8_t *io_buffer;static bool io_open;
static uint32_t audible,feed,generation_counter,last_consumed,join_underruns;
uint32_t ln_player_decoder_kind(void){return slots[audible].decoder.kind;}
static uint64_t consumed,submitted,audible_origin,join_boundary;
static bool stop_requested,seek_requested,pause_requested,have_next,cancel_next;
static bool pause_io_canceling;
static bool skip_requested,seek_paused;
static bool audio_ready,join_pending,join_gapless,event_pending,finish_sent,stepping;
static uint64_t seek_target;
static bool next_expected,eof_waiting;
static uint32_t eof_deadline;
void ln_player_set_gain_sync(bool (*sync)(void)){gain_sync=sync;}
uint32_t ln_player_headroom(void){
    if(!audio_ready)return 0;
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    if(!audio.initialized)return 0;
    /* Old PCM can remain in the skipped DMA prefix or in a rewrite overtaken
     * by playback. Increasing gain waits past ALL previously submitted PCM. */
    if(headroom_pending&&(int32_t)(audio.consumed-headroom_boundary)>=0){
        output_headroom=pending_headroom;headroom_pending=false;
    }
    return output_headroom;
}
bool ln_player_sound_pending(void){return effect_pending||headroom_pending||gain_waiting;}
static bool synchronize_gain(void){
    gain_waiting=gain_sync?!gain_sync():output_headroom!=0;return !gain_waiting;
}
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
bool ln_player_startup_disk(void){
    return state.phase>=LN_PLAYER_OPEN&&state.phase<=LN_PLAYER_PRIME&&
        !seek_paused&&slots[feed].phase!=SLOT_EOF;
}
#endif
uint32_t ln_player_background_frames(uint32_t rate){
    uint32_t frames=rate/4u*3u;
    if(frames<8192u)frames=8192u;
    if(frames>LINNET_AUDIO_RING_FRAMES-2048u)frames=LINNET_AUDIO_RING_FRAMES-2048u;
    return frames;
}
static uint32_t startup_frames(uint32_t rate){
    struct ln_ata_info media;ln_ata_snapshot(&media);
    if(media.media_kind!=LN_ATA_MEDIA_SOLID_STATE)return ln_player_background_frames(rate);
    /* Flash does not need the disk-seek runway before its first audible PCM.
     * Keep the larger background threshold so decoding still fills ahead of
     * artwork, browsing and next-track work after playback starts. */
    uint32_t frames=rate/4u;
    if(frames<8192u)frames=8192u;
    if(frames>LINNET_AUDIO_RING_FRAMES-2048u)frames=LINNET_AUDIO_RING_FRAMES-2048u;
    return frames;
}
static uint32_t prefetch_frames(uint32_t rate){
    /* Opening the next track can hold the shared reader for longer than the
     * ordinary background window. Refill current PCM before admitting it. */
    uint32_t frames=rate+rate/4u;
    if(frames<8192u)frames=8192u;
    if(frames>LINNET_AUDIO_RING_FRAMES-2048u)frames=LINNET_AUDIO_RING_FRAMES-2048u;
    return frames;
}
void ln_player_expect_next(bool expected){
    next_expected=expected&&!stop_requested&&!seek_requested&&!skip_requested&&
        state.phase!=LN_PLAYER_IDLE&&state.phase!=LN_PLAYER_STOPPING&&state.phase!=LN_PLAYER_ERROR;
}
bool ln_player_prefetch_urgent(void){
    return (next_expected||have_next)&&audio_ready&&slots[feed].phase==SLOT_EOF&&
        (state.phase==LN_PLAYER_PLAY||state.phase==LN_PLAYER_DRAIN);
}
static bool interrupted(void){return stop_requested||seek_requested||cancel_next||skip_requested;}
static uint32_t generation(void){if(++generation_counter==0||generation_counter>=0x10000000u)generation_counter=1;return generation_counter;}
static bool supported_rate(uint32_t rate){
    switch(rate){case 8000:case 11025:case 12000:case 16000:case 22050:case 24000:
    case 32000:case 44100:case 48000:case 88200:case 96000:return true;default:return false;}
}
static int poll_media(struct player_slot *s,struct ln_fat32_file *file,uint32_t *transferred){
    bool starting=state.phase==LN_PLAYER_OPEN||state.phase==LN_PLAYER_HEADER||state.phase==LN_PLAYER_PRIME;
    uint32_t began=ln_time_us();int result;
    for(uint32_t step=0;;step++){
        result=ln_storage_media_poll_deferred(s->generation,file,transferred);
        if(result||!starting||step==3u||interrupted())return result;
        uint32_t now=ln_time_us(),deadline;
        if(now-began>=500u||(ln_storage_idle_wait(now,&deadline)&&(int32_t)(deadline-now)>0))return 0;
    }
}
static int read_disk(struct player_slot *s,uint32_t offset,uint8_t *buffer,uint32_t length){
    if(io_owner&&io_owner!=s)return 0;
    if(!io_owner){
        if(have_next&&!join_pending&&s==&slots[audible^1u]&&audio_ready&&state.phase==LN_PLAYER_PLAY){
            struct ln_audio_status audio;ln_audio_snapshot(&audio);
            if(slots[feed].phase!=SLOT_EOF){
                if(audio.queued<prefetch_frames(audio.rate))return 0;
            }else if(eof_waiting&&(int32_t)(ln_time_us()-eof_deadline)>=0)return 0;
        }
        int r=ln_storage_media_read(&s->media,offset,buffer,length,s->generation);if(r<=0)return r;
        io_owner=s;io_open=false;io_offset=offset;io_length=length;io_buffer=buffer;
    }else if(io_open||io_offset!=offset||io_length!=length||io_buffer!=buffer)return -1;
    uint32_t transferred=0;int r=poll_media(s,0,&transferred);
    if(!r)return 0;io_owner=0;return r==1&&transferred==length?1:-1;
}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
static int cache_hit(struct player_slot *s,uint32_t at,uint32_t length){
    for(uint32_t i=0;i<2u;++i){
        const struct player_cache_window *w=&s->cache[i];
        if(at>=w->start&&at-w->start<=w->valid&&length<=w->valid-(at-w->start))return (int)i;
    }
    return -1;
}
static bool cache_metadata(const struct player_slot *s,uint32_t at){
    /* Indexed AAC seeks read small tables far from their media packets. */
    struct ln_decoder_cache_layout layout;
    return ln_decoder_cache_layout(&s->decoder,&layout)&&layout.media_end>layout.media_start&&
        ((uint64_t)at<layout.media_start||(uint64_t)at>=layout.media_end);
}
static bool cache_retain_tables(struct player_slot *s,uint32_t replacing){
    struct ln_decoder_cache_layout layout;
    if(!ln_decoder_cache_layout(&s->decoder,&layout))return true;
    struct ln_fat32_file root;
    if(!layout.tables_ready||!ln_storage_media_root(&root)||root.epoch!=s->media.epoch)return true;
    /* The rotating-media owner can retain tables in the unused STAGE prefix.
     * Copy only a complete resident range; oversized/nonresident tables keep
     * the ordinary read path. AAC's source reads include up to 16383 bytes
     * beyond the logical table, so retain actual file bytes there as well. */
    bool ready=true;
    for(uint32_t i=0;i<2u;i++){
        struct player_table_cache *t=&s->tables[i];
        if((t->target&&t->valid==t->target)||!layout.tables[i].bytes)continue;
        uint64_t start=layout.tables[i].offset;
        uint64_t bytes=layout.tables[i].bytes;
        if(start>UINT32_MAX||start>=s->media.size||bytes>s->media.size-start)continue;
        uint64_t tail=s->media.size-start-bytes;
        if(tail>16383u)tail=16383u;
        bytes+=tail;
        if(!bytes||bytes>PLAYER_TABLE_CACHE_BYTES)continue;
        int hit=cache_hit(s,(uint32_t)start,(uint32_t)bytes);
        if(hit<0)continue;
        if(t->start!=(uint32_t)start||t->target!=(uint32_t)bytes)
            *t=(struct player_table_cache){(uint32_t)start,0,(uint32_t)bytes};
        uint32_t copy=t->target-t->valid;if(copy>32768u)copy=32768u;
        __builtin_memcpy(PLAYER_TABLE_CACHE(i)+t->valid,PLAYER_COMPRESSED_CACHE((uint32_t)hit)+
            ((uint32_t)start-s->cache[hit].start)+t->valid,copy);
        t->valid+=copy;
        /* Postpone speculative bank reuse until its resident table is safe.
         * Each admission attempt copies at most 32 KiB per table. */
        if((uint32_t)hit==replacing&&t->valid!=t->target)ready=false;
    }
    return ready;
}
static bool cache_table_copy(struct player_slot *s,uint32_t at,uint8_t *buffer,uint32_t length){
    for(uint32_t i=0;i<2u;i++){
        const struct player_table_cache *t=&s->tables[i];
        if(t->target&&t->valid==t->target&&at>=t->start&&at-t->start<=t->valid&&length<=t->valid-(at-t->start)){
            struct ln_fat32_file root;
            if(!ln_storage_media_root(&root)||root.epoch!=s->media.epoch)return false;
            __builtin_memcpy(buffer,PLAYER_TABLE_CACHE(i)+(at-t->start),length);return true;
        }
    }
    return false;
}
uint8_t *ln_player_art_memory(uint32_t *bytes){
    if(!bytes)return 0;
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    if(!disk.initialized||disk.media_kind!=LN_ATA_MEDIA_SOLID_STATE)return 0;
    uint32_t extra_bytes;if(ln_platform_extended_memory(&extra_bytes))return 0;
    /* Only rotating media enables these compressed windows. Preserve any
     * already admitted cache owner if classification ever changes. */
    for(uint32_t i=0;i<2u;i++)if(slots[i].cache_enabled||slots[i].cache_pending)return 0;
    uint32_t end=LINNET_STAGE_CAPACITY-(LINNET_AUDIO_RING_FRAMES+1152u)*4u;
    if(end<=0x38000u)return 0;
    *bytes=end-0x38000u;return (uint8_t*)(uintptr_t)(LINNET_STAGE_ADDRESS+0x38000u);
}
static bool cache_begin(struct player_slot *s,uint32_t window,uint32_t at,bool initial,uint32_t minimum){
    if(at>=s->media.size)return false;
    if(!cache_retain_tables(s,window))return false;
    uint64_t available=s->media.size-at;
    struct ln_decoder_cache_layout layout;
    if(ln_decoder_cache_layout(&s->decoder,&layout)){
        if(layout.media_end>layout.media_start){
            uint64_t end=(uint64_t)at<layout.media_start?layout.media_start:
                (uint64_t)at<layout.media_end?layout.media_end:s->media.size;
            uint64_t region=end-(uint64_t)at;
            if(region<minimum)region=minimum;
            if(region<available)available=region;
        }
    }
    /* Every subsequent storage offset must remain representable. */
    if(available>UINT32_MAX-at)available=UINT32_MAX-at;
    if(!available)return false;
    struct player_cache_window *w=&s->cache[window];
    w->start=at;w->valid=0;
    w->target=cache_metadata(s,at)?65536u:
        initial&&s->decoder.kind!=LN_DEC_AAC_M4A?131072u:PLAYER_COMPRESSED_CACHE_BYTES;
    if(w->target>available)w->target=(uint32_t)available;
    w->filling=true;return true;
}
static int cache_progress(struct player_slot *s){
    /* A demanded decoder read keeps its exact buffer until its callback takes
     * completion. Do not poll that owner through a speculative cache tuple. */
    if(s->cache_demand_pending)return 0;
    for(uint32_t i=0;i<2u;++i){
        struct player_cache_window *w=&s->cache[i];
        if(!w->filling)continue;
        uint32_t chunk=w->target-w->valid;
        if(chunk>LN_STORAGE_MEDIA_MAX_READ)chunk=LN_STORAGE_MEDIA_MAX_READ;
        int r=read_disk(s,w->start+w->valid,PLAYER_COMPRESSED_CACHE(i)+w->valid,chunk);
        if(r<=0)return r;
        w->valid+=chunk;
        if(w->valid==w->target)w->filling=false;
        return 1;
    }
    return 1;
}
static int cache_demand_read(struct player_slot *s,uint32_t at,uint8_t *buffer,uint32_t length){
    int result=read_disk(s,at,buffer,length);
    s->cache_demand_pending=result==0;
    return result;
}
static int cache_copy(struct player_slot *s,uint32_t window,uint32_t at,uint8_t *buffer,uint32_t length){
    struct player_cache_window *w=&s->cache[window];
    __builtin_memcpy(buffer,PLAYER_COMPRESSED_CACHE(window)+(at-w->start),length);
    s->cache_current=window;
    if(cache_progress(s)<0)return -1;
    uint32_t consumed=at-w->start+length;
    uint32_t threshold=w->target/2u;if(threshold>524288u)threshold=524288u;
    /* A window opened before AAC resolves mdat can contain tables and audio.
     * Classify the consumed range and its next start, retaining the filled prefix. */
    if(!w->filling&&!cache_metadata(s,at)&&w->target-consumed<=threshold){
        uint32_t next=w->start+w->target,other=window^1u;
        struct player_cache_window *candidate=&s->cache[other];
        if(next<s->media.size&&!cache_metadata(s,next)&&!candidate->filling&&
           (candidate->start!=next||candidate->valid!=candidate->target)&&
           cache_begin(s,other,next,false,0)&&cache_progress(s)<0)return -1;
    }
    return 1;
}
static int cache_cached(struct player_slot *s,uint32_t at,uint8_t *buffer,uint32_t length){
    int hit=cache_hit(s,at,length);
    if(hit>=0)return cache_copy(s,(uint32_t)hit,at,buffer,length);
    for(uint32_t i=0;i<2u;++i){
        const struct player_cache_window *w=&s->cache[i];
        if(at<w->start||at-w->start>=w->valid)continue;
        uint32_t first=w->valid-(at-w->start);
        if(first>=length)continue;
        int next=cache_hit(s,at+first,length-first);
        if(next<0)continue;
        __builtin_memcpy(buffer,PLAYER_COMPRESSED_CACHE(i)+(at-w->start),first);
        return cache_copy(s,(uint32_t)next,at+first,buffer+first,length-first);
    }
    return 0;
}
#endif
static int read_media(void *context,uint64_t offset,uint8_t *buffer,uint32_t length){
    struct player_slot *s=context;if(offset>UINT32_MAX)return -1;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    if(s->cache_enabled&&length<=LN_STORAGE_MEDIA_MAX_READ){
        uint32_t at=(uint32_t)offset;
        if(length>UINT32_MAX-at)return read_disk(s,at,buffer,length);
        if(s->cache_demand_pending)return cache_demand_read(s,at,buffer,length);
        if(cache_table_copy(s,at,buffer,length))return 1;
        int hit=cache_cached(s,at,buffer,length);
        if(hit)return hit;
        /* Muted preparation and a healthy PCM runway keep the sequential fill.
         * Prioritize a missing region only before the active feed runs low. */
        bool demand_priority=audio_ready&&state.phase==LN_PLAYER_PLAY&&s==&slots[feed]&&
            ln_audio_capacity()>LINNET_AUDIO_RING_FRAMES-ln_player_background_frames(s->rate)/2u;
        /* Poll only an already admitted chunk; a required miss must get the
         * next admission before another speculative chunk can start. */
        if((io_owner==s||!demand_priority)&&cache_progress(s)<0)return -1;
        hit=cache_cached(s,at,buffer,length);
        if(hit)return hit;
        if(s->cache[0].filling||s->cache[1].filling){
            /* Finish an admitted cache chunk before servicing another region.
             * Retain its validated prefix and resume the same large fill once
             * the demanded read has retired, rather than waiting for its tail. */
            if(io_owner||!demand_priority)return 0;
            return cache_demand_read(s,at,buffer,length);
        }
        if(at>s->media.size||length>s->media.size-at)return -1;
        uint32_t window=s->cache_current^1u;
        uint32_t begin=at;
        if(s->cache_initial&&s->decoder.kind==LN_DEC_AAC_M4A&&!cache_metadata(s,at)){
            begin=at&~(131072u-1u);
            struct ln_decoder_cache_layout layout;
            if(ln_decoder_cache_layout(&s->decoder,&layout)&&layout.media_end>layout.media_start&&
               begin<layout.media_start)begin=(uint32_t)layout.media_start;
        }
        if(!cache_begin(s,window,begin,s->cache_initial,length+(at-begin)))
            return cache_demand_read(s,at,buffer,length);
        s->cache_initial=false;
        if(cache_progress(s)<0)return -1;
        return cache_cached(s,at,buffer,length);
    }
#endif
    return read_disk(s,(uint32_t)offset,buffer,length);
}
static void clear_slot(struct player_slot *s){
    ln_decoder_close(&s->decoder);s->phase=SLOT_EMPTY;s->buffered=0;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    s->cache_enabled=s->cache_pending=s->cache_pause_canceling=s->cache_demand_pending=false;
    s->cache[0].filling=s->cache[1].filling=false;
    for(uint32_t i=0;i<2u;i++)s->tables[i]=(struct player_table_cache){0};
#endif
}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
static bool cache_supported(const struct player_slot *s){
    const char *name=s->path;
    uint32_t n=0;while(name[n])++n;
    bool mp3=n>=4u&&name[n-4u]=='.'&&
       (name[n-3u]=='m'||name[n-3u]=='M')&&
       (name[n-2u]=='p'||name[n-2u]=='P')&&name[n-1u]=='3';
    bool m4a=n>=4u&&name[n-4u]=='.'&&
        (name[n-3u]=='m'||name[n-3u]=='M')&&name[n-2u]=='4'&&
        (name[n-1u]=='a'||name[n-1u]=='A'||name[n-1u]=='b'||name[n-1u]=='B');
    bool direct=name[0]=='/'&&name[1]=='F'&&name[2]=='I'&&name[3]=='L'&&
        name[4]=='E'&&!name[5];
    bool eligible=mp3||(direct&&s->decoder.kind==LN_DEC_MP3)||
        ((m4a||direct)&&s->decoder.kind==LN_DEC_AAC_M4A&&s->decoder.opened);
    if(!eligible)return false;
    struct ln_ata_info media;ln_ata_snapshot(&media);
    return media.media_kind==LN_ATA_MEDIA_ROTATING;
}
static void activate_cache(struct player_slot *s){
    if(io_owner==s){s->cache_pending=true;return;}
    s->cache_enabled=cache_supported(s);s->cache_pending=s->cache_demand_pending=false;
    s->cache_initial=true;s->cache_current=1u;
    for(uint32_t i=0;i<2u;++i)s->cache[i]=(struct player_cache_window){0};
    for(uint32_t i=0;i<2u;++i)s->tables[i]=(struct player_table_cache){0};
}
#endif
static bool name_copy(char *out,const char *name){
    if(!name||name[0]!='/')return false;uint32_t n=0;
    while(n<=LN_FAT32_PATH_MAX&&name[n])n++;if(!n||n>LN_FAT32_PATH_MAX)return false;
    for(uint32_t i=0;i<=n;i++)out[i]=name[i];return true;
}
static bool start_slot(struct player_slot *s,const char *name,uint64_t expected,uint64_t position,uint32_t token,
                       const struct ln_dsp *dsp,uint32_t config_rate){
    if(!name_copy(s->path,name))return false;
    s->expected=expected;s->initial=position;s->position_base=0;s->submitted=0;s->token=token;
    s->generation=generation();s->buffered=0;s->rate=0;s->seek_initial=position!=0;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    s->cache_enabled=s->cache_pending=s->cache_pause_canceling=s->cache_codec_checked=s->cache_demand_pending=false;
    s->cache_initial=true;s->cache_current=1u;
    for(uint32_t i=0;i<2u;++i)s->cache[i]=(struct player_cache_window){0};
    for(uint32_t i=0;i<2u;++i)s->tables[i]=(struct player_table_cache){0};
#endif
    s->phase=SLOT_OPEN;s->dsp=*dsp;s->config_rate=config_rate;return true;
}
static void fail(uint32_t error){
    next_expected=eof_waiting=false;
    state.error=error;stop_requested=true;state.phase=LN_PLAYER_STOPPING;(void)ln_audio_stop();
    if(io_owner)ln_storage_media_cancel(io_owner->generation);
}
/* The pending owner alone polls storage, returning to the executive after
 * bounded work. Keep interruption checks for controls from other service paths. */
static int slot_step(struct player_slot *s){
    if(s->phase==SLOT_OPEN){
        if(io_owner&&io_owner!=s)return 0;
        if(!io_owner){int r=ln_storage_media_open(s->path,s->generation);if(r<=0)return r;io_owner=s;io_open=true;}
        int r=poll_media(s,&s->media,0);if(!r)return 0;io_owner=0;
        if(interrupted())return 0;
        if(r<0||(s->expected&&s->expected!=s->media.size))return -1;
        DECODER_WORKSPACE_CHANGED();
        ln_decoder_init(&s->decoder,ln_decoder_workspace_at((uint32_t)(s-slots)),read_media,s,s->media.size);s->phase=SLOT_DECODE;return 0;
    }
    if(s->phase==SLOT_EOF)return 2;if(s->phase!=SLOT_DECODE||s->buffered)return 0;
    uint32_t frames=0;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t started=ln_time_us();
#endif
    int r=ln_decoder_decode(&s->decoder,s->pcm,PLAYER_DECODE_FRAMES,&frames);
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-started;timing_add(&timing.decode_calls,1);timing_add(&timing.decode_us,elapsed);
    if(elapsed>timing.decode_max_us)timing.decode_max_us=elapsed;
#endif
    if(interrupted())return 0;if(r<0)return r;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    if(!s->cache_codec_checked&&
       ((s->decoder.kind==LN_DEC_MP3&&s->path[0]=='/'&&s->path[1]=='F'&&
         s->path[2]=='I'&&s->path[3]=='L'&&s->path[4]=='E'&&!s->path[5])||
        (s==&slots[audible]&&s->decoder.kind==LN_DEC_AAC_M4A&&s->decoder.opened))){
        s->cache_codec_checked=true;activate_cache(s);
    }
#endif
    if(s->decoder.opened&&s->decoder.rate){
        s->rate=s->decoder.rate;if(!supported_rate(s->rate))return -4;
        if(file_configure&&!s->config_rate){struct ln_dsp_config config;
            if(!file_configure(s->rate,&config)||!ln_dsp_configure(&s->dsp,&config))return -4;
            s->config_rate=s->rate;}
        if(s->config_rate&&s->config_rate!=s->rate)return -4;
        if(s->seek_initial){
            if(io_owner)return 0;
            if(s->decoder.total_frames&&s->initial>=s->decoder.total_frames)s->initial=0;
            if(!ln_decoder_seek(&s->decoder,s->initial))return -3;
            s->position_base=s->initial;s->seek_initial=false;return 0;
        }
    }
    s->buffered=frames;if(r==2)s->phase=SLOT_EOF;return r;
}
static void next_failure(int error){
    struct player_slot *s=&slots[audible^1u];state.next_error=(uint32_t)(error<0?-error:error);
    state.next_phase=LN_PLAYER_NEXT_FAILED;s->phase=SLOT_FAILED;s->buffered=0;ln_decoder_close(&s->decoder);
}
static void advance_next(void){
    if(!have_next||join_pending||state.next_phase==LN_PLAYER_NEXT_FAILED||cancel_next)return;
    struct player_slot *s=&slots[audible^1u];int r=slot_step(s);if(interrupted())return;
    if(r<0){next_failure(r);return;}
    if(s->buffered&&s->rate)state.next_phase=LN_PLAYER_NEXT_READY;
    else if(s->phase==SLOT_EOF)next_failure(-2);
}
static bool initialize_audio(uint32_t rate){
    if(!ln_audio_init(rate))return false;last_consumed=raw_head=0;effect_pending=effect_paused_full=false;
    output_headroom=slots[feed].dsp.config.headroom_db_tenths;headroom_pending=gain_waiting=false;
    submitted=consumed;audio_ready=true;finish_sent=false;eof_waiting=false;return true;
}
static void process_pcm(struct player_slot *s,int16_t *pcm,uint32_t frames){
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t started=ln_time_us();
#endif
    ln_dsp_process(&s->dsp,pcm,frames);
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-started;timing_add(&timing.dsp_calls,1);timing_add(&timing.dsp_frames,frames);timing_add(&timing.dsp_us,elapsed);
    if(elapsed>timing.dsp_max_us)timing.dsp_max_us=elapsed;
#endif
}
static bool submit_slot(struct player_slot *s){
    if(!s->buffered)return true;if(ln_audio_capacity()<s->buffered)return false;
    uint32_t at=raw_head&(LINNET_AUDIO_RING_FRAMES-1u);
    uint32_t first=LINNET_AUDIO_RING_FRAMES-at;
    if(first>s->buffered)first=s->buffered;
    __builtin_memcpy(RAW_PCM+2*at,s->pcm,first*4u);
    if(first<s->buffered)
        __builtin_memcpy(RAW_PCM,s->pcm+2*first,(s->buffered-first)*4u);
    raw_head+=s->buffered;process_pcm(s,s->pcm,s->buffered);
    if(ln_audio_submit(s->pcm,s->buffered)!=s->buffered){fail(14);return false;}
    s->submitted+=s->buffered;submitted+=s->buffered;s->buffered=0;return true;
}
static bool next_joinable(const struct player_slot *s){
    const struct player_slot *next=&slots[audible^1u];
    return have_next&&!join_pending&&!finish_sent&&state.next_phase==LN_PLAYER_NEXT_READY&&
        next->buffered&&next->rate==s->rate&&
        next->dsp.config.headroom_db_tenths==s->dsp.config.headroom_db_tenths;
}
static bool join_ready(struct player_slot *s,const struct ln_audio_status *audio){
    if(!next_joinable(s))return false;
    struct player_slot *next=&slots[audible^1u];if(audio->free_frames<next->buffered)return false;
    join_boundary=submitted;join_gapless=audio->queued>0;join_underruns=audio->underruns;
    feed=audible^1u;join_pending=true;state.next_phase=LN_PLAYER_NEXT_COMMITTED;
    (void)submit_slot(next);
    /* A fully decoded short next track can end in the same DMA block as the
     * old tail. Its final marker cannot wait for audible promotion. */
    if(next->phase==SLOT_EOF&&!next->buffered){ln_audio_finish();finish_sent=true;}
    return true;
}
static void prepare_eof_start(struct player_slot *s,const struct ln_audio_status *audio){
    if(s->phase!=SLOT_EOF||s->buffered||finish_sent)return;
    if(join_pending){ln_audio_finish();finish_sent=true;return;}
    /* A whole short track may finish during the sink's blocking startup.
     * Append an already prepared join, or fence EOF before starting DMA. */
    if(join_ready(s,audio)||next_joinable(s))return;
    ln_audio_finish();finish_sent=true;
}
static bool close_eof(struct player_slot *s,const struct ln_audio_status *audio){
    if(state.phase!=LN_PLAYER_PLAY||s->phase!=SLOT_EOF||s->buffered||finish_sent||
       audio->queued>startup_frames(audio->rate)||next_joinable(s))return false;
    /* Pending metadata/read owners must not postpone the final DMA marker
     * until the ring is empty. Late next tracks use the normal drained join. */
    ln_audio_finish();finish_sent=true;state.phase=LN_PLAYER_DRAIN;return true;
}
static void refresh(const struct ln_audio_status *audio){
    consumed+=(uint32_t)(audio->consumed-last_consumed);last_consumed=audio->consumed;state.underruns=audio->underruns;
    if(join_pending&&consumed>join_boundary){
        uint32_t old=audible;audible=feed;audible_origin=join_boundary;
        bool clean=join_gapless&&audio->underruns==join_underruns;
        transition=(struct ln_player_transition){.token=slots[audible].token,.gapless=clean,.boundary=join_boundary,.underruns=audio->underruns};
        event_pending=true;join_pending=false;have_next=false;state.next_phase=LN_PLAYER_NEXT_NONE;state.next_error=0;
        if(clean)state.gapless_joins++;else state.missed_joins++;clear_slot(&slots[old]);
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
        activate_cache(&slots[audible]);
#endif
    }
    struct player_slot *s=&slots[audible];state.rate=s->rate;state.frames=s->decoder.total_frames;state.decoded=(uint32_t)s->submitted;
    uint64_t rendered=consumed-audible_origin;if(rendered>s->submitted)rendered=s->submitted;state.rendered=(uint32_t)rendered;
}
bool ln_player_open(const char *name){return ln_player_open_at(name,0,0);}
bool ln_player_open_at(const char *name,uint64_t bytes,uint64_t position){
    return ln_player_open_configured(name,bytes,position,0,0);
}
static bool prepare_dsp(struct ln_dsp *dsp,const struct ln_dsp_config *config,uint32_t rate){
    ln_dsp_init(dsp);return !config||((!config->headroom_db_tenths||gain_sync)&&
        supported_rate(rate)&&ln_dsp_configure(dsp,config));
}
static uint32_t effect_start(const struct ln_audio_status *audio){
    /* Leave two DMA blocks ahead of the foreground copy. Recheck on every
     * step: input, display and storage work can consume this reserve. */
    uint32_t skip=audio->playing?2048u:0u;if(skip>audio->queued)skip=audio->queued;
    return audio->consumed+skip;
}
bool ln_player_configure(const struct ln_dsp_config *config,uint32_t rate){
    if(stop_requested||skip_requested||join_pending||event_pending)return false;
    if(state.phase==LN_PLAYER_IDLE||state.phase==LN_PLAYER_ERROR)return true;
    struct player_slot *s=&slots[audible];struct ln_dsp candidate;
    if(!config||!prepare_dsp(&candidate,config,rate)||(s->rate&&s->rate!=rate))return false;
    if(audio_ready){
        /* A previous lower-gain write may still be unacknowledged. A newer
         * edit must not mistake its conservative target for actual codec gain. */
        if(gain_waiting&&!synchronize_gain())return false;
        /* Lower gain before admitting PCM with less automatic attenuation.
         * A failed codec write leaves the old DSP in place for a later retry. */
        headroom_pending=false;
        if(config->headroom_db_tenths<=output_headroom){
            output_headroom=config->headroom_db_tenths;
            if(!synchronize_gain())return false;
        }else{
            pending_headroom=config->headroom_db_tenths;
            headroom_boundary=raw_head;headroom_pending=true;
        }
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        effect_cursor=effect_start(&audio);effect_pending=effect_cursor!=raw_head;
        effect_paused_full=!audio.playing&&!audio.fault;
    }
    s->dsp=candidate;s->config_rate=rate;return true;
}
bool ln_player_open_configured(const char *name,uint64_t bytes,uint64_t position,
                               const struct ln_dsp_config *config,uint32_t config_rate){
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    if(video_firmware_owned)return false;
#endif
    if(state.phase!=LN_PLAYER_IDLE||ln_storage_busy()||!ln_storage_mounted())return false;
#if !defined(LINNET_PROFILE_RELEASE)
    if(ln_development_stage_busy())return false;
#endif
    struct ln_dsp dsp;if(!prepare_dsp(&dsp,config,config_rate))return false;
    file_configure=0;
    if(!start_slot(&slots[0],name,bytes,position,0,&dsp,config?config_rate:0))return false;
#if !defined(LINNET_PROFILE_RELEASE)
    timing=(struct ln_player_timing){0};
#endif
    audible=feed=0;state=(struct ln_player_status){.phase=LN_PLAYER_OPEN,.generation=slots[0].generation};
    io_owner=0;stop_requested=seek_requested=pause_requested=have_next=cancel_next=false;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    activate_cache(&slots[0]);
#endif
    skip_requested=seek_paused=false;
    effect_pending=effect_paused_full=headroom_pending=gain_waiting=pause_io_canceling=audio_ready=join_pending=event_pending=finish_sent=next_expected=eof_waiting=false;
    output_headroom=0;consumed=submitted=audible_origin=0;last_consumed=0;return true;
}
bool ln_player_open_file(const struct ln_fat32_file *file,bool (*configure)(uint32_t,struct ln_dsp_config *)){
    if(!file||(file->attributes&24u)||!file->size||!ln_player_open("/FILE"))return false;
    struct player_slot*s=&slots[0];s->media=*file;file_configure=configure;
    DECODER_WORKSPACE_CHANGED();
    ln_decoder_init(&s->decoder,ln_decoder_workspace_at(0),read_media,s,file->size);s->phase=SLOT_DECODE;
    return true;
}
bool ln_player_prepare_next_configured(const char *name,uint64_t bytes,uint32_t token,
                                      const struct ln_dsp_config *config,uint32_t config_rate){
    if((state.phase!=LN_PLAYER_PRIME&&state.phase!=LN_PLAYER_PLAY&&state.phase!=LN_PLAYER_PAUSE&&state.phase!=LN_PLAYER_DRAIN)||join_pending||event_pending||cancel_next||skip_requested)return false;
    if(have_next&&state.next_phase!=LN_PLAYER_NEXT_FAILED)return false;
    struct player_slot *s=&slots[audible^1u];if(io_owner==s)return false;
    struct ln_dsp dsp;if(!prepare_dsp(&dsp,config,config_rate))return false;
    char checked_name[LN_FAT32_PATH_MAX+1];if(!name_copy(checked_name,name))return false;
    clear_slot(s);if(!start_slot(s,checked_name,bytes,0,token,&dsp,config?config_rate:0))return false;
    have_next=true;state.next_phase=LN_PLAYER_NEXT_OPENING;state.next_error=0;return true;
}
bool ln_player_cancel_next(void){
    if(event_pending)return false;
    if(skip_requested){ln_player_stop_for_restart();return true;}
    if(join_pending||state.next_phase==LN_PLAYER_NEXT_COMMITTED)return false;
    next_expected=eof_waiting=false;if(!have_next)return true;
    struct player_slot *s=&slots[audible^1u];
    if(io_owner==s){cancel_next=true;state.next_phase=LN_PLAYER_NEXT_CANCELING;ln_storage_media_cancel(s->generation);}
    else{clear_slot(s);have_next=false;state.next_phase=LN_PLAYER_NEXT_NONE;state.next_error=0;}return true;
}
bool ln_player_skip_prepared_next(uint32_t token){
    struct player_slot *next=&slots[audible^1u];
    if(!token||!have_next||state.next_phase!=LN_PLAYER_NEXT_READY||next->token!=token||
       !next->buffered||!next->rate||next->submitted||io_owner==next||
       join_pending||event_pending||interrupted()||pause_requested||
       (state.phase!=LN_PLAYER_PLAY&&state.phase!=LN_PLAYER_PAUSE&&state.phase!=LN_PLAYER_DRAIN))return false;
    skip_requested=true;next_expected=eof_waiting=false;
    if(io_owner)ln_storage_media_cancel(io_owner->generation);
    return true;
}
bool ln_player_take_transition(struct ln_player_transition *out){if(!out||!event_pending)return false;*out=transition;event_pending=false;return true;}
static void stop_player(bool restarting){
    seek_paused=next_expected=eof_waiting=false;
    if(state.phase==LN_PLAYER_IDLE){if(!restarting)(void)ln_audio_stop();return;}
    state.error=0;stop_requested=true;state.phase=LN_PLAYER_STOPPING;
    if(!(restarting?ln_audio_halt():ln_audio_stop()))state.error=20;
    if(io_owner)ln_storage_media_cancel(io_owner->generation);
}
void ln_player_stop(void){stop_player(false);}
void ln_player_stop_for_restart(void){stop_player(true);}
bool ln_player_pause(bool pause){
    if(skip_requested)return false;
    /* A seek owns a halted ring until cancellation drains and priming ends.
     * Remember the latest intent; start DMA only after fresh PCM is ready. */
    if(!stop_requested&&(seek_requested||state.phase==LN_PLAYER_PRIME)){
        seek_paused=pause;pause_requested=false;return true;
    }
    if(pause&&state.phase==LN_PLAYER_PLAY){pause_requested=true;eof_waiting=false;return true;}
    if(!pause&&state.phase==LN_PLAYER_PLAY&&pause_requested){pause_requested=false;return true;}
    if(!pause&&state.phase==LN_PLAYER_PAUSE){
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        /* The fade can consume the final queued block. Refill through PRIME
         * rather than asking the sink to start an empty ring. */
        if(!audio.queued){
            seek_paused=false;
            state.phase=slots[feed].phase==SLOT_EOF?LN_PLAYER_DRAIN:LN_PLAYER_PRIME;
            return true;
        }
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
        /* Preserve muted PCM and position while storage wakes. */
        if(ln_storage_music_disk_ready()!=1){seek_paused=false;state.phase=LN_PLAYER_PRIME;return true;}
#endif
        if(!synchronize_gain()){
            seek_paused=false;state.phase=LN_PLAYER_PRIME;return true;
        }
        prepare_eof_start(&slots[feed],&audio);
        if(!ln_audio_start())return false;eof_waiting=false;state.phase=finish_sent?LN_PLAYER_DRAIN:LN_PLAYER_PLAY;return true;
    }return false;
}
bool ln_player_toggle_pause(void){
    if(stop_requested||skip_requested)return false;
    if(seek_requested||state.phase==LN_PLAYER_PRIME)return ln_player_pause(!seek_paused);
    if(state.phase==LN_PLAYER_PLAY)return ln_player_pause(!pause_requested);
    if(state.phase==LN_PLAYER_PAUSE)return ln_player_pause(false);
    return false;
}
bool ln_player_seek(uint64_t frame){
    if(skip_requested||event_pending)return false;
    uint32_t phase=state.phase;
    if(phase-LN_PLAYER_PRIME>LN_PLAYER_DRAIN-LN_PLAYER_PRIME)return false;
    bool priming=phase==LN_PLAYER_PRIME;
    if(state.frames&&frame>state.frames)return false;
    if(!seek_requested&&!priming){
        if(!ln_audio_halt()){fail(20);return false;}
        seek_paused=phase==LN_PLAYER_PAUSE||pause_requested;
    }
    seek_target=frame;seek_requested=true;next_expected=eof_waiting=false;if(io_owner)ln_storage_media_cancel(io_owner->generation);return true;
}
static bool drain_canceled(void){
    if(!io_owner)return true;int r=ln_storage_media_poll(io_owner->generation,0,0);if(!r)return false;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    io_owner->cache_demand_pending=false;
#endif
    ln_decoder_read_drained(&io_owner->decoder);io_owner=0;return true;
}
static void player_advance(void){
    if(state.phase==LN_PLAYER_IDLE)return;
    if(stop_requested){
        if(!drain_canceled())return;clear_slot(&slots[0]);clear_slot(&slots[1]);
        if(state.error)(void)ln_audio_stop();
        state.phase=state.error?LN_PLAYER_ERROR:LN_PLAYER_IDLE;stop_requested=seek_requested=pause_requested=false;
        effect_pending=effect_paused_full=headroom_pending=gain_waiting=pause_io_canceling=have_next=cancel_next=join_pending=event_pending=audio_ready=skip_requested=seek_paused=false;
        output_headroom=0;state.next_phase=LN_PLAYER_NEXT_NONE;return;
    }
    if(skip_requested){
        if(!drain_canceled())return;
        if(stop_requested)return;
        struct player_slot *next=&slots[audible^1u];
        if(!have_next||state.next_phase!=LN_PLAYER_NEXT_READY||!next->buffered||next->submitted){fail(22);return;}
        if(!ln_audio_halt()){fail(20);return;}
        audio_ready=false;clear_slot(&slots[audible]);audible^=1u;feed=audible;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
        activate_cache(next);
#endif
        next->generation=generation();next->submitted=0;next->position_base=0;
        consumed=submitted=audible_origin=0;last_consumed=0;
        join_pending=join_gapless=finish_sent=pause_requested=seek_paused=false;
        if(!initialize_audio(next->rate)){fail(11);return;}
        state.phase=LN_PLAYER_PRIME;state.generation=next->generation;state.rate=next->rate;
        state.frames=next->decoder.total_frames;state.decoded=state.rendered=state.underruns=state.error=0;
        transition=(struct ln_player_transition){.token=next->token,.manual=true,.gapless=false,.boundary=0,.underruns=0};
        event_pending=true;skip_requested=have_next=false;
        state.next_phase=LN_PLAYER_NEXT_NONE;state.next_error=0;return;
    }
    if(seek_requested){
        if(!drain_canceled())return;
        if(stop_requested)return;
        clear_slot(&slots[audible^1u]);have_next=cancel_next=join_pending=event_pending=false;state.next_phase=LN_PLAYER_NEXT_NONE;
        struct player_slot *s=&slots[audible];feed=audible;ln_decoder_read_drained(&s->decoder);
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
        s->cache_demand_pending=false;s->cache_initial=true;s->cache_current=1u;
        for(uint32_t i=0;i<2u;++i)s->cache[i]=(struct player_cache_window){0};
#endif
        if(!ln_decoder_seek(&s->decoder,seek_target)||!initialize_audio(s->rate)){fail(21);return;}
        s->phase=SLOT_DECODE;s->buffered=0;s->submitted=0;s->position_base=seek_target;audible_origin=consumed;
        state.decoded=state.rendered=0;state.phase=LN_PLAYER_PRIME;seek_requested=pause_requested=false;
        (void)ln_dsp_configure(&s->dsp,&s->dsp.config);return;
    }
    if(pause_io_canceling){
        if(!drain_canceled())return;
        pause_io_canceling=false;
    }
    if(cancel_next){
        if(!drain_canceled())return;clear_slot(&slots[audible^1u]);have_next=cancel_next=false;
        state.next_phase=LN_PLAYER_NEXT_NONE;state.next_error=0;
        if(stop_requested||seek_requested)return;
    }
    if(state.phase==LN_PLAYER_ERROR)return;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    if(io_owner&&io_owner->phase==SLOT_EOF){
        struct player_slot *finished=io_owner;
        ln_storage_media_cancel(finished->generation);
        if(!drain_canceled())return;
        finished->cache[0].filling=finished->cache[1].filling=false;
    }
#endif
    struct ln_audio_status audio={0};
    if(audio_ready){ln_audio_snapshot(&audio);refresh(&audio);if(audio.fault){fail(12);return;}}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    if(slots[audible].cache_pending&&io_owner!=&slots[audible])activate_cache(&slots[audible]);
    struct player_slot *paused=&slots[audible];
    if((pause_requested||state.phase==LN_PLAYER_PAUSE)&&io_owner==paused&&paused->cache_enabled)
        paused->cache_pause_canceling=true;
    if(paused->cache_pause_canceling){
        if(io_owner==paused){
            ln_storage_media_cancel(io_owner->generation);
            if(!drain_canceled())return;
        }
        paused->cache_pause_canceling=false;
    }
#endif
    if(state.phase==LN_PLAYER_PAUSE)return;
    if(pause_requested&&!io_owner){pause_requested=false;ln_audio_pause();state.phase=LN_PLAYER_PAUSE;return;}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
    struct player_slot *cached=&slots[audible];
    if(!pause_requested&&cached->cache_enabled&&cached->phase!=SLOT_EOF&&
       (cached->cache[0].filling||cached->cache[1].filling)&&cache_progress(cached)<0){
        fail(13);return;
    }
#endif
    if(state.phase==LN_PLAYER_DRAIN){
        if(have_next&&state.next_phase==LN_PLAYER_NEXT_OPENING)advance_next();if(interrupted())return;
        if(audio.queued||ln_time_us()-audio.last_irq_us<2000u)return;
        bool restart=have_next&&state.next_phase!=LN_PLAYER_NEXT_FAILED;
        if(!(restart?ln_audio_halt():ln_audio_stop())){fail(20);return;}audio_ready=false;
        if(have_next&&state.next_phase!=LN_PLAYER_NEXT_FAILED){
            if(state.next_phase!=LN_PLAYER_NEXT_READY)return;
            feed=audible^1u;join_boundary=consumed;join_gapless=false;join_underruns=0;
            join_pending=true;state.next_phase=LN_PLAYER_NEXT_COMMITTED;
            if(!initialize_audio(slots[feed].rate)){fail(11);return;}state.phase=LN_PLAYER_PRIME;
        }else{clear_slot(&slots[audible]);if(have_next)clear_slot(&slots[audible^1u]);have_next=false;state.phase=LN_PLAYER_IDLE;return;}
    }
    struct player_slot *s=&slots[feed];
    if(!audio_ready){
        int r=slot_step(s);if(interrupted())return;if(r<0){fail(3u+(uint32_t)-r);return;}
        if(!s->rate)return;if(!initialize_audio(s->rate)){fail(11);return;}
        state.rate=s->rate;state.frames=s->decoder.total_frames;state.phase=LN_PLAYER_PRIME;
    }
    ln_audio_snapshot(&audio);
    if(close_eof(s,&audio))return;
    /* Current decoding has priority below the read runway. An already admitted
     * exact read must drain, but its decoder cannot chain another low-PCM read. */
    if(have_next&&!join_pending&&state.next_phase==LN_PLAYER_NEXT_OPENING&&
       (io_owner==&slots[audible^1u]||(!io_owner&&s->phase!=SLOT_EOF&&state.phase==LN_PLAYER_PLAY&&
        audio.queued>=prefetch_frames(audio.rate)))){
        advance_next();if(interrupted())return;
        ln_audio_snapshot(&audio);
        /* Poll the admitted background request, then use otherwise idle CPU
         * to replenish low PCM from the current decoder's cached input.
         * read_media still prevents it from borrowing the pending owner. */
        if(s->phase==SLOT_EOF||audio.queued>=prefetch_frames(audio.rate))return;
    }
    if(s->buffered){if(!submit_slot(s))return;}
    else if(s->phase!=SLOT_EOF&&audio.free_frames>=PLAYER_DECODE_FRAMES){
        int r=slot_step(s);if(interrupted())return;if(r<0){fail(13);return;}if(s->buffered&&!submit_slot(s))return;
    }
    if(state.phase==LN_PLAYER_PRIME){
        ln_audio_snapshot(&audio);
        uint32_t prime_frames=startup_frames(s->rate);
#if defined(LINNET_SOC_PP5022)
        /* A drained rate change needs more runway for its first cold disk read. */
        if(join_pending&&!join_gapless&&s->rate!=slots[audible].rate)
            prime_frames=prefetch_frames(s->rate);
#endif
        if(audio.queued>=prime_frames||s->phase==SLOT_EOF){
            if(!audio.queued){stop_player(false);return;}
            /* A paused seek fills a fresh ring while muted. Resume starts DMA;
             * priming must not briefly expose audio before returning to pause. */
            if(seek_paused){seek_paused=false;state.phase=LN_PLAYER_PAUSE;return;}
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
            if(ln_player_startup_disk()){
                int ready=ln_storage_music_disk_ready();
                if(ready<0){fail(23);return;}if(!ready)return;
            }
#endif
            if(!synchronize_gain())return;
            prepare_eof_start(s,&audio);
            if(!ln_audio_start()){fail(15);return;}state.phase=finish_sent?LN_PLAYER_DRAIN:LN_PLAYER_PLAY;
            if(finish_sent||join_pending)return;
        }
    }
    if(s->phase!=SLOT_EOF){eof_waiting=false;return;}
    ln_audio_snapshot(&audio);
    if(close_eof(s,&audio))return;
    if(!eof_waiting){
        /* Ring capacity bounds scaled to 65,536,000; supported rates bound
         * the remainder product to 95,999,000. Preserve exact microseconds
         * without a 64-bit division runtime on this freestanding target. */
        uint32_t scaled=audio.queued*1000u;
        uint32_t runway=audio.rate?(scaled/audio.rate)*1000u+
            ((scaled%audio.rate)*1000u)/audio.rate:0;
        if(runway>8000000u)runway=8000000u;
        eof_deadline=ln_time_us()+runway;eof_waiting=true;
    }
    bool runway=audio.queued&&(int32_t)(ln_time_us()-eof_deadline)<0;
    if(have_next&&!join_pending&&state.next_phase==LN_PLAYER_NEXT_OPENING){
        if(io_owner==&slots[audible^1u]||(!io_owner&&state.phase==LN_PLAYER_PLAY&&runway)){advance_next();return;}
    }
    if(next_joinable(s)){ln_audio_snapshot(&audio);(void)join_ready(s,&audio);return;}
    /* Metadata/history may still be arriving for a track shorter than the
     * usual runway. Waiting is bounded by audio already queued, never silence. */
    if(next_expected&&!have_next&&!join_pending&&runway)return;
    if(!finish_sent){ln_audio_finish();finish_sent=true;}state.phase=LN_PLAYER_DRAIN;
}
static bool effect_yield_to_decode(const struct ln_audio_status *audio){
    uint32_t reserve=ln_player_background_frames(audio->rate);
    if(!audio->playing||audio->queued>=reserve)return false;
    /* Replacing queued PCM must not consume the decoder's remaining runway.
     * The untouched tail keeps its old effects until it plays; start fresh
     * histories for newly decoded PCM, and retain its gain boundary. */
    effect_pending=effect_paused_full=false;effect_cursor=raw_head;
    (void)ln_dsp_configure(&slots[audible].dsp,&slots[audible].dsp.config);
    if(!headroom_pending){
        pending_headroom=output_headroom;headroom_boundary=raw_head;
        headroom_pending=audio->queued!=0;
    }
    return true;
}
void ln_player_step(void){
    if(stepping)return;stepping=true;
    /* A pending media read must not delay muting or consume the queued pause
     * position. Its cancellation/completion can drain while audio is paused. */
    if(pause_requested&&audio_ready&&state.phase==LN_PLAYER_PLAY&&
       !stop_requested&&!seek_requested&&!skip_requested){
        pause_requested=false;ln_audio_pause();state.phase=LN_PLAYER_PAUSE;
        if(io_owner){
            pause_io_canceling=true;ln_storage_media_cancel(io_owner->generation);
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS)
            if(io_owner==&slots[audible]&&io_owner->cache_enabled)io_owner->cache_pause_canceling=true;
#endif
        }
    }
    /* A low-runway edit may leave an old tail awaiting playback. A settled
     * pause can safely replace all of it, including the former DMA prefix. */
    if(!effect_pending&&headroom_pending&&!join_pending&&!event_pending&&state.phase==LN_PLAYER_PAUSE&&
       !stop_requested&&!seek_requested&&!skip_requested){
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        if(!audio.playing&&!audio.fault){
            effect_cursor=audio.consumed;effect_pending=effect_cursor!=raw_head;
            effect_paused_full=true;
            (void)ln_dsp_configure(&slots[audible].dsp,&slots[audible].dsp.config);
            if(!effect_pending){output_headroom=pending_headroom;headroom_pending=false;}
        }
    }
    if(effect_pending&&!stop_requested&&!seek_requested&&!skip_requested&&!pause_requested){
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        if(effect_yield_to_decode(&audio)){
            player_advance();stepping=false;return;
        }
        /* A job begun during playback skipped a DMA prefix. If Pause settles,
         * restart from the stopped cursor before permitting early gain makeup. */
        if(!audio.playing&&!audio.fault&&!effect_paused_full){
            effect_cursor=audio.consumed;effect_paused_full=true;
            (void)ln_dsp_configure(&slots[audible].dsp,&slots[audible].dsp.config);
        }
        if(audio.playing)effect_paused_full=false;
        uint32_t first=effect_start(&audio);
        if((int32_t)(first-effect_cursor)>0){
            effect_cursor=first;
            (void)ln_dsp_configure(&slots[audible].dsp,&slots[audible].dsp.config);
        }
        uint32_t count=raw_head-effect_cursor;if(count>PLAYER_DECODE_FRAMES)count=PLAYER_DECODE_FRAMES;
        for(uint32_t i=0;i<count;i++){
            uint32_t at=(effect_cursor+i)&(LINNET_AUDIO_RING_FRAMES-1u);
            __builtin_memcpy(EFFECT_PCM+2*i,RAW_PCM+2*at,4);
        }
        process_pcm(&slots[audible],EFFECT_PCM,count);
        bool rewritten=ln_audio_rewrite(effect_cursor,EFFECT_PCM,count);
        effect_cursor+=count;
        if(!rewritten){
            effect_paused_full=false;
            /* DMA may advance during DSP. Its owned PCM cannot be replaced;
             * resume at writable audio, without processing an obsolete backlog. */
            ln_audio_snapshot(&audio);first=effect_start(&audio);
            if((int32_t)(first-effect_cursor)>0){
                effect_cursor=first;
                (void)ln_dsp_configure(&slots[audible].dsp,&slots[audible].dsp.config);
            }
        }
        effect_pending=effect_cursor!=raw_head;
        if(!effect_pending&&effect_paused_full&&headroom_pending){
            ln_audio_snapshot(&audio);
            if(!audio.playing&&!audio.fault){
                output_headroom=pending_headroom;headroom_pending=false;
            }
        }
        if(effect_pending){
            ln_audio_snapshot(&audio);
            if(effect_yield_to_decode(&audio))player_advance();
        }
    }else player_advance();
    stepping=false;
}
bool ln_player_busy(void){return state.phase!=LN_PLAYER_IDLE&&state.phase!=LN_PLAYER_ERROR;}
uint32_t ln_player_deadline(uint32_t now){
    if(gain_waiting&&!stop_requested&&!seek_requested&&!skip_requested)return now+1000u;
    if(effect_pending)return now;
    /* An admitted read for the feeding decoder owns its input buffer until
     * storage completes. Another player turn cannot produce PCM from it.
     * Storage keeps runnable FAT/completion work awake and owns the bounded
     * disk wait. A next-track read still permits the current decoder to use
     * its cached input, so do not defer that independent work. */
    if(io_owner&&(io_owner==&slots[feed]||interrupted()))return now+200000u;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    /* Full muted PCM cannot advance until storage's power transition completes. */
    if(state.phase==LN_PLAYER_PRIME&&!interrupted()&&!pause_requested&&ln_player_startup_disk()&&ln_audio_capacity()<PLAYER_DECODE_FRAMES&&
       ln_storage_music_disk_ready()==0)return now+200000u;
#endif
    if(state.phase==LN_PLAYER_OPEN||state.phase==LN_PLAYER_HEADER||state.phase==LN_PLAYER_PRIME||io_owner)return now;
    /* Decode and drain are separate bounded steps. Sleeping between them
     * caps MP3 production below44.1kHz even before accounting for compute. */
    if(stop_requested||seek_requested||cancel_next||pause_requested||skip_requested||
       (state.phase==LN_PLAYER_PLAY&&ln_audio_capacity()>=PLAYER_DECODE_FRAMES))return now;
    /* A settled pause has no decode work. Input IRQs and other service
     * deadlines still wake the executive; preserve the watchdog idle ceiling. */
    if(state.phase==LN_PLAYER_PAUSE)return now+200000u;
    return ln_player_busy()?now+10000u:now+200000u;
}
void ln_player_snapshot(struct ln_player_status *out){if(out)*out=state;}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
/* Requested snapshots only: no playback hooks, counters or cache ownership changes. */
void ln_player_cache_diagnostics(uint32_t words[32]){
    const struct player_slot *s=&slots[feed];
    for(uint32_t i=0;i<32u;++i)words[i]=0;
    words[0]=0x48434350u;words[1]=1u;words[2]=ln_time_us();words[3]=state.phase;
    words[4]=audible|(feed<<8)|((io_owner==&slots[0]?1u:io_owner==&slots[1]?2u:0u)<<16)|((uint32_t)io_open<<24);
    words[5]=(uint32_t)s->cache_enabled|((uint32_t)s->cache_pending<<1)|((uint32_t)s->cache_initial<<2)|
        ((uint32_t)s->cache_codec_checked<<3)|((uint32_t)s->cache_demand_pending<<4)|((uint32_t)s->cache_pause_canceling<<5)|
        ((uint32_t)(s->tables[0].target&&s->tables[0].valid==s->tables[0].target)<<6)|
        ((uint32_t)(s->tables[1].target&&s->tables[1].valid==s->tables[1].target)<<7)|(s->cache_current<<8);
    words[6]=(uint32_t)(uintptr_t)io_buffer;words[7]=io_offset;words[8]=io_length;words[9]=s->generation;
    words[14]=s->media.size;words[15]=s->buffered;
    for(uint32_t i=0;i<2u;++i){
        words[16u+i*4u]=s->cache[i].start;words[17u+i*4u]=s->cache[i].valid;
        words[18u+i*4u]=s->cache[i].target;words[19u+i*4u]=s->cache[i].filling;
    }
    words[27]=(s->decoder.kind<<16)|(s->phase<<24);
    struct ln_decoder_cache_layout layout;struct ln_decoder_cache_status status;
    if(ln_decoder_cache_layout(&s->decoder,&layout)&&ln_decoder_cache_status(&s->decoder,&status)){
        words[10]=(uint32_t)layout.media_start;words[11]=(uint32_t)(layout.media_start>>32);
        words[12]=(uint32_t)layout.media_end;words[13]=(uint32_t)(layout.media_end>>32);
        words[24]=(uint32_t)status.request_offset;words[25]=(uint32_t)(status.request_offset>>32);
        words[26]=status.request_bytes;words[27]|=(uint32_t)status.pending|((status.phase&15u)<<4)|(status.bank<<8);
        words[28]=(uint32_t)status.read_offset;words[29]=(uint32_t)(status.read_offset>>32);words[30]=status.read_bytes;words[31]=status.packet;
    }
}
#endif
uint64_t ln_player_position(void){
    uint64_t rendered=consumed-audible_origin;if(rendered>slots[audible].submitted)rendered=slots[audible].submitted;
    return slots[audible].position_base+rendered;
}
