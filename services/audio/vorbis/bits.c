#include "services/audio/vorbis/bits.h"
void ln_vorbis_bits_init(struct ln_vorbis_bits *b,const uint8_t *data,int bytes)
{
    *b=(struct ln_vorbis_bits){0,0,data,data,bytes<0?0:bytes};
    if(bytes<0||(!data&&bytes))ln_vorbis_bits_fail(b);
}
long ln_vorbis_bits_read(struct ln_vorbis_bits *b,int count)
{
    /* -1 can also be a valid 32-bit field, so check availability separately. */
    if((unsigned)count>32u||
       (long)(((unsigned)b->endbit+(unsigned)count+7u)>>3)>b->storage-b->endbyte){
        ln_vorbis_bits_fail(b);return -1;
    }
    if(!count)return 0;
    unsigned span=(unsigned)b->endbit+(unsigned)count;
    long value=ln_vorbis_bits_value(b,(unsigned)count,span);
    b->ptr+=span>>3;b->endbyte+=(long)(span>>3);b->endbit=(int)(span&7u);
    return value;
}
