#include "hal/api/audio.h"
#include "hal/api/click_pcm.h"
#include "hal/api/core.h"
#include "hal/codecs/codec.h"
#include "hal/codecs/cs42l55.h"
#include "hal/arm926/cache.h"
#include "board.h"
#include "irq.h"
#include "pcm_dma.h"
#if !defined(LINNET_PROFILE_RELEASE)
#include "i2c.h"
#include "registers.h"
#endif
extern void ln_memory_copy(void *,const void *,uint32_t);
extern uint32_t ln_time_us(void);
#ifdef LINNET_SRAM_MONITOR
#define AUDIO_MEMORY __attribute__((section(".dram.audio"),aligned(32)))
#else
/* DMA sees only submitted, cache-cleaned PCM; ring contents need no reset. */
#define AUDIO_MEMORY __attribute__((section(".media_workspace"),aligned(32)))
#endif
#define BLOCK 1024u
#define MASK (LINNET_AUDIO_RING_FRAMES-1u)
static uint32_t ring[LINNET_AUDIO_RING_FRAMES] AUDIO_MEMORY;
static uint32_t click_pcm[BLOCK] AUDIO_MEMORY;
static uint32_t onset[64] AUDIO_MEMORY;
static volatile uint32_t fade_remaining;
static volatile struct ln_pcm_click click;
static bool click_owned;
static volatile bool click_block,click_playing;
static uint32_t click_last;
static void copy_frames(uint32_t offset,const int16_t *samples,uint32_t frames) {
    if(frames>=8u && !((uintptr_t)samples&3u))ln_memory_copy(ring+offset,samples,frames*4u);
    else for(uint32_t i=0;i<frames;i++)ring[offset+i]=(uint16_t)samples[i*2u]|
        ((uint32_t)(uint16_t)samples[i*2u+1u]<<16);
}
/* Initialized explicitly; the first 64 frames also carry the natural EOF
 * ramp, then are cleared after that DMA completes. */
