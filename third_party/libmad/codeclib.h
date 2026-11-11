/* Freestanding libmad adapter with memory and integer arithmetic helpers. */
#ifndef LINNET_LIBMAD_CODECLIB_H
#define LINNET_LIBMAD_CODECLIB_H
#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#define betoh32(x) __builtin_bswap32((uint32_t)(x))
/* Aligned may-alias words keep freestanding memory operations efficient on
 * ARM7 while byte prefixes/tails preserve arbitrary-address semantics. */
typedef uint32_t ln_mad_word __attribute__((__may_alias__));
static inline void *ln_mad_copy(void *dst,const void *src,size_t n)
{
    uint8_t *d=dst;const uint8_t *s=src;
    if((((uintptr_t)d^(uintptr_t)s)&3u)==0){
        while(n&&((uintptr_t)d&3u)){*d++=*s++;n--;}
        while(n>=16){
            ln_mad_word *dw=(ln_mad_word *)(void *)d;
            const ln_mad_word *sw=(const ln_mad_word *)(const void *)s;
            dw[0]=sw[0];dw[1]=sw[1];dw[2]=sw[2];dw[3]=sw[3];d+=16;s+=16;n-=16;
        }
        while(n>=4){*(ln_mad_word *)(void *)d=*(const ln_mad_word *)(const void *)s;d+=4;s+=4;n-=4;}
    }
    while(n--)*d++=*s++;
    return dst;
}
static inline void *ln_mad_set(void *dst,int v,size_t n)
{
    uint8_t *d=dst;uint8_t byte=(uint8_t)v;
    while(n&&((uintptr_t)d&3u)){*d++=byte;n--;}
    uint32_t word=(uint32_t)byte*0x01010101u;
    while(n>=16){ln_mad_word *w=(ln_mad_word *)(void *)d;w[0]=word;w[1]=word;w[2]=word;w[3]=word;d+=16;n-=16;}
    while(n>=4){*(ln_mad_word *)(void *)d=word;d+=4;n-=4;}
    while(n--)*d++=byte;
    return dst;
}
static inline void *ln_mad_move(void *dst,const void *src,size_t n)
{
    uint8_t *d=dst;const uint8_t *s=src;
    if((uintptr_t)d<=(uintptr_t)s)return ln_mad_copy(dst,src,n);
    d+=n;s+=n;
    if((((uintptr_t)d^(uintptr_t)s)&3u)==0){
        while(n&&((uintptr_t)d&3u)){*--d=*--s;n--;}
        while(n>=4){d-=4;s-=4;*(ln_mad_word *)(void *)d=*(const ln_mad_word *)(const void *)s;n-=4;}
    }
    while(n--)*--d=*--s;
    return dst;
}
#define memcpy ln_mad_copy
#define memset ln_mad_set
#define memmove ln_mad_move
/* 32 bounded iterations; libmad uses this only for MPEG header arithmetic. */
static inline uint32_t ln_mad_div(uint32_t numerator,uint32_t denominator)
{
    uint32_t quotient=0;uint64_t remainder=0;
    if(!denominator)return 0;
    for(unsigned i=32;i;i--){remainder=(remainder<<1)|((numerator>>(i-1))&1u);
        if(remainder>=denominator){remainder-=denominator;quotient|=1u<<(i-1);}}
    return quotient;
}
#endif
