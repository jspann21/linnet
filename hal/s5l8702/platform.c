#include "hal/api/platform.h"
#include "hal/s5l8702/aes.h"
#include "hal/api/core.h"
#include "hal/api/audio.h"
#include "hal/api/ata.h"
#ifdef LINNET_H264_VPU
#include "vpu.h"
#endif
#ifdef LINNET_MPEG4_VPU
#include "mpeg4_vpu.h"
#endif
#include "hal/api/display.h"
#include "hal/arm926/cache.h"
#include "hal/arm926/mmu.h"
#include "pcm_dma.h"
#include "registers.h"
#include "boot_state.h"
#include "disk_mode.h"
#include "linnet_build.h"
static uint32_t progress_time;
uint32_t ln_time_us(void){return LN_TIMER_E(0x14);}
uintptr_t ln_platform_uncached_address(uintptr_t address) {
    return address>=0x08000000u && address<0x0c000000u && (ln_arm926_cache_control()&1u)?address+0x10000000u:address;
}
_Noreturn void ln_platform_hardware_reset(void) {
    __asm__ volatile("msr cpsr_c,#0xd3":::"memory");
    LN_REG(0x3c800000u)=0x100000u;for(;;)__asm__ volatile("nop");
}
bool ln_platform_set_disk_mode(bool enabled){ln_s5l_boot_request(enabled?LN_S5L_BOOT_DISK:LN_S5L_BOOT_NORMAL);return true;}
bool ln_platform_enter_disk_mode(uint32_t sector_bytes){
    const char *serial=ln_s5l_boot_serial();
    if(!serial)return false;
    if(!ln_arm926_mmu_disable())return false;
    ln_s5l_boot_request(LN_S5L_BOOT_NORMAL);
    ln_s5l_disk_mode(serial,sector_bytes?sector_bytes:512u,true);
}
bool ln_platform_decode_retail_payload(uint8_t *payload,uint32_t size){
    /* N25's original NOR body uses the device key and one complete CBC stream. */
    return ln_s5l_aes_crypt(payload,size,2u,false);
}
void ln_platform_boot_candidate(bool candidate){
    /* An unconfirmed OS gets one reset through Boot to roll back its slot.
     * A fault in the confirmed fallback still stops in the SRAM monitor. */
    ln_s5l_boot_request(candidate?LN_S5L_BOOT_NORMAL:LN_S5L_BOOT_ATTEMPT);
}
uint32_t ln_platform_clock_divider(void){return LN_REG(0x3c500004u);}
void ln_platform_cache_marker(uint32_t value){LN_REG(LINNET_MAILBOX_ADDRESS-4u)=value;}
bool ln_platform_audio_idle(void){return ln_s5l_pcm_stop();}
bool ln_platform_dma_idle(void){
    struct ln_ata_state ata;ln_ata_state_snapshot(&ata);
    if(ata.busy||ata.initializing)return false;
#ifdef LINNET_H264_VPU
    if(ln_vpu_busy())return false;
#endif
#ifdef LINNET_MPEG4_VPU
    if(ln_mpeg4_vpu_busy())return false;
#endif
    LN_CLOCK_GATE0&=~((1u<<25)|(1u<<26));
    return !(LN_REG(0x3820001cu)|LN_REG(0x3990001cu));
}
void ln_platform_quiesce(void){
#if defined(LINNET_IMAGE_OS) && LINNET_RUNTIME_POWER_POLICY
    /* Every Boot/monitor handoff inherits the original normal clock layout. */
    (void)ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ);
#endif
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    (void)ln_s5l_pcm_stop();
#ifdef LINNET_H264_VPU
    ln_vpu_close();
#endif
#ifdef LINNET_MPEG4_VPU
    ln_mpeg4_vpu_close();
#endif
}
void ln_watchdog_feed(void){progress_time=ln_time_us();ln_core_watchdog_feed();}
bool ln_watchdog_expired(void){return ln_time_us()-progress_time>=1000000u;}
uint32_t ln_platform_boot_clock_ready(void){return 0;}
int ln_platform_enter(const uint8_t *payload,uint32_t size,uint32_t load,uint32_t entry) {
    extern const uint8_t ln_persistent_enter_begin[],ln_persistent_enter_end[];
    uint32_t code_size=(uint32_t)(ln_persistent_enter_end-ln_persistent_enter_begin);
    if(!payload || !size || size>LINNET_OS_CAPACITY || load!=LINNET_LOAD_ADDRESS ||
        entry<load || entry-load>=size || (entry&3u) || code_size>256)return 0;
    if(!ln_arm926_mmu_disable())return 0;
    uint8_t *code=(uint8_t *)(uintptr_t)0x22018000u;
    for(unsigned i=0;i<code_size;i++)code[i]=ln_persistent_enter_begin[i];
    (void)ln_arm926_instruction_cache(true);
    ((void (*)(const uint8_t *,uint32_t,uint32_t,uint32_t))(uintptr_t)code)(payload,size,load,entry);
    return 0;
}
int ln_platform_enter_retail(const uint8_t *payload,uint32_t size){
    extern const uint8_t ln_persistent_enter_begin[],ln_persistent_enter_end[];
    uint32_t code_size=(uint32_t)(ln_persistent_enter_end-ln_persistent_enter_begin);
    /* The original N25 IM3 body occupies 0x22000000..0x2201f800. Keep the
     * position-independent copier in the remaining top256 bytes of SRAM. */
    if(!payload||size<32u||size>0x1f800u||code_size>256u)return 0;
    /* The persistent monitor marks Linnet Boot as an attempt. Retail firmware
     * owns the next reset after a successful handoff, so clear that marker. */
    ln_s5l_boot_request(LN_S5L_BOOT_NORMAL);
    if(!ln_arm926_mmu_disable())return 0;
    uint8_t *code=(uint8_t *)(uintptr_t)0x2201ff00u;
    for(uint32_t i=0;i<code_size;++i)code[i]=ln_persistent_enter_begin[i];
    (void)ln_arm926_instruction_cache(true);
    ((void (*)(const uint8_t *,uint32_t,uint32_t,uint32_t))(uintptr_t)code)(
        payload,size,0x22000000u,0x22000000u);
    return 0;
}
