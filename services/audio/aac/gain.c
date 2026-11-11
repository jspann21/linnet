#include "services/audio/aac/gain.h"

static inline int32_t gain_product(int32_t sample,int32_t factor)
{
#if defined(__arm__)
    uint32_t low,high;
    __asm__("smull %0,%1,%2,%3\n\t"
            "movs %0,%0,lsr #14\n\t"
            "adc %0,%0,%1,lsl #18"
            : "=&r"(low),"=&r"(high) : "r"(factor),"r"(sample) : "cc");
    return (int32_t)low;
#else
    return (int32_t)(((int64_t)sample*factor+8192)>>14);
#endif
}

void ln_aac_gain(int32_t *samples,unsigned count,int gain)
{
    /* Q28 powers of two at 1/24 steps; retain the legacy +12 rounding. */
    static const int32_t fraction[47]={
        138150608,142198729,146365470,150654306,155068814,159612677,164289686,169103741,
        174058859,179159172,184408937,189812531,195374463,201099372,206992033,213057363,
        219300420,225726413,232340701,239148804,246156398,253369332,260793620,268435456,
        276301215,284397459,292730940,301308612,310137628,319225354,328579371,338207482,
        348117717,358318345,368817874,379625063,390748926,402198743,413984066,426114725,
        438600840,451452825,464681403,478297607,492312797,506738663,521587239
    };
    int exponent=gain/24,remainder=gain%24;
    unsigned shift=(unsigned)(exponent<0?-exponent:exponent);
    if(!remainder){
        if(exponent<0){for(unsigned n=0;n<count;++n)samples[n]>>=shift;}
        else if(shift){for(unsigned n=0;n<count;++n)samples[n]=(int32_t)((uint32_t)samples[n]<<shift);}
        return;
    }
    int32_t factor=fraction[remainder+23];
    /* Preserve the decoder's Q14 product rounding and wrapping output. */
    if(exponent<0){
        for(unsigned n=0;n<count;++n)
            samples[n]=gain_product(samples[n]>>shift,factor);
    }else{
        for(unsigned n=0;n<count;++n)
            samples[n]=gain_product((int32_t)((uint32_t)samples[n]<<shift),factor);
    }
}
