#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ln_mpeg4_init(const uint8_t *config,uint32_t bytes,uint32_t width,uint32_t height);
int ln_mpeg4_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels);
void ln_mpeg4_close(void);
bool ln_mpeg4_async(void);
void ln_mpeg4_render(void);
void ln_mpeg4_diagnostics(uint32_t out[3]);
extern uint32_t ln_mpeg4_decode_us;
#ifdef __cplusplus
}
#endif
