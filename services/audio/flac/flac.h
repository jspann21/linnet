#ifndef LINNET_AUDIO_FLAC_H
#define LINNET_AUDIO_FLAC_H
#include "services/audio/codec_pcm.h"
#include "third_party/flac/decoder.h"
#define LINNET_FLAC_SEEK_POINTS 64u
struct ln_flac_point { uint64_t sample,offset; };
struct ln_flac {
    ln_pcm_read_fn read;void *context;uint64_t file_size,cursor,audio_offset,total_frames,frame,discard_to;
    uint32_t sample_rate,channels,bits,phase,metadata_left,metadata_type,seek_count;
    uint32_t have,read_pending,decoded_frames,decoded_cursor;bool last_metadata,pending;
    int error;FLACContext decoder;
    struct ln_flac_point seek[LINNET_FLAC_SEEK_POINTS];
    uint8_t header[34],buffer[MAX_FRAMESIZE];
    int32_t samples[2][MAX_BLOCKSIZE];
};
void ln_flac_init(struct ln_flac *,ln_pcm_read_fn,void *,uint64_t file_size);
int ln_flac_open_step(struct ln_flac *);
int ln_flac_decode(struct ln_flac *,int16_t *,uint32_t capacity,uint32_t *frames);
/* Uses the closest usable live position or native SEEKTABLE point, falling
 * back to the stream start. Decoding/discard advances in bounded calls. */
bool ln_flac_seek(struct ln_flac *,uint64_t frame);
#endif
