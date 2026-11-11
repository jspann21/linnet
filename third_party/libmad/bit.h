/* Linnet reader adapter for libmad's private cursor API. */
#ifndef LINNET_MAD_BITS_ADAPTER_H
#define LINNET_MAD_BITS_ADAPTER_H
#include "services/audio/mp3/bits.h"
#define mad_bitptr ln_mp3_bits
#define mad_bit_init ln_mp3_bits_init
#define mad_bit_length ln_mp3_bits_length
#define mad_bit_nextbyte ln_mp3_bits_nextbyte
#define mad_bit_skip ln_mp3_bits_skip
#define mad_bit_read ln_mp3_bits_read
#define mad_bit_crc ln_mp3_bits_crc
#define mad_bit_finish(b) ((void)(b))
#endif
