#ifndef LINNET_S5L_PCM_DMA_H
#define LINNET_S5L_PCM_DMA_H
#include <stdint.h>
#include <stdbool.h>
/* Caller retains uncached stereo PCM until completion or verified stop. */
bool ln_s5l_pcm_begin(const int16_t *samples,uint32_t frames);
/* Finite linked transfer; no CPU intervention between blocks. Maximum 130560
 * frames. Descriptor and sample memory must remain uncached until stopped. */
bool ln_s5l_pcm_begin_buffer(const int16_t *samples,uint32_t frames);
/* Completion IRQ only: replace a finished transfer without stopping I2S. */
bool ln_s5l_pcm_rearm(const int16_t *samples,uint32_t frames);
bool ln_s5l_pcm_stop(void);
void ln_s5l_pcm_interrupt(bool enabled);
void ln_s5l_pcm_status(uint32_t result[5]);
#endif
