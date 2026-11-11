#include "services/audio/flac/fixed.h"

int ln_flac_fixed(int32_t *samples,unsigned count,unsigned order)
{
    if(order>count)return -4;
    if(order>4)return -5;
    if(!order)return 0;
    /* Build backward differences at the last warm-up sample. */
    uint32_t difference[4]={0,0,0,0};
    for(unsigned n=0;n<order;++n)difference[n]=(uint32_t)samples[order-1u-n];
    for(unsigned level=1;level<order;++level)
        for(unsigned n=order-1;n>=level;--n)difference[n]=difference[n-1]-difference[n];
    uint32_t a=difference[0],b=difference[1],c=difference[2],d=difference[3];
    switch(order){
    case 1:
        #pragma clang loop unroll_count(4)
        for(unsigned n=order;n<count;++n){a+=(uint32_t)samples[n];samples[n]=(int32_t)a;}
        break;
    case 2:
        #pragma clang loop unroll_count(4)
        for(unsigned n=order;n<count;++n){b+=(uint32_t)samples[n];a+=b;samples[n]=(int32_t)a;}
        break;
    case 3:
        #pragma clang loop unroll_count(4)
        for(unsigned n=order;n<count;++n){c+=(uint32_t)samples[n];b+=c;a+=b;samples[n]=(int32_t)a;}
        break;
    case 4:
        #pragma clang loop unroll_count(4)
        for(unsigned n=order;n<count;++n){d+=(uint32_t)samples[n];c+=d;b+=c;a+=b;samples[n]=(int32_t)a;}
        break;
    }
    return 0;
}
