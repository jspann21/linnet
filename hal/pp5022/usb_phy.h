#ifndef LINNET_PP5022_USB_PHY_H
#define LINNET_PP5022_USB_PHY_H
#include "hal/api/core.h"
#include "hal/api/platform.h"
/* PP5022 PHY latch sequence required after every ARC controller reset. */
static inline bool ln_pp5022_usb_phy_reset(void){
    volatile uint32_t *phy=(volatile uint32_t *)(uintptr_t)0x70000060u;
    volatile uint32_t *control=(volatile uint32_t *)(uintptr_t)0x70000028u;
    *phy|=15u;*control|=0x10000u;*control&=~0x10000u;*phy&=~0x20u;
    if(!ln_core_delay_us(10u))return false;
    *phy|=0x20u;if(!ln_core_delay_us(10u))return false;
    *phy=(*phy&~15u)|4u;if(!ln_core_delay_us(10u))return false;
    *phy&=~0x20u;if(!ln_core_delay_us(10u))return false;
    *phy&=~15u;if(!ln_core_delay_us(10u))return false;
    *phy|=0x20u;if(!ln_core_delay_us(10u))return false;
    *control|=0x800u;*control&=~0x800u;
    uint32_t start=ln_time_us();
    for(uint32_t budget=1000000u;budget;--budget){
        if(*control&0x80u)return true;
        if(ln_time_us()-start>=100000u)break;
    }
    return false;
}
#endif
