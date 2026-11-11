#include "spi.h"
#include "registers.h"
#define SPI(offset) LN_REG(0x3c300000u+(offset))
extern uint32_t ln_time_us(void);
static uint32_t expires;
bool ln_s5l_spi0_begin(void) {
    LN_CLOCK_GATE1&=~((1u<<2)|(1u<<12));
    LN_REG(0x3cf00000u)=(LN_REG(0x3cf00000u)&0xffff0000u)|0x2222u;
    LN_REG(0x3cf00200u)=0xf;
    SPI(8)=0xf;SPI(0)|=0xc;SPI(0x30)=15;SPI(12)=6;
    SPI(4)=0x10618;SPI(0)|=0xc;SPI(0)=1;
    expires=ln_time_us()+100000u;
    LN_REG(0x3cf00200u)=0xe;return true;
}
bool ln_s5l_spi0_byte(uint8_t output,uint8_t *input) {
    if(!input)return false;
    SPI(0x34)=1;
    uint32_t budget=1000000;
    while((SPI(8)&0x1f0)==0x100)
        if(!budget-- || (int32_t)(ln_time_us()-expires)>=0)return false;
    SPI(0x10)=output;
    while(!(SPI(8)&0x3e00))
        if(!budget-- || (int32_t)(ln_time_us()-expires)>=0)return false;
    *input=(uint8_t)SPI(0x20);return true;
}
void ln_s5l_spi0_end(void) {
    LN_REG(0x3cf00200u)=0xf;SPI(0)=0;LN_CLOCK_GATE1|=1u<<2;
}
