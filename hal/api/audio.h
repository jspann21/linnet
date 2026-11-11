#ifndef LINNET_HAL_AUDIO_H
#define LINNET_HAL_AUDIO_H
#include <stdint.h>
#include <stdbool.h>
/* Covers bounded next-file chain verification while current audio continues.
 * 256KiB at stereo16-bit gives1.49s at44.1kHz; startup primes only8192frames. */
#define LINNET_AUDIO_RING_FRAMES 65536u
struct ln_audio_status {
    uint32_t rate, queued, free_frames, consumed, submitted, interrupts;
    uint32_t underruns, silence_frames, fault, playing, paused, initialized;
    uint32_t last_irq_us, max_irq_us;
    int32_t volume_db_tenths;
    /* FIFO empty at completion entry is a sample, not an underflow latch. */
    uint32_t fifo_empty_irqs, max_completion_interval_us, max_completion_late_us;
    /* Frames transferred in the current block, excluded from consumed.
     * Backends without a hardware cursor leave this zero. */
    uint32_t inflight_consumed;
};
bool ln_audio_init(uint32_t rate);
uint32_t ln_audio_submit(const int16_t *pcm, uint32_t frames);
uint32_t ln_audio_capacity(void);
/* Absolute submitted frame index; rejects overlap with the active DMA block. */
bool ln_audio_rewrite(uint32_t frame,const int16_t *pcm,uint32_t frames);
bool ln_audio_start(void);
void ln_audio_finish(void);
void ln_audio_pause(void);
bool ln_audio_stop(void);
/* Discard a stream after verified DMA idle, retaining codec power and volume.
 * A successful halt requires init before submitting the replacement stream. */
bool ln_audio_halt(void);
bool ln_audio_volume(int32_t db_tenths);
bool ln_audio_lineout(bool enabled);
/* Short UI feedback, mixed into the next safe DMA block or played alone.
 * The level is 0..4; output gain follows music volume and its configured limit.
 * Standalone clicks never consume queued music or advance its frame counters. */
void ln_audio_click(uint8_t tone,uint8_t volume,int32_t output_db_tenths);
void ln_audio_click_stop(void);
/* UINT32_MAX when no feedback drain or idle power-down needs a wake. */
uint32_t ln_audio_deadline(uint32_t now);
void ln_audio_irq(void);
void ln_audio_service(uint32_t now);
void ln_audio_snapshot(struct ln_audio_status *out);
#endif
