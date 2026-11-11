#ifndef LINNET_PP502X_RECOVERY_INTERNAL_H
#define LINNET_PP502X_RECOVERY_INTERNAL_H
#include "linnet_target.h"

#define LN_STARTUP_TIMING_MAGIC 0x50555453
#define LN_STARTUP_TIMING_BASE 192
#define LN_STARTUP_TIMING_MASK 208
#define LN_STARTUP_TIMING_TICKS 212
#define LN_BOOT_TIMELINE_BASE 256

#if defined(__ASSEMBLER__) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
/* Private scratch r7-r9 only; no lease renewal or work inside progress loops.
 * Requiring the preceding mask prevents reusing a completed stale trailer. */
.macro LN_STARTUP_STAMP index, bit
    ldr r7, =(LINNET_MAILBOX_ADDRESS - 0x1000 + 0x10000000)
    ldr r8, [r7, #LN_STARTUP_TIMING_BASE]
    ldr r9, =LN_STARTUP_TIMING_MAGIC
    cmp r8, r9
    bne 99f
    ldr r8, [r7, #LN_STARTUP_TIMING_MASK]
    cmp r8, #(\bit - 1)
    bne 99f
    ldr r9, =0x60005010
    ldr r9, [r9]
    str r9, [r7, #(LN_STARTUP_TIMING_TICKS + 4 * \index)]
    orr r8, r8, #\bit
    str r8, [r7, #LN_STARTUP_TIMING_MASK]
99:
.endm
#endif
#endif
