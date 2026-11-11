#include "timer.h"
#include "registers.h"
#define TF(offset) LN_REG(0x3c7000c0u+(offset))
#define TSTAT LN_REG(0x3c700118u)
void ln_s5l_deadline_stop(void) {
    TF(4)=0;TF(0)=0x440;TSTAT=7u<<16;
}
bool ln_s5l_deadline_arm(uint32_t microseconds) {
    if(!microseconds || microseconds>1000000u)return false;
    LN_CLOCK_GATE1&=~(1u<<5);
    ln_s5l_deadline_stop();TF(0x10)=11;TF(8)=microseconds;
    /* Latch the new compare before enabling its interrupt. On the first
     * use, enabling with the inherited zero compare can assert immediately. */
    TF(4)=2;TSTAT=7u<<16;TF(0)=0x1440;TF(4)=1;return true;
}
bool ln_s5l_deadline_pending(void){return (TSTAT&(1u<<16))!=0;}
