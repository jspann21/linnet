#ifndef LINNET_MUSIC_HISTORY_RECORD_H
#define LINNET_MUSIC_HISTORY_RECORD_H
#include <stdbool.h>
#include <stdint.h>
#define LN_MUSIC_JOURNAL_FAVORITE 1u
#define LN_MUSIC_JOURNAL_BOOKMARK 2u
#define LN_MUSIC_JOURNAL_RESUME 4u
struct ln_music_journal_record {
    uint64_t id;
    uint32_t flags,play_count;
    uint64_t bookmark_frame,resume_frame;
};
#endif
