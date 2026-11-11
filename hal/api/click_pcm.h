#ifndef LINNET_CLICK_PCM_H
#define LINNET_CLICK_PCM_H
#include <stdbool.h>
#include <stdint.h>

/* A short sine click with a raised-sine envelope. The sink owns this state under its
 * IRQ lock. Mixing uses a separate DMA block and never changes queued music. */
struct ln_pcm_click {uint32_t phase,step,left,total,envelope_step;int32_t gain;};
static inline int32_t ln_pcm_click_sine(uint32_t phase){
    static const int16_t quarter[]={0,1608,3212,4808,6393,7962,9512,11039,
        12539,14010,15446,16846,18204,19519,20787,22005,23170,24279,
        25329,26319,27245,28105,28898,29621,30273,30852,31356,31785,
        32137,32412,32609,32728,32767};
    uint32_t at=phase&32767u;if(at>16384u)at=32768u-at;
    uint32_t index=at>>9,fraction=at&511u;
    int32_t value=quarter[index];
    if(fraction)value+=(quarter[index+1u]-value)*(int32_t)fraction/512;
    return phase&32768u?-value:value;
}
static inline void ln_pcm_click_clear(volatile struct ln_pcm_click *click){
    *click=(struct ln_pcm_click){0};
}
static inline void ln_pcm_click_request(volatile struct ln_pcm_click *click,uint32_t rate,uint8_t tone,uint8_t volume){
    static const uint16_t hz[]={1650,0,1250,2500};
    static const int16_t gain[]={0,512,1024,2048,4096};
    if(click->left||!rate||tone>3u||tone==1u||!volume||volume>4u)return;
    click->phase=0;click->step=(uint32_t)hz[tone]*65536u/rate;
    click->total=click->left=rate/250u;click->gain=gain[volume];
    click->envelope_step=click->total>1u?32768u/(click->total-1u):0u;
}
static inline int32_t ln_pcm_click_sample(volatile struct ln_pcm_click *click){
    if(!click->left)return 0;
    uint32_t age=click->total-click->left;
    int32_t wave=ln_pcm_click_sine(click->phase);
    int32_t envelope=ln_pcm_click_sine(age*click->envelope_step);
    envelope=envelope*envelope/32768;
    click->phase+=click->step;--click->left;
    if(!click->left)return 0;
    return ((wave*click->gain)/32768)*envelope/32768;
}
static inline uint16_t ln_pcm_click_add(int32_t pcm,int32_t click){
    int32_t sum=pcm+click;if(sum>32767)sum=32767;if(sum< -32768)sum=-32768;
    return (uint16_t)(int16_t)sum;
}
static inline bool ln_pcm_click_mix(volatile struct ln_pcm_click *click,uint32_t *out,const uint32_t *pcm,uint32_t frames){
    if(!click->left)return false;
    uint32_t mixed=frames<click->left?frames:click->left;
    for(uint32_t i=0;i<mixed;++i){
        uint32_t value=pcm?pcm[i]:0;int32_t sample=ln_pcm_click_sample(click);
        out[i]=ln_pcm_click_add((int16_t)value,sample)|
            ((uint32_t)ln_pcm_click_add((int16_t)(value>>16),sample)<<16);
    }
    for(uint32_t i=mixed;i<frames;++i)out[i]=pcm?pcm[i]:0;
    return true;
}
#endif
