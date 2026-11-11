#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "registers.h"
#include "clicker.h"
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
#define TG(offset) LN_REG(0x3c7000e0u+(offset))
#define TSTAT LN_REG(0x3c700118u)
static volatile bool active;
static volatile uint32_t started,count,max_us,period,high_period,level;
static void stop(void){
    TG(4)=0;TG(0)=0x440;TSTAT=7u<<8;
    /* Both piezo pins low; preserve the other six GPIO0 pins. */
    LN_REG(0x3cf00000u)=(LN_REG(0x3cf00000u)&0x00ffffffu)|0xee000000u;
    if(active){uint32_t elapsed=ln_time_us()-started;if(elapsed>max_us)max_us=elapsed;}
    active=false;
}
static void arm(void){
    TG(4)=0;TG(0)=0x440;TG(0x10)=11;TG(8)=level?high_period:period-high_period;
    TG(4)=2;TSTAT=7u<<8;TG(0)=0x1440;TG(4)=1;
}
void ln_platform_click_stop(void){
    uint32_t flags=ln_core_irq_save();stop();ln_core_irq_restore(flags);
}
void ln_platform_click(uint8_t tone,uint8_t volume){
    static const uint16_t periods[]={303,0,400,200};
    static const uint8_t duty[5]={0u,16u,32u,64u,128u};
    if(tone>3u||tone==1u||!volume||volume>4u)return;
    uint32_t flags=ln_core_irq_save();
    if(active){ln_core_irq_restore(flags);return;}
    /* Classic's piezo is on GPIO0.6/0.7; GPIO command reference:
     * Rockbox's piezo-6g.c bootloader. Timer G schedules the edges, preserving
     * Timer A's watchdog, Timer F's deadlines and PCM operation. */
    LN_CLOCK_GATE1&=~((1u<<5)|(1u<<12));
    LN_REG(0x3cf00200u)=0x0070eu;
    LN_REG(0x3cf00200u)=0x0060fu;
    /* Shorten only the high phase: loudness changes while pitch stays fixed. */
    period=2u*periods[tone];high_period=period*duty[volume]/256u;
    level=1;started=ln_time_us();active=true;++count;
    arm();ln_core_irq_restore(flags);
}
bool ln_s5l_click_irq(void){
    if(!(TSTAT&(1u<<8)))return false;
    TG(4)=0;TSTAT=7u<<8;
    if(!active||ln_time_us()-started>=4000u){stop();return true;}
    level^=1u;LN_REG(0x3cf00200u)=0x0060eu|level;
    arm();return true;
}
void ln_platform_click_poll(uint32_t now){
    uint32_t flags=ln_core_irq_save();
    if(active&&now-started>=4000u)stop();
    ln_core_irq_restore(flags);
}
bool ln_platform_click_deadline(uint32_t *deadline){
    uint32_t flags=ln_core_irq_save();bool pending=active;
    if(pending&&deadline)*deadline=started+4000u;
    ln_core_irq_restore(flags);return pending;
}
void ln_platform_click_diagnostics(uint32_t out[4]){
    uint32_t flags=ln_core_irq_save();
    out[0]=active?TG(0):0;out[1]=count;out[2]=max_us;out[3]=active;
    ln_core_irq_restore(flags);
}
#endif
