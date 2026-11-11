#ifndef LINNET_HAL_POWER_H
#define LINNET_HAL_POWER_H
#include <stdbool.h>
#include <stdint.h>
#define LINNET_POWER_USB 1u
#define LINNET_POWER_FIREWIRE 2u
#define LINNET_POWER_CHARGING 4u
#define LINNET_POWER_BATTERY_VALID 8u
#define LINNET_POWER_I2C_FAULT 16u
struct ln_power_state {
    uint32_t sampled_us, next_sample_us, samples, failures, bus_resets;
    uint16_t battery_mv, battery_raw, charge_limit_ma;
    uint8_t flags, battery_percent, sleeping, backlight_on, brightness;
};
/* Cached retained PCF50605 RTC; invalid values must not be shown as real time. */
struct ln_clock_state {uint8_t hour,minute,second;bool valid;};
void ln_power_clock_snapshot(struct ln_clock_state *out);
struct ln_calendar { uint16_t year; uint8_t month,day,hour,minute,second,weekday; };
enum ln_calendar_result { LN_CALENDAR_INVALID=0, LN_CALENDAR_UNAVAILABLE,
    LN_CALENDAR_SAVED, LN_CALENDAR_RESTORED, LN_CALENDAR_UNKNOWN, LN_CALENDAR_PENDING };
/* Explicit foreground calls only. Caller rejects active/paused audio, video and
 * storage work. Never call from IRQ, rendering, startup or periodic power poll.
 * Set derives weekday. UNAVAILABLE means no write; UNKNOWN may be partial.
 * Calendar writes are not hardware-atomic. PENDING requires foreground polling;
 * each phase is bounded to 150ms plus one bus transaction. The 1 Hz RTC latch
 * is awaited asynchronously, including during rollback. */
enum ln_calendar_read_result { LN_CALENDAR_READ_UNAVAILABLE=0,
    LN_CALENDAR_READ_INVALID, LN_CALENDAR_READ_VALID };
enum ln_calendar_read_result ln_power_calendar_read_status(struct ln_calendar *out);
bool ln_power_calendar_read(struct ln_calendar *out);
enum ln_calendar_result ln_power_calendar_set(const struct ln_calendar *requested);
bool ln_power_calendar_busy(void);
enum ln_calendar_result ln_power_calendar_poll(void);

bool ln_power_init(void);
/* Advances at most one periodic I2C transfer attempt. During a multi-step
 * sample, next_deadline requests prompt continuation. ADC work expires after
 * 100ms and RTC work after 60ms; cancellation retains the logical phase and
 * age, restarting RTC at its pointer. samples/sampled_us change only when a
 * complete ADC result (success or terminal invalid/fault outcome) is published. */
bool ln_power_poll(uint32_t now, struct ln_power_state *out);
void ln_power_snapshot(struct ln_power_state *out);
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
/* Read-only sampler timing and last ADC failure; no bus transaction. */
void ln_power_sampler_diagnostics(uint32_t words[16]);
/* PTI1: per-attempt CPU idle time and lifetime timer lateness. Current idle is
 * UINT32_MAX outside an active ADC; failed idle remains frozen. No I2C. */
void ln_power_sampler_trace(uint32_t words[16]);
/* Pure diagnostic observation after CPU idle; safe with interrupts masked. */
void ln_power_idle_observe(uint32_t requested,uint32_t actual,uint32_t power_delta);
void ln_power_sampler_idle_trace(uint32_t words[16]);
#endif
uint32_t ln_power_next_deadline(void);
uint8_t ln_power_sources(void); /* Current USB/FireWire presence, without I2C. */
void ln_power_backlight(bool on, uint8_t brightness);
bool ln_power_sleep(void);
bool ln_power_wake(void);
/* Shutdown maps USB/FIREWIRE flags to PCF CHGWAK/EXTONWAK respectively.
 * These are PMU wake policy bits, not a guarantee of port-specific wake.
 * Nonzero wake policy is mandatory; caller quiesces storage/audio/USB first. */
bool ln_power_shutdown(uint8_t wake_sources);
/* 500 mA is permitted only after the caller verifies USB's negotiated budget. */
bool ln_power_charge_limit(uint16_t milliamps);
/* Single executive owner; IRQ handlers must never perform I2C transactions. */
bool ln_power_i2c_read(uint8_t address, uint8_t reg, uint8_t *out, uint32_t length);
bool ln_power_i2c_write(uint8_t address, uint8_t reg, uint8_t value);
#endif
