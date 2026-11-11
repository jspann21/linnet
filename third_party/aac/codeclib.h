#ifndef LINNET_AAC_CODECLIB_H
#define LINNET_AAC_CODECLIB_H
#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
/* Aligned may-alias words keep freestanding memory operations efficient on
 * ARM7 while byte prefixes/tails preserve arbitrary-address semantics. */
typedef uint32_t ln_faad_word __attribute__((__may_alias__));
static inline void *ln_faad_copy(void *dst,const void *src,size_t n)
{
    uint8_t *d=dst;const uint8_t *s=src;
    if((((uintptr_t)d^(uintptr_t)s)&3u)==0){
        while(n&&((uintptr_t)d&3u)){*d++=*s++;n--;}
        while(n>=16){
            ln_faad_word *dw=(ln_faad_word *)(void *)d;
            const ln_faad_word *sw=(const ln_faad_word *)(const void *)s;
            dw[0]=sw[0];dw[1]=sw[1];dw[2]=sw[2];dw[3]=sw[3];d+=16;s+=16;n-=16;
        }
        while(n>=4){*(ln_faad_word *)(void *)d=*(const ln_faad_word *)(const void *)s;d+=4;s+=4;n-=4;}
    }
    while(n--)*d++=*s++;
    return dst;
}
static inline void *ln_faad_set(void *dst,int v,size_t n)
{
    uint8_t *d=dst;uint8_t byte=(uint8_t)v;
    while(n&&((uintptr_t)d&3u)){*d++=byte;n--;}
    uint32_t word=(uint32_t)byte*0x01010101u;
    while(n>=16){ln_faad_word *w=(ln_faad_word *)(void *)d;w[0]=word;w[1]=word;w[2]=word;w[3]=word;d+=16;n-=16;}
    while(n>=4){*(ln_faad_word *)(void *)d=word;d+=4;n-=4;}
    while(n--)*d++=byte;
    return dst;
}
static inline void *ln_faad_move(void *dst,const void *src,size_t n)
{
    uint8_t *d=dst;const uint8_t *s=src;
    if((uintptr_t)d<=(uintptr_t)s)return ln_faad_copy(dst,src,n);
    d+=n;s+=n;
    if((((uintptr_t)d^(uintptr_t)s)&3u)==0){
        while(n&&((uintptr_t)d&3u)){*--d=*--s;n--;}
        while(n>=4){d-=4;s-=4;*(ln_faad_word *)(void *)d=*(const ln_faad_word *)(const void *)s;n-=4;}
    }
    while(n--)*--d=*--s;
    return dst;
}
#define memcpy ln_faad_copy
#define memset ln_faad_set
#define memmove ln_faad_move
#define assert(x) ((void)0)
#define DEBUGF(...) ((void)0)
#define betoh32(x) __builtin_bswap32((uint32_t)(x))
#define swap32(x) __builtin_bswap32((uint32_t)(x))
#define LIKELY(x) __builtin_expect(!!(x),1)
#define BS_LOG2 0
static inline unsigned bs_generic(uint32_t x,int mode){(void)mode;unsigned n=0;while(x>1){x>>=1;n++;}return n;}
static inline uint32_t ln_faad_div(uint32_t n,uint32_t d){uint32_t q=0;uint64_t r=0;if(!d)return 0;for(unsigned i=32;i;i--){r=(r<<1)|((n>>(i-1))&1u);if(r>=d){r-=d;q|=1u<<(i-1);}}return q;}
void ff_imdct_calc(unsigned int,int32_t *,const int32_t *);
void ff_imdct_half(unsigned int,int32_t *,const int32_t *);
#endif
