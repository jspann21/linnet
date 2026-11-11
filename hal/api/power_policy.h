#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "linnet_target.h"
/* Optional target-selected foreground policy. The kernel reports workload;
 * the board selects clocks and safe power states. No policy runs in Boot. */
#define LN_POWER_WORK_BUSY 1u
#define LN_POWER_WORK_PLAYING 2u
#define LN_POWER_WORK_SETTLED 4u
#define LN_POWER_WORK_NEXT_PENDING 8u
#define LN_POWER_WORK_AUDIO_FAULT 16u
struct ln_power_workload {
    uint32_t flags,queued_frames,rate,playback_generation,underruns;
};
bool ln_power_policy_begin(const struct ln_power_workload *work);
void ln_power_policy_end(void);
bool ln_power_policy_fast(bool interactive);
/* Opaque development report; its format/version is owned by the target. */
void ln_power_policy_diagnostics(uint32_t words[32]);
