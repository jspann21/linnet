#ifndef LINNET_MUSIC_BROWSER_H
#define LINNET_MUSIC_BROWSER_H
#include <stdbool.h>
#include <stdint.h>
#include "queue.h"

#define LN_MUSIC_BROWSER_ROWS 48u
/* Singleton, allocator-free browser. The outer executive calls step separately
 * from library_step. All other functions inspect memory or record intent only.
 * Owns the library's external group/member query slot exclusively. Selection and
 * prefetch slots remain available to the player. No group-sized allocation.
 * PODB group kinds must use the content compiler's ascending kind ordering.
 * Playback snapshots resolve the complete selected group before publication.
 */
bool ln_music_browser_expand(const struct ln_queue *queue);
bool ln_music_browser_take_expanded(struct ln_queue *queue);
void ln_music_browser_init(void);
bool ln_music_browser_actions(uint32_t row);
bool ln_music_browser_options(uint32_t row);
bool ln_music_browser_external_actions(uint32_t index,uint32_t kind,uint32_t count,const char *title);
bool ln_music_browser_take_search_return(void);
bool ln_music_browser_save_queue(const struct ln_queue *queue);
bool ln_music_browser_take_selection(uint32_t *row,uint32_t *first);
void ln_music_browser_notice(const char *text);
void ln_music_browser_suspend(bool suspend);
/* Visibility gates speculative cache work, never an accepted playback intent. */
void ln_music_browser_set_visible(bool visible);
bool ln_music_browser_window_needed(void);
bool ln_music_browser_query_idle(void);
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_COVERFLOW_PACKED)
bool ln_music_browser_history_scan_visible(void);
#endif
bool ln_music_browser_take_search(void);
bool ln_music_browser_take_coverflow(void);
void ln_music_browser_step(void);
/* Publish a bounded batch after the controller finishes metadata work. */
void ln_music_browser_publish(void);
uint32_t ln_music_browser_count(void);
const char *ln_music_browser_label(uint32_t row);
void ln_music_browser_window(uint32_t first);
void ln_music_browser_visible_rows(uint32_t rows);
bool ln_music_browser_select(uint32_t row);
bool ln_music_browser_shuffle_all(void);
bool ln_music_browser_row_navigates(uint32_t row);
bool ln_music_browser_open_album(uint32_t group,uint32_t count,const char *title);
struct ln_music_browser_queue {
    uint8_t library_hash[32];
    uint32_t count,cursor,tracks[LINNET_QUEUE_CAPACITY];
    uint32_t source,source_group,action;
    uint64_t frame;
    bool shuffle_all;
};
enum ln_music_browser_queue_status { LN_BROWSER_QUEUE_IDLE=0,LN_BROWSER_QUEUE_LOADING=1,
    LN_BROWSER_QUEUE_READY=2,LN_BROWSER_QUEUE_CAPACITY=-1,LN_BROWSER_QUEUE_INVALID=-2 };
/* Select normally requests the full group including tracks before cursor.
 * Groups over1024 publish a virtual source descriptor, bounded at100000 members.
 * tracks[] is populated only for source0; select_single explicitly requests one.
 * Window/navigation/new selection cancels and drains pending snapshot work. */
bool ln_music_browser_select_single(uint32_t row);
int ln_music_browser_queue_status(void);
bool ln_music_browser_take_queue(struct ln_music_browser_queue *out);
bool ln_music_browser_back(void);
const char *ln_music_browser_title(void);
const char *ln_music_browser_hint(void);
uint32_t ln_music_browser_revision(void);
bool ln_music_browser_busy(void);
bool ln_music_browser_waiting(void);
/* Advances for hierarchy changes, independently of cache publication. */
uint32_t ln_music_browser_navigation(void);
#endif
