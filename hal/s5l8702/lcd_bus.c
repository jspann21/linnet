#include "lcd_bus.h"
#include "registers.h"
#include "hal/arm926/cache.h"
#define LCD(offset) LN_REG(0x38300000u+(offset))
extern uint32_t ln_time_us(void);
static bool ready(uint32_t mask,uint32_t value) {
    if((LCD(0x1c)&mask)==value)return true;
    uint32_t deadline=ln_time_us()+2000u,budget=200000u;
    while((LCD(0x1c)&mask)!=value)
        if(!budget-- || (int32_t)(ln_time_us()-deadline)>=0)return false;
    return true;
}
bool ln_s5l_lcd_mode(uint32_t mode) {
    if(!ready(2,2))return false;
    uint32_t start=ln_time_us();
    for(unsigned budget=0;ln_time_us()==start;budget++)if(budget==200000)return false;
    LCD(0)=mode;return true;
}
bool ln_s5l_lcd_command(uint16_t command) {
    if(!ready(0x10,0))return false;
    LCD(4)=command;return true;
}
bool ln_s5l_lcd_data(uint16_t data) {
    if(!ready(0x10,0))return false;
    LCD(0x40)=data;return true;
}
bool ln_s5l_lcd_read(uint8_t *data) {
    if(!data || !ready(2,2))return false;
    LCD(0x10)=0;
    if(!ready(1,1))return false;
    *data=(uint8_t)(LCD(0x14)>>1);return true;
}
/* PL080 channel 4 belongs to the LCD; channel 0 independently owns PCM.
 * S5L8702 request 3 writes RGB565 halfwords into the LCD data FIFO. */
#define DMA(offset) LN_REG(0x38200000u+(offset))
#define LCD_CHANNEL (1u<<4)
static struct {uint32_t source,destination,next,control;} lcd_chain[20] __attribute__((aligned(32)));
static bool dma_owned;
static uint32_t dma_started;
bool ln_s5l_lcd_dma_begin(const uint16_t *pixels,uint32_t count){
    if(!pixels||((uintptr_t)pixels&1u)||!count||count>20u*4095u||dma_owned)return false;
    LN_CLOCK_GATE0&=~(1u<<25);
    if(DMA(0x1c)&LCD_CHANNEL)return false;
    ln_arm926_data_clean_range(pixels,count*2u);
    unsigned index=0;
    while(count){
        uint32_t n=count>4095u?4095u:count;count-=n;
        lcd_chain[index].source=(uint32_t)(uintptr_t)pixels;
        lcd_chain[index].destination=0x38300040u;
        lcd_chain[index].next=count?(uint32_t)(uintptr_t)&lcd_chain[index+1]:0;
        lcd_chain[index].control=n|(1u<<18)|(1u<<21)|(1u<<26)|(1u<<28)|(count?0:1u<<31);
        pixels+=n;++index;
    }
    ln_arm926_data_clean_range(lcd_chain,index*sizeof(lcd_chain[0]));
    DMA(0x30)|=1;DMA(0x190)=0;DMA(8)=LCD_CHANNEL;DMA(0x10)=LCD_CHANNEL;
    DMA(0x180)=lcd_chain[0].source;DMA(0x184)=lcd_chain[0].destination;
    DMA(0x188)=lcd_chain[0].next;DMA(0x18c)=lcd_chain[0].control;
    dma_started=ln_time_us();dma_owned=true;DMA(0x190)=1u|(3u<<6)|(1u<<11);return true;
}
int ln_s5l_lcd_dma_poll(void){
    if(!dma_owned)return -1;
    bool complete=!(DMA(0x1c)&LCD_CHANNEL)&&(DMA(0x14)&LCD_CHANNEL)&&(LCD(0x1c)&2u);
    /* A long foreground hash can postpone this poll after DMA has completed.
     * Expire only an unfinished transfer; a late successful poll is valid. */
    bool failed=(DMA(0x18)&LCD_CHANNEL)||(!complete&&ln_time_us()-dma_started>=500000u);
    if(!failed&&!complete)return 0;
    if(failed){
        DMA(0x190)|=1u<<18;uint32_t began=ln_time_us();
        while(DMA(0x190)&(1u<<17))if(ln_time_us()-began>=20000u)break;
    }
    DMA(0x190)=0;DMA(8)=LCD_CHANNEL;DMA(0x10)=LCD_CHANNEL;dma_owned=false;
    return failed?-1:1;
}
