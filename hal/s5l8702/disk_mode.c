#include "disk_mode.h"
#include "usb.h"
#include "hal/api/usb.h"
#include "hal/api/ata.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_bytes.h"
#include "boot_state.h"
#include "board.h"
/* USB MSC Bulk-Only Transport, one transparent SCSI direct-access LUN.
 * The filesystem owner must have quiesced before entering this exclusive loop. */
enum { CBW, SEND, READ_WAIT, RECEIVE, WRITE_WAIT, FLUSH_WAIT, DISCARD, STATUS, STATUS_WAIT, INVALID };
enum { WRITE_BUFFER_BYTES=LINNET_ATA_MAX_WRITE_SECTORS*512u, READ_BUFFER_BYTES=8192u,
       USB_SEND_BYTES=16384u };
static uint8_t buffer[WRITE_BUFFER_BYTES>READ_BUFFER_BYTES?WRITE_BUFFER_BYTES:READ_BUFFER_BYTES] __attribute__((aligned(32)));
static uint8_t wrapper[31],status_bytes[13];
static uint32_t state,tag,residue,length,position,lba,blocks,block_bytes,block_shift,scale,capacity;
static uint32_t batch_blocks,write_batch_bytes;
static uint8_t status,sense_key,sense_code;
static bool direction_in,reading,writing,eject,write_enabled,dirty;
static const char *device_serial;
static uint32_t activity;
static volatile uint32_t *const diagnostic=(volatile uint32_t *)(uintptr_t)0x22017c00u;
static void put32(uint8_t *p,uint32_t value){p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}
static void failure(uint8_t key,uint8_t code) {
    sense_key=key;sense_code=code;status=1;reading=writing=false;position=length=0;
    diagnostic[3]++;diagnostic[4]=(uint32_t)key<<8|code;
    for(unsigned i=0;i<4;i++)diagnostic[60+i]=ln_read_le32(wrapper+15+i*4);
    state=residue?(direction_in?SEND:DISCARD):STATUS;
    if(state==DISCARD)length=residue;
}
static bool disk_request(enum ln_ata_op operation) {
    bool transfer=operation==LN_ATA_READ || operation==LN_ATA_WRITE;
    if(transfer){
        batch_blocks=(operation==LN_ATA_READ?READ_BUFFER_BYTES:write_batch_bytes)>>block_shift;
        if(batch_blocks>blocks)batch_blocks=blocks;
    }
    return ln_ata_start(operation,operation==LN_ATA_READ || operation==LN_ATA_WRITE?(uint64_t)lba*scale:0,
        transfer?batch_blocks*scale:0,
        operation==LN_ATA_READ || operation==LN_ATA_WRITE?buffer:0,ln_time_us()+15000000u);
}
static void reply(uint32_t bytes,uint32_t allocation) {
    if(!direction_in && residue){failure(5,0x24);return;}
    if(bytes>allocation)bytes=allocation;if(bytes>residue)bytes=residue;
    length=bytes;position=0;state=bytes||residue?SEND:STATUS;
}
static void mode_sense(const uint8_t *c,bool ten) {
    uint8_t page=c[2]&0x3f,pc=c[2]>>6;
    if((page!=0 && page!=8 && page!=0x3f) || c[3] || pc==3){failure(5,0x24);return;}
    uint32_t header=ten?8:4,descriptor=(c[1]&8)?0:8;
    bool long_lba=ten && (c[1]&0x10) && descriptor;
    if(long_lba)descriptor=16;
    buffer[ten?3:2]=write_enabled?0:0x80;
    buffer[ten?7:3]=(uint8_t)descriptor;
    if(descriptor) {
        if(long_lba){buffer[4]=1;put32(buffer+header+4,capacity);put32(buffer+header+12,block_bytes);}
        else {
            uint32_t count=capacity>0xffffff?0xffffff:capacity;
            buffer[header+1]=(uint8_t)(count>>16);buffer[header+2]=(uint8_t)(count>>8);buffer[header+3]=(uint8_t)count;
            buffer[header+6]=(uint8_t)(block_bytes>>8);buffer[header+7]=(uint8_t)block_bytes;
        }
    }
    uint32_t bytes=header+descriptor;
    if(page==8 || page==0x3f){buffer[bytes]=8;buffer[bytes+1]=18;bytes+=20;}
    /* Every completed WRITE is flushed: advertise no volatile write cache. */
    buffer[ten?1:0]=(uint8_t)(bytes-(ten?2:1));
    reply(bytes,ten?ln_read_be16(c+7):c[4]);
}
static void command(void) {
    if(ln_read_le32(wrapper)!=0x43425355u || wrapper[13] || !wrapper[14] || wrapper[14]>16 || (wrapper[12]&0x7f)) {
        ln_s5l_usb_stall(true);ln_s5l_usb_stall(false);state=INVALID;return;
    }
    tag=ln_read_le32(wrapper+4);residue=ln_read_le32(wrapper+8);direction_in=(wrapper[12]&0x80)!=0;
    status=0;reading=writing=eject=false;position=length=0;state=STATUS;
    const uint8_t *c=wrapper+15;
    uint32_t slot=12+((c[0]?diagnostic[1]++:diagnostic[1])%8)*6;
    if(c[0]) {
    diagnostic[slot]=ln_time_us();diagnostic[slot+1]=c[0];diagnostic[slot+2]=residue;
    diagnostic[slot+3]=ln_read_be32(c+2);diagnostic[slot+4]=ln_read_le32(c);diagnostic[slot+5]=ln_read_le32(c+4);
    }
    if(c[0]!=0)activity=ln_time_us();
    unsigned group=c[0]>>5,required=group==0?6:group==4?16:group==5?12:10;
    if(wrapper[14]<required){failure(5,0x24);return;}
    for(unsigned i=0;i<64;i++)buffer[i]=0;
    switch(c[0]) {
    case 0: /* TEST UNIT READY */
    case 0x1e: /* PREVENT/ALLOW MEDIUM REMOVAL */
    case 0x2f: /* VERIFY(10), no BYTCHK payload */
        if(residue || (c[0]==0x2f && (c[1]&2)))failure(5,0x24);
        break;
    case 3: /* REQUEST SENSE */
        buffer[0]=0x70;buffer[2]=sense_key;buffer[7]=10;buffer[12]=sense_code;
        reply(18,c[4]);sense_key=sense_code=0;break;
    case 0x12: /* INQUIRY */
        if(c[1]&1) {
            buffer[1]=c[2];
            if(c[2]==0){buffer[3]=3;buffer[4]=0;buffer[5]=0x80;buffer[6]=0x83;reply(7,c[4]);}
            else if(c[2]==0x80){buffer[3]=16;for(unsigned i=0;i<16;i++)buffer[4+i]=(uint8_t)device_serial[i];reply(20,c[4]);}
            else if(c[2]==0x83){buffer[3]=20;buffer[4]=2;buffer[7]=16;for(unsigned i=0;i<16;i++)buffer[8+i]=(uint8_t)device_serial[i];reply(24,c[4]);}
            else failure(5,0x24);
            break;
        }
        buffer[1]=0x80;buffer[2]=4;buffer[3]=2;buffer[4]=31;
        {static const char text[]="LINNET  IPOD CLASSIC    0001";for(unsigned i=0;i<28;i++)buffer[8+i]=(uint8_t)text[i];}
        reply(36,c[4]);break;
    case 0x1a: /* MODE SENSE(6), no block descriptor */
        mode_sense(c,false);break;
    case 0x5a: /* MODE SENSE(10) */
        mode_sense(c,true);break;
    case 0x9e: /* READ CAPACITY(16) */
        if((c[1]&31)!=0x10){failure(5,0x24);break;}
        put32(buffer+4,capacity-1);put32(buffer+8,block_bytes);reply(32,ln_read_be32(c+10));break;
    case 0xa0: /* REPORT LUNS */
        buffer[3]=8;reply(16,ln_read_be32(c+6));break;
    case 0x23: /* READ FORMAT CAPACITIES */
        buffer[3]=8;put32(buffer+4,capacity);buffer[8]=2;
        buffer[10]=(uint8_t)(block_bytes>>8);buffer[11]=(uint8_t)block_bytes;
        reply(12,ln_read_be16(c+7));break;
    case 0x25: /* READ CAPACITY(10) */
        put32(buffer,capacity-1);put32(buffer+4,block_bytes);reply(8,8);break;
    case 0x28:case 0x2a: /* READ/WRITE(10) */
        lba=ln_read_be32(c+2);blocks=ln_read_be16(c+7);
        if(lba>capacity || blocks>capacity-lba){failure(5,0x21);break;}
        if(residue!=blocks*block_bytes || direction_in!=(c[0]==0x28)){status=2;failure(5,0x24);status=2;break;}
        if(!blocks)break;
        if(c[0]==0x2a && !write_enabled){failure(7,0x27);break;}
        reading=c[0]==0x28;writing=!reading;
        if(reading){if(!disk_request(LN_ATA_READ))failure(3,0x11);else state=READ_WAIT;}
        else {state=RECEIVE;batch_blocks=write_batch_bytes>>block_shift;if(batch_blocks>blocks)batch_blocks=blocks;length=batch_blocks*block_bytes;}
        break;
    case 0x35: /* SYNCHRONIZE CACHE */
    case 0x1b: /* START STOP UNIT */
        if(residue){failure(5,0x24);break;}
        if(c[0]==0x1b && (c[4]&1))break;
        eject=c[0]==0x1b && (c[4]&3)==2;
        if(!disk_request(LN_ATA_FLUSH))failure(4,0x44);else state=FLUSH_WAIT;
        break;
    default:failure(5,0x20);break;
    }
}
static void step(void) {
    struct ln_ata_result result;
    if(state==READ_WAIT || state==WRITE_WAIT || state==FLUSH_WAIT) {
        if(!ln_ata_take(&result))return;
        diagnostic[5]=result.status;diagnostic[6]=result.sectors_done;
        if(result.status || ((state==READ_WAIT || state==WRITE_WAIT) && result.sectors_done!=batch_blocks*scale)) {
            failure(3,state==READ_WAIT?0x11:0x0c);return;
        }
        if(state==READ_WAIT){length=batch_blocks*block_bytes;position=0;state=SEND;return;}
        if(state==FLUSH_WAIT){dirty=false;state=STATUS;return;}
        lba+=batch_blocks;blocks-=batch_blocks;position=0;
        if(blocks){batch_blocks=write_batch_bytes>>block_shift;if(batch_blocks>blocks)batch_blocks=blocks;length=batch_blocks*block_bytes;state=RECEIVE;}
        else if(disk_request(LN_ATA_FLUSH))state=FLUSH_WAIT;else failure(4,0x44);
        return;
    }
    if(state==CBW) {
        position+=(uint32_t)ln_s5l_usb_storage_read(wrapper+position,sizeof(wrapper)-position);
        if(position==sizeof(wrapper))command();
    } else if(state==SEND) {
        uint32_t bytes=length-position;
        if(bytes>USB_SEND_BYTES)bytes=USB_SEND_BYTES;
        if(!ln_s5l_usb_storage_write(buffer+position,bytes))return;
        position+=bytes;residue-=bytes;
        if(position==length) {
            if(reading){blocks-=batch_blocks;lba+=batch_blocks;}
            if(reading && blocks) {
                if(disk_request(LN_ATA_READ))state=READ_WAIT;else failure(3,0x11);
            } else state=STATUS;
        }
    } else if(state==RECEIVE) {
        uint32_t bytes=(uint32_t)ln_s5l_usb_storage_read(buffer+position,length-position);position+=bytes;residue-=bytes;
        if(bytes)activity=ln_time_us();
        if(position==length){
            if(disk_request(LN_ATA_WRITE)){dirty=true;state=WRITE_WAIT;}else failure(3,0x0c);
        }
    } else if(state==DISCARD) {
        uint32_t bytes=length>sizeof(buffer)?sizeof(buffer):length;
        length-=(uint32_t)ln_s5l_usb_storage_read(buffer,bytes);if(!length)state=STATUS;
    } else if(state==STATUS) {
        if(dirty){if(disk_request(LN_ATA_FLUSH))state=FLUSH_WAIT;else ln_platform_hardware_reset();return;}
        ln_write_le32(status_bytes,0x53425355u);ln_write_le32(status_bytes+4,tag);ln_write_le32(status_bytes+8,residue);status_bytes[12]=status;
        if(ln_s5l_usb_storage_write(status_bytes,sizeof(status_bytes)))state=STATUS_WAIT;
    } else if(state==STATUS_WAIT) {
        if(!ln_usb_flush(ln_time_us()+2000000u))ln_platform_hardware_reset();
        if(eject && !status){ln_s5l_boot_request(LN_S5L_BOOT_NORMAL);ln_platform_hardware_reset();}
        state=CBW;position=0;
    }
}
/* Finish the current bounded PIO operation and flush any submitted writes.
 * A reset never reports success while ATA still owns the shared buffer. */
