#pragma once
#include <stdbool.h>
#include <stdint.h>
/* S5L8702 H.264 engine. All buffers are physical SDRAM addresses; the
 * caller owns cache maintenance and keeps them alive until completion. */
struct ln_vpu_picture {
    uint32_t control,descriptors,slices,width_mbs,height_mbs;
    uint32_t output[3],references[17][3];
};
bool ln_vpu_busy(void);
bool ln_vpu_open(void);
void ln_vpu_close(void);
bool ln_vpu_start(const struct ln_vpu_picture *picture);
int ln_vpu_poll(void);
extern uint32_t ln_vpu_status,ln_vpu_fault;
uint32_t ln_vpu_clock(void);
