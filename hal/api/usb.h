#ifndef LINNET_HAL_USB_H
#define LINNET_HAL_USB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Development CDC ACM. The selected platform owns the DMA reservation.
 * Caller must enter with the platform data cache disabled.
 * Stable serial is 1..32 ASCII characters derived from physical identity. */
bool ln_usb_init(const char *stable_serial);
/* Bounded nonblocking service; false means controller/transfer failure. */
bool ln_usb_poll(void);
bool ln_usb_connected(void);
bool ln_usb_rx_pending(void);
uint32_t ln_usb_generation(void);
/* The executive rearms controller wake interrupts after draining USB work. */
void ln_usb_irq_enable(void);
/* Eight words: bulk-IN QH token, dTD token, active-pipe bits, PRIME, STATUS,
 * DONE, failure/snapshot origin, generation. A DTR falling edge preserves a
 * pending-transfer snapshot so the next session can inspect it. */
void ln_usb_diagnostics(uint32_t output[8]);
/* Partial transfers permitted; zero means backpressure or disconnected.
 * read/write consume at most 512 bytes. Poll between calls. */
size_t ln_usb_read(uint8_t *output, size_t capacity);
/* Optional receive sizing hint for the current validated frame remainder.
 * Zero restores one-packet reception. Never changes an active DMA transfer. */
void ln_usb_rx_hint(uint32_t remaining);
size_t ln_usb_write(const uint8_t *input, size_t length);
/* Deadline is absolute ln_time_us(), at most 2 seconds in the future.
 * True proves DMA completion of accepted TX bytes, including any final ZLP. */
bool ln_usb_flush(uint32_t deadline_us);
/* Prove endpoint DMA idle and controller reset complete before reusing memory.
 * Platform USB clocks remain enabled for the boot handoff. False is fatal. */
bool ln_usb_stop(void);
/* Last stop proof: bit0 RUN clear, bit1 flush complete, bit2 reset complete,
 * bit3 endpoint idle, bit4 reserved power-gated observation (not a DMA proof),
 * bit5 stop settle complete. Successful clocked handoff is 0x2f. */
uint32_t ln_usb_stop_diagnostics(void);
void ln_usb_disconnect(void);
#endif
