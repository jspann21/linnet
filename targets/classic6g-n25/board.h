#pragma once
#include <stdbool.h>
#include <stdint.h>
struct ln_n25_power_identity { uint8_t version,variant,status; };
bool ln_n25_power_identify(struct ln_n25_power_identity *identity);
bool ln_n25_lcd_power(void);
/* Cold boot only, executing from SRAM before the OS owns DRAM. */
bool ln_n25_memory_init(uint32_t *failure_address);
