#include "mpeg4_memory.h"
#include "linnet_target.h"
#include <stdint.h>
#define ARENA ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x200000u))
#define ARENA_BYTES (LINNET_STAGE_CAPACITY-0x200000u)
void ln_memory_copy(void *,const void *,uint32_t);
void ln_memory_clear(void *,uint32_t);
static uint32_t allocated;
void ln_mpeg4_memory_reset(void){allocated=0;}
void *ln_mpeg4_alloc_aligned(size_t bytes,unsigned alignment){
    if(!alignment||alignment>1024u||(alignment&(alignment-1u)))return 0;
    uint32_t start=(allocated+alignment-1u)&~(alignment-1u);
    if(!bytes||start>ARENA_BYTES||bytes>ARENA_BYTES-start)return 0;
    allocated=start+(uint32_t)bytes;return ARENA+start;
}
void *ln_mpeg4_alloc(size_t bytes){return ln_mpeg4_alloc_aligned(bytes,32);}
void ln_mpeg4_free(void *p){(void)p;}
void *ln_mpeg4_copy(void *to,const void *from,size_t bytes){ln_memory_copy(to,from,(uint32_t)bytes);return to;}
void *ln_mpeg4_set(void *to,int value,size_t bytes){
    if(!value)ln_memory_clear(to,(uint32_t)bytes);
    else {uint8_t *p=(uint8_t *)to;while(bytes--)*p++=(uint8_t)value;}
    return to;
}
int ln_mpeg4_compare(const void *a,const void *b,size_t bytes){
    const uint8_t *x=(const uint8_t *)a,*y=(const uint8_t *)b;
    while(bytes--){if(*x!=*y)return (int)*x-(int)*y;++x;++y;}return 0;
}
