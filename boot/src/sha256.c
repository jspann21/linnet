#include "sha256.h"
#if defined(LINNET_PLATFORM_CORE)
#include "linnet_target.h"
#endif
static const uint32_t constants[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static uint32_t rotate(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32u - amount));
}
#define LN_SHA_COMPRESS_ATTR
#define LN_SHA_COMPRESS_NAME compress_sdram
#define LN_SHA_CONSTANTS constants
#define LN_SHA_WORKSPACE uint32_t w[64]
#include "sha256_compress.inc"
#undef LN_SHA_COMPRESS_ATTR
#undef LN_SHA_COMPRESS_NAME
#undef LN_SHA_CONSTANTS
#undef LN_SHA_WORKSPACE
#if defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE) && LINNET_BOOT_HASH_IRAM
static const uint32_t boot_constants[64] __attribute__((section(".boot_hash.rodata"),aligned(4))) = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
/* Boot compresses synchronously on the CPU, with no callbacks or SHA users in
 * interrupt/COP handlers. Every block overwrites all 64 words before use.
 * Keep this scratch beside the verified code; OS hashing retains its stack. */
static uint32_t boot_schedule[64] __attribute__((section(".boot_hash.data"),aligned(4)));
#define LN_SHA_COMPRESS_ATTR __attribute__((section(".boot_hash.text"),noinline))
#define LN_SHA_COMPRESS_NAME compress_iram
#define LN_SHA_CONSTANTS boot_constants
#define LN_SHA_WORKSPACE uint32_t *w=boot_schedule
#include "sha256_compress.inc"
#undef LN_SHA_COMPRESS_ATTR
#undef LN_SHA_COMPRESS_NAME
#undef LN_SHA_CONSTANTS
#undef LN_SHA_WORKSPACE
static uint32_t boot_iram_ready;
int ln_sha256_boot_iram_init(void){
    extern const uint32_t __boot_hash_load[],__boot_hash_begin[],__boot_hash_end[];
    boot_iram_ready=0;
    uintptr_t start=(uintptr_t)__boot_hash_begin,end=(uintptr_t)__boot_hash_end;
    if(end<=start||end-start>0x1000u||((start|end|(uintptr_t)__boot_hash_load)&3u))return 0;
    volatile uint32_t *destination=(volatile uint32_t *)start;
    uint32_t words=(uint32_t)((end-start)/4u);
    for(uint32_t i=0;i<words;++i)destination[i]=__boot_hash_load[i];
    for(uint32_t i=0;i<words;++i)if(destination[i]!=__boot_hash_load[i])return 0;
    __asm__ volatile("" ::: "memory");
    boot_iram_ready=1;return 1;
}
int ln_sha256_boot_iram_active(void){return boot_iram_ready!=0;}
#elif defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE)
int ln_sha256_boot_iram_init(void){return 0;}
int ln_sha256_boot_iram_active(void){return 0;}
#endif
static void compress(struct ln_sha256*s,const uint8_t*block){
#if defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE) && LINNET_BOOT_HASH_IRAM
    if(boot_iram_ready){compress_iram(s,block);return;}
#endif
    compress_sdram(s,block);
}
void ln_sha256_init(struct ln_sha256 *s) {
    static const uint32_t initial[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    for(unsigned i=0;i<8;++i)s->words[i]=initial[i];
    s->bytes=0;s->used=0;
}
void ln_sha256_update(struct ln_sha256 *s,const uint8_t *bytes,size_t size) {
    s->bytes+=size;
    while(size) {
        /* Full blocks need no staging copy. Byte loads in compress preserve
         * support for callers whose input is not word-aligned. */
        if(!s->used&&size>=64u){compress(s,bytes);bytes+=64u;size-=64u;continue;}
        size_t n=64u-s->used;if(n>size)n=size;
        for(size_t i=0;i<n;++i)s->block[s->used+i]=bytes[i];
        s->used+=(uint32_t)n;bytes+=n;size-=n;
        if(s->used==64u){compress(s,s->block);s->used=0;}
    }
}
void ln_sha256_finish(struct ln_sha256 *s,uint8_t digest[32]) {
    uint64_t bits=s->bytes*8u;
    s->block[s->used++]=0x80;
    if(s->used>56){while(s->used<64)s->block[s->used++]=0;compress(s,s->block);s->used=0;}
    while(s->used<56)s->block[s->used++]=0;
    for(unsigned i=0;i<8;++i){s->block[63-i]=(uint8_t)bits;bits>>=8;}
    compress(s,s->block);
    for(unsigned i=0;i<32;++i)digest[i]=(uint8_t)(s->words[i/4]>>(24u-8u*(i%4)));
}
