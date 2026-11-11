#ifndef LINNET_MUSIC_ARTWORK_H
#define LINNET_MUSIC_ARTWORK_H
#include <stdbool.h>
#include <stdint.h>
void ln_music_art_request(const char *path,uint32_t crc);
void ln_music_art_prefetch(const char *path,uint32_t crc);
/* Called only from the outer executive. An admitted operation always drains,
 * even when allow_read is false because the PCM ring needs priority. */
void ln_music_art_step(bool allow_read);
uint32_t ln_music_art_deadline(uint32_t now);
const uint16_t *ln_music_art_pixels(void);
const uint16_t *ln_music_art_pixels_sized(uint32_t side,uint32_t *actual_side);
uint32_t ln_music_art_side(void);
uint32_t ln_music_art_revision(void);
bool ln_music_art_loading(void);
#endif
