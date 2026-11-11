#ifndef LINNET_MUSIC_LIBRARY_H
#define LINNET_MUSIC_LIBRARY_H
#include <stdbool.h>
#include <stdint.h>
#include "services/music/database.h"

#define LN_MUSIC_LIBRARY_ROWS 12u
enum ln_music_library_status {
    LN_LIBRARY_CLOSED=0,LN_LIBRARY_OPENING,LN_LIBRARY_VERIFYING,
    LN_LIBRARY_READY,LN_LIBRARY_ERROR,LN_LIBRARY_DRAINING
};
/* A publication marker exists, or its absence could not be established.
 * Reconnect to Media Manager to recover the interrupted library switch. */
#define LN_MUSIC_LIBRARY_RECOVERY_REQUIRED (-12)
struct ln_music_library_row {
    struct ln_music_track track;
    uint32_t index;
    char title[256],artist[256],path[256],album[256];
    char art_path[256];uint32_t art_crc;
    /* Album strings are resolved for playback selections/prefetch, not windows. */
    bool album_ready;
};
/* Init is boot-only. Reload/cancel preserve every admitted storage buffer until
 * its operation drains. Call step from the outer executive, never recursively
 * from storage_progress. Missing/corrupt databases stop in ERROR until reload. */
void ln_music_library_init(void);
void ln_music_library_step(void);
/* Ready memory work only; false while storage or audio reserve requires a yield. */
bool ln_music_library_can_continue(void);
uint32_t ln_music_library_deadline(uint32_t now);
void ln_music_library_reload(void);
void ln_music_library_cancel(void);
/* Pause new I/O during PCM low-water pressure; admitted operations still drain. */
void ln_music_library_pause(bool pause);
enum ln_music_library_status ln_music_library_status(void);
int ln_music_library_error(void);
#if defined(LINNET_TARGET_VIDEO5G_M25)
/* Drop startup lookahead before reuse or storage recovery. Admitted reads
 * retain their buffers and must drain before any buffer is repurposed. */
void ln_music_library_verification_reset(void);
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
/* Latched payload verification and browse readiness timestamps, modulo 32 bits;
 * a flags bit proves each timestamp exists. Reads are admitted payload reads. */
void ln_music_library_verification_diagnostics(uint32_t words[8]);
#endif
uint32_t ln_music_library_count(void);
const char *ln_music_library_title(uint32_t track_index);
/* Verified resident metadata only: never starts I/O or consumes the shared
 * query slot. Missing cache entries retain the ordinary asynchronous path.
 * Returned name pointers remain valid until library reload/cancel. */
bool ln_music_library_cached_group(uint32_t group,struct ln_music_group *out,const char **name);
bool ln_music_library_cached_member(uint32_t group,uint32_t ordinal,uint32_t *track);
/* Absolute member-table entry; callers with a freshly validated group may
 * reuse its bounded first/count within the same synchronous operation. */
bool ln_music_library_cached_member_at(uint32_t member,uint32_t *track);
/* Cover Flow alone may use the aligned, unoccupied tail after verification
 * and browse-index loading. Reacquire after reload; never access it while the
 * library is not READY or its identity differs from the catalog identity. */
uint8_t *ln_music_library_catalog_memory(uint32_t *bytes);
/* Same Cover Flow lease and release contract for unused extended metadata RAM. */
uint8_t *ln_music_library_art_memory(uint32_t *bytes);
/* Files can use the original metadata reservation only while M25 verified
 * metadata lives in extended RAM. Recheck on every access; reload or remount
 * revokes it before the original buffer can become database storage again. */
uint8_t *ln_music_library_files_memory(uint32_t *bytes,uint32_t *epoch);
uint32_t ln_music_library_group_count(void);
uint32_t ln_music_library_revision(void);
uint32_t ln_music_library_verified_bytes(void);
/* Immutable payload identity for binding queue/resume state to this database. */
bool ln_music_library_identity(uint8_t out[32]);

/* A window contains up to12 consecutive track-table rows. Cached pointers remain
 * valid until window/reload/cancel; callers must not retain them across step.
 * Revision advances per six filled slots, at end of library, and for a selected
 * row, limiting repaint storms while preserving completed-page publication. */
