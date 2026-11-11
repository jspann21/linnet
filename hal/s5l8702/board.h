#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "linnet_target.h"
/* Board wiring and presentation supplied by targets/<device>/. */
bool ln_s5l_board_disk_screen(bool symbol_visible);
bool ln_s5l_board_recovery_screen(bool connected);
bool ln_s5l_board_input_power(void);
bool ln_s5l_board_disk_power(void);
bool ln_s5l_board_disk_power_off(void);
bool ln_s5l_board_switches(bool *hold,bool *headphones);
bool ln_s5l_board_audio_power(bool reset);
bool ln_s5l_board_codec_write(uint8_t reg,uint16_t value);
bool ln_s5l_board_audio_clock(uint32_t rate,uint32_t *codec_rate);
bool ln_s5l_board_audio_clock_off(void);
bool ln_s5l_board_cpu_voltage(bool boosted);
