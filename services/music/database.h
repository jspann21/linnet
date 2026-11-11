#ifndef LINNET_MUSIC_DATABASE_H
#define LINNET_MUSIC_DATABASE_H
#include <stdbool.h>
#include <stdint.h>
#include "boot/include/sha256.h"
#define LN_MUSIC_SEARCH_GROUP 0x80000000u
#define LN_MUSIC_TRACK_BYTES 132u
bool ln_music_db_utf8(const uint8_t *s,uint32_t n);

typedef int (*ln_music_db_read_fn)(void *,uint64_t,uint8_t *,uint32_t);
enum ln_music_db_result { LN_DB_PENDING=0,LN_DB_READY=1,LN_DB_IO=-1,LN_DB_FORMAT=-2,LN_DB_ARGUMENT=-3 };
enum ln_music_db_request { LN_DB_TRACK=1,LN_DB_GROUP,LN_DB_MEMBER,LN_DB_ART,LN_DB_STRING };
struct ln_music_track {
    uint64_t id,file_bytes,data_offset,data_bytes,frames;
    uint32_t rate,codec,title,artist,album,path,genre,composer,year,number,disc,art;
    int32_t replaygain,album_replaygain;uint16_t channels,bits;uint8_t sha256[32];
};
struct ln_music_group { uint32_t kind,name,parent,first,count;uint64_t id; };
struct ln_music_art {uint64_t id;uint32_t path,crc;};
struct ln_music_db {
    ln_music_db_read_fn read;void *context;struct ln_sha256 hash;
    uint32_t size,generation,tracks,groups,members,searches,arts;
    uint32_t track_offset,group_offset,member_offset,search_offset,art_offset,pool_offset,pool_bytes;
    uint32_t cursor,index,read_offset,read_size,low,high,middle,string_offset;
    uint32_t member,search_index,search_track,search_ordinal;
    /* One completed lower bound for prefix in this immutable DB generation.
     * Ordinary record/string reads may reuse scratch without losing the key.
     * The small key cache contains a fully validated title's first 32 bytes;
     * cache hits are limited to <=32-byte prefixes and <=32-byte titles.
     * Longer public prefixes or titles retain the full read and comparison
     * path, so d->text remains a complete validated title. */
    uint32_t search_lower,search_prefix_length,search_cache_middle,search_cache_member;
    bool search_lower_valid,search_cache_valid;
    char search_cache_key[33];
    uint8_t phase,operation,query_phase;bool verified,busy,pending;int error;
    uint8_t expected_hash[32];char text[256],prefix[256];
    struct ln_music_track track;struct ln_music_group group;struct ln_music_art art;
    uint8_t buffer[4096];
};
/* Exact-read callback returns0 pending/1 complete/negative failure. Identical
 * polls retain buffer ownership. The source and generation must stay immutable.
 * No cancellation/reset is permitted until the outstanding read drains. */
void ln_music_db_init(struct ln_music_db *,ln_music_db_read_fn,void *,uint32_t size,uint32_t generation);
/* READY/verified proves header geometry, CRC, whole-payload SHA and pool edges.
 * Record fields, string boundaries and UTF-8 are checked on access. This does
 * not prove global search sort/permutation, ID uniqueness, group parent kinds,
 * or media content integrity; those require the trusted compiler/publication
 * validation and the caller's media size/hash/path checks. SHA is not auth. */
int ln_music_db_open_step(struct ln_music_db *);
bool ln_music_db_request(struct ln_music_db *,enum ln_music_db_request,uint32_t index);
int ln_music_db_step(struct ln_music_db *);
/* Prefix must already be Unicode casefolded UTF-8; ASCII A-Z folding is accepted.
 * Result is the first matching search index/reference, or UINT32_MAX.
 * Group references carry LN_MUSIC_SEARCH_GROUP. */
/* Bounded lower-bound lookup plus ordinal; consecutive ordinals of the same
 * normalized prefix reuse its completed bound. Each result still validates
 * its index/string and prefix. No match returns UINT32_MAX. */
bool ln_music_db_search_at(struct ln_music_db *,const char *prefix,uint32_t ordinal);
/* Validate direct 8.3 absolute media/artwork path after reading its string. */
bool ln_music_db_safe_path(const char *path);
#endif
