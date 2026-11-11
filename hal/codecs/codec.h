#ifndef LINNET_CODEC_H
#define LINNET_CODEC_H
#include <stdbool.h>
#include <stdint.h>
typedef bool (*ln_codec_writer)(uint8_t reg,uint16_t value);
bool ln_codec_rate_supported(uint32_t rate);
bool ln_codec_rate(ln_codec_writer write,uint32_t rate);
bool ln_codec_init(ln_codec_writer write,uint32_t rate);
bool ln_codec_volume(ln_codec_writer write,int32_t db_tenths);
bool ln_codec_lineout(ln_codec_writer write,bool enabled);
/* Full mute also closes analog outputs. Volume/routing updates retain it;
 * only an explicit unmute opens them. Keep clocks running while it settles. */
bool ln_codec_mute(ln_codec_writer write,bool mute);
/* Soft DAC/master mute, leaving the analog output state unchanged. */
bool ln_codec_digital_mute(ln_codec_writer write,bool mute);
/* Analog output state only. Unmute under a settled digital mute and valid
 * zero PCM, then allow the output controls to settle before releasing it. */
bool ln_codec_analog_mute(ln_codec_writer write,bool mute);
bool ln_codec_off(ln_codec_writer write);
#endif
