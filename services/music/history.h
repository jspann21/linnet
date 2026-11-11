#ifndef LINNET_MUSIC_HISTORY_H
#define LINNET_MUSIC_HISTORY_H
#include <stdbool.h>
#include <stdint.h>
#include "services/music/history_record.h"
enum ln_music_history_error {
    LN_HISTORY_OK=0,LN_HISTORY_MISSING=-1,LN_HISTORY_CAPACITY=-2,
    LN_HISTORY_READ_A=-3,LN_HISTORY_READ_B=-4,LN_HISTORY_BOTH_CORRUPT=-5,
    LN_HISTORY_CONFLICT=-6,LN_HISTORY_WRITE=-7,LN_HISTORY_FLUSH=-8,
    LN_HISTORY_READBACK=-9,LN_HISTORY_MISMATCH=-10,LN_HISTORY_ACK=-11,
    LN_HISTORY_ENCODE=-12,LN_HISTORY_BIND=-13,LN_HISTORY_READ_ONLY=-14
};
/* Boot-only, idempotent initialization. A latched error cannot be cleared by
 * another init call. Missing banks are never silently created or zeroed.
 * Step belongs to the outer executive; it guards recursive calls from storage
 * progress. History I/O is asynchronous: headroom gates admission, and admitted
 * requests always drain.
 * The caller owns storage/device lifecycle and freezes mutations before shutdown.
 * After initial load, flushing remains possible after library cancellation;
 * the last verified library binding is retained for this shutdown snapshot. */
void ln_music_history_init(void);
/* Intent-only audible-ID pin. Preserves its cached page and absent-record proof
 * across upcoming prefetches; an uncached pin is reacquired when admission is
 * available. Set on current-track admission/promotion, release after final resume
 * capture or replace at next admission. Dirty pages still require normal flush.
 * Pin changes are rejected after freeze. */
void ln_music_history_pin_current(bool present,uint64_t id);
/* Terminal normal-shutdown barrier: reject new events while draining writes. */
void ln_music_history_freeze(void);
void ln_music_history_step(bool may_write);
/* Reads and writes may start with playback headroom. Each call advances bounded work. */
void ln_music_history_service(bool may_read,bool may_write,bool playback);
/* Uses the latest service permissions: admitted I/O stays immediate; eligible
 * metadata/publication phases receive a short deadline between operations. */
uint32_t ln_music_history_deadline(uint32_t now);
/* No I/O:1 is loaded and fully durable,0 needs later step(true),negative is a
 * permanent failure for this boot. The caller must bound a shutdown flush loop
 * and keep calling step(true) only after other storage/audio owners have drained. */
int ln_music_history_flush(void);
/* Ready means records were loaded. A subsequent I/O error retains readable RAM
 * records but rejects new mutations and all further I/O. Inspect error too. */
bool ln_music_history_ready(void);
int ln_music_history_error(void);
bool ln_music_history_busy(void);
bool ln_music_history_finishing(void);
bool ln_music_history_dirty(void);
uint32_t ln_music_history_revision(void);
bool ln_music_history_prefetch(uint64_t id);
/* Distinguishes a proven missing record from uncached data. One prefetch may
 * be active; callers retry admission/intent once ready. Cache pointers expire
 * at the next step. */
bool ln_music_history_record_ready(uint64_t id);
/* Per-ID capacity failure does not poison already accepted dirty records. */
int ln_music_history_record_error(uint64_t id);
const struct ln_music_journal_record *ln_music_history_get(uint64_t id);
/* Intent-only calls, safe from runtime callbacks. Rejected until loading and
 * binding complete, or after a permanent error. Full journal rejects a new ID
 * without eviction; a false return must not be presented as a persisted change.
 * Successful changes during an encoded write remain dirty for a later snapshot. */
bool ln_music_history_set_favorite(uint64_t id,bool favorite);
bool ln_music_history_set_bookmark(uint64_t id,bool present,uint64_t frame);
bool ln_music_history_set_resume(uint64_t id,bool present,uint64_t frame);
bool ln_music_history_played(uint64_t id);
#endif
