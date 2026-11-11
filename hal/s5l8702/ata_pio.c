#include "ata_pio.h"
#include "registers.h"
#define ATA(offset) LN_REG(0x38700000u+(offset))
extern uint32_t ln_time_us(void);
static bool ready(uint32_t bit) {
    if(ATA(0x78)&bit)return true;
    uint32_t deadline=ln_time_us()+1000u;
    for(unsigned budget=100000;budget;--budget) {
        if(ATA(0x78)&bit)return true;
        if((int32_t)(ln_time_us()-deadline)>=0)return false;
    }
    return false;
}
static bool valid(unsigned offset){return offset>=0x54 && offset<=0x74 && !(offset&3);}
bool ln_s5l_ata_read(unsigned offset,uint16_t *value) {
    if(!value || !valid(offset) || !ready(2))return false;
    (void)ATA(offset);
    if(!ready(1))return false;
    *value=(uint16_t)ATA(0x7c);return true;
}
bool ln_s5l_ata_write(unsigned offset,uint16_t value) {
    if(!valid(offset) || !ready(2))return false;
    ATA(offset)=value;return true;
}
