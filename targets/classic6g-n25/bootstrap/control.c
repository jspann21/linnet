#include "hal/link/link.h"
#include "boot/include/linnet_bytes.h"
#include "hal/s5l8702/lcd_bus.h"
#include "hal/s5l8702/registers.h"
#include "targets/classic6g-n25/board.h"
#include "hal/s5l8702/board.h"
#include "targets/classic6g-n25/panel.h"
#include "hal/api/input.h"
#include "hal/api/audio.h"
#include "hal/s5l8702/i2c.h"
#include "hal/codecs/codec.h"
#include "hal/s5l8702/pcm_dma.h"
#include "hal/api/display.h"
#include "hal/api/ata.h"
#include "hal/pmu/pcf50635.h"
#include "hal/arm926/cache.h"
#include "hal/arm926/mmu.h"
#include "boot/include/sha256.h"
#include "targets/classic6g-n25/flash.h"
#include "hal/s5l8702/irq.h"
#include "hal/s5l8702/timer.h"
#include "hal/s5l8702/usb.h"
#include "hal/s5l8702/aes.h"
#include "hal/s5l8702/watchdog.h"
#include "hal/api/core.h"
#include "hal/api/power.h"
extern bool ln_bootstrap_memory_ready(void);
extern uint32_t ln_time_us(void);
static uint32_t codec_phase,codec_deadline;
static uint32_t tone_remaining,tone_deadline,tone_completed,tone_error;
static int16_t tone[128] __attribute__((aligned(32)));
static bool codec_configured;
static uint32_t page_table[4096] __attribute__((section(".dram.mmu"),aligned(16384)));
static volatile uint32_t cache_words[16] __attribute__((section(".dram.pcm"),aligned(32)));
static int16_t pcm_buffer[88200*2] __attribute__((section(".dram.pcm"),aligned(32)));
static uint32_t pcm_active,pcm_started,pcm_elapsed,pcm_error,pcm_complete;
static uint32_t pcm_irq_mode,pcm_saved_flags;
static bool audio_hal_active,audio_hal_paused;
static uint32_t audio_hal_flags,audio_hal_remaining;
static struct ln_audio_status audio_result;
static uint8_t flash_sector[4096] __attribute__((section(".dram.flash"),aligned(32)));
static uint32_t flash_parts;
static unsigned reset_requested;
unsigned ln_bootstrap_reset_take(void){unsigned pending=reset_requested;reset_requested=0;return pending;}
static struct ln_pcf50635_adc battery;
static volatile uint32_t software_irqs,unexpected_irqs,timer_irqs,dma_irqs;
static void interrupt_probe(uint32_t first,uint32_t second) {
    if(first&(1u<<31)){ln_s5l_irq_software(31,false);software_irqs++;}
    if((first&(1u<<7)) && ln_s5l_deadline_pending()){ln_s5l_deadline_stop();timer_irqs++;}
    if(first&(1u<<16))dma_irqs++;
    if((first&~((1u<<31)|(1u<<7)|(1u<<16)))||second)unexpected_irqs++;
    for(unsigned i=0;i<32;i++) {
        if(first&(1u<<i))ln_s5l_irq_enable(i,false);
        if(second&(1u<<i))ln_s5l_irq_enable(32+i,false);
    }
}
static void audio_interrupt(uint32_t first,uint32_t second) {
    if(first&(1u<<16))ln_audio_irq();
    for(unsigned i=0;i<32;i++) {
        if(i!=16 && (first&(1u<<i)))ln_s5l_irq_enable(i,false);
        if(second&(1u<<i))ln_s5l_irq_enable(32+i,false);
    }
}
static bool pmu_read(uint8_t reg,uint8_t *data,unsigned size){return ln_s5l_i2c_read(0,0x73,reg,data,size);}
static bool pmu_write(uint8_t reg,uint8_t value){return ln_s5l_i2c_write(0,0x73,reg,&value,1);}
static bool codec_write(uint8_t reg,uint16_t value) {
    uint8_t byte=(uint8_t)value;
    return value<=255 && ln_s5l_i2c_write(0,0x4a,reg,&byte,1);
}
static bool flash_hash(uint32_t offset,uint8_t digest[32]) {
    uint8_t block[512];struct ln_sha256 hash;ln_sha256_init(&hash);
    for(unsigned i=0;i<8;i++) {
        if(!ln_n25_flash_read(offset+i*512u,block,512))return false;
        ln_sha256_update(&hash,block,512);
    }
    ln_sha256_finish(&hash,digest);return true;
}
static bool hash_equal(const uint8_t *a,const uint8_t *b) {
    uint8_t difference=0;for(unsigned i=0;i<32;i++)difference|=a[i]^b[i];return !difference;
}
bool ln_bootstrap_audio_stop(void) {
    if(audio_hal_active) {
        (void)ln_s5l_irq_save();ln_s5l_irq_enable(16,false);ln_s5l_irq_restore(audio_hal_flags);
        audio_hal_active=false;
    }
    bool halted=ln_audio_stop();
    tone_remaining=0;
    pcm_active=0;
    if(pcm_irq_mode){(void)ln_s5l_irq_save();ln_s5l_irq_enable(16,false);ln_s5l_irq_restore(pcm_saved_flags);pcm_irq_mode=0;}
    bool muted=!codec_configured || ln_codec_mute(codec_write,true);
    bool stopped=ln_s5l_pcm_stop();
    return muted && stopped && halted;
}
void ln_bootstrap_audio_poll(void) {
    if(audio_hal_active) {
        ln_audio_service(ln_time_us());
        if(audio_hal_remaining) {
            uint32_t frames=audio_hal_remaining>2048?2048:audio_hal_remaining;
            uint32_t sent=ln_audio_submit(pcm_buffer,frames);audio_hal_remaining-=sent;
            if(!audio_hal_remaining)ln_audio_finish();
        }
        ln_audio_snapshot(&audio_result);
        if(audio_result.fault || (!audio_result.playing && !audio_hal_paused)) {
            (void)ln_s5l_irq_save();ln_s5l_irq_enable(16,false);ln_s5l_irq_restore(audio_hal_flags);
            audio_hal_active=false;(void)ln_audio_stop();
        }
    }
    if(pcm_active) {
        uint32_t status[5];ln_s5l_pcm_status(status);
        pcm_elapsed=ln_time_us()-pcm_started;
        if(status[0] || status[1] || pcm_elapsed>=3000000u) {
            pcm_complete=status[0]!=0;pcm_error=status[1] || !pcm_complete;
            pcm_active=0;
            if(!ln_s5l_pcm_stop())pcm_error=2;
            if(pcm_irq_mode) {
                (void)ln_s5l_irq_save();ln_s5l_irq_enable(16,false);ln_s5l_irq_restore(pcm_saved_flags);
                if(dma_irqs!=1 || unexpected_irqs)pcm_error=3;
                pcm_irq_mode=0;
            }
        }
    }
    if(!tone_remaining)return;
    uint32_t status[5];ln_s5l_pcm_status(status);
    if(status[1] || (int32_t)(ln_time_us()-tone_deadline)>=0)tone_error=1;
    if(status[0]){tone_completed++;tone_remaining--;}
    if(tone_error || !tone_remaining) {
        tone_remaining=0;
        if(!ln_codec_mute(codec_write,true) || !ln_s5l_pcm_stop())tone_error=2;
    } else if(status[0] && !ln_s5l_pcm_begin(tone,64)) {
        tone_error=3;tone_remaining=0;
        (void)ln_codec_mute(codec_write,true);(void)ln_s5l_pcm_stop();
    }
}

