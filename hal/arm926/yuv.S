.syntax unified
.cpu arm926ej-s
.arm
/* Exact BT.601 RGB565 conversion; planar or interleaved chroma. */
.macro pixel chroma_step, mapped=0
    .if \mapped
    ldrh r3,[lr],#2
    ldrb r0,[r4,r3]
    .if \chroma_step == 2
    bic r3,r3,#1
    .else
    mov r3,r3,lsr #1
    .endif
    ldrb r1,[r5,r3]
    ldrb r2,[r6,r3]
    .else
    ldrb r0,[r4],#2
    ldrb r1,[r5],#\chroma_step
    ldrb r2,[r6],#\chroma_step
    .endif
    sub r0,r0,#16
    sub r1,r1,#128
    sub r2,r2,#128
    smulbb r0,r0,r10
    add r0,r0,#128
    add r12,r1,r1,lsl #7
    add r12,r0,r12,lsl #2
    smlabb r3,r2,r11,r0
    smlabt r0,r1,r10,r0
    smlabt r0,r2,r11,r0
    mov r12,r12,asr #8
    mov r3,r3,asr #8
    mov r0,r0,asr #8
    ldrb r12,[r9,r12]
    ldrb r3,[r9,r3]
    ldrb r0,[r9,r0]
    and r3,r3,#248
    and r0,r0,#252
    mov r3,r3,lsl #8
    orr r3,r3,r0,lsl #3
    orr r0,r3,r12,lsr #3
.endm
.macro half_row name, chroma_step
.section .text.\name,"ax",%progbits
.global \name
.type \name,%function
\name:
    push {r4-r11,lr}
    mov r4,r0
    mov r5,r1
    mov r6,r2
    mov r7,r3
    ldr r8,[sp,#36]
    ldr r9,[sp,#40]
    ldr r10,=0xff9c012a  /* -100, 298 */
    ldr r11,=0xff300199  /* -208, 409 */

1:
    tst r8,#15
    bne 2f
    pld [r7,#32]
    pld [r4,#64]
    pld [r5,#32]
    pld [r6,#32]
2:
    pixel \chroma_step
    mov lr,r0
    pixel \chroma_step
    orr r0,lr,r0,lsl #16
    str r0,[r7],#4
    subs r8,r8,#2
    bne 1b
    pop {r4-r11,pc}
.size \name,.-\name
.ltorg
.endm
half_row ln_arm926_yuv_half_row, 1
half_row ln_arm926_nv12_half_row, 2

.macro mapped_row name, chroma_step
.section .text.\name,"ax",%progbits
.global \name
.type \name,%function
\name:
    push {r4-r11,lr}
    sub sp,sp,#4
    mov r4,r0
    mov r5,r1
    mov r6,r2
    mov r7,r3
    ldr r8,[sp,#40]
    ldr r9,[sp,#44]
    ldr lr,[sp,#48]
    ldr r10,=0xff9c012a
    ldr r11,=0xff300199
    cmp r8,#0
    beq 4f
    tst r7,#2
    beq 2f
    pixel \chroma_step, 1
    strh r0,[r7],#2
    sub r8,r8,#1
2:
    cmp r8,#2
    blo 3f
    pld [r7,#32]
    pixel \chroma_step, 1
    str r0,[sp]
    pixel \chroma_step, 1
    ldr r3,[sp]
    orr r0,r3,r0,lsl #16
    str r0,[r7],#4
    sub r8,r8,#2
    b 2b
3:
    cmp r8,#0
    beq 4f
    pixel \chroma_step, 1
    strh r0,[r7]
4:
    add sp,sp,#4
    pop {r4-r11,pc}
.size \name,.-\name
.ltorg
.endm
mapped_row ln_arm926_yuv_mapped_row, 1
mapped_row ln_arm926_nv12_mapped_row, 2
