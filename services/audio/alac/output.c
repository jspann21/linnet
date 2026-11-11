#include "services/audio/alac/output.h"

static int16_t sample16(uint32_t value)
{
    int32_t sample=(int32_t)(value<<13)>>13;
    if(sample>32767)sample=32767;
    if(sample<-32768)sample=-32768;
    return (int16_t)sample;
}
void ln_alac_output(int16_t *out,const int32_t *a,const int32_t *b,
                    unsigned count,unsigned channels,unsigned shift,unsigned weight)
{
    if(channels==1){
        for(unsigned i=0;i<count;++i){int16_t v=sample16((uint32_t)a[i]);out[2*i]=v;out[2*i+1]=v;}
    }else if(!weight){
        for(unsigned i=0;i<count;++i){out[2*i]=sample16((uint32_t)a[i]);out[2*i+1]=sample16((uint32_t)b[i]);}
    }else{
        for(unsigned i=0;i<count;++i){
            uint32_t difference=(uint32_t)b[i];
            int32_t correction=(int32_t)(difference*weight)>>shift;
            uint32_t right=(uint32_t)a[i]-(uint32_t)correction;
            out[2*i]=sample16(right+difference);out[2*i+1]=sample16(right);
        }
    }
}
