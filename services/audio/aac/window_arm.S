.syntax unified
.arm
.text
.align 2
.global ln_aac_window_add
.type ln_aac_window_add,%function
ln_aac_window_add:
    push {r4-r11,lr}
    ldr lr,[sp,#36]
    cmp lr,#4
    blo 2f
1:
    ldmia r1!,{r4-r7}
    ldmia r2!,{r8-r11}
    smull r12,r8,r4,r8
    smull r12,r9,r5,r9
    smull r12,r10,r6,r10
    smull r12,r11,r7,r11
    ldmia r3!,{r4-r7}
    add r4,r4,r8,lsl #1
    add r5,r5,r9,lsl #1
    add r6,r6,r10,lsl #1
    add r7,r7,r11,lsl #1
    stmia r0!,{r4-r7}
    sub lr,lr,#4
    cmp lr,#4
    bhs 1b
2:
    cmp lr,#0
    beq 4f
3:
    ldr r4,[r1],#4
    ldr r5,[r2],#4
    smull r12,r6,r4,r5
    ldr r4,[r3],#4
    add r4,r4,r6,lsl #1
    str r4,[r0],#4
    subs lr,lr,#1
    bne 3b
4:
    pop {r4-r11,lr}
    bx lr
.size ln_aac_window_add,.-ln_aac_window_add

.global ln_aac_window_reverse
.type ln_aac_window_reverse,%function
ln_aac_window_reverse:
    push {r4-r11,lr}
    add r2,r2,r3,lsl #2
    cmp r3,#4
    blo 2f
1:
    ldmia r1!,{r4-r7}
    ldmdb r2!,{r8-r11}
    smull r12,r11,r4,r11
    smull r12,r10,r5,r10
    smull r12,r9,r6,r9
    smull r12,r8,r7,r8
    mov r4,r11,lsl #1
    mov r5,r10,lsl #1
    mov r6,r9,lsl #1
    mov r7,r8,lsl #1
    stmia r0!,{r4-r7}
    sub r3,r3,#4
    cmp r3,#4
    bhs 1b
2:
    cmp r3,#0
    beq 4f
3:
    ldr r4,[r1],#4
    ldr r5,[r2,#-4]!
    smull r12,r6,r4,r5
    mov r4,r6,lsl #1
    str r4,[r0],#4
    subs r3,r3,#1
    bne 3b
4:
    pop {r4-r11,lr}
    bx lr
.size ln_aac_window_reverse,.-ln_aac_window_reverse
