#ifndef LINNET_MUSIC_DECODER_H
#define LINNET_MUSIC_DECODER_H
#include "services/audio/codec_pcm.h"
enum ln_decoder_kind {LN_DEC_UNKNOWN=0,LN_DEC_PCM,LN_DEC_MP3,LN_DEC_FLAC,LN_DEC_ALAC,LN_DEC_ADTS,LN_DEC_AAC_M4A,LN_DEC_VORBIS};
union ln_decoder_workspace;
#define LINNET_DECODER_SLOTS 2u
/* Fixed disjoint arenas, owned by the decoder service and never allocated or
 * cleared on lookup. Invalid slots return NULL. The player owns admission:
 * close and drain I/O before reuse; raw borrowing requires all playback idle. */
union ln_decoder_workspace *ln_decoder_workspace_at(uint32_t slot);
uint32_t ln_decoder_workspace_bytes(void);
/* Read-only hints for the player's file cache. Available while an M4A
 * container is being parsed; tables_ready requires completed container open.
 * A zero table length means no retained range is needed. */
struct ln_decoder_range {uint64_t offset,bytes;};
struct ln_decoder_cache_layout {
    uint64_t media_start,media_end;
    struct ln_decoder_range tables[2];bool tables_ready;
};
struct ln_decoder {
    union ln_decoder_workspace *data;
    ln_pcm_read_fn read;void *context;uint64_t size,total_frames;
    uint32_t kind,rate;bool opened;uint8_t signature[12];
};
void ln_decoder_init(struct ln_decoder *,union ln_decoder_workspace *,ln_pcm_read_fn,void *,uint64_t size);
int ln_decoder_decode(struct ln_decoder *,int16_t *,uint32_t,uint32_t *frames);
bool ln_decoder_seek(struct ln_decoder *,uint64_t frame);
/* Caller has drained/canceled the external read before either operation. */
void ln_decoder_read_drained(struct ln_decoder *);
void ln_decoder_close(struct ln_decoder *);
bool ln_decoder_cache_layout(const struct ln_decoder *,struct ln_decoder_cache_layout *);
#if !defined(LINNET_PROFILE_RELEASE)
struct ln_decoder_cache_status {
    uint64_t request_offset,read_offset;
    uint32_t request_bytes,read_bytes,packet,phase,bank;bool pending;
};
bool ln_decoder_cache_status(const struct ln_decoder *,struct ln_decoder_cache_status *);
#endif
#endif
