#pragma once
#include <stdint.h>
#include <stdbool.h>
bool ln_h264_init(const uint8_t *avcc,uint32_t bytes,uint32_t width,uint32_t height);
/* 2: hardware still owns the sample; call again with the same bytes.
 * 1: picture ready, 0: consumed without picture, negative: failure. */
#define LN_H264_PENDING 2
int ln_h264_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels);
/* Complete deferred color conversion before reading the output pixels.
 * Starting another sample overlaps this conversion with hardware decoding. */
bool ln_h264_async(void);
void ln_h264_render(void);
void ln_h264_close(void);
void ln_h264_diagnostics(uint32_t out[3]);
void ln_h264_output_size(uint32_t *width,uint32_t *height);
extern uint32_t ln_h264_decode_us;
