#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void ln_mpeg4_memory_reset(void);
void *ln_mpeg4_alloc(size_t bytes);
void *ln_mpeg4_alloc_aligned(size_t bytes,unsigned alignment);
#ifdef __cplusplus
}
#endif
