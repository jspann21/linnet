/* Exercise the actual mixer without hardware: endpoints, pitch, spectrum,
 * levels, clipping, and DMA block partitioning. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal/api/click_pcm.h"
static const double pi=3.14159265358979323846;
static double high_band(const int32_t *samples,uint32_t count,uint32_t rate){
    double energy=0;
    for(uint32_t k=1;k<=count/2u;++k){
        if((double)k*rate/count<8000)continue;
        double re=0,im=0;
        for(uint32_t i=0;i<count;++i){double phase=2*pi*k*i/count;
            re+=samples[i]*cos(phase);im-=samples[i]*sin(phase);}
        energy+=re*re+im*im;
    }
    return energy;
}
int main(void){
    static const uint32_t rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000};
    static const uint16_t hz[]={1650,0,1250,2500};
    static const int32_t gains[]={0,512,1024,2048,4096};
    uint32_t cases=0;double worst_ratio=0;
    for(uint32_t r=0;r<9u;++r)for(uint8_t tone=0;tone<4u;++tone)for(uint8_t volume=1;volume<=4u;++volume){
        if(tone==1u)continue;
        uint32_t rate=rates[r],n=rate/250u;
        struct ln_pcm_click click={0};int32_t samples[192],old[192];
        ln_pcm_click_request(&click,rate,tone,volume);assert(click.left==n);
        for(uint32_t i=0;i<n;++i){
            samples[i]=ln_pcm_click_sample(&click);
            assert(abs(samples[i])<=gains[volume]);
            uint32_t phase=(i*((uint32_t)hz[tone]*65536u/rate))&65535u;
            double window=sin(pi*(double)(i*(32768u/(n-1u)))/32768.0);
            double ideal=sin(2*pi*phase/65536.0)*gains[volume]*window*window;
            assert(fabs(samples[i]-ideal)<7.0);
            int32_t triangle=phase<32768u?(int32_t)(phase*2u)-32768:98303-(int32_t)(phase*2u);
            uint32_t ramp=n-i<8u?n-i:8u;if(i<ramp)ramp=i;
            old[i]=((triangle*gains[volume])/32768)*(int32_t)ramp/8;
        }
        assert(samples[0]==0&&samples[n-1u]==0&&!click.left&&ln_pcm_click_sample(&click)==0);
        if(rate>=44100u){double ratio=high_band(samples,n,rate)/high_band(old,n,rate);
            if(ratio>worst_ratio)worst_ratio=ratio;assert(ratio<0.01);}
        /* Identical PCM, independent of DMA splits; source remains immutable. */
        uint32_t source[256],snapshot[256],whole[256],split[256];
        for(uint32_t i=0;i<256u;++i)source[i]=(uint16_t)(i*71u)|((uint32_t)(uint16_t)(-30000+(int32_t)i*31)<<16);
        memcpy(snapshot,source,sizeof source);memcpy(whole,source,sizeof source);memcpy(split,source,sizeof source);
        struct ln_pcm_click a={0},b={0};ln_pcm_click_request(&a,rate,tone,volume);ln_pcm_click_request(&b,rate,tone,volume);
        assert(ln_pcm_click_mix(&a,whole,source,256));
        for(uint32_t i=0;i<256u;i+=7u){uint32_t count=256u-i<7u?256u-i:7u;
            (void)ln_pcm_click_mix(&b,split+i,source+i,count);}
        assert(!memcmp(whole,split,sizeof whole)&&!memcmp(source,snapshot,sizeof source));++cases;
    }
    struct ln_pcm_click click={0};
    ln_pcm_click_request(&click,44100,1,4);assert(!click.left);
    ln_pcm_click_request(&click,44100,0,0);assert(!click.left);
    ln_pcm_click_request(&click,0,0,4);assert(!click.left);
    ln_pcm_click_request(&click,44100,0,4);struct ln_pcm_click saved=click;
    ln_pcm_click_request(&click,44100,3,1);assert(!memcmp(&saved,&click,sizeof click));
    ln_pcm_click_clear(&click);assert(!click.left);
    assert((int16_t)ln_pcm_click_add(32760,4096)==32767);
    assert((int16_t)ln_pcm_click_add(-32760,-4096)==-32768);
    printf("PASS: %u rate/pitch/level cases, zero endpoints, analytic sine, immutable PCM, DMA splits, clipping; worst >8kHz energy ratio %.8f (%.1f dB)\n",cases,worst_ratio,10*log10(worst_ratio));
    return 0;
}
