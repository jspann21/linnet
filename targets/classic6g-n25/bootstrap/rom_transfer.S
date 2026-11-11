/* Temporary BootROM USB-handler payload. No NOR or disk writes.
 * Parameters at DFU buffer start: operation, address, length, then data.
 * Protocol/ROM entry addresses: freemyipod wInd3x S5L8702 research.
 */
.syntax unified
.cpu arm926ej-s
.arm
.section .text,"ax",%progbits
.global ln_rom_transfer
ln_rom_transfer:
    ldr r3, =0x22028220
    ldr r0, [r3]
    cmp r0, #1
    beq copy_sram
    cmp r0, #2
    beq execute_sram
    ldr r0, [r3, #4]
    b respond
copy_sram:
    ldr r0, [r3, #4]
    ldr r1, [r3, #8]
    add r2, r3, #12
    ldr r3, =0x22000000
    cmp r0, r3
    blo reject
    add r12, r0, r1
    ldr r3, =0x22018000
    cmp r12, r3
    bhi reject
    cmp r1, #0
    beq reject
    cmp r1, #0x200
    bhi reject
1:  ldrb r3, [r2], #1
    strb r3, [r0], #1
    subs r1, r1, #1
    bne 1b
    ldr r3, =0x22028220
    ldr r0, [r3, #4]
    mov r1, #0
    mcr p15, 0, r1, c7, c10, 4
    b respond
execute_sram:
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c10, 4
    ldr pc, =0x22000000
reject:
    ldr r0, =0x20000000
respond:
    mov r1, #64
    ldr r2, =0x2000aa40
    blx r2
    ldr lr, =0x200048d4
    bx lr
    .ltorg
