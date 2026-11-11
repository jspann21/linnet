#pragma once
#include <stdbool.h>
#include <stdint.h>
void ln_s5l_remote_init(void);
void ln_s5l_remote_poll(uint32_t now,bool headphones,uint8_t *actions,int8_t *volume);
void ln_s5l_remote_diagnostics(uint32_t words[6]);
