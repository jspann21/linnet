#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Timer F is the executive deadline source; Timer E remains the clock. */
bool ln_s5l_deadline_arm(uint32_t microseconds);
bool ln_s5l_deadline_pending(void);
void ln_s5l_deadline_stop(void);
