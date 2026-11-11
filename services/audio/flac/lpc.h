#pragma once
#include <stdint.h>

void ln_flac_lpc(int32_t *samples,unsigned count,const int32_t *coefficients,
                 unsigned order,unsigned shift,unsigned wide,unsigned bits);