extern void ln_storage_probe(unsigned operation,uint32_t lba,uint32_t result[16]);

/* Keep the repeated diagnostic header out of each command branch so the
 * complete monitor fits its fixed NOR reservation without losing probes. */
__attribute__((noinline)) static void reply_header(uint8_t *reply,uint32_t operation) {
    ln_write_le32(reply,0x4252504cu);ln_write_le32(reply+4,operation);
}

/* Share response fields and byte copies across the diagnostic commands. */
__attribute__((noinline)) static void reply_status(uint8_t *reply,uint32_t operation,uint32_t value) {
    reply_header(reply,operation);ln_write_le32(reply+8,value);
}
__attribute__((noinline)) static void reply_copy(uint8_t *destination,const uint8_t *source,unsigned length) {
    for(unsigned i=0;i<length;i++)destination[i]=source[i];
}

/* This command belongs to the SRAM monitor, not the runtime storage API.
 * Only IDENTIFY and single-sector reads are exposed; no ATA write opcode. */
int ln_bootstrap_control(const ln_link_frame *request,ln_link_frame *response,uint8_t *reply) {
    response->command=request->command;response->sequence=request->sequence;
    response->payload=reply;response->length=2;
    response->flags=LINNET_LINK_ACK|LINNET_LINK_ERROR;reply[0]=1;reply[1]=0;
    if(request->flags || request->length<8)return 1;
    uint32_t operation=ln_read_le32(request->payload),lba=ln_read_le32(request->payload+4);
    if(request->length!=8 && !((operation==49 || operation==50) && request->length>=24 &&
       request->length<=520 && !((request->length-8)&15u)) &&
       !(operation==51 && request->length==520) && !(operation==52 && request->length==72))return 1;
    if(audio_hal_active && operation!=45 && operation!=46 && operation!=47 && operation!=48)return 1;
    if(tone_remaining && operation!=20)return 1;
    if(pcm_active && operation!=35)return 1;
    if((operation==62 || operation==63 || operation==64) && !lba) {
        const volatile uint8_t *data=(const volatile uint8_t *)(uintptr_t)(operation==62?0x0bfff000u:operation==63?0x22017d00u:0x22017c00u);
        reply_status(reply,operation,1);
        for(unsigned i=0;i<256;i++)reply[12+i]=data[i];
        response->length=268;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==60 || operation==61) && !lba) {
        bool ok=operation!=60 || ln_power_init();
        struct ln_power_state state;ok=ln_power_poll(ln_time_us(),&state)&&ok;
        struct ln_clock_state clock;ln_power_clock_snapshot(&clock);
        reply_status(reply,operation,ok);
        const uint8_t *data=(const uint8_t *)&state;
        reply_copy(reply+12,data,sizeof(state));
        unsigned at=12+sizeof(state);reply[at]=clock.hour;reply[at+1]=clock.minute;
        reply[at+2]=clock.second;reply[at+3]=clock.valid;
        uint8_t id[2]={0};(void)pmu_read(0,id,2);reply[at+4]=id[0];reply[at+5]=id[1];
        response->length=at+6;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==58 && !lba) || (operation==59 && lba>=100 && lba<=1000000)) {
        bool ok=operation!=58 || ln_core_init();
        if(operation==59)ln_core_idle_until(ln_time_us()+lba);
        struct ln_core_status state;ln_core_snapshot(&state);
        reply_status(reply,operation,ok);
        const uint8_t *data=(const uint8_t *)&state;
        reply_copy(reply+12,data,sizeof(state));
        response->length=12+sizeof(state);response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==53 || operation==56 || operation==57 || operation==66 || operation==67) && lba==0x52454254u) {
        reset_requested=operation==53?1:operation==56?2:operation==57?3:operation==66?4:5;
        reply_status(reply,operation,1);
        response->length=12;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==54 && lba>=10 && lba<=1000) || (operation==55 && !lba)) {
        bool ok=operation==55 || ln_s5l_watchdog_timer(lba);
        uint32_t state[4];ln_s5l_watchdog_snapshot(state);
        reply_status(reply,operation,ok);
        for(unsigned i=0;i<4;i++)ln_write_le32(reply+12+i*4,state[i]);
        response->length=28;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==51 && request->length==520 && lba<8 && ln_bootstrap_memory_ready()) {
        if(!lba)flash_parts=0;
        reply_copy(flash_sector+lba*512u,request->payload+8,512);
        flash_parts|=1u<<lba;
        reply_status(reply,operation,flash_parts);
        response->length=12;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==52 && request->length==72 && !(lba&4095u) && lba>=0x8000u && lba<0x28000u &&
       flash_parts==255u && ln_bootstrap_memory_ready()) {
        uint8_t digest[32]={0};struct ln_sha256 hash;ln_sha256_init(&hash);
        ln_sha256_update(&hash,flash_sector,sizeof(flash_sector));ln_sha256_finish(&hash,digest);
        uint32_t error=hash_equal(digest,request->payload+40)?0:10;
        flash_parts=0;
        if(!error && (!flash_hash(lba,digest) || !hash_equal(digest,request->payload+8)))error=11;
        if(!error)error=ln_n25_flash_replace_sector(lba,flash_sector);
        bool readback=flash_hash(lba,digest);
        if(!error && (!readback || !hash_equal(digest,request->payload+40)))error=12;
        reply_status(reply,operation,!error);
        ln_write_le32(reply+12,error);reply_copy(reply+16,digest,32);
        response->length=48;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==49 || operation==50) && (lba==1 || lba==2) && request->length>8) {
        static uint8_t block[512] __attribute__((aligned(32)));
        uint32_t bytes=request->length-8;
        reply_copy(block,request->payload+8,bytes);
        bool ok=ln_s5l_aes_crypt(block,bytes,lba,operation==49);
        reply_status(reply,operation,ok);
        if(ok)reply_copy(reply+12,block,bytes);
        response->length=ok?12+bytes:12;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==44 || (operation>=45 && operation<=48 && !lba)) && ln_bootstrap_memory_ready()) {
        bool ok=true;
        if(operation==44) {
            if(!ln_codec_rate_supported(lba))return 1;
            codec_configured=false;audio_hal_paused=false;audio_result=(struct ln_audio_status){0};
            for(unsigned i=0;i<32768*2;i++)pcm_buffer[i]=0;
            audio_hal_flags=ln_s5l_irq_save();ln_s5l_irq_init(audio_interrupt);
            ok=ln_audio_init(lba);
            uint32_t frames=lba*3u,first=frames>32768u?32768u:frames;
            if(ok)ok=ln_audio_submit(pcm_buffer,first)==first;
            audio_hal_remaining=frames-first;
            if(ok && !audio_hal_remaining)ln_audio_finish();
            if(ok)ok=ln_audio_start();
            audio_hal_active=ok;
            ln_audio_snapshot(&audio_result);
            if(ok)ln_s5l_irq_restore(audio_hal_flags&~0x80u);
            else {(void)ln_audio_stop();ln_s5l_irq_restore(audio_hal_flags);}
        } else if(operation==46 && audio_hal_active){audio_hal_paused=true;ln_audio_pause();}
        else if(operation==47 && audio_hal_active){ok=ln_audio_start();if(ok)audio_hal_paused=false;}
        else if(operation==48 && audio_hal_active) {
            ln_audio_snapshot(&audio_result);ok=ln_audio_stop();
            (void)ln_s5l_irq_save();ln_s5l_irq_enable(16,false);ln_s5l_irq_restore(audio_hal_flags);
            audio_hal_active=false;audio_hal_paused=false;
        } else if(operation>=46)ok=false;
        uint32_t values[16]={0x4252504cu,operation,ok,audio_hal_active,audio_result.rate,audio_result.queued,
            audio_result.consumed,audio_result.submitted,audio_result.interrupts,audio_result.underruns,
            audio_result.silence_frames,audio_result.fault,audio_result.max_irq_us,
            audio_result.max_completion_interval_us,audio_result.max_completion_late_us,audio_hal_remaining};
        for(unsigned i=0;i<16;i++)ln_write_le32(reply+i*4,values[i]);
        response->length=64;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==41 || operation==42 || operation==43) && !lba && ln_bootstrap_memory_ready()) {
        uint32_t flags=ln_s5l_irq_save(),failed=0;
        bool ok;
        if(operation==41)ok=ln_arm926_mmu_enable(page_table,0x08000000u,0x04000000u);
        else if(operation==42)ok=ln_arm926_mmu_disable();
        else {
            ok=(ln_arm926_cache_control()&5u)==5u;
            if(ok) {
                volatile uint32_t *alias=(volatile uint32_t *)((uintptr_t)cache_words+0x10000000u);
                for(unsigned i=0;i<16;i++)cache_words[i]=0xa55a0000u|i;
                ln_arm926_data_clean_range((const void *)cache_words,sizeof(cache_words));
                for(unsigned i=0;i<16;i++)if(alias[i]!=(0xa55a0000u|i))failed|=1u;
                for(unsigned i=0;i<16;i++)alias[i]=0x5aa50000u|i;
                (void)ln_arm926_data_invalidate_range((void *)cache_words,sizeof(cache_words));
                for(unsigned i=0;i<16;i++)if(cache_words[i]!=(0x5aa50000u|i))failed|=2u;
                ok=!failed;
            }
        }
        ln_s5l_irq_restore(flags);
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,ln_arm926_cache_control());ln_write_le32(reply+16,failed);
        ln_write_le32(reply+20,(uint32_t)(uintptr_t)page_table);
        response->length=24;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==38 && !lba) {
        uint32_t values[64];ln_s5l_usb_trace(values);
        reply_status(reply,operation,1);
        for(unsigned i=0;i<64;i++)ln_write_le32(reply+12+i*4,values[i]);
        response->length=268;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==37 || operation==39) && lba>=100 && lba<=1000000u) {
        uint32_t flags=ln_s5l_irq_save();
        ln_s5l_irq_init(interrupt_probe);timer_irqs=unexpected_irqs=0;
        uint32_t started=ln_time_us();bool ok=ln_s5l_deadline_arm(lba);
        ln_s5l_irq_enable(7,true);
        if(operation==39 && ok)ln_s5l_irq_wait();
        ln_s5l_irq_restore(flags&~0x80u);
        for(unsigned budget=20000000;!timer_irqs && budget;--budget)
            if(ln_time_us()-started>=lba+10000u)break;
        (void)ln_s5l_irq_save();uint32_t elapsed=ln_time_us()-started;
        ln_s5l_deadline_stop();ln_s5l_irq_enable(7,false);ln_s5l_irq_restore(flags);
        ok=ok && timer_irqs==1 && !unexpected_irqs;
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,timer_irqs);ln_write_le32(reply+16,unexpected_irqs);
        ln_write_le32(reply+20,elapsed);response->length=24;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==36 && !lba) {
        bool ok=LN_REG(0x18u)==LN_REG(0x22000018u) && !(ln_arm926_cache_control()&(1u<<13));
        uint32_t started=ln_time_us(),flags=ln_s5l_irq_save();
        software_irqs=unexpected_irqs=0;
        if(ok) {
            ln_s5l_irq_init(interrupt_probe);ln_s5l_irq_enable(31,true);ln_s5l_irq_software(31,true);
            ln_s5l_irq_restore(flags&~0x80u);
            for(unsigned budget=1000000;!software_irqs && budget;--budget)
                if(ln_time_us()-started>=10000u)break;
            (void)ln_s5l_irq_save();ln_s5l_irq_enable(31,false);ln_s5l_irq_software(31,false);
            ok=software_irqs==1 && !unexpected_irqs;
        }
        ln_s5l_irq_restore(flags);
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,software_irqs);ln_write_le32(reply+16,unexpected_irqs);
        ln_write_le32(reply+20,ln_time_us()-started);response->length=24;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==34 || operation==35 || operation==40) && !lba && codec_configured && ln_bootstrap_memory_ready()) {
        bool ok=true;
        if(operation==34 || operation==40) {
            for(unsigned i=0;i<88200*2;i++)pcm_buffer[i]=0;
            LN_REG(0x3ca00040u)=12000000u/44100u;
            pcm_complete=pcm_error=pcm_elapsed=0;pcm_started=ln_time_us();
            ok=ln_codec_mute(codec_write,true) && ln_s5l_pcm_begin_buffer(pcm_buffer,88200);
            pcm_active=ok;
            dma_irqs=0;
            if(ok && operation==40) {
                pcm_saved_flags=ln_s5l_irq_save();ln_s5l_irq_init(interrupt_probe);unexpected_irqs=0;
                pcm_irq_mode=1;ln_s5l_pcm_interrupt(true);ln_s5l_irq_enable(16,true);
                ln_s5l_irq_restore(pcm_saved_flags&~0x80u);
            }
        }
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,pcm_active);ln_write_le32(reply+16,pcm_complete);
        ln_write_le32(reply+20,pcm_error);ln_write_le32(reply+24,pcm_elapsed);
        ln_write_le32(reply+28,dma_irqs);response->length=32;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==32 && !lba) {
        uint8_t id[3]={0};bool ok=ln_n25_flash_identity(id);
        reply_status(reply,operation,ok);
        for(unsigned i=0;i<3;i++)reply[12+i]=id[i];
        response->length=15;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==33 && !(lba&511) && lba<0x1000000u) {
        bool ok=ln_n25_flash_read(lba,reply+12,512);
        reply_status(reply,operation,ok);
        response->length=ok?524:12;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==30 && lba<=1) {
        bool ok=ln_arm926_instruction_cache(lba!=0);
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,ln_arm926_cache_control());response->length=16;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==31 && !lba) {
        static const uint8_t block[64]={0};struct ln_sha256 hash;
        uint32_t started=ln_time_us();ln_sha256_init(&hash);
        for(unsigned i=0;i<512;i++)ln_sha256_update(&hash,block,sizeof(block));
        ln_sha256_finish(&hash,reply+16);
        reply_header(reply,operation);
        ln_write_le32(reply+8,ln_time_us()-started);ln_write_le32(reply+12,ln_arm926_cache_control());
        response->length=48;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==28 || operation==29) && !lba) {
        int status=operation==28?(ln_pcf50635_battery_begin(&battery,pmu_write,ln_time_us())?0:-1):
            ln_pcf50635_battery_poll(&battery,pmu_read,ln_time_us());
        reply_status(reply,operation,(uint32_t)status);
        ln_write_le32(reply+12,battery.raw);ln_write_le32(reply+16,battery.millivolts);
        response->length=20;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==27 && !lba) {
        reply_header(reply,operation);
        for(unsigned i=0;i<20;i++)ln_write_le32(reply+8+i*4,LN_REG(0x3c500000u+i*4));
        uint32_t control;__asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(control));
        ln_write_le32(reply+88,control);response->length=92;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation>=24 && operation<=26) {
        static uint8_t sector_data[512];
        bool ok=true;
        if(operation==24 && !lba)ln_ata_init(ln_time_us());
        else if(operation==25 && !lba)ln_ata_step(ln_time_us());
        else if(operation==26)ok=ln_ata_start(LN_ATA_READ,lba,1,sector_data,ln_time_us()+2000000u);
        else return 1;
        struct ln_ata_state state;ln_ata_state_snapshot(&state);
        struct ln_ata_result result={0};bool finished=ln_ata_take(&result);
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,state.initialized);ln_write_le32(reply+16,state.initializing);
        ln_write_le32(reply+20,state.failed);ln_write_le32(reply+24,state.busy);
        ln_write_le32(reply+28,(uint32_t)state.sectors);ln_write_le32(reply+32,finished?result.status:0xffffffffu);
        response->length=36;
        if(finished && !result.status && result.sectors_done==1){
            reply_copy(reply+36,sector_data,512);response->length+=512;
        }
        response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation>=21 && operation<=23 && ln_bootstrap_memory_ready()) {
        bool ok=true;
        if(operation==21 && !lba) {
            ok=ln_display_init();
            uint16_t *pixels=ln_display_framebuffer();
            if(ok) {
                for(unsigned y=0;y<240;y++)for(unsigned x=0;x<320;x++)
                    pixels[y*320+x]=(uint16_t)(((x&31)<<11)|((y&63)<<5)|((x+y)&31));
                ok=ln_display_present();
            }
        } else if(operation==22 && !lba)ok=ln_display_service(256);
        else if(operation==23 && lba<300) {
            const uint16_t *pixels=ln_display_snapshot();
            if(!pixels)return 1;
            for(unsigned i=0;i<256;i++)ln_write_le16(reply+12+i*2,pixels[lba*256+i]);
            reply_status(reply,operation,lba);
            response->length=524;response->flags=LINNET_LINK_ACK;return 1;
        } else return 1;
        struct ln_display_presentation status;ln_display_presentation_snapshot(&status);
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,ln_display_busy());ln_write_le32(reply+16,status.frames_completed);
        ln_write_le32(reply+20,ln_display_busy()?0:ln_display_snapshot_crc32());
        response->length=24;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==19 || operation==20) && !lba && codec_configured) {
        bool ok=true;
        if(operation==19 && !tone_remaining) {
            for(unsigned i=0;i<64;i++) {
                int16_t sample=(int16_t)((i<32?(int)i:63-(int)i)*32-496);
                tone[i*2]=sample;tone[i*2+1]=sample;
            }
            LN_REG(0x3ca00040u)=12000000u/44100u;
            tone_completed=tone_error=0;tone_deadline=ln_time_us()+3000000u;
            ok=ln_codec_volume(codec_write,-180) && ln_s5l_pcm_begin(tone,64) && ln_codec_mute(codec_write,false);
            if(ok)tone_remaining=1000;
            else {(void)ln_codec_mute(codec_write,true);(void)ln_s5l_pcm_stop();}
        } else if(operation==19)ok=false;
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,tone_remaining);ln_write_le32(reply+16,tone_completed);ln_write_le32(reply+20,tone_error);
        response->length=24;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==17 || operation==18) && !lba && codec_phase==3) {
        static const int16_t silence[128] __attribute__((aligned(32)))={0};
        if(operation==17)LN_REG(0x3ca00040u)=12000000u/44100u;
        bool ok=operation!=17 || ln_s5l_pcm_begin(silence,64);
        uint32_t status[5];ln_s5l_pcm_status(status);
        if(operation==18)ok=ln_s5l_pcm_stop() && ok;
        reply_status(reply,operation,ok);
        for(unsigned i=0;i<5;i++)ln_write_le32(reply+12+i*4,status[i]);
        response->length=32;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==16 && !lba && codec_phase==3) {
        bool ok=ln_codec_init(codec_write,44100);codec_configured=ok;
        reply_header(reply,operation);
        for(unsigned i=0;i<32;i++)reply[12+i]=0xff;
        for(unsigned i=1;i<32 && ok;i++)ok=ln_s5l_i2c_read(0,0x4a,(uint8_t)i,reply+12+i,1);
        ln_write_le32(reply+8,ok);response->length=44;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==14 || operation==15) && !lba) {
        if(operation==14){codec_configured=false;codec_phase=ln_s5l_board_audio_power(true)?1u:0xffffffffu;codec_deadline=ln_time_us()+10000u;}
        else if(codec_phase==1 && (int32_t)(ln_time_us()-codec_deadline)>=0) {
            codec_phase=ln_s5l_board_audio_power(false)?2u:0xffffffffu;codec_deadline=ln_time_us()+1000u;
        } else if(codec_phase==2 && (int32_t)(ln_time_us()-codec_deadline)>=0)codec_phase=3;
        reply_header(reply,operation);
        response->length=12;
        if(codec_phase==3) {
            bool ok=true;
            for(unsigned i=0;i<16;i++)reply[12+i]=0xff;
            for(unsigned i=1;i<16 && ok;i++)ok=ln_s5l_i2c_read(0,0x4a,(uint8_t)i,reply+12+i,1);
            if(ok)response->length+=16;else codec_phase=0xffffffffu;
        }
        ln_write_le32(reply+8,codec_phase);response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==12 || operation==13) && !lba) {
        struct ln_input_state state={0};
        bool ok=operation!=12 || ln_input_init();
        ok=ln_input_poll(&state)&&ok;
        reply_status(reply,operation,ok);
        ln_write_le32(reply+12,state.buttons);ln_write_le32(reply+16,state.packets);
        ln_write_le32(reply+20,state.invalid_packets);ln_write_le32(reply+24,(uint32_t)state.wheel_delta);
        reply[28]=state.wheel_position;reply[29]=state.touched;reply[30]=state.hold;reply[31]=state.headphones;
        uint32_t diagnostic[16];ln_input_diagnostics(diagnostic);
        for(unsigned i=0;i<16;i++)ln_write_le32(reply+32+i*4,diagnostic[i]);
        response->length=96;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==11 && !lba) {
        reply_header(reply,operation);
        for(unsigned i=0;i<16;i++) {
            ln_write_le32(reply+8+i*8,LN_REG(0x3cf00000u+i*32));
            ln_write_le32(reply+12+i*8,LN_REG(0x3cf00004u+i*32));
        }
        ln_write_le32(reply+136,LN_REG(0x38300000u));
        ln_write_le32(reply+140,LN_REG(0x38300024u));
        ln_write_le32(reply+144,LN_REG(0x38300028u));
        response->length=148;response->flags=LINNET_LINK_ACK;return 1;
    }
    if((operation==9 || operation==10) && !lba) {
        uint32_t phase=operation==9?(ln_n25_panel_start(true)?1u:0xffffffffu):ln_n25_panel_poll();
        reply_status(reply,operation,phase);
        response->length=12;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation==8 && (lba==0 || lba==0x0a || lba==0x0b || lba==0x0c || lba==0x0d)) {
        bool powered=ln_n25_lcd_power();
        LN_REG(0x38300020u)=0x33;
        bool ok=powered && ln_s5l_lcd_mode(0x80000c20u) && ln_s5l_lcd_command(lba?(uint16_t)lba:4);
        uint8_t id[4]={0};
        for(unsigned i=0;i<4 && ok;i++)ok=ln_s5l_lcd_read(id+i);
        reply_status(reply,operation,ok);ln_write_le32(reply+12,LN_REG(0x3830001cu));
        for(unsigned i=0;i<4;i++)reply[16+i]=id[i];
        response->length=20;response->flags=LINNET_LINK_ACK;return 1;
    }
    if(operation<2 || operation>4 || (operation!=4 && lba))return 1;
    uint32_t result[16]={0};
    ln_storage_probe(operation,lba,result);
    reply_header(reply,operation);
    for(unsigned i=3;i<7;i++)ln_write_le32(reply+8+(i-3)*4,result[i]);
    response->length=24;
    if(result[3]==8 && !result[4] && result[5]==256) {
        const uint8_t *data=(const uint8_t *)(uintptr_t)result[7];
        reply_copy(reply+24,data,512);
        response->length+=512;
    }
    response->flags=LINNET_LINK_ACK;return 1;
}
