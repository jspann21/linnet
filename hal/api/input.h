#ifndef LINNET_HAL_INPUT_H
#define LINNET_HAL_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#include "linnet_target.h"
#define LINNET_BUTTON_SELECT 1u
#define LINNET_BUTTON_RIGHT 2u
#define LINNET_BUTTON_LEFT 4u
#define LINNET_BUTTON_PLAY 8u
#define LINNET_BUTTON_MENU 16u
#if defined(LINNET_IMAGE_OS) && defined(LINNET_TARGET_CLASSIC6G_N25)
#define LINNET_INPUT_REMOTE 1
#define LINNET_REMOTE_PLAY_PAUSE 1u
#define LINNET_REMOTE_NEXT 2u
#define LINNET_REMOTE_PREVIOUS 4u
#endif
struct ln_input_state {
    uint32_t buttons, timestamp_us, packets, invalid_packets;
    int32_t wheel_delta;
    uint8_t wheel_position, touched, hold, headphones;
#if defined(LINNET_INPUT_REMOTE)
    /* One-shot media commands, independent of clickwheel keys and Hold. */
    uint8_t remote_actions;
    int8_t remote_volume;
#endif
};
bool ln_input_init(void);
#if defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK && \
    (LINNET_BOOT_MENU_QUERY_BEFORE_INPUT || LINNET_BOOT_MENU_QUERY_AFTER_INPUT)
/* Board policy selects whether this held-key query precedes input init. */
bool ln_input_boot_menu_held(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_input_boot_menu_diagnostics(uint32_t output[4]);
#endif
#endif
/* Return whether user-visible input changed. Successful unchanged polls still
 * fill out; sampling a sensor or receiving an idle packet is not activity. */
bool ln_input_poll(struct ln_input_state *out);
void ln_input_snapshot(struct ln_input_state *out);
#if defined(LINNET_IMAGE_OS) && defined(LINNET_TARGET_CLASSIC6G_N25)
bool ln_input_irq(void);
#endif
#if LINNET_RUNTIME_POWER_POLICY && defined(LINNET_IMAGE_OS)
/* Optional deadline for target-selected runtime power policies. */
uint32_t ln_input_deadline(uint32_t now);
#endif
#if !defined(LINNET_PROFILE_RELEASE)
void ln_input_diagnostics(uint32_t output[16]);
#endif
#endif
