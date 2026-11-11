#ifndef LINNET_AAC_M4A_H
#define LINNET_AAC_M4A_H
#include "aac.h"
#include "services/audio/m4a/demux.h"
struct ln_aac_m4a {
    struct ln_aac core;struct ln_m4a container;
    uint64_t total_frames,frame,seek_target;
    uint32_t rate,channels,lead_remaining;
    bool opened,seeking,pending,packet_ready;int error;
    uint64_t meta_cursor,meta_end,meta_ends[5],tag_frames;
    uint32_t meta_parents[5],meta_depth,meta_atoms,meta_phase,meta_type;
    uint32_t tag_lead,tag_tail;
    bool meta_done,tag_mean,tag_name,tag_data,tag_valid,tag_seen;
    uint64_t entry_frames;uint32_t entry_lead,entry_tail;
    uint8_t meta_buffer[256];
    uint64_t index_offset;
    uint32_t index_record,index_crc;
    bool index_pending,index_check,index_rejected;
    /* Separate metadata/media windows avoid a seek for every STCO, STSZ and AU. */
    ln_aac_read_fn source_read;void *source_context;
    uint64_t cache_offset[3],cache_request_offset;
    uint32_t cache_bytes[3],cache_request_bytes;
    uint8_t *cache_request_buffer;
    uint8_t cache_age[3],cache_filling;bool cache_pending;
    uint8_t read_cache[3][16384];
};
void ln_aac_m4a_init(struct ln_aac_m4a *,ln_aac_read_fn,void *,uint64_t file_size);
int ln_aac_m4a_open_step(struct ln_aac_m4a *);
int ln_aac_m4a_decode(struct ln_aac_m4a *,int16_t *,uint32_t capacity,uint32_t *frames);
/* Exact trimmed timeline seek. Host checkpoints bound the decode preroll;
 * unprepared files reuse valid forward state or restart. Pending reads must
 * drain before seeking. decode restores the checkpoint or reopens as needed. */
bool ln_aac_m4a_seek(struct ln_aac_m4a *,uint64_t frame);
#endif
