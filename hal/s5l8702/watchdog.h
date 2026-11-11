#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Timer A owns VIC source 8 as FIQ, independently of deadline Timer F. */
bool ln_s5l_watchdog_timer(uint32_t milliseconds);
void ln_s5l_watchdog_route(bool software);
void ln_s5l_watchdog_feed(void);
void ln_s5l_watchdog_stop(void);
void ln_s5l_watchdog_snapshot(uint32_t output[4]);
