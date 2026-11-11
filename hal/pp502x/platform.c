#include "hal/pp502x/target.h"
#include "linnet_target.h"
#include "linnet_build.h"
#if defined(LINNET_SOC_PP5022)
#include "hal/pp5022/usb_phy.h"
#endif
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "hal/pp502x/disk_mode.h"
#include "hal/codecs/codec.h"

/* Register addresses and bus protocol provenance: docs/architecture.md. */
#define LINNET_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
static uint32_t ln_last_progress;

#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
static volatile bool click_active;
static volatile uint32_t click_started,click_count,click_max_us;
void ln_platform_click_stop(void){
    uint32_t flags=ln_core_irq_save();
    LINNET_REG(0x7000a000u)=0;
    if(click_active){uint32_t elapsed=ln_time_us()-click_started;if(elapsed>click_max_us)click_max_us=elapsed;}
    click_active=false;
    ln_core_irq_restore(flags);
}
void ln_platform_click(uint8_t tone,uint8_t volume){
    /* PWM0 drives the piezo on PP5020/PP5022; PWM1 remains the 4G backlight.
     * Keep the pulse independent of codec, I2S, and audio DMA. */
    static const uint8_t periods[4]={91u,0u,120u,60u};
    static const uint8_t duty[5]={0u,16u,32u,64u,128u};
    if(tone>3u||tone==1u||!volume||volume>4u)return;
    uint32_t flags=ln_core_irq_save();
    if(click_active){ln_core_irq_restore(flags);return;}
    LINNET_REG(0x70000010u)&=~0xcu;
    LINNET_REG(0x6000600cu)|=0x20000u;
    click_started=ln_time_us();click_active=true;++click_count;
    LINNET_REG(0x7000a000u)=0x80000000u|((uint32_t)duty[volume]<<16)|periods[tone];
    ln_core_wake_at(click_started+4000u);
    ln_core_irq_restore(flags);
}
void ln_platform_click_poll(uint32_t now){
    if(click_active&&now-click_started>=4000u)ln_platform_click_stop();
}
bool ln_platform_click_deadline(uint32_t *deadline){
    if(!click_active)return false;
    if(deadline)*deadline=click_started+4000u;
    return true;
}
void ln_platform_click_diagnostics(uint32_t out[4]){
    uint32_t flags=ln_core_irq_save();
    out[0]=LINNET_REG(0x7000a000u);out[1]=click_count;
    out[2]=click_max_us;out[3]=click_active;
    ln_core_irq_restore(flags);
}
#endif

uint32_t ln_time_us(void) { return LINNET_REG(0x60005010u); }
uintptr_t ln_platform_uncached_address(uintptr_t address) { return address | 0x10000000u; }
_Noreturn void ln_platform_hardware_reset(void) {
    LINNET_REG(0x60006004u)|=4u;
    for(;;)__asm__ volatile("nop");
}
void ln_platform_boot_candidate(bool candidate){(void)candidate;}
bool ln_platform_set_disk_mode(bool enabled) {
    static const uint8_t marker[]="diskmode\0\0hotstuff\0\0\1";
    volatile uint8_t *destination=(volatile uint8_t *)(uintptr_t)LINNET_DISK_MODE_MARKER;
    for(unsigned i=0;i<21;i++)destination[i]=enabled?marker[i]:0;
    return true;
}
bool ln_platform_enter_disk_mode(uint32_t sector_bytes){
    if(!ln_core_cache_disable())return false;
    ln_pp_disk_mode(LINNET_USB_SERIAL,sector_bytes?sector_bytes:512u,true);
}
bool ln_platform_decode_retail_payload(uint8_t *payload,uint32_t size){
    (void)payload;(void)size;
    return true;
}
int ln_platform_enter(const uint8_t *payload,uint32_t size,uint32_t load,uint32_t entry) {
    extern const uint8_t ln_persistent_enter_begin[],ln_persistent_enter_end[];
    uint32_t bytes=(uint32_t)(ln_persistent_enter_end-ln_persistent_enter_begin);
    if(!payload || !size || bytes>256)return 0;
    volatile uint8_t *iram=(volatile uint8_t *)(uintptr_t)0x40000000u;
    for(unsigned i=0;i<bytes;i++)iram[i]=ln_persistent_enter_begin[i];
    ((void (*)(const uint8_t *,uint32_t,uint32_t,uint32_t))(uintptr_t)0x40000000u)(payload,size,load,entry);
    return 0;
}
int ln_platform_enter_retail(const uint8_t *payload,uint32_t size) {
    extern const uint8_t ln_retail_enter_begin[],ln_retail_enter_end[];
    uint32_t bytes=(uint32_t)(ln_retail_enter_end-ln_retail_enter_begin);
    if(!payload||!size||bytes>256u)return 0;
#if LINNET_RETAIL_TIMER2_IRQ
    /* Restore normal IRQ routing before Apple's firmware owns interrupts. */
    LINNET_REG(0x6000402cu) &= ~2u;
#endif
#if defined(LINNET_RETAIL_I2C_DIVIDER)
    /* Restore the board's Apple I2C timing after codec shutdown. The bus
     * is idle here; the runtime divider can exceed Apple's write timeout. */
    LINNET_REG(0x600060a4u) = 0;
    LINNET_REG(0x600060a4u) = LINNET_RETAIL_I2C_DIVIDER;
#endif
    volatile uint8_t *iram=(volatile uint8_t *)(uintptr_t)0x40000000u;
    for(uint32_t i=0;i<bytes;++i)iram[i]=ln_retail_enter_begin[i];
    ((void (*)(const uint8_t *,uint32_t,uint32_t,uint32_t))(uintptr_t)0x40000000u)
        (payload,size,0x10000000u,0x10000000u);
    return 0;
}
uint32_t ln_platform_clock_divider(void){return LINNET_REG(0x60006044u);}
void ln_platform_cache_marker(uint32_t value){LINNET_REG(0x11ffe098u)=value;}

/* Both elapsed time and instruction-count bounds: a stopped timer cannot hang
 * an LCD operation. One budget covers the complete frame, including commands. */
static bool ln_wait(volatile uint32_t *reg, uint32_t mask, bool set,
                    uint32_t started, uint32_t timeout_us, uint32_t *budget)
{
    while (((*reg & mask) != 0u) != set) {
        if (*budget == 0u || ln_time_us() - started >= timeout_us) return false;
        --*budget;
    }
    return true;
}

#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_BOOT)
uint32_t ln_platform_boot_clock_ready(void)
{
    /* The measured native handoff leaves USB0 enabled, USB1 gated, neither
     * reset, and INIT_USB clear. Acquire only that known state using the
     * established controller_start/Rockbox enable-then-reset-pulse sequence.
     * USB0 assertion does not remain readable on this device. Neither its
     * pulse nor USB1 alone is an idle proof: the ARC proof below is required. */
    const uint32_t modules=0x00400008u;
    uint32_t enabled=LINNET_REG(0x6000600cu)&modules;
    if(enabled!=modules){
        if(enabled!=8u||(LINNET_REG(0x70000020u)&0x80000000u))return 7u;
        if(LINNET_REG(0x60006004u)&modules)return 8u;
        ln_target_usb_prepare();
        LINNET_REG(0x7000002cu)|=0x03000000u;
        LINNET_REG(0x6000600cu)|=modules;
        if((LINNET_REG(0x6000600cu)&modules)!=modules)return 16u;
        /* Match the native transport's separate reset pulses. Do
         * not infer held reset from a write or require USB0 assertion to latch. */
        LINNET_REG(0x60006004u)|=8u;
        LINNET_REG(0x60006004u)&=~8u;
        LINNET_REG(0x60006004u)|=0x00400000u;
        LINNET_REG(0x60006004u)&=~0x00400000u;
        if(LINNET_REG(0x60006004u)&modules)return 17u;
        LINNET_REG(0x70000020u)|=0x80000000u;
        uint32_t started=ln_time_us(),budget=2000000u;
        if(!ln_wait(&LINNET_REG(0x70000028u),0x80u,true,started,100000u,&budget))return 18u;
        LINNET_REG(0x70000028u)|=2u;
        started=ln_time_us();budget=20000000u;
        while(ln_time_us()-started<100000u)if(!budget--)return 19u;
        LINNET_REG(0x7000003cu)|=0x47au;
        if((LINNET_REG(0x7000003cu)&0x47au)!=0x47au)return 20u;
    }
    if((LINNET_REG(0x6000600cu)&modules)!=modules)return 7u;
    if(LINNET_REG(0x60006004u)&modules)return 8u;
    LINNET_REG(0xc5000148u)=0; /* ARC interrupt enable */
    LINNET_REG(0xc5000140u)&=~1u; /* RUN */
    if(LINNET_REG(0xc5000140u)&1u)return 9u;
    LINNET_REG(0xc50001b4u)=UINT32_MAX; /* ENDPTFLUSH */
    uint32_t started=ln_time_us(),budget=2000000u;
    if(!ln_wait(&LINNET_REG(0xc50001b4u),UINT32_MAX,false,started,20000u,&budget))return 10u;
    /* An endpoint may have crossed PRIME during the first flush. Repeat at
     * most once, as the established ARC owner does, without descriptor writes. */
    if(LINNET_REG(0xc50001b8u)){
        LINNET_REG(0xc50001b4u)=UINT32_MAX;
        started=ln_time_us();budget=2000000u;
        if(!ln_wait(&LINNET_REG(0xc50001b4u),UINT32_MAX,false,started,20000u,&budget)||
           LINNET_REG(0xc50001b8u))return 11u;
    }
    /* Fresh PHY setup retains controller_start's250ms stop interval; an
     * already accessible controller retains the established50ms stop.
     * No lease renewal occurs
     * in a wait; elapsed and instruction budgets both bound the whole stage. */
    started=ln_time_us();budget=20000000u;
    uint32_t stop_us=enabled==modules?50000u:250000u;
    while(ln_time_us()-started<stop_us)if(!budget--)return 12u;
    LINNET_REG(0xc5000140u)|=2u; /* RESET */
    started=ln_time_us();budget=2000000u;
    if(!ln_wait(&LINNET_REG(0xc5000140u),2u,false,started,100000u,&budget))return 13u;
#if defined(LINNET_SOC_PP5022)
    if(!ln_pp5022_usb_phy_reset())return 21u;
#endif
    /* Keep module clocks untouched: USB0 gating breaks native loader/pin
     * detection. Only completed reset plus idle ARC grants a clock change. */
    return (!(LINNET_REG(0xc5000140u)&3u)&&
        !(LINNET_REG(0xc50001b0u)|LINNET_REG(0xc50001b4u)|LINNET_REG(0xc50001b8u))&&
        (LINNET_REG(0x6000600cu)&modules)==modules&&
        !(LINNET_REG(0x60006004u)&modules))?0u:14u;
}
#endif

