#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ln_mpeg4_picture {
    uint32_t input, bytes, bits, output, reference, work;
    uint32_t width, height, chroma_offset, type, rounding, quantizer, fcode;
    uint32_t dc_threshold, time_increment, time_bits;
};
bool ln_mpeg4_vpu_open(void);
void ln_mpeg4_vpu_close(void);
bool ln_mpeg4_vpu_busy(void);
bool ln_mpeg4_vpu_start(const struct ln_mpeg4_picture *picture);
int ln_mpeg4_vpu_poll(void);
extern uint32_t ln_mpeg4_vpu_status,ln_mpeg4_vpu_fault,ln_mpeg4_vpu_vld;
#ifdef __cplusplus
}
#endif
