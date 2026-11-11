#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Hardware-key CBC, zero IV. Buffer uses a physical address, aligned to 32
 * bytes with capacity rounded to 32 bytes; size is a multiple of 16 and no
 * larger than the original N25 NOR payload. Key 1 is the model key, key 2 is
 * device-specific. Keys are never exported. */
bool ln_s5l_aes_crypt(uint8_t *data,uint32_t size,uint32_t key,bool encrypt);
