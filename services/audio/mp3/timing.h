#ifndef LINNET_MP3_TIMING_H
#define LINNET_MP3_TIMING_H
#include <stdint.h>

#if !defined(LINNET_PROFILE_RELEASE)
struct ln_mp3_timing {
    uint32_t frame_calls,frame_us,frame_max_us;
    uint32_t synth_calls,synth_us,synth_max_us;
    uint32_t pcm_calls,pcm_frames,pcm_us,pcm_max_us;
};
void ln_mp3_timing_snapshot(struct ln_mp3_timing *);
#endif
#endif
