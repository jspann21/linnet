.syntax unified
.cpu arm7tdmi
.arm
.section .text.ln_video_lz_decode_block,"ax",%progbits
.global ln_video_lz_decode_block
.type ln_video_lz_decode_block,%function
/* r4=input, r5=input end, r6=output, r7=output end, r8=history base.
 * Keep block state in callee-saved registers across the bulk-copy helpers.
 * Short matches use halfwords only when source and destination are aligned;
 * byte fallback and interleaved loads/stores preserve forward overlap order. */
ln_video_lz_decode_block:
    push {r4-r11, r12, lr}
    mov r4, r0
    add r5, r0, r1
    mov r6, r2
    add r7, r2, r3
    mov r8, r2
    sub r12, r7, #14
    cmp r1, #0
    beq .Llz_bad
.Llz_token:
    ldrb r9, [r4], #1
    mov r10, r9, lsr #4
    cmp r10, #2
    andls r0, r9, #15
#if defined(LINNET_TARGET_PHOTO4G_P98)
    cmpls r0, #8
#else
    cmpls r0, #4
#endif
    bhi .Llz_literal
    /* Both literals and the offset must be present. A match must start
     * at least twelve bytes before the decoded end (strict LZ4 rule). */
    sub r1, r5, r4
    cmp r1, #4
    blo .Llz_literal
    cmp r6, r12
    bhi .Llz_literal
    add r2, r4, r10
    ldrb r11, [r2]
    ldrb r1, [r2, #1]
    orrs r11, r11, r1, lsl #8
    beq .Llz_bad
    sub r1, r6, r8
    add r1, r1, r10
    cmp r11, r1
    bhi .Llz_bad
    cmp r10, #0
    ldrbne r1, [r4], #1
    strbne r1, [r6], #1
    cmp r10, #2
    ldrbeq r1, [r4], #1
    strbeq r1, [r6], #1
    add r4, r4, #2
    add r10, r0, #4
    b .Llz_short_match
.Llz_literal:
    sub r2, r7, r6
    cmp r10, #15
    bne .Llz_literal_bounds
.Llz_literal_extension:
    cmp r4, r5
    bhs .Llz_bad
    ldrb r0, [r4], #1
    adds r10, r10, r0
    bcs .Llz_bad
    cmp r10, r2
    bhi .Llz_bad
    cmp r0, #255
    beq .Llz_literal_extension
.Llz_literal_bounds:
    sub r1, r5, r4
    cmp r10, r1
    cmpls r10, r2
    bhi .Llz_bad
    cmp r10, #0
    beq .Llz_after_literal
    cmp r10, #8
    bhi .Llz_literal_bulk
    mov r1, r10
.Llz_literal_bytes:
    ldrb r0, [r4], #1
    strb r0, [r6], #1
    subs r1, r1, #1
    bne .Llz_literal_bytes
    b .Llz_after_literal
.Llz_literal_bulk:
    mov r0, r6
    mov r1, r4
    mov r2, r10
    bl ln_memory_copy
    sub r12, r7, #14
    add r4, r4, r10
    add r6, r6, r10
.Llz_after_literal:
    cmp r4, r5
    beq .Llz_terminal
    sub r0, r5, r4
    cmp r0, #2
    blo .Llz_bad
    ldrb r11, [r4], #1
    ldrb r0, [r4], #1
    orrs r11, r11, r0, lsl #8
    beq .Llz_bad
    sub r0, r6, r8
    cmp r11, r0
    bhi .Llz_bad
    sub r2, r7, r6
    cmp r2, #12
    blo .Llz_bad
    sub r2, r2, #4
    and r10, r9, #15
    cmp r10, #15
    bne .Llz_match_bounds
.Llz_match_extension:
    cmp r4, r5
    bhs .Llz_bad
    ldrb r0, [r4], #1
    adds r10, r10, r0
    bcs .Llz_bad
    cmp r10, r2
    bhi .Llz_bad
    cmp r0, #255
    beq .Llz_match_extension
.Llz_match_bounds:
    cmp r10, r2
    bhi .Llz_bad
    add r10, r10, #4
#if defined(LINNET_TARGET_PHOTO4G_P98)
    cmp r10, #12
#else
    cmp r10, #8
#endif
    bls .Llz_short_match
    mov r0, r6
    mov r1, r11
    mov r2, r10
    bl ln_video_lz_copy_long
    sub r12, r7, #14
    add r6, r6, r10
    b .Llz_next
.Llz_short_match:
    orr r0, r6, r11
    tst r0, #1
    bne .Llz_short_bytes
    sub r1, r6, r11
    ldrh r0, [r1], #2
    strh r0, [r6], #2
    ldrh r0, [r1], #2
    strh r0, [r6], #2
    cmp r10, #4
    beq .Llz_next
    cmp r10, #6
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
#if defined(LINNET_TARGET_PHOTO4G_P98)
    cmp r10, #8
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
    cmp r10, #10
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
    cmp r10, #12
#else
    cmp r10, #8
#endif
    ldrheq r0, [r1], #2
    strheq r0, [r6], #2
    ldrbne r0, [r1]
    strbne r0, [r6], #1
    b .Llz_next
.Llz_short_bytes:
    tst r11, #1
    bne .Llz_short_odd_offset
    sub r1, r6, r11
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    ldrh r0, [r1], #2
    strh r0, [r6], #2
    cmp r10, #4
    beq .Llz_short_peeled_tail
    ldrh r0, [r1], #2
    strh r0, [r6], #2
    cmp r10, #5
    beq .Llz_next
    cmp r10, #7
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
#if defined(LINNET_TARGET_PHOTO4G_P98)
    cmp r10, #8
    beq .Llz_short_peeled_tail
    cmp r10, #9
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
    cmp r10, #11
    ldrhhs r0, [r1], #2
    strhhs r0, [r6], #2
    beq .Llz_next
#endif
.Llz_short_peeled_tail:
    ldrb r0, [r1]
    strb r0, [r6], #1
    b .Llz_next
.Llz_short_odd_offset:
    sub r1, r6, r11
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #4
    beq .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #5
    beq .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #6
    beq .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #7
#if defined(LINNET_TARGET_PHOTO4G_P98)
    ldrbhi r0, [r1], #1
    strbhi r0, [r6], #1
    cmp r10, #8
    bls .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #9
    beq .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #10
    beq .Llz_next
    ldrb r0, [r1], #1
    strb r0, [r6], #1
    cmp r10, #11
    ldrbhi r0, [r1]
    strbhi r0, [r6], #1
#else
    ldrbhi r0, [r1]
    strbhi r0, [r6], #1
#endif
.Llz_next:
    cmp r4, r5
    blo .Llz_token
    b .Llz_bad
.Llz_terminal:
    cmp r6, r7
    bne .Llz_bad
    cmp r10, #0
    beq .Llz_bad
    cmp r10, #5
    bhs .Llz_good
    /* A decoded block shorter than five bytes cannot contain any match:
     * the match-start guard requires twelve remaining output bytes. */
    sub r0, r7, r8
    cmp r0, #5
    bhs .Llz_bad
.Llz_good:
    mov r0, #1
    b .Llz_return
.Llz_bad:
    mvn r0, #0
.Llz_return:
    pop {r4-r11, r12, lr}
    bx lr
.size ln_video_lz_decode_block, .-ln_video_lz_decode_block
