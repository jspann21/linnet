#ifndef LINNET_GAME_PACKAGE_H
#define LINNET_GAME_PACKAGE_H
#include "sdk/include/linnet/game.h"
/* Header is little endian; CRC32 covers bytes0..251. No structure casting.
 * Initialized image/BSS are32-byte aligned; text/entry are4-byte aligned ARM.
 * BSS immediately follows initialized bytes, and is never stored in the file. */
struct ln_game_header {
    uint32_t target,load,payload,bss,entry,capabilities,builtin,crc,stack,text,schema,version;
    uint8_t sha256[32];
    char id[LN_GAME_ID_BYTES],title[LN_GAME_TITLE_BYTES],description[LN_GAME_DESCRIPTION_BYTES];
};
bool ln_game_identity(const char *id);
bool ln_game_builtin_identity(const char *id,uint32_t builtin);
bool ln_game_header_decode(const uint8_t data[256],uint32_t target,uint32_t load,
                           struct ln_game_header *out);
/* Catalogue:32-byte header, followed by count*256-byte records, terminal CRC.
 * Each record stores id64,title40,description80,filename13,reserved3,
 * package_size4,package_crc4,builtin4,capabilities4,save_slot4,reserved36. */
#define LN_GAME_CATALOGUE_HEADER 32u
#define LN_GAME_CATALOGUE_ROW 256u
#define LN_GAME_CATALOGUE_BYTES (32u+LN_GAME_CATALOGUE_MAX*256u+4u)
bool ln_game_catalogue_header(const uint8_t *data,uint32_t bytes,uint32_t target,
                             uint32_t *generation,uint32_t *count);
bool ln_game_catalogue_row(const uint8_t *data,uint32_t index);
#endif
