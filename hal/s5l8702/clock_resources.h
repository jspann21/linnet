#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LN_S5L_PLL2_CPU 1u
#define LN_S5L_PLL2_AUDIO 2u
bool ln_s5l_pll2_acquire(uint32_t owner);
bool ln_s5l_pll2_release(uint32_t owner);
uint32_t ln_s5l_clock_resource_flags(void);
bool ln_s5l_dma_idle_gate(void);
