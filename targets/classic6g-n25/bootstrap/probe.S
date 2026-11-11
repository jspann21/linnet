.syntax unified
.cpu arm926ej-s
.arm
.section .text.start,"ax",%progbits
.global ln_start
ln_start:
    stmdb sp!, {r4-r11,r12,lr}
    mrs r4,cpsr
    orr r0,r4,#0xc0
    msr cpsr_c,r0
    /* Keep the ROM's USB state and stack; only disable instruction caching. */
    mrc p15,0,r0,c1,c0,0
    bic r0,r0,#0x1000
    mcr p15,0,r0,c1,c0,0
    mov r0,#0
    mcr p15,0,r0,c7,c5,0
    ldr r0,=0x2202822c
    bl ln_board_probe
    msr cpsr_c,r4
    mov r1,#64
    ldr r2,=0x2000aa40
    blx r2
    ldmia sp!, {r4-r11,r12,lr}
    ldr lr,=0x200048d4
    bx lr
    .ltorg
