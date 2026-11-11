#include "hal/pp502x/soc.h"
#include "hal/api/ata.h"
#include "hal/pp502x/target.h"
#include "hal/api/platform.h"
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define R8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define DATA (*(volatile uint16_t *)(uintptr_t)0xc30001e0u)
#define ERROR R8(0xc30001e4u)
#define COUNT R8(0xc30001e8u)
#define LOW R8(0xc30001ecu)
#define MID R8(0xc30001f0u)
#define HIGH R8(0xc30001f4u)
#define SELECT R8(0xc30001f8u)
#define COMMAND R8(0xc30001fcu)
#define CONTROL R8(0xc30003f8u)
enum phase { OFF,POWER,RESET_ASSERT,RESET_SETTLE,INIT_READY,IDENT_DATA,IDENT_DONE,MODE_DONE,IDLE,TRANSFER,FINISH,STANDBY_FLUSH,LBA_PROBE_DONE,APM_DONE,DMA_MODE_DONE,DMA_TRANSFER };
static enum phase phase;
static struct ln_ata_info info;
static struct ln_ata_result result;
static bool result_ready;
static uint16_t identify[256] __attribute__((aligned(32)));
static uint32_t deadline,not_before,done,count;
static uint8_t *buffer;
static enum ln_ata_op operation;
static uint64_t request_lba;
static uint32_t physical_sectors=1,transfer_done,transfer_count,edge_offset,edge_count;
static uint8_t *transfer_buffer;
static enum ln_ata_op transfer_operation;
static bool transfer_edge;
static uint8_t edge_buffer[4096] __attribute__((aligned(32)));
static uint64_t previous_capacity;
static uint8_t flush_command;
static uint8_t pio_mode;
#if (defined(LINNET_SOC_PP5022) || LINNET_TARGET_NUMBER == 98) && defined(LINNET_IMAGE_OS)
#define ATA_READ_DMA 1
#if LINNET_TARGET_NUMBER == 98
#define ATA_DMA_MODE 1u
#else
#define ATA_DMA_MODE 2u
#endif
#define IDE_CFG R32(0xc3000028u)
#define IDE_DMA_CONTROL R32(0xc3000400u)
#if defined(LINNET_SOC_PP5022)
#define ATA_DMA_BUFFER_BYTES 65536u
#else
#define ATA_DMA_BUFFER_BYTES 32768u
#endif
static uint8_t dma_buffer[ATA_DMA_BUFFER_BYTES] __attribute__((aligned(32)));
static bool dma_enabled,dma_failed;
static uint8_t dma_mode;
static uint8_t *dma_bytes(void){
    return (uint8_t *)ln_platform_uncached_address((uintptr_t)dma_buffer);
}
#if defined(LINNET_SOC_PP5022)
uint8_t *ln_pp_ata_disk_mode_buffer(void){return dma_bytes();}
#endif
static void dma_stop(void){
    IDE_CFG&=~0x8000u;
    IDE_DMA_CONTROL&=~0x80000001u;
    CONTROL=2u;
}
#else
#define ATA_READ_DMA 0
#endif
#if LINNET_ATA_TRACK_FLUSH
/* Unknown after reset/error; only a completed flush proves the cache clean. */
static bool write_dirty=true;
#endif
static bool needs_flush(void){
#if LINNET_ATA_TRACK_FLUSH
    if(info.media_kind==LN_ATA_MEDIA_ROTATING&&!write_dirty)return false;
#endif
    return info.can_flush;
}
static uint32_t hdd_idle_us,standby_started;
static bool recent_standby;
/* Back off repeated short rests, but forget the backoff after two minutes
 * asleep. Expire in step as well as on wake so the microsecond clock can wrap. */
