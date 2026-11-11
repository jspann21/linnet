.syntax unified
.cpu arm926ej-s
.arm
.text
.global ln_platform_reset
ln_platform_reset:
    msr cpsr_c,#0xd3
    mov r0,#0
    mcr p15,0,r0,c7,c10,4
    mcr p15,0,r0,c7,c5,0
    mrc p15,0,r1,c1,c0,0
    bic r1,r1,#5
    bic r1,r1,#0x2000
    orr r1,r1,#0x1000
    mcr p15,0,r1,c1,c0,0
    b 1f
1:  ldr r0,=ln_start
    ldr r1,=0x22000000
    mov r2,#16
2:  ldr r3,[r0],#4
    str r3,[r1],#4
    subs r2,r2,#1
    bne 2b
    mov r0,#0
    mcr p15,0,r0,c7,c10,4
    mcr p15,0,r0,c7,c5,0
    b ln_reset_continue
