#include "mmu.h"
#include "cache.h"
bool ln_arm926_mmu_enable(uint32_t table[4096],uint32_t ram,uint32_t bytes) {
    uintptr_t address=(uintptr_t)table;
    if(!address || (address&0x3fffu) || !bytes || ((ram|bytes)&0xfffffu) ||
       ram>=0x10000000u || bytes>0x10000000u-ram || (ln_arm926_cache_control()&5u))return false;
    for(unsigned i=0;i<4096;i++)table[i]=((uint32_t)i<<20)|0x412u;
    for(uint32_t offset=0;offset<bytes;offset+=0x100000u) {
        table[(ram+offset)>>20]=(ram+offset)|0x41eu;
        table[(ram+offset+0x10000000u)>>20]=(ram+offset)|0x412u;
    }
    uint32_t zero=0,domain=1,control=ln_arm926_cache_control()|5u;
    __asm__ volatile("mcr p15,0,%0,c7,c10,4\n"
                     "mcr p15,0,%0,c7,c6,0\nmcr p15,0,%0,c7,c5,0\n"
                     "mcr p15,0,%0,c8,c7,0\nmcr p15,0,%0,c13,c0,0\n"
                     "mcr p15,0,%1,c2,c0,0\nmcr p15,0,%2,c3,c0,0\n"
                     "mcr p15,0,%3,c1,c0,0\nb 1f\n1:"
                     ::"r"(zero),"r"(address),"r"(domain),"r"(control):"memory");
    return (ln_arm926_cache_control()&5u)==5u;
}
bool ln_arm926_mmu_disable(void) {
    ln_arm926_data_clean();
    uint32_t zero=0,control=ln_arm926_cache_control()&~5u;
    __asm__ volatile("mcr p15,0,%0,c7,c6,0\nmcr p15,0,%0,c7,c5,0\n"
                     "mcr p15,0,%1,c1,c0,0\nb 1f\n1: mcr p15,0,%0,c8,c7,0"
                     ::"r"(zero),"r"(control):"memory");
    return !(ln_arm926_cache_control()&5u);
}
