#ifndef LINNET_PHOTO_IDCT_H
#define LINNET_PHOTO_IDCT_H
#include <stdint.h>
/* Synchronous baseline 8x8 transform. Caller validates natural-order DC
 * -2048..2047, AC -1023..1023, and quantization 1..255. Inputs are immutable;
 * output has eight rows of eight bytes, stride >=8. No allocation or I/O. */
void ln_jpeg_idct8(uint8_t *output,const int32_t *coefficients,
                   const uint16_t *quantization,uint32_t stride);
#endif
