#ifndef LINNET_HAL_PLATFORM_H
#define LINNET_HAL_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#include "linnet_target.h"
uint32_t ln_time_us(void);
/* Return a CPU address that bypasses cache for target SDRAM, when required. */
uintptr_t ln_platform_uncached_address(uintptr_t address);
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
/* Qualified optional RAM, separate from image/staging/recovery reservations.
 * First 64 KiB: disk cache tags; remainder of first 16 MiB: music metadata;
 * second 16 MiB: clean disk cache. Native alias avoids changing CPU/COP maps. */
void *ln_platform_extended_memory(uint32_t *bytes);
#endif
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
/* Executive-owned PWM pulse; volume 0 mutes, 1..4 adjust duty cycle.
 * No waits or audio DMA ownership. */
void ln_platform_click(uint8_t tone,uint8_t volume);
void ln_platform_click_stop(void);
void ln_platform_click_poll(uint32_t now);
bool ln_platform_click_deadline(uint32_t *deadline);
void ln_platform_click_diagnostics(uint32_t out[4]);
#endif
_Noreturn void ln_platform_hardware_reset(void);
bool ln_platform_set_disk_mode(bool enabled);
/* Called after storage, audio, display, and DMA have quiesced. */
bool ln_platform_enter_disk_mode(uint32_t sector_bytes);
/* Authenticate the archived bytes before applying any family-specific decode. */
bool ln_platform_decode_retail_payload(uint8_t *payload,uint32_t size);
void ln_platform_boot_candidate(bool candidate);
int ln_platform_enter(const uint8_t *payload,uint32_t size,uint32_t load,uint32_t entry);
int ln_platform_enter_retail(const uint8_t *payload,uint32_t size);
uint32_t ln_platform_clock_divider(void);
void ln_platform_cache_marker(uint32_t value);
bool ln_lcd_fill(uint16_t color);
bool ln_lcd_boot(void);
#if defined(LINNET_IMAGE_BOOT)
bool ln_lcd_boot_apple(void);
bool ln_lcd_boot_menu(uint32_t available,uint32_t selected,bool starting);
#endif
/* Bounded, standalone field fault display, including release Boot. */
bool ln_lcd_fault(uint32_t reason,const uint8_t build_id[32]);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_lcd_diagnostics(uint32_t output[8]);
#endif
void ln_platform_quiesce(void);
bool ln_platform_dma_idle(void);
bool ln_platform_audio_idle(void);
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_BOOT)
/* Early Boot only, before transport owns ARC/IRAM. Nonzero leaves clock policy
 * unchanged. Known native USB state may use canonical enable/reset pulses;
 * never gates module clocks off or touches USB descriptors. */
/* 0 ready; BTT4 skip reason 7..20 otherwise. */
uint32_t ln_platform_boot_clock_ready(void);
#endif
void ln_watchdog_feed(void);
bool ln_watchdog_expired(void);
_Noreturn void ln_panic(uint32_t reason, uint32_t pc, uint32_t status);
_Noreturn void ln_main(void);
#endif
