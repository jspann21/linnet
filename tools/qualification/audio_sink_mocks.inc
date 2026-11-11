#include <string.h>
static uint32_t audit_now,audit_irq_depth,audit_in_irq,audit_i2c_in_irq;
static uint16_t audit_codec[128];
static uint32_t audit_rate_writes;
static const uint32_t *audit_samples;
static uint32_t audit_mute_at,audit_release_at,audit_analog_changes,audit_release_while_nonzero;
static int audit_reject=-1;
static bool audit_stop_fail,audit_delay_fail,audit_clockoff_fail,audit_reset;
static uint32_t audit_codec_writes;
static uint32_t audit_delay_total,audit_mclk=12000000u,audit_bad_powerdown,audit_analog_at;
static bool audit_codec_live;
static uint32_t audit_mmio[128][2],audit_mmio_count;
static void audit_codec_write_event(uint8_t reg,uint16_t value);
static uint32_t *audit_register(uint32_t address){
    for(unsigned i=0;i<audit_mmio_count;i++)if(audit_mmio[i][0]==address)return &audit_mmio[i][1];
    unsigned i=audit_mmio_count++;audit_mmio[i][0]=address;return &audit_mmio[i][1];
}
static bool audit_write(uint8_t reg,uint16_t value){
    if(reg==6||reg==7||(reg>=0x24&&reg<=0x27))audit_rate_writes++;
    if(audit_in_irq)audit_i2c_in_irq++;
    if(reg==audit_reject)return false;
#ifndef AUDIT_PP
    if(audit_reset)return false;
#endif
    audit_codec_writes++;
    audit_codec_write_event(reg,value);
    audit_codec[reg]=value;return true;
}
uint32_t ln_core_irq_save(void){uint32_t old=audit_irq_depth;audit_irq_depth++;return old;}
void ln_core_irq_restore(uint32_t flags){audit_irq_depth=flags;}
bool ln_core_delay_us(uint32_t delay){
    if(audit_delay_fail||delay>100000u)return false;
    audit_delay_total+=delay;audit_now+=delay;return true;
}
static void audit_tick(uint32_t now);
void ln_core_idle_until(uint32_t deadline){audit_tick(deadline);}
#ifdef AUDIT_PP
uint32_t ln_time_us(void){
    if(audit_stop_fail)*audit_register(0x6000b004u)=0xc0000000u;
    else if(!(*audit_register(0x6000b000u)&0xc0000000u))*audit_register(0x6000b004u)=0;
    return audit_now;
}
bool ln_power_i2c_write(uint8_t address,uint8_t reg,uint8_t value){
    (void)address;return audit_write(reg>>1,(uint16_t)((reg&1u)<<8)|value);
}
#else
static bool audit_dma_active,audit_stream_started;
uint32_t ln_time_us(void){return audit_now;}
void ln_memory_copy(void *out,const void *in,uint32_t bytes){memcpy(out,in,bytes);}
void ln_arm926_data_clean_range(const void *address,uint32_t bytes){(void)address;(void)bytes;}
uint32_t ln_s5l_irq_save(void){return ln_core_irq_save();}
void ln_s5l_irq_restore(uint32_t flags){ln_core_irq_restore(flags);}
void ln_s5l_irq_enable(unsigned source,bool enabled){(void)source;(void)enabled;}
bool ln_s5l_board_codec_write(uint8_t reg,uint16_t value){return audit_write(reg,value);}
bool ln_s5l_board_audio_power(bool reset){audit_reset=reset;if(reset){audit_codec_live=false;memset(audit_codec,0,sizeof(audit_codec));}return true;}
bool ln_s5l_board_audio_clock(uint32_t rate,uint32_t *codec_rate){audit_mclk=rate==32000u?8000000u:12000000u;*codec_rate=rate==32000u?48000u:rate;return true;}
bool ln_s5l_board_audio_clock_off(void){audit_reset=true;return !audit_clockoff_fail;}
bool ln_s5l_pcm_stop(void){
    if(audit_stop_fail)return false;
    audit_stream_started=audit_dma_active=false;return true;
}
bool ln_s5l_pcm_begin(const int16_t *samples,uint32_t frames){
    audit_samples=(const uint32_t *)samples;(void)frames;audit_dma_active=audit_stream_started=true;return true;
}
bool ln_s5l_pcm_rearm(const int16_t *samples,uint32_t frames){return ln_s5l_pcm_begin(samples,frames);}
void ln_s5l_pcm_status(uint32_t out[5]){out[0]=1;out[1]=out[2]=out[3]=out[4]=0;}
void ln_s5l_pcm_interrupt(bool enabled){(void)enabled;}
#endif
