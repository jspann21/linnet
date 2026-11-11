#ifndef LINNET_MUSIC_PROFILE_H
#define LINNET_MUSIC_PROFILE_H
#include "services/audio/dsp.h"
#define LN_MUSIC_PROFILE_BYTES 32u
enum ln_music_eq_preset {
    LN_MUSIC_EQ_FLAT,LN_MUSIC_EQ_ACOUSTIC,LN_MUSIC_EQ_BASS_BOOSTER,
    LN_MUSIC_EQ_CLASSICAL,LN_MUSIC_EQ_ELECTRONIC,LN_MUSIC_EQ_HIP_HOP,
    LN_MUSIC_EQ_LOUDNESS,LN_MUSIC_EQ_LOUNGE,LN_MUSIC_EQ_SMALL_SPEAKERS,
    LN_MUSIC_EQ_SPOKEN_WORD,LN_MUSIC_EQ_TREBLE_BOOSTER,
    LN_MUSIC_EQ_VOCAL_BOOSTER,LN_MUSIC_EQ_CUSTOM
};
struct ln_music_profile {
    uint32_t preset,mode;int32_t balance,preamp_db_q16;
    uint32_t replaygain;bool crossfeed,lineout;
    int8_t eq_db[5];int32_t max_volume_db_tenths;
};
void ln_music_profile_default(struct ln_music_profile *);
bool ln_music_profile_valid(const struct ln_music_profile *);
/* Native rates only; selected ReplayGain is Q16 dB, INT32_MIN means unity. */
bool ln_music_profile_configure(const struct ln_music_profile *,uint32_t rate,
                               int32_t replaygain_db_q16,struct ln_dsp_config *out);
/* 32-byte PROF v2. CRC32 covers bytes 0..27.
 * Flags: track/crossfeed/album bits 0..2, five gain+6 nibbles at 3..22,
 * maximum attenuation in dB at 23..29 and dock line-out at bit 30. */
bool ln_music_profile_encode(const struct ln_music_profile *,uint8_t out[LN_MUSIC_PROFILE_BYTES]);
bool ln_music_profile_decode(struct ln_music_profile *,const uint8_t *,uint32_t bytes);
#endif
