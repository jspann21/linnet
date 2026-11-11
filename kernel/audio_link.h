#ifndef LINNET_AUDIO_LINK_H
#define LINNET_AUDIO_LINK_H
#include "hal/link/link.h"
int ln_audio_link_dispatch(const ln_link_frame *,ln_link_frame *,uint8_t *reply);
#endif
