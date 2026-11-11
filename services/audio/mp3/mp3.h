#ifndef LINNET_MP3_H
#define LINNET_MP3_H
#include <stdbool.h>
#include <stdint.h>
#include "third_party/libmad/mad.h"
typedef int (*ln_mp3_read_fn)(void *,uint64_t,uint8_t *,uint32_t);
enum ln_mp3_result {LN_MP3_PENDING=0,LN_MP3_READY=1,LN_MP3_EOF=2,LN_MP3_IO=-1,LN_MP3_FORMAT=-2,LN_MP3_ARGUMENT=-3,LN_MP3_UNSUPPORTED=-4};
struct ln_mp3 {
    struct mad_stream stream;struct mad_frame frame;struct mad_synth synth;
    mad_fixed_t subband[2][36][32],overlap[2][32][18];
    uint8_t reservoir[MAD_BUFFER_MDLEN] __attribute__((aligned(4)));
    uint8_t input[131072+MAD_BUFFER_GUARD] __attribute__((aligned(4)));
    ln_mp3_read_fn read;void *context;uint64_t file_size,read_offset,decoded_frames,total_frames,seek_target;
    uint32_t valid,retained,read_bytes,pcm_cursor,rate,channels,compressed_frames,recoveries;
    uint32_t lead_trim,tail_trim,trim_remaining;
    uint32_t unsynced_bytes,consecutive_recoveries;
    uint8_t index_data[560];
    uint64_t index_original,index_next,index_proof_at;
    uint32_t index_prefix_crc,index_expected_crc,index_record_number;
    uint32_t index_samples,index_rate,index_channels,index_lead,index_tail;
    uint64_t index_total;
    uint8_t index_state;
    bool index_unusable;
    bool initialized,need_input,refilling,pending,eof,first_frame_checked,gapless,seeking,synth_pending;int error;
};
/* Caller owns all storage. Exact asynchronous read returns0 pending/1 complete/
 * negative failure. Do not reset or release until a pending read has drained. */
void ln_mp3_init(struct ln_mp3 *,ln_mp3_read_fn,void *,uint64_t file_size);
/* One I/O request, spectral decode, or synthesis step per call. Output may drain over multiple
 * calls; capacity is stereo frames. No resampling. Validated
 * Xing/Info + LAME/Lavc/Lavf extension enables encoder delay/padding trimming
 * and total_frames; gapless=false means exact trims/frame count are unknown.
 * Initial ID3v2 tag is bounded/skipped; malformed frames count in recoveries.
 * Reject after64KiB newly read without a valid frame, or65536 consecutive
 * recoverable errors. Initial ID3 bytes are excluded; a valid frame resets
 * both budgets. Large corrupt gaps require an explicit file repair.
 * At physical EOF, a gapless stream missing exactly its final MPEG frame
 * may finish at the actual decoded length if no earlier recovery occurred.
 * No silence is inserted; total_frames is corrected before reporting EOF.
 * CRC-protected frames retain libmad verification. Only Layer III is exposed. */
int ln_mp3_decode(struct ln_mp3 *,int16_t *stereo,uint32_t capacity,uint32_t *frames);
/* Exact PCM-frame seek by scanning frames with a bounded decode preroll.
 * Positions use the trimmed timeline when gapless metadata exists. Admission
 * fails while read ownership is pending or target exceeds a known total.
 * Keep calling decode: seeking stays true and frames=0 until the target is
 * reached. Each call scans or decodes at most one MPEG frame.
 * Without a known total, reaching EOF before target reports ARGUMENT. */
bool ln_mp3_seek(struct ln_mp3 *,uint64_t frame);
#endif
