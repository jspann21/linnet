#pragma once
#include <stdint.h>

void ln_mp3_overlap(const int32_t *output,int32_t history[18],int32_t samples[18][32],unsigned band);
