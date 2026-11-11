.syntax unified
.cpu arm7tdmi
.arm
#include "hal/api/recovery.h"
#include "hal/pp502x/recovery_internal.h"
#include "linnet_target.h"
#if defined(LINNET_PLATFORM_CORE)
#include "hal/api/display.h"
#include "hal/pp502x/display_internal.h"
#endif
.section .text.startup,"ax",%progbits
.global ln_platform_reset
ln_platform_reset:
    msr cpsr_c, #0xd3
    ldr r0, =0x6000c000
    ldr r1, [r0]
    tst r1, #1
    beq 2f
    ldr r2, =0xf000f044
    ldr r1, [r2]
    orr r1, r1, #6
    str r1, [r2]
    mov r3, #0x100000
1:  ldr r1, [r0]
    tst r1, #0x8000
    beq 2f
    subs r3, r3, #1
    bne 1b
    b ln_early_stop
2:  mov r1, #0
    str r1, [r0]
    nop
    nop
    nop
    ldr r0, =0x60000000
    ldrb r1, [r0]
    cmp r1, #0x55
    bne ln_cop_install
#if defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    ldr r0, =0x11ffe000
    mov r1, #0x40
    str r1, [r0, #128]
    LN_STARTUP_STAMP 3, 8
#endif
#if defined(LINNET_IMAGE_BOOT) && !defined(LINNET_PROFILE_RELEASE)
    /* Only a fresh CPU bootstrap through the native SDRAM alias may replace
     * the resident anchor. A low-address RAM Boot candidate preserves it.
     * Use the actual PC-relative entry minus its linked address, not a high
     * bit test that could accept a different relocation window. COP owns its
     * cookie/tick fields concurrently, so leave both untouched. */
    adr r0, ln_platform_reset
    ldr r1, =ln_platform_reset
    sub r0, r0, r1
    cmp r0, #0x10000000
    bne ln_keep_anchor
    ldr r0, =0x11ffe000
    mov r1, #0
    str r1, [r0, #20]
    str r1, [r0]
    str r1, [r0, #LN_STARTUP_TIMING_BASE]
ln_keep_anchor:
#endif
#if defined(LINNET_PROFILE_RELEASE) && !defined(LINNET_PLATFORM_CORE)
    ldr r0, =0x60007004
    mov r1, #0x80000000
    str r1, [r0]
    nop
    nop
    nop
    b ln_cpu_remap
#else
    /* Require a sleeping inherited COP or a live Linnet monitor handshake. */
    ldr r0, =0x60007004
    ldr r1, [r0]
    tst r1, #0x80000000
    bne ln_cpu_remap
    ldr r0, =0x11ffe000
    ldr r2, =0x4c4e434f
    ldr r4, [r0, #28]
    mov r3, #0x100000
10: ldr r1, [r0, #24]
    cmp r1, r2
    ldreq r1, [r0, #28]
    cmpeq r1, r4
    bne 11f
    subs r3, r3, #1
    bne 10b
    b ln_early_stop
11: ldr r1, [r0, #24]
    cmp r1, r2
    beq ln_cpu_remap
    subs r3, r3, #1
    bne 10b
    b ln_early_stop
#endif
ln_cpu_remap:
    adr r0, ln_remap_begin
    adr r1, ln_remap_end
    ldr r2, =0x40000000
3:  cmp r0, r1
    ldrlo r3, [r0], #4
    strlo r3, [r2], #4
    blo 3b
    ldr pc, =0x40000000
.align 2
ln_remap_begin:
    ldr r0, 4f
    ldr r1, 5f
    str r1, [r0]
    ldr r1, 6f
    str r1, [r0, #4]
    ldr pc, 7f
4:  .word 0xf000f000
5:  .word 0x00003e00
6:  .word 0x10000f84
7:  .word ln_reset_continue
ln_remap_end:
    .ltorg

/* Only COP entering Linnet is adopted; never wake an inherited parked COP. */
ln_cop_install:
#if defined(LINNET_PROFILE_RELEASE) && !defined(LINNET_PLATFORM_CORE)
    ldr r0, =0x60007004
    mov r1, #0x80000000
20: str r1, [r0]
    b 20b
#else
    adr r0, ln_cop_begin
#if defined(LINNET_PLATFORM_CORE)
    ldr r1, =ln_cop_end-ln_cop_begin
    add r1, r0, r1
#else
    adr r1, ln_cop_end
#endif
    ldr r2, =0x40001000
8:  cmp r0, r1
    ldrlo r3, [r0], #4
    strlo r3, [r2], #4
    blo 8b
    ldr pc, =0x40001000
.align 2
ln_cop_begin:
#if defined(LINNET_PLATFORM_CORE)
    ldr sp, ln_cop_stack_address
    ldr r0, ln_cop_mailbox_address
    mov r1, #0
    str r1, [r0, #LN_DISPLAY_ENABLED]
    str r1, [r0, #LN_DISPLAY_MAGIC]
    str r1, [r0, #LN_DISPLAY_PAUSE]
    str r1, [r0, #LN_DISPLAY_PAUSED]
#endif
    ldr r0, 13f
    mov r1, #0
    str r1, [r0, #20]
    ldr r1, 14f
    str r1, [r0, #24]
#if defined(LINNET_PLATFORM_CORE)
    ldr r1, ln_cop_capability_value
    str r1, [r0, #160]
    mov r1, #0
    str r1, [r0, #164]
#endif
    ldr r2, 15f
9:
#if defined(LINNET_PLATFORM_CORE)
    bl ln_cop_retail_step
#endif
    ldr r3, [r2]
    str r3, [r0, #28]
    ldr r4, [r0, #20]
    cmp r4, #1
    bne 9b
    ldr r4, [r0, #16]
    sub r4, r3, r4
    ldr r5, 16f
    cmp r4, r5
    /* CPU may publish progress after our timer sample. A negative elapsed
     * value is a fresh lease, not an unsigned wrap past the reset deadline. */
    blt 9b
    ldr r5, 17f
    mov r6, #2
    str r6, [r5]
    ldr r5, 18f
    cmp r4, r5
    blo 9b
    ldr r5, 19f
    ldr r7, [r0, #152]
    cmp r7, #0
    moveq r7, #0xc0
    streq r7, [r0, #152]
    ldr r6, [r5]
    orr r6, r6, #4
    str r6, [r5]
12: b 12b
13: .word 0x11ffe000
14: .word 0x4c4e434f
15: .word 0x60005010
16: .word 750000
17: .word 0x60004018
18: .word 1500000
19: .word 0x60006004
#if defined(LINNET_PLATFORM_CORE)
ln_cop_stack_address: .word 0x40003000
ln_cop_mailbox_address: .word LINNET_DISPLAY_MAILBOX
ln_cop_capability_value: .word LINNET_DISPLAY_CAPABILITY

#if LINNET_COP_DISPLAY
#include LINNET_COP_DISPLAY_SOURCE
#else
/* Other boards use the resident watchdog and clock rendezvous only. */
ln_cop_display_step:
    stmdb sp!, {r0-r2,lr}
    ldr r0, ln_cop_mailbox_address
    ldr r1, [r0, #LN_DISPLAY_PROBE]
    str r1, [r0, #LN_DISPLAY_PROBE_ACK]
    ldr r1, [r0, #LN_DISPLAY_PAUSE]
    cmp r1, #2
    beq 8f
    cmp r1, #0
    movne r1, #1
    str r1, [r0, #LN_DISPLAY_PAUSED]
    ldmia sp!, {r0-r2,pc}
8:  str r1, [r0, #LN_DISPLAY_PAUSED]
    ldr r2, =0x60007004
    mov r1, #0x80000000
    str r1, [r2]
    nop
    nop
    nop
    mov r1, #0
    str r1, [r0, #LN_DISPLAY_PAUSED]
    ldmia sp!, {r0-r2,pc}
    .ltorg
#endif

#endif
#if defined(LINNET_PLATFORM_CORE)
/* The Apple payload starts on both PortalPlayer cores. The CPU publishes this
 * request only after copying the authenticated image and removing low mappings.
 * Keep the check outside the fixed-offset monitor prologue used by the OS. */
ln_cop_retail_step:
    ldr r1, [r0, #164]
    ldr r3, ln_cop_retail_magic
    cmp r1, r3
    bne ln_cop_display_step
    ldr r1, ln_cop_cache_control
    ldr r3, [r1]
    tst r3, #1
    beq 21f
    ldr r1, ln_cop_cache_flush
    ldr r3, [r1]
    orr r3, r3, #6
    str r3, [r1]
#if LINNET_RETAIL_COP_ACK
    ldr r3, ln_cop_cache_wait_budget
20: ldr r1, ln_cop_cache_control
    ldr r1, [r1]
    tst r1, #0x8000
    beq 22f
    subs r3, r3, #1
    bne 20b
23: b 23b
22:
#endif
    ldr r1, ln_cop_cache_control
    mov r3, #0
    str r3, [r1]
21:
#if LINNET_RETAIL_COP_ACK
    ldr r1, ln_cop_retail_ack_address
    ldr r3, ln_cop_retail_ack_value
    str r3, [r1]
#endif
    ldr r1, ln_cop_retail_entry
    bx r1
ln_cop_retail_magic: .word 0x4c4e4150
ln_cop_cache_control: .word 0x6000c000
ln_cop_cache_flush: .word 0xf000f044
ln_cop_retail_entry: .word 0x10000000
#if LINNET_RETAIL_COP_ACK
ln_cop_retail_ack_address: .word 0x11ffe0a8
ln_cop_retail_ack_value: .word 0x4c4e4152
ln_cop_cache_wait_budget: .word 0x100000
#endif
#endif
ln_cop_end:
    .ltorg
#endif