/* Disable speculative rows while retaining caches and draining any admitted read. */
void ln_music_library_window_enable(bool enabled);
void ln_music_library_window(uint32_t first);
const struct ln_music_library_row *ln_music_library_row(uint32_t track_index);
/* New selection supersedes an older unresolved selection, draining its read.
 * take_selection copies an immutable snapshot and consumes the result. */
bool ln_music_library_select(uint32_t track_index);
/* Resolve a stable ID through the selection slot; missing returns index UINT32_MAX. */
bool ln_music_library_select_id(uint64_t id);
bool ln_music_library_take_selection(struct ln_music_library_row *out);
/* Independent next-track metadata slot. Explicit selection has priority. */
bool ln_music_library_prefetch(uint32_t track_index);
/* Discard next-track metadata intent; an admitted reader drains before reuse. */
void ln_music_library_cancel_prefetch(void);
bool ln_music_library_take_prefetch(struct ln_music_library_row *out);

/* Dedicated playback resolver, independent of external browser/search results.
 * Intent supersedes prior request; use a fresh nonzero token per request. Old
 * admitted I/O drains before replacement. UINT32_MAX means ordinal out of range.
 * Cancellation/reload invalidates results. Resolve current then prefetch serially. */
bool ln_music_library_queue_member(uint32_t group,uint32_t ordinal,uint32_t token);
bool ln_music_library_take_queue_member(uint32_t *track,uint32_t *token);

/* One external query at a time. Search returns first matching track or UINT32_MAX.
 * Group lookup returns the record/name; member lookup maps group-relative ordinal
 * to a track. Results are consumed by the matching take function. Selection has
 * priority over an unstarted query; both have priority over cache filling. */
/* kind=0 is a track; 1/2 is an artist/album with a tagged group index. */
struct ln_music_library_search_row { uint32_t index,kind,count; char title[256]; };
bool ln_music_library_search_at(const char *prefix,uint32_t ordinal);
/* index UINT32_MAX means no matching result at this ordinal. */
bool ln_music_library_take_search_row(struct ln_music_library_search_row *out);
/* Uses the shared external query slot. source0/1 takes a track index;
 * source2 maps group-relative ordinal first. Out-of-range member returns
 * index UINT32_MAX. Matching take function alone consumes this result. */
bool ln_music_library_view_row(uint32_t source,uint32_t group,uint32_t ordinal);
bool ln_music_library_take_view_row(struct ln_music_library_search_row *out);
bool ln_music_library_track_record(uint32_t index);
bool ln_music_library_take_track_record(struct ln_music_track *out);
#define LN_MUSIC_ID_BATCH 30u
bool ln_music_library_track_ids(uint32_t first);
bool ln_music_library_take_track_ids(uint64_t ids[LN_MUSIC_ID_BATCH],uint32_t *count);
/* Returns true when the host-built ID index is resident; missing IDs yield UINT32_MAX. */
bool ln_music_library_track_id_lookup(uint64_t id,uint32_t *ordinal);
/* Iterate the resident host-built index in ID order. */
bool ln_music_library_id_entry(uint32_t position,uint64_t *id,uint32_t *ordinal);
bool ln_music_library_group(uint32_t group_index);
bool ln_music_library_group_range(uint32_t kind,uint32_t *first,uint32_t *count);
bool ln_music_library_albums(uint32_t artist,uint32_t *out,uint32_t capacity,uint32_t *count);
bool ln_music_library_group_members(uint32_t group_index,uint32_t ordinal);
bool ln_music_library_take_members(uint32_t indices[LN_MUSIC_ID_BATCH],uint32_t *count);
/* Range searches need the record kind, not its display name. */
bool ln_music_library_group_record(uint32_t group_index);
bool ln_music_library_take_group(struct ln_music_group *out,char name[256]);
bool ln_music_library_group_member(uint32_t group_index,uint32_t ordinal);
bool ln_music_library_take_member(uint32_t *track_index);
#endif
