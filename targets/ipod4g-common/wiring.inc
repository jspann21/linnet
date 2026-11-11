/* Board wiring shared by Q21 and P98, not the PortalPlayer family. */
#include "hal/api/power.h"
#include "hal/api/input.h"
uint8_t ln_target_power_sources(void) {
    return ((REG(0x6000d03cu)&8u)?LINNET_POWER_USB:0u)|
           ((REG(0x6000d038u)&4u)?0u:LINNET_POWER_FIREWIRE);
}
void ln_target_power_prepare(struct ln_power_state *state) {
    state->brightness=(uint8_t)(REG(0x7000a010u)>>16);
    state->backlight_on=(REG(0x6000d024u)&8u)!=0;
    REG(0x7000008cu)|=1u;REG(0x70000084u)&=~1u;
}
uint16_t ln_target_charge_limit_get(void) {
    uint32_t value=REG(0x70000080u);
    return value&0x08000000u?0u:value&0x40u?500u:100u;
}
void ln_target_charge_limit_set(uint16_t milliamps) {
    /* Caller must supply the negotiated USB power budget before selecting 500. */
    REG(0x70000084u) |= 0x08000040u;
    uint32_t value = REG(0x70000080u) & ~0x08000040u;
    if (!milliamps) value |= 0x08000000u;
    if (milliamps == 500) value |= 0x40u;
    REG(0x70000080u) = value;
}
void ln_target_backlight(bool on,uint8_t brightness) {
    REG(0x6000d804u) = 0x0c0cu;
    REG(0x70000084u) &= ~0x02000000u;
    REG(0x6000600cu) |= 0x20000u;
    REG(0x7000a010u) = 0x80000000u | ((uint32_t)(on ? brightness : 0u) << 16);
    REG(0x6000d824u) = on ? 0x0808u : 0x0800u;
}
void ln_target_ata_power(bool on) {
    if(on){
        REG(0x6000d124u)&=~4u;REG(0x6000d100u)&=~0xbfu;REG(0x6000d108u)&=~0x1fu;
        REG(0x6000600cu)|=0x02000000u;
    }else{
        REG(0x6000600cu)&=~0x02000000u;REG(0x6000d124u)|=4u;
        REG(0x6000d100u)|=0xbfu;REG(0x6000d108u)|=0x1fu;
    }
}
void ln_target_switches_prepare(void) {
    REG(0x6000d000u)|=0xa0u;REG(0x6000d010u)&=~0xa0u;
}
uint32_t ln_target_switches_read(void) { return REG(0x6000d030u); }
void ln_target_switches_rearm(uint32_t sample) {
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
    /* The 4G optical wheel does not report a button already held at reset
     * until asked for its current keypad state. Keep this boot-only so the
     * normal input stream and OS image are unaffected. */
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
        }
    }
    return false;
}
#endif

void ln_target_usb_prepare(void){REG(0x6000d01cu)&=~8u;REG(0x6000d00cu)|=8u;}
