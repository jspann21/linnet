#pragma once
/* h264bsd 42bcb5d753ad86d84903354bf3c68423c28adb7b, oneam/h264bsd.
 * Linnet supplies a bounded video arena and freestanding memory operations. */
#include <stddef.h>
#include <stdint.h>
#include "linnet_target.h"
#define LINNET_H264 1
void *ln_h264_alloc(size_t bytes);
void ln_h264_free(void *pointer);
void *ln_h264_copy(void *to,const void *from,size_t bytes);
void *ln_h264_set(void *to,int value,size_t bytes);
#define malloc ln_h264_alloc
#define free ln_h264_free
#define memcpy ln_h264_copy
#define memset ln_h264_set
