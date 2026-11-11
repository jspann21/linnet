#ifndef LINNET_TREMOR_COMPAT_H
#define LINNET_TREMOR_COMPAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#define ICODE_ATTR
#define ICONST_ATTR
#define IBSS_ATTR
#define ROCKBOX_LITTLE_ENDIAN 1
#define ICONST_ATTR_TREMOR_WINDOW
#define ICODE_ATTR_TREMOR_MDCT
#define MEM_ALIGN_ATTR __attribute__((aligned(8)))
#define MEM_ALIGN_UP(n) (((n)+7u)&~7u)
#define LIKELY(x) __builtin_expect(!!(x),1)
#define UNLIKELY(x) __builtin_expect(!!(x),0)
#define DEBUGF(...) ((void)0)
#define letoh32(x) (x)
#define betoh32(x) __builtin_bswap32((uint32_t)(x))
void *ln_tremor_stack(size_t);
#ifdef alloca
#undef alloca
#endif
#define alloca(n) ln_tremor_stack(n)
static inline void *ln_tremor_copy(void*d,const void*s,size_t n){uint8_t*a=d;const uint8_t*b=s;for(size_t i=0;i<n;++i)a[i]=b[i];return d;}
static inline void *ln_tremor_set(void*d,int v,size_t n){uint8_t*a=d;for(size_t i=0;i<n;++i)a[i]=(uint8_t)v;return d;}
static inline void *ln_tremor_move(void*d,const void*s,size_t n){uint8_t*a=d;const uint8_t*b=s;if((uintptr_t)a<(uintptr_t)b){for(size_t i=0;i<n;++i)a[i]=b[i];}else while(n){--n;a[n]=b[n];}return d;}
static inline int ln_tremor_cmp(const void*a,const void*b,size_t n){const uint8_t*x=a,*y=b;for(size_t i=0;i<n;++i)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static inline size_t ln_tremor_len(const char*p){size_t n=0;while(p[n])++n;return n;}
static inline int ln_tremor_abs(int n){return n<0?(int)(0u-(unsigned)n):n;}
static inline long ln_tremor_labs(long n){return n<0?(long)(0ul-(unsigned long)n):n;}
void ln_tremor_sort(void*,size_t,size_t,int(*)(const void*,const void*));
#define memcpy ln_tremor_copy
#define memset ln_tremor_set
#define memmove ln_tremor_move
#define memcmp ln_tremor_cmp
#define strlen ln_tremor_len
#define abs ln_tremor_abs
#define labs ln_tremor_labs
#define qsort ln_tremor_sort
void ff_imdct_half(unsigned int,int32_t*,const int32_t*);
#endif
