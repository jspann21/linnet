#include "services/audio/vorbis/reconstruct.h"

static int32_t floor_scale(int32_t sample,int32_t gain)
{return (int32_t)((uint64_t)((int64_t)sample*gain)>>15);}

void ln_vorbis_floor_line(int first,int level,int end,int end_level,
                          const int32_t *gain,int32_t *spectrum,unsigned bins)
{
    if(first<0||end<=first||(unsigned)first>=bins)return;
    unsigned width=(unsigned)(end-first);
    unsigned length=bins-(unsigned)first;if(length>width)length=width;
    int delta=end_level-level,direction=delta<0?-1:1;
    unsigned magnitude=(unsigned)(delta<0?-delta:delta),step=0,remainder=magnitude;
    int32_t *out=spectrum+first;
    if(!magnitude){
        int32_t scale=gain[level];
        while(length--){*out=floor_scale(*out,scale);++out;}
        return;
    }
    /* Long shallow lines contain runs with identical gain. Find their lengths
     * once, then multiply each run without per-bin interpolation or lookup. */
    if(length>=64&&width>=8u*magnitude&&width<=65535){
        unsigned run=0,fraction=0;
        for(unsigned bit=16;bit;){
            --bit;fraction=(fraction<<1)|((width>>bit)&1u);
            if(fraction>=magnitude){fraction-=magnitude;run|=1u<<bit;}
        }
        unsigned carry=magnitude-1u;
        while(length){
            unsigned count=run;carry+=fraction;
            if(carry>=magnitude){carry-=magnitude;++count;}
            if(count>length)count=length;
            length-=count;int32_t scale=gain[level];
            while(count--){*out=floor_scale(*out,scale);++out;}
            level+=direction;
        }
        return;
    }
    /* Floor levels are 0..255. Divide only their eight significant bits,
     * rather than invoking a full software integer divide on ARM7. */
    if(magnitude>=width){
        remainder=0;
        for(unsigned bit=8;bit;){
            --bit;remainder=(remainder<<1)|((magnitude>>bit)&1u);
            if(remainder>=width){remainder-=width;step|=1u<<bit;}
        }
    }
    int increment=direction*(int)step;
    unsigned error=0;
    for(unsigned i=0;i<length;++i){
        out[i]=floor_scale(out[i],gain[level]);
        level+=increment;error+=remainder;
        if(error>=width){error-=width;level+=direction;}
    }
}

static uint32_t window_product(int32_t sample,int32_t coefficient)
{
    /* Each product is truncated before addition. Keeping this order preserves
     * the decoder's Q31 result, including the discarded low output bit. */
    return (uint32_t)((uint64_t)((int64_t)sample*coefficient)>>32)<<1;
}
void ln_vorbis_window_mix(int32_t *out,const int32_t *previous,
                          const int32_t *current,const int32_t *window,unsigned count)
{
    for(unsigned i=0;i<count;++i){
        int32_t a=previous[i],b=current[count-1u-i];
        int32_t low=window[i],high=window[2u*count-1u-i];
        uint32_t left=window_product(a,high)-window_product(b,low);
        uint32_t right=window_product(b,high)+window_product(a,low);
        out[i]=(int32_t)left;out[2u*count-1u-i]=(int32_t)right;
    }
}
