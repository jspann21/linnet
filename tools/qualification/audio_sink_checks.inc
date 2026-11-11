static uint32_t audit_open_without_zero,audit_silent_gap,audit_cut_before_ramp;
static void audit_codec_write_event(uint8_t reg,uint16_t value){
#ifdef AUDIT_PP
    bool video=audit_codec[0x35]!=0;
    bool digital=reg==(video?10u:5u),was_muted=!!(audit_codec[reg]&(video?64u:8u));
    bool becomes_muted=!!(value&(video?64u:8u));
    bool opening=video?(reg==52u&&(audit_codec[reg]&64u)&&!(value&64u)):
        (reg==2u&&(audit_codec[reg]&127u)<48u&&(value&127u)>=48u);
    bool current_real=real_block;
#else
    bool digital=reg==15u,was_muted=!!(audit_codec[reg]&3u),becomes_muted=!!(value&3u);
    bool opening=reg==26u&&(audit_codec[reg]&128u)&&!(value&128u);
    bool current_real=real;
#endif
#ifndef AUDIT_PP
    if(reg>=0x1a&&reg<=0x1d)audit_analog_at=audit_now;
    if(reg==2u&&(value&1u)&&audit_codec_live){
        unsigned rate=0;
        switch(audit_codec[5]){
        case 0x1d:rate=8000;break;case 0x1b:rate=11025;break;case 0x19:rate=12000;break;
        case 0x15:rate=16000;break;case 0x13:rate=22050;break;case 0x11:rate=24000;break;
        case 0x0d:rate=32000;break;case 0x0b:rate=44100;break;case 9:rate=48000;break;
        }
        uint64_t denominator=(uint64_t)rate*audit_mclk;
        unsigned wait=denominator?(unsigned)(((uint64_t)1024*1000000u*12000000u+denominator-1u)/denominator)+1000u:193000u;
        if(audit_now-audit_analog_at<wait||audit_codec[0x1a]!=0xc0||audit_codec[0x1b]!=0xc0||
           audit_codec[0x1c]!=0xc0||audit_codec[0x1d]!=0xc0)audit_bad_powerdown++;
    }
    if(reg==2u)audit_codec_live=!(value&1u);
#endif
    if(digital&&becomes_muted)audit_mute_at=audit_now;
    if(opening&&(!active||current_real))audit_open_without_zero++;
    if(opening)audit_analog_changes++;
    if(digital&&was_muted&&!becomes_muted){
        audit_release_at=audit_now;
        if(active&&(current_real||click_block))audit_release_while_nonzero++;
    }
}
void audit_complete(void){
    if(!active)return;
#ifdef AUDIT_PP
    bool was_real=real_block,was_pause=pause_pending;
    uint32_t ramp=10000u;
    audit_now=armed_us+expected_us;*audit_register(0x6000b004u)=0x40000000u;
#else
    bool was_real=real,was_pause=pause_pending;
    uint32_t ramp=(BLOCK*frame_q12+4095u)>>12;
    audit_now=armed+expected;audit_dma_active=false;
#endif
    audit_in_irq=1;ln_audio_irq();audit_in_irq=0;
    if(draining&&!active)audit_silent_gap++;
    if(was_real&&was_pause&&draining&&audit_now-audit_mute_at<ramp)audit_cut_before_ramp++;
}
static void audit_tick(uint32_t now){
    uint32_t target=now;
#ifdef AUDIT_PP
    if(active&&(int32_t)(target-(armed_us+expected_us))>=0)audit_complete();
#else
    if(active&&(int32_t)(target-(armed+expected))>=0)audit_complete();
#endif
    audit_now=target;
}
#include <assert.h>
#include <stdio.h>
static void complete_service(void){audit_complete();ln_audio_service(audit_now);}
static void finish_drain(void){
    for(unsigned n=0;n<64u&&(active||draining);n++)complete_service();
    assert(!active&&!draining);
}
static void test_click(unsigned rate){
    assert(ln_audio_stop());assert(ln_audio_init(rate));
    int16_t pcm[512];for(unsigned n=0;n<512;n++)pcm[n]=(n&1)?-1000:1000;
    assert(ln_audio_submit(pcm,256)==256);
    struct ln_audio_status before;ln_audio_snapshot(&before);
    uint32_t releases=audit_release_while_nonzero;
    ln_audio_click(0,4,-300);
    struct ln_pcm_click expected={0};ln_pcm_click_request(&expected,rate,0,4);
    unsigned emitted=0,total=expected.total,blocks=0;
    /* The real sink must emit the complete waveform without a mute/drain
     * gap between its DMA slices. Counters alone cannot establish this. */
    while(emitted<total){
        assert(active&&click_block&&!draining);
#ifdef AUDIT_PP
        assert(!digital_muted);
        assert(armed_us-audit_release_at>=10000u);
#else
        assert(!muted);
        assert(armed-audit_release_at>=((1024u*frame_q12+4095u)>>12)+1000u);
#endif
        assert(inflight<=total-emitted);
        for(unsigned n=0;n<inflight;n++){
            int16_t value=(int16_t)ln_pcm_click_sample(&expected);
            assert(audit_samples[n]==((uint16_t)value|((uint32_t)(uint16_t)value<<16)));
        }
        emitted+=inflight;blocks++;complete_service();
    }
    assert(!expected.left&&!click.left);
    assert(audit_release_while_nonzero==releases);
    finish_drain();struct ln_audio_status after;ln_audio_snapshot(&after);
    assert(after.consumed==before.consumed&&after.submitted==before.submitted&&after.queued==before.queued);
    assert(!after.fault&&!after.underruns);
    unsigned opens=audit_analog_changes;
    ln_audio_click(2,2,-300);finish_drain();
    assert(audit_analog_changes==opens);
    printf("click rate=%u complete_frames=%u contiguous_blocks=%u\n",rate,total,blocks);
}
static void test_live_click(unsigned rate){
    assert(ln_audio_stop()&&ln_audio_init(rate));
    int16_t pcm[8192];for(unsigned n=0;n<8192;n++)pcm[n]=(n&1)?-1000:1000;
    assert(ln_audio_submit(pcm,4096)==4096&&ln_audio_start());
    complete_service(); /* onset has completed; ring samples are unmodified */
    unsigned opens=audit_analog_changes;
    ln_audio_click(0,4,-300);complete_service();
    struct ln_pcm_click expected={0};ln_pcm_click_request(&expected,rate,0,4);
    unsigned emitted=0,total=expected.total;
    while(emitted<total){
        assert(active&&!draining&&inflight<=64u&&inflight<=total-emitted);
        for(unsigned n=0;n<inflight;n++){
            int32_t click_sample=ln_pcm_click_sample(&expected);
            assert(audit_samples[n]==(ln_pcm_click_add(1000,click_sample)|((uint32_t)ln_pcm_click_add(-1000,click_sample)<<16)));
        }
        emitted+=inflight;complete_service();
    }
    assert(audit_analog_changes==opens);
    ln_audio_pause();finish_drain();
    struct ln_audio_status final;ln_audio_snapshot(&final);
    assert(!final.fault&&!final.underruns&&final.submitted==4096&&final.consumed+final.queued==4096);
    for(unsigned n=0;n<4096;n++)assert(ring[n]==(1000u|((uint32_t)(uint16_t)-1000<<16)));
    printf("live click rate=%u mixed_frames=%u unchanged_media_ring\n",rate,emitted);
}
int main(void){
    const unsigned rates[]={44100,48000,8000,11025,12000,16000,22050,24000,32000};
    for(unsigned n=0;n<sizeof(rates)/sizeof(rates[0]);n++)test_click(rates[n]);
    test_live_click(44100);test_live_click(48000);
    for(unsigned r=0;r<2;r++){
        unsigned rate=r?48000:44100;
        assert(ln_audio_stop()&&ln_audio_init(rate));
        int16_t pcm[8192];for(unsigned n=0;n<8192;n++)pcm[n]=(n&1)?-1000:1000;
        for(unsigned n=0;n<16;n++)assert(ln_audio_submit(pcm,4096)==4096);
        assert(ln_audio_start());
        unsigned opens=audit_analog_changes;
        for(unsigned n=0;n<8;n++){
            ln_audio_pause();finish_drain();
            assert(ln_audio_start());
        }
        assert(audit_analog_changes==opens);
        assert(ln_audio_halt());assert(ln_audio_init(rate));
        assert(ln_audio_submit(pcm,4096)==4096&&ln_audio_start());
        assert(audit_analog_changes==opens);
        ln_audio_finish();finish_drain();
        struct ln_audio_status final;ln_audio_snapshot(&final);
        assert(!final.fault&&!final.underruns&&final.consumed==4096);
    }
    assert(ln_audio_stop());
    ln_audio_click(0,4,-300);finish_drain();
    audit_now+=500000u;ln_audio_service(audit_now);assert(!owned);
#ifndef AUDIT_PP
    for(unsigned failure=0;failure<2;failure++){
        assert(ln_audio_init(44100));ln_audio_click(0,4,-300);finish_drain();
        if(failure==0)audit_reject=0x1b;else audit_delay_fail=true;
        assert(!ln_audio_stop()&&owned&&audit_codec_live);
        audit_reject=-1;audit_delay_fail=false;assert(ln_audio_stop()&&!owned);
    }
#ifndef AUDIT_BASELINE
    assert(ln_audio_init(44100));ln_audio_click(0,4,-300);finish_drain();
    audit_clockoff_fail=true;assert(!ln_audio_stop()&&owned&&powerdown_ready&&audit_reset);
    unsigned writes=audit_codec_writes;
    audit_clockoff_fail=false;assert(ln_audio_stop()&&!owned&&!powerdown_ready);
    assert(audit_codec_writes==writes); /* RESET-low retry does not touch I2C */
    assert(ln_audio_init(44100)&&!powerdown_ready&&!audit_reset);
    ln_audio_click(0,4,-300);finish_drain();
    audit_reject=0x1b;assert(!ln_audio_stop()&&owned&&!codec_ready);
    audit_reject=-1;assert(ln_audio_init(44100)&&codec_ready&&!powerdown_ready);
    ln_audio_click(0,4,-300);finish_drain();assert(ln_audio_stop());
#endif
    for(unsigned rate=32000;rate<=48000;rate+=16000){
        assert(ln_audio_init(8000));ln_audio_click(0,4,-300);finish_drain();
        unsigned waited=audit_delay_total;
        audit_reject=5;assert(!ln_audio_init(rate));audit_reject=-1;
        assert(audit_delay_total-waited==193000u&&!owned&&!audit_bad_powerdown);
    }
#endif
    assert(ln_audio_init(44100));ln_audio_click(0,4,-300);finish_drain();
    audit_stop_fail=true;assert(!ln_audio_halt());
#ifdef AUDIT_PP
    assert(output_muted&&digital_muted);
#else
    assert(!amplifier_open&&muted);
#endif
    audit_stop_fail=false;assert(ln_audio_stop());
    ln_audio_click(0,4,-300);fault(99);assert(ln_audio_halt());
#ifdef AUDIT_PP
    assert(digital_muted&&output_muted);
#else
    assert(muted&&!amplifier_open);
#endif
    assert(audit_i2c_in_irq==0&&audit_open_without_zero==0&&audit_cut_before_ramp==0);
    assert(!audit_bad_powerdown);
    puts("PASS complete unmuted contiguous feedback; paused music preserved; warm analog gain retained; EOF and stop");
    return 0;
}
