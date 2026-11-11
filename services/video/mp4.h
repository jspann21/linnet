#ifndef LINNET_VIDEO_MP4_H
#define LINNET_VIDEO_MP4_H
#include <stdbool.h>
#include <stdint.h>
/* Exact asynchronous reads: 1 complete, 0 pending, negative failure. The
 * caller retains the supplied buffer until completion and serializes reads. */
typedef int (*ln_mp4_read_fn)(void *,uint64_t,void *,uint32_t);
enum {LN_MP4_PENDING=0,LN_MP4_READY=1,LN_MP4_EOF=2,
      LN_MP4_IO=-1,LN_MP4_FORMAT=-2,LN_MP4_UNSUPPORTED=-3};
struct ln_mp4_packet {uint64_t offset,dts;uint32_t bytes,duration;bool key;};
struct ln_mp4 {
    ln_mp4_read_fn read;void *context;uint64_t bytes,cursor,ends[8],atom_end;
    uint64_t sizes,chunks,sync,maps,duration,decode_time,packet_offset;
    uint32_t parents[8],depth,atoms,tracks,phase,type,body_bytes,seen;
    uint32_t codec,width,height,timescale,config_bytes,nal_bytes,rate,channels,lead;
    uint32_t samples,chunk_count,sync_count,default_bytes,chunk_width;
    uint32_t time_count,map_count,packet,chunk,chunk_first,chunk_samples;
    uint32_t packet_phase,packet_bytes,next_sync,sync_index;
    uint32_t seek_phase,seek_packet,seek_lo,seek_hi;
    uint32_t map_index,map_first,map_end,map_samples,seek_map,seek_base;
    struct {uint32_t count,delta;} times[128];
    uint8_t config[512],buffer[4096];
    bool video,opened,audio,seek_forward;int error;
};
void ln_mp4_init(struct ln_mp4 *,ln_mp4_read_fn,void *,uint64_t bytes);
void ln_mp4_init_audio(struct ln_mp4 *,ln_mp4_read_fn,void *,uint64_t bytes);
int ln_mp4_open_step(struct ln_mp4 *);
/* Reads only the sample index. Payload remains in the native MP4 file. */
bool ln_mp4_seek(struct ln_mp4 *,uint64_t time,int direction);
int ln_mp4_seek_step(struct ln_mp4 *);
int ln_mp4_next(struct ln_mp4 *,struct ln_mp4_packet *);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_mp4_table_diagnostics(uint32_t output[12]);
#endif
#endif
