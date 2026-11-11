#include "targets/classic6g-n25/board.h"
#include "hal/s5l8702/registers.h"
static uint32_t result[16] __attribute__((aligned(32)));
uint32_t ln_time_us(void){return LN_TIMER_E(0x14);}
void ln_storage_probe(unsigned operation,uint32_t lba,uint32_t result[16]);
void ln_filesystem_probe(const uint32_t *request,uint32_t result[16]);
uint32_t *ln_board_probe(const uint32_t *request) {
    for(unsigned n=0;n<16;n++)result[n]=0;
    result[0]=0x4252504cu;result[1]=request[0];
    __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(result[2]));
    if(*(volatile uint16_t *)(uintptr_t)0x3c500012u!=0 || LN_TIMER_E(0)!=0x440 || (LN_CLOCK_GATE1&(1u<<5))) {
        *(volatile uint16_t *)(uintptr_t)0x3c500012u=0;
        LN_CLOCK_GATE1&=~(1u<<5);LN_TIMER_E(0)=0x440;LN_TIMER_E(0x10)=11;
        LN_TIMER_E(8)=UINT32_MAX;LN_TIMER_E(4)=3;
    }
    if(request[0]==0) {
        result[3]=LN_DWC(8);result[4]=LN_DWC(12);result[5]=LN_DWC(0x800);
        result[6]=LN_DWC(0x44);result[7]=LN_DWC(0x48);result[8]=LN_DWC(0x4c);result[9]=LN_DWC(0x50);
        result[10]=LN_DWC(0x830);result[11]=LN_DWC(0xe00);
        result[12]=LN_REG(0x38100000u);
        struct ln_n25_power_identity id;
        if(ln_n25_power_identify(&id))result[13]=1u|((uint32_t)id.version<<8)|((uint32_t)id.variant<<16)|((uint32_t)id.status<<24);
    } else if(request[0]==1) {
        uint32_t mapping=LN_REG(0x38100000u);
        result[3]=ln_n25_memory_init(result+4);
        /* BootROM resumes using its original vector mapping. */
        LN_REG(0x38100000u)=mapping;
    } else if(request[0]>=2 && request[0]<=4){
        ln_storage_probe(request[0],request[1],result);
        result[11]=LN_REG(0x3cf00164u);
    }
    else if(request[0]>=5 && request[0]<=7)ln_filesystem_probe(request,result);
    else result[3]=UINT32_MAX;
    result[14]=ln_time_us();result[15]=0x454e4f44u;
    return result;
}
