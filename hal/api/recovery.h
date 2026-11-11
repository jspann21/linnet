#ifndef LINNET_HAL_RECOVERY_H
#define LINNET_HAL_RECOVERY_H
#if !defined(__ASSEMBLER__)
#include <stdint.h>
#include "linnet_target.h"
int ln_recovery_init(int loader);
int ln_recovery_ready(void);
int ln_recovery_image_identity(uint8_t build[32], uint8_t image_sha[32]);
void ln_recovery_diagnostics(uint32_t output[8]);
/* Dev-only startup timing: magic/reserved/generation/payload bytes/valid mask, eight raw
 * microsecond ticks (CRC start/end, copy dispatch, OS entry, BSS start/end,
 * IRAM end, C entry), then three zero reserved words. Subtract ticks modulo
 * 2^32 only when both corresponding mask bits exist. All zero means unavailable.
 * Native Boot mount/LOS validation and later OS runtime/USB are not timed. */
void ln_recovery_startup_diagnostics(uint32_t output[16]);
/* Boot timeline command54: 48 words at256..447, independent of startup timing. Header has
 * magic/reserved/generation/stamp mask and inherited clock-source/PLL/cache/flags.
 * Then32 raw ticks, counter mask/read-us/SHA-us/LOS-CRC-us/bytes/read-calls and
 * Clock status:0 unobserved,1 skipped,2 attempting,3 verified80MHz,4 failed.
 * Words36..39 report Boot device enable/reset/init2/USB clock registers;
 * clock status5..14 distinguishes individual skipped preconditions.
 * Status15..20 covers native USB ownership reset/enable/release/PHY/settle/XMB.
 * Stamps publish once; elapsed counters saturate.
 * Flags bit0 means native Boot prepare was observed, not a RAM upload.
 * Flag bit1 records verified Boot SHA IRAM copy. */
enum ln_boot_timeline_stage {
    LN_BT_BOOT_C, LN_BT_BOOT_RECOVERY, LN_BT_BOOT_USB, LN_BT_BOOT_STORAGE,
    LN_BT_BOOT_BLOCK, LN_BT_BOOT_FAT, LN_BT_BOOT_FILES, LN_BT_BOOT_PROOF,
    LN_BT_BOOT_STORES, LN_BT_BOOT_VALIDATE, LN_BT_BOOT_VALIDATED, LN_BT_CHAINLOAD,
    LN_BT_OS_C, LN_BT_OS_RECOVERY, LN_BT_OS_RUNTIME, LN_BT_OS_CLOCK,
    LN_BT_OS_MEDIA, LN_BT_OS_FRAME, LN_BT_OS_RUNTIME_END, LN_BT_OS_USB,
    LN_BT_OS_STORAGE, LN_BT_OS_BLOCK, LN_BT_OS_FAT, LN_BT_OS_FILES,
    LN_BT_OS_PROOF, LN_BT_OS_CONFIRMED, LN_BT_OS_POLL, LN_BT_OS_REQUEST
};
#if !defined(LINNET_PROFILE_RELEASE)
void ln_recovery_timeline_begin(int loader, uint32_t c_entry);
void ln_recovery_timeline_stamp(uint32_t stage);
void ln_recovery_timeline_add(uint32_t counter, uint32_t value);
void ln_recovery_timeline_clock(uint32_t status);
void ln_recovery_timeline_hash_iram(void);
void ln_recovery_timeline_diagnostics(uint32_t output[48]);
#define LN_BOOT_STAMP(stage) ln_recovery_timeline_stamp(stage)
#else
#define LN_BOOT_STAMP(stage) ((void)0)
#endif
void ln_recovery_arm(void);
void ln_recovery_feed(void);
void ln_recovery_mark_healthy(void);
uint32_t ln_recovery_generation(void);
int ln_recovery_cop_active(void);
int ln_recovery_quiesce(void *context);
/* Dev runtime overrides to stop USB DMA; release has no transport. */
int ln_recovery_transport_stop(void);
uint32_t ln_recovery_transport_diagnostics(void);
int ln_recovery_chainload(void *context, const uint8_t *payload, uint32_t size,
    uint32_t load, uint32_t entry, const uint8_t build_id[32],
    const uint8_t image_sha[32], uint32_t next_generation);
int ln_recovery_reboot(void *context, uint16_t command);
int ln_recovery_can_reboot(uint16_t command);
/* Implemented reboot modes, independent of transient candidate readiness. */
uint32_t ln_recovery_reboot_capabilities(void);
_Noreturn void ln_recovery_return(uint32_t reason);
void ln_recovery_watchdog_interrupt(uint32_t pc, uint32_t cpsr);
#endif
#endif
