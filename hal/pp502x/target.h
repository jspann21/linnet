#ifndef LINNET_PP502X_TARGET_H
#define LINNET_PP502X_TARGET_H
#include <stdbool.h>
#include <stdint.h>

/* Small board-policy surface implemented by each target. */
bool ln_target_battery_charging(void);

void ln_target_input_prepare(void);
uint8_t ln_target_power_sources(void);
struct ln_power_state;
void ln_target_power_prepare(struct ln_power_state *state);
void ln_target_usb_prepare(void);
uint16_t ln_target_charge_limit_get(void);
void ln_target_charge_limit_set(uint16_t milliamps);
void ln_target_backlight(bool on, uint8_t brightness);
void ln_target_ata_power(bool on);
void ln_target_switches_prepare(void);
uint32_t ln_target_switches_read(void);
void ln_target_switches_rearm(uint32_t sample);
/* Direct panel draw after the normal display owner has quiesced. */
bool ln_target_disk_screen(bool symbol_visible);
/* Normalized switch bits: bit 5 is Hold released, bit 7 headphones present. */

/* A target may fill older IDENTIFY fields that predate the ATA media/rotation
 * words.  Implementations must match an exact device identity and leave
 * unrecognized devices unchanged. */
void ln_target_ata_classify(const char *model,uint32_t *media_kind,uint32_t *rotation_rpm);

#endif
