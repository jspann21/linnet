#include "pcm_dma.h"
#include "registers.h"
#include "hal/arm926/cache.h"
/* PL080 channel 0, S5L8702 I2S0 transmit request 10. */
#define DMA(offset) LN_REG(0x38200000u+(offset))
#define I2S(offset) LN_REG(0x3ca00000u+(offset))
extern uint32_t ln_time_us(void);
static bool owned;
struct descriptor {uint32_t source,destination,next,control;};
static struct descriptor chain[64] __attribute__((aligned(32)));
#define PCM_CONTROL ((2u<<12)|(1u<<15)|(1u<<18)|(1u<<21)|(1u<<26)|(1u<<28))
bool ln_s5l_pcm_stop(void) {
    LN_CLOCK_GATE0&=~(1u<<25);
    if(!(DMA(0x1c)&1)){
        if(owned){I2S(8)=0x0a;DMA(0x110)=0;DMA(8)=1;DMA(0x10)=1;}
        owned=false;return true;
    }
    LN_CLOCK_GATE1&=~(1u<<7);
    DMA(0x110)|=1u<<18;
    uint32_t start=ln_time_us();
    for(unsigned budget=1000000;DMA(0x110)&(1u<<17);--budget)
        /* A stopped I2S clock cannot drain the FIFO. This channel only
         * reads PCM into I2S, so an abort may discard queued samples. */
        if(!budget || ln_time_us()-start>=20000)break;
    DMA(0x110)=0;I2S(8)=0x0a;
    DMA(8)=1;DMA(0x10)=1;owned=false;
    return !(DMA(0x1c)&1);
}
bool ln_s5l_pcm_begin(const int16_t *samples,uint32_t frames) {
    if(!samples || ((uintptr_t)samples&3) || !frames || frames>2047)return false;
    return ln_s5l_pcm_begin_buffer(samples,frames);
}
static bool begin_buffer(const int16_t *samples,uint32_t frames,bool continuing) {
    if(!samples || ((uintptr_t)samples&3) || !frames || frames>64u*2040u ||
       (uintptr_t)samples>UINT32_MAX-frames*4u)return false;
    LN_CLOCK_GATE0&=~(1u<<25);
    if(continuing){if(!owned || (DMA(0x1c)&1u))return false;DMA(0x110)=0;}
    else if(!ln_s5l_pcm_stop() || (DMA(0x1c)&1u))return false;
#if defined(LINNET_IMAGE_OS) && LINNET_AUDIO_CLOCK_GATING
    LN_CLOCK_GATE1&=~(1u<<7);
#endif
    ln_arm926_data_clean_range(samples,frames*4u);
    unsigned index=0;
    while(frames) {
        uint32_t count=frames>2040u?2040u:frames;frames-=count;
        chain[index].source=(uint32_t)(uintptr_t)samples;
        chain[index].destination=0x3ca00010u;
        chain[index].next=frames?(uint32_t)(uintptr_t)&chain[index+1]:0;
        chain[index].control=count*2u|PCM_CONTROL|(frames?0:1u<<31);
        samples+=count*2u;index++;
    }
    ln_arm926_data_clean_range(chain,index*sizeof(chain[0]));
    DMA(0x30)=1;DMA(8)=1;DMA(0x10)=1;
    DMA(0x100)=chain[0].source;
    DMA(0x104)=chain[0].destination;DMA(0x108)=chain[0].next;
    /* Halfword transfers, source increment, bursts 8/4, AHB master 1. */
    DMA(0x10c)=chain[0].control;
    uint32_t zero=0;
    __asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(zero):"memory");
    owned=true;I2S(8)=0x0e;
    DMA(0x110)=1u|(10u<<6)|(1u<<11);
    return true;
}
bool ln_s5l_pcm_begin_buffer(const int16_t *samples,uint32_t frames){return begin_buffer(samples,frames,false);}
bool ln_s5l_pcm_rearm(const int16_t *samples,uint32_t frames){return begin_buffer(samples,frames,true);}
void ln_s5l_pcm_status(uint32_t result[5]) {
    result[0]=DMA(0x14)&1;result[1]=DMA(0x18)&1;
    result[2]=DMA(0x10c)&0xfff;result[3]=DMA(0x110);result[4]=I2S(0x3c);
}
void ln_s5l_pcm_interrupt(bool enabled) {
    if(owned)DMA(0x110)=(DMA(0x110)&~(3u<<14))|(enabled?3u<<14:0);
}
