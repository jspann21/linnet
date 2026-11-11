#include "boot/include/linnet_bytes.h"
#if !defined(LINNET_PROFILE_RELEASE)
#define LN_VIDEO_RECORD(...) do { __VA_ARGS__; } while (0)
#else
#define LN_VIDEO_RECORD(...) ((void)0)
#endif
#include "services/video/video.h"
#include "services/video/catalog.h"
#include "services/storage/storage.h"
#include "hal/api/audio.h"
#include "hal/api/platform.h"
#define PIXELS (LINNET_VIDEO_WIDTH*LINNET_VIDEO_HEIGHT)
#define FRAME_BYTES (PIXELS*2u)
#define IPVF_PAYLOAD (FRAME_BYTES+4096u)
#define RECORD LN_STORAGE_MEDIA_MAX_READ
#define IPVF_LZ_SCRATCH ((uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS)
/* Reserved STAGE: LZ scratch, stereo PCM, decoded video, then raw records.
 * The HAL continues to own its existing DMA ring. */
#define PCM_BASE ((uint32_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x00020000u))
#define PCM_FRAMES 262144u
#define VIDEO_BASE ((uint16_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x00120000u))
#define VIDEO_STAGING_BYTES (LINNET_STAGE_CAPACITY-0x00200000u)
#define FRAME_SLOT_CAPACITY (3u+VIDEO_STAGING_BYTES/FRAME_BYTES)
#define FRAME_SLOTS (FRAME_SLOT_CAPACITY<96u?FRAME_SLOT_CAPACITY:96u)
#define VIDEO_PREROLL (FRAME_SLOTS-3u<80u?FRAME_SLOTS-3u:80u)
#define RAW_BASE ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+LINNET_STAGE_CAPACITY-0x000e0000u))
#define RAW_CAPACITY 0x000e0000u
#define RAW_ENTRIES 1024u
#define NONE 0xffffffffu
_Static_assert(RECORD>=FRAME_BYTES+16384u,"target media read window is too small for a video frame");
_Static_assert((PCM_FRAMES&(PCM_FRAMES-1u))==0u&&PCM_FRAMES*4u==0x00100000u,"video PCM staging bounds");
_Static_assert((FRAME_SLOTS-3u)*FRAME_BYTES<=VIDEO_STAGING_BYTES,"decoded video staging bounds");
_Static_assert(VIDEO_PREROLL<FRAME_SLOTS,"decoded video preroll bounds");
_Static_assert(FRAME_SLOTS<=96u,"decoded slot ownership bitmap bounds");
enum phase {IDLE,OPEN,HEADER,VERIFY,INDEX,READ,CRC,DECODE,FRAME_READY,PCM,WAIT,SEEK};
/* Only validated reads and complete decoded frames publish these buffers. */
static struct {
    uint16_t frame[2][PIXELS];
    union {uint16_t pixels[PIXELS];uint8_t bytes[IPVF_PAYLOAD];} third;
    union {uint32_t align;uint8_t bytes[RECORD];} record;
    uint8_t index_page[512];
} buffers __attribute__((section(".media_workspace"),aligned(32)));
static struct {
    struct ln_fat32_file file;struct ln_ui_asset asset;
    char path[256],title[128];
    uint32_t phase,error,epoch,working,generation,requested,revision;
    uint32_t width,height,fps,count,index_crc;
    uint32_t fps_num,fps_den,rate,media_end,index_at,index_count,first_sectors,preroll;
    uint32_t time_frame[2],time_sample[2],time_clock,time_clock_frame,time_valid;
    uint32_t verify_at,verify_crc,verify_frame,verify_end,verify_key;
    uint32_t next,target,origin,offset,bytes,sample,samples;
    uint32_t type,video_bytes,at,decode_at,decode_pixel,tile_count;
    uint32_t audio_bytes,next_sectors,decoded_bytes,source_type,seek_index,seek_limit,seek_frame,seek_offset,seek_sectors;
    uint32_t prefetched_frames,audio_offset,audio_sectors;
    uint32_t video_next,video_offset,video_bytes_saved;
    bool audio_yield;
    uint32_t raw_head,raw_tail,raw_used,raw_first,raw_count;
    const uint8_t *raw_view;
    uint8_t *raw_write;
    uint32_t pcm_head,pcm_tail;
    struct {uint32_t frame,offset,bytes;} raw[RAW_ENTRIES];
    uint32_t rect_x,rect_y,rect_w,rect_h,rect_row,rect_data_at;int32_t motion_dx,motion_dy;
    int32_t ima_left,ima_right;uint8_t ima_left_index,ima_right_index;
    uint32_t front,front_frame,previous,building,queue[FRAME_SLOTS],qhead,qcount,queue_frame[FRAME_SLOTS],submitted;
    uint32_t reads,read_bytes,prepared,presented,late,max_late,max_prepare,prepare_start,max_queue;
    uint32_t audio_gaps,audio_silence,audio_fault,identity,resume_frame;
    uint32_t index_page_at,io_started,max_io,read_time,max_read_time;
    uint32_t cpu[4],max_cpu[4],max_step,min_audio;
    uint32_t lz_input_offset,lz_input_bytes,lz_output_bytes,lz_output_slot;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t measurement,measurement_opened,measurement_started,first_present_us,index_ready_us,dropped_frames;
    uint32_t phase_us[14],phase_calls[14];
    uint32_t type_us[7],type_calls[7],lz_us;
    uint32_t worst_frame,worst_queue,late_one,late_two,last_present_us,max_gap_us,gap_frame,min_ready,start_ready;
    bool measurement_ran;
#endif
    bool active,pending,stepping,stopping,ready,started,paused,finish,seeking,offered,completed,temporal,prefetching,startup_preview;
} v;
static bool video_headphone_known,video_headphones;
static uint32_t saved_resume[2];
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t armed_measurement;
void ln_video_metadata(uint32_t page,uint8_t out[128]){
    for(uint32_t i=0;i<128u;++i)out[i]=0;
    if(page<2u){for(uint32_t i=0;i<128u;++i)out[i]=(uint8_t)v.path[page*128u+i];}
    else if(page==2u){for(uint32_t i=0;i<128u;++i)out[i]=(uint8_t)v.title[i];}
    else if(page==6u){uint32_t words[15];for(uint32_t i=0;i<7u;++i){words[i]=v.type_us[i];words[i+7u]=v.type_calls[i];}words[14]=v.lz_us;
        for(uint32_t i=0;i<15u;++i)for(uint32_t j=0;j<4u;++j)out[i*4u+j]=(uint8_t)(words[i]>>(j*8u));}
    else if(page==7u){uint32_t words[11]={0x31504956u,v.worst_frame,v.max_late,v.worst_queue,v.late_one,v.late_two,v.max_gap_us,v.gap_frame,v.min_ready,v.start_ready,FRAME_SLOTS};
        for(uint32_t i=0;i<11u;++i)for(uint32_t j=0;j<4u;++j)out[i*4u+j]=(uint8_t)(words[i]>>(j*8u));}
    else if(page==8u){uint32_t words[8]={0x41454d56u,0u,v.measurement_opened,v.first_present_us,v.index_ready_us,
            v.measurement_started?v.measurement_started-v.measurement_opened:0u,v.start_ready,v.dropped_frames};
        for(uint32_t i=0;i<8u;++i)for(uint32_t j=0;j<4u;++j)out[i*4u+j]=(uint8_t)(words[i]>>(j*8u));}
    else if(page>=4u){const uint32_t*words=page==4u?v.phase_us:v.phase_calls;
        for(uint32_t i=0;i<14u;++i)for(uint32_t j=0;j<4u;++j)out[i*4u+j]=(uint8_t)(words[i]>>(j*8u));}
    else {uint32_t words[8]={0x314d4956u,v.identity,v.count,v.fps_num,v.fps_den,v.rate,v.file.size,1u};
        for(uint32_t i=0;i<8u;++i)for(uint32_t j=0;j<4u;++j)out[i*4u+j]=(uint8_t)(words[i]>>(j*8u));}
}
bool ln_video_measurement_arm(uint32_t mode){
    if(mode>2u||v.active||v.pending)return false;
    armed_measurement=mode;return true;
}
#endif
static const uint8_t *record_source(void){return v.raw_write?v.raw_write:v.raw_view?v.raw_view:buffers.record.bytes;}
static void clear(void*p,uint32_t n){
#if defined(__arm__)
    extern void ln_memory_clear(void *,uint32_t);ln_memory_clear(p,n);
#else
    uint8_t*b=p;while(n--)*b++=0;
#endif
}
static void copy(void*d,const void*s,uint32_t n){
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);ln_memory_copy(d,s,n);
#else
    uint8_t*a=d;const uint8_t*b=s;
    if(!(((uintptr_t)a|(uintptr_t)b)&3u)){while(n>=4){__builtin_memcpy(a,b,4);a+=4;b+=4;n-=4;}}
    while(n--)*a++=*b++;
