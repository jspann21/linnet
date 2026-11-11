#ifndef LINNET_RUNTIME_H
#define LINNET_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>
#include "linnet_target.h"
enum { LN_INPUT_WHEEL=1, LN_INPUT_BUTTON=2, LN_INPUT_HOLD=3, LN_INPUT_HEADPHONE=4 };
enum { LN_CONTROL_SLEEP=1,LN_CONTROL_WAKE=2,LN_CONTROL_BACKLIGHT=3,LN_CONTROL_COP_RESET=4,LN_CONTROL_ANIMATION=5,LN_CONTROL_CLEAR_SYNTHETIC=6,LN_CONTROL_RESET_SHELL=7,LN_CONTROL_DISPLAY_DIAGNOSTIC=8 };
bool ln_runtime_init(void);
bool ln_runtime_video_open(const char *path);
bool ln_runtime_video_requested(void);
struct ln_fat32_file;
bool ln_runtime_video_open_file(const struct ln_fat32_file *,const char *title);
bool ln_runtime_first_frame_ready(void);
void ln_runtime_step(void);
#if LINNET_RUNTIME_POWER_POLICY && defined(LINNET_IMAGE_OS)
void ln_runtime_power_begin(void);
void ln_runtime_power_end(void);
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
/* Foreground checkpoint only; preserves runtime power/status ownership without
 * dispatching input, storage, music, or shutdown while a decoder is unwinding. */
void ln_runtime_power_checkpoint(void);
#endif
/* Called after the reboot ACK drains; cleanup runs in the outer executive. */
void ln_runtime_restart(void);
/* Main loop only: media/storage callbacks must unwind before shutdown. */
void ln_runtime_restart_step(void);
#if !defined(LINNET_PROFILE_RELEASE)
/* Dirty-render and publication inclusive timing; development OS only. */
void ln_runtime_render_timing(uint32_t words[16]);
void ln_runtime_exec_diagnostics(uint32_t words[24]);
#endif
void ln_runtime_idle(void);
int ln_runtime_can_inject(uint32_t first_sequence,uint32_t count);
int ln_runtime_inject(uint32_t kind,int32_t value,uint32_t sequence);
uint32_t ln_runtime_state(uint32_t *output,uint32_t capacity_words);
int ln_runtime_control(uint32_t command,uint32_t argument);
const uint16_t *ln_runtime_framebuffer(void);
uint32_t ln_runtime_ui_state(uint32_t *output,uint32_t capacity_words);
#endif
