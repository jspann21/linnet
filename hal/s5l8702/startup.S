.syntax unified
.cpu arm926ej-s
.arm
.section .vectors,"ax",%progbits
.global ln_start
ln_start:
    .rept 8
    ldr pc,[pc,#24]
    .endr
    .word ln_platform_reset,ln_undefined,ln_software_interrupt,ln_prefetch_abort
    .word ln_data_abort,ln_reserved,ln_s5l_irq_entry,ln_core_watchdog_fiq
.section .text.startup,"ax",%progbits
.global ln_reset_continue
ln_reset_continue:
    msr cpsr_c,#0xd7
    ldr sp,=__abort_stack
    msr cpsr_c,#0xdb
    ldr sp,=__undef_stack
    msr cpsr_c,#0xd2
    ldr sp,=__irq_stack
    msr cpsr_c,#0xd1
    ldr sp,=__fiq_stack
    msr cpsr_c,#0xd3
    ldr sp,=__stack_top
    mov r0,#1000
    bl ln_s5l_watchdog_timer
    mov r0,#0
    bl ln_s5l_watchdog_route
    ldr r0,=__bss_start
    ldr r1,=__bss_end
    mov r2,#0
1:  cmp r0,r1
    strlo r2,[r0],#4
    blo 1b
#if defined(LINNET_IMAGE_BOOT)
    /* Boot has no runtime executive to establish IRQ and SDRAM aliases. */
    bl ln_core_init
    cmp r0,#0
    beq 4f
    bl ln_core_cache_enable
    cmp r0,#0
    beq 4f
#endif
    bl ln_main
2:  b 2b
#if defined(LINNET_IMAGE_BOOT)
4:  mov r0,#30
    mov r1,#0
    mov r2,#1
    bl ln_panic
    b 2b
#endif
.macro fault name,reason,adjust
\name:
    sub r1,lr,#\adjust
    mrs r2,spsr
    mov r0,#\reason
    bl ln_panic
    b 2b
.endm
fault ln_undefined,1,4
fault ln_software_interrupt,2,4
fault ln_prefetch_abort,3,4
fault ln_data_abort,4,8
fault ln_reserved,5,4
.global ln_core_watchdog_fiq
ln_core_watchdog_fiq:
    /* Retain a small SRAM record without stack, SDRAM, or peripheral waits. */
    ldr r8,=0x22017b10
    mov r9,#0
    str r9,[r8]
    sub r9,lr,#4
    str r9,[r8,#4]
    mrs r10,spsr
    str r10,[r8,#8]
    eor r9,r9,r10
    ldr r11,=0x22017e04
    ldr r10,[r11],#4
    str r10,[r8,#16]
    eor r9,r9,r10
    add r12,r8,#20
    mov lr,#8
4:  ldr r10,[r11],#4
    str r10,[r12],#4
    eor r9,r9,r10
    subs lr,lr,#1
    bne 4b
    mvn r9,r9
    str r9,[r8,#12]
    ldr r9,=0x3246574c
    str r9,[r8]
    mov r9,#0
    mcr p15,0,r9,c7,c10,4
    ldr r8,=0x3c800000
    mov r9,#0x100000
    str r9,[r8]
3:  b 3b
