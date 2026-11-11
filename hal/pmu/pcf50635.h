#ifndef LINNET_PCF50635_H
#define LINNET_PCF50635_H
#include <stdint.h>
#include <stdbool.h>
typedef bool (*ln_pcf_read)(uint8_t reg,uint8_t *bytes,unsigned count);
typedef bool (*ln_pcf_write)(uint8_t reg,uint8_t value);
struct ln_pcf50635_adc {uint32_t deadline;uint16_t raw,millivolts;uint8_t phase,low;};
bool ln_pcf50635_battery_begin(struct ln_pcf50635_adc *state,ln_pcf_write write,uint32_t now);
/* One bus read per poll; 0 pending, 1 valid, -1 failed. */
int ln_pcf50635_battery_poll(struct ln_pcf50635_adc *state,ln_pcf_read read,uint32_t now);
#endif
