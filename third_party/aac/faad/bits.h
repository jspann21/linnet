/* Linnet reader adapter for the retained AAC-LC decoder. */
#ifndef LINNET_FAAD_BITS_ADAPTER_H
#define LINNET_FAAD_BITS_ADAPTER_H
#include "analysis.h"
#include "services/audio/aac/bits.h"
typedef struct ln_aac_bits bitfile;
#define BYTE_NUMBIT 8
#define bit2byte(a) (((a)+7)/8)
#define faad_initbits ln_aac_bits_init
#define faad_endbits(b) ((void)(b))
#define faad_byte_align ln_aac_bits_align
#define faad_get_processed_bits ln_aac_bits_position
#define faad_showbits ln_aac_bits_look
#define faad_flushbits ln_aac_bits_skip
#define faad_getbits(b,n,...) ln_aac_bits_read((b),(n))
#define faad_get1bit(b,...) ln_aac_bits_one(b)
#endif
