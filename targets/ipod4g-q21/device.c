#include <stdbool.h>
#include <stdint.h>
#include "hal/api/ata.h"
#include "hal/api/platform.h"
#include "hal/pp502x/target.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

bool ln_target_battery_charging(void)
{
    /* Fourth-generation monochrome boards expose active-low charge on B0. */
    return !(REG(0x6000d034u) & 1u);
}

void ln_target_ata_classify(const char *model,uint32_t *media_kind,uint32_t *rotation_rpm)
{
    static const char *const originals[]={"TOSHIBA MK2006GAL","TOSHIBA MK2004GAL","TOSHIBA MK4004GAH"};
    if(!model||!media_kind||!rotation_rpm)return;
    for(uint32_t n=0;n<sizeof(originals)/sizeof(originals[0]);++n){
        uint32_t i=0;
        while(originals[n][i]&&model[i]==originals[n][i])++i;
        if(originals[n][i]||model[i])continue;
        *media_kind=LN_ATA_MEDIA_ROTATING;
        *rotation_rpm=4200u;
        return;
    }
}

void ln_target_input_prepare(void)
{
#if defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    /* A cold handoff from the Apple ROM can leave the Q21 optical ASIC
     * waiting for its serial configuration command. The ordinary Rockbox
     * runtime init assumes its bootloader already performed this exchange. */
    REG(0x6000d004u) &= ~0x80u;
    REG(0x7000c104u) |= 0x0c000000u;
    REG(0x7000c120u) = 0x8000023au;
    REG(0x7000c100u) |= 0x80000000u;
    REG(0x6000d024u) &= ~0x10u;
    REG(0x6000d014u) |= 0x10u;
    uint32_t start=ln_time_us(),budget=100000u;
    while((REG(0x7000c104u)&0x80000000u)&&ln_time_us()-start<1500u&&budget){--budget;}
    REG(0x7000c100u) &= ~0x80000000u;
    REG(0x6000d004u) |= 0x80u;
    REG(0x6000d024u) |= 0x10u;
    REG(0x6000d014u) &= ~0x10u;
    REG(0x7000c104u) |= 0x0c000000u;
    REG(0x7000c100u) |= 0x60000000u;
    start=ln_time_us();budget=100000u;
    while(!(REG(0x7000c104u)&0x04000000u)&&ln_time_us()-start<1500u&&budget){--budget;}
    if(REG(0x7000c104u)&0x04000000u)(void)REG(0x7000c140u);
}

#include "targets/ipod4g-common/wiring.inc"
