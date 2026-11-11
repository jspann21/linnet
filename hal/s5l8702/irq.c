#include "irq.h"
#include "registers.h"
#define VIC(bank,offset) LN_REG(0x38e00000u+(bank)*0x1000u+(offset))
static ln_s5l_irq_handler dispatch;
uint32_t ln_s5l_irq_save(void) {
    uint32_t flags,masked;
    __asm__ volatile("mrs %0,cpsr\norr %1,%0,#0x80\nmsr cpsr_c,%1":"=&r"(flags),"=&r"(masked)::"memory");
    return flags;
}
void ln_s5l_irq_restore(uint32_t flags){__asm__ volatile("msr cpsr_c,%0"::"r"(flags):"memory");}
void ln_s5l_irq_wait(void) {
    uint32_t zero=0;__asm__ volatile("mcr p15,0,%0,c7,c0,4"::"r"(zero):"memory");
}
void ln_s5l_irq_init(ln_s5l_irq_handler handler) {
    (void)ln_s5l_irq_save();dispatch=handler;
    for(unsigned bank=0;bank<2;bank++) {
        VIC(bank,0x14)=UINT32_MAX;VIC(bank,0x1c)=UINT32_MAX;
        VIC(bank,0x0c)=0;VIC(bank,0x24)=0xffff;VIC(bank,0xf00)=0;
    }
    LN_REG(0x38e02008u)=UINT32_MAX;LN_REG(0x38e0200cu)=UINT32_MAX;
}
void ln_s5l_irq_enable(unsigned source,bool enabled) {
    if(source<64)VIC(source>>5,enabled?0x10:0x14)=1u<<(source&31);
}
void ln_s5l_irq_software(unsigned source,bool asserted) {
    if(source<64)VIC(source>>5,asserted?0x18:0x1c)=1u<<(source&31);
}
void ln_s5l_irq_dispatch(void) {
    (void)VIC(0,0xf00);(void)VIC(1,0xf00);
    uint32_t first=VIC(0,0),second=VIC(1,0);
    if(dispatch)dispatch(first,second);
    else {VIC(0,0x14)=first;VIC(1,0x14)=second;}
    VIC(1,0xf00)=0;VIC(0,0xf00)=0;
}
