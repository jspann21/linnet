/* Independent, bounded BCM2722 runtime startup qualification.
 * Hardware protocol references and evidence: NATIVE_VIDEO.md.
 * No vendor firmware or third-party implementation is embedded here. */
#include "boot/include/linnet_bytes.h"
#include "targets/video5g-m25/coprocessor.h"
#include "targets/video5g-m25/lcd.h"
#include "hal/api/platform.h"
#include "hal/api/display.h"
#include "hal/api/power.h"
#include "services/storage/storage.h"
#include "services/video/mp4.h"
#include "services/audio/aac/aac.h"
#include "services/music/player.h"
#include "hal/api/audio.h"
#include "hal/api/core.h"
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#endif
#include "boot/include/sha256.h"
extern void ln_memory_copy(void *,const void *,uint32_t);
/* Native playback exclusively owns the existing media staging arena. */
#define audio_decoder (*(struct ln_aac *)(uintptr_t)LINNET_STAGE_ADDRESS)
_Static_assert(sizeof(struct ln_aac)<=0x20000u,"BCM AAC workspace overlaps packet cache");
_Static_assert(0x20000u+2u*49152u<=0x38000u,"BCM packet cache overlaps packed artwork");
#define AUDIO_PRIME_FRAMES 32768u
#define AUDIO_BUFFER_FRAMES 49152u
#define BCM_IMAGE_BYTES 201376u
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define HALF(a) (*(volatile uint16_t *)(uintptr_t)(a))
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t fault_mode,fault_deadline;
static uint32_t rom_check_us,rom_check_max_us,rom_check_calls;
static uint32_t rom_burst_max_us,rom_bursts;
static uint32_t picture_cookie;
static uint32_t picture_probe[8];
static uint32_t idle_probe[8],player_reuses,player_unloads;
static uint32_t prefetch_reads,prefetch_bytes;
static uint32_t scan_service[2][4],stream_batch[8],stream_prepare[8],file_batch[8];
static uint32_t paused_polls,paused_skips,paused_last_poll,paused_max_gap;
#endif
enum { IDLE,OPEN,READ,HASH,ACQUIRE,POWER_OFF,POWER_ON,BUS_READY,BUS_SETUP,
       BUS_LATCH,UPLOAD,VALIDATE_BEGIN,VALIDATE_WAIT,EXECUTE,READY_WAIT,
       DIRECTORY,OBSERVE,RESTORED,FAILED,ROM_CHECK,CHANNELS,VERSION_SEND,VERSION_REPLY,PLAYER_SETTLE,
       MEDIA_OPEN,MEDIA_SCAN,MEDIA_PACKET,MEDIA_FRAME,AUDIO_SCAN,AUDIO_PRIME,SYNC_WAIT,
       DISPLAY_CARD,DISPLAY_HIDE,NATIVE_IDLE,STOP_DRAIN,DISPLAY_RETURN,STOP_HANDSHAKE,PICTURE_WAIT };
static struct {
    uint32_t phase,error,restore,at,deadline,generation,requested,hash_at;
    char path[256];bool normal,paused,want_paused,seek_pending,clock_valid;uint8_t options;
    uint32_t requested_seek,clock_ms,clock_us,position_base,revision,board_revision;
    uint32_t header[4],directory[4],legacy_header[4],restore_error;
    bool io,owned,reply_stream,fs_canceling,syncing_seek,seek_forward,seek_seen,image_valid;
    const uint8_t *rom;
    uint32_t rom_bytes;
    uint32_t rom_sum,rom_expected,rom_checked,map_logical,map_physical;
    uint32_t rom_diagnostic[8];
    uint32_t channel[8][20],slot;
    uint32_t reply[256],reply_bytes,command_slot,command_index,reply_parsed;
    uint32_t host_channel[32],host_event[32],epoch,epoch_at;bool epoch_valid;
    uint32_t operation,host_request[32],file_slot,control,return_deadline,sync_deadline,start_elapsed,seek_ms,seek_stage,seek_sequence,seek_frames;
    struct {struct ln_fat32_file file;uint32_t position,resource;bool open;} files[8];
    uint32_t fs_phase,fs_fd,fs_count,fs_opcode,fs_requested,fs_bytes,fs_tx_bytes,fs_tx_at,fs_wr,fs_element_size,fs_resource;
    uint32_t prefetch_at,prefetch_requested;bool prefetch_io;
    uint32_t fs_packet[2064] __attribute__((aligned(32)));
    struct ln_mp4 movie;struct ln_mp4_packet packet;struct ln_fat32_file video_file;
    uint8_t frame[65536] __attribute__((aligned(32)));
    struct ln_resident_media cached;
    struct ln_resident_binding binding;
    uint32_t cache_resources,cache_requested,cache_unexpected;
    struct ln_mp4 soundtrack;struct ln_mp4_packet audio_packet;
    struct ln_audio_status audio_status;
    uint32_t audio_phase,audio_frames,read_kind;uint64_t audio_skip;bool audio_ready,audio_started,audio_eof;
    struct {uint64_t offset[3];uint32_t bytes[3],age[3],clock,filling;bool pending;} cache[2];
    uint64_t read_offset;void *read_buffer;uint32_t read_bytes,read_at,read_requested;
    bool read_active,read_io;
    uint32_t stream_packet[332],stream_phase,stream_at,stream_wr,stream_address,stream_capacity,stream_frames;
    bool mapped,stream_waiting,display_blanked,completed;
    bool resident,display_attempted,stopping,picture_pending;uint32_t player_task,stop_deadline,stop_events;
    bool player_retained,reuse_pending,retaining,retention_revoked;
    uint32_t retained_epoch,retain_quiet_at,retain_deadline;
    uint32_t picture_sequence,picture_polls;
    uint32_t paused_poll_at;bool paused_poll_armed;
    struct ln_fat32_file file;
    struct ln_sha256 hash;
    uint8_t *image;uint32_t image_generation;bool image_leased;
} vc;
static void display_blank(bool blanked){
    if(vc.display_blanked==blanked)return;
    vc.display_blanked=blanked;
    struct ln_power_state power;ln_power_snapshot(&power);
    ln_power_backlight(power.backlight_on,power.brightness);
}
bool ln_m25_coprocessor_display_blanked(void){return vc.display_blanked&&vc.phase!=FAILED;}
void ln_m25_coprocessor_display_presented(void){
    if(!vc.owned&&(vc.phase==RESTORED||vc.phase==FAILED||vc.phase==NATIVE_IDLE))display_blank(false);
}
/* This revision has been extracted from the device's checksum-verified rsrc.
 * Other vendor revisions need their own qualification before admission. */
static const uint8_t expected_sha[32]={
    0xcd,0xe4,0xb5,0xa9,0xc9,0xa0,0x20,0xb6,0xe2,0x3d,0xe1,0xbe,0x04,0x92,0xd9,0xa8,
    0x53,0xb7,0xd9,0x74,0xc4,0x06,0x04,0x62,0xd5,0xf5,0x7a,0x80,0xe4,0x7f,0x55,0xbe};
static void unmap_rom(void){
    if(!vc.mapped)return;
    REG(0xf000f00cu)=vc.map_physical;REG(0xf000f008u)=vc.map_logical;
    __asm__ volatile("nop\n\tnop\n\tnop":::"memory");vc.mapped=false;
}
static bool rom_image(void){
    for(uint32_t i=0;i<8u;++i)vc.rom_diagnostic[i]=0;
    /* The native ROM handoff maps SDRAM over address zero, but does not
     * establish Rockbox's later NOR alias. Never dereference the alias before
     * explicitly installing and checking its dedicated mapping window. */
    vc.map_logical=REG(0xf000f008u);vc.map_physical=REG(0xf000f00cu);
    if((vc.map_physical&0xf00u)&&vc.map_logical!=0x20003c00u)return false;
    REG(0xf000f008u)=0x20003c00u;REG(0xf000f00cu)=0x00003f84u;vc.mapped=true;
    __asm__ volatile("nop\n\tnop\n\tnop":::"memory");
    vc.rom_diagnostic[0]=REG(0xf000f008u);vc.rom_diagnostic[1]=REG(0xf000f00cu);
    /* PP5022 reads the two reserved physical bits [13:12] back as zero.
     * Check the address and access fields; retain the documented write value. */
    if(REG(0xf000f008u)!=0x20003c00u||(REG(0xf000f00cu)&~0x3000u)!=0x00000f84u){unmap_rom();return false;}
    const uint8_t *rom=(const uint8_t *)(uintptr_t)0x20000000u;
    /* Directory is the final 512 bytes of the 1 MiB NOR. Bound every entry
     * and every image extent before reading or selecting recovery firmware. */
    for(uint32_t at=0xffe00u;at+40u<=0x100000u;at+=40u){
        const uint8_t *e=rom+at;
        vc.rom_diagnostic[2]=at;
        for(uint32_t i=0;i<5u;++i)vc.rom_diagnostic[3+i]=ln_read_le32(e+i*4u);
        if(ln_read_le32(e)!=0x666c7368u)break;
        if(ln_read_le32(e+4)!=0x766d6373u)continue;
        uint32_t offset=ln_read_le32(e+12),length=ln_read_le32(e+16);
        if((offset&3u)||(length&3u)||!length||length>262144u||offset>0xffe00u||length>0xffe00u-offset)break;
        vc.rom=rom+offset;vc.rom_bytes=length;vc.rom_expected=ln_read_le32(e+28);
        vc.rom_sum=vc.rom_checked=0;return true;
    }
    unmap_rom();return false;
}
static void paused_poll_clear(void){vc.paused_poll_armed=false;vc.paused_poll_at=0;}
static void phase(uint32_t next,uint32_t timeout){paused_poll_clear();vc.phase=next;vc.deadline=ln_time_us()+timeout;}
static void native_idle(void){
    vc.owned=vc.stopping=false;phase(NATIVE_IDLE,0);
    ln_resident_end(&vc.cached);
    if(vc.image_leased&&!vc.fs_canceling){ln_player_video_firmware_release();vc.image_leased=false;}
}
static bool expired(void){return (int32_t)(ln_time_us()-vc.deadline)>=0;}
static bool startup_transfers(void){
    /* The first committed picture closes this window even for silent clips.
     * Its sequence survives seeks, so later preparation keeps normal slices. */
    return vc.normal&&!(vc.options&32u)&&!vc.picture_sequence&&!vc.audio_started&&!vc.stopping&&!vc.restore;
}
static bool put(uint32_t address,uint32_t value){return ln_m25_bcm_write(address,&value,1u);}
static uint32_t halfword(const uint32_t *words,uint32_t byte){return (words[byte/4u]>>((byte&2u)*8u))&0xffffu;}
static bool ring_valid(uint32_t lo,uint32_t hi,uint32_t at){
    return !(lo&15u)&&!(hi&15u)&&!(at&15u)&&hi>lo&&at>=lo&&at<hi&&vc.header[3]<=0xffffffffu-hi;
}
static bool ring_transfer(bool write,uint32_t lo,uint32_t hi,uint32_t *at,uint32_t *words,uint32_t bytes){
    if(!ring_valid(lo,hi,*at)||(bytes&15u)||bytes>(write?4096u:128u)||bytes>hi-lo-16u)return false;
    uint32_t first=hi-*at;if(first>bytes)first=bytes;
    if(write?!ln_m25_bcm_write(vc.header[3]+*at,words,first/4u):!ln_m25_bcm_read(vc.header[3]+*at,words,first/4u))return false;
    *at+=first;if(*at==hi)*at=lo;
    if(first<bytes){
        if(write?!ln_m25_bcm_write(vc.header[3]+*at,words+first/4u,(bytes-first)/4u):!ln_m25_bcm_read(vc.header[3]+*at,words+first/4u,(bytes-first)/4u))return false;
        *at+=bytes-first;
    }
    return true;
}
static bool notify_channel(uint32_t slot){
    uint32_t host[4],remote[4];
    if(!ln_m25_bcm_read(vc.header[3]+0x10u,host,4u)||!ln_m25_bcm_read(vc.header[3]+0x20u,remote,4u))return false;
    host[0]^=(1u<<slot)&~(host[0]^remote[0]);
    host[0]=(host[0]&~0xff00u)|(remote[0]&0xff00u);
    if(!ln_m25_bcm_write(vc.header[3]+0x10u,host,4u))return false;
    HALF(0x30030000u)=0x31u;return true;
}
#include "targets/video5g-m25/native_display.inc"
#include "targets/video5g-m25/native_resources.inc"
static int media_read(void *context,uint64_t at,void *buffer,uint32_t bytes){
    uint32_t kind=(uint32_t)(uintptr_t)context;
    /* A seek may ask for data while native library warming owns storage.
     * Leave its tuple unclaimed so file_service can drain that owner first. */
    if(vc.prefetch_io&&!vc.read_active)return 0;
    if(vc.read_active&&vc.read_kind!=kind)return 0;
    if(!vc.read_active){vc.read_kind=kind;vc.read_offset=at;vc.read_buffer=buffer;vc.read_bytes=bytes;vc.read_at=0;vc.read_active=true;}
    if(at!=vc.read_offset||buffer!=vc.read_buffer||bytes!=vc.read_bytes||at>0xffffffffu)return -1;
    if(vc.read_io){
        uint32_t transferred=0;int result=ln_storage_media_poll_deferred(vc.generation,0,&transferred);
        if(!result)return 0;vc.read_io=false;
        if(result<0||transferred!=vc.read_requested){vc.read_active=false;return -1;}
        vc.read_at+=transferred;
        if(vc.read_at==bytes){vc.read_active=false;return 1;}
    }
    uint32_t count=bytes-vc.read_at;if(count>16384u)count=16384u;
    if(vc.cached.cookie){
        int result=ln_storage_resident_read(&vc.cached,&vc.video_file,(uint32_t)at+vc.read_at,(uint8_t *)buffer+vc.read_at,count);
        if(result<0){vc.read_active=false;return -1;}
        vc.read_at+=count;if(vc.read_at==bytes){vc.read_active=false;return 1;}return 0;
    }
    int result=ln_storage_media_read(&vc.video_file,(uint32_t)at+vc.read_at,(uint8_t *)buffer+vc.read_at,count,vc.generation);
    if(result<0){vc.read_active=false;return -1;}
    if(result>0){vc.read_requested=count;vc.read_io=true;}
    return 0;
}
static int video_read(void *context,uint64_t at,void *buffer,uint32_t bytes){
    uint32_t kind=(uint32_t)(uintptr_t)context;
    if(kind>1u)return -1;
    /* Cached audio may advance while a video disk read is outstanding.
     * media_read arbitrates actual I/O; the two caches own separate memory. */
    if(bytes>16384u)return media_read(context,at,buffer,bytes);
    __typeof__(vc.cache[0]) *cache=&vc.cache[kind];
    uint8_t *base=(uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x20000u+kind*49152u);
    if(cache->pending){
        uint32_t slot=cache->filling;
        int result=media_read(context,cache->offset[slot],base+slot*16384u,cache->bytes[slot]);
        if(result<=0)return result;cache->pending=false;cache->age[slot]=++cache->clock;
    }
    if(at>vc.video_file.size||bytes>vc.video_file.size-at)return -1;
    uint32_t copied=0;
    while(copied<bytes){
        uint64_t position=at+copied;bool found=false;
        for(uint32_t i=0;i<3u;++i)if(cache->bytes[i]&&position>=cache->offset[i]&&position-cache->offset[i]<cache->bytes[i]){
            uint32_t within=(uint32_t)(position-cache->offset[i]),count=cache->bytes[i]-within;
            if(count>bytes-copied)count=bytes-copied;
            ln_memory_copy((uint8_t *)buffer+copied,base+i*16384u+within,count);
            cache->age[i]=++cache->clock;copied+=count;found=true;break;
        }
        if(!found)break;
    }
    if(copied==bytes)return 1;
    uint32_t slot=0;for(uint32_t i=1;i<3u;++i)if(cache->age[i]<cache->age[slot])slot=i;
    uint64_t position=at+copied,offset=position&~4095ull;
    if(position-offset+bytes-copied>16384u)offset=position;
    uint32_t count=vc.video_file.size-(uint32_t)offset;if(count>16384u)count=16384u;
    cache->offset[slot]=offset;cache->bytes[slot]=count;cache->filling=slot;cache->pending=true;
    return 0;
}
static bool scan_open(struct ln_mp4 *movie,uint32_t kind,int *result){
    uint32_t generation=vc.generation,initial_phase=vc.phase,started=ln_time_us(),calls=0;
    bool valid=true;
    for(;calls<8u;){
        if(vc.generation!=generation||vc.phase!=initial_phase||vc.stopping||vc.restore||vc.fs_canceling){valid=false;break;}
        bool pending=vc.read_active||vc.read_io||vc.cache[kind].pending;
        *result=ln_mp4_open_step(movie);++calls;
        /* A pending read may dispatch cancellation/recovery while draining.
         * Its old parser result must never advance the replacement owner. */
        if(vc.generation!=generation||vc.phase!=initial_phase||vc.stopping||vc.restore||vc.fs_canceling){valid=false;break;}
        if(*result!=LN_MP4_PENDING||pending||vc.read_active||vc.read_io||vc.cache[kind].pending||
           ln_time_us()-started>=250u||expired())break;
    }
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-started;scan_service[kind][0]+=elapsed;
    if(elapsed>scan_service[kind][1])scan_service[kind][1]=elapsed;
    ++scan_service[kind][2];scan_service[kind][3]+=calls;
#endif
    return valid;
}
static void seed_audio_cache(void){
    /* Both parsers validate the same file independently. Preserve completed
     * metadata windows before the first video packet can evict them; audio
     * owns a separate cache and never borrows the video cache's buffers. */
    if(vc.read_active||vc.read_io||vc.cache[0].pending||vc.cache[1].pending)return;
    for(uint32_t i=0;i<3u;++i){
        uint32_t bytes=vc.cache[0].bytes[i];uint64_t offset=vc.cache[0].offset[i];
        if(vc.cache[1].bytes[i]||bytes>16384u||offset>vc.video_file.size||
           bytes>vc.video_file.size-offset)return;
    }
    uint8_t *source=(uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x20000u);
    uint8_t *destination=source+49152u;
    for(uint32_t i=0;i<3u;++i)
        if(vc.cache[0].bytes[i])ln_memory_copy(destination+i*16384u,source+i*16384u,vc.cache[0].bytes[i]);
    /* Publish only after every copy completes. No parser, storage poll or
     * progress callback can observe a partially seeded destination. */
    vc.cache[1]=vc.cache[0];
}
/* Track timestamps can exceed 32 bits even when playback time fits in ms. */
static uint64_t divide_ticks(uint64_t ticks,uint32_t rate){
    uint64_t quotient=0,remainder=0;
    if(!rate)return UINT64_MAX;
    for(uint32_t bit=64u;bit;--bit){
        remainder=(remainder<<1)|((ticks>>(bit-1u))&1u);
        if(remainder>=rate){remainder-=rate;quotient|=(uint64_t)1u<<(bit-1u);}
    }
    return quotient;
}
static uint32_t duration_ms(uint64_t ticks,uint32_t rate){
    /* Frequent PCM position queries usually fit a single 32-bit divide. */
    if(rate&&ticks<=UINT32_MAX/1000u)return (uint32_t)ticks*1000u/rate;
    if(ticks>UINT64_MAX/1000u)return UINT32_MAX;
    uint64_t ms=divide_ticks(ticks*1000u,rate);
    return ms>UINT32_MAX?UINT32_MAX:(uint32_t)ms;
}
static uint32_t elapsed_value(const uint8_t *text,uint32_t bytes){
    for(uint32_t j=0;j+8u<bytes;++j)if(text[j]=='e'&&text[j+1]=='l'&&text[j+2]=='a'&&text[j+3]=='p'&&text[j+4]=='s'&&text[j+5]=='e'&&text[j+6]=='d'&&text[j+7]=='='){
        uint32_t value=0;for(j+=8u;j<bytes&&text[j]>='0'&&text[j]<='9';++j){if(value>429496728u)return 0;value=value*10u+text[j]-'0';}return value;
    }
    return 0;
}
static bool status_field(const uint8_t *text,uint32_t bytes,const char *field,bool prefix){
    for(uint32_t at=0;at<bytes&&text[at];++at){
        if(at&&text[at-1u]!=' ')continue;
        uint32_t n=0;while(field[n]&&at+n<bytes&&text[at+n]==(uint8_t)field[n])++n;
        if(!field[n]&&(prefix||at+n==bytes||!text[at+n]||text[at+n]==' '))return true;
    }
    return false;
}
static bool playback_running(const uint8_t *text,uint32_t bytes){
    /* A valid first or repeated picture can retain elapsed=0. */
    return status_field(text,bytes,"state=play",false)&&status_field(text,bytes,"substate=running",false)&&
           status_field(text,bytes,"paused=no",false)&&(!status_field(text,bytes,"buffering=",true)||
           status_field(text,bytes,"buffering=0",false));
}
static bool native_range(uint32_t at,uint32_t bytes){
    return !(at&3u)&&((at>=0x316a0u&&at<0x140000u&&bytes<=0x140000u-at)||
        (at>=0xfe000000u&&at<0xfe400000u&&bytes<=0xfe400000u-at));
}
static int picture_committed(void){
    /* The qualified runtime's player occupies cookie slot four. Accept its
     * full generation token before following any pointer. Every extent stays
     * within the two allocator arenas declared by the verified VMCS image. */
    if((vc.player_task&255u)!=4u)return 0;
    uint32_t context,task,before[6],after[6],descriptor[7],again[7],sequence,sequence_after,cookie;
    if(!ln_m25_bcm_read(0x2f8d0u,&context,1u))return -1;
#if !defined(LINNET_PROFILE_RELEASE)
    picture_cookie=context;
    picture_probe[0]=context;
#endif
    if(!native_range(context,0x14f8u))return 0;
    if(!ln_m25_bcm_read(context,&task,1u))return -1;
#if !defined(LINNET_PROFILE_RELEASE)
    picture_probe[1]=task;
#endif
    if(task!=vc.player_task)return 0;
    if(!ln_m25_bcm_read(context+0x13b8u,before,6u)||
       !ln_m25_bcm_read(context+0x34cu,descriptor,7u))return -1;
    uint32_t array=descriptor[1],count=descriptor[4],frame=before[3];
#if !defined(LINNET_PROFILE_RELEASE)
    picture_probe[2]=before[0];picture_probe[3]=before[1];picture_probe[4]=frame;
    picture_probe[5]=array;picture_probe[6]=count;
#endif
    if(!before[0]||before[0]!=before[1]||before[5]!=context+0x34cu||count!=4u||
       !native_range(array,256u)||frame<array||frame-array>=256u||((frame-array)&63u))return 0;
    if(!ln_m25_bcm_read(frame+8u,&sequence,1u))return -1;
#if !defined(LINNET_PROFILE_RELEASE)
    picture_probe[7]=sequence;
#endif
    /* Stream metadata below selects incoming sequence numbers. MPLAYER's
     * admitted-frame path increments that number once before displaying it.
     * Compare in the host's domain, including a one-sample movie. */
    if(sequence<=1u)return 0;
    uint32_t sent_sequence=sequence-1u;
    if(sent_sequence<=vc.seek_frames||sent_sequence>vc.stream_frames)return 0;
    /* The sequence floor rejects the old frame while update_end's caller
     * has not yet stored its new pointer. Stable snapshots also reject a
     * changing update, descriptor, record or task. This acknowledges the
     * current compositor object; it does not measure an LCD scanline. */
    if(!ln_m25_bcm_read(context+0x13b8u,after,6u)||
       !ln_m25_bcm_read(context+0x34cu,again,7u)||
       !ln_m25_bcm_read(frame+8u,&sequence_after,1u)||
       !ln_m25_bcm_read(0x2f8d0u,&cookie,1u)||!ln_m25_bcm_read(context,&task,1u))return -1;
    if(cookie!=context||task!=vc.player_task||again[1]!=array||again[4]!=count||
       sequence_after!=sequence||after[0]!=before[0]||after[1]!=before[1]||after[3]!=frame||after[5]!=before[5])return 0;
    vc.picture_sequence=sent_sequence;return 1;
}
static bool player_native_files_closed(void){
    for(uint32_t i=0;i<8u;++i)if(vc.files[i].open)return false;
    return !vc.fs_phase;
}
static bool player_files_closed(void){
    return player_native_files_closed()&&!vc.fs_canceling&&!vc.read_active&&!vc.read_io&&!vc.io;
}
/* Never follow the freed codec frame array after native stop. The surviving
 * application owns only this fixed context and its inline display state.
 * -2 means identity was lost, -1 transport failure, 0 dirty, 1 stopped. */