static void hdd_idle_expire(uint32_t now) {
    if(recent_standby&&now-standby_started>=120000000u){
        recent_standby=false;hdd_idle_us=10000000u;
        if(info.media_kind==LN_ATA_MEDIA_ROTATING)info.idle_timeout_us=hdd_idle_us;
    }
}
static void hdd_wake(uint32_t now) {
    hdd_idle_expire(now);
    if(info.media_kind==LN_ATA_MEDIA_ROTATING&&recent_standby){
        hdd_idle_us=hdd_idle_us<30000000u?hdd_idle_us*2u:60000000u;
        info.idle_timeout_us=hdd_idle_us;
    }
    recent_standby=false;
}
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t init_started;
#define ATA_RECORD(index, value) (info.diagnostic[(index)] = (value))
#else
#define ATA_RECORD(index, value) ((void)0)
#endif
static bool lba_probe_required;
static char previous_model[41],previous_serial[21],previous_firmware[9];
#if !defined(LINNET_PROFILE_RELEASE)
/* Separate from info: that structure is copied on every block-service call. */
static struct {
    uint32_t reads,terminal,wall,max_wall,sector,max_sector,polls,busy,no_drq,epoch,started;
    bool active,saturated;
} perf;
static void perf_add(uint32_t *value,uint32_t amount){
    if(amount>=UINT32_MAX-*value){*value=UINT32_MAX;perf.saturated=true;}
    else *value+=amount;
}
static void perf_end(uint32_t now){
    if(!perf.active)return;
    uint32_t elapsed=now-perf.started;perf_add(&perf.terminal,1u);perf_add(&perf.wall,elapsed);
    if(elapsed>perf.max_wall)perf.max_wall=elapsed;perf.active=false;
}
#endif
static void ata_string(char *out,uint32_t first,uint32_t length) {
    for(uint32_t i=0;i<length;++i){uint8_t c=(uint8_t)(identify[first+i/2u]>>(i%2u?0u:8u));out[i]=(c>=32u&&c<=126u)?(char)c:' ';}
    uint32_t capacity=length;
    while(length&&out[length-1u]==' ')--length;
    for(uint32_t i=length;i<=capacity;++i)out[i]=0;
}
static bool same_string(const char*a,const char*b,uint32_t n){for(uint32_t i=0;i<n;++i)if(a[i]!=b[i])return false;return true;}
static void settle(void) { for(uint32_t i=0;i<4u;++i)(void)CONTROL; }
static void command(uint8_t value) {
    ATA_RECORD(3, value);
    COMMAND=value;settle();not_before=ln_time_us()+1u;++info.commands;
}
static void read_sector(uint8_t *sector) {
    if((uintptr_t)sector&3u){
        for(uint32_t i=0;i<256u;++i){uint16_t word=DATA;sector[i*2u]=(uint8_t)word;sector[i*2u+1u]=(uint8_t)(word>>8);}
        return;
    }
    /* Eight ordered 16-bit port reads per iteration, packed into four aligned
     * stores. The device still sees exactly one 512-byte sector per step. */
    uint32_t words=256u,low,high;
    __asm__ volatile(
        "1:\n"
        "ldrh %[low], [%[port]]\nldrh %[high], [%[port]]\n"
        "orr %[low], %[low], %[high], lsl #16\nstr %[low], [%[out]], #4\n"
        "ldrh %[low], [%[port]]\nldrh %[high], [%[port]]\n"
        "orr %[low], %[low], %[high], lsl #16\nstr %[low], [%[out]], #4\n"
        "ldrh %[low], [%[port]]\nldrh %[high], [%[port]]\n"
        "orr %[low], %[low], %[high], lsl #16\nstr %[low], [%[out]], #4\n"
        "ldrh %[low], [%[port]]\nldrh %[high], [%[port]]\n"
        "orr %[low], %[low], %[high], lsl #16\nstr %[low], [%[out]], #4\n"
        "subs %[words], %[words], #8\nbne 1b\n"
        : [out] "+&r"(sector), [words] "+&r"(words), [low] "=&r"(low), [high] "=&r"(high)
        : [port] "r"((uintptr_t)0xc30001e0u) : "cc", "memory");
}
static void write_sector(const uint8_t *sector){
#if LINNET_ATA_ALIGNED_WRITE
    if(!((uintptr_t)sector&1u)){
        typedef uint16_t sector_word __attribute__((may_alias));
        const sector_word *words=(const sector_word *)sector;
        /* Ordered 16-bit PIO writes, with aligned RAM loads and less loop work. */
        for(uint32_t i=0;i<256u;i+=4u){
            DATA=words[i];DATA=words[i+1u];DATA=words[i+2u];DATA=words[i+3u];
        }
        return;
    }
#endif
    for(uint32_t i=0;i<256u;++i)
        DATA=(uint16_t)((uint16_t)sector[i*2u]|((uint16_t)sector[i*2u+1u]<<8));
}
static void fail(uint32_t status) {
#if ATA_READ_DMA
    if(phase==DMA_TRANSFER){dma_stop();dma_enabled=false;dma_failed=true;}
#endif
#if LINNET_ATA_TRACK_FLUSH
    write_dirty=true;
#endif
    recent_standby=false;
    info.power_safe=0;
    ATA_RECORD(1, (uint32_t)phase);ATA_RECORD(2, status);
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t failed_at=ln_time_us();ATA_RECORD(4, failed_at-init_started);
    perf_end(failed_at);
#endif
    info.last_status=COMMAND;info.last_error=ERROR;
    if(status==LN_ATA_TIMEOUT)++info.timeouts;else ++info.errors;
    if(!info.initializing){result=(struct ln_ata_result){status,done};result_ready=true;}
    info.initializing=info.initialized=info.busy=0;info.failed=1;phase=OFF;
}
static void finish(void) {
#if !defined(LINNET_PROFILE_RELEASE)
    if(perf.active)perf_end(ln_time_us());
#endif
#if LINNET_ATA_TRACK_FLUSH
    if(operation==LN_ATA_FLUSH)write_dirty=false;
#endif
    if(operation==LN_ATA_STANDBY){
        if(info.media_kind==LN_ATA_MEDIA_ROTATING&&info.standby_supported){
            standby_started=ln_time_us();recent_standby=true;
        }
        if(info.standby_supported)info.sleeping=1;
        info.power_safe=info.can_power_off&&(info.standby_supported||info.can_flush);
    }
    else if(operation==LN_ATA_WAKE||operation==LN_ATA_READ||operation==LN_ATA_WRITE)info.sleeping=0;
    result=(struct ln_ata_result){LN_ATA_OK,done};result_ready=true;info.busy=0;phase=IDLE;
}
static bool identify_valid(void) {
    /* ATA's packet-device bit has CFA exceptions; Linux ata_id_is_cfa
     * documents848a (traditional CF) and844a (Delkin). */
    bool compact_flash=identify[0]==0x848au||identify[0]==0x844au||
        (identify[83]&0xc004u)==0x4004u;
    ATA_RECORD(21, 1);
    if((identify[0]&0x8000u)&&!compact_flash)return false;
    ATA_RECORD(21, 2);
    /* The installed CFA adapter reports valid LBA capacities/CFA support but
     * a malformed word49 (0x6c61). Rockbox uses the capacity words directly.
     * Admit that CFA shape only provisionally; READ VERIFY with SELECT_LBA
     * must succeed before exposing an initialized block device. */
    lba_probe_required=!(identify[49]&0x200u);
    if(lba_probe_required&&!compact_flash)return false;
    info.compact_flash=compact_flash;
    info.quirks=(lba_probe_required?1u:0u)|(compact_flash?4u:0u);
    info.logical_sector_bytes=512u;info.physical_sector_bytes=512u;
    if((identify[106]&0xe000u)==0x6000u)info.physical_sector_bytes=512u<<(identify[106]&15u);
    if(info.physical_sector_bytes>sizeof(edge_buffer)||
       ((identify[209]&0xc000u)==0x4000u&&(identify[209]&0x3fffu)))return false;
    physical_sectors=info.physical_sector_bytes>>9;
    ATA_RECORD(21, 3);
    if((identify[106]&0xd000u)==0x5000u&&
        (identify[117]!=256u||identify[118]))return false;
    if((identify[255]&255u)==0xa5u){
        ATA_RECORD(21, 4);
        uint32_t sum=0;for(uint32_t i=0;i<256u;++i)sum+=(identify[i]&255u)+(identify[i]>>8);
        if(sum&255u)return false;
    }
    uint64_t sectors=(uint32_t)identify[60]|((uint32_t)identify[61]<<16);
    info.lba48=(identify[83]&0xc400u)==0x4400u;
    if(info.lba48){
        uint64_t extended=(uint64_t)identify[100]|((uint64_t)identify[101]<<16)|((uint64_t)identify[102]<<32);
        ATA_RECORD(21, 5);if(identify[103])return false;
        ATA_RECORD(21, 6);if(!extended)return false;
        sectors=extended;
    }else {ATA_RECORD(21, 7);if(sectors>0x10000000u)return false;}
    ATA_RECORD(21, 8);if(!sectors||(sectors&(physical_sectors-1u)))return false;
    ATA_RECORD(21, 9);if(previous_capacity&&sectors!=previous_capacity)return false;
    ata_string(info.model,27u,40u);ata_string(info.serial,10u,20u);ata_string(info.firmware,23u,8u);
    ATA_RECORD(21, 10);
    if(previous_capacity&&(!same_string(previous_model,info.model,41u)||!same_string(previous_serial,info.serial,21u)||!same_string(previous_firmware,info.firmware,9u)))return false;
    info.sectors=sectors;
    bool iflash=same_string(info.model,"iFlash-Platform iPod Adapters",30u);
    if(iflash)info.quirks|=8u;
    info.rotation_rpm=identify[217]>=0x401u&&identify[217]!=0xffffu?identify[217]:0u;
    info.media_kind=(iflash||identify[217]==1u)?LN_ATA_MEDIA_SOLID_STATE:(info.rotation_rpm?LN_ATA_MEDIA_ROTATING:LN_ATA_MEDIA_UNKNOWN);
    bool identify_media_known=info.media_kind!=LN_ATA_MEDIA_UNKNOWN;
    if(!identify_media_known)ln_target_ata_classify(info.model,&info.media_kind,&info.rotation_rpm);
    bool commands_valid=(identify[83]&0xc000u)==0x4000u;
    bool enabled_valid=(identify[87]&0xc000u)==0x4000u;
    info.standby_supported=commands_valid&&enabled_valid&&(identify[82]&identify[85]&8u);
    info.apm_supported=commands_valid&&(identify[83]&8u);
    info.write_cache_enabled=commands_valid&&enabled_valid&&(identify[82]&identify[85]&32u);
    /* A fallback target classification improves diagnostics and policy timing,
     * but does not by itself authorize rail removal.  Keep that dependent on
     * an ATA-native media claim (or the separately handled exact iFlash case). */
    info.can_power_off=identify_media_known&&info.standby_supported;
    info.idle_timeout_us=info.media_kind==LN_ATA_MEDIA_SOLID_STATE?5000000u:
        info.media_kind==LN_ATA_MEDIA_ROTATING?hdd_idle_us:10000000u;
    info.wake_timeout_us=30000000u;
    info.can_flush=((identify[83]&0xc000u)==0x4000u&&(identify[83]&0x3000u))||
        (identify[80]!=0xffffu&&(identify[80]&0x7fe0u));
    if(iflash&&info.can_flush)info.can_power_off=1;
    flush_command=(info.lba48&&(identify[83]&0x2000u))?0xeau:0xe7u;
    info.capabilities=(info.lba48?1u:0u)|(info.can_flush?2u:0u)|(info.standby_supported?4u:0u)|(info.apm_supported?8u:0u)|(info.write_cache_enabled?16u:0u);
    ATA_RECORD(21, 0);
    return true;
}
static void reset_begin(uint32_t now) {
#if LINNET_ATA_TRACK_FLUSH
    write_dirty=true;
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    /* A reset invalidates cross-epoch deltas, including an interrupted read. */
    perf.active=false;perf_add(&perf.epoch,1u);
    init_started=now;for(uint32_t i=0;i<24u;++i)info.diagnostic[i]=0;
#endif
#if ATA_READ_DMA
    if(phase==DMA_TRANSFER)dma_stop();
    dma_enabled=false;
#endif
    for(uint32_t i=0;i<256u;++i)identify[i]=0;
    info.power_safe=0;
    info.initializing=1;info.initialized=info.failed=info.busy=info.sleeping=0;
    result_ready=false;done=0;deadline=now+30000000u;not_before=now+250000u;phase=POWER;
    ln_target_ata_power(true);
    info.powered=1;
}
static void controller_setup(void) {
#include "third_party/rockbox_hw/pp502x_ata_setup.inc"
}
void ln_ata_init(uint32_t now) {
#if ATA_READ_DMA
    dma_failed=false;
#endif
    info=(struct ln_ata_info){0};previous_capacity=0;
    hdd_idle_us=10000000u;recent_standby=false;reset_begin(now);
}
void ln_ata_recover(uint32_t now) {
    previous_capacity=info.sectors;
    for(uint32_t i=0;i<41u;++i)previous_model[i]=info.model[i];
    for(uint32_t i=0;i<21u;++i)previous_serial[i]=info.serial[i];
    for(uint32_t i=0;i<9u;++i)previous_firmware[i]=info.firmware[i];
    reset_begin(now);
}
bool ln_ata_power_off(void) {
    if(!info.initialized||info.busy||result_ready||phase!=IDLE||!info.power_safe||!info.can_power_off)return false;
    ln_target_ata_power(false);
    info.powered=0;info.initialized=0;phase=OFF;++info.poweroffs;return true;
}
bool ln_ata_power_on(uint32_t now) {
    if(info.powered||info.busy||info.initializing||result_ready||!info.sectors)return false;
    hdd_wake(now);++info.spinups;ln_ata_recover(now);return true;
}
static void transfer_begin(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *bytes){
#if ATA_READ_DMA
    bool dma_transfer=dma_enabled&&op==LN_ATA_READ;
#if defined(LINNET_SOC_PP5022) || LINNET_TARGET_NUMBER == 98
    /* Negotiated DMA applies to flash adapters regardless of model string.
     * Keep known rotating drives on their faster established PIO write path. */
    if(dma_enabled&&op==LN_ATA_WRITE&&info.media_kind!=LN_ATA_MEDIA_ROTATING)dma_transfer=true;
#endif
    if(dma_transfer&&sectors>sizeof(dma_buffer)/512u)
        sectors=sizeof(dma_buffer)/512u;
#endif
    transfer_operation=op;transfer_count=sectors;transfer_done=0;transfer_buffer=bytes;
    bool extended=lba+sectors>0x10000000u||sectors>256u;
    SELECT=0xe0u;settle();
    if(extended){
        COUNT=(uint8_t)(sectors>>8);LOW=(uint8_t)(lba>>24);MID=(uint8_t)(lba>>32);HIGH=(uint8_t)(lba>>40);
    }else SELECT=(uint8_t)(0xe0u|((lba>>24)&15u));
    COUNT=(uint8_t)sectors;LOW=(uint8_t)lba;MID=(uint8_t)(lba>>8);HIGH=(uint8_t)(lba>>16);
#if LINNET_ATA_TRACK_FLUSH
    if(op==LN_ATA_WRITE)write_dirty=true;
#endif
#if ATA_READ_DMA
    if(dma_transfer){
        /* Only the uncached alias touches this dedicated buffer. Other callers
         * copy at the boundary; Disk Mode can own the buffer directly. */
        if(op==LN_ATA_WRITE&&bytes!=dma_bytes()){
            extern void ln_memory_copy(void *,const void *,uint32_t);
            ln_memory_copy(dma_bytes(),bytes,sectors*512u);
        }
        /* PIO uses alternate status, leaving the previous device INTRQ
         * asserted. Acknowledge it before unmasking and clearing the latch. */
        (void)COMMAND;CONTROL=0u;settle();IDE_CFG|=8u;
        IDE_DMA_CONTROL|=2u;
        R32(0xc3000408u)=sectors*512u-4u;
        R32(0xc300040cu)=(uint32_t)(uintptr_t)dma_bytes();
        if(op==LN_ATA_READ)IDE_DMA_CONTROL|=8u;
        else IDE_DMA_CONTROL&=~8u;
        IDE_CFG|=0x8000u;
        uint8_t cmd=op==LN_ATA_READ?(extended?0x25u:0xc8u):(extended?0x35u:0xcau);
        ATA_RECORD(3,cmd);COMMAND=cmd;++info.commands;
        IDE_DMA_CONTROL|=1u;not_before=ln_time_us()+1u;
        phase=DMA_TRANSFER;return;
    }
#endif
    command(op==LN_ATA_READ?(extended?0x24u:0x20u):(extended?0x34u:0x30u));phase=TRANSFER;
}
static void transfer_next(void){
    uint64_t lba=request_lba+done;
    uint32_t remaining=count-done;
    edge_offset=(uint32_t)lba&(physical_sectors-1u);
    transfer_edge=edge_offset||remaining<physical_sectors;
    if(transfer_edge){
        edge_count=physical_sectors-edge_offset;if(edge_count>remaining)edge_count=remaining;
        /* Read the complete physical sector before returning a partial read
         * or changing just the requested bytes of a partial write. */
        transfer_begin(LN_ATA_READ,lba-edge_offset,physical_sectors,edge_buffer);
    }else{
        uint32_t sectors=remaining&~(physical_sectors-1u);
        if(!info.lba48&&sectors>256u)sectors=256u;
        transfer_begin(operation,lba,sectors,buffer+done*512u);
    }
}
bool ln_ata_start(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *bytes,uint32_t limit) {
    if(!info.initialized||info.busy||result_ready||phase!=IDLE||op<LN_ATA_READ||op>LN_ATA_WAKE)return false;
    uint32_t now=ln_time_us();if((int32_t)(limit-now)<=0||(uint32_t)(limit-now)>30000000u)return false;
    bool data=op==LN_ATA_READ||op==LN_ATA_WRITE;
    if(data&&(!bytes||!sectors||
        sectors>(op==LN_ATA_READ?LINNET_ATA_MAX_READ_SECTORS:LINNET_ATA_MAX_WRITE_SECTORS)||
        lba>=info.sectors||sectors>info.sectors-lba))return false;
    if(!data&&(sectors||bytes))return false;
    if(op==LN_ATA_WRITE&&!info.can_flush)return false;
    if(op==LN_ATA_WAKE||data)info.power_safe=0;
    if(info.sleeping&&(op==LN_ATA_WAKE||data)){hdd_wake(now);++info.spinups;}
    operation=op;buffer=bytes;count=sectors;done=0;request_lba=lba;deadline=limit;info.busy=1;
    CONTROL=2u;SELECT=0xe0u;settle();
    if(!data){
        if(op==LN_ATA_FLUSH&&!needs_flush()){finish();return true;}
        if(op==LN_ATA_STANDBY&&!info.standby_supported&&!info.can_flush){finish();return true;}
        if(op==LN_ATA_STANDBY&&needs_flush()){command(flush_command);phase=STANDBY_FLUSH;return true;}
        if(op==LN_ATA_WAKE){COUNT=(uint8_t)physical_sectors;LOW=MID=HIGH=0;command(0x40u);}
        else command(op==LN_ATA_FLUSH?flush_command:0xe0u);
        phase=FINISH;return true;
    }
#if !defined(LINNET_PROFILE_RELEASE)
    if(op==LN_ATA_READ){perf.started=now;perf.active=true;perf_add(&perf.reads,1u);}
#endif
    transfer_next();return true;
}
void ln_ata_step(uint32_t now) {
    hdd_idle_expire(now);
    if(phase==OFF||phase==IDLE)return;
    if((int32_t)(now-deadline)>=0){fail(LN_ATA_TIMEOUT);return;}
    if((int32_t)(now-not_before)<0)return;
    if(phase==POWER){controller_setup();CONTROL=6u;not_before=now+5u;phase=RESET_ASSERT;return;}
    if(phase==RESET_ASSERT){CONTROL=2u;not_before=now+2000u;phase=RESET_SETTLE;return;}
    if(phase==RESET_SETTLE){SELECT=0xa0u;settle();phase=INIT_READY;return;}
#if ATA_READ_DMA
    if(phase==DMA_TRANSFER){
        /* Reading ATA status before INTRQ can hang this controller. */
        if(!(IDE_CFG&8u))return;
        dma_stop();
        uint8_t status=COMMAND;info.last_status=status;
        if(!status||status==0xffu||(status&0xa9u)){
            dma_enabled=false;dma_failed=true;fail(LN_ATA_IO);return;
        }
        if(transfer_operation==LN_ATA_READ){
            extern void ln_memory_copy(void *,const void *,uint32_t);
            if(transfer_buffer!=dma_bytes())
                ln_memory_copy(transfer_buffer,dma_bytes(),transfer_count*512u);
            info.sectors_read+=transfer_count;
        }else info.sectors_written+=transfer_count;
        transfer_done=transfer_count;
        phase=FINISH;
    }
#endif
    uint8_t status=CONTROL;info.last_status=status;
#if !defined(LINNET_PROFILE_RELEASE)
    if(perf.active&&(phase==TRANSFER||phase==FINISH)){
        perf_add(&perf.polls,1u);
        if(status&0x80u)perf_add(&perf.busy,1u);
        else if(phase==TRANSFER&&status&&status!=0xffu&&!(status&0x21u)&&!(status&8u))perf_add(&perf.no_drq,1u);
    }
#endif
    if(status&0x80u){
        /* Mechanical wake/reset takes seconds. Let the executive sleep between
         * checks instead of polling every100us; data transfers keep their fast
         * service cadence and every wait remains bounded by the deadline. */
        if(info.media_kind==LN_ATA_MEDIA_ROTATING&&
           (phase==INIT_READY||(phase==FINISH&&operation==LN_ATA_WAKE)))not_before=now+10000u;
        return;
    }
    if(!status||status==0xffu){if(phase!=INIT_READY)fail(LN_ATA_OFFLINE);return;}
    if(phase==INIT_READY){if(!(status&0x40u))return;command(0xecu);phase=IDENT_DATA;return;}
    if(status&0x21u){
#if ATA_READ_DMA
        if(phase==DMA_MODE_DONE&&!(status&0x20u)&&(ERROR&4u)){
            dma_failed=true;status&=(uint8_t)~1u;
        }else
#endif
        if(phase==APM_DONE&&!(status&0x20u)&&(ERROR&4u)){info.quirks|=2u;status&=(uint8_t)~1u;}
        else {fail(LN_ATA_IO);return;}
    }
    if(phase==IDENT_DATA){
        if(!(status&8u))return;
        for(uint32_t i=0;i<256u;++i)identify[i]=DATA;
        settle();phase=IDENT_DONE;return;
    }
    if(phase==IDENT_DONE){
        if(status&8u)return;
        if(!identify_valid()){fail(LN_ATA_UNSUPPORTED);return;}
        pio_mode=0;
        if(identify[53]&2u){if(identify[64]&2u)pio_mode=4u;else if(identify[64]&1u)pio_mode=3u;}
        static const uint32_t timings[5]={0xc293u,0x43a2u,0x11a1u,0x7232u,0x3131u};
        R32(0xc3000000u)=timings[pio_mode];ATA_RECORD(22, pio_mode);
        ERROR=3u;COUNT=(uint8_t)(8u+pio_mode);command(0xefu);phase=MODE_DONE;return;
    }
#if ATA_READ_DMA
    if(phase==MODE_DONE&&!dma_failed&&(identify[53]&4u)&&identify[88]!=0xffffu&&
       (identify[88]&((1u<<(ATA_DMA_MODE+1u))-1u))&&
       ((identify[49]&0x100u)||info.compact_flash)&&
       (R32(0x60006020u)&65535u)==0x7777u&&R32(0x60006034u)==LN_PLL_80&&
       R32(0x600060c4u)==0xc0000000u){
        if(status&8u){fail(LN_ATA_IO);return;}
        dma_mode=ATA_DMA_MODE;
        while(!(identify[88]&(1u<<dma_mode)))--dma_mode;
        ERROR=3u;COUNT=0x40u+dma_mode;command(0xefu);phase=DMA_MODE_DONE;return;
    }
    if(phase==DMA_MODE_DONE){
        if(status&8u){fail(LN_ATA_IO);return;}
        if(!dma_failed){
            IDE_CFG=(IDE_CFG&~0x10000000u)|0x20000000u;
            static const uint32_t timings[]={0x800037c1u,0x80003491u,0x80003371u};
            R32(0xc3000004u)=timings[dma_mode];dma_enabled=true;
        }
    }
#endif
    if((phase==MODE_DONE||phase==DMA_MODE_DONE)&&info.apm_supported){
        if(status&8u){fail(LN_ATA_IO);return;}
        ERROR=5u;COUNT=0x80u;command(0xefu);phase=APM_DONE;return;
    }
    if(phase==MODE_DONE||phase==DMA_MODE_DONE||phase==APM_DONE){if(status&8u){fail(LN_ATA_IO);return;}
        if(lba_probe_required){
            SELECT=0xe0u;settle();COUNT=1u;LOW=MID=HIGH=0;
            command(0x40u);phase=LBA_PROBE_DONE;return;
        }
        info.initializing=0;info.initialized=1;phase=IDLE;return;}
    if(phase==LBA_PROBE_DONE){
        if(status&8u||!(status&0x40u)){fail(LN_ATA_IO);return;}
        info.initializing=0;info.initialized=1;phase=IDLE;return;
    }
    if(phase==TRANSFER){
        uint32_t service_started=ln_time_us(),serviced=0;
        if(!(status&8u))return;
        for(;;){
        uint8_t *sector=transfer_buffer+transfer_done*512u;
        if(transfer_operation==LN_ATA_READ){
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t started=ln_time_us();
#endif
            read_sector(sector);
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t elapsed=ln_time_us()-started;perf_add(&perf.sector,elapsed);
            if(elapsed>perf.max_sector)perf.max_sector=elapsed;
#endif
        }
        else write_sector(sector);
        ++transfer_done;if(transfer_operation==LN_ATA_READ)++info.sectors_read;else ++info.sectors_written;
        settle();if(transfer_done==transfer_count){phase=FINISH;return;}
        if(++serviced==LINNET_ATA_MAX_SECTORS||ln_time_us()-service_started>=500u)return;
        /* A multi-sector command may already be presenting its next sector.
         * Drain only an immediately ready clean DRQ; otherwise yield and let
         * the ordinary deadline/error path inspect it on the next step. */
        status=CONTROL;info.last_status=status;
        if(!status||status==0xffu){fail(LN_ATA_OFFLINE);return;}
        if(status&0x21u){fail(LN_ATA_IO);return;}
        if(status&0x80u||!(status&8u))return;
        }
    }
    if(status&8u){fail(LN_ATA_IO);return;}
    if(phase==STANDBY_FLUSH){
#if LINNET_ATA_TRACK_FLUSH
        write_dirty=false;
#endif
        if(!info.standby_supported){finish();return;}command(0xe0u);phase=FINISH;return;
    }
    if(operation==LN_ATA_READ||operation==LN_ATA_WRITE){
        if(transfer_edge){
            uint8_t *edge=edge_buffer+edge_offset*512u,*caller=buffer+done*512u;
            if(transfer_operation==LN_ATA_READ){
                if(operation==LN_ATA_WRITE){
                    for(uint32_t i=0;i<edge_count*512u;++i)edge[i]=caller[i];
                    transfer_begin(LN_ATA_WRITE,request_lba+done-edge_offset,physical_sectors,edge_buffer);return;
                }
                for(uint32_t i=0;i<edge_count*512u;++i)caller[i]=edge[i];
            }
            done+=edge_count;
        }else done+=transfer_count;
        if(done<count){transfer_next();return;}
    }
    finish();
}
bool ln_ata_take(struct ln_ata_result *out) { if(!out||!result_ready)return false;*out=result;result_ready=false;return true; }
void ln_ata_snapshot(struct ln_ata_info *out) { if(out)*out=info; }
void ln_ata_state_snapshot(struct ln_ata_state *out) {
    if(!out)return;
    out->sectors=info.sectors;
    out->initialized=info.initialized;
    out->initializing=info.initializing;
    out->busy=info.busy;
    out->sleeping=info.sleeping;
    out->failed=info.failed;
    out->can_flush=info.can_flush;
    out->powered=info.powered;
    out->standby_supported=info.standby_supported;
    out->can_power_off=info.can_power_off;
    out->power_safe=info.power_safe;
    out->idle_timeout_us=info.idle_timeout_us;
    out->media_kind=info.media_kind;
}

