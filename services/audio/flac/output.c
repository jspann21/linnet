#include "services/audio/flac/output.h"

static int16_t sample16(uint32_t value,unsigned shift)
{
    /* Preserve the decoder's wrapping Q29 intermediate before saturation. */
    int32_t sample=(int32_t)(value<<shift)>>13;
    if(sample>32767)sample=32767;
    if(sample<-32768)sample=-32768;
    return (int16_t)sample;
}
void ln_flac_output(int16_t *out,const int32_t *a,const int32_t *b,
                    unsigned count,unsigned channels,unsigned bits,unsigned mode)
{
    unsigned shift=29u-bits;
    if(channels==1){
        for(unsigned i=0;i<count;++i){int16_t v=sample16((uint32_t)a[i],shift);out[2*i]=v;out[2*i+1]=v;}
        return;
    }
    switch(mode){
    case 0:
        for(unsigned i=0;i<count;++i){out[2*i]=sample16((uint32_t)a[i],shift);out[2*i+1]=sample16((uint32_t)b[i],shift);}
        break;
    case 1:
        for(unsigned i=0;i<count;++i){uint32_t left=(uint32_t)a[i];out[2*i]=sample16(left,shift);out[2*i+1]=sample16(left-(uint32_t)b[i],shift);}
        break;
    case 2:
        for(unsigned i=0;i<count;++i){uint32_t right=(uint32_t)b[i];out[2*i]=sample16((uint32_t)a[i]+right,shift);out[2*i+1]=sample16(right,shift);}
        break;
    case 3:
        for(unsigned i=0;i<count;++i){uint32_t right=(uint32_t)a[i]-(uint32_t)(b[i]>>1);out[2*i]=sample16(right+(uint32_t)b[i],shift);out[2*i+1]=sample16(right,shift);}
        break;
    }
}