#endif
}
static uint16_t *frame_slot(uint32_t slot){return slot<2u?buffers.frame[slot]:slot==2u?buffers.third.pixels:slot<FRAME_SLOTS?VIDEO_BASE+(slot-3u)*PIXELS:0;}
/* IPVF uses MSB-first CRC-32 state without a final XOR. */
#include "crc_table.h"
static uint32_t ipvf_crc(uint32_t crc,const uint8_t*b,uint32_t n){return ipvf_crc_slice4(crc,b,n);}
static uint64_t divide64(uint64_t numerator,uint32_t denominator){
    uint64_t quotient=0;uint32_t remainder=0;if(!denominator)return 0;
    for(uint32_t bit=64;bit;--bit){uint32_t carry=remainder>>31;remainder=(remainder<<1)|(uint32_t)((numerator>>(bit-1u))&1u);if(carry||remainder>=denominator){remainder-=denominator;quotient|=(uint64_t)1u<<(bit-1u);}}return quotient;
}
static uint32_t frame_sample(uint32_t frame){
    uint32_t slot=frame&1u,bit=1u<<slot;
    if((v.time_valid&bit)&&v.time_frame[slot]==frame)return v.time_sample[slot];
    uint32_t sample=(uint32_t)divide64((uint64_t)frame*v.rate*v.fps_den+v.fps_num/2u,v.fps_num);
    v.time_frame[slot]=frame;v.time_sample[slot]=sample;v.time_valid|=bit;
    return sample;
}
static uint32_t sample_frame(uint32_t sample){
    /* Audio advances in DMA blocks while decode/PCM steps run many times.
     * Reuse exact timing conversions until their input changes. The cache
     * is cleared with the stream, so seeks retain the same time base. */
    if((v.time_valid&4u)&&v.time_clock==sample)return v.time_clock_frame;
    v.time_clock=sample;v.time_clock_frame=(uint32_t)divide64((uint64_t)sample*v.fps_num,v.rate*v.fps_den);
    v.time_valid|=4u;return v.time_clock_frame;
}
#include "ima_lookup.h"
static inline __attribute__((always_inline)) int16_t ima_nibble(uint32_t code,int32_t*predictor,uint32_t*index){
 uint32_t entry=ima_lookup[(*index<<3)|(code&7u)];
 int32_t difference=(int32_t)(entry>>8);
 int32_t sample=*predictor+(code&8u?-difference:difference);
 if(sample>32767)sample=32767;if(sample< -32768)sample=-32768;
 *predictor=sample;*index=entry&255u;return (int16_t)sample;
}
static void fail(uint32_t e){v.error=e;v.stopping=true;++v.epoch;++v.revision;}
static uint32_t generation(void){if(++v.generation>=0x78000000u)v.generation=0x71000001u;return v.generation;}
static int io(uint32_t at,void*buf,uint32_t n,bool open){
    if(!v.pending){int r=open?ln_storage_media_open(v.path,generation()):ln_storage_media_read(&v.file,at,buf,n,generation());if(r<=0)return r;v.pending=true;v.requested=n;LN_VIDEO_RECORD(v.io_started=ln_time_us());return 0;}
    uint32_t got=0,started=ln_time_us();int r=0;
    /* FAT advances one small state/sector per poll, including cache hits.
     * Drain a bounded burst locally rather than paying a complete executive
     * round for every memory-only transition. Read polls use the video-owned
     * storage service; open polls retain the ordinary callback path. */
    for(uint32_t polls=0;polls<(open?64u:256u);polls++){
        r=open?ln_storage_media_poll(v.generation,&v.file,&got):
            ln_storage_media_poll_video(v.generation,0,&got);
        if(r||v.working!=v.epoch||ln_time_us()-started>=(open?250u:4000u))break;
    }
    if(!r)return 0;v.pending=false;
    if(v.working!=v.epoch)return 0;if(r!=1||(!open&&got!=v.requested))return -1;
    LN_VIDEO_RECORD(uint32_t elapsed=ln_time_us()-v.io_started;if(elapsed>v.max_io)v.max_io=elapsed;
        if(!open){++v.reads;v.read_bytes+=got;if(v.phase==READ)v.read_time+=elapsed;});return 1;
}
static uint32_t staged_pcm(void){return v.pcm_head-v.pcm_tail;}
static void pump_ipvf_pcm(void){
    if(!v.ready||v.finish)return;
    uint32_t frames=staged_pcm(),offset=v.pcm_tail&(PCM_FRAMES-1u);
    if(frames>1024u)frames=1024u;
    if(frames>PCM_FRAMES-offset)frames=PCM_FRAMES-offset;
    if(frames)v.pcm_tail+=ln_audio_submit((const int16_t*)(PCM_BASE+offset),frames);
    /* All decoded samples must enter the HAL before it receives EOF. */
    if(v.prefetched_frames==v.count&&!staged_pcm()){
        ln_audio_finish();v.finish=true;
    }
}
static uint32_t submit_ipvf_pcm(void){
    uint32_t capacity=PCM_FRAMES-staged_pcm();if(!capacity)return 0;uint32_t frames=v.samples-v.at;if(frames>64u)frames=64u;if(frames>capacity)frames=capacity;
    const uint8_t*source=record_source()+16u+v.video_bytes;bool silence=!v.audio_bytes,mono=v.audio_bytes==4u+v.samples/2u;
    if(v.at==0u&&!silence){if((mono&&(source[3]||source[2]>88u))||(!mono&&(source[3]||source[7]||source[2]>88u||source[6]>88u)))return NONE;
        uint32_t left=ln_read_le16(source);v.ima_left=left<0x8000u?(int32_t)left:(int32_t)left-0x10000;v.ima_left_index=source[2];
        if(mono){v.ima_right=v.ima_left;v.ima_right_index=v.ima_left_index;}else{uint32_t right=ln_read_le16(source+4);v.ima_right=right<0x8000u?(int32_t)right:(int32_t)right-0x10000;v.ima_right_index=source[6];}}
    uint32_t head=v.pcm_head,i=0;
    if(silence){for(;i<frames;++i)PCM_BASE[(head+i)&(PCM_FRAMES-1u)]=0;}
    else{
        /* Keep the ADPCM state in registers throughout this bounded chunk. */
        int32_t left=v.ima_left,right=v.ima_right;
        uint32_t left_index=v.ima_left_index,right_index=v.ima_right_index;
        if(v.at==0u&&frames){PCM_BASE[head&(PCM_FRAMES-1u)]=(uint16_t)left|((uint32_t)(uint16_t)right<<16);i=1u;}
        if(mono){
            uint32_t nibble=v.at+i-1u;
            for(;i<frames;++i,++nibble){
                uint32_t code=(source[4u+(nibble>>1)]>>((nibble&1u)*4u))&15u;
                uint32_t sample=(uint16_t)ima_nibble(code,&left,&left_index);
                PCM_BASE[(head+i)&(PCM_FRAMES-1u)]=sample|(sample<<16);
            }
        }else{
            const uint8_t *packed=source+8u+v.at+i-1u;
            for(;i<frames;++i){
                uint32_t codes=*packed++;
                uint32_t l=(uint16_t)ima_nibble(codes&15u,&left,&left_index);
                uint32_t r=(uint16_t)ima_nibble(codes>>4,&right,&right_index);
                PCM_BASE[(head+i)&(PCM_FRAMES-1u)]=l|(r<<16);
            }
        }
        v.ima_left=left;v.ima_left_index=(uint8_t)left_index;
        v.ima_right=right;v.ima_right_index=(uint8_t)right_index;
    }
    v.pcm_head+=frames;return frames;
}
static int read_ipvf_index(uint32_t index){
    uint32_t offset=v.index_at+index*16u,page=offset&~511u;
    if(v.index_page_at!=page){v.index_page_at=NONE;uint32_t n=v.file.size-page;if(n>512u)n=512u;int r=io(page,buffers.index_page,n,false);if(r!=1)return r;v.index_page_at=page;}
    copy(buffers.record.bytes,buffers.index_page+(offset&511u),16);return 1;
}
static bool ipvf_index_record(const uint8_t*b){
    uint64_t offset=ln_read_le64(b+4);uint32_t frame=ln_read_le32(b),sectors=ln_read_le16(b+12),flags=ln_read_le16(b+14);
    return frame<v.count&&offset>=512u&&offset<v.media_end&&!(offset&511u)&&offset<=0x7fffffffu&&
        sectors&&sectors<=LINNET_STORAGE_MAX_READ_SECTORS&&offset+(uint64_t)sectors*512u<=v.media_end&&!(flags&~1u);
}
static bool ipvf_header(void){
    const uint8_t*b=buffers.record.bytes;uint32_t flags=ln_read_le32(b+20),metadata=ln_read_le16(b+66),metadata_at=ln_read_le32(b+68);
    uint64_t media_end=ln_read_le64(b+44),index_at=ln_read_le64(b+52),index_bytes=(uint64_t)ln_read_le32(b+60)*16u;
    if(ln_read_le32(b)!=0x46565049u||ln_read_le16(b+4)!=80u||ln_read_le16(b+6)!=LINNET_VIDEO_WIDTH||ln_read_le16(b+8)!=LINNET_VIDEO_HEIGHT||
       !ln_read_le16(b+10)||!ln_read_le16(b+12)||ln_read_le16(b+10)<4u*ln_read_le16(b+12)||ln_read_le16(b+10)>240u*ln_read_le16(b+12)||
       !ln_read_le32(b+16)||(flags!=11u&&flags!=27u)||ln_read_le32(b+24)!=512u||!ln_read_le16(b+14)||ln_read_le16(b+14)>LINNET_STORAGE_MAX_READ_SECTORS||
       ln_read_le16(b+28)!=2u||ln_read_le16(b+30)!=2u||ln_read_le16(b+32)!=16u||ln_read_le32(b+34)!=44100u||ln_read_le16(b+42)||
       media_end<=512u||media_end>0x7fffffffu||index_at!=media_end||index_at>v.file.size||
       !ln_read_le32(b+60)||ln_read_le32(b+60)>ln_read_le32(b+16)||ln_read_le16(b+64)!=16u||metadata_at!=80u||metadata>432u||
       index_bytes!=v.file.size-index_at)return false;
    uint64_t total=divide64((uint64_t)ln_read_le32(b+16)*44100u*ln_read_le16(b+12)+ln_read_le16(b+10)/2u,ln_read_le16(b+10));
    if(!total||total>0xffffffffu||ln_read_le32(b+38)!=(uint32_t)total)return false;
    uint32_t tags=0,at=80,end=80u+metadata;while(at<end){if(end-at<2u)return false;uint32_t tag=b[at],n=b[at+1];
        if(tag<1u||tag>3u||!n||(tags&(1u<<tag))||n>end-at-2u)return false;tags|=1u<<tag;
        if(tag==1u){uint32_t take=n<127u?n:127u;copy(v.title,b+at+2u,take);v.title[take]=0;}at+=2u+n;}
    if(!ln_bytes_zero(b+end,512u-end))return false;
    v.temporal=(flags&16u)!=0;v.width=LINNET_VIDEO_WIDTH;v.height=LINNET_VIDEO_HEIGHT;v.fps_num=ln_read_le16(b+10);v.fps_den=ln_read_le16(b+12);v.fps=v.fps_num/v.fps_den;
    /* Keep a half-second video cushion; the audio
     * producer continues targeting four seconds independently. */
    v.preroll=(v.fps_num+2u*v.fps_den-1u)/(2u*v.fps_den);
    if(v.preroll>VIDEO_PREROLL)v.preroll=VIDEO_PREROLL;if(v.preroll<2u)v.preroll=2u;
    v.rate=44100;v.count=ln_read_le32(b+16);v.media_end=(uint32_t)media_end;v.index_at=(uint32_t)index_at;
    v.index_count=ln_read_le32(b+60);v.index_crc=ln_read_le32(b+72);v.identity=ln_read_le32(b+76);v.first_sectors=ln_read_le16(b+14);
    return true;
}
static uint32_t free_slot(void){
    /* Build ownership once instead of rescanning the whole queue per slot. */
    uint32_t used[3]={0,0,0};
    if(v.front!=NONE)used[v.front>>5]|=1u<<(v.front&31u);
    if(v.previous!=NONE)used[v.previous>>5]|=1u<<(v.previous&31u);
    for(uint32_t q=0,at=v.qhead;q<v.qcount;++q){uint32_t s=v.queue[at];used[s>>5]|=1u<<(s&31u);if(++at==FRAME_SLOTS)at=0;}
    for(uint32_t s=0;s<FRAME_SLOTS;++s)if(!(used[s>>5]&(1u<<(s&31u))))return s;
    return NONE;
}
static void present(struct ln_audio_status*a){
    uint32_t clock=v.origin+a->consumed;
    if(v.qcount&&!v.offered){
        /* Decode every dependency, but present only the newest due frame. */
        while(v.started&&v.qcount>1u){uint32_t next=(v.qhead+1u)%FRAME_SLOTS;
            if(frame_sample(v.queue_frame[next])>clock)break;
            v.qhead=next;--v.qcount;
#if !defined(LINNET_PROFILE_RELEASE)
            ++v.dropped_frames;
#endif
        }
        uint32_t s=v.queue[v.qhead],frame=v.queue_frame[v.qhead],sample=frame_sample(frame);
        if((!v.started&&v.front==NONE)||(v.started&&sample<=clock)){
            v.front=s;v.front_frame=frame;v.asset=(struct ln_ui_asset){v.width,v.height,frame_slot(s)};v.qhead=(v.qhead+1)%FRAME_SLOTS;--v.qcount;
#if !defined(LINNET_PROFILE_RELEASE)
            if(v.measurement==2u){
                ++v.presented;
                if(v.started&&clock>sample){uint32_t late=clock-sample;++v.late;if(late>v.max_late)v.max_late=late;}
                return;
            }
#endif
            /* Spare render time when audio is running short. At low reserve,
             * retain alternate display frames; at critical reserve prioritize
             * the producer until it has at least a quarter of a second queued.
             * The decoder's previous-frame ownership is independent of display. */
            uint32_t reserve=a->queued+staged_pcm();
            bool expired=v.started&&clock>=frame_sample(frame+1u);
            if(expired||(v.started&&!v.paused&&!v.finish&&reserve<v.rate&&
               ((frame&1u)||reserve<v.rate/4u))){
#if !defined(LINNET_PROFILE_RELEASE)
                ++v.dropped_frames;
#endif
                return;
            }
            v.offered=true;++v.revision;
        }
    }
}
#define VIDEO_LZ_CODE __attribute__((noinline))
extern void ln_video_lz_copy_long(uint8_t *,uint32_t,uint32_t);
/* Independent raw LZ4 blocks retain exact output and terminal bounds. Each
 * bounded block is completed in one pass; the caller owns its output slot. */
#if defined(__arm__)
extern int ln_video_lz_decode_block(const uint8_t*,uint32_t,uint8_t*,uint32_t);
#else
static VIDEO_LZ_CODE int ln_video_lz_decode_block(const uint8_t *in,uint32_t input_size,uint8_t *out,uint32_t size){
    const uint8_t *source=in,*input_end=in+input_size;
    uint8_t *destination=out,*output_end=out+size;bool matched=false;
    if(!input_size)return -1;
    for(;;){
        if(source>=input_end)return -1;
        uint32_t token=*source++,literal=token>>4;
        /* Common short sequences fit entirely inside the already bounded
         * block. Validate their combined input/output spans once. */
        if(literal<=2u&&(token&15u)<=4u&&(uint32_t)(input_end-source)>=literal+2u&&
           (uint32_t)(output_end-destination)>=literal+12u){
            uint32_t offset=ln_read_le16(source+literal),length=4u+(token&15u);
            if(!offset||offset>(uint32_t)(destination-out)+literal)return -1;
            if(literal){destination[0]=source[0];if(literal==2u)destination[1]=source[1];}
            source+=literal+2u;destination+=literal;
            uint8_t*write=destination;const uint8_t*match=write-offset;
            write[0]=match[0];write[1]=match[1];write[2]=match[2];write[3]=match[3];
            if(length>4u)write[4]=match[4];
            if(length>5u)write[5]=match[5];
            if(length>6u)write[6]=match[6];
            if(length>7u)write[7]=match[7];
            destination+=length;matched=true;continue;
        }
        if(literal==15u){uint32_t extension;do{if(source>=input_end)return -1;extension=*source++;if(extension>size||literal>size-extension)return -1;literal+=extension;}while(extension==255u);}
        if(literal>(uint32_t)(input_end-source)||literal>(uint32_t)(output_end-destination))return -1;
        if(literal){
            /* Avoid the bulk copier's register save/restore for tiny tokens. */
            if(literal<=8u){for(uint32_t i=0;i<literal;++i)destination[i]=source[i];}
            else copy(destination,source,literal);
            source+=literal;destination+=literal;
        }
        if(source==input_end)return destination==output_end&&literal&&
            (literal>=5u||(!matched&&size<5u))?1:-1;
        if(input_end-source<2)return -1;
        uint32_t offset=ln_read_le16(source);source+=2u;if(!offset||offset>(uint32_t)(destination-out))return -1;
        uint32_t length=token&15u;
        if(length==15u){uint32_t extension;do{if(source>=input_end)return -1;extension=*source++;if(extension>size||length>size-extension)return -1;length+=extension;}while(extension==255u);}
        if(size<4u||length>size-4u)return -1;length+=4u;
        if(length>(uint32_t)(output_end-destination)||(uint32_t)(output_end-destination)<12u)return -1;matched=true;
        /* Dense RGB565 footage produces mostly four-to-eight-byte matches.
         * Copy these directly, in forward order even for offsets 1..3,
         * without entering the alignment and bulk-copy machinery. */
        if(length<=8u){
            uint8_t*write=destination;const uint8_t*match=write-offset;
            write[0]=match[0];write[1]=match[1];write[2]=match[2];write[3]=match[3];
            if(length>4u)write[4]=match[4];
            if(length>5u)write[5]=match[5];
            if(length>6u)write[6]=match[6];
            if(length>7u)write[7]=match[7];
            destination+=length;continue;
        }
        ln_video_lz_copy_long(destination,offset,length);
        destination+=length;
    }
}
#endif
static int decode_lz4(void){
    uint8_t *out=v.lz_output_slot==NONE?IPVF_LZ_SCRATCH:
        v.lz_output_slot==2u?buffers.third.bytes:(uint8_t*)frame_slot(v.lz_output_slot);
    return ln_video_lz_decode_block(record_source()+v.lz_input_offset,v.lz_input_bytes,out,v.lz_output_bytes);
}
static bool raw_room(uint32_t bytes){return bytes<=RAW_CAPACITY-v.raw_used&&v.raw_count<RAW_ENTRIES;}
static void raw_store(void){
    uint32_t n=RAW_CAPACITY-v.raw_tail;if(n>v.bytes)n=v.bytes;
    if(!v.raw_write){copy(RAW_BASE+v.raw_tail,buffers.record.bytes,n);
        copy(RAW_BASE,buffers.record.bytes+n,v.bytes-n);}
    uint32_t slot=(v.raw_first+v.raw_count)%RAW_ENTRIES;
    v.raw[slot].frame=v.next;v.raw[slot].offset=v.offset;v.raw[slot].bytes=v.bytes;
    v.raw_tail=(v.raw_tail+v.bytes)%RAW_CAPACITY;v.raw_used+=v.bytes;++v.raw_count;
}
static bool raw_load(void){
    if(!v.raw_count)return false;
    uint32_t slot=v.raw_first;
    if(v.raw[slot].frame!=v.next||v.raw[slot].offset!=v.offset||v.raw[slot].bytes!=v.bytes)return false;
    uint32_t n=RAW_CAPACITY-v.raw_head;if(n>v.bytes)n=v.bytes;
    if(n==v.bytes)v.raw_view=RAW_BASE+v.raw_head;
    else{
        copy(buffers.record.bytes,RAW_BASE+v.raw_head,n);
        copy(buffers.record.bytes+n,RAW_BASE,v.bytes-n);
        v.raw_view=buffers.record.bytes;
    }
    /* Retain the descriptor and ring bytes until all video phases finish. */
    return true;
}
static void raw_release(void){
    if(!v.raw_view)return;
    uint32_t bytes=v.raw[v.raw_first].bytes;
    v.raw_view=0;
    v.raw_head=(v.raw_head+bytes)%RAW_CAPACITY;v.raw_used-=bytes;
    v.raw_first=(v.raw_first+1u)%RAW_ENTRIES;--v.raw_count;
}
static void audio_cursor_reset(void){
    v.raw_view=0;v.raw_write=0;
    v.prefetching=false;v.audio_yield=false;v.prefetched_frames=v.next;v.audio_offset=v.offset;v.audio_sectors=v.bytes/512u;
    v.raw_head=v.raw_tail=v.raw_used=v.raw_first=v.raw_count=0;
    v.pcm_head=v.pcm_tail=0;
}
static void audio_cursor_restore(void){
    v.raw_view=0;v.raw_write=0;
    v.next=v.video_next;v.offset=v.video_offset;v.bytes=v.video_bytes_saved;
    v.prefetching=false;v.audio_yield=true;v.building=NONE;v.phase=INDEX;
}
static bool ipvf_record_header(void){
    const uint8_t*b=record_source();uint64_t used;v.source_type=b[0];v.tile_count=b[1];v.next_sectors=ln_read_le16(b+2);
    v.video_bytes=ln_read_le32(b+4);v.decoded_bytes=ln_read_le32(b+8);v.audio_bytes=ln_read_le32(b+12);
    v.sample=frame_sample(v.next);v.samples=frame_sample(v.next+1u)-v.sample;
    if(!v.samples||v.video_bytes>RECORD||v.decoded_bytes>IPVF_PAYLOAD)return false;
    uint32_t mono=4u+v.samples/2u,stereo=8u+v.samples-1u;
    if(v.audio_bytes&&v.audio_bytes!=mono&&v.audio_bytes!=stereo)return false;
    used=16u+(uint64_t)v.video_bytes+v.audio_bytes;if(used>v.bytes||((used+511u)&~511u)!=v.bytes||!ln_bytes_zero(b+(uint32_t)used,v.bytes-(uint32_t)used))return false;
    if((v.next+1u<v.count&&(!v.next_sectors||v.next_sectors>LINNET_STORAGE_MAX_READ_SECTORS))||(v.next+1u==v.count&&v.next_sectors))return false;
    if(v.offset>v.media_end||v.bytes>v.media_end-v.offset||(v.next+1u==v.count&&v.offset+v.bytes!=v.media_end))return false;
    bool compressed=v.source_type==3u||v.source_type==4u||v.source_type==5u||v.source_type==6u;
    bool temporal=v.source_type==5u||v.source_type==6u;v.type=v.source_type==0u||v.source_type==3u||temporal?0u:v.source_type==1u||v.source_type==4u?1u:2u;
    if(v.source_type>6u||(v.next==0u&&v.type!=0u)||(v.seeking&&v.next==v.seek_frame&&v.source_type!=0u&&v.source_type!=3u)||(v.type==0u&&v.tile_count)||
       (v.type==1u&&(!v.tile_count||!v.decoded_bytes))||(v.type==2u&&(v.tile_count||v.video_bytes||v.decoded_bytes))||
       (v.type==0u&&v.decoded_bytes!=FRAME_BYTES)||
       (compressed&&(!v.video_bytes||v.video_bytes>=v.decoded_bytes))||(!compressed&&v.video_bytes!=v.decoded_bytes)||
       (temporal&&(!v.temporal||v.next==0u||v.video_bytes<(v.source_type==6u?7u:5u))))return false;
    v.lz_input_offset=16u;v.lz_input_bytes=v.video_bytes;v.lz_output_bytes=v.decoded_bytes;v.lz_output_slot=NONE;v.motion_dx=v.motion_dy=0;
    if(temporal){const uint8_t*p=b+16;if(v.source_type==6u){v.motion_dx=(int8_t)p[0];v.motion_dy=(int8_t)p[1];
            if(v.motion_dx<=-(int32_t)v.width||v.motion_dx>=(int32_t)v.width||v.motion_dy<=-(int32_t)v.height||v.motion_dy>=(int32_t)v.height)return false;v.lz_input_offset+=2u;v.lz_input_bytes-=2u;p+=2;}
        uint32_t expected=ln_read_le32(p);v.lz_input_offset+=4u;v.lz_input_bytes-=4u;/* The FIFO retains immutable bytes already checked by the audio scan. */
        if(v.prefetching&&ipvf_crc(~0u,b+v.lz_input_offset,v.lz_input_bytes)!=expected)return false;}
    v.decode_at=v.decode_pixel=v.rect_row=0;v.rect_data_at=NONE;v.at=compressed?0u:1u;
    return true;
}
#if defined(__arm__)
extern void ln_video_xor_rgb565(uint16_t*,const uint8_t*,const uint16_t*,uint32_t);
#endif
static int decode_ipvf(void){
    /* Repeat records retain immutable pixels; each queue entry owns its time. */
    if(v.type==2u){if(v.previous==NONE)return -1;v.building=v.previous;return 1;}
    bool compressed=v.source_type>=3u;if(compressed&&!v.at){
        /* Full-frame records decode directly into their unpublished slot. */
        if(v.type==0u)v.lz_output_slot=v.building;
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t started=ln_time_us();
#endif
        int result=decode_lz4();
#if !defined(LINNET_PROFILE_RELEASE)
        v.lz_us+=ln_time_us()-started;
#endif
        if(result<=0)return result;v.at=1;v.decode_at=v.decode_pixel=0;}
    const uint8_t*payload=compressed?(v.type==0u?(const uint8_t*)frame_slot(v.building):IPVF_LZ_SCRATCH):record_source()+16;uint16_t*out=frame_slot(v.building),*previous=frame_slot(v.previous);
    /* Complete one retained record per decoder pass. The target-native
     * frame bound limits this work and lets the audio cursor advance
     * between records. */
    if(v.type==0u){
        /* Native pixels need no byte conversion. Full-frame LZ4 already
         * owns out; XOR may consume its residual in place. */
        typedef uint32_t pixel_pair __attribute__((may_alias));
#if !defined(__arm__)
        const pixel_pair*source=(const pixel_pair*)payload;
        pixel_pair*target=(pixel_pair*)out;
#endif
        if(v.source_type==5u||(v.source_type==6u&&!v.motion_dx&&!v.motion_dy)){
#if defined(__arm__)
            ln_video_xor_rgb565(out,payload,previous,PIXELS);
#else
            const pixel_pair*prediction=(const pixel_pair*)previous;
            for(uint32_t pair=0;pair<PIXELS/2u;++pair){uint32_t word=source[pair];
                target[pair]=prediction[pair]^word;}
#endif
        }else{
            if(payload!=(const uint8_t*)out)copy(out,payload,FRAME_BYTES);
            if(v.source_type==6u){
                /* Residual-only borders predict zero. Intersect translated
                 * source bounds once, then XOR only valid row spans. */
                uint32_t first_x=v.motion_dx>0?(uint32_t)v.motion_dx:0u;
                uint32_t last_x=v.motion_dx<0?(uint32_t)((int32_t)v.width+v.motion_dx):v.width;
                uint32_t first_y=v.motion_dy>0?(uint32_t)v.motion_dy:0u;
                uint32_t last_y=v.motion_dy<0?(uint32_t)((int32_t)v.height+v.motion_dy):v.height;
                for(uint32_t row=first_y;row<last_y;++row){
                    uint16_t*span=out+row*v.width+first_x;
                    const uint16_t*prediction=previous+(uint32_t)((int32_t)row-v.motion_dy)*v.width+
                        (uint32_t)((int32_t)first_x-v.motion_dx);
                    uint32_t width=last_x-first_x;
                    if(!(v.motion_dx&1)){
                        pixel_pair*destination=(pixel_pair*)span;
                        const pixel_pair*predicted=(const pixel_pair*)prediction;
                        for(uint32_t pair=0;pair<width/2u;++pair)destination[pair]^=predicted[pair];
                    }else for(uint32_t column=0;column<width;++column)span[column]^=prediction[column];
                }
            }
        }
        v.decode_pixel=PIXELS;return 1;
    }
    if(!v.decode_pixel){
        /* For one validated full-width rectangle, initialize only rows that
         * its conversion will leave untouched. The normal parser still runs. */
        bool full_width=v.type==1u&&v.tile_count==1u&&v.decoded_bytes>=8u&&
            !payload[0]&&(uint32_t)payload[2]==v.width&&payload[3]&&payload[1]+payload[3]<=v.height&&
            v.decoded_bytes==8u+v.width*2u*payload[3]&&ln_read_le32(payload+4u)==v.width*2u*payload[3];
        if(full_width){
            uint32_t top=payload[1],bottom=top+payload[3];
            if(top)copy(out,previous,top*v.width*2u);
            if(bottom<v.height)copy(out+bottom*v.width,previous+bottom*v.width,(v.height-bottom)*v.width*2u);
        }else copy(out,previous,FRAME_BYTES);
        v.decode_pixel=PIXELS;
    }
    while(v.tile_count){if(v.decode_at+8u>v.decoded_bytes)return -1;uint32_t x=payload[v.decode_at],y=payload[v.decode_at+1],w=payload[v.decode_at+2],h=payload[v.decode_at+3],bytes=ln_read_le32(payload+v.decode_at+4);
        if(!w||!h||x+w>v.width||y+h>v.height||bytes!=w*h*2u||bytes>v.decoded_bytes-v.decode_at-8u)return -1;
        const uint8_t*source=payload+v.decode_at+8u;
        if(w==v.width)copy(out+y*v.width,source,bytes);
        else for(uint32_t row=0;row<h;++row)
            copy(out+(y+row)*v.width+x,source+row*w*2u,w*2u);
        v.decode_at+=8u+bytes;--v.tile_count;
    }
    return v.decode_at==v.decoded_bytes?1:-1;
}
/* Frame/record buffers are overwritten before use; reset only ownership and
 * stream state when switching videos. Index-page ownership is invalidated below. */
