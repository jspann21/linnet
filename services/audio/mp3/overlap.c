#include "services/audio/mp3/overlap.h"

/* Portable host counterpart of overlap.S with the same wrapping arithmetic. */
void ln_mp3_overlap(const int32_t *output,int32_t history[18],
                    int32_t samples[18][32],unsigned band)
{
    for(unsigned i=0;i<18;++i){
        uint32_t value=(uint32_t)history[i]+(output?(uint32_t)output[i]:0u);
        if((band&1u)&&(i&1u))value=0u-value;
        samples[i][band]=(int32_t)value;
        history[i]=output?output[i+18]:0;
    }
}
