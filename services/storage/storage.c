#include "boot/include/linnet_bytes.h"
#include "boot/include/sha256.h"
#include "services/storage/storage.h"
#include "services/storage/block.h"
#include "services/storage/fat32.h"
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && defined(LINNET_VIDEO)
#include "targets/video5g-m25/coprocessor.h"
#endif
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "hal/api/ata.h"
#include "hal/api/recovery.h"
#if defined(LINNET_IMAGE_BOOT)
#define LN_STORAGE_STAMP(name) LN_BOOT_STAMP(LN_BT_BOOT_##name)
#else
#define LN_STORAGE_STAMP(name) LN_BOOT_STAMP(LN_BT_OS_##name)
#endif
#include "services/update/update.h"
#if defined(LINNET_COVERFLOW_PACKED)
#include "services/music/coverflow.h"
#endif
#include "linnet_build.h"
#if defined(LINNET_AUDIO) && defined(LINNET_TARGET_VIDEO5G_M25)
#include "services/music/library.h"
#endif
#if defined(LINNET_IMAGE_OS)
#include "kernel/runtime.h"
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#include "hal/api/usb.h"
#endif

static struct ln_fat32 volume;
#define STORAGE_FILE_COUNT 18u
static struct ln_fat32_file files[STORAGE_FILE_COUNT],proof_files[STORAGE_FILE_COUNT];
static const char *const paths[STORAGE_FILE_COUNT]={"/.LINNET/OS0.LOS","/.LINNET/OS1.LOS",
    "/.LINNET/BOOT0.BIN","/.LINNET/BOOT1.BIN","/.LINNET/STATE0.BIN",
    "/.LINNET/STATE1.BIN","/.LINNET/SCRATCH.BIN","/.LINNET/WORK.BIN",
    "/.LINNET/MUSIC0.BIN","/.LINNET/MUSIC1.BIN",
    "/.LINNET/UPDATE0.BIN","/.LINNET/UPDATE1.BIN",
    "/.LINNET/THEME0.BIN","/.LINNET/THEME1.BIN", "/.LINNET/LISTS0.BIN", "/.LINNET/LISTS1.BIN",
    "/.LINNET/GMSAVE0.BIN","/.LINNET/GMSAVE1.BIN"
};
static uint8_t mbr[512] __attribute__((aligned(32)));
static uint32_t phase,error,found,opening,epoch=1,request,request_generation,request_sectors;
static bool pending,locked,media_realtime,media_realtime_wake_requested;
static uint32_t partition_scale;
static uint32_t media_owner;
#define MEDIA_WAKE_GENERATION 0x76000001u
static uint32_t video_browse_until,media_wake_ticket;
static uint32_t media_wake_status;
static bool video_browse,video_wake_requested,music_boundary,music_wake_requested;
static uint32_t fixed_owner,fixed_started;
static uint32_t media_read_ahead=LN_BLOCK_READ_AHEAD_DEFAULT;
static bool media_canceled,media_opening;
static bool background_running;
/* Nonzero only while a bounded deferred-runtime poll is on the stack. This is a
 * scoped scheduling hint, not a storage-owner or cancellation state. Boot
 * shares the polling code but has no runtime scheduler to read this hint. */
