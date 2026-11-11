#ifndef LINNET_AAC_RECONSTRUCT_H
#define LINNET_AAC_RECONSTRUCT_H
#include <stdint.h>
struct ic_stream;
void ln_aac_intensity(const struct ic_stream *,const struct ic_stream *,const int32_t *,int32_t *,unsigned);
uint8_t ln_aac_pulse(const struct ic_stream *,int16_t *,unsigned);
#endif
