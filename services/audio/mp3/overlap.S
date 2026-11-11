.syntax unified
.arm
.section .text.ln_mp3_overlap,"ax",%progbits
.align 2
.global ln_mp3_overlap
.type ln_mp3_overlap,%function
ln_mp3_overlap:
    add r2,r2,r3,lsl #2
    cmp r0,#0
    beq .Lzero
    push {r4-r11}
    add r12,r0,#72
    tst r3,#1
    .rept 3
    ldmia r1,{r4-r9}
    ldmia r0!,{r10,r11}
    add r4,r4,r10
    add r5,r5,r11
    rsbne r5,r5,#0
    str r4,[r2],#128
    str r5,[r2],#128
    ldmia r0!,{r10,r11}
    add r6,r6,r10
    add r7,r7,r11
    rsbne r7,r7,#0
    str r6,[r2],#128
    str r7,[r2],#128
    ldmia r0!,{r10,r11}
    add r8,r8,r10
    add r9,r9,r11
    rsbne r9,r9,#0
    str r8,[r2],#128
    str r9,[r2],#128
    ldmia r12!,{r4-r9}
    stmia r1!,{r4-r9}
    .endr
    pop {r4-r11}
    bx lr
.Lzero:
    push {r4,r5}
    mov r12,#0
    tst r3,#1
    .rept 9
    ldmia r1,{r4,r5}
    rsbne r5,r5,#0
    str r4,[r2],#128
    str r5,[r2],#128
    stmia r1!,{r0,r12}
    .endr
    pop {r4,r5}
    bx lr
.size ln_mp3_overlap,.-ln_mp3_overlap
