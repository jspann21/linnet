#include "vpu.h"
#include "registers.h"
#include "hal/api/platform.h"
#define V(offset) LN_REG(0x39800000u+(offset))
#define MODE LN_REG(0x38100314u)
#define CLOCK (*(volatile uint16_t *)(uintptr_t)0x3c50000au)
#define EDGE LN_REG(0x38e0200cu)
static uint32_t saved_gates,started;
static uint16_t saved_clock;
static uint16_t active_clock;
static bool opened,busy;
uint32_t ln_vpu_status,ln_vpu_fault;
bool ln_vpu_busy(void){return busy;}
uint32_t ln_vpu_clock(void){return active_clock;}
/* Register/descriptor layout checked against retail 1.1.2, routine
 * 0x081923fc (descriptors) and 0x08192518 (launch/completion). */
static void reset(void){
    LN_CLOCK_GATE0&=~(1u<<17);MODE|=1u;
    V(0xec)&=~1u;(void)V(0xec);
    for(unsigned offset=0;offset<0x130;offset+=4)V(offset)=0;
    EDGE=8;
}
bool ln_vpu_open(void){
    if(opened)return !busy;
    /* The video clock uses the already locked PLL2 owned by core.c. */
    if((LN_REG(0x3c500040u)&0x44u)!=0x44u)return false;
    saved_gates=LN_CLOCK_GATE0&(15u<<14);saved_clock=CLOCK;
    MODE&=~1u;LN_CLOCK_GATE0|=15u<<14;
    CLOCK=(uint16_t)((saved_clock&~0xb000u)|0x3000u);
    active_clock=CLOCK;
    LN_CLOCK_GATE0&=~(7u<<14);
    LN_REG(0x38e01014u)=8;LN_REG(0x38e02004u)|=8;
    reset();LN_CLOCK_GATE0|=1u<<17;
    opened=true;ln_vpu_status=ln_vpu_fault=0;return true;
}
void ln_vpu_close(void){
    if(!opened)return;
    reset();MODE&=~1u;LN_CLOCK_GATE0=(LN_CLOCK_GATE0&~(15u<<14))|saved_gates;
    CLOCK=saved_clock;opened=busy=false;
}
bool ln_vpu_start(const struct ln_vpu_picture *p){
    if(!opened||busy||!p||!p->width_mbs||!p->height_mbs||p->width_mbs>40||p->height_mbs>30||!p->slices||p->slices>10)return false;
    reset();
    V(0xd8)=p->control;V(0xe0)=p->width_mbs|(p->height_mbs<<8);
    V(0xdc)=p->descriptors;
    uint32_t pitch=p->width_mbs*16u;
    V(0xe4)=pitch|((pitch/2u)<<16);
    for(unsigned i=0;i<3;i++)V(0xcc+4*i)=p->output[i];
    for(unsigned i=0;i<17;i++)for(unsigned j=0;j<3;j++)V(i*12+j*4)=p->references[i][j];
    V(0x118)=0x82625a00u;
    V(0xe8)=2u*p->width_mbs*p->height_mbs|(p->slices<<16);
    started=ln_time_us();busy=true;V(0xe8)|=0x88003001u;return true;
}
int ln_vpu_poll(void){
    if(!busy)return -1;
    if(!(LN_REG(0x38e01008u)&8u)){
        if(ln_time_us()-started<50000u)return 0;
        ln_vpu_fault=1;reset();LN_CLOCK_GATE0|=1u<<17;busy=false;return -1;
    }
    ln_vpu_status=V(0xf4);EDGE=8;
    bool ok=(ln_vpu_status&0x1ffc0000u)==0;
    if(!ok)ln_vpu_fault=2;
    V(0x118)=0x82625a00u;LN_CLOCK_GATE0|=1u<<17;busy=false;
    return ok?1:-1;
}
