#include "services/audio/aac/window.h"

/* Portable host counterpart of window_arm.S, including its Q31 truncation. */
static uint32_t product(int32_t sample,int32_t window)
{
    return (uint32_t)((uint64_t)((int64_t)sample*window)>>32)<<1;
}

void ln_aac_window_add(int32_t *out,const int32_t *samples,const int32_t *window,
                       const int32_t *overlap,unsigned count)
{
    for(unsigned i=0;i<count;++i)
        out[i]=(int32_t)((uint32_t)overlap[i]+product(samples[i],window[i]));
}

void ln_aac_window_reverse(int32_t *out,const int32_t *samples,
                           const int32_t *window,unsigned count)
{
    for(unsigned i=0;i<count;++i)
        out[i]=(int32_t)product(samples[i],window[count-i-1]);
}