#if !defined(LINNET_PROFILE_RELEASE)
void ln_ata_diagnostics(uint32_t d[24]) {
    if(!d)return;
    for(uint32_t i=0;i<24u;++i)d[i]=info.diagnostic[i];
    d[0]=(uint32_t)phase;
    if(!info.failed)d[4]=ln_time_us()-init_started;
    d[5]=identify[0]|(uint32_t)identify[49]<<16;d[6]=identify[60]|(uint32_t)identify[61]<<16;
    d[7]=identify[83]|(uint32_t)identify[80]<<16;d[8]=identify[100]|(uint32_t)identify[101]<<16;
    d[9]=identify[102]|(uint32_t)identify[103]<<16;d[10]=identify[106]|(uint32_t)identify[117]<<16;
    d[11]=identify[118]|(uint32_t)identify[255]<<16;
    d[12]=R32(0xc3000028u);d[13]=R32(0xc3000000u);d[14]=R32(0xc3000004u);
    d[15]=R32(0x6000600cu);d[16]=R32(0x60006004u);d[17]=R32(0x6000d124u);
#if ATA_READ_DMA
    if(phase==DMA_TRANSFER){
        d[18]=R32(0x6000d100u);d[19]=R32(0x6000d108u);
        d[20]=info.last_status;d[22]=d[23]=0;return;
    }
#endif
    d[18]=R32(0x6000d100u);d[19]=R32(0x6000d108u);d[20]=CONTROL;
    d[22]=SELECT|(uint32_t)COUNT<<8|(uint32_t)LOW<<16|(uint32_t)MID<<24;
    d[23]=HIGH|(uint32_t)ERROR<<8;
}
void ln_ata_clock_diagnostics(uint32_t out[8]){
    bool valid=info.powered&&(R32(0x6000600cu)&0x02000000u)&&!(R32(0x60006004u)&0x02000000u);
    out[0]=0x464e4941u;out[1]=0u;out[2]=valid;out[3]=R32(0x600060c4u);
    out[4]=valid?R32(0xc3000028u):0u;out[5]=valid?R32(0xc3000000u):0u;
    out[6]=info.initialized?pio_mode:0xffffffffu;out[7]=R32(0x60006020u);
}
void ln_ata_capabilities(uint32_t out[16]){
    uint32_t enabled=R32(0x6000600cu),reset=R32(0x60006004u);
    bool timing_valid=info.powered&&(enabled&0x02000000u)&&!(reset&0x02000000u);
    out[0]=0x50414341u;out[1]=0u;out[2]=ln_time_us();
    out[3]=(info.initialized?1u:0u)|(info.initializing?2u:0u)|(info.busy?4u:0u)|
        (info.powered?8u:0u)|(info.failed?16u:0u)|(timing_valid?32u:0u);
    out[4]=info.initialized?identify[53]:0u;out[5]=info.initialized?identify[64]:0u;
    out[6]=info.initialized?identify[67]:0u;out[7]=info.initialized?identify[68]:0u;
    out[8]=timing_valid?R32(0xc3000028u):0u;
    out[9]=timing_valid?R32(0xc3000000u):0u;
    out[10]=timing_valid?R32(0xc3000004u):0u;
    out[11]=enabled;out[12]=reset;
    out[13]=R32(0x60006020u);out[14]=R32(0x60006034u);out[15]=perf.epoch;
}
void ln_ata_performance(uint32_t out[16]){
    out[0]=info.commands;out[1]=info.sectors_read;out[2]=perf.reads;out[3]=perf.terminal;
    out[4]=perf.wall;out[5]=perf.max_wall;out[6]=perf.sector;out[7]=perf.max_sector;
    out[8]=perf.polls;out[9]=perf.busy;out[10]=perf.no_drq;out[11]=perf.epoch;
    out[12]=info.errors;out[13]=info.timeouts;
    out[14]=(perf.active?1u:0u)|(info.initializing?2u:0u)|(info.busy?8u:0u)|
        (perf.saturated?0x80000000u:0u);out[15]=ln_time_us();
}
#endif
uint32_t ln_ata_next_deadline(uint32_t now) {
    if(phase==OFF||phase==IDLE)return now+200000u;
    uint32_t next=now+100u;if((int32_t)(not_before-now)>0)next=not_before;
    return (int32_t)(deadline-next)<0?deadline:next;
}
