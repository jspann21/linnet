#pragma once
#include <stdbool.h>
#include <stdint.h>
bool ln_s5l_lcd_mode(uint32_t mode);
bool ln_s5l_lcd_command(uint16_t command);
bool ln_s5l_lcd_data(uint16_t data);
bool ln_s5l_lcd_read(uint8_t *data);
bool ln_s5l_lcd_dma_begin(const uint16_t *pixels,uint32_t count);
/* 0 pending, 1 completed and LCD FIFO drained, -1 transfer failure. */
int ln_s5l_lcd_dma_poll(void);
