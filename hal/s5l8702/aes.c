#include "aes.h"
#include "registers.h"
#include "hal/arm926/cache.h"
#define AES(offset) LN_REG(0x38c00000u+(offset))
extern uint32_t ln_time_us(void);
bool ln_s5l_aes_crypt(uint8_t *data,uint32_t size,uint32_t key,bool encrypt) {
    if(!data || ((uintptr_t)data&31u) || !size || size>0x1f800u || (size&15u) || (key!=1 && key!=2))return false;
    LN_CLOCK_GATE0&=~(1u<<10);
    AES(8)=1;AES(8)=0;AES(0)=1;AES(0x10)=0;AES(0x8c)=0;
    for(unsigned i=0;i<4;i++)AES(0x74+i*4)=0;
    AES(0x6c)=key;AES(0x88)=~key;AES(0x14)=encrypt?9:8;
    AES(0x18)=size;AES(0x24)=size;AES(0x2c)=size;AES(0x34)=size;
    AES(0x20)=(uint32_t)(uintptr_t)data;AES(0x28)=(uint32_t)(uintptr_t)data;AES(0x30)=(uint32_t)(uintptr_t)data;
    ln_arm926_data_clean_range(data,size);AES(4)=1;
    uint32_t start=ln_time_us(),budget=2000000;bool ok=true;
    while(!(AES(12)&15u))if(!budget-- || ln_time_us()-start>=100000u){ok=false;break;}
    if(!ok){AES(8)=1;AES(8)=0;}
    LN_CLOCK_GATE0|=1u<<10;
    (void)ln_arm926_data_invalidate_range(data,(size+31u)&~31u);return ok;
}
