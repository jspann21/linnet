#ifndef LINNET_PHOTOS_H
#define LINNET_PHOTOS_H
#include <stdint.h>
#include <stdbool.h>
#include "ui/runtime/ui.h"
enum ln_photos_view {LN_PHOTOS_ALBUMS,LN_PHOTOS_LIST,LN_PHOTOS_VIEWER};
void ln_photos_init(void);
/* Prepare host-sized grid pages during the initial idle, awake startup. */
void ln_photos_prepare(bool eligible);
void ln_photos_step(void);
uint32_t ln_photos_deadline(uint32_t now);
void ln_photos_open(void);
struct ln_fat32_file;
bool ln_photos_open_file(const struct ln_fat32_file *,const char *title);
bool ln_photos_open_prepared(const struct ln_fat32_file *,uint32_t offset,uint32_t bytes,const char *title);
bool ln_photos_file_mode(void);
void ln_photos_close(void);
bool ln_photos_active(void);
bool ln_photos_pending(void);
/* Unfinished visible work takes precedence over speculative music metadata. */
bool ln_photos_busy(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_photos_diagnostics(uint32_t out[32]);
#endif
uint32_t ln_photos_view(void);
uint32_t ln_photos_count(void);
const char *ln_photos_label(uint32_t row);
void ln_photos_window(uint32_t first,uint32_t count);
const struct ln_ui_asset *ln_photos_thumbnail(uint32_t row);
const struct ln_ui_asset *ln_photos_asset(void);
bool ln_photos_select(uint32_t row);
bool ln_photos_back(void);
void ln_photos_next(int direction);
bool ln_photos_zoom(int direction);
bool ln_photos_pan(int64_t dx,int64_t dy);
void ln_photos_rotate(void);
void ln_photos_slideshow(bool enabled);
bool ln_photos_slideshow_active(void);
uint32_t ln_photos_revision(void);
uint32_t ln_photos_navigation(void);
uint32_t ln_photos_selection(void);
bool ln_photos_focus(uint32_t row);
const char *ln_photos_title(void);
const char *ln_photos_status(void);
/* 16-byte resume: catalog identity, photo index, zoom/rotation/view, packed pan.
 * Photo index65535 is outside every valid LPIC photo range and denotes an empty
 * album: zoom/rotation are zero, view is albums/list, and the pan word is the
 * album index. This fits the compact 16-bit index.
 * Restore validates catalog identity, album bounds and its empty record first. */
void ln_photos_resume_get(uint8_t out[16]);
bool ln_photos_resume_set(const uint8_t in[16]);
#endif
