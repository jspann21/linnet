#ifndef LINNET_FILES_H
#define LINNET_FILES_H
#include "ui/runtime/ui.h"
void ln_files_init(void);
void ln_files_step(struct ln_ui_state *);
void ln_files_close(void);
bool ln_files_pending(void);
uint32_t ln_files_deadline(uint32_t now);
bool ln_files_input(struct ln_ui_state *,uint32_t kind,int32_t value);
uint32_t ln_files_count(void);
const char *ln_files_title(void);
const char *ln_files_label(uint32_t row);
const char *ln_files_hint(void);
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
/* Geometry only; keep the full existing Files cache when partitioning RAM. */
uint32_t ln_files_cache_maximum_bytes(void);
#endif
bool ln_files_navigates(uint32_t row);
bool ln_files_audio_next(bool previous);
bool ln_files_audio_pending(void);
void ln_files_audio_cancel(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_files_diagnostics(uint32_t words[16]);
#endif
#endif