static int player_stopped(void){
    if((vc.player_task&255u)!=4u)return -2;
    uint32_t context,cookie,task,state,before[4],after[4],again;
    if(!ln_m25_bcm_read(0x2f8d0u,&context,1u))return -1;
    if(!native_range(context,0x14f8u))return -2;
    if(!ln_m25_bcm_read(context,&task,1u))return -1;
    if(task!=vc.player_task)return -2;
    if(!ln_m25_bcm_read(context+0x108u,&state,1u)||
       !ln_m25_bcm_read(context+0x13b8u,before,4u)||
       !ln_m25_bcm_read(context+0x13b8u,after,4u)||
       !ln_m25_bcm_read(context+0x108u,&again,1u)||
       !ln_m25_bcm_read(0x2f8d0u,&cookie,1u)||
       !ln_m25_bcm_read(context,&task,1u))return -1;
    if(cookie!=context||task!=vc.player_task)return -2;
#if !defined(LINNET_PROFILE_RELEASE)
    idle_probe[0]=context;idle_probe[1]=task;idle_probe[6]=state;
    for(uint32_t i=0;i<4u;++i)idle_probe[2u+i]=before[i];
#endif
    if((state&255u)||(again&255u))return 0;
    for(uint32_t i=0;i<4u;++i)if(before[i]||after[i])return 0;
    return 1;
}
static int player_rings_idle(void){
    uint32_t stream=0;
    while(stream<8u&&halfword(vc.channel[stream],4u)!=7u)++stream;
    if(vc.file_slot>=8u||stream==8u||stream==vc.file_slot)return -1;
    for(uint32_t i=0;i<2u;++i){
        uint32_t slot=i?stream:vc.file_slot,words[20];
        uint32_t off=halfword(vc.directory,slot*2u);
        if(!ln_m25_bcm_read(vc.header[3]+off,words,20u))return -1;
        if(halfword(words,4u)!=halfword(vc.channel[slot],4u))return -1;
        uint32_t txlo=halfword(words,6u),txhi=halfword(words,8u),txrd=halfword(words,16u),txwr=halfword(words,32u);
        uint32_t rxlo=halfword(words,10u),rxhi=halfword(words,12u),rxrd=halfword(words,48u),rxwr=halfword(words,64u);
        if(!ring_valid(txlo,txhi,txrd)||!ring_valid(txlo,txhi,txwr)||
           !ring_valid(rxlo,rxhi,rxrd)||!ring_valid(rxlo,rxhi,rxwr))return -1;
        if(txrd!=txwr||rxrd!=rxwr)return 0;
    }
    return 1;
}
static void picture_wait(void){
#if !defined(LINNET_PROFILE_RELEASE)
    for(uint32_t i=0;i<8u;++i)picture_probe[i]=0;
#endif
    vc.control=0;vc.picture_pending=true;vc.clock_valid=false;vc.picture_polls=0;vc.sync_deadline=ln_time_us()+5000000u;
    phase(PICTURE_WAIT,0);
}
static uint32_t audio_prime_frames(void){
    /* Preserve at least the existing 44.1 kHz startup duration at lower
     * rates, rounded up to a whole AAC block. Steady playback keeps its
     * larger fixed ring target; higher rates keep the original reserve. */
    uint32_t rate=vc.soundtrack.rate;
    if(!vc.normal||!rate||rate>=44100u)return AUDIO_PRIME_FRAMES;
    uint32_t frames=(AUDIO_PRIME_FRAMES*rate+44099u)/44100u;
    return (frames+1023u)&~1023u;
}
static bool audio_service(void){
    if(!vc.audio_ready||vc.audio_eof||(vc.fs_phase&&(vc.fs_phase!=4u||vc.prefetch_io))||
       (vc.seek_stage&&vc.seek_stage!=6u&&!(vc.seek_stage==1u&&vc.read_active)))return true;
    struct ln_audio_status status;ln_audio_snapshot(&status);
    if(status.fault)return false;
    /* Once the startup reserve is ready, let the native stream reach its
     * first picture before decoding extra PCM. Running playback retains its
     * normal buffer target; admitted reads still drain before yielding. */
    bool priming=vc.normal&&!vc.audio_started;
    uint32_t reserve=priming?audio_prime_frames():AUDIO_BUFFER_FRAMES;
    if(status.queued>=reserve&&(!vc.read_active||(priming&&vc.read_kind!=1u)))return true;
    int result;
    if(vc.audio_phase==1u){
        result=ln_mp4_next(&vc.soundtrack,&vc.audio_packet);
        if(result==LN_MP4_EOF){vc.audio_eof=true;ln_audio_finish();return true;}
        if(result<0)return false;
        if(result>0){if(vc.audio_packet.bytes>8191u||vc.audio_packet.duration>1024u)return false;vc.audio_phase=2u;}
    }else if(vc.audio_phase==2u){
        result=video_read((void *)1u,vc.audio_packet.offset,audio_decoder.input,vc.audio_packet.bytes);
        if(result<0)return false;if(result>0)vc.audio_phase=3u;
    }else if(vc.audio_phase==3u){
        result=ln_aac_raw_decode(&audio_decoder,audio_decoder.input,vc.audio_packet.bytes);if(result!=1)return false;
        uint64_t remaining=vc.audio_packet.dts<vc.audio_skip?vc.audio_skip-vc.audio_packet.dts:0u;
        uint32_t skip=remaining>vc.audio_packet.duration?vc.audio_packet.duration:(uint32_t)remaining;
        audio_decoder.pcm_cursor=skip;audio_decoder.pcm_frames=vc.audio_packet.duration;vc.audio_phase=4u;
    }else{
        uint32_t remaining=audio_decoder.pcm_frames-audio_decoder.pcm_cursor;
        uint32_t accepted=remaining?ln_audio_submit(audio_decoder.pcm+audio_decoder.pcm_cursor*2u,remaining):0u;
        audio_decoder.pcm_cursor+=accepted;vc.audio_frames+=accepted;
        if(audio_decoder.pcm_cursor==audio_decoder.pcm_frames)vc.audio_phase=1u;
    }
    return true;
}
static bool stream_step(uint32_t bulk_limit){
    vc.stream_waiting=false;
    if(vc.operation!=2u)return true;
    uint32_t slot=0;while(slot<8u&&halfword(vc.channel[slot],4u)!=7u)++slot;
    if(slot==8u)return false;
    uint32_t *c=vc.channel[slot],off=(vc.directory[slot/2u]>>((slot&1u)*16u))&0xffffu,record=vc.header[3]+off;
    if(vc.seek_stage){
        int result;
        if(vc.seek_stage==1u){
            if(vc.read_active||vc.fs_phase)return true;
            for(uint32_t i=0;i<2u;++i)if(vc.cache[i].pending){vc.cache[i].bytes[vc.cache[i].filling]=0;vc.cache[i].pending=false;}
            vc.movie.packet_phase=0;
            if(!ln_mp4_seek(&vc.movie,divide_ticks((uint64_t)vc.seek_ms*vc.movie.timescale,1000u),vc.seek_forward?1:-1))return false;
            if(vc.audio_ready){
                if(!ln_audio_halt()||!ln_audio_init(vc.soundtrack.rate)||ln_aac_raw_init(&audio_decoder,vc.soundtrack.config,vc.soundtrack.config_bytes)!=1)return false;
                vc.audio_phase=1u;vc.audio_frames=0;vc.audio_eof=vc.audio_started=false;
            }
            vc.seek_stage=2u;
        }else if(vc.seek_stage==2u){result=ln_mp4_seek_step(&vc.movie);if(result<0)return false;if(result>0)vc.seek_stage=3u;}
        else if(vc.seek_stage==3u){
            result=ln_mp4_next(&vc.movie,&vc.packet);if(result<0||result==LN_MP4_EOF)return false;
            if(result>0){
                vc.seek_ms=duration_ms(vc.packet.dts,vc.movie.timescale);vc.position_base=vc.seek_ms;
                if(vc.audio_ready){
                    vc.audio_skip=vc.soundtrack.lead+divide_ticks((uint64_t)vc.seek_ms*vc.soundtrack.rate,1000u);
                    vc.soundtrack.packet_phase=0;
                    if(!ln_mp4_seek(&vc.soundtrack,vc.audio_skip>1024u?vc.audio_skip-1024u:0u,0))return false;
                }
                vc.seek_stage=4u;
            }
        }
        else if(vc.seek_stage==4u){result=vc.audio_ready?ln_mp4_seek_step(&vc.soundtrack):1;if(result<0)return false;if(result>0)vc.seek_stage=5u;}
        else if(vc.seek_stage==5u){result=video_read(0,vc.packet.offset,vc.frame,vc.packet.bytes);if(result<0)return false;if(result>0)vc.seek_stage=6u;}
        else if(vc.seek_stage==6u){
            if(vc.audio_ready&&!vc.audio_eof&&vc.audio_frames<audio_prime_frames())return true;
            vc.stream_phase=2u;vc.seek_stage=0;
        }
        return true;
    }
    if(vc.stream_phase==5u||vc.stream_phase==6u){
        if(vc.fs_phase)return true;
        int result=vc.stream_phase==5u?ln_mp4_next(&vc.movie,&vc.packet):video_read(0,vc.packet.offset,vc.frame,vc.packet.bytes);
        if(result<0)return false;
        if(result==LN_MP4_EOF&&vc.stream_phase==5u){vc.stream_phase=7u;return true;}
        if(result>0)vc.stream_phase=vc.stream_phase==5u?6u:2u;
        return true;
    }
    if(vc.stream_phase==3u){
        uint32_t total=(vc.packet.bytes+15u)&~15u,count=total-vc.stream_at;
        if(total>vc.stream_capacity)return false;
        if(count>bulk_limit)count=bulk_limit;
        if(!count)return true;
        if(!ln_m25_bcm_write(vc.stream_address+vc.stream_at,(const uint32_t *)(vc.frame+vc.stream_at),count/4u))return false;
        vc.stream_at+=count;if(vc.stream_at==total)vc.stream_phase=4u;
        return true;
    }
    if(vc.stream_phase==4u){
        if(bulk_limit<64u)return true;
        if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
        uint32_t lo=halfword(c,6),hi=halfword(c,8),rd=halfword(c,16),wr=halfword(c,32);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
        uint32_t used=wr>=rd?wr-rd:hi-rd+wr-lo;
        if(hi-lo-used<80u)return true;
        if(!ring_transfer(true,lo,hi,&wr,vc.stream_packet,64u))return false;
        c[8]=(c[8]&0xffff0000u)|wr;
        if(!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(slot))return false;
        ++vc.stream_frames;vc.stream_phase=5u;return true;
    }
    if(vc.stream_phase==1u){
        if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
        uint32_t lo=halfword(c,6),hi=halfword(c,8),rd=halfword(c,16),wr=halfword(c,32);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
        uint32_t used=wr>=rd?wr-rd:hi-rd+wr-lo;
        if(sizeof vc.stream_packet>hi-lo-used-16u)return true;
        if(!vc.stream_at)vc.stream_wr=wr;
        uint32_t limit=startup_transfers()?512u:128u;
        uint32_t count=sizeof vc.stream_packet-vc.stream_at;if(count>limit)count=limit;
        if(!ring_transfer(true,lo,hi,&vc.stream_wr,vc.stream_packet+vc.stream_at/4u,count))return false;
        vc.stream_at+=count;
        if(vc.stream_at==sizeof vc.stream_packet){
            c[8]=(c[8]&0xffff0000u)|vc.stream_wr;
            if(!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(slot))return false;
            vc.stream_phase=2u;
        }
        return true;
    }
    if(!ln_m25_bcm_read(record+64u,c+16,4u))return false;
    uint32_t lo=halfword(c,10),hi=halfword(c,12),rd=halfword(c,48),wr=halfword(c,64);
    if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
    uint32_t available=wr>=rd?wr-rd:hi-rd+wr-lo;
    if(available<16u){vc.stream_waiting=true;return true;}
    uint32_t count=available>128u?128u:available;
    if(!ring_transfer(false,lo,hi,&rd,vc.host_channel,count)||vc.host_channel[0]!=0xf1a55a1fu)return false;
    if(vc.host_channel[2]==68u){
        if((vc.host_channel[3]&0xffffu)!=4u||available<32u)return false;
        if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
        uint32_t txlo=halfword(c,6),txhi=halfword(c,8),txrd=halfword(c,16),txwr=halfword(c,32);
        if(!ring_valid(txlo,txhi,txrd)||!ring_valid(txlo,txhi,txwr))return false;
        uint32_t used=txwr>=txrd?txwr-txrd:txhi-txrd+txwr-txlo;if(txhi-txlo-used<48u)return true;
        uint32_t reply[8]={0xf1a55a1fu,vc.host_channel[1],99u,8u,19u,0,0,0};
        if(!ring_transfer(true,txlo,txhi,&txwr,reply,32u))return false;c[8]=(c[8]&0xffff0000u)|txwr;
        rd=halfword(c,48)+32u;if(rd>=hi)rd=lo+rd-hi;c[12]=(c[12]&0xffff0000u)|rd;
        if(!ln_m25_bcm_write(record+48u,c+12,4u)||!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(slot))return false;
        ++vc.stop_events;return true;
    }
    if(vc.host_channel[2]==67u){
        if((vc.host_channel[3]&0xffffu)!=8u||available<32u)return false;
        vc.seek_ms=vc.host_channel[5];vc.seek_sequence=vc.host_channel[1];vc.seek_stage=1u;
        vc.seek_seen=true;vc.seek_frames=vc.stream_frames;
        rd=halfword(c,48)+32u;if(rd>=hi)rd=lo+rd-hi;c[12]=(c[12]&0xffff0000u)|rd;
        return ln_m25_bcm_write(record+48u,c+12,4u)&&notify_channel(slot);
    }
    if(vc.host_channel[2]==66u&&(vc.stream_phase==2u||vc.stream_phase==7u)){
        if((vc.host_channel[3]&0xffffu)!=12u||available<32u)return false;
        rd=halfword(c,48)+32u;if(rd>=hi)rd=lo+rd-hi;
        c[12]=(c[12]&0xffff0000u)|rd;
        if(!ln_m25_bcm_write(record+48u,c+12,4u)||!notify_channel(slot))return false;
        if(vc.stream_phase==7u){
            uint32_t end[8]={0xf1a55a1fu,vc.host_channel[1],97u,8u,0,0,0,0};
            if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
            uint32_t txlo=halfword(c,6),txhi=halfword(c,8),txrd=halfword(c,16),txwr=halfword(c,32);
            if(!ring_valid(txlo,txhi,txrd)||!ring_valid(txlo,txhi,txwr))return false;
            uint32_t used=txwr>=txrd?txwr-txrd:txhi-txrd+txwr-txlo;
            if(txhi-txlo-used<48u)return false;
            if(!ring_transfer(true,txlo,txhi,&txwr,end,32u))return false;
            c[8]=(c[8]&0xffff0000u)|txwr;
            if(!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(slot))return false;
            vc.stream_phase=8u;return true;
        }
        for(uint32_t i=0;i<16u;++i)vc.stream_packet[i]=0;
        vc.stream_packet[0]=0xf1a55a1fu;vc.stream_packet[1]=vc.host_channel[1];
        vc.stream_packet[2]=97u;vc.stream_packet[3]=48u;
        vc.stream_packet[5]=vc.packet.key?1u:0u;vc.stream_packet[6]=duration_ms(vc.packet.dts,vc.movie.timescale);
        vc.stream_packet[7]=vc.stream_frames+1u;vc.stream_packet[9]=vc.packet.bytes;
        /* Fragment bits: first=1, last=2; an entire sample carries both. */
        vc.stream_packet[10]=3u;vc.stream_packet[11]=vc.stream_address;
        for(uint32_t i=vc.packet.bytes;i<((vc.packet.bytes+15u)&~15u);++i)vc.frame[i]=0;
        vc.stream_at=0;vc.stream_phase=3u;return true;
    }
    if(vc.host_channel[2]!=65u||vc.stream_phase)return true;
    if((vc.host_channel[3]&0xffffu)!=1312u||available<1328u||count<40u)return false;
    vc.stream_address=vc.host_channel[6];vc.stream_capacity=vc.host_channel[7];
    if(!vc.stream_address||(vc.stream_address&15u)||vc.stream_capacity<vc.packet.bytes||vc.stream_capacity>0x100000u||vc.stream_address>0x10000000u-vc.stream_capacity)return false;
    rd=lo+((rd-lo+1328u-count)%(hi-lo));c[12]=(c[12]&0xffff0000u)|rd;
    if(!ln_m25_bcm_write(record+48u,c+12,4u)||!notify_channel(slot))return false;
    for(uint32_t i=0;i<332u;++i)vc.stream_packet[i]=0;
    vc.stream_packet[0]=0xf1a55a1fu;vc.stream_packet[1]=vc.host_channel[1];vc.stream_packet[2]=96u;vc.stream_packet[3]=1312u;
    uint8_t config[256];uint32_t config_bytes=vc.movie.config_bytes;
    for(uint32_t i=0;i<config_bytes;++i)config[i]=vc.movie.config[i];
    if(vc.movie.nal_bytes){
        const uint8_t *source=vc.movie.config;uint32_t at=6u,out=0,count=source[5]&31u;
        for(uint32_t group=0;group<2u;++group){
            if(group){if(at>=vc.movie.config_bytes)return false;count=source[at++];}
            for(uint32_t item=0;item<count;++item){
                if(at+2u>vc.movie.config_bytes)return false;
                uint32_t size=((uint32_t)source[at]<<8)|source[at+1u];at+=2u;
                if(!size||size>vc.movie.config_bytes-at||size+4u>sizeof config-out)return false;
                config[out++]=0;config[out++]=0;config[out++]=0;config[out++]=1;
                for(uint32_t i=0;i<size;++i)config[out++]=source[at++];
            }
        }
        config_bytes=out;
    }
    uint32_t *p=vc.stream_packet+4;p[0]=1u;p[1]=duration_ms(vc.movie.duration,vc.movie.timescale);p[2]=p[4]=p[5]=1u;
    p+=8u;p[0]=1u;p[1]=vc.movie.nal_bytes?0x48323634u:0x4d504734u;p[4]=config_bytes;
    for(uint32_t i=0;i<config_bytes;++i)p[5u+i]=config[i];
    p+=5u+config_bytes;p[0]=vc.movie.width;p[1]=vc.movie.height;p[2]=1u;/* Retail ARM 0x1cab94 preserves avcC lengthSizeMinusOne. */
    p[53]=vc.movie.nal_bytes?vc.movie.nal_bytes-1u:0u;
    vc.stream_phase=1u;vc.stream_at=0;return true;
}
/* Join one frame response, optionally after its ordinary packet step and a
 * fully cached payload copy. Additional preparation never starts storage work.
 * Initial metadata and subsequent native requests retain their own executive
 * turns. Bulk bytes exclude ring control traffic. */
