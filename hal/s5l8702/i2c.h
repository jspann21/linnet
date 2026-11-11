#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Seven-bit slave address; register transactions, maximum 32 data bytes.
 * Caller serializes access. Timer E must already be running. */
bool ln_s5l_i2c_read(unsigned bus, uint8_t slave, uint8_t reg, uint8_t *data, size_t length);
bool ln_s5l_i2c_write(unsigned bus, uint8_t slave, uint8_t reg, const uint8_t *data, size_t length);
