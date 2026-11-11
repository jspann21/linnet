#ifndef LINNET_MUSIC_QUEUE_H
#define LINNET_MUSIC_QUEUE_H
#include <stdint.h>
#include <stdbool.h>
#define LINNET_QUEUE_CAPACITY 1024u
#define LINNET_QUEUE_SOURCE_MAX 100000u
enum ln_queue_source {LN_QUEUE_EXPLICIT=0,LN_QUEUE_ALL=1,LN_QUEUE_GROUP=2};
#define LN_QUEUE_REVERSE 0x80000000u
#define LINNET_QUEUE_RESUME_MAX (80u+6u*LINNET_QUEUE_CAPACITY)
enum ln_queue_repeat { LINNET_QUEUE_REPEAT_NONE,LINNET_QUEUE_REPEAT_ALL,LINNET_QUEUE_REPEAT_ONE };
/* Explicit queue limit is 1024 entries; overflow returns false without truncation.
 * Track values are indexes into the caller's library. Caller must set and compare
 * library_identity (PODB payload SHA256) and validate indexes against track count.
 * This is queue/resume state, not a favorites/rating/play-count journal. */
struct ln_queue {
    /* tracks preserves occurrences (duplicates allowed); order permutes their indexes. */
    uint32_t tracks[LINNET_QUEUE_CAPACITY],order[LINNET_QUEUE_CAPACITY];
    uint32_t count,cursor,repeat,seed;
    uint32_t source,source_group,stride,offset,inverse,source_count,excluded_count;
    uint32_t excluded[32];
    bool shuffled;
    uint8_t library_identity[32];
    uint64_t position_samples;
    uint64_t last_track_id;
    int32_t volume_db_tenths;
};
/* Virtual current returns source ordinal (all-tracks ordinal is PODB index).
 * Group ordinals must be asynchronously resolved by the library. Virtual add/move
 * reject; removal stores at most32 source-ordinal exclusions and rejects overflow.
 * select_occurrence, shuffle/repeat preserve duplicate source occurrences.
 * Virtual shuffle uses a seeded coprime affine permutation (not uniform shuffle). */
bool ln_queue_track_at(const struct ln_queue *,uint32_t playback_ordinal,uint32_t *track_or_source_ordinal);
/* Prepare virtual exclusions once, then resolve against the same unchanged,
 * valid queue snapshot. ranks has room for32 entries and is caller-owned. */
bool ln_queue_exclusion_ranks(const struct ln_queue *,uint32_t ranks[32]);
bool ln_queue_track_at_ranks(const struct ln_queue *,uint32_t playback_ordinal,
                           const uint32_t ranks[32],uint32_t *source_ordinal);
bool ln_queue_select_at(struct ln_queue *,uint32_t playback_ordinal);
bool ln_queue_set_source(struct ln_queue *,uint32_t source,uint32_t group,uint32_t count,uint32_t ordinal);
void ln_queue_init(struct ln_queue *queue,uint32_t seed);
bool ln_queue_add(struct ln_queue *queue,uint32_t track);
bool ln_queue_select_occurrence(struct ln_queue *queue,uint32_t occurrence);
bool ln_queue_remove_occurrence(struct ln_queue *queue,uint32_t occurrence);
bool ln_queue_move_occurrence(struct ln_queue *queue,uint32_t occurrence,uint32_t index);
bool ln_queue_current(const struct ln_queue *queue,uint32_t *track);
bool ln_queue_next(struct ln_queue *queue,bool manual);
bool ln_queue_previous(struct ln_queue *queue);
void ln_queue_shuffle(struct ln_queue *queue,bool enabled);
bool ln_queue_set_repeat(struct ln_queue *queue,uint32_t repeat);
/* Encode returns bytes written or zero. Decode is atomic on invalid records.
 * Explicit state is at most6224 bytes. Virtual state is108+4*excluded_count,
 * at most236 bytes. Large queues use the collections sidecars.
 * Bytes0..3 are QUEU; byte4 selects explicit0 or virtual1 storage.
 * Common fields occupy0..75;76source,80group,84stride,88offset,92inverse,
 * 96source_count,100excluded_count,104 source ordinals; final4 bytes CRC32.
 * Format carries fixed fields plus original order and byte-index permutation;
 * explicit little endian, exact length, and CRC32. */
uint32_t ln_queue_encode(const struct ln_queue *queue,uint8_t *out,uint32_t capacity);
bool ln_queue_decode(struct ln_queue *queue,const uint8_t *data,uint32_t length);
#endif
