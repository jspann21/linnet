#ifndef LINNET_AUDIO_DSP_H
#define LINNET_AUDIO_DSP_H
#include <stdint.h>
#include <stdbool.h>
#define LINNET_DSP_BANDS 5u
#define LINNET_DSP_UNITY 65536u
#define LINNET_DSP_PROFILE_BYTES 160u
enum ln_dsp_mode { LINNET_DSP_STEREO, LINNET_DSP_MONO, LINNET_DSP_LEFT,
    LINNET_DSP_RIGHT, LINNET_DSP_KARAOKE };
/* Q2.30 coefficients. Denominator: 1 + a1*z^-1 + a2*z^-2.
 * Frequency/Q/gain design belongs to the host; runtime does not infer presets. */
struct ln_dsp_biquad { int32_t b0,b1,b2,a1,a2; };
struct ln_dsp_config {
    uint32_t gain_q16,replaygain_q16;
    int32_t balance;
    uint32_t mode,band_count;
    struct ln_dsp_biquad bands[LINNET_DSP_BANDS];
    /* Zero amount bypasses crossfeed. Low-pass update coefficient is Q0.16;
     * host chooses it for the actual stream sample rate. */
    uint32_t crossfeed_amount_q16,crossfeed_coefficient_q16;
    /* Automatic DSP reserve only; the player compensates at the output after
     * matching PCM is audible. Manual gain/ReplayGain are never compensated.
     * Runtime metadata, absent from the LDSP file format. */
    uint32_t headroom_db_tenths;
};
struct ln_dsp_history { int32_t x1,x2,y1,y2; };
struct ln_dsp {
    struct ln_dsp_config config;
    struct ln_dsp_history history[LINNET_DSP_BANDS][2];
    int32_t crossfeed_low[2];
    uint32_t frames,output_clips,internal_clips;
};
void ln_dsp_init(struct ln_dsp *dsp);
/* dB is signedQ16.16. Unknown INT32_MIN meansunity. OutputQ16 is clamped
 * to1..1048576 (16x); inputs<=-96dB use the1LSB floor. Exp2 interpolation
 * contributes <0.003dB error before Q16 rounding. No floating-point/runtime. */
uint32_t ln_dsp_gain_from_db(int32_t db_q16);
/* LDSP bytes: magic0,reservedu16@4=0,sizeu16@6=160,rateu32@8,
 * flagsu32@12=0,reserved16..27=0; gainu32@28,ReplayGainu32@32,
 * balancei32@36,modeu32@40,bandcountu32@44,five(b0,b1,b2,a1,a2)i32
 * records@48,amountu32@148,lowpassu32@152,CRC32/ISO-HDLC@156 over0..155.
 * All integers little-endian; inactive band records must be zero. Decode is
 * atomic and requires an exact actual sample-rate match,8000..192000Hz. */
bool ln_dsp_profile_decode(const uint8_t *data,uint32_t bytes,uint32_t actual_rate,
                           struct ln_dsp_config *out);
/* Configuration rejects unstable filters atomically, retaining old settings.
 * Accepted changes reset histories. Apply at a stopped/paused stream boundary
 * to avoid an audible discontinuity; no implicit interpolation is performed. */
bool ln_dsp_configure(struct ln_dsp *dsp,const struct ln_dsp_config *config);
void ln_dsp_process(struct ln_dsp *dsp,int16_t *interleaved,uint32_t frames);
#endif
