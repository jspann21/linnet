#include "hal/api/ata.h"
#include "ata_pio.h"
#include "board.h"
#include "registers.h"
#if !defined(LINNET_SRAM_MONITOR)
#include "hal/arm926/cache.h"
static uint8_t dma_buffer[16384] __attribute__((aligned(32)));
static bool dma_enabled,dma_active,dma_request;
#endif
#define ATA(offset) LN_REG(0x38700000u+(offset))
extern uint32_t ln_time_us(void);
static struct ln_ata_info info;
static struct ln_ata_result result;
static uint16_t identity[256];
static uint32_t phase,due,expires,word,total,done,command_left;
static uint64_t sector;
static uint8_t *data;
static enum ln_ata_op operation;
static bool result_ready,identifying,lba_probe_required;
static uint64_t previous_sectors;
static uint32_t previous_physical_sector_bytes;
static uint32_t physical_sectors;
static char previous_model[41],previous_serial[21],previous_firmware[9];
#if defined(LINNET_SRAM_MONITOR)
static uint8_t physical_buffer[4096] __attribute__((section(".dram.flash"),aligned(32)));
#else
static uint8_t physical_buffer[4096] __attribute__((aligned(32)));
static bool physical_writing;
static enum ln_ata_op physical_operation;
static uint64_t physical_sector;
static uint32_t physical_remaining,physical_done,physical_count,physical_deadline;
#endif
static bool physical_active;
static uint8_t *physical_data;
static uint32_t physical_offset;
enum { OFF,RESET_ON,RESET_OFF,SOFT_ON,SOFT_OFF,CONFIGURE,IDENTIFY,TRANSFER,FINISH,PIO_MODE,DMA_MODE,DMA_WAIT,STANDBY_FLUSH,LBA_PROBE };
static void finish(uint32_t status) {
#if !defined(LINNET_SRAM_MONITOR)
    if(dma_active){ATA(8)=2;ATA(0x18)&=~0x100cu;ATA(0x14)=0;dma_active=false;}
#endif
    info.busy=info.initializing=0;
    if(status!=LN_ATA_OK){info.errors++;info.failed=1;if(status==LN_ATA_TIMEOUT)info.timeouts++;}
    if(identifying) {info.initialized=status==LN_ATA_OK;identifying=false;}
    else {result=(struct ln_ata_result){status,done};result_ready=true;}
    phase=OFF;
}
static bool put(unsigned offset,uint16_t value){return ln_s5l_ata_write(offset,value);}
static void finish_identify(void){
    if(!lba_probe_required){finish(LN_ATA_OK);return;}
    /* This exact CFA controller omits word 49's LBA bit. Prove an LBA taskfile
     * command succeeds before exposing its otherwise valid capacity. */
    if(!put(0x6c,0xe0)||!put(0x5c,1)||!put(0x60,0)||!put(0x64,0)||
       !put(0x68,0)||!put(0x70,0x40)){finish(LN_ATA_IO);return;}
    ++info.commands;phase=LBA_PROBE;
}
static void text(char *out,unsigned first,unsigned count) {
    unsigned capacity=count*2;
    for(unsigned i=0;i<capacity;i++){
        uint8_t c=(uint8_t)(identity[first+i/2]>>(i&1u?0:8));
        out[i]=c>=32u&&c<=126u?(char)c:' ';
    }
    unsigned length=capacity;while(length&&out[length-1]==' ')length--;
    for(unsigned i=length;i<=capacity;i++)out[i]=0;
}
static bool exact(const char *left,const char *right){
    unsigned at=0;while(right[at]&&left[at]==right[at])at++;
    return !left[at]&&!right[at];
}
static bool decode(void) {
    text(info.model,27,20);text(info.serial,10,10);text(info.firmware,23,4);
    info.quirks=0;
    bool iflash=exact(info.model,"iFlash-Platform iPod Adapters")&&
        exact(info.firmware,"iFlash85")&&(identity[83]&0xc004u)==0x4004u;
    lba_probe_required=!(identity[49]&(1u<<9));
    if(lba_probe_required&&!iflash)return false;
    if(iflash){info.compact_flash=1;info.quirks=(lba_probe_required?1u:0u)|12u;}
    info.lba48=(identity[83]&0xc400u)==0x4400u;
    info.sectors=(uint32_t)identity[60]|((uint32_t)identity[61]<<16);
    if(info.lba48)info.sectors=(uint64_t)identity[100]|((uint64_t)identity[101]<<16)|
        ((uint64_t)identity[102]<<32)|((uint64_t)identity[103]<<48);
    if(!info.sectors || info.sectors>((uint64_t)1<<48))return false;
    info.logical_sector_bytes=512;
    if((identity[106]&0xd000u)==0x5000u &&
       ((uint32_t)identity[117]|((uint32_t)identity[118]<<16))!=256)return false;
    info.physical_sector_bytes=512;
    if((identity[106]&0xe000u)==0x6000u)info.physical_sector_bytes=512u<<(identity[106]&15);
    /* IDENTIFY words 106/209 describe size and alignment independently of
     * model names. Use bounded whole physical sectors even when a drive can
     * emulate smaller I/O itself; some iPod drives require this in the host. */
    if(info.physical_sector_bytes>sizeof physical_buffer||
       ((identity[209]&0xc000u)==0x4000u&&(identity[209]&0x3fffu)))return false;
    physical_sectors=info.physical_sector_bytes>>9;
    if(info.sectors&(physical_sectors-1u))return false;
    if(physical_sectors>1u)info.quirks|=16u;
    info.can_flush=(identity[83]&0xd000u)==0x5000u;
    info.standby_supported=(identity[82]&8)!=0;
    info.apm_supported=(identity[83]&0xc008u)==0x4008u;
    info.write_cache_enabled=(identity[85]&(1u<<5))!=0;
    info.rotation_rpm=identity[217]>=0x401u && identity[217]!=0xffff?identity[217]:0;
    info.media_kind=iflash||identity[217]==1?LN_ATA_MEDIA_SOLID_STATE:
        info.rotation_rpm?LN_ATA_MEDIA_ROTATING:LN_ATA_MEDIA_UNKNOWN;
    if(previous_sectors){
        if(info.sectors!=previous_sectors||info.physical_sector_bytes!=previous_physical_sector_bytes)return false;
        for(unsigned i=0;i<sizeof previous_model;i++)if(info.model[i]!=previous_model[i])return false;
        for(unsigned i=0;i<sizeof previous_firmware;i++)if(info.firmware[i]!=previous_firmware[i])return false;
        /* This exact adapter reports a different synthetic serial after a
         * rail cycle. Its matched model, firmware, and capacity identify it. */
        if(!iflash)for(unsigned i=0;i<sizeof previous_serial;i++)if(info.serial[i]!=previous_serial[i])return false;
    }
    /* The SRAM monitor does not remove the disk supply. Keep its media kind
     * conservative and leave the model fallback to persistent Boot and OS. */
#if !defined(LINNET_SRAM_MONITOR)
    /* These mechanical drives omit IDENTIFY word 217's rotation rate.
     * MK1231GAL: Toshiba's 2008-09-10 product specifications, 4,200 rpm:
     * https://www.global.toshiba/ww/news/corporate/2008/09/pr1001.html
     * MK1634GAL: Toshiba's 2009-09-10 product specifications, 4,200 rpm:
     * https://www.global.toshiba/ww/news/corporate/2009/09/pr1001.html */
    if(info.media_kind==LN_ATA_MEDIA_UNKNOWN &&
       (exact(info.model,"TOSHIBA MK3008GAL")||exact(info.model,"TOSHIBA MK1231GAL")||
        exact(info.model,"TOSHIBA MK1634GAL"))){
        info.media_kind=LN_ATA_MEDIA_ROTATING;info.rotation_rpm=4200;
    }
#endif
    info.can_power_off=info.media_kind!=LN_ATA_MEDIA_UNKNOWN&&info.can_flush&&
        (info.standby_supported||iflash);
    info.capabilities=info.lba48|(info.can_flush<<1)|(info.standby_supported<<2)|
        (info.apm_supported<<3)|(info.write_cache_enabled<<4);
    info.idle_timeout_us=30000000;info.wake_timeout_us=15000000;
    return true;
}
static void reset(uint32_t now) {
    physical_active=false;
#if !defined(LINNET_SRAM_MONITOR)
    if(dma_active){ATA(8)=2;ATA(0x18)&=~0x100cu;ATA(0x14)=0;}
    dma_enabled=dma_active=dma_request=false;
#endif
    info.initialized=info.initializing=info.busy=info.failed=info.sleeping=info.power_safe=0;
    result_ready=false;identifying=true;lba_probe_required=false;word=total=done=0;phase=OFF;
    if(!ln_s5l_board_disk_power()){finish(LN_ATA_OFFLINE);return;}
    info.powered=info.initializing=1;
    ATA(0x18)=1;due=now+10000;expires=now+15000000;phase=RESET_ON;
}
void ln_ata_init(uint32_t now){info=(struct ln_ata_info){0};previous_sectors=0;previous_physical_sector_bytes=0;info.spinups=1;reset(now);}
void ln_ata_recover(uint32_t now){
    previous_sectors=info.sectors;
    previous_physical_sector_bytes=info.physical_sector_bytes;
    for(unsigned i=0;i<sizeof previous_model;i++)previous_model[i]=info.model[i];
    for(unsigned i=0;i<sizeof previous_serial;i++)previous_serial[i]=info.serial[i];
    for(unsigned i=0;i<sizeof previous_firmware;i++)previous_firmware[i]=info.firmware[i];
    reset(now);
}
bool ln_ata_power_off(void){
    if(!info.initialized||info.busy||physical_active||info.initializing||result_ready||phase!=OFF||!info.power_safe||!info.can_power_off)return false;
    if(!ln_s5l_board_disk_power_off()){ln_ata_recover(ln_time_us());return false;}
    info.powered=info.initialized=0;++info.poweroffs;return true;
}
bool ln_ata_power_on(uint32_t now){
    if(info.powered||info.busy||info.initializing||result_ready||!info.sectors)return false;
    ++info.spinups;ln_ata_recover(now);return info.initializing;
}
static bool issue(void) {
    bool extended=sector>=0x10000000u;
    if(extended && !info.lba48)return false;
    command_left=total-done;
    if(command_left>256u)command_left=256u;
#if !defined(LINNET_SRAM_MONITOR)
    dma_request=dma_enabled;
    if(dma_request&&command_left>sizeof dma_buffer/512u)command_left=sizeof dma_buffer/512u;
#endif
    if(!extended && command_left>0x10000000u-sector)command_left=(uint32_t)(0x10000000u-sector);
    if(!put(0x6c,extended?0x40:(uint16_t)(0xe0|((sector>>24)&15))))return false;
    if(extended && (!put(0x5c,(uint16_t)(command_left>>8)) || !put(0x60,(uint16_t)((sector>>24)&255)) ||
       !put(0x64,(uint16_t)((sector>>32)&255)) || !put(0x68,(uint16_t)((sector>>40)&255))))return false;
    if(!put(0x5c,(uint16_t)(command_left&255u)) || !put(0x60,(uint16_t)(sector&255)) || !put(0x64,(uint16_t)((sector>>8)&255)) ||
       !put(0x68,(uint16_t)((sector>>16)&255)))return false;
    uint16_t command=operation==LN_ATA_WRITE?(extended?0x34:0x30):(extended?0x24:0x20);
#if !defined(LINNET_SRAM_MONITOR)
    if(dma_request)command=operation==LN_ATA_WRITE?(extended?0x35:0xca):(extended?0x25:0xc8);
#endif
    if(!put(0x70,command))return false;
    info.commands++;word=0;phase=TRANSFER;return true;
}
static bool start_io(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *buffer,uint32_t deadline) {
    uint32_t now=ln_time_us();
    if(!info.initialized || info.failed || info.busy || result_ready || (int32_t)(deadline-now)<=0 ||
       deadline-now>30000000u || op<LN_ATA_READ || op>LN_ATA_WAKE)return false;
    bool transfer=op==LN_ATA_READ || op==LN_ATA_WRITE;
    if(transfer && (!buffer || !sectors || sectors>(op==LN_ATA_WRITE?LINNET_ATA_MAX_WRITE_SECTORS:LINNET_ATA_MAX_READ_SECTORS) ||
       lba>=info.sectors || sectors>info.sectors-lba || (op==LN_ATA_WRITE && !info.can_flush)))return false;
    if(!transfer && (buffer || sectors || lba))return false;
    if((op==LN_ATA_FLUSH && !info.can_flush) ||
       (op==LN_ATA_STANDBY && !info.standby_supported && !info.can_flush))return false;
    operation=op;sector=lba;total=sectors;data=buffer;done=word=0;expires=deadline;
    info.busy=1;info.power_safe=0;
    if(transfer){if(!issue())finish(LN_ATA_IO);}
    else {
        /* Some flash adapters can flush but cannot enter ATA Standby. A flush
         * still provides the storage barrier required for a mode handoff. */
        bool flush=op==LN_ATA_STANDBY&&info.can_flush&&
            (info.write_cache_enabled||!info.standby_supported);
        uint16_t command=op==LN_ATA_FLUSH||flush?0xe7:op==LN_ATA_STANDBY?0xe0:0xe1;
        if(!put(0x6c,0xe0) || !put(0x70,command))finish(LN_ATA_IO);
        else {info.commands++;phase=flush?STANDBY_FLUSH:FINISH;}
    }
    return true;
}
static void step_io(uint32_t now) {
    if(phase==OFF)return;
    if((int32_t)(now-expires)>=0){finish(LN_ATA_TIMEOUT);return;}
    if(phase<IDENTIFY && (int32_t)(now-due)<0)return;
#if !defined(LINNET_SRAM_MONITOR)
    if(phase==DMA_WAIT){
        if(!(ATA(0x10)&1u))return;
        ATA(8)=2;ATA(0x18)&=~0x100cu;ATA(0x14)=0;ATA(0x10)=31;dma_active=false;
        uint32_t bytes=command_left*512u;
        if(operation==LN_ATA_READ){
            if(!ln_arm926_data_invalidate_range(dma_buffer,bytes)){finish(LN_ATA_IO);return;}
            extern void ln_memory_copy(void *,const void *,uint32_t);
            ln_memory_copy(data+done*512u,dma_buffer,bytes);
            info.sectors_read+=command_left-1u;
        }else info.sectors_written+=command_left-1u;
        done+=command_left-1u;sector+=command_left-1u;
        phase=FINISH;return;
    }
#endif
    uint16_t status=0;
    switch(phase) {
    case RESET_ON:ATA(0x18)=0;due=now+10000;phase=RESET_OFF;return;
    case RESET_OFF:ATA(12)=1;due=now+10000;phase=SOFT_ON;return;
    case SOFT_ON:ATA(12)=0;due=now+100000;phase=SOFT_OFF;return;
    case SOFT_OFF:ATA(0)=1;due=now+200000;phase=CONFIGURE;return;
    case CONFIGURE:ATA(0x2c)=0x191f7;ATA(0x68)=0;ATA(0x18)=0x40;phase=IDENTIFY;return;
    default:break;
    }
    if(!ln_s5l_ata_read(0x74,&status)){finish(LN_ATA_IO);return;}
    info.last_status=status;
    if(status==0xff || (status&0x80))return;
    if(phase==IDENTIFY) {
        if(!put(0x6c,0) || !put(0x70,0xec)){finish(LN_ATA_IO);return;}
        info.commands++;phase=TRANSFER;return;
    }
    if(phase==PIO_MODE){
        /* These PIO4 host timings meet the drive's minimum at HClk=108
         * MHz and remain conservative at the normal 54 MHz bus clock. */
        if(!(status&0x29u)&&(identity[53]&2u)&&(identity[64]&2u))ATA(0x2c)=0x2072u;
#if !defined(LINNET_SRAM_MONITOR)
        /* The exact iFlash85 adapter omits word 49's DMA flag while its
         * valid word 88 advertises UDMA modes. Keep the mode advertisement
         * and SET FEATURES acknowledgement as the transfer gate. */
        if(!(status&0x29u)&&((identity[49]&0x100u)||(info.quirks&8u))&&
           (identity[53]&4u)&&(identity[88]&1u)){
            bool fast=(info.quirks&8u)&&(identity[88]&4u);
            ATA(0x30)=fast?0x02030a52u:0x04071152u;
            if(!put(0x58,3)||!put(0x5c,fast?0x42:0x40)||!put(0x70,0xef)){finish(LN_ATA_IO);return;}
            ++info.commands;phase=DMA_MODE;return;
        }
#endif
        finish_identify();return;
    }
#if !defined(LINNET_SRAM_MONITOR)
    if(phase==DMA_MODE){dma_enabled=!(status&0x29u);finish_identify();return;}
#endif
    if(phase==LBA_PROBE){
        if((status&0x29u)||!(status&0x40u))finish(LN_ATA_IO);
        else finish(LN_ATA_OK);
        return;
    }
    if(status&0x21) {
        uint16_t error=0;(void)ln_s5l_ata_read(0x58,&error);info.last_error=error;finish(LN_ATA_IO);return;
    }
    if(phase==TRANSFER) {
        if(!(status&8))return;
#if !defined(LINNET_SRAM_MONITOR)
        if(!identifying&&dma_request){
            uint32_t bytes=command_left*512u;
            if(operation==LN_ATA_WRITE){
                extern void ln_memory_copy(void *,const void *,uint32_t);
                ln_memory_copy(dma_buffer,data+done*512u,bytes);
            }
            ln_arm926_data_clean_range(dma_buffer,bytes);
            uint32_t address=operation==LN_ATA_WRITE?0x44u:0x3cu;
            ATA(address)=(uint32_t)(uintptr_t)dma_buffer;ATA(address+4u)=bytes;ATA(0x34)=bytes-1u;
            ATA(0x18)=(ATA(0x18)&~0x190u)|0x60cu|(operation==LN_ATA_WRITE?0x10u:0u);
            ATA(0x10)=31;ATA(0x14)=1;dma_active=true;phase=DMA_WAIT;ATA(8)=1;return;
        }
#endif
        uint32_t slice=ln_time_us();
        for(unsigned count=0;count<256 && word<256;count++,word++) {
            if(count && ln_time_us()-slice>=500u)break;
            uint16_t value;
            if(!identifying && operation==LN_ATA_WRITE) {
                value=(uint16_t)(data[done*512+word*2]|((uint16_t)data[done*512+word*2+1]<<8));
                if(!put(0x54,value)){finish(LN_ATA_IO);return;}
            } else {
                if(!ln_s5l_ata_read(0x54,&value)){finish(LN_ATA_IO);return;}
                if(identifying)identity[word]=value;
                else {data[done*512+word*2]=(uint8_t)value;data[done*512+word*2+1]=(uint8_t)(value>>8);}
            }
        }
        if(word==256){
            if(!identifying && command_left>1u){
                /* READ/WRITE SECTORS asserts DRQ separately for each 512-byte
                 * sector. Yield before polling the next sector's status. */
                --command_left;++done;++sector;word=0;
                if(operation==LN_ATA_READ)++info.sectors_read;else ++info.sectors_written;
            }else phase=FINISH;
        }
        return;
    }
    if(status&8)return;
    if(phase==STANDBY_FLUSH){
        if(!info.standby_supported){info.sleeping=0;info.power_safe=1;finish(LN_ATA_OK);return;}
        if(!put(0x70,0xe0)){finish(LN_ATA_IO);return;}
        ++info.commands;phase=FINISH;return;
    }
    if(identifying){
        if(!decode()){finish(LN_ATA_UNSUPPORTED);return;}
#if !defined(LINNET_SRAM_MONITOR)
        if((identity[53]&2u)&&(identity[64]&2u)){
            if(!put(0x6c,0xe0)||!put(0x58,3)||!put(0x5c,0x0c)||!put(0x70,0xef)){finish(LN_ATA_IO);return;}
            ++info.commands;phase=PIO_MODE;return;
        }
        /* iFlash85 leaves the advanced PIO bitmap blank. Probe its reported
         * UDMA mode without requiring an unrelated PIO4 negotiation. */
        if(info.quirks&8u){phase=PIO_MODE;return;}
#endif
        finish_identify();return;
    }
    if(operation==LN_ATA_READ || operation==LN_ATA_WRITE) {
        done++;sector++;info.sleeping=0;
        if(operation==LN_ATA_READ)info.sectors_read++;else info.sectors_written++;
        if(done<total){if(!issue())finish(LN_ATA_IO);return;}
    } else if(operation==LN_ATA_STANDBY){info.sleeping=info.standby_supported;info.power_safe=1;}
    else if(operation==LN_ATA_WAKE)info.sleeping=0;
    finish(LN_ATA_OK);
}
#if !defined(LINNET_SRAM_MONITOR)
static bool physical_read(void){
    uint32_t mask=physical_sectors-1u;
    physical_offset=(uint32_t)(physical_sector&mask)*512u;
    physical_count=physical_sectors-(uint32_t)(physical_sector&mask);
    if(physical_count>physical_remaining)physical_count=physical_remaining;
    return start_io(LN_ATA_READ,physical_sector&~(uint64_t)mask,physical_sectors,physical_buffer,physical_deadline);
}
bool ln_ata_start(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *buffer,uint32_t deadline){
    if(physical_active)return false;
    if((op!=LN_ATA_READ&&op!=LN_ATA_WRITE)||!(info.quirks&16u))
        return start_io(op,lba,sectors,buffer,deadline);
    if(!(lba&(physical_sectors-1u))&&!(sectors&(physical_sectors-1u)))return start_io(op,lba,sectors,buffer,deadline);
    physical_writing=false;
    if(!buffer||!sectors||sectors>(op==LN_ATA_WRITE?LINNET_ATA_MAX_WRITE_SECTORS:LINNET_ATA_MAX_READ_SECTORS)||
       lba>=info.sectors||sectors>info.sectors-lba||(op==LN_ATA_WRITE&&!info.can_flush))return false;
    physical_operation=op;physical_sector=lba;physical_data=buffer;
    physical_remaining=sectors;physical_done=0;physical_deadline=deadline;
    physical_active=physical_read();return physical_active;
}
void ln_ata_step(uint32_t now){
    step_io(now);
    if(!physical_active||!result_ready)return;
    uint32_t status=result.status;
    if(!status&&result.sectors_done!=physical_sectors){finish(LN_ATA_IO);status=LN_ATA_IO;}
    result_ready=false;
    if(status)goto complete;
    if(physical_operation==LN_ATA_WRITE&&!physical_writing){
        extern void ln_memory_copy(void *,const void *,uint32_t);
        ln_memory_copy(physical_buffer+physical_offset,physical_data+physical_done*512u,physical_count*512u);
        physical_writing=true;
        if(start_io(LN_ATA_WRITE,physical_sector&~(uint64_t)(physical_sectors-1u),physical_sectors,physical_buffer,physical_deadline))return;
        goto failed;
    }
    if(physical_operation==LN_ATA_READ){
        extern void ln_memory_copy(void *,const void *,uint32_t);
        ln_memory_copy(physical_data+physical_done*512u,physical_buffer+physical_offset,physical_count*512u);
    }
    physical_done+=physical_count;physical_remaining-=physical_count;physical_sector+=physical_count;
    if(physical_remaining){
        physical_writing=false;
        if(physical_read())return;
        goto failed;
    }
    goto complete;
failed:
    status=(int32_t)(ln_time_us()-physical_deadline)>=0?LN_ATA_TIMEOUT:LN_ATA_IO;finish(status);
complete:
    physical_active=false;result=(struct ln_ata_result){status,physical_done};result_ready=true;
}
#else
/* SRAM exposes only bounded single-sector read diagnostics; its aligned read
 * adapter needs no disk-writing state or filesystem transaction machinery. */