/* Pure coverage probe: additional preparation never creates a read/fill. */
static bool stream_payload_cached(void){
    uint64_t at=vc.packet.offset;uint32_t bytes=vc.packet.bytes,covered=0;
    if(!bytes||bytes>16384u||at>vc.video_file.size||bytes>vc.video_file.size-at||vc.cache[0].pending)return false;
    while(covered<bytes){
        uint64_t position=at+covered;bool found=false;
        for(uint32_t i=0;i<3u;++i)if(vc.cache[0].bytes[i]&&position>=vc.cache[0].offset[i]&&
            position-vc.cache[0].offset[i]<vc.cache[0].bytes[i]){
            uint32_t count=vc.cache[0].bytes[i]-(uint32_t)(position-vc.cache[0].offset[i]);
            if(count>bytes-covered)count=bytes-covered;covered+=count;found=true;break;
        }
        if(!found)return false;
    }
    return true;
}
static bool stream_preparation_pending(void){
    return vc.read_active||vc.read_io||vc.cache[0].pending||vc.cache[1].pending||
        vc.fs_phase||vc.prefetch_io||vc.io;
}
static bool stream_batch_ready(void){
    return startup_transfers()&&!vc.seek_pending&&!vc.seek_stage&&!vc.want_paused&&
        !vc.fs_canceling&&vc.stream_phase>=2u&&vc.stream_phase<=6u;
}
static bool stream_service(void){
    if(!stream_batch_ready())return stream_step(4096u);
    uint32_t started=ln_time_us(),generation=vc.generation,owner=vc.phase;
    uint32_t steps=0,bulk=0,initial=vc.stream_phase,frames=vc.stream_frames;
    bool ok=true;
#if !defined(LINNET_PROFILE_RELEASE)
    bool time_stop=false,byte_stop=false;
#endif
    while(steps<5u){
        uint32_t previous=vc.stream_phase,at=vc.stream_at;
        bool preparation=previous==5u||previous==6u;
        bool pending=preparation&&stream_preparation_pending();
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t preparation_started=ln_time_us();
        if(previous==5u)++stream_prepare[0];
#endif
        ++steps;if(!stream_step(4096u-bulk)){ok=false;break;}
        if(vc.generation!=generation||vc.phase!=owner)break;
#if !defined(LINNET_PROFILE_RELEASE)
        if(previous==5u&&vc.stream_phase==6u)++stream_prepare[1];
        if(previous==6u&&vc.stream_phase==2u&&!pending&&!stream_preparation_pending())++stream_prepare[2];
        if(preparation&&steps>1u){
            uint32_t elapsed=ln_time_us()-preparation_started;stream_prepare[5]+=elapsed;
            if(elapsed>stream_prepare[6])stream_prepare[6]=elapsed;
        }
#endif
        /* An ordinary pending step still runs once. It cannot drain and then
         * advance another stage in this invocation, even after completion. */
        if(preparation&&(pending||stream_preparation_pending())){
#if !defined(LINNET_PROFILE_RELEASE)
            if(pending)++stream_prepare[3];
#endif
            break;
        }
        if(previous==3u){
            uint32_t sent=vc.stream_at-at;
            if(sent>4096u-bulk){ok=false;break;}bulk+=sent;
        }
        if(vc.stream_frames!=frames){
            if(previous!=4u||vc.stream_frames!=frames+1u||bulk>4032u){ok=false;break;}
            bulk+=64u;break;
        }
        if(!stream_batch_ready())break;
        if(!((previous==5u&&vc.stream_phase==6u)||(previous==6u&&vc.stream_phase==2u)||
             (previous==2u&&vc.stream_phase==3u)||(previous==3u&&vc.stream_phase==4u)))break;
        if(vc.stream_phase==4u&&bulk>4032u){
#if !defined(LINNET_PROFILE_RELEASE)
            byte_stop=true;
#endif
            break;
        }
        if(ln_time_us()-started>=250u){
#if !defined(LINNET_PROFILE_RELEASE)
            time_stop=true;
#endif
            break;
        }
        if(vc.stream_phase==6u&&(stream_preparation_pending()||!stream_payload_cached())){
#if !defined(LINNET_PROFILE_RELEASE)
            ++stream_prepare[4];
#endif
            break;
        }
    }
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-started;stream_batch[0]+=elapsed;
    if(elapsed>stream_batch[1])stream_batch[1]=elapsed;
    ++stream_batch[2];stream_batch[3]+=steps-1u;
    if(initial==2u&&vc.stream_frames==frames+1u)++stream_batch[4];
    if(bulk>stream_batch[5])stream_batch[5]=bulk;
    if(time_stop)++stream_batch[6];if(byte_stop)++stream_batch[7];
    if((initial==5u||initial==6u)&&vc.stream_frames==frames+1u)++stream_prepare[7];
#else
    (void)initial;
#endif
    return ok;
}
static uint32_t file_record(void){return vc.header[3]+((vc.directory[vc.file_slot/2u]>>((vc.file_slot&1u)*16u))&0xffffu);}
static bool service_resets(void){
    for(uint32_t slot=0;slot<8u;++slot){
        uint32_t *c=vc.channel[slot],tag=halfword(c,4u);
        if(tag!=6u&&tag!=7u)continue;
        uint32_t off=(vc.directory[slot/2u]>>((slot&1u)*16u))&0xffffu,record=vc.header[3]+off;
        if(!ln_m25_bcm_read(record+64u,c+16,4u))return false;
        uint32_t lo=halfword(c,10),hi=halfword(c,12),rd=halfword(c,48),wr=halfword(c,64);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
        uint32_t available=wr>=rd?wr-rd:hi-rd+wr-lo;
        if(available<16u)continue;
        uint32_t message[8]={0};
        if(!ring_transfer(false,lo,hi,&rd,message,16u)||message[0]!=0xf1a55a1fu)return false;
        if(tag==6u&&message[2]!=64u){
            uint32_t peek=halfword(c,48),bytes=available>128u?128u:available;
            if(!ring_transfer(false,lo,hi,&peek,vc.host_event,bytes))return false;
        }
        bool clock=tag==6u&&message[2]==70u;
        bool notification=tag==6u&&message[2]==71u&&(message[3]&0xffffu)==8u;
        if(!notification&&((message[2]!=64u&&!clock)||(message[3]&0xffffu)!=0u))continue;
        if(notification){
            if(available<32u)continue;
            if(!ring_transfer(false,lo,hi,&rd,message+4,16u))return false;
        }
        uint32_t txlo=halfword(c,6),txhi=halfword(c,8),txwr=halfword(c,32);
        if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
        uint32_t txrd=halfword(c,16);
        if(!ring_valid(txlo,txhi,txrd)||!ring_valid(txlo,txhi,txwr))return false;
        uint32_t used=txwr>=txrd?txwr-txrd:txhi-txrd+txwr-txlo;
        if(txhi-txlo-used<48u)continue;
        c[12]=(c[12]&0xffff0000u)|rd;
        if(!ln_m25_bcm_write(record+48u,c+12,4u))return false;
        message[2]=clock&&!vc.epoch_valid?1u:0u;message[3]=notification?0u:4u;
        if(clock)message[4]=vc.epoch_valid?vc.epoch+(ln_time_us()-vc.epoch_at)/1000000u:10u;
        if(!ring_transfer(true,txlo,txhi,&txwr,message,notification?16u:32u))return false;
        c[8]=(c[8]&0xffff0000u)|txwr;
        if(!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(slot))return false;
    }
    return true;
}
static void file_response(bool ok,uint32_t value,uint32_t bytes){
    vc.fs_packet[2]=ok?0u:1u;vc.fs_packet[3]=bytes;vc.fs_packet[4]=value;
    vc.fs_tx_bytes=16u+((bytes+15u)&~15u);vc.fs_tx_at=0;vc.fs_phase=3u;
}
static bool same_name(const char *a,const char *b){
    while(*a&&*b){char c=*a++;if(c>='a'&&c<='z')c-=32;if(c!=*b++)return false;}
    return !*a&&!*b;
}
static const char *codec_path(const char *name,uint32_t *resource){
    *resource=0;
    const char *base=name;
    for(const char *p=name;*p;++p)if(*p=='/'||*p=='\\')base=p+1;
    if(same_name(base,"MPLAYER")||same_name(base,"MPLAYER.VLL")){*resource=1u;return "/.LINNET/BCM/MPLAYER.VLL";}
    if(same_name(base,"H264DEC.VLL")){*resource=2u;return "/.LINNET/BCM/H264DEC.VLL";}
    if(same_name(base,"MPG4DEC.VLL")){*resource=3u;return "/.LINNET/BCM/MPG4DEC.VLL";}
    if(same_name(base,"AACDEC.VLL")){*resource=4u;return "/.LINNET/BCM/AACDEC.VLL";}
    if(same_name(base,"PASSTHRUHANDLER.VLL")){*resource=5u;return "/.LINNET/BCM/PASSTHRU.VLL";}
    return 0;
}
static void cache_libraries(void){
    if(!vc.cached.cookie)return;
    uint32_t ids[3]={1u,5u,vc.movie.nal_bytes?2u:3u};
    struct ln_fat32_file root;
    if(!resource_binding()||!ln_storage_media_root(&root))goto unavailable;
    for(uint32_t i=0;i<3u;++i){
        const struct ln_fat32_file *file=&resources.file[ids[i]-1u].file;
        if(!resource_file_matches(ids[i],file)||file->epoch!=root.epoch||file->size!=resource_layout[ids[i]-1u].bytes||
           !resource_covered(ids[i],file->epoch,0,file->size))goto unavailable;
    }
    vc.cache_resources=0;
    for(uint32_t i=0;i<3u;++i)vc.cache_resources|=1u<<ids[i];
    return;
unavailable:ln_resident_end(&vc.cached);vc.cached.failure=67u;
}
/* The VC file service owns no disk writes. Library names map to an explicit
 * local resource set; handles and offsets are bounded independently of VC. */
