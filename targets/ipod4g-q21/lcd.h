#ifndef LINNET_IPOD4G_Q21_LCD_H
#define LINNET_IPOD4G_Q21_LCD_H
#include <stdbool.h>
#include <stdint.h>

bool ln_q21_lcd_init(void);
bool ln_q21_lcd_reinitialize(void);
bool ln_q21_lcd_diagnostic(void);
bool ln_q21_lcd_begin_row(uint32_t y);
bool ln_q21_lcd_begin_span(uint32_t y, uint32_t block);
bool ln_q21_lcd_flush(void);
bool ln_q21_lcd_write(uint8_t first, uint8_t second);

#endif
