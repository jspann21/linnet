#include "services/audio/aac/stereo.h"
#include "third_party/aac/faad/common.h"
#include "third_party/aac/faad/structs.h"
#include "third_party/aac/faad/syntax.h"

static int selected(const ic_stream *left,const ic_stream *right,unsigned group,unsigned band)
{
    unsigned code=right->sfb_cb[group][band];
    return (left->ms_mask_present==2||left->ms_used[group][band])&&
           code!=INTENSITY_HCB&&code!=INTENSITY_HCB2&&left->sfb_cb[group][band]!=NOISE_HCB;
}
/* Keep the sample loop separate so group traversal stays compact in the cache. */
static __attribute__((noinline)) void reconstruct(int32_t *l,int32_t *r,unsigned count)
{
    for(unsigned i=0;i<count;++i){
        uint32_t mid=(uint32_t)l[i],side=(uint32_t)r[i];
        l[i]=(int32_t)(mid+side);r[i]=(int32_t)(mid-side);
    }
}
void ln_aac_mid_side(const struct ic_stream *left,const struct ic_stream *right,
                     int32_t *l,int32_t *r,unsigned frame_length)
{
    if(!left->ms_mask_present)return;
    unsigned window=0,stride=frame_length/8u;
    for(unsigned group=0;group<left->num_window_groups;++group){
        unsigned windows=left->window_group_length[group];
        for(unsigned band=0;band<left->max_sfb;){
            if(!selected(left,right,group,band)){++band;continue;}
            unsigned first=left->swb_offset[band++];
            while(band<left->max_sfb&&selected(left,right,group,band))++band;
            unsigned end=left->swb_offset[band];
            /* Adjacent selected bands share one loop across the group's windows. */
            for(unsigned w=0;w<windows;++w){
                unsigned base=(window+w)*stride;
                reconstruct(l+base+first,r+base+first,end-first);
            }
        }
        window+=windows;
    }
}
