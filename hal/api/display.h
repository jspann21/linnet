#ifndef LINNET_HAL_DISPLAY_H
#define LINNET_HAL_DISPLAY_H
#include "linnet_target.h"

#define LINNET_DISPLAY_PIXELS (LINNET_UI_WIDTH * LINNET_UI_HEIGHT)
#define LINNET_DISPLAY_BYTES (LINNET_DISPLAY_PIXELS * 2u)

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Live presentation certificate fields; no cached state or COP control. */
struct ln_display_presentation {
    uint32_t frames_submitted, frames_completed, failures, resets, generation;
};
void ln_display_presentation_snapshot(struct ln_display_presentation *output);

struct ln_display_metrics {
    uint32_t frames_submitted, frames_completed, jobs_completed, pairs_written;
    uint32_t queue_high_water, failures, last_error, resets;
    uint32_t generation, queued_jobs, cop_capable, cop_active;
    uint32_t max_present_us, head, tail;
};

uint32_t ln_display_target_diagnostics(uint32_t selector,uint32_t output[32]);
bool ln_display_init(void);
/* High byte: pause/fill/resume stage; lower bytes retain failure diagnostics. */
uint32_t ln_display_init_error(void);
uint16_t *ln_display_framebuffer(void); /* CPU-owned target-native RGB565 draw buffer. */
/* Optional idle-only full-frame transaction. NULL retains ordinary rendering.
 * On success, synchronously repaint the target framebuffer and call present before
 * any other executive work. Ordinary framebuffer access and snapshots return
 * NULL during the transaction. Row scanout imports this frame on next access;
 * column scanout transposes the cached draw buffer at publication. */
uint16_t *ln_display_direct_framebuffer(void);
bool ln_display_damage(int32_t x, int32_t y, int32_t width, int32_t height);
/* Snapshot and queue for the panel frame pulse; false when busy or no damage. */
bool ln_display_present(void);
void ln_display_column_mode(bool enabled);
/* CPU fallback performs at most max_pairs steps (clamped to 256); COP mode
 * handles sync timeout and observes completion. True means healthy, including
 * idle or waiting for hardware; false means a presentation failure. */
bool ln_display_service(uint32_t max_pairs);
void ln_display_sync_irq(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_display_sync_diagnostics(uint32_t output[16]);
void ln_display_blit_diagnostics(uint32_t output[16]);
uint32_t ln_display_column_mismatches(void);
#endif
bool ln_display_busy(void);
#if LINNET_DISPLAY_PIPELINE
/* CPU scanout is waiting for the display controller; no framebuffer writes are runnable. */
bool ln_display_wait_deadline(uint32_t *deadline);
/* One frame may render while the display controller completes scanout. */
bool ln_display_can_render(void);
#endif
bool ln_display_cop_capable(void);
/* Upgrade the verified resident monitor's timer/progress race guard while it
 * is parked. One ARM condition field changes; no worker relocation or restart.
 * Accepts the already corrected monitor. Intended before enabling CPU cache. */
bool ln_display_upgrade_monitor(void);
bool ln_display_upgrade_blit(void);
/* Clock rendezvous diagnostics: error, capability, COP_CTL, enabled, state,
 * pause request, pause ACK, nonce ACK. Error codes: 1 capability absent,
 * 2 nonce timeout, 3 already parked by caller, 4 prepare-pause timeout,
 * 5 park timeout, 6 resume timeout, 7 cancellation timeout,
 * 8 unsupported monitor, 9 monitor patch verification failure. */
uint32_t ln_display_cop_error(void);
void ln_display_cop_diagnostics(uint32_t output[8]);
bool ln_display_reset(void); /* Quiesce, discard pending jobs, queue a full redraw. */
bool ln_display_quiesce(void);
void ln_display_get_metrics(struct ln_display_metrics *output);
/* Immutable until the next present or direct-frame transaction begins. */
const uint16_t *ln_display_snapshot(void);
uint32_t ln_display_snapshot_crc32(void);
size_t ln_display_capture(uint8_t *output, size_t capacity);
#endif
#endif
