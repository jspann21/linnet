#include "services/audio/flac/lpc.h"

static inline int64_t accumulate_wide(int64_t sum,int32_t sample,int32_t coefficient)
{
#if defined(__arm__)
    uint32_t low=(uint32_t)sum,high=(uint32_t)((uint64_t)sum>>32);
    /* ARM7 multiply timing depends on Rs: keep the short coefficient there. */
    __asm__("smlal %0,%1,%2,%3" : "+&r"(low),"+&r"(high) : "r"(sample),"r"(coefficient));
    return (int64_t)(((uint64_t)high<<32)|low);
#else
    return sum+(int64_t)sample*coefficient;
#endif
}

static inline uint32_t accumulate(uint32_t sum,int32_t sample,int32_t coefficient)
{
#if defined(__arm__)
    __asm__("mla %0,%1,%2,%0" : "+&r"(sum) : "r"(sample),"r"(coefficient));
    return sum;
#else
    return sum+(uint32_t)sample*(uint32_t)coefficient;
#endif
}

static int64_t prediction(const int32_t *sample,const int32_t *coefficients,unsigned order)
{
    int64_t sum=0;
    for(unsigned tap=0;tap<order;++tap)sum=accumulate_wide(sum,sample[-1-(int)tap],coefficients[tap]);
    return sum;
}

void ln_flac_lpc(int32_t *samples,unsigned count,const int32_t *coefficients,
                 unsigned order,unsigned shift,unsigned wide,unsigned bits)
{
    shift&=15u;
    if(wide){
        for(unsigned n=order;n<count;++n){
            uint32_t value=(uint32_t)samples[n]+(uint32_t)(prediction(samples+n,coefficients,order)>>shift);
            samples[n]=(int32_t)value;
        }
        if(bits>16)return;
        uint32_t range=0,bias=1u<<(bits-1u);
        for(unsigned n=order;n<count;++n)range|=(uint32_t)samples[n]+bias;
        if(range<2u*bias)return;
        /* Recover residuals before replaying legacy streams with wrapping sums. */
        for(unsigned n=count;n-->order;)
            samples[n]=(int32_t)((uint32_t)samples[n]-(uint32_t)(prediction(samples+n,coefficients,order)>>shift));
    }
    for(unsigned n=order;n<count;++n){
        uint32_t sum=0;
        for(unsigned tap=0;tap<order;++tap)
            sum=accumulate(sum,samples[n-tap-1u],coefficients[tap]);
        samples[n]=(int32_t)((uint32_t)samples[n]+(uint32_t)((int32_t)sum>>shift));
    }
}
