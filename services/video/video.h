#ifndef LINNET_VIDEO_H
#define LINNET_VIDEO_H
#include <stdbool.h>
#include <stdint.h>
#include "ui/runtime/ui.h"
/* Caller must stop and drain music/photos before admission. Storage buffers and
 * audio ownership remain held until active() and pending() are both false. */
void ln_video_init(void);
void ln_video_library_load(void);
uint32_t ln_video_library_count(void);
uint32_t ln_video_library_revision(void);
/* False while metadata is unknown; a resolved empty/error list needs no wake. */
bool ln_video_library_empty(void);
const char *ln_video_library_label(uint32_t row);
const char *ln_video_library_path(uint32_t row);
bool ln_video_open(const char *path);
struct ln_fat32_file;
bool ln_video_open_file(const struct ln_fat32_file *,const char *title);
void ln_video_step(void);
void ln_video_stop(void);
bool ln_video_active(void);
/* Successful end of playback, after its resources have drained. Cleared by
 * the next open or an explicit stop; failures do not count as completion. */
bool ln_video_completed(void);
bool ln_video_pending(void);
/* Playback I/O owns video navigation; catalog prefetch remains independently
 * pending for shutdown/drain without intercepting unrelated button events. */
bool ln_video_playback_pending(void);
void ln_video_pause(bool paused);
bool ln_video_controls(bool show);
bool ln_video_paused(void);
bool ln_video_seek(int32_t seconds);
/* Presentation options: TV mode bits0..1 (off/ask/on), PAL bit2,
 * widescreen bit3 and S-Video bit4. */
uint8_t ln_video_options(void);
void ln_video_options_set(uint8_t options);
const char *ln_video_option_label(uint32_t row);
bool ln_video_option_adjust(uint32_t row,int32_t direction);
bool ln_video_output_prompt(void);
void ln_video_output_once(bool television);
void ln_video_headphones(bool connected);
uint32_t ln_video_progress(void); /* 0..1000 */
const char *ln_video_title(void);
const char *ln_video_status(void);
const struct ln_ui_asset *ln_video_asset(void);
uint32_t ln_video_revision(void);
/* Acknowledge only after ln_display_present accepted the offered snapshot. */
void ln_video_presented(void);
void ln_video_resume_get(uint32_t out[2]);
void ln_video_resume_set(const uint32_t in[2]);
uint32_t ln_video_deadline(uint32_t now);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_video_diagnostics(uint32_t out[32]);
#endif
#if !defined(LINNET_PROFILE_RELEASE)
/* Arm one next-open measurement while inactive/drained: 0 disarm, 1 normal
 * display, 2 consume due frames without display snapshots. Starts at frame0,
 * stops after8s playback (or8s preparation), preserves saved resume. */
bool ln_video_measurement_arm(uint32_t mode);
void ln_video_metadata(uint32_t page,uint8_t out[128]);
#endif
#endif
