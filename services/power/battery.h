#ifndef LINNET_BATTERY_H
#define LINNET_BATTERY_H
#include "hal/api/power.h"
struct ln_battery_guard { uint32_t sample, sampled_us; uint8_t consecutive; };
/* Three fresh low samples request shutdown without optional persistence. */
bool ln_battery_shutdown_due(struct ln_battery_guard *,const struct ln_power_state *,uint32_t now);
#endif
