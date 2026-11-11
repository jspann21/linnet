.syntax unified
.cpu arm926ej-s
.arm
.section .text.ln_sram_enter,"ax",%progbits
.global ln_sram_enter_start,ln_sram_enter_end
/* Relocated to reserved SRAM before overwriting the running monitor.
 * r0 source, r1 destination, r2 bytes, r3 entry. No stack or absolute data. */
ln_sram_enter_start:
    msr cpsr_c,#0xd3
1:  ldrb r12,[r0],#1
    strb r12,[r1],#1
    subs r2,r2,#1
    bne 1b
    mov r0,#0
    mcr p15,0,r0,c7,c10,4
    mcr p15,0,r0,c7,c5,0
    nop
    nop
    nop
    bx r3
ln_sram_enter_end:
