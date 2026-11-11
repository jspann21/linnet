#ifndef LINNET_AAC_STEREO_H
#define LINNET_AAC_STEREO_H
#include <stdint.h>
struct ic_stream;
void ln_aac_mid_side(const struct ic_stream *,const struct ic_stream *,int32_t *,int32_t *,unsigned);
#endif
