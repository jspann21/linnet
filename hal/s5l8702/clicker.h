#pragma once
#include <stdbool.h>
/* Timer G shares the 32-bit timer IRQ with the executive's Timer F. */
bool ln_s5l_click_irq(void);
