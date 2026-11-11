#pragma once
#include <stdint.h>
#include <stdbool.h>
bool ln_video_decoder_init(uint32_t codec,const uint8_t *config,uint32_t bytes,uint32_t width,uint32_t height);
int ln_video_decoder_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels);
bool ln_video_decoder_async(void);
void ln_video_decoder_render(void);
void ln_video_decoder_close(void);
void ln_video_decoder_output_size(uint32_t *width,uint32_t *height);
void ln_video_decoder_diagnostics(uint32_t out[3]);
uint32_t ln_video_decoder_time(void);
