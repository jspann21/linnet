#ifndef LINNET_WM8758_H
#define LINNET_WM8758_H
#include "hal/codecs/codec.h"

bool ln_wm8758_unmute_headphones_at_minimum(ln_codec_writer write);
bool ln_wm8758_unmute_clicks(ln_codec_writer write);

#endif
