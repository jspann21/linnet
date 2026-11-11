.syntax unified
.cpu arm926ej-s
.arm
.section .text.start,"ax",%progbits
.global ln_start
ln_start:
    .rept 8
    ldr pc,[pc,#24]
    .endr
    .word ln_bootstrap_start,ln_bootstrap_exception,ln_bootstrap_exception,ln_bootstrap_exception
    .word ln_bootstrap_exception,ln_bootstrap_exception,ln_s5l_irq_entry,ln_bootstrap_fiq
ln_bootstrap_start:
    msr cpsr_c, #0xd2
    ldr sp, =ln_s5l_irq_stack_end
    msr cpsr_c, #0xd3
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c10, 4
    mrc p15, 0, r1, c1, c0, 0
    bic r1, r1, #0x1000
    bic r1, r1, #0x2000
    bic r1, r1, #5
    mcr p15, 0, r1, c1, c0, 0
    ldr sp, =0x2201fff0
    ldr r1, =__bss_start
    ldr r2, =__bss_end
1:  cmp r1, r2
    strlo r0, [r1], #4
    blo 1b
    bl ln_bootstrap_main
ln_bootstrap_exception:
2:  b 2b
ln_bootstrap_fiq:
    ldr r8,=0x3c800000
    mov r9,#0x100000
    str r9,[r8]
3:  b 3b