static bool file_step(void){
    if(vc.read_active||vc.read_io)return true;
    if(vc.file_slot>=8u)return true;
    uint32_t *c=vc.channel[vc.file_slot],record=file_record(),n=0;
    if(vc.fs_phase==4u){
        struct ln_fat32_file *file=&vc.files[vc.fs_fd].file;uint32_t id=vc.files[vc.fs_fd].resource;
        if(vc.cached.cookie||!resource_file_matches(id,file))return false;
        if(vc.prefetch_io){
            uint32_t generation=vc.generation;
            int result=ln_storage_media_poll_deferred(generation,0,&n);
            /* Progress can cancel or replace this owner before returning. */
            if(vc.generation!=generation||vc.fs_phase!=4u||vc.restore||vc.fs_canceling)return true;
            if(!result)return true;
            vc.prefetch_io=false;
            if(result<0||n!=vc.prefetch_requested||!resource_file_matches(id,file))return false;
#if !defined(LINNET_PROFILE_RELEASE)
            prefetch_bytes+=n;
#endif
            resource_publish(id,file->epoch,vc.prefetch_at,n);vc.prefetch_at+=n;
            return true; /* Give AAC a turn before admitting another chunk. */
        }
        bool warming=startup_transfers();
        if(vc.stopping||!warming||vc.prefetch_at==file->size){
            n=vc.fs_requested;uint32_t position=vc.files[vc.fs_fd].position;
            if(resource_read(id,file->epoch,position,vc.fs_packet+8,n)){
                vc.files[vc.fs_fd].position+=n;vc.fs_bytes+=n;
                for(uint32_t i=n;i<((n+15u)&~15u);++i)((uint8_t *)(vc.fs_packet+8))[i]=0;
                file_response(true,n/vc.fs_element_size,16u+n);return true;
            }
            if(!vc.stopping&&warming)return false;
            /* Finish only the original request when cancellation interrupts
             * optional warming. Native stop may need that reply to drain. */
            int result=ln_storage_media_read(file,position,vc.fs_packet+8,n,vc.generation);
            if(result!=1)return false;vc.fs_phase=2u;return true;
        }
        n=file->size-vc.prefetch_at;if(n>16384u)n=16384u;
        if(resource_covered(id,file->epoch,vc.prefetch_at,n)){vc.prefetch_at+=n;return true;}
        /* DMA may partially overwrite a canceled request. Retire all previous
         * coverage in its destination before exposing the buffer to storage. */
        if(!resource_forget_span(id,file->epoch,vc.prefetch_at,n))return false;
        int result=ln_storage_media_read(file,vc.prefetch_at,vc.image+resource_layout[id-1u].data+vc.prefetch_at,n,vc.generation);
        if(result<0)return false;if(!result)return true;
        vc.prefetch_requested=n;vc.prefetch_io=true;
#if !defined(LINNET_PROFILE_RELEASE)
        ++prefetch_reads;
#endif
        return true;
    }
    if(vc.fs_phase==1u||vc.fs_phase==2u){
        int result=ln_storage_media_poll_deferred(vc.generation,&vc.files[vc.fs_fd].file,&n);
        if(!result)return true;
        if(result<0){file_response(false,(uint32_t)-7009,4u);return true;}
        if(vc.fs_phase==1u){
            vc.files[vc.fs_fd].open=true;vc.files[vc.fs_fd].position=0;vc.files[vc.fs_fd].resource=0;
            if(resource_binding()&&vc.fs_resource&&vc.fs_resource<=5u&&vc.files[vc.fs_fd].file.epoch==resources.epoch&&vc.files[vc.fs_fd].file.size==resource_layout[vc.fs_resource-1u].bytes){
                vc.files[vc.fs_fd].resource=vc.fs_resource;
                resource_opened(vc.fs_resource,&vc.files[vc.fs_fd].file);
            }
            file_response(true,vc.fs_fd+1u,4u);
        }
        else{
            if(resource_file_matches(vc.files[vc.fs_fd].resource,&vc.files[vc.fs_fd].file))
                resource_store(vc.files[vc.fs_fd].resource,vc.files[vc.fs_fd].file.epoch,vc.files[vc.fs_fd].position,vc.fs_packet+8,n);
            vc.files[vc.fs_fd].position+=n;vc.fs_bytes+=n;
            for(uint32_t i=n;i<((n+15u)&~15u);++i)((uint8_t *)(vc.fs_packet+8))[i]=0;
            file_response(true,n/vc.fs_element_size,16u+n);
        }
        return true;
    }
    if(vc.fs_phase==3u){
        if(!ln_m25_bcm_read(record+16u,c+4,4u))return false;
        uint32_t lo=halfword(c,6),hi=halfword(c,8),rd=halfword(c,16),wr=halfword(c,32);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
        uint32_t used=wr>=rd?wr-rd:hi-rd+wr-lo;
        if(vc.fs_tx_bytes>hi-lo-16u-used)return true;
        if(!vc.fs_tx_at)vc.fs_wr=wr;
        /* Startup has no audible DMA deadline. Use the same 4 KiB bus bound
         * as video payloads, retaining small slices once audio has started.
         * The complete response remains unpublished until its final byte. */
        uint32_t limit=startup_transfers()?4096u:512u;
        n=vc.fs_tx_bytes-vc.fs_tx_at;if(n>limit)n=limit;
        if(!ring_transfer(true,lo,hi,&vc.fs_wr,vc.fs_packet+vc.fs_tx_at/4u,n))return false;
        vc.fs_tx_at+=n;
        if(vc.fs_tx_at==vc.fs_tx_bytes){
            c[8]=(c[8]&0xffff0000u)|vc.fs_wr;
            if(!ln_m25_bcm_write(record+32u,c+8,4u)||!notify_channel(vc.file_slot))return false;
            vc.fs_phase=0;
        }
        return true;
    }
    if(!ln_m25_bcm_read(record+64u,c+16,4u))return false;
    uint32_t lo=halfword(c,10),hi=halfword(c,12),rd=halfword(c,48),wr=halfword(c,64);
    if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr))return false;
    uint32_t available=wr>=rd?wr-rd:hi-rd+wr-lo;
    if(available<16u)return true;
    if(!ring_transfer(false,lo,hi,&rd,vc.fs_packet,16u)||vc.fs_packet[0]!=0xf1a55a1fu)return false;
    uint32_t length=vc.fs_packet[3]&0xffffu,padded=(length+15u)&~15u;
    if(length<4u||length>304u)return false;
    if(available<16u+padded)return true;
    for(uint32_t at=0;at<padded;at+=n){n=padded-at;if(n>128u)n=128u;if(!ring_transfer(false,lo,hi,&rd,vc.fs_packet+4u+at/4u,n))return false;}
    for(uint32_t j=0;j<32u;++j)vc.host_request[j]=j<(16u+padded)/4u?vc.fs_packet[j]:0;
    c[12]=(c[12]&0xffff0000u)|rd;
    if(!ln_m25_bcm_write(record+48u,c+12,4u)||!notify_channel(vc.file_slot))return false;
    ++vc.fs_count;vc.fs_opcode=vc.fs_packet[2];
    uint32_t fd=vc.fs_packet[4],arg=vc.fs_packet[5],arg2=vc.fs_packet[6];
    vc.fs_packet[3]=0;
    if(vc.fs_opcode==76u){
        if(length<=16u||fd!=1u){file_response(false,(uint32_t)-7003,4u);return true;}
        char *name=(char *)(vc.fs_packet+8);bool terminated=false;
        for(uint32_t j=0;j<length-16u;++j)if(!name[j]){terminated=true;break;}
        const char *path=terminated?codec_path(name,&vc.fs_resource):0;
        for(vc.fs_fd=0;vc.fs_fd<8u&&vc.files[vc.fs_fd].open;++vc.fs_fd){}
        if(!path||vc.fs_fd==8u){file_response(false,(uint32_t)-7001,4u);return true;}
        vc.cache_requested|=1u<<vc.fs_resource;
        if(vc.cached.cookie&&!(vc.cache_resources&(1u<<vc.fs_resource))){++vc.cache_unexpected;return false;}
        if(resource_binding()&&vc.fs_resource&&resources.file[vc.fs_resource-1u].ready&&resources.file[vc.fs_resource-1u].file.epoch==resources.epoch){
            vc.files[vc.fs_fd].file=resources.file[vc.fs_resource-1u].file;
            vc.files[vc.fs_fd].open=true;vc.files[vc.fs_fd].position=0;vc.files[vc.fs_fd].resource=vc.fs_resource;
            file_response(true,vc.fs_fd+1u,4u);return true;
        }
        int result=ln_storage_media_open(path,vc.generation);
        if(result!=1){file_response(false,(uint32_t)-7009,4u);return true;}
        vc.fs_phase=1u;return true;
    }
    if(!fd||fd>8u||!vc.files[fd-1u].open){file_response(false,(uint32_t)-7001,4u);return true;}
    vc.fs_fd=fd-1u;
    if(vc.fs_opcode==65u){vc.files[vc.fs_fd].open=false;file_response(true,0,4u);return true;}
    if(vc.fs_opcode==67u&&length>=12u){
        int64_t position=(int32_t)arg;
        if(arg2==1u)position+=vc.files[vc.fs_fd].position;
        else if(arg2==2u)position+=vc.files[vc.fs_fd].file.size;
        else if(arg2!=0u)position=-1;
        if(position<0||position>vc.files[vc.fs_fd].file.size)file_response(false,(uint32_t)-7002,4u);
        else{vc.files[vc.fs_fd].position=(uint32_t)position;file_response(true,(uint32_t)position,4u);}
        return true;
    }
    if(vc.fs_opcode==68u&&length>=12u){
        /* VC02 READ is fread(fd, element_size, count), unlike the later
         * VCHI service's offset/count request. Position changes use LSEEK. */
        uint32_t position=vc.files[vc.fs_fd].position;
        if(position>vc.files[vc.fs_fd].file.size||!arg||arg>8192u||arg2>8192u/arg){file_response(false,(uint32_t)-7002,4u);return true;}
        n=vc.files[vc.fs_fd].file.size-position;if(n>arg*arg2)n=arg*arg2;
        vc.fs_requested=n;vc.fs_element_size=arg;
        if(!n){vc.fs_packet[5]=vc.fs_packet[6]=vc.fs_packet[7]=0;file_response(true,0,16u);return true;}
        bool cache_file=resource_file_matches(vc.files[vc.fs_fd].resource,&vc.files[vc.fs_fd].file);
        if(cache_file&&resource_read(vc.files[vc.fs_fd].resource,vc.files[vc.fs_fd].file.epoch,position,vc.fs_packet+8,n)){
            vc.files[vc.fs_fd].position+=n;vc.fs_bytes+=n;
            for(uint32_t i=n;i<((n+15u)&~15u);++i)((uint8_t *)(vc.fs_packet+8))[i]=0;
            file_response(true,n/vc.fs_element_size,16u+n);return true;
        }
        if(vc.cached.cookie){
            vc.cached.failure=68u;return false;
        }
        if(cache_file&&startup_transfers()){
            vc.prefetch_at=vc.prefetch_requested=0;vc.prefetch_io=false;vc.fs_phase=4u;return true;
        }
        int result=ln_storage_media_read(&vc.files[vc.fs_fd].file,position,vc.fs_packet+8,n,vc.generation);
        if(result!=1){file_response(false,(uint32_t)-7009,4u);return true;}
        vc.fs_phase=2u;return true;
    }
    file_response(false,(uint32_t)-7002,4u);return true;
}
/* Additional service is limited to the first fragment of one synchronous,
 * successful request. No pending open/read/warming completion is combined. */
static bool file_batch_ready(void){
    return startup_transfers()&&vc.operation==2u&&vc.file_slot<8u&&
        !vc.seek_pending&&!vc.seek_stage&&!vc.want_paused&&!vc.fs_canceling&&
        !vc.read_active&&!vc.read_io&&!vc.io&&!vc.prefetch_io&&
        !vc.cache[0].pending&&!vc.cache[1].pending&&!ln_storage_busy()&&
        vc.image_leased&&resources.initialized&&resources.epoch;
}
static bool file_service(void){
    if(vc.fs_phase||!file_batch_ready())return file_step();
    uint32_t started=ln_time_us(),generation=vc.generation,owner=vc.phase;
    uint32_t slot=vc.file_slot,requests=vc.fs_count,epoch=resources.epoch;
    uint32_t image_generation=vc.image_generation;
    uint8_t *image=vc.image;
    bool ok=file_step();
    if(ok&&vc.generation==generation&&vc.phase==owner&&vc.file_slot==slot&&
       vc.image==image&&vc.image_generation==image_generation&&resources.epoch==epoch&&
       file_batch_ready()&&vc.fs_count==requests+1u&&vc.fs_phase==3u&&
       !vc.fs_packet[2]&&!vc.fs_tx_at){
#if !defined(LINNET_PROFILE_RELEASE)
        ++file_batch[2];
#endif
        /* Admission bound only: one existing BCM transfer can exceed it.
         * Phase 3 retains its original 4096-byte fragment/publication rules. */
        if(ln_time_us()-started<250u){
#if !defined(LINNET_PROFILE_RELEASE)
            ++file_batch[3];
#endif
            ok=file_step();
#if !defined(LINNET_PROFILE_RELEASE)
            if(vc.generation==generation&&vc.phase==owner&&vc.file_slot==slot){
                uint32_t sent=vc.fs_tx_at;
                file_batch[5]+=sent;
                if(sent>file_batch[6])file_batch[6]=sent;
                if(ok&&!vc.fs_phase)++file_batch[4];
            }
#endif
        }
#if !defined(LINNET_PROFILE_RELEASE)
        else ++file_batch[7];
#endif
    }
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-started;file_batch[0]+=elapsed;
    if(elapsed>file_batch[1])file_batch[1]=elapsed;
#endif
    return ok;
}
static void recover(uint32_t error){
    paused_poll_clear();
    if(error||(vc.normal&&!vc.picture_sequence))ln_resident_forget(&vc.binding);
    vc.picture_pending=false;
    if(!error&&vc.resident&&!vc.restore){
        if(!vc.stopping){
            vc.clock_ms=ln_m25_coprocessor_position();vc.clock_us=ln_time_us();vc.clock_valid=true;vc.paused=true;
            if(vc.audio_ready)ln_audio_pause();
            vc.stop_events=0;vc.stopping=true;vc.stop_deadline=ln_time_us()+15000000u;++vc.revision;
        }
        return;
    }
    if(vc.resident)vc.owned=true;
    vc.player_retained=vc.reuse_pending=vc.retaining=false;
    vc.retained_epoch=0;
    vc.resident=false;nd.enabled=false;
    vc.clock_ms=ln_m25_coprocessor_position();vc.clock_us=ln_time_us();vc.clock_valid=true;vc.paused=true;
    if(vc.audio_ready){ln_audio_snapshot(&vc.audio_status);if(!ln_audio_stop()&&!error)error=49u;vc.audio_ready=false;}
    ln_resident_end(&vc.cached);
    if(error)vc.image_valid=false;
    if(vc.fs_phase==1u||vc.fs_phase==2u||vc.prefetch_io||vc.read_io||vc.io){ln_storage_media_cancel(vc.generation);vc.fs_canceling=true;}
    vc.prefetch_io=false;
    vc.read_active=vc.read_io=vc.io=false;
    vc.fs_phase=0;
    if(!vc.error)vc.error=error;++vc.revision;
    if(!vc.owned){unmap_rom();phase(FAILED,0);return;}
    if(vc.restore){
        vc.restore_error=error;phase(FAILED,0);
        /* A failed NOR reload still owns the display bus. Return through the
         * platform recovery path, which resets if presentation cannot drain. */
        ln_panic(32u,0,error);
    }
    vc.restore=1u;vc.at=0;display_blank(true);REG(0x70000080u)&=~0x4000u;phase(POWER_OFF,50000u);
}
static void stop_drain(void){
    vc.clock_ms=ln_m25_coprocessor_position();vc.clock_us=ln_time_us();vc.clock_valid=true;vc.paused=true;
    if(vc.audio_ready){if(!ln_audio_stop()){recover(49u);return;}vc.audio_ready=false;}
    if(vc.fs_phase==1u||vc.fs_phase==2u||vc.prefetch_io||vc.read_io||vc.io){
        ln_storage_media_cancel(vc.generation);vc.fs_canceling=true;vc.fs_phase=0;
    }
    vc.prefetch_io=false;
    if(vc.fs_phase==4u)vc.fs_phase=0;
    vc.read_active=vc.read_io=vc.io=false;phase(STOP_DRAIN,5000000u);
}
static bool unselected_application(void){
    /* A delayed stream request is not evidence that selectplay was never
     * sent. Only these command states prove it has not been published. */
    return !vc.stream_phase&&(!vc.control||vc.control==12u||vc.control==13u)&&(vc.command_index<4u||
        (vc.command_index==4u&&vc.phase==VERSION_SEND));
}
static bool direct_file;
bool ln_m25_coprocessor_begin(uint32_t operation){
    if(vc.owned||vc.fs_canceling||(vc.resident&&nd.mode)||(vc.phase!=IDLE&&vc.phase!=FAILED&&vc.phase!=RESTORED&&vc.phase!=NATIVE_IDLE))return false;
    if(vc.resident&&operation!=2u)return false;
    if(vc.player_retained&&!player_files_closed())return false;
    bool reuse=vc.resident&&vc.player_retained;
    {
        uint32_t image_generation;
        uint8_t *image=ln_player_video_firmware_memory(&image_generation);
        if(!image)return false;
        bool changed=vc.image!=image||vc.image_generation!=image_generation;
        if(changed)vc.image_valid=false;
        vc.image=image;vc.image_generation=image_generation;vc.image_leased=true;
        resource_reset(changed);
    }
    direct_file=false;
    paused_poll_clear();
    ln_resident_end(&vc.cached);vc.cached.failure=66u;vc.cached.elapsed=vc.cached.count=0;
    vc.cache_resources=vc.cache_requested=vc.cache_unexpected=0;
    vc.prefetch_at=vc.prefetch_requested=0;vc.prefetch_io=false;
    struct ln_calendar calendar;
    vc.epoch_valid=operation!=3u&&ln_power_calendar_read(&calendar);vc.epoch_at=ln_time_us();vc.epoch=0;
    if(vc.epoch_valid){
        static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
        uint32_t years=calendar.year-2000u,total=years*365u+(years+3u)/4u+calendar.day-1u;
        for(uint32_t month=1;month<calendar.month;++month)total+=days[month-1u]+(month==2u&&!(calendar.year&3u));
        vc.epoch=946684800u+total*86400u+calendar.hour*3600u+calendar.minute*60u+calendar.second;
    }
    uint32_t info=REG(0x4001ff1cu);
    if((info&3u)||info<0x40000000u||info>0x4001ff00u-0x88u||REG(0x4001ff18u)!=0x53797349u){vc.error=51u;phase(FAILED,0);return false;}
    /* Retail display selection uses the SysCfg model/revision at +0x84;
     * the older boardHwRev field at +0x48 is zero on production M25. */
    vc.board_revision=REG(info+0x84u);
    if((vc.board_revision>>16)!=0xbu){vc.error=51u;phase(FAILED,0);return false;}
    if(!vc.mapped&&!rom_image()){vc.error=18u;phase(FAILED,0);return false;}
    vc.rom_sum=vc.rom_checked=0;
    vc.error=vc.restore_error=vc.restore=vc.at=vc.hash_at=vc.requested=0;vc.io=false;
    vc.completed=false;
    vc.normal=vc.paused=vc.want_paused=vc.seek_pending=vc.clock_valid=vc.syncing_seek=vc.seek_seen=false;vc.seek_ms=vc.position_base=vc.seek_frames=0;++vc.revision;
    vc.audio_ready=vc.audio_started=vc.audio_eof=false;vc.audio_phase=vc.audio_frames=vc.start_elapsed=0;vc.audio_status=(struct ln_audio_status){0};
    for(uint32_t i=0;i<2u;++i)vc.cache[i]=(__typeof__(vc.cache[0])){0};
    if(!vc.resident){
        for(uint32_t i=0;i<4u;++i)vc.header[i]=vc.directory[i]=vc.legacy_header[i]=0;
        for(uint32_t i=0;i<8u;++i)for(uint32_t j=0;j<20u;++j)vc.channel[i][j]=0;
        nd=(__typeof__(nd)){.slot=8u};vc.command_slot=vc.file_slot=8u;
    }
    vc.slot=0;
    vc.reply_bytes=0;vc.stopping=vc.picture_pending=vc.retaining=false;vc.stop_events=0;
    vc.reuse_pending=reuse;vc.player_retained=false;if(!reuse)vc.player_task=vc.retained_epoch=0;
    vc.picture_sequence=vc.picture_polls=0;
    vc.command_index=vc.reply_parsed=0;
    vc.operation=operation;vc.control=vc.seek_stage=0;
    vc.fs_phase=vc.fs_count=vc.fs_opcode=vc.fs_bytes=0;
    vc.stream_phase=vc.stream_at=vc.stream_address=vc.stream_capacity=vc.stream_frames=0;vc.read_active=vc.read_io=false;
    for(uint32_t i=0;i<8u;++i){vc.files[i].open=false;vc.files[i].resource=0;}
    for(uint32_t i=0;i<256u;++i)vc.reply[i]=0;
    for(uint32_t i=0;i<32u;++i)vc.host_channel[i]=vc.host_request[i]=0;
    if(++vc.generation<0x7a000000u||vc.generation>=0x7b000000u)vc.generation=0x7a000000u;
    /* Every open validates the full selected NOR extent in bounded slices. */
#if !defined(LINNET_PROFILE_RELEASE)
    rom_check_us=rom_check_max_us=rom_check_calls=0;
    paused_polls=paused_skips=paused_last_poll=paused_max_gap=0;
    rom_burst_max_us=rom_bursts=0;
    prefetch_reads=prefetch_bytes=0;
    for(uint32_t i=0;i<2u;++i)for(uint32_t j=0;j<4u;++j)scan_service[i][j]=0;
    for(uint32_t i=0;i<8u;++i)stream_batch[i]=stream_prepare[i]=file_batch[i]=0;
#endif
    phase(ROM_CHECK,30000000u);return true;
}
bool ln_m25_coprocessor_control(uint32_t command){
    if(command==6u&&vc.resident&&vc.phase==NATIVE_IDLE&&!nd.mode&&!ln_player_busy()){
        vc.operation=3u;vc.control=10u;phase(VERSION_SEND,5000000u);return true;
    }
    if(command<1u||command>5u||vc.operation!=2u||vc.phase!=PLAYER_SETTLE||vc.command_index!=8u||vc.control)return false;
    if(command>=4u){vc.seek_forward=command==4u;vc.seek_ms=command==4u?10000u:2000u;command=4u;}
    vc.control=command;vc.return_deadline=vc.deadline;phase(VERSION_SEND,5000000u);return true;
}
bool ln_m25_coprocessor_active(void){return vc.fs_canceling||(vc.phase!=IDLE&&vc.phase!=RESTORED&&vc.phase!=FAILED&&vc.phase!=NATIVE_IDLE);}
static bool paused_receives_empty(void){
    uint32_t seen=0;
    /* The completed service pass freshly validated these receive snapshots.
     * Producer changes after that pass are picked up by the bounded poll. */
    for(uint32_t slot=0;slot<8u;++slot){
        uint32_t *c=vc.channel[slot],tag=halfword(c,4u);
        if(tag<5u||tag>7u)continue;
        uint32_t lo=halfword(c,10u),hi=halfword(c,12u),rd=halfword(c,48u),wr=halfword(c,64u);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr)||rd!=wr)return false;
        seen|=1u<<(tag-5u);
    }
    return seen==7u;
}
static bool paused_service_ready(void){
    if(!vc.paused||!vc.want_paused||!vc.normal||vc.phase!=PLAYER_SETTLE||
       vc.command_index!=8u||vc.control||vc.operation!=2u||(vc.options&32u)||!vc.owned||
       vc.restore||vc.stopping||vc.fs_canceling||vc.fs_phase||vc.read_active||
       vc.seek_pending||vc.seek_stage||!vc.stream_waiting||
       vc.picture_sequence<=vc.seek_frames||vc.picture_pending||
       vc.io||vc.read_io||vc.prefetch_io||vc.cache[0].pending||vc.cache[1].pending||
       nd.mode||nd.pending||nd.visible)return false;
#if !defined(LINNET_PROFILE_RELEASE)
    if(fault_mode)return false;
#endif
    if(vc.audio_ready){
        struct ln_audio_status a;ln_audio_snapshot(&a);
        if(!vc.audio_started||a.fault||a.playing||!a.paused||
           (!vc.audio_eof&&a.queued<AUDIO_BUFFER_FRAMES))return false;
    }
    return paused_receives_empty();
}
uint32_t ln_m25_coprocessor_deadline(uint32_t now){
    if(!ln_m25_coprocessor_active())return now+200000u;
    /* Both bus power transitions only act when their 50 ms timer expires.
     * The display wait path still wakes the executive every millisecond. */
    if(vc.phase==POWER_OFF||vc.phase==POWER_ON){
        if(vc.fs_canceling)return now;
#if !defined(LINNET_PROFILE_RELEASE)
        if(fault_mode)return now;
#endif
        return (int32_t)(vc.deadline-now)>0?vc.deadline:now;
    }
    /* Pre-play preparation uses the same bounded ring polling as playback.
     * Pending data, controls and low PCM still run immediately. */
    bool startup_wait=vc.phase==PICTURE_WAIT||vc.phase==SYNC_WAIT;
    if(!vc.normal||(!startup_wait&&(vc.phase!=PLAYER_SETTLE||
       (vc.command_index!=8u&&vc.command_index!=6u)||vc.control))||
       vc.restore||vc.fs_canceling||vc.fs_phase||vc.read_active||vc.seek_pending||vc.seek_stage||
       vc.want_paused!=vc.paused||!vc.stream_waiting)return now;
#if !defined(LINNET_PROFILE_RELEASE)
    if(fault_mode)return now;
#endif
    if(vc.audio_ready){
        struct ln_audio_status a;ln_audio_snapshot(&a);
        uint32_t reserve=vc.audio_started?AUDIO_BUFFER_FRAMES:audio_prime_frames();
        if(a.fault||(!vc.audio_eof&&a.queued<reserve))return now;
    }
    if((int32_t)(vc.deadline-now)<=0)return now;
    if(vc.paused_poll_armed&&paused_service_ready()){
        if((int32_t)(vc.paused_poll_at-now)<=0)return now;
        return (int32_t)(vc.deadline-vc.paused_poll_at)<0?vc.deadline:vc.paused_poll_at;
    }
    return (int32_t)(vc.deadline-now)<1000?vc.deadline:now+1000u;
}
bool ln_m25_coprocessor_open(const char *path,uint8_t options){
    if(!path||path[0]!='/')return false;uint32_t n=0;while(path[n]&&n<255u)++n;if(!n||n==255u)return false;
    struct ln_fat32_file root,prior;
    bool bound=ln_storage_media_root(&root)&&ln_resident_match(&vc.binding,path,root.epoch,&prior);
    if(!ln_m25_coprocessor_begin(2u))return false;
    for(uint32_t i=0;i<=n;++i)vc.path[i]=path[i];vc.normal=true;vc.options=options;
    bool retained=false;
    if(bound&&!(options&32u)&&vc.resident&&vc.image_valid){
        if(ln_storage_resident_admit_files(&vc.cached,&prior,1u,false)){
            vc.video_file=vc.cached.entry[0].file;direct_file=true;
            retained=true;
        }
    }
    if(!retained)ln_resident_forget(&vc.binding);
    return true;
}
void ln_m25_coprocessor_file(const struct ln_fat32_file*file){ln_resident_end(&vc.cached);ln_resident_forget(&vc.binding);vc.video_file=*file;direct_file=true;}
bool ln_m25_coprocessor_disk_required(void){return !vc.cached.cookie;}
void ln_m25_coprocessor_stop(void){vc.completed=false;if(ln_m25_coprocessor_active()&&!vc.restore)recover(0);}
bool ln_m25_coprocessor_completed(void){return vc.completed&&!ln_m25_coprocessor_active()&&!ln_m25_coprocessor_error();}
void ln_m25_coprocessor_pause(bool paused){paused_poll_clear();vc.want_paused=paused;++vc.revision;}
bool ln_m25_coprocessor_paused(void){return vc.want_paused;}
uint32_t ln_m25_coprocessor_position(void){
    uint32_t position=vc.clock_valid?vc.clock_ms+(!vc.paused?(ln_time_us()-vc.clock_us)/1000u:0u):vc.position_base;
    if(vc.audio_ready&&vc.audio_started){
        struct ln_audio_status a;ln_audio_snapshot(&a);
        /* Audio can end before the last video frame. Once PCM is drained,
         * use the coprocessor clock so the shorter soundtrack cannot stall EOF. */
        uint32_t audio_position=vc.position_base+duration_ms(a.consumed,a.rate);
        if(!vc.audio_eof||a.queued||a.playing||audio_position>position)return audio_position;
    }
    return position;
}
bool ln_m25_coprocessor_seek(uint32_t ms){
    if(!vc.normal||!vc.movie.opened||vc.restore||!ln_m25_coprocessor_active())return false;
    uint32_t duration=duration_ms(vc.movie.duration,vc.movie.timescale);if(!duration)return false;
    paused_poll_clear();
    if(ms>=duration){vc.completed=true;recover(0);vc.clock_ms=duration;return true;}vc.requested_seek=ms;vc.seek_pending=vc.picture_pending=true;++vc.revision;return true;
}
uint32_t ln_m25_coprocessor_duration(void){return vc.movie.opened?duration_ms(vc.movie.duration,vc.movie.timescale):0u;}
uint32_t ln_m25_coprocessor_revision(void){return vc.revision;}
uint32_t ln_m25_coprocessor_error(void){return vc.error?vc.error:vc.restore_error;}
bool ln_m25_coprocessor_owned(void){return vc.owned;}
#if !defined(LINNET_PROFILE_RELEASE)
bool ln_m25_coprocessor_fault(uint32_t mode){
    if(mode<1u||mode>2u||fault_mode||!vc.owned||vc.restore||vc.phase!=PLAYER_SETTLE)return false;
    /* Defer until after the Link acknowledgement has drained. No disk writes
     * are permitted while video owns the media/display resources. */
    fault_mode=mode;fault_deadline=ln_time_us()+500000u;return true;
}
#endif
/* Returns true once the ROM display runtime is restored. Every invocation is
 * bounded; disk waits, power delays and firmware waits return to the executive. */
