/* RAM-only bridge for entering the Classic monitor from ordinary Linnet Link.
 * The outer Linnet image is loaded at the normal SDRAM address. This stub
 * copies the embedded, independently hashed monitor to SRAM and enters it.
 */
.syntax unified
.cpu arm926ej-s
.arm
.section .text,"ax",%progbits
.global ln_sram_bridge
ln_sram_bridge:
    msr cpsr_c, #0xd3
    adr r0, monitor_start
    ldr r1, =monitor_end-monitor_start
    ldr r2, =0x22000000
1:  cmp r1, #4
    ldrhs r3, [r0], #4
    strhs r3, [r2], #4
    subhs r1, r1, #4
    bhs 1b
2:  cmp r1, #0
    ldrbne r3, [r0], #1
    strbne r3, [r2], #1
    subne r1, r1, #1
    bne 2b
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
    mcr p15, 0, r0, c7, c5, 0
    ldr pc, =0x22000000
    .ltorg
    .balign 4
monitor_start:
    .incbin LINNET_BOOTSTRAP_MONITOR_PATH
monitor_end:
