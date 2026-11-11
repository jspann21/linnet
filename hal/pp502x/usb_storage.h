#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool ln_pp_usb_storage_init(const char *serial);
size_t ln_pp_usb_storage_read(uint8_t *data, size_t size);
size_t ln_pp_usb_storage_write(const uint8_t *data, size_t size);
size_t ln_pp_usb_storage_packet_size(void);
bool ln_pp_usb_storage_zlp(void);
void ln_pp_usb_storage_stall(bool input);
bool ln_pp_usb_storage_reset_pending(void);
bool ln_pp_usb_storage_reset_complete(void);
