#include "linnet_target.h"
.syntax unified
.cpu arm7tdmi
.arm
/* RGB565 2x2 transpose into the target panel's column order.
 * r0: destination at the first logical x/y; columns descend by one panel height.
 * r1: row-major source with panel-width pitch; r2/r3: positive even width/height.
 * Both pointers are word aligned. Each source pair becomes two column pairs. */
.section .text.ln_memory_columns,"ax",%progbits
.global ln_memory_columns
.type ln_memory_columns,%function
ln_memory_columns:
    push {r4-r11,lr}
    mov r8, #(LINNET_LCD_HEIGHT_ASM * 2)
    mul r4, r2, r8
    mov r8, r4
    mov r9, #(LINNET_LCD_WIDTH_ASM * 4)
    sub r9, r9, r2, lsl #1
    mov r10, r2, lsr #1
    mov r11, r3, lsr #1
.Lcolumns_rows:
    mov r12, r10
.Lcolumns_pair:
    ldr r4, [r1]
    ldr r5, [r1, #(LINNET_LCD_WIDTH_ASM * 2)]
    mov r6, r4, lsl #16
    mov r6, r6, lsr #16
    orr r6, r6, r5, lsl #16
    mov r7, r5, lsr #16
    mov r7, r7, lsl #16
    orr r7, r7, r4, lsr #16
    str r6, [r0]
    str r7, [r0, #-(LINNET_LCD_HEIGHT_ASM * 2)]
    add r1, r1, #4
    sub r0, r0, #(LINNET_LCD_HEIGHT_ASM * 4)
    subs r12, r12, #1
    bne .Lcolumns_pair
    add r0, r0, r8
    add r0, r0, #4
    add r1, r1, r9
    subs r11, r11, #1
    bne .Lcolumns_rows
    pop {r4-r11,pc}
.size ln_memory_columns, .-ln_memory_columns