static uint32_t deferred_runtime_poll_depth __attribute__((unused));
static struct ln_state_store boot_store,settings_store,theme_store;
#if !defined(LINNET_PROFILE_RELEASE)
static struct ln_state_store history_store;
static bool history_opened;
#endif
static bool theme_available,theme_opened;
static uint32_t history_summary=UINT32_MAX;
static struct ln_update updater;
static bool stores_ready,boot_checked,health_checked;
#if defined(LINNET_IMAGE_BOOT) && (LINNET_BOOT_APPLE_FALLBACK || LINNET_BOOT_SELECTOR_ROCKBOX)
static uint32_t boot_choice;
#endif
#if defined(LINNET_IMAGE_OS)
static uint32_t health_open_attempts,health_retry_at;
#endif
static uint32_t progress_clock,progress_stalls;
#if defined(LINNET_IMAGE_OS)
static uint32_t background_at;
#endif
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t publication_fault;
/* POLL: completed, inclusive spans. Nested means same-function reentry. */
static struct {uint32_t epoch,flags,value[12],depth[2];} poll_timing;
_Static_assert(sizeof(poll_timing)<=64u,"storage timing RAM budget");
static struct schedule_diagnostics {
    uint32_t epoch,flags,samples,state[5],wait_us,wait_max,overdue_us,overdue_max;
    uint32_t gap_max,last_at,last_state,cancel_requests,cancel_terminals,cancel_polls;
    uint32_t cancel_us,cancel_max,cancel_started,cancel_generation;
    bool cancel_active;
} schedule_timing;
_Static_assert(sizeof(schedule_timing)<=96u,"schedule timing RAM budget");
static void schedule_add(uint32_t *value,uint32_t amount){
    if(amount>UINT32_MAX-*value){*value=UINT32_MAX;schedule_timing.flags|=0x80000000u;}
    else *value+=amount;
}
static void timing_add(uint32_t index,uint32_t value){
    if(value>UINT32_MAX-poll_timing.value[index]){
        poll_timing.value[index]=UINT32_MAX;poll_timing.flags|=1u<<index;
    }else poll_timing.value[index]+=value;
}
static void timing_span(uint32_t index,uint32_t start){
    uint32_t elapsed=ln_time_us()-start;
    if(elapsed>=0x80000000u)poll_timing.flags|=1u<<30;
    timing_add(index,1u);timing_add(index+1u,elapsed);
}
static uint32_t timing_enter(uint32_t index){
    uint32_t depth=poll_timing.depth[index];
    if(depth==UINT32_MAX)poll_timing.flags|=1u<<29;
    else ++poll_timing.depth[index];
    return depth;
}
static void timing_leave(uint32_t index,uint32_t depth,uint32_t start){
    /* A single end timestamp keeps total and nested spans identical. */
    uint32_t elapsed=ln_time_us()-start,base=index*4u;
    if(elapsed>=0x80000000u)poll_timing.flags|=1u<<30;
    timing_add(base,1u);timing_add(base+1u,elapsed);
    if(depth){timing_add(base+2u,1u);timing_add(base+3u,elapsed);}
    poll_timing.depth[index]=depth;
}
void ln_storage_media_performance(uint32_t words[16]){
    words[0]=0x4c4c4f50u;words[1]=0u;words[2]=poll_timing.epoch;
    words[3]=poll_timing.flags|((poll_timing.depth[0]||poll_timing.depth[1])?1u<<28:0u);
    for(uint32_t i=0;i<12u;++i)words[i+4u]=poll_timing.value[i];
}
enum ln_storage_schedule ln_storage_schedule_state(uint32_t now,uint32_t *deadline){
    uint32_t due;enum ln_block_schedule block=ln_block_schedule_state(now,&due);
    if(deadline)*deadline=due;
    if(!ln_storage_busy())return LN_STORAGE_SCHEDULE_IDLE;
    if(pending||phase==1){
        if(block==LN_BLOCK_SCHEDULE_COMPLETION)return LN_STORAGE_SCHEDULE_COMPLETION_RUNNABLE;
        if(block==LN_BLOCK_SCHEDULE_RUNNABLE)return LN_STORAGE_SCHEDULE_BLOCK_RUNNABLE;
        if(block==LN_BLOCK_SCHEDULE_WAITING)return LN_STORAGE_SCHEDULE_BLOCK_WAIT;
    }
    return LN_STORAGE_SCHEDULE_FAT_RUNNABLE;
}
void ln_storage_schedule_observe(uint32_t now){
    uint32_t due;enum ln_storage_schedule state=ln_storage_schedule_state(now,&due);
    schedule_add(&schedule_timing.samples,1u);schedule_add(&schedule_timing.state[state],1u);
    if(schedule_timing.last_at){
        uint32_t gap=now-schedule_timing.last_at;if(gap>schedule_timing.gap_max)schedule_timing.gap_max=gap;
    }
    schedule_timing.last_at=now;schedule_timing.last_state=(uint32_t)state;
    if(state==LN_STORAGE_SCHEDULE_BLOCK_WAIT){
        uint32_t offered=due-now;
        if(offered>=0x80000000u)schedule_timing.flags|=2u;
        else {schedule_add(&schedule_timing.wait_us,offered);if(offered>schedule_timing.wait_max)schedule_timing.wait_max=offered;}
    }else if(state==LN_STORAGE_SCHEDULE_BLOCK_RUNNABLE&&(int32_t)(now-due)>0){
        uint32_t overdue=now-due;schedule_add(&schedule_timing.overdue_us,overdue);
        if(overdue>schedule_timing.overdue_max)schedule_timing.overdue_max=overdue;
    }
}
void ln_storage_schedule_performance(uint32_t words[24]){
    uint32_t block[4];ln_block_schedule_metrics(block);
    words[0]=0x44484353u;words[1]=0u;words[2]=schedule_timing.epoch;
    words[3]=schedule_timing.flags|block[3];words[4]=schedule_timing.samples;
    for(uint32_t i=0;i<5u;++i)words[5u+i]=schedule_timing.state[i];
    words[10]=schedule_timing.wait_us;words[11]=schedule_timing.wait_max;
    words[12]=schedule_timing.overdue_us;words[13]=schedule_timing.overdue_max;
    words[14]=schedule_timing.gap_max;words[15]=schedule_timing.last_state;
    words[16]=block[0];words[17]=block[1];words[18]=block[2];
    words[19]=schedule_timing.cancel_requests;words[20]=schedule_timing.cancel_terminals;
    words[21]=schedule_timing.cancel_polls;words[22]=schedule_timing.cancel_us;
    words[23]=schedule_timing.cancel_max;
}
static void schedule_cancel_complete(uint32_t generation){
    if(!schedule_timing.cancel_active||schedule_timing.cancel_generation!=generation)return;
    uint32_t elapsed=ln_time_us()-schedule_timing.cancel_started;
    schedule_add(&schedule_timing.cancel_terminals,1u);schedule_add(&schedule_timing.cancel_us,elapsed);
    if(elapsed>schedule_timing.cancel_max)schedule_timing.cancel_max=elapsed;
    schedule_timing.cancel_active=false;
}
static int storage_fat_poll(void){
    uint32_t start=ln_time_us();int result=ln_fat32_poll(&volume);
    timing_span(8u,start);return result;
}
#else
#define storage_fat_poll() ln_fat32_poll(&volume)
#endif
#if !defined(LINNET_PROFILE_RELEASE)
int ln_storage_publication_fault(uint32_t stage){
    if(stage<1||stage>4||publication_fault||!ln_storage_prepared())return 0;
    publication_fault=stage;return 1;
}
void ln_state_publication_step(uint32_t stage){
    if(publication_fault!=stage)return;publication_fault=0;
    /* Deliberate crash-consistency campaign cut: bypass the flush barrier. */
    ln_platform_hardware_reset();
}
#endif
/* Explicitly reserved in memory.ld; copied Boot/OS code shares this layout. */
#define handoff ((volatile uint32_t *)ln_platform_uncached_address(LINNET_MAILBOX_ADDRESS+0x100u))
static uint32_t next_generation(void){if(++epoch==0)++epoch;return epoch;}
static __attribute__((noinline)) int sector_complete(uint32_t generation){
    if(generation!=request_generation)return -1;
    struct ln_block_completion completion;
    if(!ln_block_take(generation,&completion))return 0;
    pending=false;
    return completion.request_id==request&&completion.status==LN_ATA_OK&&
        completion.sectors_done==request_sectors?1:-1;
}
static int sector_io(void *context,enum ln_fat32_io operation,uint64_t lba,uint8_t *sector,uint32_t generation){
    (void)context;
    if(!pending){
        _Static_assert((int)LN_FAT32_READ==(int)LN_BLOCK_READ&&
            (int)LN_FAT32_WRITE==(int)LN_BLOCK_WRITE&&(int)LN_FAT32_FLUSH==(int)LN_BLOCK_FLUSH,"FAT/block operation mapping");
        enum ln_block_op op=(enum ln_block_op)operation;
        uint32_t deadline=ln_time_us()+ln_block_request_timeout();
        bool admitted=op==LN_BLOCK_READ?
            ln_block_submit_read(lba,1,sector,generation,deadline,media_read_ahead,&request):
            ln_block_submit(op,lba,operation==LN_FAT32_FLUSH?0:1,sector,generation,deadline,&request);
        if(!admitted)return -1;
        pending=true;request_generation=generation;
        request_sectors=operation==LN_FAT32_FLUSH?0u:1u;return 0;
    }
    return sector_complete(generation);
}
static int sectors_io(void *context,enum ln_fat32_io operation,uint64_t lba,uint8_t *buffer,uint32_t sectors,uint32_t generation){
    (void)context;
    if(!pending){
        uint32_t deadline=ln_time_us()+ln_block_request_timeout();
        /* Small hard-disk batches retain sequential read-ahead. Flash and
         * larger streams transfer directly without an intermediate copy. */
        struct ln_ata_state device;ln_ata_state_snapshot(&device);
        bool submitted=operation==LN_FAT32_WRITE?
            ln_block_submit(LN_BLOCK_WRITE,lba,sectors,buffer,generation,deadline,&request):
            sectors<=8u&&device.media_kind==LN_ATA_MEDIA_ROTATING?
            ln_block_submit_read(lba,sectors,buffer,generation,deadline,media_read_ahead,&request):
            ln_block_submit_direct_read(lba,sectors,buffer,generation,deadline,&request);
        if(!submitted)return -1;
        pending=true;request_generation=generation;request_sectors=sectors;return 0;
    }
    return sector_complete(generation);
}
void ln_storage_progress(void *context){
    (void)context;uint32_t now=ln_time_us();
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t timing_depth=timing_enter(1u);
#endif
    if(now==progress_clock){
        if(++progress_stalls>=1000000u){
            /* Buffer ownership cannot be released if the deadline clock died. */
            ln_platform_hardware_reset();
        }
    }else{progress_clock=now;progress_stalls=0;}
    if(media_wake_ticket){
        struct ln_block_completion completion;
        if(ln_block_take(MEDIA_WAKE_GENERATION,&completion)){
            media_wake_ticket=0;
            media_wake_status=completion.status;
        }
    }
    ln_block_allow_idle(phase==6&&!locked&&!pending&&!media_realtime&&!music_boundary&&
        (!video_browse||(int32_t)(now-video_browse_until)>=0));
    ln_block_step(now);
#if defined(LINNET_IMAGE_OS)
    if((int32_t)(now-background_at)>=0){background_at=now+1000u;
    if(!deferred_runtime_poll_depth){background_running=true;
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t runtime_start=ln_time_us();
#endif
        ln_runtime_step();
#if !defined(LINNET_PROFILE_RELEASE)
        timing_span(10u,runtime_start);
#endif
        background_running=false;}}
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    ln_recovery_feed();
    /* Progress DMA without admitting a second Link command recursively. */
    (void)ln_usb_poll();
    timing_leave(1u,timing_depth,now);
#else
    ln_core_watchdog_feed();
#endif
}
void ln_storage_init(void){phase=1;partition_scale=1;error=found=opening=0;pending=locked=media_realtime=media_realtime_wake_requested=music_boundary=music_wake_requested=false;media_wake_ticket=0;media_read_ahead=LN_BLOCK_READ_AHEAD_DEFAULT;
    media_wake_status=LN_ATA_OK;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t timing_flags=(poll_timing.depth[0]||poll_timing.depth[1])?1u<<29:0u;
    if(poll_timing.epoch==UINT32_MAX)timing_flags|=1u<<31;
    else ++poll_timing.epoch;
    poll_timing.flags=timing_flags;
    for(uint32_t i=0;i<12u;++i)poll_timing.value[i]=0;
    poll_timing.depth[0]=poll_timing.depth[1]=0;
    uint32_t schedule_flags=schedule_timing.cancel_active?1u:0u;
    uint32_t schedule_epoch=schedule_timing.epoch;
    if(schedule_epoch==UINT32_MAX)schedule_flags|=0x80000000u;else ++schedule_epoch;
    schedule_timing=(struct schedule_diagnostics){0};
    schedule_timing.flags=schedule_flags;schedule_timing.epoch=schedule_epoch;
#endif
    LN_STORAGE_STAMP(STORAGE);
#if defined(LINNET_IMAGE_BOOT)
    handoff[0]=0;
#endif
    ln_block_init(ln_time_us());}
