#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *ln_mpeg4_alloc(size_t);
void ln_mpeg4_free(void *);
void *ln_mpeg4_copy(void *,const void *,size_t);
void *ln_mpeg4_set(void *,int,size_t);
int ln_mpeg4_compare(const void *,const void *,size_t);
#ifdef __cplusplus
}
#endif
#define oscl_malloc ln_mpeg4_alloc
#define oscl_free ln_mpeg4_free
#define oscl_memcpy ln_mpeg4_copy
#define oscl_memset ln_mpeg4_set
#define oscl_memcmp ln_mpeg4_compare
