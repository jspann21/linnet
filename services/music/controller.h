#ifndef LINNET_MUSIC_CONTROLLER_H
#define LINNET_MUSIC_CONTROLLER_H
#include "services/music/library.h"
void ln_music_init(void);
struct ln_fat32_file;
bool ln_music_open_file(const struct ln_fat32_file *,const char *title);
bool ln_music_file_mode(void);
uint32_t ln_music_option_count(void);
void ln_music_step(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_music_budget_diagnostics(uint32_t output[12]);
#endif
/* One-shot sleep timer; input never resets it. Timer state is boot-local. */
bool ln_music_sleep_adjust(uint32_t row,int32_t direction);
const char *ln_music_sleep_label(uint32_t row);
const char *ln_music_sleep_status(void);
uint32_t ln_music_sleep_revision(void);
bool ln_music_sleep_step(uint32_t now);
/* Consumes an executive-issued UI intent after browser track admission. */
bool ln_music_take_now_playing(void);
bool ln_music_select(uint32_t index);
/* Music Quiz temporarily owns playback without replacing the user's queue.
 * End restores the original occurrence, position, and pause state. */
bool ln_music_quiz_begin(uint32_t index);
bool ln_music_quiz_select(uint32_t index);
void ln_music_quiz_end(void);
bool ln_music_next(bool previous);
void ln_music_file_transport_finished(bool found);
void ln_music_toggle(void);
void ln_music_stop(void);
bool ln_music_seek(uint64_t frame);
bool ln_music_volume(int32_t steps);
bool ln_music_volume_sync(void);
int32_t ln_music_output_volume(void);
void ln_music_headphones(bool connected);
/* Terminal normal shutdown / Disk Mode only. Caller must admit with storage
 * idle; freezes intents, drains read owners and bounds persistence work. Never
 * invoke from panic/crash recovery. False leaves admission frozen. */
bool ln_music_prepare_shutdown(void);
bool ln_music_quiesce(bool save_settings);
uint32_t ln_music_playback_modes(void);
uint32_t ln_music_option_action(uint32_t row);
uint32_t ln_music_queue_action(uint32_t row);
const char *ln_music_option_label(uint32_t index);
bool ln_music_option_select(uint32_t index);
uint32_t ln_music_revision(void);
uint32_t ln_music_sound_count(void);
uint32_t ln_music_sound_action(uint32_t row);
const char *ln_music_sound_title(void);
bool ln_music_sound_pending(void);
const char *ln_music_sound_status(void);
bool ln_music_sound_enter(uint32_t row);
bool ln_music_sound_back(uint32_t *row);
const char *ln_music_sound_label(uint32_t index);
bool ln_music_sound_adjust(uint32_t index,int32_t direction);
bool ln_music_save_queue(void);
void ln_music_queue_open(void);
void ln_music_queue_close(void);
void ln_music_queue_choose(uint32_t row);
bool ln_music_queue_edit(uint32_t action);
const char *ln_music_notice(void);
bool ln_music_loading(void);
uint32_t ln_music_deadline(uint32_t now);
const struct ln_music_library_row *ln_music_current(void);
/* Retained current playback occurrence; position is one-based. No I/O. */
bool ln_music_queue_position(uint32_t *position,uint32_t *count);
#endif