static void storage_step(bool defer_runtime){
    if(defer_runtime)++deferred_runtime_poll_depth;
    ln_storage_progress(0);
    if(defer_runtime)--deferred_runtime_poll_depth;
    if(locked)return;
    if(phase==1){struct ln_block_info info;ln_block_snapshot(&info);
        if(info.initializing)return;
        if(!info.ready){error=1;phase=9;return;}
        LN_STORAGE_STAMP(BLOCK);
        struct ln_fat32_config config={.io=sector_io,.sectors_io=sectors_io,
            .context=0,.disk_sectors=info.sectors,.writable=true};
        ln_fat32_init(&volume,&config);phase=2;
    }
    if(phase==2){int result=partition_scale==1?sector_io(0,LN_FAT32_READ,0,mbr,epoch):1;if(!result)return;
        if(result<0||mbr[510]!=0x55||mbr[511]!=0xaa){error=2;phase=9;return;}
        uint64_t start=0,count=0;uint32_t matches=0;bool tail_claimed=false;
        for(uint32_t i=0;i<4;++i){const uint8_t *p=mbr+446+i*16;uint32_t n=ln_read_le32(p+12),s=ln_read_le32(p+8);
            if(!n)continue;
            if(!s||((uint64_t)s+n)*partition_scale>volume.config.disk_sectors){error=3;phase=9;return;}
            for(uint32_t j=0;j<i;++j){const uint8_t *q=mbr+446+j*16;uint32_t qn=ln_read_le32(q+12),qs=ln_read_le32(q+8);
                if(qn&&(uint64_t)s<(uint64_t)qs+qn&&(uint64_t)qs<(uint64_t)s+n){error=3;phase=9;return;}}
            if(((uint64_t)s+n)*partition_scale==volume.config.disk_sectors)tail_claimed=true;
            if(p[4]==0x0b||p[4]==0x0c){start=(uint64_t)s*partition_scale;count=(uint64_t)n*partition_scale;++matches;}
        }
        /* M25's factory MBR omits the final physical sector while its FAT32
         * BPB includes it. Admit that single tail sector only at disk end.
         * No partition metadata is rewritten; BPB validation still applies. */
(void)tail_claimed;
#if LINNET_STORAGE_TAIL_SECTOR
        if(matches==1&&!tail_claimed&&start+count+1u==volume.config.disk_sectors)++count;
#endif
        if(matches!=1||!ln_block_set_window(start,count)||!ln_fat32_mount(&volume,start,count,next_generation())){error=4;phase=9;return;}phase=3;
    }
    if(phase==3){int result=storage_fat_poll();if(!result)return;
        /* Some retail disks express MBR extents in 1/2/4 KiB units. Only a
         * rejected BPB advances the candidate; I/O and FAT corruption remain
         * terminal. Each candidate is bounded by the physical disk capacity. */
        if(result==LN_FAT32_FORMAT&&partition_scale<8){partition_scale*=2;phase=2;return;}
        if(result<0){error=(uint32_t)-result+100;phase=9;return;}LN_STORAGE_STAMP(FAT);opening=UINT32_MAX;phase=4;}
    if(phase==4){if(opening==STORAGE_FILE_COUNT){
            LN_STORAGE_STAMP(FILES);
            if((found&127u)!=127u){phase=6;return;}
            uint32_t count=0;for(uint32_t i=0;i<STORAGE_FILE_COUNT;++i)if(found&(1u<<i))proof_files[count++]=files[i];
            if(!ln_fat32_validate_set(&volume,proof_files,count,next_generation())){error=7;phase=9;return;}
            phase=7;return;
        }
        /* proof_files is unused until discovery finishes; its first entry
         * temporarily holds the common parent, adding no persistent storage. */
        bool admitted=opening==UINT32_MAX?ln_fat32_open(&volume,"/.LINNET",next_generation()):
            ln_fat32_open_at(&volume,&proof_files[0],paths[opening]+sizeof("/.LINNET")-1u,next_generation());
        if(!admitted){error=5;phase=9;return;}phase=5;
    }
    if(phase==5){int result=storage_fat_poll();if(!result)return;
        if(opening==UINT32_MAX){
            if(result==LN_FAT32_DONE&&(volume.file.attributes&16u)){
                proof_files[0]=volume.file;opening=0;
            }else if(result==LN_FAT32_NOT_FOUND||result==LN_FAT32_DONE)opening=STORAGE_FILE_COUNT;
            else{error=(uint32_t)-result+200;phase=9;return;}
            phase=4;return;
        }
        if(result==LN_FAT32_DONE){files[opening]=volume.file;found|=1u<<opening;}
        else if(result!=LN_FAT32_NOT_FOUND){error=(uint32_t)-result+200;phase=9;return;}
        ++opening;phase=4;
    }
    if(phase==7){int result=storage_fat_poll();if(!result)return;
        if(result<0){error=(uint32_t)-result+500;phase=9;return;}LN_STORAGE_STAMP(PROOF);phase=6;
    }
}
void ln_storage_step(void){storage_step(false);}
/* The executive runs the full runtime after this bounded step. Other callers,
 * including synchronous drains, retain ordinary recursive runtime service. */
void ln_storage_step_deferred(void){storage_step(true);}
bool ln_storage_busy(void){return locked||background_running||(phase>=1&&phase<6)||phase==7;}
bool ln_storage_mounted(void){return phase==6&&volume.mounted;}
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
static bool resident_ready(struct ln_resident_media *r){
    struct ln_ata_state device;ln_ata_state_snapshot(&device);
    if(device.media_kind!=LN_ATA_MEDIA_ROTATING||!ln_storage_mounted()||volume.mounted!=1u||
       ln_storage_busy()||pending||media_owner||fixed_owner||media_wake_ticket){
        ln_resident_end(r);r->failure=64u;return false;
    }
    return true;
}
bool ln_storage_resident_admit(struct ln_resident_media *r,const char *const *paths,
    uint32_t count,struct ln_fat32 *scratch,bool append){
    if(!resident_ready(r))return false;
    return append?ln_resident_append(r,&volume,paths,count,scratch):ln_resident_admit(r,&volume,paths,count,scratch);
}
bool ln_storage_resident_admit_files(struct ln_resident_media *r,const struct ln_fat32_file *files,uint32_t count,bool append){
    return resident_ready(r)&&ln_resident_admit_files(r,&volume,files,count,append);
}
bool ln_storage_resident_valid(const struct ln_resident_media *r){
    return ln_storage_mounted()&&volume.mounted==1u&&ln_resident_valid(r,volume.epoch);
}
int ln_storage_resident_read(const struct ln_resident_media *r,const struct ln_fat32_file *file,
    uint32_t offset,void *buffer,uint32_t bytes){
    return ln_storage_resident_valid(r)?ln_resident_read(r,file,offset,buffer,bytes):-1;
}
#endif
int ln_storage_media_realtime(bool active){
    struct ln_ata_state device;ln_ata_state_snapshot(&device);
    media_realtime=active&&device.media_kind==LN_ATA_MEDIA_ROTATING;
    if(!media_realtime){media_realtime_wake_requested=false;return 1;}
    struct ln_block_info block;ln_block_snapshot(&block);
    if(!ln_storage_mounted()||device.failed||block.quarantined)return -1;
    if(device.initialized&&device.powered&&!device.sleeping&&!device.initializing&&!block.power_transition)return 1;
    if(media_realtime_wake_requested)
        return !media_wake_ticket&&media_wake_status!=LN_ATA_OK?-1:0;
    /* A cached stream may never issue a physical read during startup. Wake
     * once in parallel with preparation, before its PCM reserve is consumed. */
    if(!media_wake_ticket&&ln_block_submit(LN_BLOCK_WAKE,0,0,0,MEDIA_WAKE_GENERATION,
        ln_time_us()+30000000u,&media_wake_ticket))media_realtime_wake_requested=true;
    return 0;
}
void ln_storage_music_boundary(uint32_t intents){
    if(!intents||!ln_storage_mounted()){
        music_boundary=music_wake_requested=false;return;
    }
    struct ln_ata_state device;ln_ata_state_snapshot(&device);
    music_boundary=device.media_kind==LN_ATA_MEDIA_ROTATING&&
        (intents&(LN_STORAGE_MUSIC_BOUNDARY|LN_STORAGE_MUSIC_PREPARE));
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    music_boundary=music_boundary||(device.media_kind==LN_ATA_MEDIA_SOLID_STATE&&
        (intents&LN_STORAGE_MUSIC_PREPARE));
#endif
    if(!music_boundary){music_wake_requested=false;return;}
    if(music_wake_requested||media_wake_ticket||device.busy)return;
    if(device.powered&&!device.sleeping&&!device.initializing
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
       &&ln_storage_music_disk_ready()==1
#endif
       ){music_wake_requested=true;return;}
    if(ln_block_submit(LN_BLOCK_WAKE,0,0,0,MEDIA_WAKE_GENERATION,
                       ln_time_us()+30000000u,&media_wake_ticket))music_wake_requested=true;
}
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
int ln_storage_music_disk_ready(void){
    struct ln_ata_state device;ln_ata_state_snapshot(&device);
    if(device.media_kind==LN_ATA_MEDIA_UNKNOWN)return 1;
    struct ln_block_info block;ln_block_snapshot(&block);
    if(!ln_storage_mounted()||device.failed||block.quarantined)return -1;
    if(device.initialized&&device.powered&&!device.sleeping&&!device.initializing&&!block.power_transition)return 1;
    return music_wake_requested&&!media_wake_ticket&&media_wake_status!=LN_ATA_OK?-1:0;
}
#endif
void ln_storage_video_browse(bool active,bool activity){
    uint32_t now=ln_time_us();
    if(!active){video_browse=false;video_wake_requested=false;return;}
    if(!video_browse||(activity&&(int32_t)(now-video_browse_until)>=0))
        video_wake_requested=false;
    if(!video_browse||activity)video_browse_until=now+60000000u;
    video_browse=true;
    if(video_wake_requested||media_wake_ticket||!ln_storage_mounted())return;
    struct ln_ata_state device;ln_ata_state_snapshot(&device);
    if(device.powered&&!device.sleeping&&!device.busy&&!device.initializing){video_wake_requested=true;return;}
    if(ln_block_submit(LN_BLOCK_WAKE,0,0,0,MEDIA_WAKE_GENERATION,now+30000000u,&media_wake_ticket))
        video_wake_requested=true;
}
uint32_t ln_storage_disk_sector_bytes(void){return partition_scale*512u;}
bool ln_storage_startup_complete(void){return phase==6||phase==9;}
bool ln_storage_prepared(void){return ln_storage_mounted()&&volume.mounted==1&&volume.write_proven&&(found&127u)==127u&&files[0].size==LINNET_OS_CAPACITY&&files[1].size==LINNET_OS_CAPACITY&&
    files[2].size==512&&files[3].size==512&&files[4].size==512&&files[5].size==512&&files[6].size==0x400000u;}
