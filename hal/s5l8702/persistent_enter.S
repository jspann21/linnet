.syntax unified
.cpu arm926ej-s
.arm
.text
.global ln_persistent_enter_begin,ln_persistent_enter_end
/* SRAM trampoline: source, length, destination, entry. */
ln_persistent_enter_begin:
    msr cpsr_c,#0xd3
1:  cmp r1,#4
    ldrhs r12,[r0],#4
    strhs r12,[r2],#4
    subhs r1,r1,#4
    bhs 1b
2:  cmp r1,#0
    ldrbne r12,[r0],#1
    strbne r12,[r2],#1
    subne r1,r1,#1
    bne 2b
    mov r0,#0
    mcr p15,0,r0,c7,c10,4
    mcr p15,0,r0,c7,c5,0
    bx r3
ln_persistent_enter_end:
