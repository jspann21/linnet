#include "services/audio/alac/predict.h"

static inline int32_t signed_sample(uint32_t value,unsigned bits)
{
    return (int32_t)(value<<(32u-bits))>>(32u-bits);
}

static inline __attribute__((always_inline)) void predict(int32_t *samples,unsigned count,unsigned bits,
                                                         int16_t *coefficients,unsigned order,unsigned shift)
{
    for(unsigned n=order+1;n<count;++n){
        const int32_t *history=samples+n-order-1;
        uint32_t base=(uint32_t)*history,sum=1u<<(shift-1u);
        int32_t residual=samples[n];
        for(unsigned tap=0;tap<order;++tap)
            sum+=((uint32_t)history[order-tap]-base)*(uint32_t)(int32_t)coefficients[tap];
        samples[n]=signed_sample((uint32_t)((int32_t)sum>>shift)+base+(uint32_t)residual,bits);
        if(!residual)continue;
        int direction=residual>0?1:-1;
        for(unsigned age=1;age<=order;++age){
            int32_t delta=(int32_t)(base-(uint32_t)history[age]);
            int adjustment=((delta>0)-(delta<0))*direction;
            coefficients[order-age]=(int16_t)(coefficients[order-age]-adjustment);
            int32_t change=(int32_t)((uint32_t)delta*(uint32_t)adjustment)>>shift;
            residual=(int32_t)((uint32_t)residual-(uint32_t)change*age);
            if(direction>0?residual<=0:residual>=0)break;
        }
    }
}

void ln_alac_predict_four(int32_t *,int,int,int16_t *,int);

void ln_alac_predict(int32_t *samples,unsigned count,unsigned bits,int16_t *coefficients,
                      unsigned order,unsigned shift)
{
    if(!order)return;
    unsigned warm=order==31?count-1u:order;
    for(unsigned n=1;n<=warm;++n)
        samples[n]=signed_sample((uint32_t)samples[n-1]+(uint32_t)samples[n],bits);
    if(order==31)return;
    if(order==4)ln_alac_predict_four(samples,count,bits,coefficients,shift);
    else if(order==8)predict(samples,count,bits,coefficients,8,shift);
    else if(order==6)predict(samples,count,bits,coefficients,6,shift);
    else predict(samples,count,bits,coefficients,order,shift);
}
