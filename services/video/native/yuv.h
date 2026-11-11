#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ln_yuv_init(uint32_t width,uint32_t height,uint32_t left,uint32_t top);
void ln_yuv_size(uint32_t *width,uint32_t *height);
void ln_yuv_convert(const uint8_t *data,uint32_t stride,uint32_t rows,uint16_t *pixels);
void ln_yuv_convert_nv12(const uint8_t *data,uint32_t stride,uint32_t chroma_offset,uint16_t *pixels);
extern uint32_t ln_yuv_convert_us;
#ifdef __cplusplus
}
#endif
