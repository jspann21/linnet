#include "hal/api/recovery.h"
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "hal/api/audio.h"
#include "hal/api/display.h"
#include "services/storage/storage.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "hal/arm926/cache.h"
#include "linnet_build.h"
#include "disk_mode.h"
#include "boot_state.h"
#include "boot/include/loader.h"
static uint32_t generation,started;
static volatile uint32_t *const timeline=(volatile uint32_t *)(uintptr_t)0x22017d00u;
static bool ready,healthy,disk_requested,quiesced;
static uint8_t build[32],image_sha[32];
int ln_recovery_init(int loader) {
    quiesced=false;
    disk_requested=loader && ln_s5l_boot_mode()==LN_S5L_BOOT_ATTEMPT_DISK;
    if(disk_requested)ln_s5l_boot_request(LN_S5L_BOOT_ATTEMPT);
    const uint8_t own[32]=LINNET_BUILD_ID_BYTES;
    const uint8_t *record=(const uint8_t *)(uintptr_t)0x22017e00u;
    bool valid=ln_read_le32(record)==0x484e494cu && ln_read_le32(record+72)==ln_crc32(record,72);
    for(unsigned i=0;i<32;i++)if(record[8+i]!=own[i])valid=false;
    generation=valid?ln_read_le32(record+4):0;
    if(disk_requested)generation=1; /* Keep Boot in control until its mount resolves. */
    for(unsigned i=0;i<32;i++){build[i]=own[i];image_sha[i]=valid?record[40+i]:0;}
    started=ln_time_us();ready=true;return 1;
}
int ln_recovery_ready(void){return ready;}
int ln_recovery_image_identity(uint8_t out_build[32],uint8_t out_sha[32]) {
    if(!ready || !out_build || !out_sha)return 0;
    for(unsigned i=0;i<32;i++){out_build[i]=build[i];out_sha[i]=image_sha[i];}return 1;
}
void ln_recovery_diagnostics(uint32_t out[8]) {
    out[0]=0x32523553u;out[1]=generation;out[2]=ready;out[3]=healthy;
    out[4]=ln_time_us()-started;out[5]=ln_recovery_transport_diagnostics();out[6]=0;out[7]=0;
}
void ln_recovery_startup_diagnostics(uint32_t out[16]){for(unsigned i=0;i<16;i++)out[i]=0;}
void ln_recovery_timeline_begin(int loader,uint32_t c_entry) {
    for(unsigned i=0;i<48;i++)timeline[i]=0;
    timeline[0]=0x4d495442u;timeline[2]=generation;
    unsigned stage=loader?LN_BT_BOOT_C:LN_BT_OS_C;
    timeline[3]=1u<<stage;timeline[8+stage]=c_entry;
}
void ln_recovery_timeline_stamp(uint32_t stage) {
    if(stage<28 && !(timeline[3]&(1u<<stage))){timeline[8+stage]=ln_time_us();timeline[3]|=1u<<stage;}
}
void ln_recovery_timeline_add(uint32_t counter,uint32_t value) {
    if(counter<5){volatile uint32_t *v=timeline+41+counter;*v=value>UINT32_MAX-*v?UINT32_MAX:*v+value;timeline[40]|=1u<<counter;}
}
void ln_recovery_timeline_clock(uint32_t status){timeline[46]=status;}
void ln_recovery_timeline_hash_iram(void){}
void ln_recovery_timeline_diagnostics(uint32_t out[48]){for(unsigned i=0;i<48;i++)out[i]=timeline[i];}
void ln_recovery_arm(void){ln_core_watchdog_arm();}
void ln_recovery_feed(void){
    ln_core_watchdog_feed();
    if(disk_requested && !ln_storage_busy()) {
        disk_requested=false;
        if(ln_storage_mounted())(void)ln_recovery_reboot(0,34);
    }
}
void ln_recovery_mark_healthy(void){healthy=true;}
uint32_t ln_recovery_generation(void){return generation;}
int ln_recovery_cop_active(void){return 0;}
int ln_recovery_quiesce(void *context) {
    (void)context;
    if(quiesced)return 1;
#if defined(LINNET_IMAGE_OS) && LINNET_RUNTIME_POWER_POLICY
    /* Development RAM boot has its own quiesce path, separate from the
     * platform shutdown hook. Restore clocks before its MMU/cache handoff. */
    if(!ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ))return 0;
#endif
    timeline[6]=1;
    if(!ln_storage_quiesce())return 0;
    timeline[6]=2;
#ifdef LINNET_AUDIO
    if(!ln_audio_stop())return 0;
#endif
    timeline[6]=3;if(!ln_display_quiesce())return 0;
    timeline[6]=4;if(!ln_recovery_transport_stop())return 0;
    timeline[6]=5;if(!ln_platform_dma_idle())return 0;
    timeline[6]=6;quiesced=true;return 1;
}
int ln_recovery_chainload(void *context,const uint8_t *payload,uint32_t size,uint32_t load,uint32_t entry,
    const uint8_t next_build[32],const uint8_t sha[32],uint32_t next_generation) {
    uintptr_t address=(uintptr_t)payload;
    if(!ready || !next_build || !sha || size<64 || size>LINNET_OS_CAPACITY || load!=LINNET_LOAD_ADDRESS ||
        entry<load || entry-load>=size || (entry&3u) || address<LINNET_STAGE_ADDRESS ||
        address>=LINNET_STAGE_ADDRESS+LINNET_STAGE_CAPACITY || size>LINNET_STAGE_ADDRESS+LINNET_STAGE_CAPACITY-address)return 0;
    if(!ln_recovery_quiesce(context))return 0;
    uint8_t *record=(uint8_t *)(uintptr_t)0x22017e00u;
    ln_write_le32(record,0x484e494cu);ln_write_le32(record+4,next_generation);
    for(unsigned i=0;i<32;i++){record[8+i]=next_build[i];record[40+i]=sha[i];}
    ln_write_le32(record+72,ln_crc32(record,72));
    ln_core_watchdog_disarm();return ln_platform_enter(payload,size,load,entry);
}
int ln_recovery_can_reboot(uint16_t command){return ready && !ln_storage_busy() && (command==32 || command==33 || (command==34 && ln_storage_mounted()));}
uint32_t ln_recovery_reboot_capabilities(void){return LINNET_LOADER_CAP_REBOOT_DISK|LINNET_LOADER_CAP_REBOOT_OS;}
int ln_recovery_reboot(void *context,uint16_t command) {
    if(!ln_recovery_can_reboot(command) || !ln_recovery_quiesce(context))return 0;
    if(command==34)return ln_platform_enter_disk_mode(ln_storage_disk_sector_bytes());
    ln_s5l_boot_request(command==32?LN_S5L_BOOT_NORMAL:LN_S5L_BOOT_RECOVERY);
    ln_platform_hardware_reset();
}
_Noreturn void ln_recovery_return(uint32_t reason){timeline[7]=reason;ln_platform_hardware_reset();}
void ln_recovery_watchdog_interrupt(uint32_t pc,uint32_t cpsr){ln_panic(8,pc,cpsr);}
