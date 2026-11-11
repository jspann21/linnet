/* Bounded Rice decoding shim; errors use the retained decoder's sentinel. */
#include <limits.h>
static inline int get_sr_golomb_flac(GetBitContext*g,int k,int limit,int escape)
{
    (void)escape;if(k<0||k>30)return INT_MIN;
    uint32_t q=0;
    while(!get_bits1(g)){if(g->error||q>=(uint32_t)limit||q>=(UINT32_MAX>>k))return INT_MIN;++q;}
    uint32_t value=(q<<k)|get_bits(g,k);if(g->error||value==UINT32_MAX)return INT_MIN;
    return (int32_t)(value>>1)^-(int32_t)(value&1u);
}
