#include "linnet_target.h"
#if defined(LINNET_IMAGE_OS)
#include "hal/api/power_policy.h"
#include "hal/api/core.h"
#include "hal/api/audio.h"
#include "hal/api/display.h"
#include "hal/api/input.h"
#include "hal/api/platform.h"
#include "hal/s5l8702/registers.h"
#include "hal/s5l8702/i2c.h"
#include "hal/s5l8702/clock_resources.h"

/* Original foreground policy; register facts and limits: docs/classic-power.md.
 * CPU-only division preserves the 54/27 MHz AHB/APB buses and independent audio
 * and timer clocks. Residency is elapsed time, including WFI, not energy. */
static uint64_t residency[3];
static uint32_t sampled_at,last_hz,started,work_hz,fast_until;
static uint32_t transitions,failures,heavy_fallbacks,underrun_fallbacks,reason;
static uint32_t max_work[2],loops[2],audio_generation,audio_underruns,queued,rate;
static bool initialized,disabled,playback_disabled;
static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static uint32_t account(void){
    struct ln_core_status core;ln_core_snapshot(&core);uint32_t now=ln_time_us();
    if(initialized){
        unsigned slot=last_hz==54000000u?0u:last_hz==216000000u?2u:1u;
        residency[slot]+=(uint32_t)(now-sampled_at);
        if(core.clock_hz!=last_hz)++transitions;
    }else {initialized=true;fast_until=now+250000u;}
    sampled_at=now;last_hz=core.clock_hz;return core.clock_hz;
}
static bool set_clock(uint32_t hz){
    if(last_hz==hz)return true;
    bool ok=ln_core_clock_set(hz);(void)account();
    if(ok)return true;
    ++failures;disabled=true;
    /* Readback or transition failure permanently disables this optimization.
     * A failed restoration stops the executive before further storage work. */
    ok=ln_core_clock_set(108000000u);(void)account();return ok;
}
bool ln_power_policy_fast(bool interactive){
    uint32_t hz=account();if(interactive)fast_until=ln_time_us()+250000u;
    return hz!=54000000u||set_clock(108000000u);
}
bool ln_power_policy_begin(const struct ln_power_workload *work){
    uint32_t hz=account(),now=ln_time_us();reason=(work->flags&LN_POWER_WORK_BUSY)?1u:0u;
    queued=work->queued_frames;rate=work->rate;
    if(work->playback_generation!=audio_generation){
        audio_generation=work->playback_generation;audio_underruns=work->underruns;playback_disabled=false;
    }
    if(work->underruns!=audio_underruns){
        audio_underruns=work->underruns;playback_disabled=true;++underrun_fallbacks;
    }
    if(!due(now,fast_until))reason|=4u;
    if(disabled)reason|=16u;
    bool playing=(work->flags&LN_POWER_WORK_PLAYING)!=0;
    bool settled=(work->flags&LN_POWER_WORK_SETTLED)!=0;
    if(playing){
        /* Enter with one second queued, restore at half a second. The margin
         * exceeds a bounded decoder turn and a DMA block by a wide interval. */
        uint32_t reserve=hz==54000000u?rate/2u:rate;
        if(!rate||queued<reserve)reason|=2u;
        if(playback_disabled||(work->flags&LN_POWER_WORK_AUDIO_FAULT))reason|=16u;
        if(work->flags&LN_POWER_WORK_NEXT_PENDING)reason|=1u;
    }else if(!settled||(work->flags&LN_POWER_WORK_AUDIO_FAULT))reason|=1u;
    /* Video owns the existing PLL2 boost and its voltage restoration. */
    if(hz==216000000u)reason|=32u;
    bool ok=true;
    if(hz!=216000000u)ok=set_clock(reason?108000000u:54000000u);
    started=ln_time_us();work_hz=last_hz;return ok;
}
void ln_power_policy_end(void){
    uint32_t hz=account(),now=ln_time_us(),elapsed=now-started;
    /* A frame or video start can change clocks inside this turn. Do not
     * mistake that mixed-rate interval for a slow decoder at 54 MHz. */
    if(work_hz==hz&&work_hz!=216000000u){
        unsigned slot=work_hz==54000000u?0u:1u;++loops[slot];
        if(elapsed>max_work[slot])max_work[slot]=elapsed;
        if(!slot&&elapsed>12000u){fast_until=now+2000000u;++heavy_fallbacks;}
    }
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    if(!audio.playing&&!ln_display_busy())(void)ln_s5l_dma_idle_gate();
}
void ln_power_policy_diagnostics(uint32_t words[32]){
    uint32_t hz=account();for(unsigned n=0;n<32u;n++)words[n]=0;
    words[0]=0x43525750u;words[1]=1;words[2]=hz;words[3]=reason;
    words[4]=transitions;words[5]=failures;
    for(unsigned n=0;n<3u;n++){words[6+n*2u]=(uint32_t)residency[n];words[7+n*2u]=(uint32_t)(residency[n]>>32);}
    words[12]=max_work[0];words[13]=max_work[1];words[14]=loops[0];words[15]=loops[1];
    words[16]=heavy_fallbacks;words[17]=underrun_fallbacks;words[18]=queued;words[19]=rate;
    words[20]=LN_REG(0x3c500000u);words[21]=LN_REG(0x3c500004u);words[22]=LN_REG(0x3c500020u);
    words[23]=LN_REG(0x3c500028u);words[24]=LN_REG(0x3c500044u);
    words[25]=LN_CLOCK_GATE0;words[26]=LN_CLOCK_GATE1;
    if(!(words[26]&(1u<<7))){words[27]=LN_REG(0x3ca00000u);words[28]=LN_REG(0x3ca00004u);}
    uint8_t voltage;words[29]=ln_s5l_i2c_read(0,0x73,0x1e,&voltage,1)?voltage:UINT32_MAX;
    words[30]=ln_input_deadline(ln_time_us());words[31]=(disabled?1u:0u)|(playback_disabled?2u:0u)|
        (ln_s5l_clock_resource_flags()<<8);
}
#endif
