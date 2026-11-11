/* Linnet fixed-point audio kernels. */
.syntax unified
.cpu arm7tdmi
.arm
.text
/* Direct-form biquad from dsp.c: five signed products, one Q30 rounding,
 * then a Q8 headroom clamp. Histories advance only after their products.
 * r0=coefficients, r1=history, r2=samples, r3=count; stack=stride,clips.
 * The internal caller supplies stride 1 or 2 and stable coefficients.
 * One end-pointer loop handles both layouts without duplicating the kernel.
 */
.align 2
.global ln_dsp_filter_arm
.type ln_dsp_filter_arm,%function
ln_dsp_filter_arm:
    cmp r3,#0
    bxeq lr
    push {r4-r11,lr}
    sub sp,sp,#12
    str r1,[sp]
    ldr r12,[sp,#52]
    str r12,[sp,#4]
    ldm r0,{r4-r8}
    /* Stable coefficients exclude INT_MIN, so feedback negation is exact. */
    rsb r7,r7,#0
    rsb r8,r8,#0
    ldm r1,{r9-r12}
    mov r0,r2
    ldr r2,[sp,#48]
    mul r1,r2,r3
    add r1,r0,r1,lsl #2
    mov r2,r2,lsl #2
.Lbiquad:
    smull lr,r3,r6,r10
    smlal lr,r3,r5,r9
    mov r10,r9
    ldr r9,[r0]
    smlal lr,r3,r4,r9
    smlal lr,r3,r8,r12
    mov r12,r11
    smlal lr,r3,r7,r11
    /* Bias 2^29 for nonnegative sums, 2^29-1 for negative sums. */
    mov r11,r3,lsr #31
    rsb r11,r11,#0x20000000
    adds lr,lr,r11
    adc r3,r3,#0
    mov r11,lr,lsr #30
    orr r11,r11,r3,lsl #2
    cmp r11,#0x08000000
    bgt .Lbiquad_high
    cmn r11,#0x08000000
    bge .Lbiquad_store
    mov r11,#0xf8000000
    b .Lbiquad_clip
.Lbiquad_high:
    mov r11,#0x08000000
.Lbiquad_clip:
    ldr lr,[sp,#4]
    ldr r3,[lr]
    cmn r3,#1
    addne r3,r3,#1
    str r3,[lr]
.Lbiquad_store:
    str r11,[r0],r2
    cmp r0,r1
    bne .Lbiquad
    ldr r0,[sp]
    stm r0,{r9-r12}
    add sp,sp,#12
    pop {r4-r11,lr}
    bx lr
.size ln_dsp_filter_arm,.-ln_dsp_filter_arm

/* Round a signed 64-bit accumulator by 16 with symmetric ties away. */
.macro ROUND16 destination,lo,hi,scratch
    mov \scratch,\hi,lsr #31
    rsb \scratch,\scratch,#0x8000
    adds \lo,\lo,\scratch
    adc \hi,\hi,#0
    mov \destination,\lo,lsr #16
    orr \destination,\destination,\hi,lsl #16
.endm

/* r0=interleaved Q8 samples, r1=frames, r2=two histories, r3=coefficient;
 * stack=amount. Guarded samples and convex histories keep differences within
 * +/-2^28. Histories and configuration stay in registers for the whole block.
 * Mix identity x*(65536-a)+low*a = x*65536+(low-x)*a saves one multiply,
 * with the complete signed64 sum retained before rounding (including ties). */
.align 2
.global ln_dsp_crossfeed_stereo_arm
.type ln_dsp_crossfeed_stereo_arm,%function
ln_dsp_crossfeed_stereo_arm:
    push {r4-r11,lr}
    ldr r7,[sp,#36]
    push {r2}
    mov r6,r3
    ldm r2,{r4,r5}
    cmp r1,#0
    beq .Lcrossdone
.Lcrossloop:
    ldm r0,{r8,r9}
    sub r12,r8,r4
    smull r10,r11,r6,r12
    ROUND16 r12,r10,r11,r3
    add r4,r4,r12
    sub r12,r9,r5
    smull r10,r11,r6,r12
    ROUND16 r12,r10,r11,r3
    add r5,r5,r12
    sub r12,r5,r8
    smull r10,r11,r7,r12
    adds r10,r10,r8,lsl #16
    adc r11,r11,r8,asr #16
    ROUND16 r12,r10,r11,r3
    str r12,[r0],#4
    sub r12,r4,r9
    smull r10,r11,r7,r12
    adds r10,r10,r9,lsl #16
    adc r11,r11,r9,asr #16
    ROUND16 r12,r10,r11,r3
    str r12,[r0],#4
    subs r1,r1,#1
    bne .Lcrossloop
.Lcrossdone:
    pop {r2}
    stm r2,{r4,r5}
    pop {r4-r11,lr}
    bx lr
.size ln_dsp_crossfeed_stereo_arm,.-ln_dsp_crossfeed_stereo_arm

/* r0=Q8 samples, r1=count, r2=validated positive Q16 gain, r3=clip counter.
 * The largest rounded magnitude is 2^31, so inspect signed64 high bits before
 * narrowing. Clipping and the counter exactly match the portable guard. */
.align 2
.global ln_dsp_gain_arm
.type ln_dsp_gain_arm,%function
ln_dsp_gain_arm:
    push {r4-r7,lr}
    cmp r1,#0
    beq .Lgainreturn
    /* Attenuation of already guarded samples cannot clip. Choose once per
     * block, preserving the clip counter untouched on this common path. */
    cmp r2,#0x10000
    bls .Lgainattenuate
    ldr r7,[r3]
.Lgainloop:
    ldr r4,[r0]
    smull r5,r6,r2,r4
    /* Compare against Q8 guard in the original Q24 product. Rounding can
     * reach the guard from below without clipping, hence round first. */
    mov r4,r6,lsr #31
    rsb r4,r4,#0x8000
    adds r5,r5,r4
    adc r6,r6,#0
    cmp r6,#0x800
    bgt .Lgainpositive
    blt .Lgainnegativecheck
    /* Rounded value > GUARD iff high=0x800 and low >= 0x10000. */
    cmp r5,#0x10000
    bhs .Lgainpositive
.Lgainnarrow:
    mov r4,r5,lsr #16
    orr r4,r4,r6,lsl #16
.Lgainstore:
    str r4,[r0],#4
    subs r1,r1,#1
    bne .Lgainloop
.Lgaindone:
    str r7,[r3]
.Lgainreturn:
    pop {r4-r7,lr}
    bx lr
.Lgainattenuate:
    ldr r4,[r0]
    smull r5,r6,r2,r4
    ROUND16 r4,r5,r6,r7
    str r4,[r0],#4
    subs r1,r1,#1
    bne .Lgainattenuate
    b .Lgainreturn
.Lgainnegativecheck:
    cmn r6,#0x800
    bge .Lgainnarrow
    mov r4,#0xf8000000
    b .Lgainclip
.Lgainpositive:
    mov r4,#0x08000000
.Lgainclip:
    adds r7,r7,#1
    mvncs r7,#0
    b .Lgainstore
.size ln_dsp_gain_arm,.-ln_dsp_gain_arm
