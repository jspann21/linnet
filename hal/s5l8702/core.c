#include "hal/api/core.h"
#include "hal/api/audio.h"
#include "hal/api/platform.h"
#include "hal/api/input.h"
#include "hal/arm926/cache.h"
#include "hal/arm926/mmu.h"
#include "irq.h"
#include "timer.h"
#include "watchdog.h"
#include "registers.h"
#include "board.h"
#include "clicker.h"
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
#include "clock_resources.h"
#endif
static struct ln_core_status state;
static volatile uint32_t wakes;
static uint32_t clock_failure;
static bool initialized;
#if !defined(LINNET_IMAGE_OS) || !LINNET_AUDIO_CLOCK_GATING
static bool pll2_owned;
#endif
static uint32_t saved_sm1;
static bool saved_sm1_valid;
#ifdef LINNET_SRAM_MONITOR
static uint32_t table[4096] __attribute__((section(".dram.mmu"),aligned(16384)));
#else
static uint32_t table[4096] __attribute__((aligned(16384)));
#endif
static void interrupt(uint32_t first,uint32_t second) {
    uint32_t started=ln_time_us();state.irq_count++;
    if(first&(1u<<7)) {
        bool handled=false;
        if(ln_s5l_deadline_pending()){
            ln_s5l_deadline_stop();wakes|=LINNET_WAKE_TIMER;handled=true;
        }
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
        if(ln_s5l_click_irq())handled=true;
#endif
        if(handled)first&=~(1u<<7);
    }
#if defined(LINNET_IMAGE_OS) && defined(LINNET_TARGET_CLASSIC6G_N25)
    if(first&(1u<<23)){
        if(ln_input_irq())wakes|=LINNET_WAKE_INPUT;
        first&=~(1u<<23);
    }
#endif
#if defined(LINNET_AUDIO) || defined(LINNET_SRAM_MONITOR)
    if(first&(1u<<16)){ln_audio_irq();first&=~(1u<<16);}
#endif
    if(first || second) {
        state.unexpected_irq++;
        for(unsigned n=0;n<32;n++) {
            if(first&(1u<<n))ln_s5l_irq_enable(n,false);
            if(second&(1u<<n))ln_s5l_irq_enable(n+32,false);
        }
    }
    uint32_t elapsed=ln_time_us()-started;
    if(elapsed>state.irq_max_us)state.irq_max_us=elapsed;
}
bool ln_core_init(void) {
    state=(struct ln_core_status){0};wakes=0;
    ln_s5l_irq_init(interrupt);ln_s5l_deadline_stop();ln_s5l_irq_enable(7,true);
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
#ifndef LINNET_SRAM_MONITOR
    ln_core_watchdog_arm();
#endif
    uint32_t started=ln_time_us(),budget=100000;
    while(ln_time_us()==started)if(!budget--)return false;
    state.timer_valid=1;state.sdram_valid=1;
    state.clock_hz=108000000u;
    uint32_t flags=ln_s5l_irq_save();ln_s5l_irq_restore(flags&~0x80u);
    initialized=true;return true;
}
uint32_t ln_core_irq_save(void){return ln_s5l_irq_save();}
void ln_core_irq_restore(uint32_t flags){ln_s5l_irq_restore(flags);}
void ln_core_irq_dispatch(void){ln_s5l_irq_dispatch();}
uint32_t ln_core_take_wakes(void) {
    uint32_t flags=ln_s5l_irq_save(),result=wakes;wakes=0;
    ln_s5l_irq_restore(flags);return result;
}
void ln_core_idle_until(uint32_t deadline) {
    if(!initialized)return;
    uint32_t flags=ln_s5l_irq_save(),start=ln_time_us();
    int32_t delay=(int32_t)(deadline-start);
    /* The OS schedules its 20 ms switch sample explicitly; wheel IRQs and
     * audio completions wake earlier. Release has no Link USB poller. */
#if defined(LINNET_PROFILE_RELEASE) && defined(LINNET_IMAGE_OS) && defined(LINNET_IDLE_POLL_MAX_US)
    if(delay>(int32_t)LINNET_IDLE_POLL_MAX_US)delay=LINNET_IDLE_POLL_MAX_US;
#else
    /* Development Link and other targets retain their 1 ms poll bound. */
    if(delay>1000)delay=1000;
#endif
    if(delay>0 && ln_s5l_deadline_arm((uint32_t)delay)) {
        ln_s5l_irq_wait();state.idle_count++;state.idle_us+=ln_time_us()-start;
    }
    ln_s5l_irq_restore(flags);
    int32_t late=(int32_t)(ln_time_us()-deadline);
    if(late>0 && (uint32_t)late>state.deadline_late_us)state.deadline_late_us=(uint32_t)late;
}
void ln_core_usb_rearm(void){wakes|=LINNET_WAKE_USB;}
void ln_core_input_rearm(void){wakes|=LINNET_WAKE_INPUT;}
void ln_core_snapshot(struct ln_core_status *out) {
    if(!out)return;
    uint32_t flags=ln_s5l_irq_save();
    state.clock_source=LN_REG(0x3c500004u);state.pll_control=LN_REG(0x3c500020u);
    state.cache_control=ln_arm926_cache_control();state.wake_sources=wakes;*out=state;
    ln_s5l_irq_restore(flags);
}
bool ln_core_clock_set(uint32_t hz) {
    uint32_t source=LN_REG(0x3c500000u)&0xffffu,divs=LN_REG(0x3c500004u);
    bool normal=source==0x1000u&&divs==0x00404101u;
    uint32_t normal_divs=0x00404101u;
    bool allowed=hz==108000000u||hz==216000000u;
#if defined(LINNET_IMAGE_OS) && LINNET_RUNTIME_POWER_POLICY
    /* CPU divisor enable (bit 30), count 0: divide PLL0's 108 MHz by 2.
     * AHB stays 54 MHz, APB 27 MHz; SDRAM, I2S and ECLK are untouched.
     * Keep this OS-only: Boot and the resident monitor retain their clocks. */
    normal=normal||(source==0x1000u&&divs==0x40404101u);
    if(hz==54000000u){normal_divs|=1u<<30;allowed=true;}
#endif
    bool pll2_valid=LN_REG(0x3c500028u)==0x01002401u;
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
    pll2_valid=(pll2_valid||LN_REG(0x3c500028u)==0x04004800u)&&!(LN_REG(0x3c500044u)&64u);
#endif
    bool boosted=source==0x3000u&&(divs==0x00414301u||divs==0x00404303u)&&pll2_valid;
    if(!allowed||LN_REG(0x3c500020u)!=0x01002402u||(!normal&&!boosted)){clock_failure=1;return false;}
    if(hz==216000000u&&!boosted){
#if !defined(LINNET_IMAGE_OS) || !LINNET_AUDIO_CLOCK_GATING
        uint32_t mode=LN_REG(0x3c500044u);
        if((mode&4u)&&(LN_REG(0x3c500028u)!=0x01002401u||(mode&64u))){clock_failure=1;return false;}
#endif
        uint32_t sm1=LN_REG(0x38501000u);if((sm1&15u)>7u){clock_failure=1;return false;}
        if(!ln_s5l_board_cpu_voltage(true)){(void)ln_s5l_board_cpu_voltage(false);clock_failure=2;return false;}
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
        if(!ln_s5l_pll2_acquire(LN_S5L_PLL2_CPU)){
            (void)ln_s5l_board_cpu_voltage(false);clock_failure=4;return false;
        }
#else
        if(!(mode&4u)){
            LN_REG(0x3c500028u)=0x01002401u;LN_REG(0x3c500038u)=32400u;
            LN_REG(0x3c500044u)=(mode&~64u)|4u;pll2_owned=true;
        }
        uint32_t began=ln_time_us();
        while((LN_REG(0x3c500040u)&0x44u)!=0x44u){
            if(ln_time_us()-began>=10000u){
                if(pll2_owned){LN_REG(0x3c500044u)&=~4u;pll2_owned=false;}
                (void)ln_s5l_board_cpu_voltage(false);clock_failure=4;return false;
            }
        }
        LN_REG(0x3c500044u)|=1u<<18;
#endif
        /* CPU=216 MHz, HClk=108 MHz, PClk=27 MHz. Raise APB and SRAM
         * dividers before switching sources; preserve their original rates. */
        uint32_t flags=ln_s5l_irq_save();
        saved_sm1=sm1;saved_sm1_valid=true;
        LN_REG(0x38501000u)=(sm1&~15u)|(((sm1&15u)+1u)*2u-1u);
        LN_REG(0x3c500004u)=0x00404303u;
        LN_REG(0x3c500000u)=(LN_REG(0x3c500000u)&0xffff0000u)|0x3000u;
        ln_s5l_irq_restore(flags);
    }else if(hz!=216000000u&&boosted){
        uint32_t flags=ln_s5l_irq_save();
        LN_REG(0x3c500000u)=(LN_REG(0x3c500000u)&0xffff0000u)|0x1000u;
        LN_REG(0x3c500004u)=0x00404101u;
        if(divs==0x00404303u){
            uint32_t sm1=LN_REG(0x38501000u);
            LN_REG(0x38501000u)=saved_sm1_valid?saved_sm1:(sm1&~15u)|(((sm1&15u)+1u)/2u-1u);
            saved_sm1_valid=false;
        }
        ln_s5l_irq_restore(flags);
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
        if(!ln_s5l_pll2_release(LN_S5L_PLL2_CPU)){state.clock_hz=108000000u;clock_failure=4;return false;}
#else
        if(pll2_owned){LN_REG(0x3c500044u)&=~(1u<<18);LN_REG(0x3c500044u)&=~4u;pll2_owned=false;}
#endif
        if(!ln_s5l_board_cpu_voltage(false)){state.clock_hz=108000000u;clock_failure=2;return false;}
    }
#if defined(LINNET_IMAGE_OS) && LINNET_RUNTIME_POWER_POLICY
    if(hz!=216000000u&&LN_REG(0x3c500004u)!=normal_divs){
        uint32_t flags=ln_s5l_irq_save();LN_REG(0x3c500004u)=normal_divs;ln_s5l_irq_restore(flags);
    }
#endif
    bool valid=(LN_REG(0x3c500000u)&0xffffu)==(hz==216000000u?0x3000u:0x1000u)&&
        LN_REG(0x3c500004u)==(hz==216000000u?0x00404303u:normal_divs);
    clock_failure=valid?0:5;if(valid)state.clock_hz=hz;return valid;
}
uint32_t ln_core_clock_failure(void){return clock_failure;}
bool ln_core_cop_pause(bool paused){(void)paused;return true;}
bool ln_core_cache_enable(void) {
    uint32_t flags=ln_s5l_irq_save();
    bool ok=ln_arm926_mmu_enable(table,0x08000000u,0x04000000u);
    ln_s5l_irq_restore(flags);return ok;
}
bool ln_core_cache_clean(void){ln_arm926_data_clean();return true;}
bool ln_core_code_publish(const void *address,uint32_t bytes){
    uintptr_t at=(uintptr_t)address;
    if(!bytes||(at&31u)||(bytes&31u)||at>UINT32_MAX-bytes)return false;
    ln_arm926_data_clean_range(address,bytes);
    return ln_arm926_instruction_cache((ln_arm926_cache_control()&0x1000u)!=0);
}
bool ln_core_cache_disable(void) {
    uint32_t flags=ln_s5l_irq_save();bool ok=ln_arm926_mmu_disable();
    ln_s5l_irq_restore(flags);return ok;
}
#ifndef LINNET_SRAM_MONITOR
bool ln_core_delay_us(uint32_t delay) {
    if(delay>100000u)return false;
    uint32_t started=ln_time_us(),budget=20000000;
    while(ln_time_us()-started<delay)if(!budget--)return false;
    ln_core_watchdog_feed();
    return true;
}
#endif
void ln_core_watchdog_arm(void) {
    if(ln_s5l_watchdog_timer(1000))ln_s5l_watchdog_route(false);
}
void ln_core_watchdog_feed(void){ln_s5l_watchdog_feed();}
void ln_core_watchdog_disarm(void) {
    __asm__ volatile("msr cpsr_c,#0xd3":::"memory");ln_s5l_watchdog_stop();
}
