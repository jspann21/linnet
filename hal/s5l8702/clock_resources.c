#include "linnet_target.h"
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
#include "clock_resources.h"
#include "registers.h"
#include "irq.h"
extern uint32_t ln_time_us(void);
/* PLL2's enable, input selection and output selection are one shared resource.
 * CPU video boost and the 32 kHz audio clock each hold an idempotent lease.
 * Restore inherited configuration only after the final consumer moves away. */
#define PLL2_BITS ((1u<<2)|(1u<<6)|(1u<<18))
static uint32_t users,saved_mode,saved_pms,saved_lock;
static bool owned;
static bool pll2_216_valid(void){
    uint32_t pms=LN_REG(0x3c500028u),mode=LN_REG(0x3c500044u);
    return (pms==0x01002401u||pms==0x04004800u)&&!(mode&(1u<<6));
}
bool ln_s5l_pll2_acquire(uint32_t owner){
    if(owner!=LN_S5L_PLL2_CPU&&owner!=LN_S5L_PLL2_AUDIO)return false;
    uint32_t mode=LN_REG(0x3c500044u);
    if(users){
        if(!(mode&4u)||!pll2_216_valid()||!(mode&(1u<<18)))return false;
        users|=owner;return true;
    }
    /* An inherited enabled PLL belongs to its original owner. Its validated
     * 216 MHz configuration may be shared, but must never be switched off. */
    if((mode&4u)&&!pll2_216_valid())return false;
    if(!(mode&4u)&&(LN_REG(0x3c500000u)&0xf000u)==0x3000u)return false;
    saved_mode=mode&PLL2_BITS;saved_pms=LN_REG(0x3c500028u);saved_lock=LN_REG(0x3c500038u);
    owned=!(mode&4u);
    if(owned){
        LN_REG(0x3c500028u)=0x01002401u;LN_REG(0x3c500038u)=32400u;
        LN_REG(0x3c500044u)=(mode&~(1u<<6))|4u;
    }
    uint32_t start=ln_time_us();
    while((LN_REG(0x3c500040u)&0x44u)!=0x44u){
        if(ln_time_us()-start<10000u)continue;
        if(owned){
            LN_REG(0x3c500044u)&=~((1u<<18)|4u);
            LN_REG(0x3c500028u)=saved_pms;LN_REG(0x3c500038u)=saved_lock;
            LN_REG(0x3c500044u)=(LN_REG(0x3c500044u)&~PLL2_BITS)|saved_mode;
        }
        owned=false;return false;
    }
    LN_REG(0x3c500044u)|=1u<<18;users=owner;return true;
}
bool ln_s5l_pll2_release(uint32_t owner){
    if(owner!=LN_S5L_PLL2_CPU&&owner!=LN_S5L_PLL2_AUDIO)return false;
    if(!(users&owner))return true;
    uint32_t remaining=users&~owner;
    if(remaining){users=remaining;return true;}
    if((LN_REG(0x3c500000u)&0xf000u)==0x3000u)return false;
    bool audio_clocked=!(LN_CLOCK_GATE1&(1u<<7))&&(LN_REG(0x3ca00000u)&1u);
    if(audio_clocked&&(LN_REG(0x3c50000cu)&0x3000u)==0x3000u)return false;
    if(owned){
        LN_REG(0x3c500044u)&=~((1u<<18)|4u);
        LN_REG(0x3c500028u)=saved_pms;LN_REG(0x3c500038u)=saved_lock;
    }
    LN_REG(0x3c500044u)=(LN_REG(0x3c500044u)&~PLL2_BITS)|saved_mode;
    bool ok=(LN_REG(0x3c500044u)&PLL2_BITS)==saved_mode&&
        (!owned||LN_REG(0x3c500028u)==saved_pms);
    if(ok){users=0;owned=false;}return ok;
}
uint32_t ln_s5l_clock_resource_flags(void){return users|(owned?256u:0u);}
bool ln_s5l_dma_idle_gate(void){
    uint32_t flags=ln_s5l_irq_save();
    bool idle=(LN_CLOCK_GATE0&(1u<<25))!=0;
    if(!idle){
        idle=!(LN_REG(0x3820001cu)|LN_REG(0x38200014u)|LN_REG(0x38200018u));
        if(idle)LN_CLOCK_GATE0|=1u<<25;
    }
    ln_s5l_irq_restore(flags);return idle;
}
#endif
