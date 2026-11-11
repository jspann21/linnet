#include "services/video/video.h"
#include "services/video/catalog.h"
#include "services/video/mp4.h"
#include "services/video/native/decoder.h"
#include "services/video/native/yuv.h"
#include "services/audio/aac/aac.h"
#include "services/storage/storage.h"
#include "hal/api/audio.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_bytes.h"
#define AAC (*(struct ln_aac *)(uintptr_t)LINNET_STAGE_ADDRESS)
#define PACKET ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x40000u))
#define CACHE_SLOTS 64u
/* Target policy sets the priming interval; audio_step keeps filling the ring. */
#define START_AUDIO_FRAMES LINNET_VIDEO_START_AUDIO_FRAMES
_Static_assert(0x40000u+LINNET_VIDEO_PACKET_BYTES+CACHE_SLOTS*16384u<=0x200000u,"Video packet/cache overlaps decoder arena");
#define CACHE(slot) ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x40000u+LINNET_VIDEO_PACKET_BYTES+(slot)*16384u))
extern void ln_memory_copy(void *,const void *,uint32_t);
#ifndef LINNET_VIDEO_CLOCK_HZ
#define LINNET_VIDEO_CLOCK_HZ LINNET_ACTIVE_CLOCK_HZ
#endif
enum {IDLE,OPEN,OPEN_WAIT,VIDEO_HEADER,AUDIO_HEADER,PRIME,PLAY,STOPPING,FAILED};
static struct {
    uint32_t phase,error,generation,revision,front,back,presented,decoded,max_decode,late,max_late;
    uint32_t ready,frame_due;
    uint32_t video_phase,audio_phase,audio_frames,paused_at,position,duration,identity,resume[2];
    uint32_t seek_phase,seek_target,base,tail_position,wall_at,wall_ms,wall_us,audio_at;
    uint64_t audio_skip,audio_elapsed;
    uint32_t audio_us,io_started,io_us,dropped,last_picture;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t opened_us,first_present_us,index_ready_us,audio_start_us,start_ready_frames;
#endif
    uint32_t io_kind,io_bytes,io_requested,io_at,read_bytes,reads;uint64_t io_offset;void *io_buffer;
    bool active,io,reading,stepping,paused,started,audio,audio_eof,video_eof,offered,frame_ready,stop,completed;
    bool tail,headphone_known,headphones,boosted;
    uint8_t options;
    char path[256],title[128];
    struct ln_fat32_file file;
    struct ln_mp4 movie,sound;
    struct ln_mp4_packet video_packet,audio_packet;
    struct {uint64_t offset[CACHE_SLOTS];uint32_t bytes[CACHE_SLOTS],age[CACHE_SLOTS],clock,filling,owner;bool pending;} cache;
    struct ln_ui_asset asset;
} v;
/* The renderer sees a slot only after its decoded pixels are complete. */
static uint16_t pixels[3][LINNET_VIDEO_WIDTH*LINNET_VIDEO_HEIGHT]
    __attribute__((section(".media_workspace"),aligned(32)));
static uint32_t milliseconds(uint64_t ticks,uint32_t rate){
    uint64_t quotient=0,remainder=0;if(!rate)return 0;
    /* Scale before division: the fractional product can exceed 32 bits. */
    if(ticks>UINT64_MAX/1000u)return UINT32_MAX;
    ticks*=1000u;
    for(unsigned bit=64;bit;bit--){remainder=(remainder<<1)|((ticks>>(bit-1))&1u);if(remainder>=rate){remainder-=rate;quotient|=1ull<<(bit-1);}}
    return quotient>UINT32_MAX?UINT32_MAX:(uint32_t)quotient;
}
static void wall_reset(uint32_t now){v.wall_at=now;v.wall_ms=v.wall_us=0;}
static uint32_t wall_position(uint32_t now,uint32_t base){
    /* The hardware microsecond clock wraps every 71 minutes. Fold each
     * bounded delta into playback time instead of subtracting from start. */
    uint32_t delta=now-v.wall_at;v.wall_at=now;
    uint32_t fraction=v.wall_us+delta%1000u;
    uint64_t ms=(uint64_t)v.wall_ms+delta/1000u+fraction/1000u;
    v.wall_ms=ms>UINT32_MAX?UINT32_MAX:(uint32_t)ms;
    v.wall_us=fraction%1000u;
    uint64_t position=(uint64_t)base+v.wall_ms;
    return position>UINT32_MAX?UINT32_MAX:(uint32_t)position;
}
static uint32_t clock_raw_ms(void){
    if(!v.started)return v.base;
    uint32_t now=v.paused?v.paused_at:ln_time_us();
    if(v.audio){
        struct ln_audio_status a;ln_audio_snapshot(&a);
        uint32_t frames=a.consumed+a.inflight_consumed;
        /* DMA progress is an estimate and can retreat slightly at a block
         * boundary. A signed delta accepts counter wrap but ignores retreat. */
        int32_t advance=(int32_t)(frames-v.audio_at);
        if(advance>0){v.audio_elapsed+=(uint32_t)advance;v.audio_at=frames;}
        uint64_t audio_position=(uint64_t)v.base+milliseconds(v.audio_elapsed,a.rate);
        uint32_t position=audio_position>UINT32_MAX?UINT32_MAX:(uint32_t)audio_position;
        /* Audio and video tracks need not end on the same timestamp. */
        if(!v.audio_eof||a.queued)return position;
        if(!v.tail){v.tail=true;wall_reset(now);v.tail_position=position;}
        return wall_position(now,v.tail_position);
    }
    return wall_position(now,v.base);
}
static uint32_t clock_ms(void){uint32_t ms=clock_raw_ms();return ms>v.duration?v.duration:ms;}
static uint64_t ticks(uint32_t ms,uint32_t rate){
    uint64_t value=(uint64_t)ms*rate,result=0;uint32_t remainder=0;
    for(unsigned bit=64;bit;--bit){remainder=(remainder<<1)|((value>>(bit-1))&1u);if(remainder>=1000u){remainder-=1000u;result|=1ull<<(bit-1);}}
    return result;
}
static void fail(uint32_t reason){v.error=reason;v.stop=true;++v.revision;}
static int media_read(void *context,uint64_t offset,void *buffer,uint32_t bytes){
    uint32_t kind=(uint32_t)(uintptr_t)context;
    if(v.reading&&kind!=v.io_kind)return 0;
    if(!v.reading){v.io_kind=kind;v.io_offset=offset;v.io_buffer=buffer;v.io_bytes=bytes;v.io_at=0;v.reading=true;v.io_started=ln_time_us();}
    if(offset!=v.io_offset||buffer!=v.io_buffer||bytes!=v.io_bytes||offset>0xffffffffu)return -1;
    if(v.io){uint32_t transferred=0;int r=ln_storage_media_poll_deferred(v.generation,0,&transferred);
        if(!r)return 0;v.io=false;
        if(r<0||transferred!=v.io_requested){v.reading=false;return -1;}
        v.io_at+=transferred;v.read_bytes+=transferred;++v.reads;
        if(v.io_at==bytes){v.reading=false;v.io_us+=ln_time_us()-v.io_started;return 1;}
    }
    uint32_t count=bytes-v.io_at;if(count>16384)count=16384;
    int r=ln_storage_media_read(&v.file,(uint32_t)offset+v.io_at,(uint8_t *)buffer+v.io_at,count,v.generation);
    if(r<0){v.reading=false;return -1;}if(r>0){v.io=true;v.io_requested=count;}return 0;
}
static int cached_read(void *context,uint64_t offset,void *buffer,uint32_t bytes){
    uint32_t kind=(uint32_t)(uintptr_t)context;if(kind>1)return -1;
    if(bytes>16384)return media_read(context,offset,buffer,bytes);
    __typeof__(v.cache) *c=&v.cache;
    if(c->pending&&c->owner==kind){uint32_t slot=c->filling;int r=media_read(context,c->offset[slot],CACHE(slot),c->bytes[slot]);
        if(r<=0)return r;c->pending=false;c->age[slot]=++c->clock;
    }
    if(offset>v.file.size||bytes>v.file.size-offset)return -1;
    uint32_t copied=0;
    while(copied<bytes){
        uint64_t position=offset+copied;bool found=false;
        for(uint32_t i=0;i<CACHE_SLOTS;i++)if(!(c->pending&&c->filling==i)&&c->bytes[i]&&position>=c->offset[i]&&position-c->offset[i]<c->bytes[i]){
            uint32_t within=(uint32_t)(position-c->offset[i]),count=c->bytes[i]-within;
            if(count>bytes-copied)count=bytes-copied;
            ln_memory_copy((uint8_t *)buffer+copied,CACHE(i)+within,count);
            c->age[i]=++c->clock;copied+=count;found=true;break;
        }
        if(!found)break;
    }
    if(copied==bytes)return 1;
    if(c->pending)return 0;
    uint32_t slot=0;for(uint32_t i=1;i<CACHE_SLOTS;i++)if(c->age[i]<c->age[slot])slot=i;
    uint64_t at=(offset+copied)&~16383ull;if(at>=v.file.size)return -1;
    uint32_t count=v.file.size-(uint32_t)at;if(count>16384)count=16384;
    c->offset[slot]=at;c->bytes[slot]=count;c->filling=slot;c->owner=kind;c->pending=true;
    /* Start ownership immediately so the audio reserve cannot starve a
     * video cache miss before its first storage request has been issued. */
    int r=media_read(context,at,CACHE(slot),count);
    if(r>0){c->pending=false;c->age[slot]=++c->clock;}return r<0?r:0;
}
static bool audio_step(void){
    if(!v.audio||v.audio_eof)return true;
    struct ln_audio_status a;ln_audio_snapshot(&a);if(a.fault)return false;
    if(a.queued>=49152u&&!(v.reading&&v.io_kind==1))return true;
    int r;
    if(!v.audio_phase){r=ln_mp4_next(&v.sound,&v.audio_packet);
        if(r==LN_MP4_EOF){v.audio_eof=true;ln_audio_finish();return true;}
        if(r<0)return false;if(r>0){if(v.audio_packet.bytes>8191||v.audio_packet.duration>1024)return false;v.audio_phase=1;}
    }else if(v.audio_phase==1){r=cached_read((void *)1,v.audio_packet.offset,AAC.input,v.audio_packet.bytes);if(r<0)return false;if(r>0)v.audio_phase=2;
    }else if(v.audio_phase==2){
        uint32_t began=ln_time_us();r=ln_aac_raw_decode(&AAC,AAC.input,v.audio_packet.bytes);v.audio_us+=ln_time_us()-began;
        if(r!=1)return false;
        uint64_t lead=v.audio_skip>v.sound.lead?v.audio_skip:v.sound.lead;
        uint32_t skip=v.audio_packet.dts<lead?(lead-v.audio_packet.dts>v.audio_packet.duration?v.audio_packet.duration:(uint32_t)(lead-v.audio_packet.dts)):0;
        if(skip>v.audio_packet.duration)skip=v.audio_packet.duration;
        AAC.pcm_cursor=skip;AAC.pcm_frames=v.audio_packet.duration;v.audio_phase=3;
    }else {uint32_t count=AAC.pcm_frames-AAC.pcm_cursor;
        uint32_t used=count?ln_audio_submit(AAC.pcm+AAC.pcm_cursor*2,count):0;
        AAC.pcm_cursor+=used;v.audio_frames+=used;if(AAC.pcm_cursor==AAC.pcm_frames)v.audio_phase=0;
    }return true;
}
static void step(void){
    if(v.stop){
        if(v.io||v.phase==OPEN_WAIT){ln_storage_media_cancel(v.generation);int r=ln_storage_media_poll_deferred(v.generation,0,0);if(!r)return;v.io=false;}
        (void)ln_audio_stop();ln_video_decoder_close();
        if(v.boosted){if(!ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ))v.error=19;v.boosted=false;}
        v.active=v.reading=v.stop=false;v.phase=v.error?FAILED:IDLE;++v.revision;return;
    }
    int r;
    if(v.phase==OPEN){
        if(!v.boosted&&LINNET_VIDEO_CLOCK_HZ!=LINNET_ACTIVE_CLOCK_HZ){
            if(!ln_core_clock_set(LINNET_VIDEO_CLOCK_HZ)){fail(18);return;}v.boosted=true;
        }
        r=ln_storage_media_open(v.path,v.generation);if(r<0)fail(1);else if(r>0)v.phase=OPEN_WAIT;return;
    }
    if(v.phase==OPEN_WAIT){r=ln_storage_media_poll_deferred(v.generation,&v.file,0);if(r<0)fail(2);else if(r>0){ln_mp4_init(&v.movie,cached_read,0,v.file.size);v.phase=VIDEO_HEADER;}return;}
    if(v.phase==VIDEO_HEADER){r=ln_mp4_open_step(&v.movie);if(r<0){fail(3);return;}if(!r)return;
        if(!ln_video_decoder_init(v.movie.codec,v.movie.config,v.movie.config_bytes,v.movie.width,v.movie.height)){fail(4);return;}
        v.duration=milliseconds(v.movie.duration-v.movie.lead,v.movie.timescale);
        ln_mp4_init_audio(&v.sound,cached_read,(void *)1,v.file.size);v.phase=AUDIO_HEADER;return;
    }
    if(v.phase==AUDIO_HEADER){r=ln_mp4_open_step(&v.sound);if(!r)return;
        if(r==LN_MP4_EOF)v.audio=false;
        else if(r<0){fail(5);return;}
        else {if(ln_aac_raw_init(&AAC,v.sound.config,v.sound.config_bytes)!=1||AAC.rate!=v.sound.rate||!ln_audio_init(AAC.rate)){fail(6);return;}
            (void)ln_audio_volume(-400);v.audio=true;}
#if !defined(LINNET_PROFILE_RELEASE)
        v.index_ready_us=ln_time_us()-v.opened_us;
#endif
        v.phase=PRIME;return;
    }
    if(v.phase!=PRIME&&v.phase!=PLAY)return;
    if(v.seek_phase){
        if(v.seek_phase==1){
            if(v.io){ln_storage_media_cancel(v.generation);r=ln_storage_media_poll_deferred(v.generation,0,0);if(!r)return;v.io=false;}
            if(v.audio&&!ln_audio_halt()){fail(14);return;}
            v.reading=false;v.cache.pending=false;for(unsigned j=0;j<CACHE_SLOTS;j++)v.cache.bytes[j]=v.cache.age[j]=0;
            v.movie.packet_phase=v.sound.packet_phase=0;
            if(!ln_mp4_seek(&v.movie,ticks(v.seek_target,v.movie.timescale)+v.movie.lead,-1)){fail(15);return;}
            v.seek_phase=2;
        }else if(v.seek_phase==2){
            r=ln_mp4_seek_step(&v.movie);if(r<0){fail(15);return;}if(!r)return;
            v.base=milliseconds(v.movie.decode_time>v.movie.lead?v.movie.decode_time-v.movie.lead:0,v.movie.timescale);
            v.audio_skip=ticks(v.base,v.sound.timescale)+v.sound.lead;
            if(v.audio&&!ln_mp4_seek(&v.sound,v.audio_skip,0)){fail(16);return;}v.seek_phase=3;
        }else {
            if(v.audio){r=ln_mp4_seek_step(&v.sound);if(r<0){fail(16);return;}if(!r)return;}
            if(!ln_video_decoder_init(v.movie.codec,v.movie.config,v.movie.config_bytes,v.movie.width,v.movie.height)||
                (v.audio&&(ln_aac_raw_init(&AAC,v.sound.config,v.sound.config_bytes)!=1||!ln_audio_init(AAC.rate)))){fail(17);return;}
            v.started=v.tail=v.audio_eof=v.video_eof=v.frame_ready=v.offered=false;
            v.video_phase=v.audio_phase=0;v.position=v.base;v.phase=PRIME;v.seek_phase=0;++v.revision;
        }return;
    }
    if(v.paused){
        /* A pause can arrive while a media read owns FAT and an ATA buffer.
         * Drain its cancellation now so the disk may enter standby. The
         * parser will retry the same read after playback resumes. */
        if(v.io){
            ln_storage_media_cancel(v.generation);
            if(!ln_storage_media_poll_deferred(v.generation,0,0))return;
            v.io=v.reading=v.cache.pending=false;
            v.cache.bytes[v.cache.filling]=0;
        }
        return;
    }
    struct ln_audio_status a={0};if(v.audio)ln_audio_snapshot(&a);
    if(!audio_step()){fail(7);return;}
    if(v.audio&&a.queued<START_AUDIO_FRAMES&&!v.audio_eof&&!(v.reading&&v.io_kind==0))return;
    if(!v.started&&(v.frame_ready||v.video_eof)&&(!v.audio||a.queued>=START_AUDIO_FRAMES||v.audio_eof)){
        if(v.audio&&!ln_audio_start()){fail(8);return;}
#if !defined(LINNET_PROFILE_RELEASE)
        if(v.audio&&!v.audio_start_us){
            struct ln_audio_status started_audio;ln_audio_snapshot(&started_audio);
            v.audio_start_us=ln_time_us()-v.opened_us;v.start_ready_frames=started_audio.queued;
        }
#endif
        wall_reset(ln_time_us());v.audio_at=0;v.audio_elapsed=0;
        v.started=true;v.phase=PLAY;
    }
    uint32_t now=clock_ms();v.position=now;
    if(v.frame_ready){
        /* Keep one decoded frame while preparing the next hardware job.
         * Its color conversion can then overlap that job. A storage miss
         * must not delay an already due presentation. */
        bool runnable=v.video_phase==3||v.video_eof||v.reading||!ln_video_decoder_async();
        if(v.started&&now>=v.frame_due&&!v.offered&&runnable){
            ln_video_decoder_render();
            uint32_t width,height;ln_video_decoder_output_size(&width,&height);
            v.front=v.ready;v.asset=(struct ln_ui_asset){width,height,pixels[v.front]};
            v.offered=true;v.frame_ready=false;++v.revision;
            if(now>v.frame_due+5){++v.late;if(now-v.frame_due>v.max_late)v.max_late=now-v.frame_due;}
        }
        if(v.frame_ready&&(v.video_phase==3||!ln_video_decoder_async()))return;
    }
    if(v.video_eof){if(!v.frame_ready&&!v.offered&&(!v.audio||(v.audio_eof&&!a.queued))&&now>=v.duration){v.resume[0]=v.identity;v.resume[1]=0;v.completed=true;v.stop=true;}return;}
    if(!v.video_phase){r=ln_mp4_next(&v.movie,&v.video_packet);
        if(r==LN_MP4_EOF){v.video_eof=true;return;}if(r<0){fail(9);return;}
        if(r>0){if(v.video_packet.bytes>LINNET_VIDEO_PACKET_BYTES){fail(10);return;}v.video_phase=1;}
    }else if(v.video_phase==1){r=cached_read(0,v.video_packet.offset,PACKET,v.video_packet.bytes);if(r<0)fail(11);else if(r>0)v.video_phase=2;
    }else {
        uint32_t due=milliseconds(v.video_packet.dts>v.movie.lead?v.video_packet.dts-v.movie.lead:0,v.movie.timescale);
        uint32_t span=milliseconds(v.video_packet.duration,v.movie.timescale);
        uint32_t clock=clock_ms();
        bool discard=v.started&&v.movie.packet<v.movie.samples&&clock>due+span&&clock-v.last_picture<100u;
        /* Always decode reference pictures. Late pictures can omit RGB
         * conversion and presentation without corrupting later frames. */
        uint32_t began=ln_time_us();r=ln_video_decoder_decode(PACKET,v.video_packet.bytes,discard?0:pixels[v.back]);uint32_t elapsed=ln_time_us()-began;
        if(elapsed>v.max_decode)v.max_decode=elapsed;if(r<0){fail(12);return;}
        if(r==2){v.video_phase=3;return;}
        if(r>0){
            ++v.decoded;
            if(discard)++v.dropped;
            else {
                v.frame_ready=true;v.ready=v.back;v.frame_due=due;v.last_picture=clock;
                for(unsigned index=0;index<3;index++)if(index!=v.front&&index!=v.ready){v.back=index;break;}
            }
        }v.video_phase=0;
    }
}
void ln_video_init(void){v.phase=IDLE;v.options=8;v.front=0xffffffffu;}
bool ln_video_open(const char *path){
    if(v.active||v.io||ln_video_catalog_pending()||!path||path[0]!='/')return false;
    uint32_t n=0,id=2166136261;while(path[n]&&n<255){id=(id^(uint8_t)path[n])*16777619u;++n;}if(!n||n==255)return false;
    uint32_t generation=v.generation+1,revision=v.revision+1,resume[2]={v.resume[0],v.resume[1]};uint8_t options=v.options;
    bool headphone_known=v.headphone_known,headphones=v.headphones;
    /* Reset ownership before the decoder publishes another complete frame. */
    __builtin_memset(&v,0,sizeof v);
#if !defined(LINNET_PROFILE_RELEASE)
    v.opened_us=ln_time_us();
#endif
    v.generation=generation?generation:1;v.revision=revision;v.options=options;v.resume[0]=resume[0];v.resume[1]=resume[1];v.identity=id;
    v.headphone_known=headphone_known;v.headphones=headphones;
    if(resume[0]==id&&resume[1]){v.seek_target=resume[1];v.seek_phase=1;}
    for(uint32_t i=0;i<=n;i++)v.path[i]=path[i];const char *title=path;for(uint32_t i=0;i<n;i++)if(path[i]=='/')title=path+i+1;
    for(uint32_t i=0;title[i]&&i<127;i++)v.title[i]=title[i];v.front=0xffffffffu;v.active=true;v.phase=OPEN;return true;
}
bool ln_video_open_file(const struct ln_fat32_file*file,const char*title){
    if(!file||!title||!ln_video_open(title))return false;
    v.file=*file;
    if(LINNET_VIDEO_CLOCK_HZ!=LINNET_ACTIVE_CLOCK_HZ){if(!ln_core_clock_set(LINNET_VIDEO_CLOCK_HZ)){fail(18);return true;}v.boosted=true;}
    ln_mp4_init(&v.movie,cached_read,0,v.file.size);v.phase=VIDEO_HEADER;return true;
}
void ln_video_step(void){ln_video_catalog_step(v.active||v.io);if(!v.active||v.stepping)return;v.stepping=true;
    uint32_t began=ln_time_us();for(unsigned i=0;i<32;i++){
        step();
        /* Storage and display advance in the outer executive. Repeated
         * polling here only delays the owner needed to finish the work. */
        if(!v.active||v.io||v.reading||v.offered||v.paused||v.phase==OPEN_WAIT||ln_time_us()-began>2000)break;
    }v.stepping=false;
}
void ln_video_stop(void){v.completed=false;if(v.active){v.resume[0]=v.identity;v.resume[1]=clock_ms();v.stop=true;}}
bool ln_video_active(void){return v.active;}
bool ln_video_completed(void){return v.completed&&!v.active&&!v.io&&!v.error;}
bool ln_video_pending(void){return v.io||ln_video_catalog_pending();}
bool ln_video_playback_pending(void){return v.io;}
void ln_video_pause(bool pause){if(!v.active||v.paused==pause)return;
    if(pause){v.paused_at=ln_time_us();if(v.started&&(!v.audio||v.tail))
            (void)wall_position(v.paused_at,v.tail?v.tail_position:v.base);
        if(v.audio)ln_audio_pause();}
    else {if(v.started&&(!v.audio||v.tail))v.wall_at=ln_time_us();
        if(v.audio&&v.started&&!v.seek_phase&&!v.tail&&!ln_audio_start()){fail(13);return;}}
    v.paused=pause;++v.revision;
}
bool ln_video_controls(bool show){(void)show;return true;}
bool ln_video_paused(void){return v.paused;}
bool ln_video_seek(int32_t seconds){
    if(!v.active||!v.started||v.stop||v.seek_phase||!seconds)return false;
    int64_t target=(int64_t)clock_ms()+(int64_t)seconds*1000;
    if(target<0)target=0;if(target>=v.duration)target=v.duration?v.duration-1u:0;
    v.seek_target=(uint32_t)target;v.seek_phase=1;return true;
}
uint8_t ln_video_options(void){return v.options;}
void ln_video_options_set(uint8_t options){v.options=options&8u;}
const char *ln_video_option_label(uint32_t row){return row==0?"TV Out: Unavailable":row==1?"Display: Built-in":row==2?"Aspect: Original":"Video: Native";}
bool ln_video_option_adjust(uint32_t row,int32_t direction){(void)row;(void)direction;return false;}
bool ln_video_output_prompt(void){return false;}
void ln_video_output_once(bool television){(void)television;}
void ln_video_headphones(bool connected){if(v.headphone_known&&v.headphones&&!connected&&v.started)ln_video_pause(true);v.headphone_known=true;v.headphones=connected;}
uint32_t ln_video_progress(void){uint32_t position=v.position,duration=v.duration;if(!duration)return 0;if(position>=duration)return 1000;while(duration>0x3fffffu){position>>=1;duration>>=1;}return position*1000u/duration;}
const char *ln_video_title(void){return v.title;}
const char *ln_video_status(void){return v.error?"Video unavailable":v.paused?"Paused":v.started?"Playing":v.active?"Opening":"Stopped";}
const struct ln_ui_asset *ln_video_asset(void){return v.front==0xffffffffu?0:&v.asset;}
uint32_t ln_video_revision(void){return v.revision;}
void ln_video_presented(void){if(v.offered){
#if !defined(LINNET_PROFILE_RELEASE)
    if(!v.presented)v.first_present_us=ln_time_us()-v.opened_us;
#endif
    v.offered=false;++v.presented;}}
