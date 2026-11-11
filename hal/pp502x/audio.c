/* Linnet PP5020 playback sink. Register protocol cross-checked against local
 * Rockbox firmware/target/arm/pp/{pcm-pp,i2s-pp}.c and drivers/audio/wm8975.c,
 * wmcodec-pp.c and export/pp5020.h. Independent implementation.
 * DMA owns only channel 0 and ordinary IRQ 26; recovery retains all FIQ state. */
#include "hal/api/audio.h"
#include "hal/api/click_pcm.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "hal/api/power.h"
#include "hal/codecs/codec.h"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define IRQ (1u<<26)
#define CMD REG(0x6000b000u)
#define STATUS REG(0x6000b004u)
#define REQUEST REG(0x6000a008u)
#define IIS REG(0x70002800u)
#define FIFO REG(0x7000280cu)
#define CHUNK 1024u
#define MASK (LINNET_AUDIO_RING_FRAMES-1u)
/* Cleared cursors publish only submitted PCM; DMA never reads unused slots. */
static uint32_t ring[LINNET_AUDIO_RING_FRAMES] __attribute__((section(".media_workspace"),aligned(32)));
static uint32_t silence[CHUNK] __attribute__((aligned(32)));
#if defined(LINNET_TARGET_VIDEO5G_M25)
/* Fade a resume without modifying queued media or the DMA-owned ring. */
static uint32_t onset[CHUNK] __attribute__((aligned(32)));
static volatile uint32_t fade_remaining;
#endif
static uint32_t click_pcm[CHUNK] __attribute__((aligned(32)));
static volatile struct ln_pcm_click click;
static bool click_owned;
static volatile bool click_block,click_playing;
static uint32_t click_last;
static volatile struct ln_audio_status state;
static volatile uint32_t head,tail,inflight;
static volatile uint32_t inflight_last;
static volatile bool real_block,active,pause_pending,owned,starved,draining,warming,start_armed,drain_ready,tail_ramp;
static volatile bool finishing;
static bool codec_ready,halted,dma_quiet,lineout_enabled;
static bool digital_muted,output_muted;
static volatile uint32_t mute_until;
static volatile uint32_t drain_until;
static uint32_t frame_us_q12;
static volatile uint32_t armed_us, expected_us, previous_completion_us;
static volatile bool completion_seen;
static volatile uint32_t *native(uint32_t *p)
{ return (volatile uint32_t *)((uintptr_t)p|0x10000000u); }
static void ramp_to_silence(uint32_t sample)
{
    int32_t left=(int16_t)sample,right=(int16_t)(sample>>16);
    for(uint32_t i=0;i<64u;i++){
        int32_t gain=(int32_t)(64u-i);
        native(silence)[i]=(uint16_t)(left*gain/64)|((uint32_t)(uint16_t)(right*gain/64)<<16);
    }
    tail_ramp=true;
}
static bool codec(uint8_t reg,uint16_t value)
{ return ln_power_i2c_write(0x1au,(uint8_t)((reg<<1)|(value>>8)),(uint8_t)value); }
static void mask_irq(void) { REG(0x60004028u)=IRQ; }
static void fault(uint32_t code)
{
    if(!state.fault)state.fault=code;
    state.playing=0; mask_irq();
    CMD&=~0xc0000000u;
}
static bool quiesce(void)
{
    dma_quiet=false;
    mask_irq(); REQUEST&=~4u; CMD&=~0xc0000000u;
    uint32_t started=ln_time_us();
    for(uint32_t budget=0;budget<1000000u;++budget){
        if(!(STATUS&0xc0000000u)){active=false;inflight=0;dma_quiet=true;return true;}
        if(ln_time_us()-started>=100000u)break;
    }
    fault(1);return false;
}
static void arm(bool drain)
{
    uint32_t queued=head-tail,count=state.playing?queued:0;
    if(queued>LINNET_AUDIO_RING_FRAMES){fault(2);return;}
    if(count>CHUNK)count=CHUNK;
    uint32_t offset=tail&MASK;
    if(count>LINNET_AUDIO_RING_FRAMES-offset)count=LINNET_AUDIO_RING_FRAMES-offset;
    volatile uint32_t *source;
    click_block=!state.playing&&!drain;
    if(drain){source=native(silence);count=CHUNK;real_block=false;}
    else if(count){source=native(ring)+offset;real_block=true;starved=false;}
    else {source=native(silence);count=click_block?click.left:CHUNK;real_block=false;
        if(!click_block){if(!starved)++state.underruns;starved=true;}}
    if(!count)return;
    /* A short feedback tone needs no copy of the following unmodified music
     * in IRQ context. Resume directly from the ring on the next completion. */
    if(!drain&&click.left){
        if(count>click.left)count=click.left;
        /* Sine/envelope mixing must fit the16-frame FIFO runway too. Real
         * Video traces exceeded it when an entire176-frame click ran here. */
        if(count>64u)count=64u;
    }
#if defined(LINNET_TARGET_VIDEO5G_M25)
    if(real_block&&fade_remaining){
        /* Arm only the onset. Copying the remainder of a 1024-frame block
         * here can exhaust the 16-frame FIFO before DMA is rearmed. */
        if(count>fade_remaining)count=fade_remaining;
        for(uint32_t i=0;i<count;i++){
            uint32_t sample=source[i];
            int32_t gain=(int32_t)(64u-fade_remaining+i);
            int32_t left=(int16_t)sample,right=(int16_t)(sample>>16);
            sample=(uint16_t)(left*gain/64)|((uint32_t)(uint16_t)(right*gain/64)<<16);
            native(onset)[i]=sample;
        }
        fade_remaining-=count;source=native(onset);
    }
#endif
    /* Never mix clicks into the zeros used for analog mute settling. */
    if(!drain&&ln_pcm_click_mix(&click,(uint32_t *)native(click_pcm),(const uint32_t *)source,count))source=native(click_pcm);
    inflight_last=real_block?source[count-1u]:0;
    inflight=count;active=true;dma_quiet=false;
    if(real_block||click_block)start_armed=true;
    state.last_irq_us=ln_time_us();
    armed_us=state.last_irq_us;
    /* Rates are >=8kHz and count<=1024: both products fit uint32_t.
     * Q12 truncation contributes less than 0.25us per full chunk. */
    expected_us=(count*frame_us_q12+4095u)>>12;
    __asm__ volatile("":::"memory");
    REG(0x6000b010u)=(uint32_t)(uintptr_t)source;
    CMD=0xcd020000u|(count*4u-4u);
}
bool ln_audio_volume(int32_t value)
{
    if(value < -730 || value > 0)return false;
    if(!ln_codec_volume(codec,value))return false;
    state.volume_db_tenths=value;return true;
}
bool ln_audio_lineout(bool enabled)
{
    if(codec_ready&&!ln_codec_lineout(codec,enabled))return false;
    lineout_enabled=enabled;return true;
}
bool ln_audio_halt(void)
{
    ln_audio_click_stop();
    if(!owned)return true;
    if(halted)return true;
    bool settled=true;
    if((state.playing||click_playing)&&!state.fault){
        ln_audio_pause();
        uint32_t began=ln_time_us(),timeout=4u*((CHUNK*frame_us_q12+4095u)>>12)+40000u;
        while((state.playing||click_playing)&&!state.fault&&ln_time_us()-began<timeout){
            uint32_t now=ln_time_us();ln_audio_service(now);
            if(state.playing||click_playing)ln_core_idle_until(now+1000u);
        }
        settled=!state.playing&&!click_playing&&!state.fault;
    }
#if defined(LINNET_TARGET_VIDEO5G_M25)
    /* A warm stream replacement needs digital silence, not a headphone PGA
     * mute/unmute cycle. Keep its analog bias and gain latched until Stop. */
    bool muted=digital_muted||ln_codec_digital_mute(codec,true);digital_muted=muted;
    if(state.fault||!settled){
        muted=ln_codec_mute(codec,true);output_muted=digital_muted=muted;
    }
#else
    bool muted=ln_codec_mute(codec,true);output_muted=digital_muted=muted;
#endif
    uint32_t flags=ln_core_irq_save();
    bool idle=quiesce();
    state.playing=0;state.paused=0;pause_pending=draining=warming=click_playing=false;
    ln_core_irq_restore(flags);
    if(!idle){
        /* quiesce itself can be the operation that first reports a fault. */
        output_muted=digital_muted=ln_codec_mute(codec,true);
        return false; /* Never reuse a DMA source until BUSY clears. */
    }
    IIS&=~(1u<<29);FIFO|=1u<<8;
    state.initialized=0;head=tail=0;finishing=false;completion_seen=false;
    halted=muted&&settled;
    if(!halted){codec_ready=false;state.fault=3;}
    return halted;
}
bool ln_audio_stop(void)
{
    click_owned=false;
    if(!owned)return true;
    bool muted=ln_audio_halt();
    if(!dma_quiet)return false;
    bool powered_off=ln_codec_off(codec);
    REG(0x60006004u)|=0x800u;
    REG(0x6000600cu)&=~0x800u;
    state.initialized=0;owned=false;codec_ready=false;halted=false;head=tail=0;
    return muted&&powered_off;
}
bool ln_audio_init(uint32_t rate)
{
    click_owned=false;
    if(!ln_codec_rate_supported(rate))return false;
    bool warm=owned&&codec_ready&&!state.fault;
    uint32_t old_rate=state.rate;
    int32_t volume=state.volume_db_tenths;
    if(warm){if(!ln_audio_halt())return false;}
    else if(!ln_audio_stop())return false;
    owned=true;
    if(!quiesce())return false;
    state=(struct ln_audio_status){0};head=tail=0;pause_pending=draining=warming=drain_ready=false;starved=false;finishing=false;
    frame_us_q12=4096000000u/rate;completion_seen=false;
    if(warm){
        /* Analog paths and headphone gain remain latched across tracks. */
        state.volume_db_tenths=volume;
        if(old_rate!=rate&&!ln_codec_rate(codec,rate))goto failed;
        goto stream_ready;
    }
    REG(0x70000020u)&=~0x300u;REG(0x70000010u)&=~0x03000000u;
    REG(0x60006004u)|=0x800u;
    __asm__ volatile("nop\n\tnop\n\tnop":::"memory");
    REG(0x60006004u)&=~0x800u;REG(0x6000600cu)|=0x802u;
    REG(0x70000018u)&=~0xcu;
    IIS|=1u<<31;IIS&=~(1u<<31);
    IIS=(IIS&~((3u<<28)|(1u<<25)|3u|(3u<<10)|(3u<<8)|(7u<<4)))|(7u<<4);
    FIFO|=0x1131u;
    REG(0x6000d920u)=0x4040u;REG(0x6000d820u)=0x0404u;
    if(!ln_codec_init(codec,rate)||!ln_codec_lineout(codec,lineout_enabled)||!ln_audio_volume(-400))goto failed;
    codec_ready=true;
stream_ready:
    digital_muted=true;if(!warm)output_muted=true;mute_until=0;drain_until=0;
#if defined(LINNET_TARGET_VIDEO5G_M25)
    fade_remaining=0;
#endif
    for(uint32_t i=0;i<CHUNK;++i)native(silence)[i]=0;
    tail_ramp=false;
    REG(0x6000a000u)|=1u<<31;
    REG(0x6000b018u)=0x70002840u;REG(0x6000b014u)=1u<<26;
    REG(0x6000b01cu)=0x20010000u;
    REG(0x6000402cu)&=~IRQ;
    state.rate=rate;state.initialized=1;halted=false;return true;
failed:
    (void)ln_audio_stop();state.fault=3;return false;
}
void ln_audio_click_stop(void){
    uint32_t flags=ln_core_irq_save();ln_pcm_click_clear(&click);ln_core_irq_restore(flags);
}
void ln_audio_click(uint8_t tone,uint8_t volume,int32_t output_db_tenths){
    if(tone>3u||tone==1u||!volume||volume>4u)return;
    if(!state.initialized){
        if(!ln_audio_init(44100u))return;
        click_owned=true;
        if(!ln_audio_volume(output_db_tenths)){(void)ln_audio_stop();return;}
    }
    if(state.fault)return;
    uint32_t flags=ln_core_irq_save();
    ln_pcm_click_request(&click,state.rate,tone,volume);click_last=ln_time_us();
    ln_core_irq_restore(flags);
    ln_audio_service(ln_time_us());
}
uint32_t ln_audio_capacity(void)
{
    uint32_t queued=head-tail;
    return state.initialized&&!state.fault&&queued<=LINNET_AUDIO_RING_FRAMES?
        LINNET_AUDIO_RING_FRAMES-queued:0;
}
uint32_t ln_audio_submit(const int16_t *pcm,uint32_t frames)
{
    if(!pcm||finishing)return 0;
    uint32_t free=ln_audio_capacity();if(frames>free)frames=free;
    uint32_t cursor=head,first=frames,offset=cursor&MASK;
    if(first>LINNET_AUDIO_RING_FRAMES-offset)first=LINNET_AUDIO_RING_FRAMES-offset;
    volatile uint32_t *out=native(ring)+offset;
    const int16_t *in=pcm,*end=pcm+first*2u;
    while(in!=end){uint32_t left=(uint16_t)*in++;*out++=left|((uint32_t)(uint16_t)*in++<<16);}
    out=native(ring);end=pcm+frames*2u;
    while(in!=end){uint32_t left=(uint16_t)*in++;*out++=left|((uint32_t)(uint16_t)*in++<<16);}
    __asm__ volatile("":::"memory");
    head=cursor+frames;state.submitted+=frames;return frames;
}
/* Replace only queued PCM beyond the DMA-owned block. The short copy is atomic
 * against completion; neither counters nor playback position are changed. */
bool ln_audio_rewrite(uint32_t frame,const int16_t *pcm,uint32_t frames){
    uint32_t flags=ln_core_irq_save(),offset=frame-tail,queued=head-tail;
    bool allowed=state.initialized&&!state.fault&&offset<=queued&&frames<=queued-offset&&
        (!active||!real_block||offset>=inflight);
    if(allowed)for(uint32_t i=0;i<frames;i++)native(ring)[(frame+i)&MASK]=
        (uint16_t)pcm[2*i]|((uint32_t)(uint16_t)pcm[2*i+1]<<16);
    ln_core_irq_restore(flags);return allowed;
}
static bool startup_wait(uint32_t began,uint32_t timeout)
{
    uint32_t now=ln_time_us();
    if(state.fault)return false;
    if(now-began>=timeout){fault(5);return false;}
    ln_core_idle_until(now+1000u);return true;
}
/* Click-only output shares the same startup and mute sequence as music,
 * while state.playing and the ring cursors continue to describe media only. */
static bool start_output(bool media)
{
    if(!state.initialized||state.fault||(media?head==tail:!click.left))return false;
    if(state.playing&&!pause_pending&&!draining&&!digital_muted)return true;
    uint32_t flags=ln_core_irq_save();
    bool continuing=active&&real_block&&!output_muted;
    pause_pending=draining=false;
    click_playing=!media;
    if(media){click_owned=false;state.paused=0;state.playing=1;}
    if(continuing){
        ln_core_irq_restore(flags);
        if(!ln_codec_digital_mute(codec,false)){fault(3);goto failed;}
        digital_muted=false;return true;
    }
    warming=true;start_armed=false;
    uint32_t zeros=state.silence_frames;
    REQUEST|=4u;IIS|=1u<<29;
    if(!active){completion_seen=false;arm(true);}
    REG(0x60004024u)=IRQ;
    ln_core_irq_restore(flags);
    uint32_t began=ln_time_us(),timeout=3u*((CHUNK*frame_us_q12+4095u)>>12)+30000u;
    while(state.silence_frames==zeros)if(!startup_wait(began,timeout))goto failed;
    /* Output gain is restored immediately under digital mute after a zero
     * block has flushed the FIFO, rather than waiting for a zero crossing. */
    if(output_muted){
        output_muted=false;
        if(!ln_codec_analog_mute(codec,false)){fault(3);goto failed;}
    }
    uint32_t ready=ln_time_us()+1000u;
    while((int32_t)(ln_time_us()-ready)<0)if(!startup_wait(began,timeout))goto failed;
#if defined(LINNET_TARGET_VIDEO5G_M25)
    /* WM8758 documents a soft ramp but no maximum ramp time. Release it on
     * zero data, then make the PCM onset itself continuous at the next IRQ. */
    digital_muted=false;
    if(!ln_codec_digital_mute(codec,false)){fault(3);goto failed;}
    ready=ln_time_us()+10000u;
    while((int32_t)(ln_time_us()-ready)<0)if(!startup_wait(began,timeout))goto failed;
    fade_remaining=64u;
#endif
    warming=false;
    while(!start_armed)if(!startup_wait(began,timeout))goto failed;
#if !defined(LINNET_TARGET_VIDEO5G_M25)
    if(!draining){
        digital_muted=false;
        if(!ln_codec_digital_mute(codec,false)){fault(3);goto failed;}
    }
#endif
    return true;
failed:
    warming=false;ln_audio_service(ln_time_us());return false;
}
bool ln_audio_start(void){return start_output(true);}
void ln_audio_pause(void)
{
    if((!state.playing&&!click_playing)||pause_pending||draining)return;
    /* The serial clock must keep running for the DAC soft ramp. The next
     * completion drains silence without consuming any further queued PCM. */
    if(!ln_codec_digital_mute(codec,true)){fault(3);return;}
    digital_muted=true;mute_until=ln_time_us()+10000u;
    uint32_t flags=ln_core_irq_save();
    if(state.playing||click_playing){if(active)pause_pending=true;else{draining=true;drain_ready=false;drain_until=ln_time_us();arm(true);}}
    ln_core_irq_restore(flags);
}
void ln_audio_finish(void){uint32_t flags=ln_core_irq_save();finishing=true;ln_core_irq_restore(flags);}
void ln_audio_irq(void)
{
    uint32_t started=ln_time_us(),fifo=FIFO,status=STATUS;
    if(!state.initialized||!active||(status&0xc0000000u)!=0x40000000u){fault(4);return;}
    if(((fifo>>16)&0x3fu)>=16u)++state.fifo_empty_irqs;
    if(completion_seen){
        uint32_t interval=started-previous_completion_us;
        if(interval>state.max_completion_interval_us)state.max_completion_interval_us=interval;
    }
    previous_completion_us=started;completion_seen=true;
    uint32_t duration=started-armed_us;
    if(duration>expected_us&&duration-expected_us>state.max_completion_late_us)
        state.max_completion_late_us=duration-expected_us;
    ++state.interrupts;
    uint32_t last_sample=0;
    if(real_block){tail+=inflight;state.consumed+=inflight;last_sample=inflight_last;}
    else if(!click_block)state.silence_frames+=inflight;
    if(tail_ramp){for(uint32_t i=0;i<64u;i++)native(silence)[i]=0;tail_ramp=false;}
    active=false;inflight=0;state.last_irq_us=started;
    if(draining)drain_ready=true;
    else if(!warming&&((pause_pending&&(int32_t)(started-mute_until)>=0)||(state.playing&&finishing&&tail==head)||(click_block&&!state.playing&&!click.left))){
        pause_pending=false;draining=true;drain_ready=false;drain_until=started;
#if defined(LINNET_TARGET_VIDEO5G_M25)
        /* Even if soft mute has not settled, the last PCM sample reaches
         * zero continuously. Natural EOF and pause share this short tail. */
        if(last_sample)ramp_to_silence(last_sample);
#else
        if(finishing&&tail==head&&last_sample)ramp_to_silence(last_sample);
#endif
    }
    if(state.playing||click_playing)arm(draining||warming);
    uint32_t elapsed=ln_time_us()-started;if(elapsed>state.max_irq_us)state.max_irq_us=elapsed;
}
void ln_audio_service(uint32_t now)
{
    uint32_t flags=ln_core_irq_save();
    if(active&&(int32_t)(now-state.last_irq_us)>=250000)fault(5);
    bool needs_digital=codec_ready&&!digital_muted&&(draining||(!state.playing&&!click_playing)||state.fault);
    ln_core_irq_restore(flags);
    /* I2C belongs to the foreground, including natural end-of-track mute. */
    if(needs_digital){
        if(!ln_codec_digital_mute(codec,true))fault(3);
        else{digital_muted=true;mute_until=ln_time_us()+10000u;}
    }
    if(!state.fault&&draining&&drain_ready&&(int32_t)(now-drain_until)>=0&&(int32_t)(now-mute_until)>=0){
#if !defined(LINNET_TARGET_VIDEO5G_M25)
        if(!output_muted){
            if(!ln_codec_analog_mute(codec,true))fault(3);
            else{output_muted=true;drain_until=ln_time_us()+1000u;}
        }else
#endif
        {
            flags=ln_core_irq_save();
            if(quiesce()){
                IIS&=~(1u<<29);FIFO|=1u<<8;
                draining=click_playing=false;state.playing=0;state.paused=1;
            }
            ln_core_irq_restore(flags);
        }
    }
    if(state.fault){
        if(codec_ready&&!output_muted)output_muted=ln_codec_mute(codec,true);
        flags=ln_core_irq_save();if(active)(void)quiesce();ln_core_irq_restore(flags);
    }
    if(state.initialized&&!state.fault&&!state.playing&&!click_playing&&!active&&!draining&&click.left)
        (void)start_output(false);
    if(click_owned&&(state.fault||(!active&&!draining&&!click.left&&(int32_t)(ln_time_us()-click_last)>=500000)))(void)ln_audio_stop();
}
uint32_t ln_audio_deadline(uint32_t now){
    if(state.fault)return UINT32_MAX;
    if(draining||click.left)return now+1000u;
    if(click_owned)return (int32_t)(now-(click_last+500000u))>=0?now:click_last+500000u;
    return UINT32_MAX;
}
void ln_audio_snapshot(struct ln_audio_status *out)
{
    if(!out)return;
    uint32_t flags=ln_core_irq_save();*out=state;
    out->queued=head-tail;out->free_frames=ln_audio_capacity();ln_core_irq_restore(flags);
}
