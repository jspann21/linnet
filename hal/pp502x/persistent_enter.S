.syntax unified
.cpu arm7tdmi
.arm
#include "linnet_target.h"
.section .text.persistent_enter,"ax",%progbits
.align 2
.global ln_persistent_enter_begin
.global ln_persistent_enter_end
/* IRAM-resident copy after verified persistent selection; no development API. */
ln_persistent_enter_begin:
    msr cpsr_c, #0xd3
1:  cmp r1, #4
    ldrhs r4, [r0], #4
    strhs r4, [r2], #4
    subhs r1, r1, #4
    bhs 1b
2:  cmp r1, #0
    ldrbne r4, [r0], #1
    strbne r4, [r2], #1
    subne r1, r1, #1
    bne 2b
    bx r3
ln_persistent_enter_end:

.align 2
.global ln_retail_enter_begin
.global ln_retail_enter_end
/* Apple firmware expects the ROM's unmapped SDRAM state. Execute entirely
 * from IRAM while removing Linnet's low SDRAM alias. */
ln_retail_enter_begin:
    msr cpsr_c, #0xd3
3:  cmp r1, #4
    ldrhs r4, [r0], #4
    strhs r4, [r2], #4
    subhs r1, r1, #4
    bhs 3b
4:  cmp r1, #0
    ldrbne r4, [r0], #1
    strbne r4, [r2], #1
    subne r1, r1, #1
    bne 4b
    ldr r0, 6f
    mov r1, #0
5:  str r1, [r0], #4
    ldr r4, 7f
    cmp r0, r4
    blo 5b
    /* The resident COP branches to the same verified Apple entry. Clear on
     * fresh COP startup prevents a stale request surviving a later reset. */
#if LINNET_RETAIL_COP_ACK
    ldr r0, 11f
    mov r1, #0
    str r1, [r0]
#endif
    ldr r0, 8f
    ldr r1, 9f
    str r1, [r0]
    ldr r0, 10f
    mov r1, #0
    str r1, [r0]
#if LINNET_RETAIL_COP_ACK
    /* Apple reuses IRAM during startup. Wait until the resident COP has
     * stopped using its Linnet code before letting the CPU enter Apple. */
    ldr r0, 11f
    ldr r1, 12f
    ldr r2, 13f
14: ldr r4, [r0]
    cmp r4, r1
    beq 15f
    subs r2, r2, #1
    bne 14b
    ldr r0, 16f
    ldr r1, [r0]
    orr r1, r1, #4
    str r1, [r0]
17: b 17b
15:
#endif
    bx r3
6:  .word 0xf000f000
7:  .word 0xf000f040
8:  .word 0x11ffe0a4
9:  .word 0x4c4e4150
10: .word 0x60007004
#if LINNET_RETAIL_COP_ACK
11: .word 0x11ffe0a8
12: .word 0x4c4e4152
13: .word 0x100000
16: .word 0x60006004
#endif
ln_retail_enter_end:
