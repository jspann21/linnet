.syntax unified
.cpu arm7tdmi
.arm
.section .text.recovery,"ax",%progbits
.global ln_watchdog_fiq
ln_watchdog_fiq:
    sub lr, lr, #4
    stmdb sp!, {r0-r12, lr}
    mrs r1, spsr
    stmdb sp!, {r1,r2}
    mov r0, lr
    bl ln_recovery_watchdog_interrupt
    ldmia sp!, {r1,r2}
    msr spsr_cxsf, r1
    ldmia sp!, {r0-r12, lr}
    movs pc, lr
.align 2
.global ln_copy_trampoline_begin
.global ln_copy_trampoline_end
/* Position independent IRAM copy: source r0, bytes r1, dest r2, entry r3. */
ln_copy_trampoline_begin:
    msr cpsr_c, #0xd3
    ldr r4, 3f
    mov r5, #0x31
    str r5, [r4, #128]
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
    ldr r0, 3f
    mov r1, #0x32
    str r1, [r0, #128]
    ldr r1, 4f
    ldr r1, [r1]
    str r1, [r0, #16]
    mov r1, #1
    str r1, [r0, #20]
    nop
    nop
    nop
    bx r3
3:  .word 0x11ffe000
4:  .word 0x60005010
ln_copy_trampoline_end:
