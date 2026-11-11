#pragma once
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
enum { LN_S5L_BOOT_RECOVERY=0, LN_S5L_BOOT_NORMAL=1, LN_S5L_BOOT_DISK=2, LN_S5L_BOOT_ATTEMPT=3, LN_S5L_BOOT_ATTEMPT_DISK=4 };
static inline int ln_s5l_boot_mode(void) {
    const uint8_t *record=(const uint8_t *)(uintptr_t)0x22017b00u;
    return ln_read_le32(record)==0x54424e4cu && ln_read_le32(record+8)==ln_crc32(record,8)?(int)ln_read_le32(record+4):-1;
}
static inline void ln_s5l_boot_request(unsigned mode) {
    uint8_t *record=(uint8_t *)(uintptr_t)0x22017b00u;
    ln_write_le32(record,0);ln_write_le32(record+4,mode);
    ln_write_le32(record,0x54424e4cu);ln_write_le32(record+8,ln_crc32(record,8));
    uint32_t zero=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(zero):"memory");
}
/* The serial is bound by the host to the inspected Apple USB endpoint before
 * loading the monitor. Retain it separately from image/generation handoffs so
 * ordinary release Boot and OS can expose the same identity in Disk Mode. */
static inline bool ln_s5l_boot_serial_save(const char *serial) {
    uint8_t *record=(uint8_t *)(uintptr_t)0x22017e80u;
    for(unsigned i=0;i<16;i++){
        char c=serial[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')))return false;
    }
    if(serial[16])return false;
    ln_write_le32(record,0);
    for(unsigned i=0;i<16;i++)record[4+i]=(uint8_t)serial[i];
    record[20]=0;record[21]=record[22]=record[23]=0;
    ln_write_le32(record,0x52534e4cu);ln_write_le32(record+24,ln_crc32(record,24));
    uint32_t zero=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(zero):"memory");
    return true;
}
static inline const char *ln_s5l_boot_serial(void) {
    const uint8_t *record=(const uint8_t *)(uintptr_t)0x22017e80u;
    if(ln_read_le32(record)!=0x52534e4cu || record[20] ||
       ln_read_le32(record+24)!=ln_crc32(record,24))return 0;
    for(unsigned i=4;i<20;i++)if(!((record[i]>='0'&&record[i]<='9')||
                                  (record[i]>='A'&&record[i]<='F')))return 0;
    return (const char *)record+4;
}
