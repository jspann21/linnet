.syntax unified
.cpu arm7tdmi
.arm
.section .text.ln_video_xor_rgb565,"ax",%progbits
.global ln_video_xor_rgb565
.type ln_video_xor_rgb565,%function
/* r0=destination, r1=native residual, r2=native prediction,
 * r3=pixels. Internal full-frame path: all buffers are word aligned,
 * prediction is disjoint; destination may equal residual. The pixel count
 * is a multiple of eight.
 * Read each input and write each output once, in four-word bursts. */
ln_video_xor_rgb565:
    cmp r3, #0
    bxeq lr
    push {r4-r11, lr}
.Lxor_blocks:
    ldmia r1!, {r4-r7}
    ldmia r2!, {r8-r11}
    eor r4, r4, r8
    eor r5, r5, r9
    eor r6, r6, r10
    eor r7, r7, r11
    stmia r0!, {r4-r7}
    subs r3, r3, #8
    bne .Lxor_blocks
    pop {r4-r11, lr}
    bx lr
.size ln_video_xor_rgb565, .-ln_video_xor_rgb565
.section .note.GNU-stack,"",%progbits
