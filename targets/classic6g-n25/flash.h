#ifndef LINNET_N25_FLASH_H
#define LINNET_N25_FLASH_H
#include <stdbool.h>
#include <stdint.h>
bool ln_n25_flash_identity(uint8_t id[3]);
bool ln_n25_flash_read(uint32_t offset,uint8_t *bytes,uint32_t size);
/* Only the existing 128 KiB bootloader slot; caller verifies old/new hashes.
 * Returns zero after readback and restoration of the protection register. */
uint32_t ln_n25_flash_replace_sector(uint32_t offset,const uint8_t bytes[4096]);
#endif
