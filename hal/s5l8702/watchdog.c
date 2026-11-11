#include "watchdog.h"
#include "registers.h"
#define TA(offset) LN_REG(0x3c700000u+(offset))
#define VIC(offset) LN_REG(0x38e00000u+(offset))
void ln_s5l_watchdog_stop(void) {
    VIC(0x14)=1u<<8;VIC(0x1c)=1u<<8;
    TA(4)=0;TA(0)=0x70040u;VIC(0x0c)&=~(1u<<8);
}
bool ln_s5l_watchdog_timer(uint32_t milliseconds) {
    if(milliseconds<10 || milliseconds>1000)return false;
    ln_s5l_watchdog_stop();LN_CLOCK_GATE1&=~(1u<<5);
    /* ECLK / 2 / 120 = 50 kHz, within the 16-bit counter. */
    TA(0x10)=119;TA(8)=milliseconds*50u;TA(4)=2;
    TA(0)=0x71040u;TA(4)=1;return true;
}
void ln_s5l_watchdog_route(bool software) {
    VIC(0x0c)|=1u<<8;VIC(0x10)=1u<<8;
    if(software)VIC(0x18)=1u<<8;
    uint32_t flags;
    __asm__ volatile("mrs %0,cpsr\nbic %0,%0,#0x40\nmsr cpsr_c,%0":"=&r"(flags)::"memory");
}
void ln_s5l_watchdog_feed(void){TA(4)=3;}
void ln_s5l_watchdog_snapshot(uint32_t out[4]) {
    out[0]=TA(0);out[1]=TA(0x14);out[2]=VIC(4);out[3]=VIC(0x0c);
}
