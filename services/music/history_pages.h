#ifndef LINNET_MUSIC_HISTORY_PAGES_H
#define LINNET_MUSIC_HISTORY_PAGES_H
#include "journal_pages.h"
const struct ln_history_page *ln_paged_history_page(uint32_t index);
#if LINNET_TARGET_NUMBER==25u
/* Pure readiness query; never requests a page or changes cache age. */
bool ln_paged_history_page_waiting(uint32_t index);
/* Pure, synchronous snapshot of a fully covered resident played set. The
 * caller owns both outputs and the shared 250us foreground budget. */
bool ln_paged_history_played_cached(uint64_t *ids,uint32_t *scores,uint32_t capacity,uint32_t *count,uint32_t began);
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_COVERFLOW_PACKED)
void ln_paged_history_scan_policy(bool active,bool bulk_allowed);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_paged_history_window_diagnostics(uint32_t words[8]);
#endif
#endif
void ln_paged_history_pin_current(bool present,uint64_t id);
void ln_paged_history_step(bool may_read,bool may_write);
bool ln_paged_history_ready(void);
int ln_paged_history_error(void);
bool ln_paged_history_busy(void);
bool ln_paged_history_io_pending(void);
/* A page write has completed; only flush, readback and acknowledgment remain. */
bool ln_paged_history_finishing(void);
uint32_t ln_paged_history_deadline(uint32_t now,bool may_read,bool may_write);
bool ln_paged_history_dirty(void);
uint32_t ln_paged_history_revision(void);
/* Negative until every page has been verified; includes accepted RAM changes. */
int ln_paged_history_bookmark_count(void);
bool ln_paged_history_favorites_empty(void);
bool ln_paged_history_played_empty(void);
bool ln_paged_history_bookmark_page(uint32_t index);
bool ln_paged_history_favorite_page(uint32_t index);
bool ln_paged_history_played_page(uint32_t index);
bool ln_paged_history_prefetch(uint64_t);
bool ln_paged_history_record_ready(uint64_t);
int ln_paged_history_record_error(uint64_t);
const struct ln_music_journal_record *ln_paged_history_get(uint64_t);
bool ln_paged_history_put(const struct ln_music_journal_record *);
#endif
