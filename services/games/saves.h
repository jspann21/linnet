#ifndef LINNET_GAME_SAVES_H
#define LINNET_GAME_SAVES_H
#include "sdk/include/linnet/game.h"
#define LN_GAME_SAVE_FILE_BYTES (LN_GAME_CATALOGUE_MAX*4096u)
void ln_game_save_begin(const char *id,uint32_t slot,uint32_t schema,uint32_t builtin_best);
bool ln_game_save_ready(void);
void ln_game_save_step(bool may_write);
bool ln_game_save_busy(void);
bool ln_game_save_inflight(void);
bool ln_game_save_failed(void);
bool ln_game_save_work(bool may_write);
void ln_game_save_end(void);
uint32_t ln_game_save_best(void);
bool ln_game_save_record(uint32_t score);
int32_t ln_game_save_read(uint32_t schema,void *data,uint32_t capacity);
bool ln_game_save_write(uint32_t schema,const void *data,uint32_t bytes);
int32_t ln_game_save_status(void);
#endif