void ln_video_resume_get(uint32_t out[2]){out[0]=v.active?v.identity:v.resume[0];out[1]=v.active?clock_ms():v.resume[1];}
void ln_video_resume_set(const uint32_t in[2]){v.resume[0]=in[0];v.resume[1]=in[1];}
uint32_t ln_video_deadline(uint32_t now){
    if(!v.active)return now+200000u;
    if(v.stop||v.seek_phase||(v.paused&&v.io))return now;
    if(v.paused)return now+200000u;
    if(v.frame_ready)return now+1000u;
    /* A pending read cannot be advanced by another decoder call. The storage
     * deadline wakes the executive as soon as ATA has work to service. */
    if(v.io)return now+200000u;
    /* Decode and submit phases still run without an artificial frame delay. */
    return now;
}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_video_diagnostics(uint32_t out[32]){uint32_t words[32]={0x53444956u,0,v.phase,v.error,v.active,v.io,v.paused,v.movie.packet,v.movie.samples,v.front,v.reads,v.read_bytes,v.decoded,v.presented,v.late,v.max_late,v.max_decode,v.frame_ready,v.offered,ln_video_decoder_time(),ln_yuv_convert_us,v.seek_phase,v.audio_frames,v.revision,v.audio_us,v.io_us,v.dropped,0,0,0,v.position,v.duration};for(unsigned i=0;i<32;i++)out[i]=words[i];ln_video_decoder_diagnostics(out+27);}
bool ln_video_measurement_arm(uint32_t mode){return !mode&&!v.active;}
void ln_video_metadata(uint32_t page,uint8_t out[128]){
    for(unsigned i=0;i<128;i++)out[i]=0;
    if(!page){ln_write_le32(out,v.movie.width);ln_write_le32(out+4,v.movie.height);}
    else if(page==8u){
        uint32_t words[8]={0x41454d56u,0,v.opened_us,v.first_present_us,v.index_ready_us,
            v.audio_start_us,v.start_ready_frames,v.dropped};
        for(unsigned i=0;i<8;i++)ln_write_le32(out+i*4u,words[i]);
    }
}
#endif