static bool drain(void) {
    struct ln_ata_state drive;struct ln_ata_result result;
    bool flush_started=false;
    for(;;) {
        ln_ata_step(ln_time_us());ln_ata_state_snapshot(&drive);
        if(ln_ata_take(&result)) {
            if(result.status)return false;
            if(flush_started || state==FLUSH_WAIT)dirty=false;
        }
        if(!drive.busy) {
            if(!dirty)return true;
            if(!disk_request(LN_ATA_FLUSH))return false;
            flush_started=true;
        }
        ln_core_watchdog_feed();
    }
}
_Noreturn void ln_s5l_disk_mode(const char *serial,uint32_t sector_bytes,bool writable) {
    if(sector_bytes!=512 && sector_bytes!=2048 && sector_bytes!=4096)ln_platform_hardware_reset();
    (void)ln_s5l_board_disk_screen(true);
    /* Exclusive Disk Mode needs the faster bus; exit resets the normal clock. */
    (void)ln_core_clock_set(216000000u);
    block_bytes=sector_bytes;block_shift=sector_bytes==4096u?12u:sector_bytes==2048u?11u:9u;
    scale=sector_bytes>>9;write_enabled=writable;state=CBW;position=0;
    device_serial=serial;activity=ln_time_us();dirty=false;
    for(unsigned i=0;i<64;i++)diagnostic[i]=0;
    diagnostic[0]=0x4443534du;diagnostic[2]=sector_bytes;
    ln_core_watchdog_arm();ln_ata_init(ln_time_us());
    struct ln_ata_state drive;
    do {ln_ata_step(ln_time_us());ln_ata_state_snapshot(&drive);ln_core_watchdog_feed();}while(drive.initializing);
    uint64_t logical_sectors=drive.sectors>>(sector_bytes==4096?3:sector_bytes==2048?2:0);
    if(!drive.initialized || drive.failed || !drive.can_flush || logical_sectors>UINT32_MAX)ln_platform_hardware_reset();
    capacity=(uint32_t)logical_sectors;
    struct ln_ata_info media;ln_ata_snapshot(&media);
    write_batch_bytes=media.media_kind==LN_ATA_MEDIA_SOLID_STATE &&
        media.physical_sector_bytes==512u && (media.quirks&8u)?
        WRITE_BUFFER_BYTES:LINNET_ATA_MAX_SECTORS*512u;
    if(!ln_s5l_usb_storage_init(serial))ln_platform_hardware_reset();
    uint32_t connection=ln_usb_generation();
    uint32_t blink_started=ln_time_us();
    uint32_t idle_started=blink_started;
    bool symbol_visible=true;
    for(;;) {
        diagnostic[7]=state;diagnostic[8]=position;diagnostic[9]=lba;diagnostic[10]=residue;
        /* Read-only probes can expire safely. Writable host filesystems retain
         * ownership until eject, transport failure, or physical disconnect. */
        if(!writable && ln_time_us()-activity>120000000u){(void)drain();ln_platform_hardware_reset();}
        ln_ata_step(ln_time_us());
        if(!ln_usb_poll()){(void)drain();ln_platform_hardware_reset();}
        if(ln_s5l_usb_storage_reset_pending() || connection!=ln_usb_generation()) {
            if(!drain())ln_platform_hardware_reset();
            if(ln_s5l_usb_storage_reset_pending() && !ln_s5l_usb_storage_reset_complete())ln_platform_hardware_reset();
            connection=ln_usb_generation();state=CBW;position=0;
            idle_started=ln_time_us();
        }
        if((state==RECEIVE || state==DISCARD || state==INVALID) && ln_time_us()-activity>15000000u) {
            (void)drain();ln_platform_hardware_reset();
        }
        bool was_idle=state==CBW && position==0;
        if(ln_usb_connected())step();
        /* Wait until the full command and status exchange have been quiet.
         * The text remains visible in both blink phases. */
        uint32_t now=ln_time_us();
        bool idle=state==CBW && position==0;
        if(!idle || !was_idle)idle_started=now;
        if(idle && now-idle_started>=250000u && now-blink_started>=1200000u){
            ln_core_watchdog_feed();
            if(ln_s5l_board_disk_screen(!symbol_visible))symbol_visible=!symbol_visible;
            blink_started=ln_time_us();
        }
        ln_core_watchdog_feed();
    }
}
