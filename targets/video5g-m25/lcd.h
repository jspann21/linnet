#ifndef LINNET_M25_LCD_H
#define LINNET_M25_LCD_H
#include <stdbool.h>
#include <stdint.h>
bool ln_m25_lcd_init(void);
bool ln_m25_lcd_ready(bool *ready);
bool ln_m25_lcd_prepare(void);
bool ln_m25_lcd_write(uint32_t pixel,const uint16_t *data,uint32_t count);
bool ln_m25_lcd_commit(void);
bool ln_m25_lcd_runtime_header(uint32_t words[4]);
bool ln_m25_bcm_read(uint32_t address,uint32_t *words,uint32_t count);
bool ln_m25_bcm_write(uint32_t address,const uint32_t *words,uint32_t count);
#endif
