/* M25 board wiring. Register provenance is in README.md alongside this target. */
#include "hal/pp502x/target.h"
#include "hal/api/power.h"
#include "hal/api/ata.h"
#include "hal/api/core.h"
#include "hal/api/input.h"
#include "hal/api/platform.h"
#if defined(LINNET_IMAGE_OS)
#include "targets/video5g-m25/coprocessor.h"
#endif
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
bool ln_target_battery_charging(void){return !(REG(0x6000d034u)&1u);}
uint8_t ln_target_power_sources(void){
    uint32_t pins=REG(0x6000d13cu);
    return ((pins&0x10u)?LINNET_POWER_USB:0u)|((pins&8u)?0u:LINNET_POWER_FIREWIRE);
}
void ln_target_power_prepare(struct ln_power_state *state){
    state->brightness=128u;state->backlight_on=(REG(0x6000d024u)&8u)!=0;
    REG(0x6000d004u)|=1u;REG(0x6000d014u)&=~1u;
    REG(0x6000d10cu)|=0x18u;REG(0x6000d11cu)&=~0x18u;
}
uint16_t ln_target_charge_limit_get(void){
    return REG(0x6000d12cu)&4u?0u:REG(0x6000d020u)&4u?500u:100u;
}
void ln_target_charge_limit_set(uint16_t milliamps){
    REG(0x6000d10cu)|=4u;REG(0x6000d11cu)|=4u;
    REG(0x6000d000u)|=4u;REG(0x6000d010u)|=4u;
    if(milliamps)REG(0x6000d12cu)&=~4u;else REG(0x6000d12cu)|=4u;
    if(milliamps==500u)REG(0x6000d020u)|=4u;else REG(0x6000d020u)&=~4u;
}
void ln_target_ata_power(bool on){
    if(on){
        REG(0x70000080u)&=~0x40000000u;
        (void)ln_core_delay_us(5000u);
        REG(0x6000600cu)|=0x02000000u;
        REG(0x6000d088u)=0;REG(0x6000d08cu)=0;
        REG(0x6000d100u)&=~0xbfu;REG(0x6000d108u)&=~0x1fu;
    }else{
        REG(0x6000600cu)&=~0x02000000u;
        (void)ln_core_delay_us(10u);
        REG(0x6000d088u)=0xffu;REG(0x6000d08cu)=0xffu;
        REG(0x6000d100u)|=0xbfu;REG(0x6000d108u)|=0x1fu;
        REG(0x70000080u)|=0x40000000u;
    }
}
void ln_target_ata_classify(const char *model,uint32_t *kind,uint32_t *rpm){
    /* Older IDENTIFY data omits rotation rate; match only the documented model. */
    static const char *const models[]={"TOSHIBA MK3008GAL","TOSHIBA MK6008GAH","TOSHIBA MK8010GAH",
        "HTC426030G5CE00"};
    if(!model||!kind||!rpm)return;
    for(uint32_t n=0;n<sizeof(models)/sizeof(models[0]);++n){
        uint32_t i=0;while(models[n][i]&&model[i]==models[n][i])++i;
        if(!models[n][i]&&!model[i]){*kind=LN_ATA_MEDIA_ROTATING;*rpm=4200u;return;}
    }
}
void ln_target_input_prepare(void){
#if defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
}
void ln_target_switches_prepare(void){REG(0x6000d000u)|=0xa0u;REG(0x6000d010u)&=~0xa0u;}
uint32_t ln_target_switches_read(void){return REG(0x6000d030u);}
void ln_target_switches_rearm(uint32_t sample){
    REG(0x6000d060u)=(REG(0x6000d060u)&~0xa0u)|(~sample&0xa0u);
    REG(0x6000d070u)=0xa0u;REG(0x6000d050u)|=0xa0u;
}
#if defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t ln_boot_menu_query[4];
void ln_input_boot_menu_diagnostics(uint32_t output[4]) {
    for(uint32_t i=0;i<4u;++i)output[i]=ln_boot_menu_query[i];
}
#endif
bool ln_input_boot_menu_held(void) {
    /* A button held through reset can precede the first wheel packet. Ask
     * the keypad for its current state after initializing the controller. */
    for(uint32_t attempt=0;attempt<5u;++attempt){
#if !defined(LINNET_PROFILE_RELEASE)
        ln_boot_menu_query[0]=attempt+1u;
#endif
        REG(0x6000d004u)&=~0x80u;
        REG(0x7000c104u)|=0x0c000000u;
        REG(0x7000c120u)=0x8000023au;
        REG(0x7000c100u)|=0x80000000u;
        REG(0x6000d024u)&=~0x10u;
        REG(0x6000d014u)|=0x10u;
        uint32_t start=ln_time_us();
        while((REG(0x7000c104u)&0x80000000u)&&ln_time_us()-start<1500u){}
        REG(0x7000c100u)&=~0x80000000u;
        REG(0x6000d004u)|=0x80u;
        REG(0x6000d024u)|=0x10u;
        REG(0x6000d014u)&=~0x10u;
        REG(0x7000c104u)|=0x0c000000u;
        REG(0x7000c100u)|=0x60000000u;
        start=ln_time_us();
        while(!(REG(0x7000c104u)&0x04000000u)&&ln_time_us()-start<1500u){}
        uint32_t status=REG(0x7000c104u);
#if !defined(LINNET_PROFILE_RELEASE)
        ln_boot_menu_query[1]=status;
#endif
        if(status&0x04000000u){
            if(!ln_core_delay_us(50u))return false;
            uint32_t packet=REG(0x7000c140u);
#if !defined(LINNET_PROFILE_RELEASE)
            ln_boot_menu_query[2]=packet;
#endif
            REG(0x7000c104u)|=0x0c000000u;
            REG(0x7000c100u)=0x400a1f00u;
            if((packet&0x8000ffffu)==0x8000023au){
#if !defined(LINNET_PROFILE_RELEASE)
                ln_boot_menu_query[3]=(packet&0x00100000u)?2u:1u;
#endif
                return (packet&0x00100000u)!=0u;
            }
            if((packet&0x800000ffu)==0x8000001au&&
               (!(packet&0x40000000u)||((packet>>16)&0x7fu)<96u)){
#if !defined(LINNET_PROFILE_RELEASE)
                ln_boot_menu_query[3]=(packet&0x00001000u)?4u:3u;
#endif
                return (packet&0x00001000u)!=0u;
            }
        }
    }
    return false;
}
#endif
void ln_target_backlight(bool on,uint8_t brightness){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_display_blanked())on=false;
#endif
    static bool initialized,enabled;
    static uint8_t level=16;
    REG(0x6000d004u)|=8u;REG(0x6000d014u)|=8u;
    REG(0x6000d00cu)|=0x80u;REG(0x6000d01cu)|=0x80u;
    REG(0x6000d10cu)|=0x80u;REG(0x6000d11cu)|=0x80u;
    if(!on||!brightness){
        REG(0x6000d12cu)&=~0x80u;REG(0x6000d02cu)&=~0x80u;REG(0x6000d024u)&=~8u;
        enabled=false;initialized=true;return;
    }
    if(!initialized||!enabled){
        REG(0x6000d024u)|=8u;REG(0x6000d02cu)|=0x80u;
        if(!ln_core_delay_us(10000u))return;
        level=16;enabled=true;initialized=true;
    }
    uint8_t desired=1u+((uint32_t)brightness*31u)/255u;
    while(level!=desired){
        uint32_t flags=ln_core_irq_save();
        REG(0x6000d02cu)&=~0x80u;
        bool ok=ln_core_delay_us(level<desired?10u:200u);
        REG(0x6000d02cu)|=0x80u;ln_core_irq_restore(flags);
        if(!ok||!ln_core_delay_us(10u))return;
        level+=level<desired?1:-1;
    }
    REG(0x6000d12cu)|=0x80u;
}

void ln_target_usb_prepare(void){REG(0x6000d11cu)&=~0x10u;REG(0x6000d10cu)|=0x10u;}