void ln_video_init(void){clear(&v,sizeof v);v.generation=0x71000000u;v.front=v.previous=v.building=v.index_page_at=NONE;LN_VIDEO_RECORD(v.min_audio=NONE);v.preroll=2u;}
bool ln_video_open(const char*path){
    if(v.active||v.pending||ln_video_catalog_pending()||!path||path[0]!='/')return false;uint32_t n=0;while(path[n]&&n<255)n++;if(!n||n==255)return false;
    uint32_t gen=v.generation,revision=v.revision;ln_video_init();v.generation=gen;v.revision=revision+1;copy(v.path,path,n+1);
#if !defined(LINNET_PROFILE_RELEASE)
    v.measurement=armed_measurement;armed_measurement=0;
    v.measurement_ran=v.measurement!=0;v.measurement_opened=ln_time_us();
#endif
    const char*name=path;for(uint32_t i=0;i<n;i++)if(path[i]=='/')name=path+i+1;
    uint32_t i=0;while(name[i]&&i<127){v.title[i]=name[i];i++;}v.title[i]=0;
    v.active=true;v.phase=OPEN;v.epoch=v.working=1;return true;
}
bool ln_video_open_file(const struct ln_fat32_file*file,const char*title){
    if(!file||!title||!ln_video_open(title))return false;
    v.file=*file;v.phase=HEADER;return true;
}
void ln_video_stop(void){v.completed=false;if(v.active&&!v.stopping){v.stopping=true;++v.epoch;}}
bool ln_video_active(void){return v.active;}
bool ln_video_completed(void){return v.completed&&!v.active&&!v.pending&&!v.error;}
bool ln_video_pending(void){return v.pending||ln_video_catalog_pending();}
bool ln_video_playback_pending(void){return v.pending;}
bool ln_video_controls(bool show){(void)show;return true;}
bool ln_video_paused(void){return v.paused;}
void ln_video_pause(bool pause){if(!v.active||v.stopping)return;v.paused=pause;if(v.started){if(pause)ln_audio_pause();else if(!ln_audio_start())fail(13);}++v.revision;}
/* Seeking needs the same bounded cushion as startup on expensive scenes. */
static void seek_preroll(void){
    uint32_t frames=(v.fps_num+2u*v.fps_den-1u)/(2u*v.fps_den);
    if(frames<2u)frames=2u;if(frames<v.preroll)v.preroll=frames;
}
bool ln_video_seek(int32_t seconds){
#if !defined(LINNET_PROFILE_RELEASE)
    if(v.measurement)return false;
#endif
    if(!v.ready||v.stopping)return false;struct ln_audio_status a;ln_audio_snapshot(&a);int64_t frame=(int64_t)(v.started&&v.working==v.epoch?sample_frame(v.origin+a.consumed):v.target)+(int64_t)(seconds*(int32_t)v.fps_num/(int32_t)v.fps_den);
    if(frame<0)frame=0;if(frame>=v.count)frame=v.count-1;v.target=(uint32_t)frame;seek_preroll();
    v.active=true;v.completed=false;v.seeking=true;++v.epoch;return true;
}
uint8_t ln_video_options(void){return 8u;}
void ln_video_options_set(uint8_t options){(void)options;}
const char *ln_video_option_label(uint32_t row){(void)row;return "";}
bool ln_video_option_adjust(uint32_t row,int32_t direction){(void)row;(void)direction;return false;}
bool ln_video_output_prompt(void){return false;}
void ln_video_output_once(bool television){(void)television;}
void ln_video_headphones(bool connected){
    if(v.active&&video_headphone_known&&video_headphones&&!connected)ln_video_pause(true);
    video_headphones=connected;video_headphone_known=true;
}
static void step(void){
    if(!v.active)return;
#if !defined(LINNET_PROFILE_RELEASE)
    /* The one-shot starts at frame zero and stops after eight seconds of
     * playback, or eight seconds of preparation if audio never starts. */
    if(v.measurement&&ln_time_us()-(v.started?v.measurement_started:v.measurement_opened)>=8000000u)
        ln_video_stop();
#endif
    if(v.working!=v.epoch){
        if(v.pending){ln_storage_media_cancel(v.generation);if(!ln_storage_media_poll_video(v.generation,0,0))return;v.pending=false;}
        v.working=v.epoch;
        if(!ln_audio_stop()){v.error=13;v.stopping=true;--v.working;return;}
        v.pcm_head=v.pcm_tail=0;v.raw_view=0;v.raw_write=0;
        if(v.stopping){v.active=false;v.phase=IDLE;++v.revision;
#if !defined(LINNET_PROFILE_RELEASE)
            v.measurement=0;
#endif
            return;}
        v.front=v.previous=v.building=NONE;v.qhead=v.qcount=v.submitted=0;v.started=v.finish=v.offered=false;v.prefetching=false;v.prefetched_frames=0;v.seek_index=0;v.seek_limit=v.index_count;v.seek_frame=0;v.seek_offset=512;v.seek_sectors=v.first_sectors;v.index_page_at=NONE;v.phase=SEEK;++v.revision;
    }
    pump_ipvf_pcm();
    struct ln_audio_status a;ln_audio_snapshot(&a);
    if(v.started){v.resume_frame=sample_frame(v.origin+a.consumed);if(v.resume_frame>=v.count)v.resume_frame=v.count-1;LN_VIDEO_RECORD(v.audio_gaps=a.underruns;v.audio_silence=a.silence_frames;v.audio_fault=a.fault;if(a.queued<v.min_audio)v.min_audio=a.queued);if(a.fault){fail(13);return;}}
    present(&a);
    /* Hand a due frame to the renderer before starting another read/decode. */
    if(v.offered)return;
    /* High-rate dense records can fill both queues before one second of PCM. */
    if(v.ready&&!v.started&&!v.paused&&v.front!=NONE&&a.queued&&
       (a.queued+staged_pcm()>=v.rate||v.prefetched_frames==v.count||
        (v.phase==INDEX&&!v.prefetching&&!v.raw_view&&!v.raw_write&&
         (v.qcount>=FRAME_SLOTS-1u||free_slot()==NONE)&&!raw_room(v.audio_sectors*512u)))&&
       ((v.qcount+1u)>=v.preroll||(!v.prefetching&&v.next==v.count))){
        if(!ln_audio_start()){fail(13);return;}v.started=true;++v.revision;
#if !defined(LINNET_PROFILE_RELEASE)
        if(!v.measurement_started){v.measurement_started=ln_time_us();v.start_ready=v.qcount+1u;}
        v.min_ready=v.qcount;v.last_present_us=0;
#endif
    }
    int r;uint8_t*b=buffers.record.bytes;
    switch(v.phase){
    case OPEN:r=io(0,0,0,true);if(r<0)fail(1);else if(r==1)v.phase=HEADER;return;
    case HEADER:r=io(0,b,512,false);if(r<0){fail(2);return;}if(!r)return;
        if(!ipvf_header()){fail(3);return;}v.verify_at=v.index_at;v.verify_end=0;v.verify_frame=0;
        /* Frame zero's location and length come from the validated fixed
         * header, and its record is independently bounds checked. Decode it
         * before scanning the seek index so selection gets visual feedback;
         * playback and resume still wait for complete index verification. */
        v.next=0;v.offset=512;v.bytes=v.first_sectors*512u;v.origin=0;
        audio_cursor_reset();v.startup_preview=true;v.phase=INDEX;
        v.verify_crc=~0u;return;
    case VERIFY:{uint32_t end=v.file.size,n=end-v.verify_at;if(n>4096)n=4096;r=io(v.verify_at,b,n,false);if(r<0){fail(4);return;}if(!r)return;
        v.verify_crc=ipvf_crc(v.verify_crc,b,n);
        for(uint32_t at=0;at<n;at+=16u){if(!ipvf_index_record(b+at)){fail(5);return;}uint32_t frame=ln_read_le32(b+at);uint64_t offset=ln_read_le64(b+at+4);
            if((!v.verify_frame&&(frame||offset!=512u))||(v.verify_frame&&(frame<=v.verify_key||offset<=v.verify_end))){fail(5);return;}
            v.verify_key=frame;v.verify_end=(uint32_t)offset;++v.verify_frame;
        }
        v.verify_at+=n;if(v.verify_at==end){if(v.verify_crc!=v.index_crc||v.verify_frame!=v.index_count){fail(6);return;}
            v.ready=true;v.startup_preview=false;
#if !defined(LINNET_PROFILE_RELEASE)
            v.index_ready_us=ln_time_us()-v.measurement_opened;
#endif
            bool resume=saved_resume[0]==v.identity&&saved_resume[1]>0u&&saved_resume[1]<v.count;
#if !defined(LINNET_PROFILE_RELEASE)
            if(v.measurement)resume=false;
#endif
            if(resume){v.target=saved_resume[1];seek_preroll();v.seeking=true;v.seek_index=0;v.seek_limit=v.index_count;v.phase=SEEK;}
            else {if(!ln_audio_init(v.rate)){fail(13);return;}v.next=0;v.offset=512;v.bytes=v.first_sectors*512u;v.origin=0;
                audio_cursor_reset();v.phase=INDEX;}}return;}
    case SEEK:
        /* Small indexes stay sequential to avoid extra physical disk seeks. */
        if(v.index_count<=128u){
            if(v.seek_index<v.index_count){r=read_ipvf_index(v.seek_index);if(r<0){fail(7);return;}if(!r)return;if(!ipvf_index_record(b)){fail(7);return;}
                uint32_t frame=ln_read_le32(b);if(frame<=v.target){v.seek_frame=frame;v.seek_offset=(uint32_t)ln_read_le64(b+4);v.seek_sectors=ln_read_le16(b+12);++v.seek_index;return;}}
        }else
        /* The complete index was CRC checked and ordered before playback. */
        if(v.seek_index<v.seek_limit){uint32_t middle=v.seek_index+(v.seek_limit-v.seek_index)/2u;
            r=read_ipvf_index(middle);if(r<0){fail(7);return;}if(!r)return;if(!ipvf_index_record(b)){fail(7);return;}
            uint32_t frame=ln_read_le32(b);if(frame<=v.target){v.seek_frame=frame;v.seek_offset=(uint32_t)ln_read_le64(b+4);v.seek_sectors=ln_read_le16(b+12);v.seek_index=middle+1u;}
            else v.seek_limit=middle;return;}
            v.next=v.seek_frame;v.offset=v.seek_offset;v.bytes=v.seek_sectors*512u;v.resume_frame=v.target;v.origin=frame_sample(v.target);if(!ln_audio_init(v.rate)){fail(13);return;}audio_cursor_reset();v.phase=INDEX;return;
    case INDEX:
        /* Retire the previous video record before the audio producer runs. */
        raw_release();v.raw_write=0;
        /* Audio owns a forward record cursor independently of render slots.
         * Retaining every scanned record keeps storage reads strictly forward.
         * Count both software staging and the HAL toward the four-second
         * decoded cushion. Keep refilling below four seconds so audio stays
         * ahead of the decoded video queue during expensive scenes. */
        if(!v.startup_preview&&!v.prefetching&&!v.audio_yield&&v.prefetched_frames<v.count&&
           (!v.submitted||a.queued+staged_pcm()<4u*v.rate||v.next>=v.prefetched_frames)&&
           raw_room(v.audio_sectors*512u)){
            v.video_next=v.next;v.video_offset=v.offset;v.video_bytes_saved=v.bytes;
            v.next=v.prefetched_frames;v.offset=v.audio_offset;v.bytes=v.audio_sectors*512u;v.prefetching=true;
        }
        v.audio_yield=false;
        if(v.next>=v.count){if(!v.finish&&!staged_pcm()){ln_audio_finish();v.finish=true;}v.phase=WAIT;return;}
        if(!v.prefetching){if(v.qcount>=FRAME_SLOTS-1u)return;v.building=free_slot();if(v.building==NONE)return;}
        v.at=0;LN_VIDEO_RECORD(v.read_time=0;clear(v.cpu,sizeof v.cpu);v.prepare_start=ln_time_us());v.phase=READ;return;
    case READ:{if(v.prefetching){
            if(!v.pending&&!v.at&&raw_room(v.bytes)&&v.bytes<=RAW_CAPACITY-v.raw_tail)v.raw_write=RAW_BASE+v.raw_tail;
            if(v.raw_write)b=v.raw_write;
        }if(!v.prefetching&&!v.startup_preview){
            if(!raw_load()){fail(8);return;}v.at=0;v.phase=CRC;return;
        }uint32_t n=v.bytes-v.at;r=io(v.offset+v.at,b+v.at,n,false);if(r<0){fail(8);return;}if(!r)return;v.at+=n;if(v.at==v.bytes){LN_VIDEO_RECORD(if(v.read_time>v.max_read_time)v.max_read_time=v.read_time);v.at=0;v.phase=CRC;}return;}
    case CRC:{
        if(!ipvf_record_header()){fail(9);return;}
        /* The preview already decoded frame zero. The audio pass still
         * reads and validates its record, but never enqueue it twice. */
        if(!v.prefetching&&!v.startup_preview&&!v.seeking&&v.next==0u&&v.front!=NONE&&v.front_frame==0u){
            v.previous=v.front;v.offset+=v.bytes;v.bytes=v.next_sectors*512u;++v.next;v.building=NONE;v.phase=INDEX;return;
        }
        if(v.prefetching){if(!raw_room(v.bytes)){fail(8);return;}raw_store();v.at=0;v.phase=PCM;}else v.phase=DECODE;return;}
    case DECODE:
        r=decode_ipvf();if(r<0){fail(11);return;}if(r==1){v.at=0;v.phase=FRAME_READY;}return;
    case FRAME_READY:{
        v.previous=v.building;LN_VIDEO_RECORD(++v.prepared);
        LN_VIDEO_RECORD(uint32_t elapsed=ln_time_us()-v.prepare_start;if(elapsed>v.max_prepare)v.max_prepare=elapsed);
        if(v.seeking&&v.next<v.target){v.offset+=v.bytes;v.bytes=v.next_sectors*512u;++v.next;v.phase=INDEX;return;}
        v.seeking=false;v.at=0;v.phase=PCM;return;}
    case PCM:{
        if(v.startup_preview){
            uint32_t slot=(v.qhead+v.qcount)%FRAME_SLOTS;v.queue[slot]=v.building;v.queue_frame[slot]=v.next;++v.qcount;
            LN_VIDEO_RECORD(if(v.qcount>v.max_queue)v.max_queue=v.qcount);
            v.offset+=v.bytes;v.bytes=v.next_sectors*512u;++v.next;v.phase=VERIFY;return;
        }
        uint32_t sent;
        if((!v.prefetching&&v.next<v.prefetched_frames)||(v.prefetching&&v.seeking&&v.next<v.target))sent=v.samples-v.at;
        else sent=submit_ipvf_pcm();
        if(sent==NONE){fail(13);return;}v.at+=sent;if(v.prefetching&&(!v.seeking||v.next>=v.target))v.submitted+=sent;if(v.at!=v.samples)return;
        if(v.prefetching){v.offset+=v.bytes;v.bytes=v.next_sectors*512u;++v.next;v.prefetched_frames=v.next;
            v.audio_offset=v.offset;v.audio_sectors=v.next_sectors;
            struct ln_audio_status queued;ln_audio_snapshot(&queued);
            /* Once audio runs, return to video after each retained record. */
            if(v.next<v.count&&queued.queued+staged_pcm()<4u*v.rate&&raw_room(v.bytes)&&!v.started){v.phase=INDEX;return;}
            audio_cursor_restore();return;}
        uint32_t slot=(v.qhead+v.qcount)%FRAME_SLOTS;v.queue[slot]=v.building;v.queue_frame[slot]=v.next;++v.qcount;LN_VIDEO_RECORD(if(v.qcount>v.max_queue)v.max_queue=v.qcount);
        v.offset+=v.bytes;v.bytes=v.next_sectors*512u;++v.next;v.phase=INDEX;return;}
    case WAIT:if(v.started&&!v.paused&&!a.queued&&!staged_pcm()&&!v.qcount&&!v.offered){if(!ln_audio_stop()){fail(13);return;}v.resume_frame=0;v.completed=true;v.active=false;v.phase=IDLE;++v.revision;
#if !defined(LINNET_PROFILE_RELEASE)
        v.measurement=0;
#endif
        }return;
    default:return;
    }
}
void ln_video_step(void){
    ln_video_catalog_step(v.active||v.pending);
    if(v.stepping)return;v.stepping=true;uint32_t step_started=ln_time_us();
    for(uint32_t slices=0;slices<64u;slices++){
        uint32_t phase=v.phase,at=v.at,next=v.next,revision=v.revision,epoch=v.epoch;
        uint32_t decode_at=v.decode_at,decode_pixel=v.decode_pixel,tile_count=v.tile_count;
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t started=ln_time_us();
        uint32_t stage=phase==CRC?0:phase==DECODE?2u:phase==FRAME_READY?3:NONE;
#endif
        step();
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t elapsed=ln_time_us()-started;
        if(phase<=SEEK){v.phase_us[phase]+=elapsed;++v.phase_calls[phase];}
        if(phase==DECODE&&v.source_type<7u){v.type_us[v.source_type]+=elapsed;++v.type_calls[v.source_type];}
        if(stage!=NONE){v.cpu[stage]+=elapsed;if(v.cpu[stage]>v.max_cpu[stage])v.max_cpu[stage]=v.cpu[stage];}
#endif
        if(!v.active||v.pending||v.offered||ln_time_us()-step_started>=4000u)break;
        /* Decode slices advance decoder cursors without changing at.
         * They are finite progress and may use the remaining step budget. */
        if(phase==v.phase&&at==v.at&&next==v.next&&revision==v.revision&&epoch==v.epoch&&
           decode_at==v.decode_at&&decode_pixel==v.decode_pixel&&tile_count==v.tile_count)break;
    }
    LN_VIDEO_RECORD(uint32_t elapsed=ln_time_us()-step_started;if(elapsed>v.max_step)v.max_step=elapsed);
    v.stepping=false;
}
uint32_t ln_video_progress(void){if(v.completed)return 1000;if(!v.count||!v.fps)return 0;return v.resume_frame*1000u/v.count;}
const char*ln_video_title(void){return v.title;}
const char*ln_video_status(void){return v.error?"Video unavailable":v.stopping?"Stopping video":!v.active?"Finished":v.seeking?"Seeking":v.paused?"Paused":v.started?"Playing":"Loading video...";}
const struct ln_ui_asset*ln_video_asset(void){
#if !defined(LINNET_PROFILE_RELEASE)
    if(v.measurement_ran&&v.measurement!=1u)return 0;
#endif
    return v.front==NONE?0:&v.asset;
}
uint32_t ln_video_revision(void){return v.revision;}
void ln_video_presented(void){
    if(!v.offered||v.front==NONE)return;
#if !defined(LINNET_PROFILE_RELEASE)
    if(!v.presented)v.first_present_us=ln_time_us()-v.measurement_opened;
#endif
    v.offered=false;
#if !defined(LINNET_PROFILE_RELEASE)
    ++v.presented;
    if(v.started){struct ln_audio_status a;ln_audio_snapshot(&a);uint32_t clock=v.origin+a.consumed,sample=frame_sample(v.front_frame);
        if(clock>sample){uint32_t late=clock-sample;++v.late;
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t duration=frame_sample(v.front_frame+1u)-sample;
            if(late>duration)++v.late_one;if(late>2u*duration)++v.late_two;
            if(late>v.max_late){v.worst_frame=v.front_frame;v.worst_queue=v.qcount;}
#endif
            if(late>v.max_late)v.max_late=late;}
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t now=ln_time_us();if(v.last_present_us){uint32_t gap=now-v.last_present_us;if(gap>v.max_gap_us){v.max_gap_us=gap;v.gap_frame=v.front_frame;}}
        v.last_present_us=now;if(v.next<v.count&&v.qcount<v.min_ready)v.min_ready=v.qcount;
#endif
    }
#endif
}
void ln_video_resume_get(uint32_t out[2]){
#if !defined(LINNET_PROFILE_RELEASE)
    if(v.measurement_ran){out[0]=saved_resume[0];out[1]=saved_resume[1];return;}
#endif
    if(v.ready){out[0]=v.identity;out[1]=v.resume_frame;}else{out[0]=saved_resume[0];out[1]=saved_resume[1];}}
