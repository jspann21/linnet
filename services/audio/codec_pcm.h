#ifndef LINNET_CODEC_PCM_H
#define LINNET_CODEC_PCM_H
#include <stdbool.h>
#include <stdint.h>
/* Exact random read: 0 pending, 1 complete, negative failure. A pending read
 * is polled with identical arguments; the decoder owns its persistent buffer.
 * Source identity and exclusive decoder ownership must remain unchanged. */
typedef int (*ln_pcm_read_fn)(void *,uint64_t,uint8_t *,uint32_t);
enum ln_pcm_result { LN_PCM_PENDING=0,LN_PCM_READY=1,LN_PCM_EOF=2,
    LN_PCM_IO=-1,LN_PCM_FORMAT=-2,LN_PCM_UNSUPPORTED=-3,LN_PCM_ARGUMENT=-4 };
struct ln_pcm {
    ln_pcm_read_fn read; void *context;
    uint64_t file_size,limit,cursor,next,data_offset,data_size,total_frames,frame;
    uint32_t sample_rate,chunk_size,declared_frames,pending_bytes;
    uint16_t channels,bits,block_align;
    uint8_t phase,container,chunk_kind;
    bool big_endian,unsigned_8,have_format,have_data,pending;
    int error;
    uint8_t buffer[4096];
    /* One bounded sequential window amortizes sector reads across PCM blocks. */
    uint64_t cache_offset,cache_request_offset;
    uint32_t cache_bytes,cache_request_bytes;
    uint8_t read_cache[65536];
};
void ln_pcm_init(struct ln_pcm *,ln_pcm_read_fn,void *,uint64_t file_size);
int ln_pcm_open_step(struct ln_pcm *);
/* Capacity is stereo frames (two signed 16-bit samples). On pending, output
 * is untouched. Keep capacity unchanged until that read completes. No resample. */
int ln_pcm_decode(struct ln_pcm *,int16_t *,uint32_t capacity_frames,uint32_t *frames);
bool ln_pcm_seek(struct ln_pcm *,uint64_t frame);
#endif
