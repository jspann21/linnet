/* CS42L55 playback configuration: Cirrus DS773F1, sections 4.11/4.13/6.
 * Register compensation is manufacturer-specified data. */
#include "cs42l55.h"
#include "hal/api/core.h"
static bool output_muted=true,lineout_enabled;
static uint8_t output_gain;
static bool lineout(ln_codec_writer write,bool enabled,bool mute) {
    uint16_t value=enabled&&!mute?output_gain:0xc0;
    bool left=write(0x1c,value),right=write(0x1d,value);return left&&right;
}
static bool set_output_gain(ln_codec_writer write,uint8_t gain){
    bool left=write(0x1a,gain),right=write(0x1b,gain);
    if(!lineout_enabled)return left&&right;
    bool dock_left=write(0x1c,gain),dock_right=write(0x1d,gain);
    return left&&right&&dock_left&&dock_right;
}
static bool minimum_output(ln_codec_writer write,bool mute){
    uint8_t hp=mute?0xc0:0x40,line=mute||!lineout_enabled?0xc0:0x40;
    bool left=write(0x1a,hp),right=write(0x1b,hp);
    bool dock_left=write(0x1c,line),dock_right=write(0x1d,line);
    return left&&right&&dock_left&&dock_right;
}
bool ln_cs42l55_unmute_at_minimum(ln_codec_writer write){return minimum_output(write,false);}
static uint8_t rate_value(uint32_t rate) {
    switch(rate) {
    case 8000:return 0x1d;case 11025:return 0x1b;case 12000:return 0x19;
    case 16000:return 0x15;case 22050:return 0x13;case 24000:return 0x11;
    case 32000:return 0x0d;case 44100:return 0x0b;case 48000:return 9;
    default:return 0;
    }
}
bool ln_codec_rate_supported(uint32_t rate){return rate_value(rate)!=0;}
bool ln_codec_rate(ln_codec_writer write,uint32_t rate) {
    uint8_t value=rate_value(rate);return value && write(5,value);
}
bool ln_codec_volume(ln_codec_writer write,int32_t value) {
    if(value<-1020 || value>0)return false;
    /* Exact unsigned division by five without an ARM runtime helper. */
    uint32_t steps=(uint32_t)(((uint64_t)(4-value)*0xcccccccdu)>>34);
    /* Apply whole dB steps at the output amplifiers, where they also
     * attenuate the DAC noise floor. Master volume carries the remainder. */
    uint32_t analog=steps/2u;
    if(analog>60u)analog=60u;
    uint8_t gain=(uint8_t)(0u-analog)&0x7fu;
    uint8_t digital=(uint8_t)(2u*analog-steps);
    if(!write(0x18,digital)||!write(0x19,digital))return false;
    if(!output_muted&&output_gain!=gain&&!set_output_gain(write,gain))return false;
    output_gain=gain;
    return true;
}
bool ln_codec_lineout(ln_codec_writer write,bool enabled) {
    if(!lineout(write,enabled,output_muted)||!write(3,enabled?0xaa:0xaf))return false;
    lineout_enabled=enabled;return true;
}
bool ln_codec_analog_mute(ln_codec_writer write,bool mute) {
    if(mute){
        output_muted=true;
        /* Bit 7 mutes. Preserve the selected gain across a warm mute. */
        uint8_t value=0x80u|output_gain;
        bool left=write(0x1a,value),right=write(0x1b,value);
        bool dock=lineout(write,lineout_enabled,true);return left&&right&&dock;
    }
    if(!set_output_gain(write,output_gain))return false;
    output_muted=false;return true;
}
bool ln_codec_mute(ln_codec_writer write,bool mute) {
    if(mute){bool digital=ln_codec_digital_mute(write,true),analog=ln_codec_analog_mute(write,true);
        return digital&&analog;}
    return ln_codec_analog_mute(write,false)&&ln_codec_digital_mute(write,false);
}
bool ln_codec_digital_mute(ln_codec_writer write,bool mute) {
    return write(0x0f,mute?3:0);
}
bool ln_cs42l55_shutdown(ln_codec_writer write,uint32_t settling_us) {
    /* DS773F1 4.11.1: mute AND maximally attenuate all analog outputs before
     * PDN. Do not reuse output_muted: a prior partial I2C write can set it.
     * ANLGZC can defer these controls for 1024 LRCK periods (6.7.2). */
    bool digital=ln_codec_digital_mute(write,true);
    bool analog=minimum_output(write,true);
    if(!digital||!analog)return false;
    output_muted=true;
    /* The core delay API accepts at most100ms per call. */
    while(settling_us){
        uint32_t delay=settling_us>100000u?100000u:settling_us;
        if(!ln_core_delay_us(delay))return false;
        settling_us-=delay;
    }
    /* No analog gain/mute write after the settling wait. */
    if(!write(3,0xaf)||!write(2,0x0f))return false;
    lineout_enabled=false;return true;
}
bool ln_codec_off(ln_codec_writer write){return ln_cs42l55_shutdown(write,193000u);}
bool ln_codec_init(ln_codec_writer write,uint32_t rate) {
    output_muted=true;lineout_enabled=false;
    if(!ln_codec_rate_supported(rate) || !write(2,0x0f))return false;
    static const uint8_t compensation[][2]={
        {0,0x99},{0x2e,0x30},{0x32,7},{0x33,0xff},{0x34,0xf8},{0x35,0xdc},
        {0x36,0xfc},{0x37,0xac},{0x3a,0xf8},{0x3c,0xd3},{0x3d,0x23},
        {0x3e,0x81},{0x3f,0x46},{0,0}};
    for(unsigned i=0;i<sizeof(compensation)/sizeof(compensation[0]);i++)
        if(!write(compensation[i][0],compensation[i][1]))return false;
    /* DS773F1 4.11 requires both analog mute and maximum attenuation
     * before releasing PDN for playback. */
    if(!ln_codec_digital_mute(write,true) || !minimum_output(write,true) || !write(3,0xaf) ||
       !write(4,0x2e) || !ln_codec_rate(write,rate) || !write(7,0x1c) ||
       !write(8,0) || !write(0x0b,0) || !write(0x0c,0) ||
       !write(6,0x30) || !ln_codec_volume(write,-180) || !write(2,6))return false;
    /* Leave both channels muted; the audio owner unmutes after priming. */
    return ln_core_delay_us(75000);
}
