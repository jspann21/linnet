#ifndef LINNET_AAC_BITS_H
#define LINNET_AAC_BITS_H
#include <stdint.h>
/* Input is borrowed for a synchronous call. Cached words never read outside
 * the access unit. Lookahead zero extends; an overrun on consumption fails. */
struct ln_aac_bits {
    const uint8_t *data;
    uint32_t size,endbit,left,current,next;
    uint8_t error,no_more_reading;
};
void ln_aac_bits_init(struct ln_aac_bits *,const void *,uint32_t);
void ln_aac_bits_advance(struct ln_aac_bits *,uint32_t);
static inline uint32_t ln_aac_bits_look(const struct ln_aac_bits *b,uint32_t count)
{
    if(!count||count>32)return 0;
    uint32_t mask=UINT32_MAX>>(32u-count);
    if(count<=b->left)return (b->current>>(b->left-count))&mask;
    unsigned extra=count-b->left;
    uint32_t value=extra==32?0:b->current<<extra;
    return (value|(b->next>>(32u-extra)))&mask;
}
static inline void ln_aac_bits_skip(struct ln_aac_bits *b,uint32_t count)
{
    if(count<b->left)b->left-=count;
    else ln_aac_bits_advance(b,count);
}
static inline uint32_t ln_aac_bits_read(struct ln_aac_bits *b,uint32_t count)
{
    uint32_t value=ln_aac_bits_look(b,count);
    ln_aac_bits_skip(b,count);
    return value;
}
static inline uint32_t ln_aac_bits_one(struct ln_aac_bits *b)
{
    if(b->left>1)return (b->current>>--b->left)&1u;
    return ln_aac_bits_read(b,1);
}
static inline uint32_t ln_aac_bits_position(const struct ln_aac_bits *b)
{return b->endbit-b->left;}
static inline uint8_t ln_aac_bits_align(struct ln_aac_bits *b)
{uint8_t count=(uint8_t)((0u-ln_aac_bits_position(b))&7u);ln_aac_bits_skip(b,count);return count;}
#endif
