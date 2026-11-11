.syntax unified
.cpu arm7tdmi
.arm
#include "hal/api/display.h"
#include "hal/pp502x/display_internal.h"
.section .text.ln_video_blit_blob,"ax",%progbits
.balign 4
.global ln_video_blit_begin
.global ln_video_blit_end
/* Entered by the verified old worker's state-3 BEQ. That worker has already
 * saved {r0-r7,lr}, checked deadline, and charged one work-budget unit.
 * Never call SDRAM code or use registers r8-r12. Return through its saved PC.
 * Each invocation writes <=32 pairs and never crosses a column boundary. */
ln_video_blit_begin:
    ldr r5, [r0, #LN_DISPLAY_HEIGHT]
    ldr r6, [r0, #LN_DISPLAY_Y_CURSOR]
    sub r5, r5, r6
    mov r5, r5, lsr #1
    cmp r5, #32
    movhi r5, #32
    ldr r7, [r0, #LN_DISPLAY_PAIRS_LEFT]
    cmp r5, r7
    movhi r5, r7
    ldr r7, [r0, #LN_DISPLAY_BUDGET]
    add r7, r7, #1
    cmp r5, r7
    movhi r5, r7
    cmp r5, #0
    beq .Lreturn
    ldr r1, [r0, #LN_DISPLAY_X]
    ldr r7, [r0, #LN_DISPLAY_WIDTH]
    add r1, r1, r7
    rsb r1, r1, #LINNET_LCD_WIDTH_ASM
    ldr r7, [r0, #LN_DISPLAY_X_CURSOR]
    add r1, r1, r7
    mov r7, #LINNET_LCD_HEIGHT_ASM
    mul r2, r1, r7
    ldr r7, [r0, #LN_DISPLAY_Y]
    add r2, r2, r7
    add r2, r2, r6
    ldr r1, [r0, #LN_DISPLAY_BUFFER]
    add r1, r1, r2, lsl #1
    ldr r2, .Lcontrol
    ldr r3, .Ldata
    ldr r4, .Lmask
    mov lr, #0
.Lpair:
    /* Brief FIFO backpressure is cheaper to retry here than to re-enter
     * the monitor and rebuild the source address. Bound each retry so a
     * stalled LCD still returns to the lease/deadline checks. */
    mov r7, #32
.Lpair_wait:
    ldr r6, [r2]
    tst r6, #0x01000000
    bne .Lpair_ready
    subs r7, r7, #1
    bne .Lpair_wait
    b .Lcommit
.Lpair_ready:
    ldr r6, [r1], #4
    mov r7, r6, lsr #8
    and r7, r7, r4
    and r6, r6, r4
    orr r6, r7, r6, lsl #8
    str r6, [r3]
    add lr, lr, #1
    subs r5, r5, #1
    bne .Lpair
.Lcommit:
    cmp lr, #0
    beq .Lreturn
    ldr r1, [r0, #LN_DISPLAY_Y_CURSOR]
    add r1, r1, lr, lsl #1
    ldr r2, [r0, #LN_DISPLAY_HEIGHT]
    cmp r1, r2
    moveq r1, #0
    ldreq r2, [r0, #LN_DISPLAY_X_CURSOR]
    addeq r2, r2, #1
    streq r2, [r0, #LN_DISPLAY_X_CURSOR]
    str r1, [r0, #LN_DISPLAY_Y_CURSOR]
    ldr r1, [r0, #LN_DISPLAY_PAIRS]
    add r1, r1, lr
    str r1, [r0, #LN_DISPLAY_PAIRS]
    ldr r1, [r0, #LN_DISPLAY_BUDGET]
    add r1, r1, #1
    sub r1, r1, lr
    str r1, [r0, #LN_DISPLAY_BUDGET]
    ldr r1, [r0, #LN_DISPLAY_PAIRS_LEFT]
    subs r1, r1, lr
    str r1, [r0, #LN_DISPLAY_PAIRS_LEFT]
    moveq r1, #4
    streq r1, [r0, #LN_DISPLAY_STATE]
.Lreturn:
    ldmia sp!, {r0-r7,pc}
.Lcontrol: .word 0x70008a20
.Ldata: .word 0x70008b00
.Lmask: .word 0x00ff00ff
.global ln_video_command_begin
ln_video_command_begin:
    ldr r2, .Lport
    ldr r3, [r2]
    tst r3, #0x80000000
    bne .Lreturn
    ldr r1, [r0, #LN_DISPLAY_COMMAND]
    cmp r1, #0
    moveq r3, #0x18
    beq .Lcommand_send
    cmp r1, #1
    moveq r3, #0
    beq .Lcommand_send
    cmp r1, #2
    moveq r3, #0x12
    beq .Lcommand_send
    cmp r1, #3
    ldreq r3, [r0, #LN_DISPLAY_Y]
    beq .Lcommand_send
    cmp r1, #4
    moveq r3, #0x13
    beq .Lcommand_send
    cmp r1, #5
    ldreq r3, [r0, #LN_DISPLAY_X]
    ldreq r4, [r0, #LN_DISPLAY_WIDTH]
    addeq r3, r3, r4
    rsbeq r3, r3, #LINNET_LCD_WIDTH_ASM
    beq .Lcommand_send
    cmp r1, #6
    moveq r3, #0x15
    beq .Lcommand_send
    cmp r1, #7
    ldreq r3, [r0, #LN_DISPLAY_Y]
    ldreq r4, [r0, #LN_DISPLAY_HEIGHT]
    addeq r3, r3, r4
    subeq r3, r3, #1
    beq .Lcommand_send
    cmp r1, #8
    moveq r3, #0x16
    beq .Lcommand_send
    ldr r3, [r0, #LN_DISPLAY_X]
    rsb r3, r3, #(LINNET_LCD_WIDTH_ASM - 1)
.Lcommand_send:
    orr r3, r3, #0x80000000
    str r3, [r2]
    add r1, r1, #1
    str r1, [r0, #LN_DISPLAY_COMMAND]
    cmp r1, #10
    moveq r1, #2
    streq r1, [r0, #LN_DISPLAY_STATE]
    b .Lreturn
.Lport: .word 0x70008a0c
ln_video_blit_end:
