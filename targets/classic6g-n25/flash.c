#include "flash.h"
#include "hal/s5l8702/spi.h"
extern uint32_t ln_time_us(void);
bool ln_n25_flash_identity(uint8_t id[3]) {
    if(!id || !ln_s5l_spi0_begin())return false;
    uint8_t discard;bool ok=ln_s5l_spi0_byte(0x9f,&discard);
    for(unsigned i=0;i<3 && ok;i++)ok=ln_s5l_spi0_byte(0xff,id+i);
    ln_s5l_spi0_end();return ok;
}
bool ln_n25_flash_read(uint32_t offset,uint8_t *bytes,uint32_t size) {
    if(!bytes || !size || size>512 || offset>=0x1000000u || size>0x1000000u-offset)return false;
    if(!ln_s5l_spi0_begin())return false;
    uint8_t discard;bool ok=ln_s5l_spi0_byte(3,&discard) &&
        ln_s5l_spi0_byte((uint8_t)(offset>>16),&discard) &&
        ln_s5l_spi0_byte((uint8_t)(offset>>8),&discard) && ln_s5l_spi0_byte((uint8_t)offset,&discard);
    for(unsigned i=0;i<size && ok;i++)ok=ln_s5l_spi0_byte(0xff,bytes+i);
    ln_s5l_spi0_end();return ok;
}
static bool instruction(uint8_t command) {
    uint8_t discard;if(!ln_s5l_spi0_begin())return false;
    bool ok=ln_s5l_spi0_byte(command,&discard);ln_s5l_spi0_end();return ok;
}
static bool status_read(uint8_t *status) {
    uint8_t discard;if(!ln_s5l_spi0_begin())return false;
    bool ok=ln_s5l_spi0_byte(5,&discard) && ln_s5l_spi0_byte(0xff,status);
    ln_s5l_spi0_end();return ok;
}
static bool ready(uint32_t maximum) {
    uint32_t started=ln_time_us();uint8_t value;
    for(unsigned budget=10000;budget;--budget) {
        if(!status_read(&value))return false;if(!(value&1u))return true;
        if(ln_time_us()-started>=maximum)return false;
    }
    return false;
}
static bool status_write(uint8_t value) {
    uint8_t discard;
    if(!instruction(0x50) || !ln_s5l_spi0_begin())return false;
    bool ok=ln_s5l_spi0_byte(1,&discard) && ln_s5l_spi0_byte(value,&discard);
    ln_s5l_spi0_end();return ok && ready(100000);
}
static bool write_at(uint8_t command,uint32_t address,int byte) {
    uint8_t discard,status;
    if(!instruction(6) || !status_read(&status) || !(status&2u) || !ln_s5l_spi0_begin())return false;
    bool ok=ln_s5l_spi0_byte(command,&discard) && ln_s5l_spi0_byte((uint8_t)(address>>16),&discard) &&
        ln_s5l_spi0_byte((uint8_t)(address>>8),&discard) && ln_s5l_spi0_byte((uint8_t)address,&discard);
    if(byte>=0 && ok)ok=ln_s5l_spi0_byte((uint8_t)byte,&discard);
    ln_s5l_spi0_end();return ok && ready(command==0x20?100000u:1000u);
}
uint32_t ln_n25_flash_replace_sector(uint32_t offset,const uint8_t bytes[4096]) {
    if(!bytes || (offset&4095u) || offset<0x8000u || offset>=0x29000u)return 1;
    uint8_t id[3],before,after,scratch[512];
    if(!ln_n25_flash_identity(id) || id[0]!=0xbf || id[1]!=0x25 || id[2]!=0x8e ||
       !status_read(&before) || (before&0x41u))return 2;
    uint32_t error=0,started=ln_time_us();
    if(!instruction(4) || !status_write(before&0x80u) || !status_read(&after) || (after&0x3cu))error=3;
    if(!error && !write_at(0x20,offset,-1))error=4;
    for(unsigned chunk=0;chunk<8 && !error;chunk++) {
        if(!ln_n25_flash_read(offset+chunk*512u,scratch,512)){error=5;break;}
        for(unsigned i=0;i<512;i++)if(scratch[i]!=0xff){error=5;break;}
    }
    for(unsigned i=0;i<4096 && !error;i++) {
        /* Commit the header's magic last, after its entire sector is ready. */
        unsigned at=offset==0x8000u?(i+4u)&4095u:i;
        if(ln_time_us()-started>=2000000u){error=6;break;}
        if(bytes[at]!=0xff && !write_at(2,offset+at,bytes[at]))error=7;
    }
    bool protected=status_write(before&0xbcu) && instruction(4) && status_read(&after) &&
        (after&0xbfu)==(before&0xbcu);
    if(!protected)return 8;
    for(unsigned chunk=0;chunk<8 && !error;chunk++) {
        if(!ln_n25_flash_read(offset+chunk*512u,scratch,512)){error=9;break;}
        for(unsigned i=0;i<512;i++)if(scratch[i]!=bytes[chunk*512u+i]){error=9;break;}
    }
    return error;
}
