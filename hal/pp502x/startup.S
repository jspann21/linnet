.syntax unified
.cpu arm7tdmi
.arm
#include "hal/api/recovery.h"
#include "hal/pp502x/recovery_internal.h"
.section .vectors,"ax",%progbits
.global ln_start
ln_start:
    b ln_platform_reset
    b ln_undefined
    b ln_software_interrupt
    b ln_prefetch_abort
    b ln_data_abort
    b ln_reserved
#if defined(LINNET_PLATFORM_CORE)
    b ln_core_irq
#else
    b ln_irq_fault
#endif
#if defined(LINNET_PROFILE_RELEASE) && defined(LINNET_PLATFORM_CORE)
    b ln_core_watchdog_fiq
#elif defined(LINNET_PROFILE_RELEASE)
    b ln_fiq_fault
#else
    b ln_watchdog_fiq
#endif

.section .text.startup,"ax",%progbits
.global ln_reset_continue
ln_reset_continue:
    msr cpsr_c, #0xd7
    ldr sp, =__abort_stack
    msr cpsr_c, #0xdb
    ldr sp, =__undef_stack
    msr cpsr_c, #0xd2
    ldr sp, =__irq_stack
    msr cpsr_c, #0xd1
    ldr sp, =__fiq_stack
    msr cpsr_c, #0xd3
    ldr sp, =__stack_top
#if defined(LINNET_IMAGE_OS)
    /* Cold Boot still runs at its inherited clock. Clearing the media BSS
     * can exceed the COP lease there, although a warm80MHz RAM boot passes.
     * Renew only after each completed1KiB block, never while a store stalls. */
    ldr r4, =0x60005010
    ldr r5, =0x11ffe010
    mov r3, #256
#if !defined(LINNET_PROFILE_RELEASE)
    mov r6, #0x41
    str r6, [r5, #112]
#endif
#endif
    #if defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    LN_STARTUP_STAMP 4, 16
    #endif
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
#if defined(LINNET_IMAGE_OS)
    /* Eight ordered word stores amortize the cold uncached loop overhead.
     * r4/r5 retain the timer/control addresses; none of the zero registers
     * alias either address or the advancing destination. The linker aligns
     * both BSS bounds, so the final short remainder is still whole words. */
    mov r7, #0
    mov r8, #0
    mov r9, #0
    mov r10, #0
    mov r11, #0
    mov r12, #0
    mov lr, #0
    sub r6, r1, r0
3:  cmp r6, #32
    blo 7f
    stmia r0!, {r2, r7-r12, lr}
    sub r6, r6, #32
    subs r3, r3, #8
    bne 3b
    /* Renew only after all 256 words completed, never inside STM/waits. */
    ldr r1, [r4]
    str r1, [r5]
    mov r3, #256
    b 3b
7:  cmp r6, #0
    beq 5f
8:  str r2, [r0], #4
    subs r6, r6, #4
    bne 8b
#else
3:  cmp r0, r1
    bhs 5f
    str r2, [r0], #4
    b 3b
#endif
5:
#if defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    LN_STARTUP_STAMP 5, 32
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_IMAGE_OS)
    /* The verified payload contains the IRAM load image in low SDRAM.
     * Reset already disabled cache and IRQ/FIQ; never execute hot code until
     * this complete word copy has finished. COP and USB regions are separate. */
    ldr r0, =__audio_icode_load
    ldr r1, =__audio_icode_begin
    ldr r2, =__audio_icode_end
    mov r6, #256
#if !defined(LINNET_PROFILE_RELEASE)
    mov r3, #0x43
    str r3, [r5, #112]
#endif
4:  cmp r1, r2
    bhs 6f
    ldr r3, [r0], #4
    str r3, [r1], #4
    subs r6, r6, #1
    bne 4b
    ldr r3, [r4]
    str r3, [r5]
    mov r6, #256
    b 4b
6:
#endif
#if defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    LN_STARTUP_STAMP 6, 64
    ldr r0, =0x11ffe000
    mov r1, #0x42
    str r1, [r0, #128]
    LN_STARTUP_STAMP 7, 128
#endif
    bl ln_main
.global ln_early_stop
ln_early_stop:
    b ln_early_stop

.macro LINNET_FAULT name, number, adjustment
\name:
    sub r1, lr, #\adjustment
    mrs r2, spsr
    mov r0, #\number
    bl ln_panic
    b ln_early_stop
.endm
LINNET_FAULT ln_undefined, 1, 4
LINNET_FAULT ln_software_interrupt, 2, 4
LINNET_FAULT ln_prefetch_abort, 3, 4
LINNET_FAULT ln_data_abort, 4, 8
LINNET_FAULT ln_reserved, 5, 4
LINNET_FAULT ln_irq_fault, 6, 4
LINNET_FAULT ln_fiq_fault, 7, 4
