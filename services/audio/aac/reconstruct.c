#include "services/audio/aac/reconstruct.h"
#include "third_party/aac/faad/common.h"
#include "third_party/aac/faad/structs.h"
#include "third_party/aac/faad/syntax.h"

static __attribute__((noinline)) void intensity_band(const int32_t *source,int32_t *dest,
                                                     unsigned count,int exponent,int32_t fraction,unsigned negative)
{
    /* Match ARM's register shifts, including large transmitted scale factors. */
    unsigned shift=(unsigned)(exponent<0?-exponent:exponent)&255u;
    for(unsigned i=0;i<count;++i){
        int32_t scaled=exponent<0?(int32_t)(shift<32?(uint32_t)source[i]<<shift:0u):
                                 source[i]>>(shift<32?shift:31);
        uint32_t value=(uint32_t)(((int64_t)scaled*fraction+(INT64_C(1)<<27))>>28);
        dest[i]=(int32_t)(negative?0u-value:value);
    }
}
void ln_aac_intensity(const struct ic_stream *left,const struct ic_stream *right,
                      const int32_t *source,int32_t *dest,unsigned frame_length)
{
    /* Q28 values of 2^(-r/4), rounded to the nearest integer. */
    static const int32_t fractions[4]={268435456,225726413,189812531,159612677};
    unsigned window=0,stride=frame_length/8u;
    for(unsigned group=0;group<right->num_window_groups;++group){
        for(unsigned band=0;band<right->max_sfb;++band){
            unsigned code=right->sfb_cb[group][band];
            if(code!=INTENSITY_HCB&&code!=INTENSITY_HCB2)continue;
            int scale=right->scale_factors[group][band];
            unsigned negative=(code==INTENSITY_HCB2)^
                (left->ms_mask_present==1&&left->ms_used[group][band]);
            unsigned start=right->swb_offset[band],count=right->swb_offset[band+1]-start;
            for(unsigned w=0;w<right->window_group_length[group];++w){
                unsigned offset=(window+w)*stride+start;
                intensity_band(source+offset,dest+offset,count,scale>>2,fractions[(unsigned)scale&3u],negative);
            }
        }
        window+=right->window_group_length[group];
    }
}
uint8_t ln_aac_pulse(const struct ic_stream *stream,int16_t *coefficients,unsigned count)
{
    const pulse_info *pulse=&stream->pul;
    if(pulse->number_pulse>=4||pulse->pulse_start_sfb>=52)return 15;
    unsigned position=stream->swb_offset[pulse->pulse_start_sfb];
    for(unsigned n=0;n<=pulse->number_pulse;++n){
        position+=pulse->pulse_offset[n];
        if(position>=count)return 15;
        int value=coefficients[position],amplitude=pulse->pulse_amp[n];
        coefficients[position]=(int16_t)(value+(value>0?amplitude:-amplitude));
    }
    return 0;
}
