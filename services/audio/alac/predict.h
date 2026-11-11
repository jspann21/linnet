#pragma once
#include <stdint.h>

void ln_alac_predict(int32_t *samples,unsigned count,unsigned bits,int16_t *coefficients,
                      unsigned order,unsigned shift);
