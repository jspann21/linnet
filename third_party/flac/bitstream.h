/* Linnet bounded bitreader adaptation for the retained LGPL FLAC decoder. */
#ifndef LINNET_FLAC_BITS_H
#define LINNET_FLAC_BITS_H
#include <stdint.h>
#include <stdbool.h>
#define ICODE_ATTR_FLAC
#define ICONST_ATTR
#define MAX_BLOCKSIZE 4608
typedef struct {const uint8_t *buffer;int size_in_bits,index;bool error;} GetBitContext;
static inline void init_get_bits(GetBitContext *g,const uint8_t *p,int n){*g=(GetBitContext){p,n,0,false};}
static inline unsigned get_bits(GetBitContext *g,int n){
    if(n<0||n>32||g->index>g->size_in_bits-n){g->error=true;return 0;}
    uint32_t value=0;
    while(n){uint32_t remain=8u-((uint32_t)g->index&7u),take=(uint32_t)n<remain?(uint32_t)n:remain;
        value=(value<<take)|((g->buffer[(uint32_t)g->index>>3]>>(remain-take))&((1u<<take)-1u));g->index+=(int)take;n-=(int)take;}
    return value;
}
static inline unsigned get_bits1(GetBitContext*g){return get_bits(g,1);}
static inline int get_sbits(GetBitContext*g,int n){uint32_t value=get_bits(g,n);if(n<=0||n>32)return 0;if(n==32)return (int32_t)value;uint32_t sign=1u<<(n-1);return (int32_t)((value^sign)-sign);}
static inline int get_bits_count(const GetBitContext*g){return g->index;}
static inline void skip_bits(GetBitContext*g,int n){(void)get_bits(g,n);}
static inline void align_get_bits(GetBitContext*g){skip_bits(g,(8-(g->index&7))&7);}
static inline int av_log2(unsigned n){int value=0;while(n>>=1)++value;return value;}
#endif
