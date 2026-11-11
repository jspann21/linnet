#ifndef LINNET_M4A_DEMUX_H
#define LINNET_M4A_DEMUX_H
#include <stdbool.h>
#include <stdint.h>
typedef int (*ln_m4a_read_fn)(void *,uint64_t,uint8_t *,uint32_t);
enum ln_m4a_result { LN_M4A_PENDING=0,LN_M4A_READY=1,LN_M4A_EOF=2,
    LN_M4A_IO=-1,LN_M4A_FORMAT=-2,LN_M4A_UNSUPPORTED=-3,LN_M4A_ARGUMENT=-4 };
struct ln_m4a_time {uint32_t count,delta;};
struct ln_m4a_chunk {uint32_t first,count;};
struct ln_m4a {
    ln_m4a_read_fn read;void *context;
    uint64_t file_size,cursor,ends[8],atom_start,atom_end,mdat_start,mdat_end;
    uint64_t media_start[8],media_end[8],track_start;
    uint64_t sizes_offset,chunks_offset,packet_offset,total_frames;
    uint32_t parents[8],depth,atoms,tracks,seen,atom_type,body_bytes;
    uint32_t media_count,track_depth;
    uint32_t rate,channels,packets,chunks,default_size,chunk_width,time_count,chunk_count;
    uint32_t packet,packet_size,chunk_index,chunk_first,chunk_packets,chunk_ordinal;
    uint32_t size_scan,next_packet,next_chunk;
    uint64_t next_offset;
    bool next_valid;
    uint32_t skip,phase,decode_phase,max_packet_frames;
    uint32_t codec,asc_bytes,current_packet_frames,lead_trim,tail_trim;
    uint32_t movie_scale,edit_duration;
    uint64_t read_offset;
    uint32_t read_total,read_done;
    uint8_t asc[64],alac_config[24];
    struct ln_m4a_time times[128];struct ln_m4a_chunk map[1024];
    uint8_t buffer[32768];
    bool pending,opened,allow_aac,track_probe,time_regular,extends_to_eof;int error;
};
/* Shared bounded M4A demux for AAC and ALAC. codec is17 AAC-LC or18 ALAC.
 * read_packet returns1 with buffer/packet_size/current_packet_frames and advances
 * packet,0 pending,2 EOF. No next call until the consumer finishes this buffer.
 * total_frames is the encoded sample timeline. Discard lead_trim decoded frames;
 * each packet's valid frames are current_packet_frames (last may be shorter).
 * AAC reduces rounded timestamps to its fixed packet size. Edits may remove
 * less than one trailing AAC packet via tail_trim; movie-tick rounding alone
 * does not change the sample count. ALAC retains exact STTS packet timing.
 * A raw timeline seek stores the intra-packet discard in skip; caller consumes
 * it. Single nonnegative rate1 edit is accepted, negative/multiple edits reject.
 * ASC is <=64 bytes. AAC access units are <=8191 bytes. */
void ln_m4a_init(struct ln_m4a *,ln_m4a_read_fn,void *,uint64_t file_size);
int ln_m4a_open_step(struct ln_m4a *);
int ln_m4a_read_packet(struct ln_m4a *);
bool ln_m4a_seek(struct ln_m4a *,uint64_t frame);
bool ln_m4a_seek_indexed(struct ln_m4a *,uint32_t packet,uint64_t offset);
#endif
