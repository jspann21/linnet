#ifndef LINNET_VORBIS_BITS_H
#define LINNET_VORBIS_BITS_H
#include <stdint.h>
#include <stddef.h>
/* LSB-first packet cursor. Keep the cached byte pointer and byte/bit position
 * available to Tremor's Huffman fast path. Failure is sticky for consuming
 * operations; lookahead never changes the cursor. */
struct ln_vorbis_bits {
    long endbyte;int endbit;
    const uint8_t *buffer,*ptr;
    long storage;
};
void ln_vorbis_bits_init(struct ln_vorbis_bits *,const uint8_t *,int);
long ln_vorbis_bits_read(struct ln_vorbis_bits *,int);
static inline void ln_vorbis_bits_fail(struct ln_vorbis_bits *b)
{b->endbyte=b->storage;b->endbit=1;b->ptr=NULL;}
static inline long ln_vorbis_bits_value(const struct ln_vorbis_bits *b,unsigned count,unsigned span)
{
    if(!count)return 0;
    const uint8_t *p=b->ptr;
    uint32_t value=p[0];
    /* A field touches one to five bytes. Build the low word in little-endian
     * order, then splice the fifth byte after dropping the starting offset. */
    if(span>8){
        value|=(uint32_t)p[1]<<8;
        if(span>16){
            value|=(uint32_t)p[2]<<16;
            if(span>24)value|=(uint32_t)p[3]<<24;
        }
    }
    value>>=b->endbit;
    if(span>32)value|=(uint32_t)p[4]<<(32u-(unsigned)b->endbit);
    return (int32_t)(value&(UINT32_MAX>>(32u-(unsigned)count)));
}
static inline long ln_vorbis_bits_look(const struct ln_vorbis_bits *b,int count)
{
    if((unsigned)count>32u)return -1;
    unsigned span=(unsigned)b->endbit+(unsigned)count;
    if((long)((span+7u)>>3)>b->storage-b->endbyte)return -1;
    return ln_vorbis_bits_value(b,(unsigned)count,span);
}
static inline void ln_vorbis_bits_advance(struct ln_vorbis_bits *b,unsigned count)
{
    /* Divide first so even UINT_MAX cannot overflow the span calculation. */
    unsigned remainder=(count&7u)+(unsigned)b->endbit;
    unsigned whole=(count>>3)+(remainder>>3);
    if((long)whole+(long)((remainder&7u)!=0)>b->storage-b->endbyte){
        ln_vorbis_bits_fail(b);return;
    }
    b->endbyte+=(long)whole;if(whole)b->ptr+=whole;b->endbit=(int)(remainder&7u);
}
static inline long ln_vorbis_bits_bytes(const struct ln_vorbis_bits *b)
{return b->endbyte+(b->endbit!=0);}
#endif