uint32_t ln_storage_capacity(uint32_t file){return file<STORAGE_FILE_COUNT&&(found&(1u<<file))?files[file].size:0;}
bool ln_storage_game_saves_available(void){
    if(!ln_storage_prepared()||ln_storage_capacity(LN_STORAGE_GAMESAVE0)!=524288u||
       ln_storage_capacity(LN_STORAGE_GAMESAVE1)!=524288u)return false;
    struct ln_ata_info info;ln_ata_snapshot(&info);
    uint32_t unit=info.physical_sector_bytes;
    if(unit<512u||unit>4096u||(unit&(unit-1u)))return false;
    uint32_t sectors=unit>>9;
    return volume.write_proven&&volume.sectors_per_cluster>=sectors&&
        !(volume.sectors_per_cluster&(sectors-1u))&&!(volume.data_start&(sectors-1u));
}

int ln_storage_prepare_writes(void){
    if(ln_storage_busy())return 0;
    if(ln_storage_prepared())return 1;
    if(!ln_storage_mounted()||volume.mounted!=1||(found&127u)!=127u||
       files[0].size!=LINNET_OS_CAPACITY||files[1].size!=LINNET_OS_CAPACITY||files[2].size!=512||files[3].size!=512||
       files[4].size!=512||files[5].size!=512||files[6].size!=0x400000u||
       !ln_fat32_proof_pending(&volume))return -1;
    if(pending||media_owner)return 0;
    if(!ln_fat32_refresh_proof(&volume,next_generation()))return -1;
    phase=7;return 0;
}
uint32_t ln_storage_next_deadline(uint32_t now){return ln_block_next_deadline(now);}
bool ln_storage_idle_wait(uint32_t now,uint32_t *deadline){
    /* FAT can have runnable CPU work between sector requests. Only a pending
     * sector or phase-1 device initialization may delegate to block wait. */
    if(!ln_storage_busy()||(!pending&&phase!=1u))return false;
    return ln_block_idle_wait(now,deadline);
}
static int finish(void){
    uint32_t start=ln_time_us();int result;
    do{ln_storage_progress(0);result=storage_fat_poll();
        /* All lower requests have five-second deadlines. Stop after bounded
         * total file work; drain current DMA/buffer ownership before return. */
        if(!result&&ln_time_us()-start>120000000u){ln_block_cancel(request_generation);
            while(pending){ln_storage_progress(0);(void)storage_fat_poll();}
            ln_fat32_abandon(&volume);phase=9;error=6;locked=false;return 0;}
    }while(!result);
    locked=false;if(result<0){error=(uint32_t)-result+300;return 0;}return 1;
}
int ln_storage_media_open(const char *path,uint32_t generation){
    if(!generation||!path)return -1;
    if(!ln_storage_mounted()||locked||media_owner)return 0;
    if(!ln_fat32_open_readonly(&volume,0,path,next_generation()))return -1;
    media_read_ahead=LN_BLOCK_READ_AHEAD_DEFAULT;
    media_owner=generation;media_canceled=false;media_opening=true;locked=true;return 1;
}
int ln_storage_media_list(struct ln_fat32_directory *cursor,void *sector,uint32_t generation){
    if(!generation||!cursor||!sector)return -1;
    if(!ln_storage_mounted()||locked||media_owner)return 0;
    if(!ln_fat32_list(&volume,cursor,sector,next_generation()))return -1;
    media_read_ahead=1u;media_owner=generation;media_canceled=false;media_opening=false;locked=true;return 1;
}
bool ln_storage_media_root(struct ln_fat32_file *directory){
    if(!directory||!ln_storage_mounted())return false;
    *directory=(struct ln_fat32_file){.cluster=volume.root,.epoch=volume.epoch,.attributes=16};return true;
}
int ln_storage_media_open_at(const struct ln_fat32_file *directory,const char *leaf,uint32_t generation){
    if(!directory||!leaf||!generation)return -1;
    if(!ln_storage_mounted()||locked||media_owner)return 0;
    if(!ln_fat32_open_readonly(&volume,directory,leaf,next_generation()))return -1;
    media_read_ahead=1u;media_owner=generation;media_canceled=false;media_opening=true;locked=true;return 1;
}
int ln_storage_media_read(const struct ln_fat32_file *file,uint32_t offset,
                          void *buffer,uint32_t length,uint32_t generation){
    if(!generation||!file||!buffer||!length||length>LN_STORAGE_MEDIA_MAX_READ)return -1;
    if(!ln_storage_mounted()||locked||media_owner)return 0;
    if(!ln_fat32_read(&volume,file,offset,buffer,length,next_generation()))return -1;
    /* Small random metadata benefits from exact sectors; larger reads allow
     * the block owner to choose read-ahead for the identified medium. */
    media_read_ahead=length<512u?1u:LN_BLOCK_READ_AHEAD_DEFAULT;
    media_owner=generation;media_canceled=false;media_opening=false;locked=true;return 1;
}
static int media_poll(uint32_t generation,struct ln_fat32_file *file,uint32_t *transferred,
                      bool defer_runtime){
    if(!media_owner||generation!=media_owner)return LN_FAT32_STALE;
    if(defer_runtime)++deferred_runtime_poll_depth;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t timing_start=ln_time_us(),timing_depth=timing_enter(0u);
#endif
    uint32_t operation_generation=volume.generation;
    int result;
    struct ln_block_completion completion;bool have_completion=false;
    if(media_canceled){
        if(!pending)goto cancel;
        /* Preserve a terminal status that existed before cancellation. The
         * block cancel API deliberately relabels an untaken DONE slot. */
        if(ln_block_take(request_generation,&completion)){
            pending=false;have_completion=true;goto cancel;
        }
        ln_block_cancel(request_generation);ln_storage_progress(0);
        goto cancel;
    }
    /* Reads may submit/check a sector before progress and consume its result
     * afterwards. Opens retain their existing one-transition ordering. */
    result=media_opening?0:storage_fat_poll();
    ln_storage_progress(0);
    /* Progress can reenter a caller that drains/replaces this operation.
     * Never poll or publish that caller's result, even if it reused its tag. */
    if(media_owner!=generation||volume.generation!=operation_generation){
        result=LN_FAT32_STALE;goto complete;
    }
    if(media_canceled)goto cancel;
    if(!result)result=storage_fat_poll();
    if(!result)goto complete;
    if(transferred)*transferred=volume.transferred;
    if(result==LN_FAT32_DONE&&media_opening&&file)*file=volume.file;
    media_owner=0;locked=false;media_canceled=false;media_read_ahead=LN_BLOCK_READ_AHEAD_DEFAULT;
    goto complete;
cancel:
#if !defined(LINNET_PROFILE_RELEASE)
    schedule_add(&schedule_timing.cancel_polls,1u);
#endif
    /* A canceled read must not reenter FAT: consuming a canceled completion as
     * ordinary I/O would quarantine the mount, while another poll could submit
     * more sectors. Drain the exact block request here, then retire FAT state. */
    if(media_owner!=generation||volume.generation!=operation_generation){
        result=LN_FAT32_STALE;goto complete;
    }
    if(pending){
        if(!ln_block_take(request_generation,&completion)){
            ln_block_cancel(request_generation);result=0;goto complete;
        }
        pending=false;have_completion=true;
    }
    if(have_completion){
        bool matching=completion.request_id==request&&completion.generation==request_generation;
        /* A completed direct read can span multiple sectors. Apply the same
         * full-length check as normal read completion before retiring it. */
        bool clean=completion.status==LN_ATA_CANCELED||
            (completion.status==LN_ATA_OK&&completion.sectors_done==request_sectors);
        result=ln_fat32_retire_read(&volume,matching&&clean?LN_FAT32_STALE:LN_FAT32_IO);
    }else result=ln_fat32_retire_read(&volume,LN_FAT32_STALE);
    media_owner=0;locked=false;media_canceled=false;media_read_ahead=LN_BLOCK_READ_AHEAD_DEFAULT;
complete:
#if !defined(LINNET_PROFILE_RELEASE)
    schedule_cancel_complete(generation);
    timing_leave(0u,timing_depth,timing_start);
#endif
    if(defer_runtime)--deferred_runtime_poll_depth;
    return result;
}
int ln_storage_media_poll(uint32_t generation,struct ln_fat32_file *file,uint32_t *transferred){
    return media_poll(generation,file,transferred,false);
}
int ln_storage_media_poll_video(uint32_t generation,struct ln_fat32_file *file,
                                uint32_t *transferred){
    return ln_storage_media_poll_deferred(generation,file,transferred);
}
int ln_storage_media_poll_deferred(uint32_t generation,struct ln_fat32_file *file,
                                   uint32_t *transferred){
    return media_poll(generation,file,transferred,true);
}
void ln_storage_media_cancel(uint32_t generation){if(media_owner==generation){
#if !defined(LINNET_PROFILE_RELEASE)
    if(!media_canceled){schedule_add(&schedule_timing.cancel_requests,1u);
        schedule_timing.cancel_started=ln_time_us();schedule_timing.cancel_generation=generation;
        schedule_timing.cancel_active=true;}
#endif
    media_canceled=true;
}}
int ln_storage_read(uint32_t file,uint32_t offset,void *buffer,uint32_t length){
    if(!ln_storage_mounted()||locked||file>=STORAGE_FILE_COUNT||!(found&(1u<<file)))return 0;
    locked=true;if(!ln_fat32_read(&volume,&files[file],offset,buffer,length,next_generation())){locked=false;return 0;}return finish();
}
int ln_storage_write(uint32_t file,uint32_t offset,const void *buffer,uint32_t length){
    if(!ln_storage_prepared()||locked||file>=STORAGE_FILE_COUNT||!(found&(1u<<file)))return 0;
    locked=true;if(!ln_fat32_write(&volume,&files[file],offset,buffer,length,next_generation())){locked=false;return 0;}return finish();
}
int ln_storage_flush(void){
    if(!ln_storage_mounted()||locked)return 0;
    locked=true;if(!ln_fat32_sync(&volume,next_generation())){locked=false;return 0;}return finish();
}
#if LINNET_TARGET_NUMBER==25u
bool ln_storage_async_read_available(void){
    return !ln_storage_busy()&&!pending&&!media_owner&&!fixed_owner&&ln_storage_mounted();
}
#endif
static int async_fixed(uint32_t file,uint32_t offset,void *buffer,uint32_t length,
                       uint32_t *ticket,uint32_t operation){
    if(!ticket||(operation!=LN_FAT32_FLUSH&&
       (file>=STORAGE_FILE_COUNT||!(found&(1u<<file))||!buffer||!length||length>16384u)))return -1;
    if(ln_storage_busy()||pending||media_owner||fixed_owner)return 0;
    if(!ln_storage_mounted()||(operation==LN_FAT32_WRITE&&!ln_storage_prepared()))return -1;
    uint32_t generation=next_generation();bool admitted;
    if(operation==LN_FAT32_FLUSH)admitted=ln_fat32_sync(&volume,generation);
    else if(operation==LN_FAT32_WRITE)admitted=ln_fat32_write(&volume,&files[file],offset,buffer,length,generation);
    else admitted=ln_fat32_read(&volume,&files[file],offset,buffer,length,generation);
    if(!admitted)return -1;
    fixed_owner=generation;fixed_started=ln_time_us();locked=true;*ticket=generation;return 1;
}
int ln_storage_async_read(uint32_t file,uint32_t offset,void *buffer,uint32_t length,uint32_t *ticket){
    return async_fixed(file,offset,buffer,length,ticket,LN_FAT32_READ);
}
int ln_storage_async_write(uint32_t file,uint32_t offset,const void *buffer,uint32_t length,uint32_t *ticket){
    return async_fixed(file,offset,(void *)buffer,length,ticket,LN_FAT32_WRITE);
}
int ln_storage_async_flush(uint32_t *ticket){return async_fixed(0,0,0,0,ticket,LN_FAT32_FLUSH);}
int ln_storage_async_poll(uint32_t ticket,uint32_t *transferred){
    if(!fixed_owner||ticket!=fixed_owner)return LN_FAT32_STALE;
    ln_storage_progress(0);
    /* Match synchronous fixed-file work's total deadline, but drain a live
     * sector asynchronously before retiring the mount or releasing its buffer. */
    bool expired=ln_time_us()-fixed_started>120000000u;int result;
    if(expired){
        if(pending){ln_block_cancel(request_generation);(void)storage_fat_poll();if(pending)return 0;}
        ln_fat32_abandon(&volume);phase=9;error=6;result=LN_FAT32_IO;
    }else result=storage_fat_poll();
    if(!result)return 0;
    if(transferred)*transferred=volume.transferred;
    fixed_owner=0;locked=false;
    if(result<0&&!expired)error=(uint32_t)-result+300;
    return result;
}
int ln_storage_async_poll_deferred(uint32_t ticket,uint32_t *transferred){
    ++deferred_runtime_poll_depth;
    int result=ln_storage_async_poll(ticket,transferred);
    --deferred_runtime_poll_depth;return result;
}
bool ln_storage_quiesce_ready(void){return !locked&&!pending&&!ln_storage_busy();}
static int storage_quiesce(bool image_handoff){
    if(!ln_storage_quiesce_ready())return 0;
    struct ln_block_info info;ln_block_snapshot(&info);
    if(!info.ready)return !info.quarantined&&!found;
    bool keep_awake=false;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_IMAGE_BOOT)
    if(image_handoff){
        struct ln_ata_state media;ln_ata_state_snapshot(&media);
        /* The OS will mount the same rotating disk immediately after handoff. */
        keep_awake=media.media_kind==LN_ATA_MEDIA_ROTATING;
    }
