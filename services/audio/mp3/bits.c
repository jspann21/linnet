#include "services/audio/mp3/bits.h"

void ln_mp3_bits_init(struct ln_mp3_bits *b,const uint8_t *data)
{b->base=data;b->readbit=0;}
uint32_t ln_mp3_bits_length(const struct ln_mp3_bits *start,const struct ln_mp3_bits *end)
{return end->readbit-start->readbit;}
const uint8_t *ln_mp3_bits_nextbyte(const struct ln_mp3_bits *b)
{return b->base+((b->readbit+7u)>>3);}
void ln_mp3_bits_skip(struct ln_mp3_bits *b,uint32_t count)
{b->readbit+=count;}

uint32_t ln_mp3_bits_read(struct ln_mp3_bits *b,uint32_t count)
{
    if(!count||count>32)return 0;
    const uint8_t *p=b->base+(b->readbit>>3);
    unsigned offset=b->readbit&7u,span=offset+count;
    /* Place intersecting bytes at the high end of a word. The initial shift
     * removes preceding bits; the final shift removes following bits. */
    uint32_t value=(uint32_t)p[0]<<24;
    if(span>8){
        value|=(uint32_t)p[1]<<16;
        if(span>16){
            value|=(uint32_t)p[2]<<8;
            if(span>24)value|=p[3];
        }
    }
    value<<=offset;
    if(span>32)value|=(uint32_t)p[4]>>(8u-offset);
    b->readbit+=count;
    return value>>(32u-count);
}

/* Each entry is x advanced by eight zero bits through polynomial 0x8005. */
#include "services/audio/mp3/crc16.inc"
static uint16_t octet(uint16_t crc,uint32_t value)
{return (uint16_t)((uint32_t)crc<<8)^crc16[(crc>>8)^(value&255u)];}
uint16_t ln_mp3_bits_crc(struct ln_mp3_bits b,uint32_t count,uint16_t crc)
{
    while(count>=32){
        uint32_t word=ln_mp3_bits_read(&b,32);
        crc=octet(octet(octet(octet(crc,word>>24),word>>16),word>>8),word);
        count-=32;
    }
    while(count>=8){crc=octet(crc,ln_mp3_bits_read(&b,8));count-=8;}
    if(count){
        uint32_t tail=ln_mp3_bits_read(&b,count);
        while(count--){
            unsigned feedback=(crc>>15)^((tail>>count)&1u);
            crc=(uint16_t)((uint32_t)crc<<1);
            if(feedback)crc^=0x8005u;
        }
    }
    return crc;
}
