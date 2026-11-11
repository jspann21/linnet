#ifndef LINNET_ARM926_CACHE_H
#define LINNET_ARM926_CACHE_H
#include <stdbool.h>
#include <stdint.h>
uint32_t ln_arm926_cache_control(void);
bool ln_arm926_instruction_cache(bool enabled);
void ln_arm926_data_clean(void);
void ln_arm926_data_clean_range(const void *address,uint32_t size);
/* Invalidate only caller-owned complete 32-byte lines after DMA writes. */
bool ln_arm926_data_invalidate_range(void *address,uint32_t size);
#endif