#else
    (void)image_handoff;
#endif
    uint32_t deadline=ln_time_us()+30000000u;int result;
    /* No FAT owner needs executive service after the admission check. Avoid
     * admitting a new Photos/media read while closing the block queue. */
    ++deferred_runtime_poll_depth;
    do{
        ln_storage_progress(0);
        result=keep_awake?ln_block_quiesce_keep_awake(ln_time_us(),deadline):
                          ln_block_quiesce(ln_time_us(),deadline);
    }while(!result);
    --deferred_runtime_poll_depth;
    return result==1;
}
int ln_storage_quiesce(void){return storage_quiesce(false);}
int ln_storage_handoff_quiesce(void){return storage_quiesce(true);}
int ln_storage_resume(void){return ln_block_resume();}
int ln_storage_recover(void){
    if(locked||pending||ln_storage_busy()||!ln_block_recover(ln_time_us()))return 0;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && defined(LINNET_VIDEO)
    ln_m25_coprocessor_storage_invalidated();
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    media_wake_status=LN_ATA_OK;
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_TARGET_VIDEO5G_M25)
    ln_music_library_verification_reset();
#endif
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    ln_coverflow_history_reset();
    ln_coverflow_preview_reset();
#endif
    ln_fat32_abandon(&volume);phase=1;found=opening=error=0;stores_ready=false;
    next_generation();return 1;
}
int ln_storage_disk_mode(void){
#if !defined(LINNET_PROFILE_RELEASE)
    return ln_recovery_reboot(0,34);
#else
    uint32_t sector_bytes=ln_storage_disk_sector_bytes();
    if(!ln_storage_quiesce()||!ln_platform_audio_idle())return 0;
#if defined(LINNET_IMAGE_OS)
    extern bool ln_display_quiesce(void);
    if(!ln_display_quiesce())return 0;
#endif
    ln_core_watchdog_disarm();ln_platform_quiesce();
    if(!ln_platform_dma_idle()){ln_core_watchdog_arm();return 0;}
    if(!ln_platform_enter_disk_mode(sector_bytes)){ln_core_watchdog_arm();return 0;}
    ln_core_watchdog_arm();
    return 0;
#endif
}
int ln_storage_restart(void){
    if(!ln_storage_quiesce()||!ln_platform_audio_idle())return 0;
#if !defined(LINNET_PROFILE_RELEASE)
    if(!ln_recovery_quiesce(0))return 0;
#elif defined(LINNET_IMAGE_OS)
    extern bool ln_display_quiesce(void);if(!ln_display_quiesce())return 0;
#endif
    ln_core_watchdog_disarm();ln_platform_quiesce();
    if(!ln_platform_set_disk_mode(false)){ln_core_watchdog_arm();return 0;}
    ln_platform_hardware_reset();
}
void ln_storage_state(uint32_t words[24]){
    struct ln_block_info b;ln_block_snapshot(&b);
    uint32_t values[24]={0x524f5453u,phase,error,found,ln_storage_prepared(),(uint32_t)b.sectors,(uint32_t)(b.sectors>>32),
        b.ready,b.quarantined,b.pending,b.completed,b.submitted,b.retries,b.failed,b.canceled,b.coalesced,b.cache_hits,
        b.read_ahead_sectors,b.max_service_us,(uint32_t)volume.partition,(uint32_t)volume.partition_sectors,volume.clusters,epoch,ln_storage_capacity(LN_STORAGE_WORK)};
    for(uint32_t i=0;i<24;++i)words[i]=values[i];
}
static int state_read(void *context,uint32_t copy,uint8_t data[512]){
    return copy<2?ln_storage_read((uint32_t)(uintptr_t)context+copy,0,data,512):0;
}
static int state_write(void *context,uint32_t copy,const uint8_t data[512]){
    return copy<2?ln_storage_write((uint32_t)(uintptr_t)context+copy,0,data,512):0;
}
static int flush_callback(void *context){(void)context;return ln_storage_flush();}
static int image_read(void *context,uint32_t slot,uint32_t offset,uint8_t *data,uint32_t length){
    (void)context;return slot<2?ln_storage_read(slot,offset,data,length):0;
}
static int image_write(void *context,uint32_t slot,uint32_t offset,const uint8_t *data,uint32_t length){
    (void)context;return slot<2?ln_storage_write(slot,offset,data,length):0;
}
static uint32_t image_capacity(void *context,uint32_t slot){(void)context;return slot<2?ln_storage_capacity(slot):0;}
/* Optional history is independent of boot metadata and settings. It never authorizes a boot
 * or blocks one merely because the sidecars are absent or their records corrupt.
 * Invalid FAT allocation still fails the shared write-safety proof. */
