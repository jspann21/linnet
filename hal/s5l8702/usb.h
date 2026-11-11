#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Live DWC trace followed by the trace saved before the last controller reset. */
void ln_s5l_usb_trace(uint32_t output[64]);
bool ln_s5l_usb_storage_init(const char *serial);
size_t ln_s5l_usb_storage_read(uint8_t *data,size_t size);
bool ln_s5l_usb_storage_write(const uint8_t *data,size_t size);
void ln_s5l_usb_stall(bool input);
bool ln_s5l_usb_storage_reset_pending(void);
bool ln_s5l_usb_storage_reset_complete(void);
