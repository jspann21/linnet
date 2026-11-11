#ifndef LINNET_AUDIO_VORBIS_H
#define LINNET_AUDIO_VORBIS_H
#include "services/audio/codec_pcm.h"
#include "third_party/vorbis/ivorbiscodec.h"
#if !defined(__arm__)
#include <setjmp.h>
#endif
#define LINNET_VORBIS_POOL (768u*1024u)
struct ln_vorbis {
    ln_pcm_read_fn read;void*context;uint64_t file_size,cursor,frame,total_frames,discard_to;
    uint32_t sample_rate,channels,headers,page_have,page_need,segments,segment,body,packet_size;
    uint32_t serial,sequence,packets,pool_high,pool_used,scratch_count;
    int error;bool pending,page_ready,page_eos,ended,opened;
    uint64_t page_granule;
    vorbis_info info;vorbis_dsp_state dsp;vorbis_block block;
    ogg_packet op;void *scratch[256];
#if defined(__arm__)
    uintptr_t jump[1];
#else
    jmp_buf jump;
#endif
    uint8_t page[65307];
    uint8_t packet[65536] __attribute__((aligned(8)));
    uint8_t pool[LINNET_VORBIS_POOL] __attribute__((aligned(8)));
};
_Static_assert(sizeof(struct ln_vorbis)<=1024u*1024u,"Vorbis context exceeds fixed memory budget");
void ln_vorbis_init(struct ln_vorbis*,ln_pcm_read_fn,void*,uint64_t file_size);
int ln_vorbis_open_step(struct ln_vorbis*);
int ln_vorbis_decode(struct ln_vorbis*,int16_t*,uint32_t capacity,uint32_t*frames);
/* Seeking restarts and decodes/discards in bounded calls; no fast index claim.
 * Chained/multiplexed logical streams explicitly return unsupported. */
bool ln_vorbis_seek(struct ln_vorbis*,uint64_t frame);
void ln_vorbis_close(struct ln_vorbis*);
#endif
