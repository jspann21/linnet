.syntax unified
.cpu arm926ej-s
.arm
.text
.global ln_s5l_irq_entry
ln_s5l_irq_entry:
    sub lr,lr,#4
    stmdb sp!,{r0-r3,r12,lr}
    mrs r0,spsr
    stmdb sp!,{r0,r1}
    bl ln_s5l_irq_dispatch
    ldmia sp!,{r0,r1}
    msr spsr_cxsf,r0
    ldmia sp!,{r0-r3,r12,pc}^
.section .bss.irq_stack,"aw",%nobits
.balign 8
    .space 2048
.global ln_s5l_irq_stack_end
ln_s5l_irq_stack_end:
