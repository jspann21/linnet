#include "board.h"
#include "hal/s5l8702/i2c.h"
#include "hal/s5l8702/registers.h"
#include "hal/s5l8702/board.h"
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
#include "hal/s5l8702/clock_resources.h"
#endif
#define MIU(offset) LN_REG(0x38100000u+(offset))
extern uint32_t ln_time_us(void);
static bool pmu_write(uint8_t reg,uint8_t value) {
    return ln_s5l_i2c_write(0,0x73,reg,&value,1);
}
bool ln_s5l_board_cpu_voltage(bool boosted){
    static uint8_t previous;static bool saved;
    if(boosted){
        if(!saved){if(!ln_s5l_i2c_read(0,0x73,0x1e,&previous,1))return false;saved=true;}
        /* PCF50635 DOWN1: 1.1 V for the normal 216 MHz operating point.
         * Preserve an inherited voltage above that level. */
        if(previous>=0x13)return true;
        if(!pmu_write(0x1e,0x13))return false;
        uint32_t began=ln_time_us();while(ln_time_us()-began<1000u){}
        uint8_t value=0;return ln_s5l_i2c_read(0,0x73,0x1e,&value,1)&&value==0x13;
    }
    if(!saved)return true;
    if(!pmu_write(0x1e,previous))return false;saved=false;return true;
}
bool ln_s5l_board_input_power(void) {
    return pmu_write(0x15,0) && pmu_write(0x37,0x15) && pmu_write(0x38,4);
}
bool ln_s5l_board_audio_power(bool reset) {
    if(!pmu_write(0x2d,0x15) || !pmu_write(0x2e,1) ||
       !pmu_write(0x2f,0x15) || !pmu_write(0x30,1) ||
       !pmu_write(0x33,9) || !pmu_write(0x34,1))return false;
    LN_CLOCK_GATE1&=~(1u<<7);
    LN_REG(0x3cf00040u)=(LN_REG(0x3cf00040u)&0xffffu)|0x23320000u;
    LN_REG(0x3cf00060u)=(LN_REG(0x3cf00060u)&~15u)|2u;
    LN_REG(0x3ca00004u)=0x0b100019u;LN_REG(0x3ca00000u)=1;
    LN_REG(0x3c50000cu)&=0xffff0000u;
    LN_REG(0x3cf00060u)=(LN_REG(0x3cf00060u)&~0xf000u)|0x1000u;
    if(reset)LN_REG(0x3cf00064u)&=~8u;else LN_REG(0x3cf00064u)|=8u;
    return true;
}
bool ln_s5l_board_codec_write(uint8_t reg,uint16_t value) {
    uint8_t byte=(uint8_t)value;
    return value<=255 && ln_s5l_i2c_write(0,0x4a,reg,&byte,1);
}
bool ln_s5l_board_audio_clock(uint32_t rate,uint32_t *codec_rate) {
    uint32_t divider,clock=0;
    if(!codec_rate)return false;
    switch(rate) {
    case 8000:divider=1500;break;case 11025:divider=1088;break;
    case 12000:divider=1000;break;case 16000:divider=750;break;
    case 22050:divider=544;break;case 24000:divider=500;break;
    case 32000:divider=250;clock=0x3028;break;
    case 44100:divider=272;break;case 48000:divider=250;break;
    default:return false;
    }
    if(rate==32000) {
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
        if(!ln_s5l_pll2_acquire(LN_S5L_PLL2_AUDIO))return false;
#else
        /* CS42L55 uses its 48 kHz mode with an 8 MHz MCLK on N25.
         * PLL2 = 216 MHz, divided by 3 and 9. PLL0/USB are untouched. */
        if(LN_REG(0x3c500028u)!=0x04004800u)return false;
        LN_REG(0x3c500038u)=0xe10;LN_REG(0x3c500044u)&=~(1u<<6);
        LN_REG(0x3c500044u)|=4u;
        uint32_t start=ln_time_us(),budget=1000000;
        while(!(LN_REG(0x3c500040u)&4u))if(!budget-- || ln_time_us()-start>=10000u)return false;
        LN_REG(0x3c500044u)|=1u<<18;
#endif
    }
    LN_REG(0x3c50000cu)=(LN_REG(0x3c50000cu)&0xffff0000u)|clock|0x8000u;
    uint32_t started=ln_time_us(),budget=1000000;
    while(ln_time_us()-started<100u)if(!budget--)return false;
    LN_REG(0x3c50000cu)&=~0x8000u;LN_REG(0x3ca00040u)=divider;
    *codec_rate=rate==32000?48000:rate;
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
    if(rate!=32000&&!ln_s5l_pll2_release(LN_S5L_PLL2_AUDIO))return false;
#endif
    return true;
}
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
bool ln_s5l_board_audio_clock_off(void){
    /* Called only after PCM has drained and the codec acknowledged power-down.
     * CS42L55 DS773F1 4.11.1 also requires RESET low. PDN acknowledgement
     * alone does not put the external analog circuitry in its reset state.
     * Assert the board's GPIO C3 before removing MCLK, and keep it asserted
     * until the next cold audio initialization. Keep PMU rails intact so
     * headset sensing remains available at idle. */
    LN_REG(0x3cf00064u)&=~8u;
    if(LN_REG(0x3cf00064u)&8u)return false;
    LN_REG(0x3ca00000u)=0;
    /* Bit 1 is status and remains set after disabling the clock. */
    if((LN_REG(0x3ca00000u)&1u)||!ln_s5l_pll2_release(LN_S5L_PLL2_AUDIO))return false;
    LN_CLOCK_GATE1|=1u<<7;return true;
}
#endif
bool ln_s5l_board_switches(bool *hold,bool *headphones) {
    uint8_t value;
    if(!hold || !headphones || !ln_s5l_i2c_read(0,0x73,0x87,&value,1))return false;
    *hold=!(value&2u);*headphones=(LN_REG(0x3cf00144u)&0x40u)!=0;return true;
}
bool ln_n25_lcd_power(void) {
    if(!pmu_write(0x31,0x15) || !pmu_write(0x32,1))return false;
    LN_CLOCK_GATE1&=~(1u<<12);
    LN_REG(0x3cf00060u)=(LN_REG(0x3cf00060u)&0xffffu)|0x33330000u;
    LN_REG(0x3cf00080u)=0x33333333u;
    LN_REG(0x3cf000a0u)=0x33333333u;
    LN_REG(0x3cf000c0u)=(LN_REG(0x3cf000c0u)&0x00ffff00u)|0x3f000033u;
    LN_CLOCK_GATE0&=~2u;
    return true;
}
bool ln_s5l_board_disk_power(void) {
    /* This first PATA path must not drive a CE-ATA board's pin mux. */
    LN_CLOCK_GATE1&=~(1u<<12);
    if(LN_REG(0x3cf00164u)&2u)return false;
    if(!pmu_write(0x1a,0x6f) || !pmu_write(0x1c,0) ||
       !pmu_write(0x1d,0x59) || !pmu_write(0x1b,1))return false;
    for(unsigned port=7;port<=9;port++)LN_REG(0x3cf00000u+port*32u)=0x44444444;
    LN_REG(0x3cf00140u)=(LN_REG(0x3cf00140u)&~0xffffu)|0x4444;
    LN_CLOCK_GATE0&=~(1u<<5);
    return true;
}
bool ln_s5l_board_disk_power_off(void) {
    /* Isolate PATA pins before removing AUTO's disk supply. */
    for(unsigned port=7;port<=9;port++)LN_REG(0x3cf00000u+port*32u)=0;
    LN_REG(0x3cf00140u)&=~0xffffu;
    if(!pmu_write(0x1b,0))return false;
    uint8_t enabled;
    if(!ln_s5l_i2c_read(0,0x73,0x1b,&enabled,1) || enabled)return false;
    LN_CLOCK_GATE0|=1u<<5;
    return true;
}
bool ln_n25_power_identify(struct ln_n25_power_identity *identity) {
    uint8_t id[2],status;
    if(!identity || !ln_s5l_i2c_read(0,0x73,0,id,2) ||
       !ln_s5l_i2c_read(0,0x73,0x12,&status,1))return false;
    identity->version=id[0];identity->variant=id[1];identity->status=status;
    return !(id[0]==0xff && id[1]==0xff);
}
static void command(uint32_t value) {
    MIU(4)=value;
    MIU(4)=0x33;MIU(4)=0x33;MIU(4)=0x33;
}
bool ln_n25_memory_init(uint32_t *failure_address) {
    if(!failure_address)return false;
    *failure_address=0;
    struct ln_n25_power_identity identity;
    if(!ln_n25_power_identify(&identity))return false;
    /* PCF50635 DOWN2 and MEMLDO supply the N25 SDRAM at 1.8 V. */
    if(!pmu_write(0x22,0x2f) || !pmu_write(0x24,0) || !pmu_write(0x25,0x10) ||
       !pmu_write(0x23,1) || !pmu_write(0x26,9) || !pmu_write(0x27,1))return false;
    uint32_t until=ln_time_us()+1000u,budget=1000000u;
    while((int32_t)(ln_time_us()-until)<0)if(!budget--)return false;
    LN_CLOCK_GATE0&=~0x18u;
    LN_REG(0x3c500000u)&=~(1u<<31);
    MIU(0)=0x80d;MIU(0xf0)=0;MIU(8)=0x6105d;MIU(0x10)=0x1fb621;
    static const uint16_t drive_offsets[]={0x200,0x204,0x210,0x214,0x220,0x224,0x230,0x234};
    static const uint16_t drive_values[]={0x1845,0x1845,0x1800,0x1800,0x1845,0x1845,0x1885,0x1885};
    for(unsigned n=0;n<8;n++)MIU(drive_offsets[n])=drive_values[n];
    MIU(0x14)=0x19;MIU(0x18)=0x19;MIU(0x1c)=0x790682b;
    MIU(0x314)&=~0x10u;
    for(unsigned offset=0x2c;offset<0xbc;offset+=4)MIU(offset)&=~(1u<<24);
    MIU(0x1cc)=0x540;MIU(0x1d4)|=0x80;
    command(0x33);command(0x233);command(0x333);command(0x333);
    MIU(12)=0x33;command(0x133);MIU(12)=0x8040;command(0x133);
    MIU(8)|=0x61000;
    /* Detect stuck data lines and bank/address aliasing before advertising RAM. */
    const uint32_t base=0x08000000u;
    for(unsigned bit=0;bit<32;bit++) {
        uint32_t pattern=1u<<bit;
        LN_REG(base)=pattern;
        if(LN_REG(base)!=pattern){*failure_address=base;return false;}
        LN_REG(base)=~pattern;
        if(LN_REG(base)!=~pattern){*failure_address=base;return false;}
    }
    LN_REG(base)=0x6543210u;
    for(unsigned bit=2;bit<26;bit++)LN_REG(base+(1u<<bit))=0xa5000000u|bit;
    if(LN_REG(base)!=0x6543210u){*failure_address=base;return false;}
    for(unsigned bit=2;bit<26;bit++) {
        uint32_t address=base+(1u<<bit);
        if(LN_REG(address)!=(0xa5000000u|bit)){*failure_address=address;return false;}
    }
    return true;
}
