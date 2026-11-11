#ifndef LINNET_ALAC_H
#define LINNET_ALAC_H
#include "services/audio/m4a/demux.h"
#include "third_party/alac/decomp.h"
typedef ln_m4a_read_fn ln_alac_read_fn;
enum ln_alac_result { LN_ALAC_PENDING=LN_M4A_PENDING,LN_ALAC_READY=LN_M4A_READY,LN_ALAC_EOF=LN_M4A_EOF,
    LN_ALAC_IO=LN_M4A_IO,LN_ALAC_FORMAT=LN_M4A_FORMAT,
    LN_ALAC_UNSUPPORTED=LN_M4A_UNSUPPORTED,LN_ALAC_ARGUMENT=LN_M4A_ARGUMENT };
struct ln_alac {
    struct ln_m4a container;
    alac_file core;
    uint64_t frame;
    uint32_t pcm_count,pcm_cursor;
    int32_t pcm[2][4096];
    /* Independent media/table windows; AAC supplies its own outer cache. */
    ln_alac_read_fn source_read;void *source_context;
    uint64_t cache_offset[2],cache_request_offset;
    uint32_t cache_bytes[2],cache_request_bytes;
    uint8_t *cache_request_buffer;
    uint8_t cache_victim,cache_filling;bool cache_pending;
    uint8_t read_cache[2][16384];
};
/* Exact immutable asynchronous reads, same ownership contract as PCM. Calls to
 * the upstream core are serialized: coefficient scratch is static. M4A support
 * is nonfragmented, one audio track, <=8 media extents, integer16 mono/stereo, <=4096 frames/packet,
 * <=32768 compressed bytes/packet, <=1M packets/chunks,
 * <=128 ALAC time runs and1024 sample-to-chunk runs. No resampling or encryption. */
void ln_alac_init(struct ln_alac *,ln_alac_read_fn,void *,uint64_t file_size);
int ln_alac_open_step(struct ln_alac *);
int ln_alac_decode(struct ln_alac *,int16_t *stereo,uint32_t capacity,uint32_t *frames);
bool ln_alac_seek(struct ln_alac *,uint64_t frame);
#endif
