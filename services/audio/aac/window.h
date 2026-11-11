#ifndef LINNET_AAC_WINDOW_H
#define LINNET_AAC_WINDOW_H
#include <stdint.h>
void ln_aac_window_add(int32_t *,const int32_t *,const int32_t *,const int32_t *,unsigned);
void ln_aac_window_reverse(int32_t *,const int32_t *,const int32_t *,unsigned);
#endif