void ln_platform_quiesce(void)
{
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    extern bool ln_m25_coprocessor_quiesce_display(void);
    if(!ln_m25_coprocessor_quiesce_display())ln_panic(32u,0u,55u);
#endif
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    LINNET_REG(0x60004028u) = UINT32_MAX;
    LINNET_REG(0x60004128u) = UINT32_MAX;
    LINNET_REG(0x60004038u) = UINT32_MAX;
    LINNET_REG(0x60004138u) = UINT32_MAX;
    LINNET_REG(0x6000a008u) = 0;
    for (uint32_t channel = 0; channel < 4u; ++channel)
        LINNET_REG(0x6000b000u + channel * 32u) &= ~0xc0000000u;
}

bool ln_platform_dma_idle(void)
{
#if (defined(LINNET_SOC_PP5022) || LINNET_TARGET_NUMBER == 98) && defined(LINNET_IMAGE_OS)
    if((LINNET_REG(0x6000600cu)&0x02000000u)&&
       !(LINNET_REG(0x60006004u)&0x02000000u)&&
       (LINNET_REG(0xc3000400u)&1u))return false;
#endif
    uint32_t started = ln_time_us(), budget = 100000u;
    for (uint32_t channel = 0; channel < 4u; ++channel) {
        if (!ln_wait(&LINNET_REG(0x6000b004u + channel * 32u), 0x80000000u,
                     false, started, 100000u, &budget)) return false;
    }
    LINNET_REG(0x6000a000u) &= ~0x80000000u;
    return true;
}

#define LINNET_REG8(a) (*(volatile uint8_t *)(uintptr_t)(a))
static bool ln_codec_idle(void)
{
    uint32_t started = ln_time_us();
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (!(LINNET_REG8(0x7000c01cu) & 0x40u)) return true;
        if (ln_time_us() - started >= 5000u) break;
    }
    return false;
}

static bool ln_codec_write(uint8_t reg, uint16_t value)
{
    if (!ln_codec_idle()) return false;
    LINNET_REG8(0x7000c004u) = 0x34u; /* WM8975 7-bit address 0x1a. */
    LINNET_REG8(0x7000c00cu) = (uint8_t)((reg << 1) | ((value >> 8) & 1u));
    LINNET_REG8(0x7000c010u) = (uint8_t)value;
    LINNET_REG8(0x7000c000u) = (uint8_t)((LINNET_REG8(0x7000c000u) & ~0x26u) | 2u);
    LINNET_REG8(0x7000c000u) |= 0x80u;
    return ln_codec_idle();
}

bool ln_platform_audio_idle(void)
{
    if (!ln_platform_dma_idle()) return false;
    /* Hold I2S device in reset so inherited FIFO contents cannot be clocked. */
    LINNET_REG(0x60006004u) |= 0x800u;
    LINNET_REG(0x6000600cu) &= ~0x800u;
    LINNET_REG(0x6000600cu) |= 0x1000u;
    LINNET_REG(0x60006004u) |= 0x1000u;
    __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
    LINNET_REG(0x60006004u) &= ~0x1000u;
    LINNET_REG(0x600060a4u) = 0;
    LINNET_REG(0x600060a4u) = 0x80u;
    return ln_codec_off(ln_codec_write);
}

/* Cooperative progress detector. */
void ln_watchdog_feed(void) { ln_last_progress = ln_time_us(); }
bool ln_watchdog_expired(void) { return ln_time_us() - ln_last_progress >= 500000u; }