#if !defined(LINNET_PROFILE_RELEASE)
static void history_open(void){
    if(history_opened||!stores_ready||ln_storage_busy())return;
    history_opened=true;
    history_summary=UINT32_MAX;
    if(ln_storage_capacity(LN_STORAGE_UPDATE0)!=512u||ln_storage_capacity(LN_STORAGE_UPDATE1)!=512u)return;
    struct ln_state_io io={(void *)(uintptr_t)LN_STORAGE_UPDATE0,state_read,state_write,flush_callback};
    if(ln_state_open(&history_store,&io)<0)return;
    if(!history_store.length){history_summary=0;return;}
    const uint8_t *p=history_store.payload;
    if(history_store.length!=60u||ln_read_le32(p)!=0x4b424c52u||ln_read_le32(p+4)!=0u||
       ln_read_le32(p+8)<1u||ln_read_le32(p+8)>2u||ln_read_le32(p+12)>1u||
       (ln_read_le32(p+16)>1u&&ln_read_le32(p+16)!=255u)){
        history_store.ready=0;return;
    }
    history_summary=ln_read_le32(p+8)|(ln_read_le32(p+12)<<8);
}
#endif
uint32_t ln_storage_update_history(void){return history_summary;}
static void theme_open(void){
    theme_available=false;
    if(ln_storage_capacity(LN_STORAGE_THEME0)!=512u||ln_storage_capacity(LN_STORAGE_THEME1)!=512u)return;
    struct ln_state_io io={(void *)(uintptr_t)LN_STORAGE_THEME0,state_read,state_write,flush_callback};
    if(ln_state_open(&theme_store,&io)<0||theme_store.length>256u)return;
    theme_available=true;
}
#if defined(LINNET_IMAGE_BOOT)
static void history_save(uint32_t reason,uint32_t failed,uint32_t fallback,
                         uint32_t attempt,uint32_t generation,const uint8_t build[32]){
#if !defined(LINNET_PROFILE_RELEASE)
    history_open();
    if(history_summary==UINT32_MAX||!history_store.ready||history_store.poisoned)return;
    uint8_t payload[60]={0};uint32_t words[7]={0x4b424c52u,0,reason,failed,fallback,attempt,generation};
    for(uint32_t i=0;i<7;++i)for(uint32_t j=0;j<4;++j)payload[4*i+j]=(uint8_t)(words[i]>>(8*j));
    for(uint32_t i=0;i<32;++i)payload[28+i]=build[i];
    /* Required boot metadata is already durable. A history publication failure
     * remains poisoned and is never retried during this execution. */
    if(ln_state_publish(&history_store,payload,sizeof(payload)))history_summary=reason|(failed<<8);
    else history_summary=UINT32_MAX;
#else
    (void)reason; (void)failed; (void)fallback;
    (void)attempt; (void)generation; (void)build;
#endif
}
#endif
static int open_stores(void){
    if(stores_ready)return 1;
    /* Status queries may arrive while media owns FAT. A refused read then
     * means busy, not corrupt state; leave unopened stores intact and defer. */
    if(ln_storage_busy()||!ln_storage_prepared())return 0;
    struct ln_state_io bio={(void *)(uintptr_t)LN_STORAGE_BOOT0,state_read,state_write,flush_callback};
    struct ln_state_io sio={(void *)(uintptr_t)LN_STORAGE_STATE0,state_read,state_write,flush_callback};
    if(ln_state_open(&boot_store,&bio)!=1||ln_state_open(&settings_store,&sio)!=1){error=400;return 0;}
    struct ln_update_io io={0,image_read,image_write,flush_callback,image_capacity,ln_storage_progress};
    struct ln_image_policy policy={LINNET_TARGET_NUMBER,
#if defined(LINNET_PROFILE_RELEASE) && !(defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_IMAGE_BOOT))
        1,
#else
        /* Classic uses compact release Boot for both User and Developer OS
         * installations. The OS profile does not require Link inside Boot. */
        7,
#endif
        LINNET_LOAD_ADDRESS,LINNET_OS_CAPACITY};
    if(!ln_update_open(&updater,&boot_store,&io,&policy)){error=401;return 0;}
    stores_ready=true;theme_opened=false;history_summary=UINT32_MAX;
#if !defined(LINNET_PROFILE_RELEASE)
    history_opened=false;
#if defined(LINNET_IMAGE_OS)
    /* Keep the Update screen responsive; Boot only needs this optional store
     * when recording a failed attempt or rejected candidate. */
    history_open();
#endif
#endif
    return 1;
}
#if defined(LINNET_IMAGE_BOOT) && (LINNET_BOOT_APPLE_FALLBACK || LINNET_BOOT_SELECTOR_ROCKBOX)
void ln_storage_boot_choice(uint32_t choice){if(!boot_choice&&choice>=1u&&choice<=4u)boot_choice=choice;}
static int selector_open(const char *path,struct ln_fat32_file *file){
    if(!ln_fat32_open(&volume,path,next_generation()))return 0;
    uint32_t previous_error=error;
    locked=true;if(!finish()){
        /* Startup choices and their configuration are optional. Absence must
         * not leave healthy recovery Boot with a persistent storage fault.
         * Preserve actual I/O, format and allocation failures. */
        if(error==300u-LN_FAT32_NOT_FOUND)error=previous_error;
        return 0;
    }
    *file=volume.file;return 1;
}
static int selector_read(const struct ln_fat32_file *file,uint32_t offset,uint8_t *data,uint32_t length){
    if(!length||length>LN_STORAGE_MEDIA_MAX_READ||!ln_fat32_read(&volume,file,offset,data,length,next_generation()))return 0;
    locked=true;return finish();
}
static int selector_quiesce(void){
    if(!ln_platform_audio_idle())return 0;
#if defined(LINNET_SOC_PP5020) || defined(LINNET_SOC_PP5022)
#if !defined(LINNET_PROFILE_RELEASE)
    if(!ln_recovery_quiesce(0))return 0;
    /* External firmware owns the next reset; withdraw Linnet's recovery lease. */
    ln_core_watchdog_disarm();
#else
    if(!ln_storage_quiesce())return 0;
    ln_core_watchdog_disarm();ln_platform_quiesce();
    if(!ln_platform_dma_idle()){ln_core_watchdog_arm();return 0;}
    if(!ln_core_cache_disable()){ln_core_watchdog_arm();return 0;}
#endif
#else
    if(!ln_storage_quiesce())return 0;
    ln_core_watchdog_disarm();ln_platform_quiesce();
    if(!ln_platform_dma_idle()){ln_core_watchdog_arm();return 0;}
#endif
    return 1;
}
uint32_t ln_storage_boot_options(void){
    if(!ln_storage_startup_complete()||ln_storage_busy())return 0u;
    uint32_t options=1u;
    if(!ln_storage_mounted())return options;
#if LINNET_BOOT_SELECTOR_ROCKBOX
    struct ln_fat32_file rb_config,rb_image;
    uint8_t rb_data[512],rb_header[8];char rb_path[LN_FAT32_PATH_MAX+1];
    if(selector_open("/.LINNET/RBPATH.CFG",&rb_config)&&rb_config.size==sizeof(rb_data)&&
       selector_read(&rb_config,0,rb_data,sizeof(rb_data))&&
       rb_data[0]=='R'&&rb_data[1]=='B'&&rb_data[2]=='P'&&rb_data[3]=='A'&&
       rb_data[4]=='T'&&rb_data[5]=='H'&&rb_data[6]=='1'&&rb_data[7]==0&&
       ln_crc32(rb_data,508u)==ln_read_le32(rb_data+508u)){
        uint32_t length=ln_read_le32(rb_data+8u);
        if(length&&length<=LN_FAT32_PATH_MAX&&12u+length<508u&&
           rb_data[12u]=='/'&&!rb_data[12u+length]){
            for(uint32_t i=0;i<=length;++i)rb_path[i]=(char)rb_data[12u+i];
            if(selector_open(rb_path,&rb_image)&&rb_image.size>=40u&&
               rb_image.size-8u<=LINNET_STAGE_CAPACITY&&
               selector_read(&rb_image,0,rb_header,sizeof(rb_header))){
                static const char model[]=LINNET_FILE_WRAPPER_MODEL;
                bool match=true;
                for(uint32_t i=0;i<4u;++i)if(rb_header[4u+i]!=(uint8_t)model[i])match=false;
                if(match)options|=2u;
            }
        }
    }
#endif
#if LINNET_BOOT_APPLE_FALLBACK
    struct ln_fat32_file apple_config,apple_image;
    uint8_t apple_data[512];
    if(selector_open("/.LINNET/APPLE.CFG",&apple_config)&&apple_config.size==sizeof(apple_data)&&
       selector_read(&apple_config,0,apple_data,sizeof(apple_data))&&
       apple_data[0]=='A'&&apple_data[1]=='P'&&apple_data[2]=='P'&&apple_data[3]=='L'&&
       apple_data[4]=='E'&&apple_data[5]=='O'&&apple_data[6]=='S'&&apple_data[7]=='1'&&
       ln_read_le32(apple_data+8u)==LINNET_TARGET_NUMBER&&
       ln_crc32(apple_data,508u)==ln_read_le32(apple_data+508u)){
        uint32_t size=ln_read_le32(apple_data+12u);
        if(size&&size<=LINNET_STAGE_CAPACITY&&
           selector_open("/.LINNET/APPLE.BIN",&apple_image)&&apple_image.size==size)
            options|=4u;
    }
#endif
    return options;
}
uint32_t ln_storage_boot_default(uint32_t options){
    if(!options||!ln_storage_mounted()||ln_storage_busy())return 1u;
    struct ln_fat32_file config_file;
    uint8_t config[512];
    static const uint8_t magic[8]={'L','N','S','T','A','R','T','1'};
    if(!selector_open("/.LINNET/STARTUP.CFG",&config_file)||config_file.size!=sizeof(config)||
       !selector_read(&config_file,0,config,sizeof(config)))return 1u;
    for(uint32_t i=0;i<8u;++i)if(config[i]!=magic[i])return 1u;
    if(ln_read_le32(config+8u)!=LINNET_TARGET_NUMBER||
       ln_crc32(config,508u)!=ln_read_le32(config+508u))return 1u;
    for(uint32_t i=16u;i<508u;++i)if(config[i])return 1u;
    uint32_t choice=ln_read_le32(config+12u);
    return choice>=1u&&choice<=3u&&(options&(1u<<(choice-1u)))?choice:1u;
}
#if LINNET_BOOT_SELECTOR_ROCKBOX
static int selector_rockbox(void){
    struct ln_fat32_file config_file,rockbox_file;
    uint8_t config[512],header[8];char path[LN_FAT32_PATH_MAX+1];
    if(!selector_open("/.LINNET/RBPATH.CFG",&config_file)||config_file.size!=sizeof(config) ||
       !selector_read(&config_file,0,config,sizeof(config)))return 0;
    static const uint8_t magic[8]={'R','B','P','A','T','H','1',0};
    for(uint32_t i=0;i<8u;++i)if(config[i]!=magic[i])return 0;
    if(ln_crc32(config,508u)!=ln_read_le32(config+508u))return 0;
    uint32_t path_length=ln_read_le32(config+8u);
    if(!path_length||path_length>LN_FAT32_PATH_MAX||12u+path_length>=508u||
       config[12u]!='/'||config[12u+path_length])return 0;
    for(uint32_t i=0;i<=path_length;++i)path[i]=(char)config[12u+i];
    if(!selector_open(path,&rockbox_file)||rockbox_file.size<40u||
       rockbox_file.size-8u>LINNET_STAGE_CAPACITY||
       !selector_read(&rockbox_file,0,header,sizeof(header)))return 0;
    static const char model[]=LINNET_FILE_WRAPPER_MODEL;
    for(uint32_t i=0;i<4u;++i)if(header[4u+i]!=(uint8_t)model[i])return 0;
    uint32_t expected=((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|
        ((uint32_t)header[2]<<8)|header[3];
    uint8_t *payload=(uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS;
    uint32_t payload_size=rockbox_file.size-8u,offset=0,sum=LINNET_FILE_WRAPPER_CHECKSUM_SEED;
    while(offset<payload_size){
        uint32_t chunk=payload_size-offset;
        if(chunk>LN_STORAGE_MEDIA_MAX_READ)chunk=LN_STORAGE_MEDIA_MAX_READ;
        if(!selector_read(&rockbox_file,offset+8u,payload+offset,chunk))return 0;
        for(uint32_t i=0;i<chunk;++i)sum+=payload[offset+i];
        offset+=chunk;
    }
    if(sum!=expected||payload_size<40u)return 0;
#if defined(LINNET_TARGET_CLASSIC6G_N25)
    /* S5L runtime vectors end in a trampoline into SDRAM. The eight bytes
     * at offset32 are handler addresses, not the PortalPlayer marker. */
    if(payload_size<72u||ln_read_le32(payload)!=0xea00000du)return 0;
    for(uint32_t i=4u;i<32u;i+=4u)if(ln_read_le32(payload+i)!=0xe59ff014u)return 0;
    uint32_t entry=ln_read_le32(payload+64u);
    if(ln_read_le32(payload+60u)!=0xe51ff004u||(entry&3u)||
       entry<LINNET_LOAD_ADDRESS+68u||entry-LINNET_LOAD_ADDRESS>=payload_size)return 0;
#else
    static const uint8_t rockbox[8]={'R','o','c','k','b','o','x',1};
    for(uint32_t i=0;i<8u;++i)if(payload[32u+i]!=rockbox[i])return 0;
#endif
    if(!selector_quiesce())return 0;
#if defined(LINNET_TARGET_CLASSIC6G_N25)
    /* Rockbox's runtime starts in SDRAM; Apple's NORBOOT starts in SRAM. */
    ln_platform_boot_candidate(true);
    (void)ln_platform_enter(payload,payload_size,LINNET_LOAD_ADDRESS,LINNET_LOAD_ADDRESS);
#else
    (void)ln_platform_enter_retail(payload,payload_size);
#endif
    ln_core_watchdog_arm();
    return 0;
}
#endif
#if LINNET_BOOT_APPLE_FALLBACK
static int selector_apple(void){
    struct ln_fat32_file config_file,firmware_file;
    uint8_t config[512],digest[32];
    if(!selector_open("/.LINNET/APPLE.CFG",&config_file)||config_file.size!=sizeof(config)||
       !selector_read(&config_file,0,config,sizeof(config)))return 0;
    static const uint8_t magic[8]={'A','P','P','L','E','O','S','1'};
    for(uint32_t i=0;i<8u;++i)if(config[i]!=magic[i])return 0;
    if(ln_read_le32(config+8u)!=LINNET_TARGET_NUMBER||ln_crc32(config,508u)!=ln_read_le32(config+508u))return 0;
    for(uint32_t i=48u;i<508u;++i)if(config[i])return 0;
    uint32_t payload_size=ln_read_le32(config+12u);
    if(!payload_size||payload_size>LINNET_STAGE_CAPACITY||
       !selector_open("/.LINNET/APPLE.BIN",&firmware_file)||firmware_file.size!=payload_size)return 0;
    uint8_t *payload=(uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS;
    struct ln_sha256 hash;ln_sha256_init(&hash);
    for(uint32_t offset=0;offset<payload_size;){
        uint32_t chunk=payload_size-offset;
        if(chunk>LN_STORAGE_MEDIA_MAX_READ)chunk=LN_STORAGE_MEDIA_MAX_READ;
        if(!selector_read(&firmware_file,offset,payload+offset,chunk))return 0;
        /* A full disk read can exceed the watchdog's 500 ms progress window
         * when SHA-256 runs at the Boot clock. Authenticate every byte while
         * publishing progress between bounded hash slices. */
        for(uint32_t hashed=0;hashed<chunk;){
            uint32_t slice=chunk-hashed;
            if(slice>8192u)slice=8192u;
            ln_sha256_update(&hash,payload+offset+hashed,slice);
            hashed+=slice;
#if !defined(LINNET_PROFILE_RELEASE)
            ln_recovery_feed();
#else
            ln_core_watchdog_feed();
#endif
        }
        offset+=chunk;
    }
    ln_sha256_finish(&hash,digest);
    for(uint32_t i=0;i<32u;++i)if(digest[i]!=config[16u+i])return 0;
    /* Decode only after authenticating the exact archived bytes. */
    if(!ln_platform_decode_retail_payload(payload,payload_size))return 0;
    if(!selector_quiesce())return 0;
    (void)ln_platform_enter_retail(payload,payload_size);
    ln_core_watchdog_arm();return 0;
}
#endif
#endif
#if defined(LINNET_IMAGE_BOOT)
static void boot_failure_recovery(void){
#if defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_IMAGE_BOOT) && defined(LINNET_PROFILE_RELEASE)
    /* A release Boot has no Link endpoint. Return to the installed monitor
     * after a terminal failure so Media Manager can repair the OS. */
    ln_platform_boot_candidate(false);
    ln_platform_hardware_reset();
#endif
}
#endif
void ln_storage_boot(void){
#if defined(LINNET_IMAGE_BOOT)
#if LINNET_BOOT_APPLE_FALLBACK || LINNET_BOOT_SELECTOR_ROCKBOX
    if(!boot_choice)return;
    /* Input selection can complete before disk spin-up and FAT discovery. Keep
     * the choice pending until selector files can be opened safely. */
    if(!ln_storage_startup_complete()||ln_storage_busy())return;
    if(!ln_storage_mounted()){
        /* Keep the original failure inspectable and leave Link able to launch
         * a corrected RAM image. Repeated automatic remounts never reach idle
         * on an incompatible drive; explicit storage recovery remains available. */
        boot_choice=1u;boot_failure_recovery();return;
    }
    if(boot_choice==4u)boot_choice=ln_storage_boot_default(ln_storage_boot_options());
#if LINNET_BOOT_SELECTOR_ROCKBOX
    if(boot_choice==2u){
        boot_choice=1u;
        (void)selector_rockbox();
        error=404u;
        if(!ln_storage_recover()){error=405u;return;}
        return;
    }
#endif
#if LINNET_BOOT_APPLE_FALLBACK
    if(boot_choice==3u){
        boot_choice=1u;
        (void)selector_apple();
        /* Missing or invalid Apple data never blocks the healthy Linnet path. */
        error=406u;
        if(!ln_storage_recover()){error=407u;return;}
        return;
    }
#endif
#endif
    if(boot_checked||ln_storage_busy())return;
    /* Startup choices can discover directories outside .LINNET, invalidating
     * the FAT ownership proof. Refresh it before opening/writing boot state;
     * leave this attempt pending while the read-only proof runs. */
    int prepared=ln_storage_prepare_writes();
    if(!prepared)return;
    boot_checked=true;
    if(prepared<0||!open_stores()){boot_failure_recovery();return;}
    LN_BOOT_STAMP(LN_BT_BOOT_STORES);
    uint32_t failed=updater.boot.candidate_slot,pending_attempt=updater.boot.attempt_pending;
    if(!ln_update_after_reset(&updater)){boot_failure_recovery();return;}
    if(pending_attempt&&failed<=1u)history_save(1,failed,updater.boot.confirmed_slot,
        updater.boot.attempts,updater.boot.generation,updater.image[failed].build_id);
#if !defined(LINNET_PROFILE_RELEASE)
    /* A recovered Boot stays available for inspection after rollback. */
    if(ln_recovery_generation()!=0)return;
#endif
    uint8_t *scratch=(uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS;struct ln_image_info info;
    LN_BOOT_STAMP(LN_BT_BOOT_VALIDATE);
    uint8_t slot=ln_update_prepare_boot(&updater,scratch,LINNET_STAGE_CAPACITY,&info);
    if(slot<=1u&&updater.ready&&!boot_store.poisoned&&updater.rejected_slot<=1u)history_save(2,updater.rejected_slot,slot,
        updater.boot.attempts,updater.boot.generation,updater.image[updater.rejected_slot].build_id);
    if(slot>1){error=402;boot_failure_recovery();return;}
    LN_BOOT_STAMP(LN_BT_BOOT_VALIDATED);
    uint8_t bytes[80]={0};
    for(uint32_t i=0;i<32;++i){bytes[8+i]=info.build_id[i];bytes[40+i]=updater.image[slot].sha256[i];}
    bytes[0]='H';bytes[1]='A';bytes[2]='N';bytes[3]='D';bytes[4]=slot;
    uint32_t crc=ln_crc32(bytes,76);for(uint32_t i=0;i<4;++i)bytes[76+i]=(uint8_t)(crc>>(i*8));
    handoff[0]=0;for(uint32_t i=1;i<20;++i)handoff[i]=ln_read_le32(bytes+4*i);handoff[0]=ln_read_le32(bytes);
    ln_platform_boot_candidate(updater.boot.attempt_pending&&updater.boot.candidate_slot==slot);
#if !defined(LINNET_PROFILE_RELEASE)
    if(!ln_recovery_chainload(0,scratch+128,info.payload_size,info.load_address,info.entry_address,
       info.build_id,updater.image[slot].sha256,ln_recovery_generation()+1u))error=403;
#else
    if(!ln_storage_handoff_quiesce()||!ln_platform_audio_idle()){handoff[0]=0;error=403;boot_failure_recovery();return;}
    ln_core_watchdog_disarm();ln_platform_quiesce();
    if(!ln_platform_dma_idle()){handoff[0]=0;ln_core_watchdog_arm();error=403;boot_failure_recovery();return;}
#if defined(LINNET_TARGET_CLASSIC6G_N25)
    /* Compact Boot can launch a Developer OS. Preserve the validated image
     * identity in the same retained record used by the SRAM monitor, so Link
     * can identify a cold disk boot as precisely as a RAM transfer. */
    uint8_t *identity=(uint8_t *)(uintptr_t)0x22017e00u;
    ln_write_le32(identity,0);ln_write_le32(identity+4,0);
    for(uint32_t i=0;i<32;++i){identity[8+i]=info.build_id[i];identity[40+i]=updater.image[slot].sha256[i];}
    ln_write_le32(identity,0x484e494cu);
    ln_write_le32(identity+72,ln_crc32(identity,72));
    uint32_t barrier=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(barrier):"memory");
#endif
    (void)ln_platform_enter(scratch+128,info.payload_size,info.load_address,info.entry_address);
    ln_core_watchdog_arm();error=403;
#endif
    handoff[0]=0;boot_failure_recovery();
#else
    (void)boot_checked;
#endif
}
void ln_storage_healthy(uint32_t readiness){
#if defined(LINNET_IMAGE_OS)
    if(health_checked||ln_storage_busy()||
       (readiness&LINNET_BOOT_HEALTH_ALL)!=LINNET_BOOT_HEALTH_ALL)return;
    if(health_open_attempts&&(int32_t)(ln_time_us()-health_retry_at)<0)return;
    uint8_t bytes[80];for(uint32_t i=0;i<20;++i){uint32_t v=handoff[i];for(uint32_t j=0;j<4;++j)bytes[i*4+j]=(uint8_t)(v>>(j*8));}
    if(ln_read_le32(bytes)!=0x444e4148u||bytes[4]>1||ln_crc32(bytes,76)!=ln_read_le32(bytes+76)){
        health_checked=true;handoff[0]=0;return;
    }
    const uint8_t build[32]=LINNET_BUILD_ID_BYTES;
    for(uint32_t i=0;i<32;++i)if(build[i]!=bytes[8+i]){health_checked=true;handoff[0]=0;return;}
    if(!ln_storage_prepared())return;
    if(!open_stores()){
        /* Only read/open failures are retried. An uncertain publication must
         * retain its poison state and must never be automatically rewritten. */
        if(++health_open_attempts>=3u){health_checked=true;error=404;}
        health_retry_at=ln_time_us()+1000000u;return;
    }
    if(updater.upload_active)return;
    health_checked=true;
    if(!ln_update_mark_healthy(&updater,bytes[4],bytes+40,readiness)){error=404;return;}
    LN_BOOT_STAMP(LN_BT_OS_CONFIRMED);
    handoff[0]=0;
#else
    (void)readiness;(void)health_checked;
#endif
}
int ln_storage_update(uint32_t action,const uint8_t *data,uint32_t length,uint32_t offset){
    /* Directory discovery can invalidate the FAT write proof after stores were
     * opened.  Recheck it before every update action; otherwise UPDATE_BEGIN
     * could poison the cached state store when its publication is refused by
     * the lower write guard. */
    if(ln_storage_busy()||!ln_storage_prepared()||!open_stores())return 0;
    if(action==1)return data&&ln_update_begin(&updater,length,data);
    if(action==2)return ln_update_write(&updater,offset,data,length);
    if(action==3){
#if defined(LINNET_COVERFLOW_PACKED)
        if(!ln_coverflow_stage_prepare())return 0;
#endif
        return ln_update_finish(&updater,
        (uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS,LINNET_STAGE_CAPACITY);
    }
    return 0;
}
void ln_storage_update_snapshot(uint32_t words[16]){
    if(!words)return;
    int available=stores_ready&&ln_storage_prepared();
    uint32_t v[16]={0x41545355u,(uint32_t)available,updater.ready,boot_store.generation,updater.boot.generation,
        updater.boot.confirmed_slot,updater.boot.candidate_slot,updater.boot.attempt_pending,updater.boot.attempts,
        updater.image[0].length,updater.image[1].length,updater.upload_active,updater.upload_slot,updater.upload_offset,boot_store.poisoned,error};
    for(uint32_t i=0;i<16;++i)words[i]=v[i];
}
void ln_storage_update_state(uint32_t words[16]){
    (void)open_stores();
    ln_storage_update_snapshot(words);
}
int ln_storage_settings(bool write,uint8_t *data,uint32_t *length){
    if(!data||!length||!open_stores())return 0;
    if(write)return *length<=480&&ln_state_publish(&settings_store,data,*length);
    if(*length<settings_store.length)return 0;*length=settings_store.length;
    for(uint32_t i=0;i<*length;++i)data[i]=settings_store.payload[i];return 1;
}
int ln_storage_theme(bool write,uint8_t *data,uint32_t *length){
    if(!data||!length)return -1;
    if(ln_storage_busy()||!ln_storage_prepared())return 0;
    if(!open_stores())return -1;
    if(!theme_opened){theme_open();theme_opened=true;}
    if(!theme_available||!theme_store.ready||theme_store.poisoned)return -1;
    if(write){
        if(*length>256u)return -1;
        if(!ln_state_publish(&theme_store,data,*length)){theme_available=false;return -1;}
        return 1;
    }
    if(*length<theme_store.length)return -1;
    *length=theme_store.length;
    for(uint32_t i=0;i<*length;++i)data[i]=theme_store.payload[i];return 1;
}
int ln_storage_file_resize(uint32_t action,uint32_t length){
    if(locked||!ln_storage_prepared()||!length||length>0x100000u)return 0;
    bool accepted=false;locked=true;
    if(action==1&&!(found&128u))accepted=ln_fat32_create(&volume,paths[7],length,next_generation());
    if(action==2&&(found&128u))accepted=ln_fat32_grow(&volume,&files[7],length,next_generation());
    if(!accepted){locked=false;return 0;}
    if(!finish())return 0;
    return ln_storage_recover();
}
