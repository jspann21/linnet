#include "services/audio/aac/bits.h"
static uint32_t word(const struct ln_aac_bits *b,uint32_t byte)
{
    if(byte>=b->size)return 0;
    const uint8_t *p=b->data+byte;
    uint32_t remaining=b->size-byte;
    if(remaining>=4)return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    uint32_t value=(uint32_t)p[0]<<24;
    if(remaining>1)value|=(uint32_t)p[1]<<16;
    if(remaining>2)value|=(uint32_t)p[2]<<8;
    return value;
}
void ln_aac_bits_init(struct ln_aac_bits *b,const void *data,uint32_t bytes)
{
    *b=(struct ln_aac_bits){0};
    if(!data||!bytes||bytes>8192){b->error=1;b->no_more_reading=1;return;}
    b->data=data;b->size=bytes;b->left=bytes<4?bytes*8u:32u;b->endbit=b->left;
    b->current=word(b,0)>>(32u-b->left);b->next=word(b,4);
}
void ln_aac_bits_advance(struct ln_aac_bits *b,uint32_t count)
{
    if(b->error)return;
    uint32_t position=b->endbit-b->left;
    if(count>32||count>b->size*8u-position){
        b->error=1;b->no_more_reading=1;b->endbit=position;b->left=0;
        b->current=b->next=0;return;
    }
    position+=count;
    uint32_t byte=b->endbit/8u,remaining=b->size-byte;
    uint32_t width=remaining<4?remaining*8u:32u;
    b->endbit+=width;b->left=b->endbit-position;
    b->current=width?b->next>>(32u-width):0;
    b->next=word(b,byte+4u);
    b->no_more_reading=position==b->size*8u;
}