bool ln_ata_start(enum ln_ata_op op,uint64_t lba,uint32_t sectors,uint8_t *buffer,uint32_t deadline){
    if(physical_active)return false;
    if((info.quirks&16u)&&op==LN_ATA_WRITE)return false;
    if((info.quirks&16u)&&op==LN_ATA_READ){
        if(sectors!=1||!buffer||lba>=info.sectors)return false;
        physical_offset=(uint32_t)(lba&(physical_sectors-1u))*512u;physical_data=buffer;
        physical_active=start_io(op,lba&~(uint64_t)(physical_sectors-1u),physical_sectors,physical_buffer,deadline);return physical_active;
    }
    return start_io(op,lba,sectors,buffer,deadline);
}
void ln_ata_step(uint32_t now){
    step_io(now);
    if(!physical_active||!result_ready)return;
    physical_active=false;
    if(!result.status&&result.sectors_done==physical_sectors){
        extern void ln_memory_copy(void *,const void *,uint32_t);
        ln_memory_copy(physical_data,physical_buffer+physical_offset,512);result.sectors_done=1;
    }else{
        if(!result.status)finish(LN_ATA_IO);
        result.sectors_done=0;
    }
}
bool ln_s5l_ata_identify_copy(uint16_t out[256]){
    if(!out||!info.initialized||info.failed)return false;
    for(unsigned i=0;i<256;i++)out[i]=identity[i];return true;
}
#endif
bool ln_ata_take(struct ln_ata_result *out){if(!out || physical_active || !result_ready)return false;*out=result;result_ready=false;return true;}
void ln_ata_snapshot(struct ln_ata_info *out){if(out)*out=info;}
void ln_ata_state_snapshot(struct ln_ata_state *out) {
    if(out)*out=(struct ln_ata_state){info.sectors,info.initialized,info.initializing,info.busy,info.sleeping,info.failed,
        info.can_flush,info.powered,info.standby_supported,info.can_power_off,info.power_safe,info.idle_timeout_us,info.media_kind};
}
uint32_t ln_ata_next_deadline(uint32_t now){return phase==OFF?now+200000u:phase<IDENTIFY && (int32_t)(due-now)>0?due:now+100u;}
void ln_ata_diagnostics(uint32_t out[24]){
    if(!out)return;
    for(unsigned i=0;i<24;i++)out[i]=0;
    out[0]=phase;out[1]=word;out[2]=info.last_status;
    out[3]=identity[49];out[4]=identity[53];out[5]=identity[88];out[7]=identity[64];
#if !defined(LINNET_SRAM_MONITOR)
    out[6]=dma_enabled;
#endif
}
/* SoC-specific timing formats are deliberately distinct from PP502x. */
void ln_ata_capabilities(uint32_t out[16]){if(out){for(unsigned i=0;i<16;i++)out[i]=0;out[0]=0x50414341;out[1]=1;out[3]=info.capabilities;}}
void ln_ata_clock_diagnostics(uint32_t out[8]){if(out){for(unsigned i=0;i<8;i++)out[i]=0;out[0]=0x464e4941;out[1]=1;out[2]=info.powered;out[4]=info.powered?ATA(0x2c):0;}}
void ln_ata_performance(uint32_t out[16]){if(out){for(unsigned i=0;i<16;i++)out[i]=0;out[0]=info.commands;out[1]=info.sectors_read;out[12]=info.errors;out[13]=info.timeouts;out[14]=0x80000000u;}}
