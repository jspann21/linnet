#include "cache.h"
uint32_t ln_arm926_cache_control(void) {
    uint32_t value;__asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(value));return value;
}
bool ln_arm926_instruction_cache(bool enabled) {
    uint32_t zero=0,value=ln_arm926_cache_control();
    value=enabled?value|0x1000u:value&~0x1000u;
    __asm__ volatile("mcr p15,0,%0,c7,c10,4\n"
                     "mcr p15,0,%0,c7,c5,0\n"
                     "mcr p15,0,%1,c1,c0,0\n"
                     /* ARM926 flushes speculative fetches on a branch. */
                     "b 1f\n1:\n"
                     : : "r"(zero),"r"(value):"memory");
    return (ln_arm926_cache_control()&0x1000u)==(enabled?0x1000u:0u);
}
void ln_arm926_data_clean(void) {
    uint32_t zero=0;
    __asm__ volatile("1: mrc p15,0,apsr_nzcv,c7,c10,3\nbne 1b\nmcr p15,0,%0,c7,c10,4"
                     ::"r"(zero):"cc","memory");
}
void ln_arm926_data_clean_range(const void *address,uint32_t size) {
    uintptr_t start=(uintptr_t)address;
    if(!size || start>UINT32_MAX-size)return;
    uintptr_t last=(start+size-1u)&~31u;start&=~31u;
    if(ln_arm926_cache_control()&4u)for(;;) {
        __asm__ volatile("mcr p15,0,%0,c7,c10,1"::"r"(start):"memory");
        if(start==last)break;start+=32u;
    }
    uint32_t zero=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(zero):"memory");
}
bool ln_arm926_data_invalidate_range(void *address,uint32_t size) {
    uintptr_t start=(uintptr_t)address;
    if(!size || ((start|size)&31u) || start>UINT32_MAX-size)return false;
    uintptr_t end=start+size;
    for(;start<end;start+=32u)__asm__ volatile("mcr p15,0,%0,c7,c6,1"::"r"(start):"memory");
    return true;
}
