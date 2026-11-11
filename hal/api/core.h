#ifndef LINNET_HAL_CORE_H
#define LINNET_HAL_CORE_H
#include <stdbool.h>
#include <stdint.h>
#define LINNET_WAKE_TIMER 1u
#define LINNET_WAKE_INPUT 2u
#define LINNET_WAKE_GPIO 4u
#define LINNET_WAKE_USB 8u
struct ln_core_status {
    uint32_t clock_hz, clock_source, pll_control, cache_control;
    uint32_t sdram_valid, timer_valid, irq_count, unexpected_irq;
    uint32_t idle_count, idle_us, deadline_late_us, wake_sources;
    /* C dispatcher wall time; excludes assembly exception save/restore. */
    uint32_t irq_max_us;
};
bool ln_core_init(void);
uint32_t ln_core_take_wakes(void);
void ln_core_idle_until(uint32_t deadline_us);
/* Request an executive timer wake without sleeping or delaying an earlier wake. */
void ln_core_wake_at(uint32_t deadline_us);
void ln_core_usb_rearm(void);
void ln_core_input_rearm(void);
void ln_core_irq_dispatch(void);
void ln_core_irq(void);
void ln_core_snapshot(struct ln_core_status *out);
bool ln_core_clock_set(uint32_t hz);
/* 1 invalid target, 2 COP park, 3 stub size, 4 relock timer, 5 readback,
 * 6 COP resume; zero means the most recent transition succeeded. */
uint32_t ln_core_clock_failure(void);
bool ln_core_cop_pause(bool paused);
bool ln_core_cache_enable(void);
bool ln_core_cache_clean(void);
bool ln_core_cache_disable(void);
/* Publish newly written, caller-owned ARM code before its first dispatch. */
bool ln_core_code_publish(const void *address,uint32_t bytes);
uint32_t ln_core_irq_save(void);
void ln_core_irq_restore(uint32_t flags);
bool ln_core_delay_us(uint32_t delay);
void ln_core_watchdog_arm(void);
void ln_core_watchdog_feed(void);
/* Terminal shutdown only, after peripheral quiescence. Leaves CPU IRQ/FIQ
 * masked, disarms the shared COP lease, and disables/acknowledges Timer2. */
void ln_core_watchdog_disarm(void);
void ln_core_watchdog_fiq(void);
#endif
