#ifndef LINNET_MUSIC_SETTINGS_H
#define LINNET_MUSIC_SETTINGS_H
#include "services/music/queue.h"
#include "services/music/profile.h"
uint32_t ln_music_settings_encode(const struct ln_queue *,const struct ln_music_profile *,uint8_t *,uint32_t);
bool ln_music_settings_decode(struct ln_queue *,struct ln_music_profile *,const uint8_t *,uint32_t);
#endif
