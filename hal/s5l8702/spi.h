#ifndef LINNET_S5L_SPI_H
#define LINNET_S5L_SPI_H
#include <stdbool.h>
#include <stdint.h>
bool ln_s5l_spi0_begin(void);
bool ln_s5l_spi0_byte(uint8_t output,uint8_t *input);
void ln_s5l_spi0_end(void);
#endif
