#include "services/audio/vorbis/reconstruct.h"
#include "third_party/vorbis/ivorbiscodec.h"

static void copy_samples(int32_t *to,const int32_t *from,unsigned count)
{for(unsigned i=0;i<count;++i)to[i]=from[i];}

void ln_vorbis_overlap(unsigned block,unsigned previous,unsigned small,unsigned large,
                       int channels,const int32_t *window,struct vorbis_dsp_state *state)
{
    (void)small;
    unsigned quarter=block/4u,old_quarter=previous/4u;
    unsigned shared=quarter<old_quarter?quarter:old_quarter;
    unsigned leading=old_quarter-shared,trailing=quarter-shared;
    unsigned output_samples=quarter+old_quarter;
    for(int channel=0;channel<channels;++channel){
        int32_t *current=state->residues[state->ri]+(unsigned)channel*block/2u;
        int32_t *saved=state->saved_ptr[channel];
        int32_t *output=state->floors+(unsigned)channel*output_samples;
        copy_samples(output,saved,leading);
        ln_vorbis_window_mix(output+leading,saved+leading,current,window,shared);
        copy_samples(output+leading+shared*2u,current+shared,trailing);
        if(state->residues[1])state->saved_ptr[channel]=current+quarter;
        else{
            copy_samples(saved,current+quarter,quarter);
            state->saved_ptr[channel]=state->saved+(unsigned)channel*large/4u;
        }
        state->pcmb[channel]=output;
    }
    if(state->residues[1])state->ri^=1;
}