bool ln_m25_coprocessor_step(void){
    uint32_t value=0,n=0;int result;
    if(vc.cached.cookie&&!ln_storage_resident_valid(&vc.cached)&&!vc.restore){recover(57u);return false;}
#if !defined(LINNET_PROFILE_RELEASE)
    if(fault_mode&&(int32_t)(ln_time_us()-fault_deadline)>=0){
        uint32_t mode=fault_mode;fault_mode=0;
        if(vc.owned&&!vc.restore){
            if(vc.audio_ready)ln_audio_pause();
            if(mode==1u){
                /* Exercise the actual external-bus access with BCM clocked
                 * off. The independent COP lease handles a stalled CPU. */
                REG(0x70000080u)&=~0x4000u;
                (void)REG(0x70000080u);
                (void)ln_m25_bcm_read(0x1f0u,&value,1u);
                recover(52u);
            }else{
                recover(52u);
                recover(53u);
            }
        }
        return false;
    }
#endif
    if(vc.fs_canceling&&ln_storage_media_poll_deferred(vc.generation,0,&n))vc.fs_canceling=false;
    if(vc.phase==IDLE||vc.phase==FAILED||vc.phase==RESTORED||vc.phase==NATIVE_IDLE){
        if(vc.image_leased&&!vc.fs_canceling){ln_player_video_firmware_release();vc.image_leased=false;}
        return false;
    }
    if(vc.resident&&vc.stopping){
        if((int32_t)(ln_time_us()-vc.stop_deadline)>=0){recover(55u);return false;}
        if(!nd.mode&&vc.phase!=VERSION_REPLY&&vc.phase!=STOP_DRAIN&&vc.phase!=DISPLAY_RETURN&&vc.phase!=STOP_HANDSHAKE){
            if(!vc.player_task&&vc.control!=11u){stop_drain();return false;}
            if(vc.control!=7u&&vc.control!=8u&&vc.control!=9u&&vc.control!=11u){
                /* An application loaded before selectplay has no stream to
                 * stop: native mp_stop rejects that idle state. Unload its
                 * known task directly; selected streams keep the stop fence. */
                vc.control=unselected_application()?8u:7u;phase(VERSION_SEND,5000000u);
            }
        }
    }
    if(!vc.restore&&!vc.stopping&&vc.phase==PICTURE_WAIT&&(int32_t)(ln_time_us()-vc.sync_deadline)>=0){
        recover(50u);return false;
    }
    if(vc.phase==STOP_HANDSHAKE){
        if(!service_resets()||!file_service()||!audio_service()||!stream_service()){recover(36u);return false;}
        if(vc.control==7u){
            if(vc.stop_events){vc.control=9u;phase(VERSION_SEND,5000000u);}
            else if(expired())recover(55u);
        }else if(expired())phase(VERSION_SEND,5000000u);
        return false;
    }
    if(vc.phase==STOP_DRAIN){
        if(vc.fs_canceling)return false;
        if(!service_resets()||!file_service()){recover(36u);return false;}
        if(vc.fs_phase)return false;
        if(vc.retaining){
            int stopped=player_stopped(),rings=player_rings_idle();
            if(stopped<0||rings<0){recover(54u);return false;}
            struct ln_fat32_file root;
            bool quiet=stopped&&rings&&player_files_closed()&&ln_storage_media_root(&root)&&root.epoch;
#if !defined(LINNET_PROFILE_RELEASE)
            idle_probe[7]=quiet;
#endif
            uint32_t now=ln_time_us();
            if(!quiet)vc.retain_quiet_at=0;
            else if(!vc.retain_quiet_at)vc.retain_quiet_at=now;
            else if(now-vc.retain_quiet_at>=10000u){
                vc.player_retained=true;vc.retained_epoch=root.epoch;vc.retaining=false;
            }
            if(vc.retaining){
                if((int32_t)(now-vc.retain_deadline)>=0){
                    vc.retaining=false;vc.control=8u;phase(VERSION_SEND,5000000u);
                }
                return false;
            }
        }
        if(nd.visible){native_idle();++vc.revision;return true;}
        if(!native_display_begin(4u)){recover(54u);return false;}
        phase(DISPLAY_RETURN,2000000u);return false;
    }
    if(vc.phase==DISPLAY_CARD||vc.phase==DISPLAY_HIDE||vc.phase==DISPLAY_RETURN){
        if(vc.phase==DISPLAY_HIDE&&(!service_resets()||!file_service()||!audio_service()||!stream_service())){recover(36u);return false;}
        result=native_display_service();
        if(result<0){recover(54u);return false;}
        if(result>0){
            if(vc.phase==DISPLAY_CARD&&vc.operation==3u){
                vc.resident=true;native_idle();return true;
            }else if(vc.phase==DISPLAY_RETURN){native_idle();++vc.revision;return true;}
            else if(vc.phase==DISPLAY_CARD){display_blank(false);vc.command_index=0;phase(VERSION_SEND,5000000u);}
            else{++vc.revision;phase(PLAYER_SETTLE,500000u);}
        }
        return false;
    }
    if(vc.phase==ROM_CHECK){
        if(expired()){recover(16);return false;}
        uint32_t burst_started=ln_time_us();
        /* Keep the full fresh NOR proof. Batch only its short synchronous
         * slices; no other startup phase runs before returning to the executive. */
        for(uint32_t slices=0;slices<4u&&vc.rom_checked<vc.rom_bytes;++slices){
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t started=ln_time_us();
#endif
            n=vc.rom_bytes-vc.rom_checked;if(n>512u)n=512u;
            /* Directory admission and each slice preserve word alignment. The
             * restore uploader already reads this NOR mapping as 32-bit words;
             * sum every byte lane while retaining the full fresh checksum scan. */
            const volatile uint32_t *words=(const volatile uint32_t *)(vc.rom+vc.rom_checked);
            uint32_t sum=vc.rom_sum;
            for(uint32_t i=0;i<n/4u;++i){uint32_t word=words[i];
                sum+=(word&255u)+((word>>8)&255u)+((word>>16)&255u)+(word>>24);
            }
            vc.rom_sum=sum;
            vc.rom_checked+=n;
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t elapsed=ln_time_us()-started;rom_check_us+=elapsed;++rom_check_calls;
            if(elapsed>rom_check_max_us)rom_check_max_us=elapsed;
#endif
            if(ln_time_us()-burst_started>=250u)break;
        }
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t burst_elapsed=ln_time_us()-burst_started;++rom_bursts;
        if(burst_elapsed>rom_burst_max_us)rom_burst_max_us=burst_elapsed;
#endif
        if(vc.rom_checked==vc.rom_bytes){
            if(vc.rom_sum!=vc.rom_expected)recover(17);
            else if(vc.resident)phase(MEDIA_OPEN,60000000u);
            else if(vc.image_valid)phase(vc.operation==2u?MEDIA_OPEN:ACQUIRE,
                                         vc.operation==2u?60000000u:1000000u);
            else{vc.image_valid=false;phase(OPEN,60000000u);}
        }
        return false;
    }
    if(vc.phase==OPEN||vc.phase==READ){
        if(vc.io){
            result=ln_storage_media_poll_deferred(vc.generation,&vc.file,&n);
            if(!result){if(expired())ln_storage_media_cancel(vc.generation);return false;}
            vc.io=false;
            if(result<0){recover(0x100u+(uint32_t)-result);return false;}
            if(vc.phase==OPEN){
                if(vc.file.size!=BCM_IMAGE_BYTES){recover(2);return false;}
                phase(READ,60000000u);
            }else{
                if(n!=vc.requested){recover(3);return false;}
                vc.at+=n;
                if(vc.at==BCM_IMAGE_BYTES){ln_sha256_init(&vc.hash);phase(HASH,5000000u);}
            }
            return false;
        }
        if(expired()){recover(4);return false;}
        if(vc.phase==OPEN)result=ln_storage_media_open("/.LINNET/BCM/VMCS.BIN",vc.generation);
        else{
            n=BCM_IMAGE_BYTES-vc.at;if(n>16384u)n=16384u;vc.requested=n;
            result=ln_storage_media_read(&vc.file,vc.at,vc.image+vc.at,n,vc.generation);
        }
        if(result<0)recover(5);else if(result>0)vc.io=true;
        return false;
    }
    if(vc.phase==HASH){
        n=BCM_IMAGE_BYTES-vc.hash_at;if(n>2048u)n=2048u;
        ln_sha256_update(&vc.hash,vc.image+vc.hash_at,n);vc.hash_at+=n;
        if(vc.hash_at==BCM_IMAGE_BYTES){
            uint8_t digest[32];ln_sha256_finish(&vc.hash,digest);
            for(uint32_t i=0;i<32u;++i)if(digest[i]!=expected_sha[i]){recover(6);return false;}
            vc.image_valid=true;
            phase(vc.operation==2u?MEDIA_OPEN:ACQUIRE,vc.operation==2u?60000000u:1000000u);
        }
        return false;
    }
    if(vc.phase==ACQUIRE){
        if(ln_display_busy()){if(expired())recover(7);return false;}
        if(vc.resident){vc.owned=true;vc.command_index=0;phase(VERSION_SEND,5000000u);return false;}
        vc.owned=true;vc.at=0;display_blank(true);REG(0x70000080u)&=~0x4000u;phase(POWER_OFF,50000u);return false;
    }
    if(vc.phase==MEDIA_OPEN){
        if(direct_file){direct_file=false;ln_mp4_init(&vc.movie,video_read,0,vc.video_file.size);phase(MEDIA_SCAN,60000000u);return false;}
        if(expired()){recover(41);return false;}
        if(vc.io){
            result=ln_storage_media_poll_deferred(vc.generation,&vc.video_file,&n);
            if(!result)return false;vc.io=false;
            if(result<0){recover(40);return false;}
            if(vc.normal)(void)ln_resident_remember(&vc.binding,vc.path,&vc.video_file);
            ln_mp4_init(&vc.movie,video_read,0,vc.video_file.size);phase(MEDIA_SCAN,60000000u);
        }else{
            result=ln_storage_media_open(vc.normal?vc.path:"/Videos/MOTION.mp4",vc.generation);
            if(result<0)recover(40);else if(result>0)vc.io=true;
        }
        return false;
    }
    if(vc.phase==MEDIA_SCAN||vc.phase==MEDIA_PACKET||vc.phase==MEDIA_FRAME){
        if(expired()){recover(41);return false;}
        if(vc.phase==MEDIA_SCAN){
            if(!scan_open(&vc.movie,0u,&result))return false;
            if(result<0)recover(0x300u+(uint32_t)-result);
            else if(result>0){
                if(vc.movie.config_bytes>256u||vc.movie.width>640u||vc.movie.height>480u||
                   (vc.movie.nal_bytes&&(vc.movie.config[1]!=66u||vc.movie.config[3]>13u||vc.movie.width>320u||vc.movie.height>240u)))recover(42);
                else {cache_libraries();seed_audio_cache();phase(MEDIA_PACKET,30000000u);}
            }
        }else if(vc.phase==MEDIA_PACKET){
            result=ln_mp4_next(&vc.movie,&vc.packet);
            if(result<0||result==LN_MP4_EOF)recover(43);else if(result>0)phase(MEDIA_FRAME,30000000u);
        }else{
            result=video_read(0,vc.packet.offset,vc.frame,vc.packet.bytes);
            if(result<0)recover(44);else if(result>0){ln_mp4_init_audio(&vc.soundtrack,video_read,(void *)1u,vc.video_file.size);phase(AUDIO_SCAN,60000000u);}
        }
        return false;
    }
    if(vc.phase==AUDIO_SCAN){
        if(expired()){recover(47);return false;}
        if(!scan_open(&vc.soundtrack,1u,&result))return false;
        if(result==LN_MP4_EOF)phase(ACQUIRE,1000000u);
        else if(result<0)recover(0x400u+(uint32_t)-result);
        else if(result>0){
            if(ln_aac_raw_init(&audio_decoder,vc.soundtrack.config,vc.soundtrack.config_bytes)!=1||audio_decoder.rate!=vc.soundtrack.rate||!ln_audio_init(audio_decoder.rate)){recover(48);return false;}
            vc.audio_ready=true;vc.audio_phase=1u;vc.audio_skip=vc.soundtrack.lead;
            /* Normal playback primes PCM while the native runtime starts.
             * Command 6 still gates mp_play on the complete audio reserve. */
            phase(vc.normal?ACQUIRE:AUDIO_PRIME,vc.normal?1000000u:30000000u);
        }
        return false;
    }
    if(vc.phase==AUDIO_PRIME){
        if(expired()||!audio_service()){recover(49);return false;}
        if(vc.audio_frames>=audio_prime_frames()||vc.audio_eof)phase(ACQUIRE,1000000u);
        return false;
    }
    if(vc.phase==POWER_OFF){
        if(expired()){REG(0x70000080u)|=0x4000u;phase(POWER_ON,50000u);}return false;
    }
    if(vc.phase==POWER_ON){
        if(expired()){
            REG(0x70000008u)&=~0xf00u;REG(0x70000040u)=0x1313u;
            phase(BUS_READY,1000000u);
        }
        return false;
    }
    if(vc.phase==SYNC_WAIT){
        /* Keep PCM and native stream requests moving between status polls. */
        if(!service_resets()){recover(36);return false;}
        if(!file_service()){recover(30);return false;}
        if(!audio_service()||!stream_service()){recover(37);return false;}
        if(expired())phase(VERSION_SEND,5000000u);
        return false;
    }
    if(vc.phase==PICTURE_WAIT){
        if(!service_resets()||!file_service()||!audio_service()||!stream_service()){recover(36u);return false;}
        if(vc.seek_pending||vc.seek_stage){phase(PLAYER_SETTLE,500000u);return false;}
        if(expired()){
            ++vc.picture_polls;int committed=picture_committed();
            if(committed<0){recover(54u);return false;}
            if(committed){
                /* Finish the requested picture even if Pause arrived during
                 * a seek. Pausing the decoder first leaves the old picture
                 * displayed and defers all seek decoding until the next Play.
                 * Keep PCM stopped; PLAYER_SETTLE pauses native playback as
                 * soon as the new compositor frame has been acknowledged. */
                if(!vc.want_paused){
                    if(vc.audio_ready&&!ln_audio_start()){recover(49u);return false;}
                    vc.audio_started=vc.audio_ready;
                }
                vc.picture_pending=false;vc.syncing_seek=false;vc.clock_us=ln_time_us();vc.clock_valid=true;
                if(nd.visible){
                    if(!native_display_begin(3u)){recover(54u);return false;}
                    phase(DISPLAY_HIDE,2000000u);
                }else{++vc.revision;phase(PLAYER_SETTLE,500000u);}
            }else phase(PICTURE_WAIT,10000u);
        }
        return false;
    }
    if(vc.phase==PLAYER_SETTLE){
        uint32_t now=ln_time_us(),generation=vc.generation;
        bool quiet=paused_service_ready();
        if(vc.paused_poll_armed&&quiet&&(int32_t)(vc.paused_poll_at-now)>0&&
           (int32_t)(vc.deadline-now)>0){
#if !defined(LINNET_PROFILE_RELEASE)
            ++paused_skips;
#endif
            return false;
        }
#if !defined(LINNET_PROFILE_RELEASE)
        if(quiet){
            if(vc.paused_poll_armed&&paused_last_poll){
                uint32_t gap=now-paused_last_poll;if(gap>paused_max_gap)paused_max_gap=gap;
            }
            ++paused_polls;paused_last_poll=now;
        }
#endif
        paused_poll_clear();
        if(!service_resets()||!file_service()||!audio_service()||!stream_service()){recover(36);return false;}
        if(vc.normal&&vc.command_index==6u){
            int disk=vc.cached.cookie?1:ln_storage_media_realtime(true);
            if(disk<0){recover(49u);return false;}
            if(!disk){if(expired())recover(49u);return false;}
            /* mp_region has acknowledged completion. Wait for useful work,
             * not an unconditional two seconds with the display hidden. */
            if(!vc.seek_stage&&(!vc.audio_ready||vc.audio_frames>=audio_prime_frames()||vc.audio_eof)){
                ++vc.command_index;phase(VERSION_SEND,5000000u);
            }else if(expired())recover(49);
        }else if(vc.normal&&vc.command_index==8u){
            uint32_t command=0;
            if(vc.seek_pending){
                vc.seek_forward=vc.requested_seek>=ln_m25_coprocessor_position();vc.seek_ms=vc.requested_seek;vc.seek_pending=false;command=4u;
            }else if(vc.picture_pending&&!vc.paused&&!vc.seek_stage){command=6u;vc.sync_deadline=ln_time_us()+15000000u;}
            else if(vc.want_paused!=vc.paused)command=vc.want_paused?1u:2u;
            else if(expired()){
                uint32_t duration=duration_ms(vc.movie.duration,vc.movie.timescale);
                struct ln_audio_status audio={0};
                if(vc.audio_ready)ln_audio_snapshot(&audio);
                /* The video completion tolerance must not discard the final
                 * PCM block. Wait for the soundtrack and DMA queue to drain. */
                if(vc.stream_phase==8u&&ln_m25_coprocessor_position()+80u>=duration&&
                   (!vc.audio_ready||(vc.audio_eof&&!audio.queued&&!audio.playing))){vc.completed=true;recover(0);return false;}
                command=3u;
            }
            if(command){vc.control=command;vc.return_deadline=vc.deadline;phase(VERSION_SEND,5000000u);}
        }else if(expired()){++vc.command_index;phase(VERSION_SEND,5000000u);}
        if(generation==vc.generation&&paused_service_ready()){
            now=ln_time_us();
            if((int32_t)(vc.deadline-now)>0){
                vc.paused_poll_at=(int32_t)(vc.deadline-now)<10000?vc.deadline:now+10000u;
                vc.paused_poll_armed=true;
            }
        }
        return false;
    }
    if(expired()){recover(vc.phase==OBSERVE?0u:0x200u+vc.phase);return false;}
    switch(vc.phase){
    case BUS_READY:
        value=HALF(0x30070000u);
        if(!(value&0x80u)&&(value&0x40u))phase(BUS_SETUP,1000000u);
        break;
    case BUS_SETUP:{
        static const uint8_t controls[]={0xa1,0x81,0x91,0x02,0x12,0x22,0x72,0x62};
        for(uint32_t i=0;i<8u;++i)HALF(0x30030000u)=controls[i];
        for(uint32_t i=3;i<8u;++i)HALF(0x30070000u)=controls[i];
        phase(BUS_LATCH,1000000u);break;}
    case BUS_LATCH:
        if((HALF(0x30020000u)&1u)&&(HALF(0x30060000u)&1u)){
            (void)HALF(0x30010000u);(void)HALF(0x30050000u);
            value=HALF(0x30070000u);
            if(!(value&0x80u)&&(value&0x40u))phase(UPLOAD,5000000u);
        }
        break;
    case UPLOAD:{
        const uint8_t *source=vc.restore?vc.rom:vc.image;
        uint32_t bytes=vc.restore?vc.rom_bytes:BCM_IMAGE_BYTES;
        n=bytes-vc.at;if(n>4096u)n=4096u;
        if(!ln_m25_bcm_write(vc.at,(const uint32_t *)(source+vc.at),n/4u)){recover(8);break;}
        vc.at+=n;if(vc.at==bytes){
            phase(VALIDATE_BEGIN,1000000u);
        }break;}
    case VALIDATE_BEGIN:
        /* RetailOS at 0x265518 selects model 1 for family 0xb revisions
         * below 0x10, and model 2 for revisions from 0x10 onward. */
        if(!vc.restore&&!put(0x1f4u,(vc.board_revision&0xffffu)>=0x10u?2u:1u)){recover(9);break;}
        if(!put(0x1f8u,0)||!put(0x10000c00u,0xc0000000u)){recover(9);break;}
        phase(VALIDATE_WAIT,2000000u);break;
    case VALIDATE_WAIT:
        if(!ln_m25_bcm_read(0x10000c00u,&value,1u)){recover(10);break;}
        if(value&1u)phase(EXECUTE,1000000u);break;
    case EXECUTE:
        if(!put(0x10000c00u,0)||!put(0x10000400u,0xa5a50002u)){recover(11);break;}
        phase(READY_WAIT,5000000u);break;
    case READY_WAIT:{
        uint32_t *header=vc.restore?vc.legacy_header:vc.header;
        if(!ln_m25_bcm_read(0x1f0u,header,4u)){recover(12);break;}
        if(header[2]){
            if(vc.restore){vc.owned=false;unmap_rom();phase(RESTORED,0);return true;}
            if(header[2]!=1u||!header[3]||(header[3]&3u)||header[3]>0xffffffefu){recover(13);break;}
            phase(DIRECTORY,1000000u);
        }
        break;}
    case DIRECTORY:
        if(!ln_m25_bcm_read(vc.header[3],vc.directory,4u)){recover(14);break;}
        phase(CHANNELS,1000000u);break;
    case CHANNELS:{
        uint32_t offset=(vc.directory[vc.slot/2u]>>((vc.slot&1u)*16u))&0xffffu;
        if(offset){
            if((offset&15u)||vc.header[3]>0xffffffffu-offset-80u){recover(19);break;}
            if(!ln_m25_bcm_read(vc.header[3]+offset,vc.channel[vc.slot],20u)){recover(20);break;}
            if(halfword(vc.channel[vc.slot],4u)==1u)vc.command_slot=vc.slot;
            if(halfword(vc.channel[vc.slot],4u)==5u)vc.file_slot=vc.slot;
            if(halfword(vc.channel[vc.slot],4u)==2u)nd.slot=vc.slot;
        }
        if(++vc.slot==8u){if(vc.normal||vc.operation==3u)vc.command_index=1u;phase(VERSION_SEND,5000000u);}
        break;}
    case VERSION_SEND:{
        if(!service_resets()){recover(36);break;}
        if(vc.command_slot>=8u){recover(21);break;}
        uint32_t *c=vc.channel[vc.command_slot];
        uint32_t offset=(vc.directory[vc.command_slot/2u]>>((vc.command_slot&1u)*16u))&0xffffu;
        if(!ln_m25_bcm_read(vc.header[3]+offset+16u,c+4,4u)){recover(25);break;}
        uint32_t lo=halfword(c,6),hi=halfword(c,8),rd=halfword(c,16),wr=halfword(c,32);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr)||rd!=wr||hi-lo<48u){recover(22);break;}
        static const char *const native_commands[]={"load_application mplayer","display_control 0 model=2 power=2","display_control 0 freeze=0","vmcs_display_enable","mp_selectplay passthru:linnet 0","mp_pause","mp_region display=0 dest=\"0 0 320 240\" mode=fill","mp_play","mp_get_status","mp_get_stats video","pm_set_policy min"};
        static const char *const tv_encoding[]={"display_control 2 dac=1 encoding=0","display_control 2 dac=1 encoding=1"};
        static const char *const tv_shape[]={"display_control 2 svideo=0 wide=0","display_control 2 svideo=0 wide=1","display_control 2 svideo=1 wide=0","display_control 2 svideo=1 wide=1"};
        static const char *const lcd_region="mp_region display=0 dest=\"0 0 320 240\" mode=fill";
        char seek_command[32]="mp_seek ";
        if(vc.control==4u){vc.seek_seen=false;uint32_t n=vc.seek_ms,at=8u,divisor=1000000000u;while(divisor>1u&&n<divisor)divisor/=10u;do{seek_command[at++]=(char)('0'+n/divisor);n%=divisor;divisor/=10u;}while(divisor);seek_command[at]=0;}
        if(vc.control==8u||vc.control==12u){
            /* A retained token predates this open. Validate its generation
             * again before unloading, including cancellation before status. */
            if(vc.retained_epoch&&player_stopped()<0){recover(54u);break;}
            static const char prefix[]="end_application ";uint32_t at=0;
            while(prefix[at]){seek_command[at]=prefix[at];++at;}
            uint32_t n=vc.player_task,divisor=1000000000u;while(divisor>1u&&n<divisor)divisor/=10u;
            do{seek_command[at++]=(char)('0'+n/divisor);n%=divisor;divisor/=10u;}while(divisor);seek_command[at]=0;
        }
        bool tv=(vc.options&32u)!=0,wide=(vc.options&8u)!=0,svideo=(vc.options&16u)!=0,pal=(vc.options&4u)!=0;
        const char *native=vc.command_index==1u?tv?tv_encoding[pal?1u:0u]:(vc.board_revision&0xffffu)<0x10u?"display_control 0 model=1 power=2":native_commands[1u]:vc.command_index==2u&&tv?tv_shape[(svideo?2u:0u)|(wide?1u:0u)]:vc.command_index==6u?lcd_region:native_commands[vc.command_index];
        if(vc.reuse_pending&&!vc.control&&!vc.command_index)vc.control=13u;
        const char *command=vc.control==4u||vc.control==8u||vc.control==12u?seek_command:vc.control==7u?"mp_stop":vc.control==10u?"pm_get_status":vc.control==11u?"display_control 2 dac=0":vc.control?(vc.control==1u?"mp_pause":vc.control==2u?"mp_play":"mp_get_status"):vc.operation==2u||vc.operation==3u?native:!vc.operation?"version":vc.command_index==0u?"load_application mplayer":vc.command_index==2u?"mp_selectplay passthru:linnet 0":"mp_get_status";
        uint32_t message[32]={0xf1a55a1fu,vc.command_index+1u,1u,0},length=0;
        while(command[length]){((uint8_t *)(message+4))[length]=(uint8_t)command[length];++length;}
        message[3]=length+1u;length=16u+((length+16u)&~15u);
        if(!ring_transfer(true,lo,hi,&wr,message,length)){recover(23);break;}
        c[8]=(c[8]&0xffff0000u)|wr;
        if(!ln_m25_bcm_write(vc.header[3]+offset+32u,c+8,4u)||!notify_channel(vc.command_slot)){recover(24);break;}
        vc.reply_bytes=vc.reply_parsed=0;
        vc.reply_stream=false;
        for(uint32_t i=0;i<256u;++i)vc.reply[i]=0;
        phase(VERSION_REPLY,vc.operation?120000000u:5000000u);break;}
    case VERSION_REPLY:{
        if(!service_resets()){recover(36);break;}
        if(!file_service()){recover(30);break;}
        if(!audio_service()||!stream_service()){recover(37);break;}
        /* Retain an unconsumed host-request message while qualifying the
         * player stream contract. Unsupported requests never receive success. */
        for(uint32_t slot=0;slot<8u;++slot)if(halfword(vc.channel[slot],4u)==7u){
            uint32_t *h=vc.channel[slot],off=(vc.directory[slot/2u]>>((slot&1u)*16u))&0xffffu;
            if(!ln_m25_bcm_read(vc.header[3]+off+64u,h+16,4u)){recover(31);break;}
            uint32_t lo=halfword(h,10),hi=halfword(h,12),rd=halfword(h,48),wr=halfword(h,64);
            if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr)){recover(32);break;}
            uint32_t bytes=wr>=rd?wr-rd:hi-rd+wr-lo;
            if(bytes){if(bytes>128u)bytes=128u;if(!ring_transfer(false,lo,hi,&rd,vc.host_channel,bytes)){recover(33);break;}}
        }
        if(vc.phase!=VERSION_REPLY)break;
        uint32_t *c=vc.channel[vc.command_slot];
        uint32_t offset=(vc.directory[vc.command_slot/2u]>>((vc.command_slot&1u)*16u))&0xffffu;
        if(!ln_m25_bcm_read(vc.header[3]+offset+64u,c+16,4u)){recover(25);break;}
        uint32_t lo=halfword(c,10),hi=halfword(c,12),rd=halfword(c,48),wr=halfword(c,64);
        if(!ring_valid(lo,hi,rd)||!ring_valid(lo,hi,wr)){recover(26);break;}
        uint32_t available=wr>=rd?wr-rd:hi-rd+wr-lo;
        if(available<16u)break;
        if(available>128u)available=128u;
        if(available>sizeof vc.reply-vc.reply_bytes){recover(34);break;}
        if(!ring_transfer(false,lo,hi,&rd,vc.reply+vc.reply_bytes/4u,available)){recover(27);break;}
        vc.reply_bytes+=available;
        c[12]=(c[12]&0xffff0000u)|rd;
        if(!ln_m25_bcm_write(vc.header[3]+offset+48u,c+12,4u)||!notify_channel(vc.command_slot)){recover(29);break;}
        while(vc.reply_bytes-vc.reply_parsed>=16u){
            uint32_t *message=vc.reply+vc.reply_parsed/4u,bytes=message[3]&0xffffu;
            if(message[0]!=0xf1a55a1fu||message[1]!=vc.command_index+1u){recover(28);break;}
            bool stream=bytes==0xffffu;
            if(vc.reply_stream){
                /* After the NUL-terminated stream, the final 16-byte header
                 * reports completion; its length is not another payload. */
                if(message[2]>1u){recover(28);break;}bytes=0;
            }else if(stream){
                const uint8_t *text=(const uint8_t *)(message+4);bytes=0;
                uint32_t remaining=vc.reply_bytes-vc.reply_parsed-16u;
                while(bytes<remaining&&text[bytes])++bytes;
                if(bytes==remaining)break;
                ++bytes;
            }
            uint32_t total=16u+((bytes+15u)&~15u);
            if(total>vc.reply_bytes-vc.reply_parsed)break;
            vc.reply_parsed+=total;
            const uint8_t *response=(const uint8_t *)(message+4);
            if(vc.operation==2u&&!vc.command_index&&!vc.control){
                for(uint32_t i=0;i+5u<bytes;++i)if(response[i]=='t'&&response[i+1u]=='a'&&response[i+2u]=='s'&&response[i+3u]=='k'&&response[i+4u]=='='){
                    uint32_t task=0,j=i+5u;
                    while(j<bytes&&response[j]>='0'&&response[j]<='9'&&task<1000000u){task=task*10u+response[j++]-'0';}
                    if(j>i+5u&&task<1000000u)vc.player_task=task;
                }
            }
            if(bytes>=7u&&response[0]=='e'&&response[1]=='r'&&response[2]=='r'&&
               response[3]=='o'&&response[4]=='r'&&response[5]=='='&&
               response[6]>='1'&&response[6]<='9'){recover(35);break;}
            if(stream)vc.reply_stream=true;
            if(message[2]<=1u){
                /* This read-only native query returns one with its status
                 * text on the qualified VMCS revision. Mutating commands
                 * still require zero; generic error replies were rejected
                 * above. Never infer or change policy from these values. */
                bool power_status=vc.control==10u&&status_field(response,bytes,"core=",true)&&
                    status_field(response,bytes,"free=",true)&&status_field(response,bytes,"overrun=",true);
                if(message[2]&&!power_status){recover(35);break;}
                if(vc.resident&&vc.operation==2u&&!vc.command_index&&!vc.control&&!vc.player_task){recover(55u);break;}
                if(vc.control==7u){
                    if(vc.stop_events){vc.control=9u;phase(VERSION_SEND,5000000u);}
                    else phase(STOP_HANDSHAKE,5000000u);
                    break;
                }
                if(vc.control==9u){
                    if(vc.stop_events&&status_field(response,bytes,"state=idle",false)){
                        if(vc.normal&&!(vc.options&32u)){
                            vc.control=0;vc.retaining=true;vc.retain_quiet_at=0;
                            vc.retain_deadline=ln_time_us()+250000u;stop_drain();
                        }else{vc.control=8u;phase(VERSION_SEND,5000000u);}
                    }
                    else phase(STOP_HANDSHAKE,10000u);
                    break;
                }
                if(vc.control==10u){vc.control=0;native_idle();break;}
                if(vc.control==11u){vc.control=0;stop_drain();break;}
                if(vc.control==8u){
                    vc.player_task=vc.retained_epoch=0;vc.player_retained=vc.reuse_pending=vc.retaining=vc.retention_revoked=false;
#if !defined(LINNET_PROFILE_RELEASE)
                    if(player_unloads!=UINT32_MAX)++player_unloads;
#endif
                    if(vc.options&32u){vc.control=11u;phase(VERSION_SEND,5000000u);}
                    else{vc.control=0;stop_drain();}break;
                }
                if(vc.control==12u){
                    vc.player_task=vc.retained_epoch=0;vc.player_retained=vc.reuse_pending=vc.retaining=vc.retention_revoked=false;
#if !defined(LINNET_PROFILE_RELEASE)
                    if(player_unloads!=UINT32_MAX)++player_unloads;
#endif
                    vc.control=0;
                    if(vc.stopping)stop_drain();else phase(VERSION_SEND,5000000u);
                    break;
                }
                if(vc.resident&&vc.stopping){
                    if(vc.player_task){vc.control=unselected_application()?8u:7u;phase(VERSION_SEND,5000000u);}else stop_drain();
                    break;
                }
                if(vc.control==13u){
                    int stopped=player_stopped(),rings=player_rings_idle();
                    if(stopped<0||rings<0){recover(54u);break;}
                    struct ln_fat32_file root;
                    bool bound=ln_storage_media_root(&root)&&root.epoch==vc.retained_epoch;
                    bool reusable=status_field(response,bytes,"state=idle",false)&&stopped&&rings&&
                        player_native_files_closed()&&bound&&!vc.retention_revoked&&!(vc.options&32u);
                    vc.reuse_pending=false;
                    if(reusable){
#if !defined(LINNET_PROFILE_RELEASE)
                        if(player_reuses!=UINT32_MAX)++player_reuses;
#endif
                        vc.control=0;vc.command_index=4u;
                    }else vc.control=12u;
                    phase(VERSION_SEND,5000000u);break;
                }
                if(vc.control){
                    uint32_t position=ln_m25_coprocessor_position();
                    bool defer_picture=vc.control==6u&&vc.normal&&(nd.visible||vc.picture_pending||vc.syncing_seek||vc.seek_pending||vc.seek_stage);
                    if(vc.control==1u){vc.paused=true;if(vc.audio_ready)ln_audio_pause();}
                    if(vc.control==4u&&vc.paused){
                        /* Queue the seek before releasing native pause, so
                         * an old near-EOF stream cannot drain before rewind. */
                        vc.syncing_seek=true;vc.picture_pending=true;
                        vc.control=2u;phase(VERSION_SEND,5000000u);break;
                    }
                    if(vc.control==2u||vc.control==4u){
                        if(vc.control==4u)vc.syncing_seek=true;
                        if(vc.syncing_seek)vc.picture_pending=true;
                        vc.control=6u;vc.sync_deadline=ln_time_us()+15000000u;phase(SYNC_WAIT,10000u);break;
                    }
                    if(vc.control==6u){
                        uint32_t elapsed=elapsed_value(response,bytes);
                        bool waiting=!playback_running(response,bytes)||
                            (vc.syncing_seek&&(!vc.seek_seen||vc.seek_stage||vc.stream_frames==vc.seek_frames));
                        if(waiting){if((int32_t)(ln_time_us()-vc.sync_deadline)>=0){recover(50);break;}phase(SYNC_WAIT,10000u);break;}
                        vc.start_elapsed=elapsed;vc.paused=false;
                        if(!defer_picture){
                            if(vc.audio_ready&&!ln_audio_start()){recover(49);break;}vc.audio_started=vc.audio_ready;
                        }
                        display_blank(false);
                    }
                    uint32_t elapsed=elapsed_value(response,bytes);
                    if(vc.control==6u&&vc.syncing_seek)elapsed=vc.seek_ms;
                    else if(vc.clock_valid&&elapsed<position)elapsed=position;
                    /* Status can retain the last changed picture's timestamp.
                     * Only an explicit seek may move playback time backward. */
                    vc.clock_ms=elapsed;vc.clock_us=ln_time_us();vc.clock_valid=!defer_picture&&!vc.picture_pending;
                    bool picture=defer_picture&&!vc.seek_pending&&!vc.seek_stage;
                    vc.control=0;
                    if(picture)picture_wait();
                    else{paused_poll_clear();vc.phase=PLAYER_SETTLE;vc.deadline=vc.normal?ln_time_us()+500000u:vc.return_deadline;++vc.revision;}break;
                }
                if(vc.operation==2u&&vc.command_index==7u)vc.sync_deadline=ln_time_us()+5000000u;
                if(vc.operation==2u&&vc.command_index==8u){
                    uint32_t elapsed=elapsed_value(response,bytes);
                    if(!playback_running(response,bytes)){if((int32_t)(ln_time_us()-vc.sync_deadline)>=0){recover(50);break;}phase(SYNC_WAIT,10000u);break;}
                    bool defer_picture=vc.normal&&(nd.visible||vc.picture_pending||vc.seek_pending||vc.seek_stage);
                    vc.start_elapsed=elapsed;vc.clock_ms=elapsed;vc.clock_us=ln_time_us();vc.clock_valid=!defer_picture;
                    if(!defer_picture){
                        if(vc.audio_ready&&!ln_audio_start()){recover(49);break;}vc.audio_started=vc.audio_ready;
                    }
                    if(!vc.seek_pending&&!vc.seek_stage)display_blank(false);
                    if(vc.normal&&nd.visible&&!vc.seek_pending&&!vc.seek_stage){
                        picture_wait();
                    }else{++vc.revision;phase(PLAYER_SETTLE,vc.normal?500000u:30000000u);}
                }else if(vc.operation==3u&&vc.command_index==3u){vc.command_index=10u;phase(VERSION_SEND,5000000u);
                }else if((vc.normal&&!vc.resident&&vc.command_index==3u)||(vc.operation==3u&&vc.command_index==10u)){
                    if(!native_display_begin(1u)){recover(54u);break;}phase(DISPLAY_CARD,2000000u);
                }else if(vc.resident&&vc.normal&&(vc.options&32u)&&vc.command_index==2u){
                    /* vmcs_display_enable initializes one legacy display
                     * object and rejects a second call. Resident boot already
                     * initialized it; TV only needs encoding and shape here. */
                    vc.command_index=4u;phase(VERSION_SEND,5000000u);
                }else if(vc.normal&&vc.command_index==0u){vc.retention_revoked=false;vc.command_index=vc.resident&&(vc.options&32u)?1u:4u;phase(VERSION_SEND,5000000u);
                }else if(vc.operation==2u&&vc.command_index==6u)phase(PLAYER_SETTLE,vc.normal?30000000u:2000000u);
                else if(vc.operation==1u&&vc.command_index==2u)phase(PLAYER_SETTLE,2000000u);
                else if(vc.operation&&vc.command_index<(vc.operation==2u?8u:vc.operation==3u?3u:2u)){++vc.command_index;phase(VERSION_SEND,5000000u);}
                else phase(OBSERVE,1000000u);
                break;
            }
        }
        break;}
    case OBSERVE:if(!file_service()||!audio_service()||!stream_service())recover(30);break;
    default:recover(15);break;
    }
    return false;
}
bool ln_m25_coprocessor_prepare_display(void){
    if(!vc.display_attempted){
        if(ln_storage_busy())return false;
        if(!ln_m25_coprocessor_begin(3u))return vc.phase==FAILED;
        vc.display_attempted=true;
    }
    (void)ln_m25_coprocessor_step();
    return !ln_m25_coprocessor_active();
}
void ln_m25_coprocessor_storage_invalidated(void){
    ln_resident_forget(&vc.binding);
    vc.retention_revoked=true;
    resources.initialized=false;resources.epoch=0;vc.image_valid=false;
    for(uint32_t i=0;i<5u;++i)resources.file[i].ready=false;
    /* Recovery can reuse FAT's mount-local epoch. An active media task must
     * release its old handles rather than continue through that reused value. */
    if(ln_m25_coprocessor_active()&&!vc.restore)recover(56u);
}
uint32_t ln_m25_coprocessor_display_address(void){return vc.resident&&nd.enabled?nd.address:0;}
bool ln_m25_coprocessor_display_prepare(void){
    if(!vc.resident||!nd.enabled||vc.owned)return false;
    return nd.mode?nd.mode==2u:native_display_begin(2u);
}
bool ln_m25_coprocessor_display_ready(bool *ready){
    if(!ready||!vc.resident||!nd.enabled)return false;
    if(!nd.mode){*ready=true;return true;}
    if(!service_resets()){recover(36u);return false;}
    /* Allow short BCM replies to arrive inside this bounded service turn.
     * Returning on the first empty read inserts an entire audio decode turn
     * between each surface RPC. Scanout still yields after 250 us. */
    uint32_t started=ln_time_us();
    for(uint32_t budget=0;budget<64u;++budget){
        int result=native_display_service();
        if(result<0){recover(54u);return false;}
        *ready=result>0||(nd.mode==2u&&nd.step==7u);
        if(*ready||ln_time_us()-started>=250u)return true;
    }
    *ready=false;return true;
}
bool ln_m25_coprocessor_display_commit(void){
    if(!vc.resident||!nd.enabled||nd.mode!=2u||nd.step!=7u)return false;
    /* Submit the uploaded surface before yielding to the music decoder. */
    nd.step=5u;bool ready;return ln_m25_coprocessor_display_ready(&ready);
}
bool ln_m25_coprocessor_display_can_overlap(void){
    /* Pixel upload alone does not submit the native surface. Finish sending
     * its add/update RPCs before a long UI render; only the final update reply
     * waits for scanout and can profitably overlap CPU drawing. */
    return !vc.resident||!nd.enabled||!nd.mode||
        (nd.mode==2u&&nd.step==6u&&nd.pending);
}
bool ln_m25_coprocessor_quiesce_display(void){
    if(!vc.resident)return !vc.owned;
    if(vc.phase!=NATIVE_IDLE||nd.mode||vc.fs_canceling)return false;
    if(vc.player_task){
        if(!vc.player_retained||!player_files_closed()||player_stopped()!=1||player_rings_idle()!=1)return false;
        vc.player_retained=false;vc.retaining=vc.reuse_pending=false;
        /* The old video staging arena may now belong to music or an upload.
         * Unloading the idle shell must never run its former packet producer. */
        vc.operation=3u;vc.normal=false;
        vc.stopping=true;vc.stop_deadline=ln_time_us()+15000000u;vc.control=8u;phase(VERSION_SEND,5000000u);
        uint32_t unload_started=ln_time_us();
        while(vc.phase!=NATIVE_IDLE||vc.player_task){
            (void)ln_m25_coprocessor_step();
#if !defined(LINNET_PROFILE_RELEASE)
            ln_recovery_feed();
#else
            ln_core_watchdog_feed();
#endif
            if(!vc.resident||vc.error||ln_time_us()-unload_started>=5000000u)return false;
        }
    }
    vc.resident=false;vc.owned=true;recover(0);
    uint32_t started=ln_time_us();
    while(vc.owned){
        (void)ln_m25_coprocessor_step();
#if !defined(LINNET_PROFILE_RELEASE)
        ln_recovery_feed();
#else
        ln_core_watchdog_feed();
#endif
        if(ln_time_us()-started>=10000000u)return false;
    }
    return !vc.restore_error&&vc.phase==RESTORED;
}
uint32_t ln_m25_coprocessor_snapshot(uint32_t out[32]){
    if(!out)return 0;
    for(uint32_t i=0;i<32u;++i)out[i]=0;
    out[0]=0x4d250002u;out[1]=vc.phase;out[2]=vc.error;out[3]=vc.restore;
    out[4]=vc.at;out[5]=BCM_IMAGE_BYTES;out[6]=vc.rom_bytes;out[7]=vc.owned;
    for(uint32_t i=0;i<4u;++i){out[8+i]=vc.header[i];out[12+i]=vc.directory[i];out[16+i]=vc.legacy_header[i];}
    out[20]=vc.restore_error;out[21]=vc.rom_checked;out[22]=vc.map_logical;out[23]=vc.map_physical;
    for(uint32_t i=0;i<8u;++i)out[24+i]=vc.rom_diagnostic[i];
    return 32u;
}
uint32_t ln_m25_coprocessor_channel(uint32_t slot,uint32_t out[32]){
    if(slot==15u&&out){
        for(uint32_t i=0;i<32u;++i)out[i]=0;
        out[0]=nd.slot;out[1]=nd.resource;out[2]=nd.address;out[3]=nd.element;
        out[4]=nd.sequence;out[5]=nd.step;out[6]=nd.mode;out[7]=nd.at;
        out[8]=nd.pending;out[9]=nd.enabled;out[10]=nd.visible;
#if !defined(LINNET_PROFILE_RELEASE)
        out[11]=picture_cookie;
#endif
        out[20]=vc.player_task;out[21]=vc.resident;out[22]=vc.stopping;
        out[23]=vc.stop_events;
        out[24]=resources.hits;out[25]=resources.misses;out[26]=resources.bytes;out[27]=vc.image_generation;
        out[28]=nd.front_resource;out[29]=nd.front_address;
        out[30]=vc.picture_sequence;out[31]=vc.picture_polls;
        for(uint32_t i=0;i<8u;++i)out[12u+i]=nd.reply[i];return 32u;
    }
    if(slot==14u&&out){struct ln_audio_status a=vc.audio_status;if(vc.audio_ready)ln_audio_snapshot(&a);for(uint32_t i=0;i<32u;++i)out[i]=0;out[0]=vc.audio_phase;out[1]=vc.audio_frames;out[2]=vc.soundtrack.samples;out[3]=vc.soundtrack.lead;out[4]=vc.soundtrack.error;out[5]=vc.audio_phase?audio_decoder.error:0u;out[6]=a.rate;out[7]=a.queued;out[8]=a.consumed;out[9]=a.underruns;out[10]=a.fault;out[11]=vc.audio_eof;out[12]=a.silence_frames;out[13]=vc.start_elapsed;out[14]=vc.seek_stage;out[15]=vc.seek_ms;out[16]=vc.board_revision;out[17]=REG(0x4001ff1cu);out[18]=REG(0x4001ff18u);
#if !defined(LINNET_PROFILE_RELEASE)
        struct ln_power_state power;ln_power_snapshot(&power);
        out[19]=vc.display_blanked;out[20]=(REG(0x6000d024u)&8u)!=0;
        out[21]=power.backlight_on;out[22]=power.brightness;out[23]=vc.clock_valid;
        for(uint32_t i=0;i<8u;++i)out[24u+i]=picture_probe[i];
#endif
        return 32u;}
    if(slot==13u&&out){for(uint32_t i=0;i<32u;++i)out[i]=vc.host_event[i];return 32u;}
    if(slot>=16u&&slot<24u&&out){for(uint32_t i=0;i<32u;++i)out[i]=vc.reply[(slot-16u)*32u+i];return 32u;}
    if(slot==8u&&out){for(uint32_t i=0;i<32u;++i)out[i]=vc.reply[i];return 32u;}
    if(slot==9u&&out){for(uint32_t i=0;i<32u;++i)out[i]=vc.host_request[i];return 32u;}
    if(slot==10u&&out){for(uint32_t i=0;i<32u;++i)out[i]=0;out[0]=vc.fs_count;out[1]=vc.fs_opcode;out[2]=vc.fs_phase;out[3]=vc.fs_bytes;out[4]=vc.fs_requested;out[5]=vc.fs_tx_at;out[6]=vc.fs_tx_bytes;out[7]=vc.command_index;out[8]=vc.reply_bytes;out[9]=vc.reply_parsed;
        out[14]=vc.picture_pending;out[15]=vc.seek_frames;out[16]=vc.stream_frames;
#if !defined(LINNET_PROFILE_RELEASE)
        out[10]=rom_check_us;out[11]=rom_check_max_us;out[12]=rom_check_calls;out[13]=vc.rom_bytes;
        out[17]=vc.player_retained;out[18]=vc.reuse_pending;out[19]=vc.retention_revoked;
        out[20]=player_reuses;out[21]=player_unloads;out[22]=vc.retained_epoch;out[23]=vc.retaining;
        for(uint32_t i=0;i<8u;++i)out[24u+i]=idle_probe[i];
#endif
        return 32u;}
    if(slot==11u&&out){for(uint32_t i=0;i<32u;++i)out[i]=vc.host_channel[i];return 32u;}
    if(slot==12u&&out){for(uint32_t i=0;i<32u;++i)out[i]=0;out[0]=vc.stream_phase;out[1]=vc.stream_address;out[2]=vc.stream_capacity;out[3]=vc.movie.codec;out[4]=vc.movie.width;out[5]=vc.movie.height;out[6]=vc.movie.timescale;out[7]=vc.movie.config_bytes;out[8]=vc.movie.nal_bytes;out[9]=vc.packet.bytes;out[10]=vc.movie.error;out[11]=vc.stream_frames;
#if !defined(LINNET_PROFILE_RELEASE)
        out[12]=vc.cached.cookie;out[13]=vc.cached.failure;out[14]=vc.cached.elapsed;out[15]=vc.cached.count;
        out[16]=vc.cache_resources;out[17]=vc.cache_requested;out[18]=vc.cache_unexpected;
        out[19]=ln_storage_resident_valid(&vc.cached);out[20]=sizeof(vc.cached);
        out[22]=(uint32_t)vc.cached.missing_lba;out[23]=(uint32_t)(vc.cached.missing_lba>>32);out[24]=(uint32_t)vc.cached.result;
        out[25]=vc.binding.file.epoch;out[26]=vc.binding.file.cluster;out[27]=vc.binding.file.size;
        out[28]=vc.video_file.epoch;out[29]=vc.video_file.cluster;out[30]=vc.video_file.size;out[31]=vc.binding.path[0]!=0;
#endif
        return 32u;}
    if(slot>=8u||!out)return 0;
    for(uint32_t i=0;i<32u;++i)out[i]=0;
    out[0]=0x4d250010u+slot;out[1]=vc.phase;out[2]=vc.header[3];
    out[3]=(vc.directory[slot/2u]>>((slot&1u)*16u))&0xffffu;
    for(uint32_t i=0;i<20u;++i)out[4+i]=vc.channel[slot][i];
#if !defined(LINNET_PROFILE_RELEASE)
    if(!slot){out[24]=prefetch_reads;out[25]=prefetch_bytes;out[26]=vc.prefetch_at;out[27]=vc.prefetch_io;}
    if(slot==2u){out[24]=rom_burst_max_us;out[25]=rom_bursts;}
    if(slot==1u)for(uint32_t i=0;i<2u;++i)for(uint32_t j=0;j<4u;++j)out[24u+i*4u+j]=scan_service[i][j];
    if(slot==3u)for(uint32_t i=0;i<8u;++i)out[24u+i]=stream_batch[i];
    if(slot==4u)for(uint32_t i=0;i<8u;++i)out[24u+i]=stream_prepare[i];
    if(slot==5u)for(uint32_t i=0;i<8u;++i)out[24u+i]=file_batch[i];
    if(slot==7u){out[24]=vc.paused_poll_armed;out[25]=vc.paused_poll_at;out[26]=paused_polls;out[27]=paused_skips;out[28]=paused_max_gap;out[29]=10000u;}
#endif
    return 32u;
}
