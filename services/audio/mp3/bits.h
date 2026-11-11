#ifndef LINNET_MP3_BITS_H
#define LINNET_MP3_BITS_H
#include <stdint.h>
/* MSB-first cursor over decoder-owned input with its existing guard bytes.
 * Copies share an immutable base; readbit remains an absolute bit position
 * so the Layer III Huffman reader can compare speculative cursors. */
struct ln_mp3_bits {const uint8_t *base;uint32_t readbit;};
void ln_mp3_bits_init(struct ln_mp3_bits *,const uint8_t *);
uint32_t ln_mp3_bits_length(const struct ln_mp3_bits *,const struct ln_mp3_bits *);
const uint8_t *ln_mp3_bits_nextbyte(const struct ln_mp3_bits *);
void ln_mp3_bits_skip(struct ln_mp3_bits *,uint32_t);
uint32_t ln_mp3_bits_read(struct ln_mp3_bits *,uint32_t);
uint16_t ln_mp3_bits_crc(struct ln_mp3_bits,uint32_t,uint16_t);
#endif
