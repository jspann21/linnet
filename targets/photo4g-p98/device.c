#include <stdbool.h>
#include <stdint.h>
#include "hal/api/ata.h"
#include "hal/api/platform.h"
#include "hal/pp502x/target.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

bool ln_target_battery_charging(void)
{
    /* P98 exposes the active-low charger state through GPO32 input bit 0. */
    REG(0x70000088u) = 0;
    return !(REG(0x70000088u) & 1u);
}

void ln_target_ata_classify(const char *model,uint32_t *media_kind,uint32_t *rotation_rpm)
{
    static const char *const models[]={"TOSHIBA MK2006GAL","TOSHIBA MK3006GAL",
        "TOSHIBA MK4006GAH","TOSHIBA MK6006GAH"};
    if(!model||!media_kind||!rotation_rpm)return;
    for(uint32_t n=0;n<sizeof(models)/sizeof(models[0]);++n){
        uint32_t i=0;while(models[n][i]&&model[i]==models[n][i])++i;
        if(!models[n][i]&&!model[i]){*media_kind=LN_ATA_MEDIA_ROTATING;*rotation_rpm=4200u;return;}
    }
}

void ln_target_input_prepare(void) {
#if defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
}

#include "targets/ipod4g-common/wiring.inc"
