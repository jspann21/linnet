#ifndef LINNET_VORBIS_RECONSTRUCT_H
#define LINNET_VORBIS_RECONSTRUCT_H
#include <stdint.h>
struct vorbis_dsp_state;
void ln_vorbis_floor_line(int,int,int,int,const int32_t *,int32_t *,unsigned);
void ln_vorbis_window_mix(int32_t *,const int32_t *,const int32_t *,const int32_t *,unsigned);
void ln_vorbis_overlap(unsigned,unsigned,unsigned,unsigned,int,const int32_t *,struct vorbis_dsp_state *);
#endif
