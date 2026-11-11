#ifndef LINNET_AAC_H
#define LINNET_AAC_H
#include <stdbool.h>
#include <stdint.h>
#define LINNET_AAC_WORKSPACE_BYTES 98304u
typedef int (*ln_aac_read_fn)(void *,uint64_t,uint8_t *,uint32_t);
enum ln_aac_result {LN_AAC_PENDING=0,LN_AAC_READY=1,LN_AAC_EOF=2,LN_AAC_IO=-1,LN_AAC_FORMAT=-2,LN_AAC_ARGUMENT=-3,LN_AAC_UNSUPPORTED=-4};
struct ln_aac {
    uint8_t workspace[LINNET_AAC_WORKSPACE_BYTES] __attribute__((aligned(8)));
    uint8_t input[8192];int16_t pcm[2048];
    ln_aac_read_fn read;void *context;uint64_t file_size,offset,decoded_frames,seek_target;
    uint32_t rate,channels,frame_bytes,pcm_frames,pcm_cursor,access_units;
    uint8_t phase,frequency_index;bool configured,pending,seeking,overlap_invalid;int error;
};
/* Caller-owned fixed storage. ADTS LC, mono/stereo, one raw block per frame.
 * CRC-protected ADTS, PCE, SBR/HE, ER, ADIF and frame-size960 unsupported.
 * Read callback exact/polling:0pending/1complete/negativefailure; retain source
 * and buffers until completion. No native allocation, resampling or ADTS trims. */
void ln_aac_init(struct ln_aac *,ln_aac_read_fn,void *,uint64_t file_size);
int ln_aac_decode(struct ln_aac *,int16_t *stereo,uint32_t capacity,uint32_t *frames);
/* Exact untrimmed ADTS PCM seek via bounded decode/discard. Forward/equal
 * targets reuse valid overlap; backward targets or invalid overlap restart.
 * No index: remaining
 * forward distance takes proportional work spread across decode calls.
 * Rejects pending reads; target beyond EOF reports ARGUMENT when discovered. */
bool ln_aac_seek(struct ln_aac *,uint64_t frame);
/* Raw AAC-LC for bounded container demux. ASC<=64 bytes; AU<=8191 bytes.
 * raw_init starts a fresh context (old read must have drained). raw_decode
 * exposes one1024-frame stereo PCM block in d->pcm with pcm_frames/cursor.
 * Caller consumes the block before next raw_decode; raw calls are synchronous
 * and bounded to one AU. Container owns encoder-delay/final-duration trimming.
 * Raw and streaming operations must never run concurrently or from an ISR. */
int ln_aac_raw_init(struct ln_aac *,const uint8_t *asc,uint32_t bytes);
int ln_aac_raw_decode(struct ln_aac *,const uint8_t *access_unit,uint32_t bytes);
/* Container-only full-AU discard: runs all codec recurrence and validation,
 * omits PCM16 conversion, and returns with the block already consumed.
 * Caller must account its timeline/trim before yielding. No PCM is exposed. */
int ln_aac_raw_discard(struct ln_aac *,const uint8_t *access_unit,uint32_t bytes);
/* Internal seek-only intermediate AU: validates/advances spectral state but
 * leaves overlap invalid. A normal discarded predecessor must repair it
 * before raw_decode; streaming retargets restart while invalid. */
int ln_aac_raw_skip_synthesis(struct ln_aac *,const uint8_t *access_unit,uint32_t bytes);
/* Portable LC recurrence checkpoint before an AU; overlap is rebuilt by
 * discarding a predecessor before exposing PCM. No pointers leave the core. */
bool ln_aac_checkpoint(struct ln_aac *,uint32_t state[3],bool restore);
#endif
