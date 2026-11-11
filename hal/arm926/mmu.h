#pragma once
#include <stdbool.h>
#include <stdint.h>
/* 16 KiB aligned, caller-owned physical table. Identity maps all addresses,
 * caches only the declared RAM, and maps its uncached alias at +0x10000000.
 * Call with IRQ/FIQ masked and no DMA touching cached RAM. Uncached MMIO and
 * SRAM keep their physical addresses. RAM uses whole MiB sections. */
bool ln_arm926_mmu_enable(uint32_t table[4096],uint32_t ram,uint32_t bytes);
/* Cleans RAM and returns to physical addressing before a boot handoff. */
bool ln_arm926_mmu_disable(void);