void ln_video_resume_set(const uint32_t in[2]){saved_resume[0]=in[0];saved_resume[1]=in[1];}
static bool paused_quiescent(void){
    return v.active&&v.paused&&!v.pending&&!v.stopping&&!v.seeking&&v.working==v.epoch&&
        v.phase==INDEX&&v.next<v.count&&v.front!=NONE&&!v.offered&&(v.qcount>=FRAME_SLOTS-1u||free_slot()==NONE);
}
uint32_t ln_video_deadline(uint32_t now){
    if(!v.active)return now+200000u;
    /* A settled pause with all decoded slots owned has no read/decode work to
     * advance. Input IRQs, executive deadlines and the watchdog cap still
     * bound the sleep; transition, cancel and seek states remain runnable. */
    if(paused_quiescent())return now+200000u;
    if(v.pending||v.phase==WAIT||(v.phase==INDEX&&(v.qcount>=FRAME_SLOTS-1u||free_slot()==NONE)))return now+1000u;
    return now;
}
/* submitted is decoded audio admitted to software staging for IPVF;
 * the audio HAL diagnostics retain the count actually admitted to DMA. */
#if !defined(LINNET_PROFILE_RELEASE)
void ln_video_diagnostics(uint32_t out[32]){uint32_t a[32]={0x53444956u,0,v.phase,v.error,v.active,v.pending,v.paused,v.next,v.count,v.front==NONE?NONE:v.front_frame,v.reads,v.read_bytes,v.prepared,v.presented,v.late,v.max_late,v.max_prepare,v.qcount,v.max_queue,v.audio_gaps,v.audio_silence,v.audio_fault,v.submitted,v.revision,v.max_io,v.max_read_time,v.max_cpu[0],v.max_cpu[1],v.max_cpu[2],v.max_cpu[3],v.max_step,v.min_audio};for(uint32_t i=0;i<32;i++)out[i]=a[i];}

#endif
