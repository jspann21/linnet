#include "services/photos/idct.h"

static __attribute__((always_inline)) inline int64_t product(int32_t coefficient,int32_t value)
{
#if defined(__arm__)
    /* Retain a single signed 32x32->64 multiply. Generic ARM lowering can
     * expand this into an unsigned multiply plus sign-correction multiplies. */
    int64_t result;
    __asm__("smull %Q0, %R0, %1, %2" : "=&r"(result) : "r"(coefficient),"r"(value));
    return result;
#else
    return (int64_t)coefficient*value;
#endif
}

/* Separable cosine synthesis. The basis is 1 for frequency zero and
 * sqrt(2)*cos((2*x+1)*u*pi/16) otherwise; two passes then divide by 8.
 * Reflecting x around 3.5 preserves even frequencies and negates odd ones.
 * Q20 constants below are rounded directly from that basis. Four fractional
 * bits survive the first pass. Baseline inputs keep the workspace below
 * 2^26 and every signed64 accumulator below 2^50, including rounding bias.
 */
static __attribute__((always_inline)) inline void synthesize(
    const int32_t *v,int32_t *out,unsigned shift)
{
    const int32_t c1=1454417,c2=1370031,c3=1232995;
    const int32_t c5=823861,c6=567485,c7=289301;
    int64_t base0=(int64_t)(v[0]+v[4])*1048576;
    int64_t base1=(int64_t)(v[0]-v[4])*1048576;
    int64_t pair0=product(c2,v[2])+product(c6,v[6]);
    int64_t pair1=product(c6,v[2])-product(c2,v[6]);
    int64_t odd0=product(c1,v[1])+product(c3,v[3])+product(c5,v[5])+product(c7,v[7]);
    int64_t odd1=product(c3,v[1])-product(c7,v[3])-product(c1,v[5])-product(c5,v[7]);
    int64_t odd2=product(c5,v[1])-product(c1,v[3])+product(c7,v[5])+product(c3,v[7]);
    int64_t odd3=product(c7,v[1])-product(c5,v[3])+product(c3,v[5])-product(c1,v[7]);
    int64_t bias=INT64_C(1)<<(shift-1);
    out[0]=(int32_t)((base0+pair0+odd0+bias)>>shift);
    out[7]=(int32_t)((base0+pair0-odd0+bias)>>shift);
    out[1]=(int32_t)((base1+pair1+odd1+bias)>>shift);
    out[6]=(int32_t)((base1+pair1-odd1+bias)>>shift);
    out[2]=(int32_t)((base1-pair1+odd2+bias)>>shift);
    out[5]=(int32_t)((base1-pair1-odd2+bias)>>shift);
    out[3]=(int32_t)((base0-pair0+odd3+bias)>>shift);
    out[4]=(int32_t)((base0-pair0-odd3+bias)>>shift);
}

static uint8_t pixel(int32_t value)
{ return value < -128 ? 0 : value > 127 ? 255 : (uint8_t)(value+128); }

void ln_jpeg_idct8(uint8_t *output,const int32_t *coefficients,
                   const uint16_t *quantization,uint32_t stride)
{
    int32_t work[64],input[8],values[8];
    for(unsigned column=0;column<8;++column){
        const int32_t *c=coefficients+column;
        int32_t ac=c[8]|c[16]|c[24]|c[32]|c[40]|c[48]|c[56];
        if(ac){
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
            for(unsigned row=0;row<8;++row){
                unsigned index=row*8+column;
                input[row]=coefficients[index]*(int32_t)quantization[index];
            }
            synthesize(input,values,16);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
            for(unsigned row=0;row<8;++row)work[row*8+column]=values[row];
        }else{
            int32_t dc=c[0]*(int32_t)quantization[column]*16;
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
            for(unsigned row=0;row<8;++row)work[row*8+column]=dc;
        }
    }
    for(unsigned row=0;row<8;++row){
        const int32_t *v=work+row*8;
        if(v[1]|v[2]|v[3]|v[4]|v[5]|v[6]|v[7]){
            synthesize(v,values,27);
            for(unsigned x=0;x<8;++x)output[x]=pixel(values[x]);
        }else{
            uint8_t dc=pixel((v[0]+64)>>7);
            for(unsigned x=0;x<8;++x)output[x]=dc;
        }
        output+=stride;
    }
}
