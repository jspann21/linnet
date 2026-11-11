#ifndef LINNET_MUSIC_COLLECTIONS_H
#define LINNET_MUSIC_COLLECTIONS_H
#include <stdint.h>
#include <stdbool.h>
#include "queue.h"
#define LN_COLLECTION_SLOTS 16u
#define LN_COLLECTION_BYTES 147456u
#define LN_COLLECTION_RESUME (LINNET_QUEUE_RESUME_MAX+44u)
struct ln_collection {char name[32];uint32_t count;uint64_t ids[LINNET_QUEUE_CAPACITY];};
void ln_collections_step(bool may_read,bool may_write);
bool ln_collections_ready(void);
bool ln_collections_writable(void);
bool ln_collections_busy(void);
bool ln_collections_dirty(void);
const char *ln_collections_status(void);
uint32_t ln_collections_revision(void);
/* Name points into the current saved bank; consume before a playlist edit. */
const char *ln_collection_name(uint32_t slot);
/* Read-only live-bank view. Consume or copy before a playlist edit or reload;
 * persistence readback does not replace this storage. */
const struct ln_collection *ln_collection_get(uint32_t slot);
bool ln_collection_put(uint32_t slot,const struct ln_collection *value);
int ln_collection_free(void);
void ln_collections_played(uint64_t id);
uint32_t ln_collections_recent_rank(uint64_t id);
uint32_t ln_collections_recent_count(void);
bool ln_collections_recent_id(uint32_t position,uint64_t *id);
bool ln_collections_resume_set(const uint8_t *data,uint32_t length);
uint32_t ln_collections_resume_get(uint8_t *out,uint32_t capacity);
#endif