static uint32_t silence[BLOCK] AUDIO_MEMORY;
static volatile struct ln_audio_status status;
static volatile uint32_t head,tail,inflight,armed,expected,previous,drain_until;
static volatile uint32_t inflight_last;
static volatile bool active,real,finishing,pause_pending,draining,starved,warming,start_armed,drain_ready,tail_ramp;
static bool owned,codec_ready,lineout,muted,amplifier_open,clock_valid,powerdown_ready;
static uint32_t frame_q12;
static volatile uint32_t mute_until;
static void ramp_to_silence(uint32_t sample) {
    int32_t left=(int16_t)sample,right=(int16_t)(sample>>16);
    for(uint32_t i=0;i<64u;i++){
        int32_t gain=(int32_t)(64u-i);
        silence[i]=(uint16_t)(left*gain/64)|((uint32_t)(uint16_t)(right*gain/64)<<16);
    }
    tail_ramp=true;
}
static uint32_t frame_period(uint32_t rate) {
    switch(rate) {
    case 8000:return 4096000000u/8000;case 11025:return 4096000000u/11025;
    case 12000:return 4096000000u/12000;case 16000:return 4096000000u/16000;
    case 22050:return 4096000000u/22050;case 24000:return 4096000000u/24000;
    case 32000:return 4096000000u/32000;case 44100:return 4096000000u/44100;
    case 48000:return 4096000000u/48000;default:return 0;
    }
}
static void fault(uint32_t reason) {
    if(!status.fault)status.fault=reason;
    ln_s5l_irq_enable(16,false);status.playing=0;
}
static bool arm(bool continuing) {
    uint32_t queued=head-tail,count=status.playing?queued:0,offset=tail&MASK;
    if(queued>LINNET_AUDIO_RING_FRAMES){fault(1);return false;}
    bool silence_only=warming||draining;
    click_block=!status.playing&&!silence_only;
    if(silence_only)count=0;
    real=count!=0;
    if(count) {
        if(count>BLOCK)count=BLOCK;
        if(count>LINNET_AUDIO_RING_FRAMES-offset)count=LINNET_AUDIO_RING_FRAMES-offset;
        starved=false;
    } else {
        count=click_block?click.left:BLOCK;
        if(!silence_only&&!click_block){if(!starved)status.underruns++;starved=true;}
    }
    if(!count)return false;
    /* Bound feedback synthesis and cache cleaning before rearming DMA. The
     * rest of a music block can continue directly from the immutable ring. */
    if(!silence_only&&click.left){
        if(count>click.left)count=click.left;
        if(count>64u)count=64u;
    }
    const int16_t *samples=(const int16_t *)(real?ring+offset:silence);
    if(real&&fade_remaining){
        if(count>fade_remaining)count=fade_remaining;
        for(uint32_t i=0;i<count;i++){
            uint32_t sample=((const uint32_t *)samples)[i];
            int32_t gain=(int32_t)(64u-fade_remaining+i);
            onset[i]=(uint16_t)((int16_t)sample*gain/64)|
                ((uint32_t)(uint16_t)((int16_t)(sample>>16)*gain/64)<<16);
        }
        fade_remaining-=count;samples=(const int16_t *)onset;
    }
    if(!silence_only&&ln_pcm_click_mix(&click,click_pcm,(const uint32_t *)samples,count))samples=(const int16_t *)click_pcm;
    inflight_last=real?((const uint32_t *)samples)[count-1u]:0;
    bool ok=continuing?ln_s5l_pcm_rearm(samples,count):ln_s5l_pcm_begin(samples,count);
    if(!ok){fault(2);return false;}
    inflight=count;active=true;armed=ln_time_us();expected=(count*frame_q12+4095u)>>12;
    if(real||click_block)start_armed=true;
    ln_s5l_pcm_interrupt(true);ln_s5l_irq_enable(16,true);return true;
}
bool ln_audio_volume(int32_t value) {
    if(value<-1020 || value>0 || !codec_ready || !ln_codec_volume(ln_s5l_board_codec_write,value))return false;
    status.volume_db_tenths=value;return true;
}
bool ln_audio_lineout(bool enabled) {
    if(codec_ready && !ln_codec_lineout(ln_s5l_board_codec_write,enabled))return false;
    lineout=enabled;return true;
}
bool ln_audio_halt(void) {
    ln_audio_click_stop();
    if(!owned)return true;
    bool ok=true;
    if(codec_ready&&!muted&&!status.playing&&!click_playing){
        muted=ln_codec_digital_mute(ln_s5l_board_codec_write,true);
        if(!muted||!ln_core_delay_us(10000u))return false;
    }
    if((status.playing||click_playing) && !status.fault) {
        /* A manual track change must drain through silence like Pause.
         * Aborting a live I2S FIFO makes an audible output transient. */
        ln_audio_pause();
        uint32_t began=ln_time_us();
        uint32_t block_us=(BLOCK*frame_q12+4095u)>>12;
        uint32_t timeout=5u*block_us+30000u;
        while((status.playing||click_playing) && !status.fault && ln_time_us()-began<timeout) {
            uint32_t now=ln_time_us();ln_audio_service(now);
            if(status.playing||click_playing)ln_core_idle_until(now+1000u);
        }
        if(status.playing || click_playing || status.fault)ok=false;
    }
    /* Retain the headphone PGA across warm stream changes. The completed
     * digital ramp and zero DMA provide silence; Stop still mutes/powers off. */
    if(codec_ready&&(status.fault||!ok)){
        if(ln_codec_mute(ln_s5l_board_codec_write,true)){amplifier_open=false;muted=true;}
        else ok=false;
    }
    uint32_t flags=ln_s5l_irq_save();ln_s5l_irq_enable(16,false);
    bool stopped=ln_s5l_pcm_stop();
    if(stopped){active=draining=pause_pending=warming=click_playing=false;head=tail=inflight=0;status.initialized=status.playing=status.paused=0;}
    else fault(3);
    ln_s5l_irq_restore(flags);
    if(!stopped&&codec_ready){
        /* pcm_stop may have raised the first fault after the earlier check. */
        if(ln_codec_mute(ln_s5l_board_codec_write,true)){amplifier_open=false;muted=true;}
    }
    return ok && stopped;
}
static bool shutdown_codec(void){
    /* A failed rate change may leave old codec dividers with the new MCLK.
     * At the8MHz/32kHz board setting an old8kHz divider yields5.33kHz, so
     * 193ms covers1024frames plus1ms even in that uncertain configuration. */
    uint32_t settle=clock_valid?((BLOCK*frame_q12+4095u)>>12)+1000u:193000u;
    if(!powerdown_ready){
        if(!ln_cs42l55_shutdown(ln_s5l_board_codec_write,settle)){
            /* Partial preparation is not a reusable warm configuration. */
            codec_ready=false;status.fault=4;return false;
        }
        powerdown_ready=true;
    }
    codec_ready=amplifier_open=false;muted=true;
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
    if(!ln_s5l_board_audio_clock_off()){clock_valid=false;status.fault=4;return false;}
#endif
    owned=clock_valid=powerdown_ready=false;return true;
}
bool ln_audio_stop(void) {
    click_owned=false;
    if(!owned)return true;
    if(!ln_audio_halt())return false;
    /* Even a partially initialized codec must acknowledge power-down before
     * its MCLK can stop. A failed stop preserves ownership for recovery. */
    return shutdown_codec();
}
bool ln_audio_init(uint32_t rate) {
    click_owned=false;
    uint32_t period=frame_period(rate),codec_rate;
    if(!period || !ln_codec_rate_supported(rate))return false;
    bool warm=owned && codec_ready && !status.fault;
    uint32_t old_rate=status.rate;
    int32_t volume=warm?status.volume_db_tenths:-400;
    if(!ln_audio_halt())return false;
    status=(struct ln_audio_status){0};head=tail=inflight=0;active=draining=finishing=pause_pending=starved=warming=drain_ready=false;
    for(uint32_t i=0;i<BLOCK;i++)silence[i]=0;
    tail_ramp=false;fade_remaining=0;
    owned=true;previous=0;frame_q12=period;mute_until=0;
    if(!warm) {
        codec_ready=clock_valid=powerdown_ready=false;
        if(!ln_s5l_board_audio_power(true) || !ln_core_delay_us(1000) ||
           !ln_s5l_board_audio_power(false) || !ln_core_delay_us(1000))goto failed;
    }
    if(!warm || old_rate!=rate) {
        clock_valid=false;
        if(!ln_s5l_board_audio_clock(rate,&codec_rate))goto failed;
        if(warm) {if(!ln_codec_rate(ln_s5l_board_codec_write,codec_rate))goto failed;}
        else if(!ln_codec_init(ln_s5l_board_codec_write,codec_rate))goto failed;
        clock_valid=true;
    }
    codec_ready=true;muted=true;if(!warm)amplifier_open=false;
    if(warm)status.volume_db_tenths=volume;
    else if(!ln_audio_volume(volume) || !ln_codec_lineout(ln_s5l_board_codec_write,lineout))goto failed;
    status.rate=rate;status.initialized=1;return true;
failed:
    if(ln_s5l_pcm_stop())(void)shutdown_codec();
    else if(ln_codec_mute(ln_s5l_board_codec_write,true)){amplifier_open=false;muted=true;}
    status.fault=4;return false;
}
void ln_audio_click_stop(void){
    uint32_t flags=ln_s5l_irq_save();ln_pcm_click_clear(&click);ln_s5l_irq_restore(flags);
}
void ln_audio_click(uint8_t tone,uint8_t volume,int32_t output_db_tenths){
    if(tone>3u||tone==1u||!volume||volume>4u)return;
    if(!status.initialized){
        if(!ln_audio_init(44100u))return;
        click_owned=true;
        if(!ln_audio_volume(output_db_tenths)){(void)ln_audio_stop();return;}
    }
    if(status.fault)return;
    uint32_t flags=ln_s5l_irq_save();
    ln_pcm_click_request(&click,status.rate,tone,volume);click_last=ln_time_us();
    ln_s5l_irq_restore(flags);
    ln_audio_service(ln_time_us());
}
uint32_t ln_audio_capacity(void) {
    uint32_t queued=head-tail;
    return status.initialized && !status.fault && queued<=LINNET_AUDIO_RING_FRAMES?
        LINNET_AUDIO_RING_FRAMES-queued:0;
}
uint32_t ln_audio_submit(const int16_t *samples,uint32_t frames) {
    if(!samples || finishing)return 0;
    uint32_t room=ln_audio_capacity();if(frames>room)frames=room;
    uint32_t cursor=head,offset=cursor&MASK,first=frames;
    if(first>LINNET_AUDIO_RING_FRAMES-offset)first=LINNET_AUDIO_RING_FRAMES-offset;
    copy_frames(offset,samples,first);
    if(frames>first)copy_frames(0,samples+first*2u,frames-first);
    ln_arm926_data_clean_range(ring+offset,first*4u);
    if(frames>first)ln_arm926_data_clean_range(ring,(frames-first)*4u);
    __asm__ volatile("":::"memory");head=cursor+frames;status.submitted+=frames;return frames;
}
bool ln_audio_rewrite(uint32_t frame,const int16_t *samples,uint32_t frames) {
    /* Effect rewrites are bounded foreground work between completion IRQs. */
    if(!samples || frames>2048u)return false;
    uint32_t flags=ln_s5l_irq_save(),offset=frame-tail,queued=head-tail;
    bool ok=status.initialized && !status.fault && offset<=queued && frames<=queued-offset &&
        (!active || !real || offset>=inflight);
    if(ok) {
        uint32_t start=frame&MASK,first=frames;
        if(first>LINNET_AUDIO_RING_FRAMES-start)first=LINNET_AUDIO_RING_FRAMES-start;
        copy_frames(start,samples,first);
        if(frames>first)copy_frames(0,samples+first*2u,frames-first);
        ln_arm926_data_clean_range(ring+start,first*4u);
        if(frames>first)ln_arm926_data_clean_range(ring,(frames-first)*4u);
    }
    ln_s5l_irq_restore(flags);return ok;
}
static bool startup_wait(uint32_t began,uint32_t timeout) {
    uint32_t now=ln_time_us();
    if(status.fault)return false;
    if(now-began>=timeout){fault(6);return false;}
    ln_core_idle_until(now+1000u);return true;
}
/* Keep click-only transfers separate from media ownership and ring cursors. */
static bool start_output(bool media) {
    if(!status.initialized || status.fault || (media?head==tail:!click.left))return false;
    if(status.playing&&!pause_pending&&!draining&&!muted)return true;
    uint32_t flags=ln_s5l_irq_save();
    bool continuing=active&&real&&amplifier_open;
    pause_pending=draining=false;
    click_playing=!media;
    if(media){click_owned=false;status.paused=0;status.playing=1;}
    if(continuing){
        ln_s5l_irq_restore(flags);
        if(!ln_codec_digital_mute(ln_s5l_board_codec_write,false)){fault(4);goto failed;}
        muted=false;return true;
    }
    /* Flush the serial FIFO with real zero transfers before opening analog
     * outputs. Keep rearming zeros throughout the zero-cross settling time. */
    warming=true;start_armed=false;
    uint32_t zeros=status.silence_frames;
    if(!active)previous=0;
    bool ok=active||arm(false);ln_s5l_irq_restore(flags);
    if(!ok)goto failed;
    uint32_t block_us=(BLOCK*frame_q12+4095u)>>12;
    uint32_t began=ln_time_us(),timeout=4u*block_us+30000u;
    while(status.silence_frames==zeros)if(!startup_wait(began,timeout))goto failed;
    /* A partial unmute counts as open until a complete mute is acknowledged. */
    if(!amplifier_open){
        amplifier_open=true;
        if(!ln_codec_analog_mute(ln_s5l_board_codec_write,false)){fault(4);goto failed;}
    }
    uint32_t ready=ln_time_us()+block_us+1000u;
    while((int32_t)(ln_time_us()-ready)<0)if(!startup_wait(began,timeout))goto failed;
    /* A feedback tone is only four milliseconds long. It must not be sent
     * while the codec is still ramping out of mute. Release onto zero DMA
     * and clock a complete settling block before arming any nonzero PCM. */
    muted=false;
    if(!ln_codec_digital_mute(ln_s5l_board_codec_write,false)){fault(4);goto failed;}
    ready=ln_time_us()+block_us+1000u;
    while((int32_t)(ln_time_us()-ready)<0)if(!startup_wait(began,timeout+block_us+1000u))goto failed;
    fade_remaining=64u;warming=false;
    while(!start_armed)if(!startup_wait(began,timeout+block_us+1000u))goto failed;
    return true;
failed:
    warming=false;ln_audio_service(ln_time_us());return false;
}
bool ln_audio_start(void){return start_output(true);}
void ln_audio_pause(void) {
    if((!status.playing&&!click_playing) || pause_pending || draining)return;
    /* Keep LRCK running while the digital soft ramp settles. Headphone
     * amplifier power stays on while its output is subsequently muted. */
    if(!ln_codec_digital_mute(ln_s5l_board_codec_write,true)){fault(4);return;}
    muted=true;mute_until=ln_time_us()+((BLOCK*frame_q12+4095u)>>12);
    uint32_t flags=ln_s5l_irq_save();if(status.playing||click_playing)pause_pending=true;ln_s5l_irq_restore(flags);
}
void ln_audio_finish(void) {
    uint32_t flags=ln_s5l_irq_save();finishing=true;ln_s5l_irq_restore(flags);
}
void ln_audio_irq(void) {
    uint32_t started=ln_time_us(),words[5];ln_s5l_pcm_status(words);
    if(!active || words[1] || !words[0]){fault(5);return;}
    ln_s5l_irq_enable(16,false);status.interrupts++;
    uint32_t interval=started-previous;
    if(previous && interval>status.max_completion_interval_us)status.max_completion_interval_us=interval;
    previous=started;
    uint32_t elapsed=started-armed;
    if(elapsed>expected && elapsed-expected>status.max_completion_late_us)status.max_completion_late_us=elapsed-expected;
    uint32_t last_sample=0;
    if(real){tail+=inflight;status.consumed+=inflight;last_sample=inflight_last;}
    else if(!click_block)status.silence_frames+=inflight;
    if(tail_ramp){for(uint32_t i=0;i<64u;i++)silence[i]=0;tail_ramp=false;}
    active=false;inflight=0;status.last_irq_us=started;
    if(draining)drain_ready=true;
    else if(!warming&&((pause_pending&&(int32_t)(started-mute_until)>=0)||(status.playing&&finishing&&head==tail)||(click_block&&!status.playing&&!click.left))){
        /* The first deadline is "now", not zero: zero is in the future for
         * signed wrap-safe comparisons during the timer's upper half. */
        pause_pending=false;draining=true;drain_ready=false;drain_until=started;
        /* Finish may arrive after the final real block is armed. Extend its
         * actual final stereo sample into a short fade, without touching the
         * DMA-owned ring or consuming/discarding any additional media. */
        if(finishing&&head==tail&&last_sample)ramp_to_silence(last_sample);
    }
    if(status.playing||click_playing)(void)arm(true);
    elapsed=ln_time_us()-started;if(elapsed>status.max_irq_us)status.max_irq_us=elapsed;
}
void ln_audio_service(uint32_t now) {
    uint32_t flags=ln_s5l_irq_save();
    /* Completion may arm a new block after the executive sampled now. */
    if(active && (int32_t)(now-armed)>250000)fault(6);
    bool needs_mute=codec_ready && !muted && (draining || (!status.playing&&!click_playing) || status.fault);
    ln_s5l_irq_restore(flags);
    if(needs_mute){
        muted=ln_codec_digital_mute(ln_s5l_board_codec_write,true);
        if(!muted)fault(4);
        else mute_until=ln_time_us()+((BLOCK*frame_q12+4095u)>>12);
    }
    /* Finish clocking digital silence before stopping DMA. Leave the analog
     * gain/mute latched across Pause and warm track changes. */
    if(!status.fault && draining && drain_ready && (int32_t)(now-drain_until)>=0 &&
       (int32_t)(now-mute_until)>=0) {
        flags=ln_s5l_irq_save();
        draining=click_playing=false;status.playing=0;status.paused=1;
        if(!ln_s5l_pcm_stop())fault(3);else{active=false;inflight=0;}
        ln_s5l_irq_restore(flags);
    }
    if(status.fault && codec_ready && amplifier_open){
        if(ln_codec_mute(ln_s5l_board_codec_write,true)){amplifier_open=false;muted=true;}
    }
    if(status.fault && active){(void)ln_s5l_pcm_stop();active=false;}
    if(status.initialized&&!status.fault&&!status.playing&&!click_playing&&!active&&!draining&&click.left)
        (void)start_output(false);
    if(click_owned&&(status.fault||(!active&&!draining&&!click.left&&(int32_t)(ln_time_us()-click_last)>=500000)))(void)ln_audio_stop();
}
uint32_t ln_audio_deadline(uint32_t now){
    if(status.fault)return UINT32_MAX;
    if(draining||click.left)return now+1000u;
    if(click_owned)return (int32_t)(now-(click_last+500000u))>=0?now:click_last+500000u;
    return UINT32_MAX;
}
void ln_audio_snapshot(struct ln_audio_status *out) {
    if(!out)return;
    uint32_t flags=ln_s5l_irq_save();*out=status;out->queued=head-tail;out->free_frames=ln_audio_capacity();
    if(active&&real){
        uint32_t dma[5];ln_s5l_pcm_status(dma);
        uint32_t remaining=(dma[2]+1u)/2u;
        if(remaining<=inflight)out->inflight_consumed=inflight-remaining;
    }
    ln_s5l_irq_restore(flags);
}
#if !defined(LINNET_PROFILE_RELEASE) && defined(LINNET_IMAGE_OS)
void ln_s5l_audio_diagnostics(uint32_t out[16]) {
    uint8_t registers[32]={0};
    for(unsigned n=0;n<16u;++n)out[n]=0;
    out[0]=0x43445541u; /* AUDC: read-only codec registers 01h..20h. */
    /* MAP bit 7 enables CS42L55 address auto-increment. One transaction has
     * the existing I2C driver's 20 ms deadline; no IRQ masking or writes. */
    if(codec_ready && ln_s5l_i2c_read(0,0x4a,0x81,registers,sizeof(registers))) {
        out[1]=1;
        for(unsigned n=0;n<32u;++n)out[2u+n/4u]|=(uint32_t)registers[n]<<((n%4u)*8u);
    }
    if(!(LN_CLOCK_GATE1&(1u<<7))){
        out[10]=LN_REG(0x3ca00000u);out[11]=LN_REG(0x3ca00004u);
        out[12]=LN_REG(0x3ca00008u);out[13]=LN_REG(0x3ca00040u);
    }
    out[14]=LN_REG(0x3c50000cu);out[15]=codec_ready;
}
#endif
