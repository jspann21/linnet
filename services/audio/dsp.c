#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "services/audio/dsp.h"
/* Q8 signal values retain fractional precision between filters. Headroom is
 * bounded to +/-2^27 (24 dB above full-scale PCM16). A sum of five products of
 * these values and signed Q2.30 coefficients is below 5*2^58, safely in int64.
 * No negative signed left shift or overflowing signed arithmetic is used. */
/* Keep this hot block loop in cached SDRAM. Dedicated IRAM placement did not
 * improve measured PP5020 throughput; the same arithmetic is used here. */
#define LN_DSP_ICODE
#define GUARD 134217728
#define Q30 1073741824LL
static void increment(uint32_t *value){if(*value!=UINT32_MAX)++*value;}
#if !defined(__arm__)
static int32_t guard(struct ln_dsp *dsp,int64_t value)
{
    if(value>GUARD){increment(&dsp->internal_clips);return GUARD;}
    if(value<-GUARD){increment(&dsp->internal_clips);return -GUARD;}
    return (int32_t)value;
}
#endif
/* Round nearest with symmetric ties away from zero; divide by 2^bits. */
static int64_t rounded(int64_t value,uint32_t bits)
/* The ARM/native toolchains use arithmetic signed right shift. All callers
 * use 16, 22 or 30 bits and the bounded products cannot overflow with bias.
 * Keep bias unsigned32 so ARM needs only one carry-propagating addition. */
{ uint32_t bias=(1u<<(bits-1u))-(uint32_t)(value<0);
  return (value+bias)>>bits; }
void ln_dsp_init(struct ln_dsp *dsp)
{
    if(!dsp)return;
    *dsp=(struct ln_dsp){0};
    dsp->config.gain_q16=LINNET_DSP_UNITY;
    dsp->config.replaygain_q16=LINNET_DSP_UNITY;
    for(uint32_t i=0;i<LINNET_DSP_BANDS;++i)dsp->config.bands[i].b0=(int32_t)Q30;
}
uint32_t ln_dsp_gain_from_db(int32_t db_q16)
{
    static const uint32_t power[17]={1073741824u,1121280436u,1170923762u,
        1222764986u,1276901417u,1333434672u,1392470869u,1454120821u,
        1518500250u,1585730000u,1655936265u,1729250827u,1805811301u,
        1885761398u,1969251188u,2056437387u,2147483648u};
    if(db_q16==INT32_MIN)return LINNET_DSP_UNITY;
    if(db_q16<=-96*65536)return 1;
    if(db_q16>=25*65536)return 16u*LINNET_DSP_UNITY;
    /* log2(10)/20 rounded toQ30. ProductQ46 becomes a signedQ24 exponent. */
    int32_t logarithm=(int32_t)rounded((int64_t)db_q16*178344657,22);
    int32_t exponent=logarithm/16777216;
    int32_t fraction=logarithm-exponent*16777216;
    if(fraction<0){fraction+=16777216;exponent--;}
    uint32_t index=(uint32_t)fraction>>20,within=(uint32_t)fraction&1048575u;
    uint64_t value=power[index]+(((uint64_t)(power[index+1]-power[index])*within+524288u)>>20);
    uint32_t shift=(uint32_t)(14-exponent);
    uint32_t gain=(uint32_t)((value+(1ull<<(shift-1u)))>>shift);
    if(!gain)return 1;
    return gain>16u*LINNET_DSP_UNITY?16u*LINNET_DSP_UNITY:gain;
}
static bool valid_config(const struct ln_dsp_config *config)
{
    if(!config||config->gain_q16>16u*LINNET_DSP_UNITY||
       config->replaygain_q16>16u*LINNET_DSP_UNITY||config->balance < -1000||
       config->balance>1000||config->mode>LINNET_DSP_KARAOKE||
       config->band_count>LINNET_DSP_BANDS||config->crossfeed_amount_q16>32768u||
       config->crossfeed_coefficient_q16>65535u||config->headroom_db_tenths>90u||
       (config->crossfeed_amount_q16&&!config->crossfeed_coefficient_q16))return false;
    for(uint32_t i=0;i<config->band_count;++i){
        const struct ln_dsp_biquad *b=&config->bands[i];
        /* Strict second-order Jury conditions for poles inside unit circle.
         * int64 conversion precedes additions so extreme inputs cannot wrap. */
        int64_t a1=b->a1,a2=b->a2;
        if(a2<=-Q30||a2>=Q30||Q30+a1+a2<=0||Q30-a1+a2<=0)return false;
    }
    return true;
}
bool ln_dsp_configure(struct ln_dsp *dsp,const struct ln_dsp_config *config)
{
    if(!dsp||!valid_config(config))return false;
    struct ln_dsp_config copy=*config;
    /* Config may alias dsp->config. Copy before resetting histories. */
    for(uint32_t i=0;i<LINNET_DSP_BANDS;++i)
        for(uint32_t ch=0;ch<2;++ch)dsp->history[i][ch]=(struct ln_dsp_history){0};
    dsp->crossfeed_low[0]=dsp->crossfeed_low[1]=0;
    dsp->config=copy;return true;
}
bool ln_dsp_profile_decode(const uint8_t *data,uint32_t bytes,uint32_t rate,struct ln_dsp_config *out)
{
    if(!data||!out||bytes!=160||rate<8000||rate>192000||data[0]!='L'||data[1]!='D'||data[2]!='S'||data[3]!='P'||
       data[4]||data[5]||data[6]!=160||data[7]||ln_read_le32(data+8)!=rate||ln_read_le32(data+12))return false;
    for(uint32_t i=16;i<28;i++)if(data[i])return false;
    if(ln_crc32(data,156)!=ln_read_le32(data+156))return false;
    struct ln_dsp_config config;
    config.gain_q16=ln_read_le32(data+28);config.replaygain_q16=ln_read_le32(data+32);config.balance=(int32_t)ln_read_le32(data+36);
    config.mode=ln_read_le32(data+40);config.band_count=ln_read_le32(data+44);
    for(uint32_t i=0;i<5;i++){
        const uint8_t *p=data+48+i*20;struct ln_dsp_biquad *b=&config.bands[i];
        b->b0=(int32_t)ln_read_le32(p);b->b1=(int32_t)ln_read_le32(p+4);b->b2=(int32_t)ln_read_le32(p+8);b->a1=(int32_t)ln_read_le32(p+12);b->a2=(int32_t)ln_read_le32(p+16);
        if(i>=config.band_count&&(b->b0||b->b1||b->b2||b->a1||b->a2))return false;
    }
    config.crossfeed_amount_q16=ln_read_le32(data+148);config.crossfeed_coefficient_q16=ln_read_le32(data+152);
    config.headroom_db_tenths=0;
    if(!valid_config(&config))return false;
    *out=config;return true;
}
/* Keep one band's recurrence in locals across a bounded sample block. */
#if defined(__arm__)
/* ARM7 leaf keeps all coefficients/history in registers. The portable path
 * below defines the same rounding, guard and saturating counter behavior. */
extern void ln_dsp_crossfeed_stereo_arm(int32_t *,uint32_t,int32_t *,uint32_t,uint32_t);
extern void ln_dsp_gain_arm(int32_t *,uint32_t,uint32_t,uint32_t *);
extern void ln_dsp_filter_arm(const struct ln_dsp_biquad *,struct ln_dsp_history *,
                              int32_t *,uint32_t,uint32_t,uint32_t *);
static void filter_block(struct ln_dsp *dsp,uint32_t band,uint32_t ch,
                         int32_t *samples,uint32_t frames,uint32_t stride)
{
    ln_dsp_filter_arm(&dsp->config.bands[band],&dsp->history[band][ch],
                      samples,frames,stride,&dsp->internal_clips);
}
#else
static LN_DSP_ICODE void filter_block(struct ln_dsp *dsp,uint32_t band,uint32_t ch,
                                     int32_t *samples,uint32_t frames,uint32_t stride)
{
    const struct ln_dsp_biquad b=dsp->config.bands[band];
    struct ln_dsp_history *h=&dsp->history[band][ch];
    int32_t x1=h->x1,x2=h->x2,y1=h->y1,y2=h->y2;
    uint32_t clips=dsp->internal_clips;
    for(uint32_t i=0;i<frames;++i){
        int32_t x=*samples;
        int64_t sum=(int64_t)b.b0*x+(int64_t)b.b1*x1+(int64_t)b.b2*x2-
            (int64_t)b.a1*y1-(int64_t)b.a2*y2;
        int32_t y=(int32_t)rounded(sum,30);
        if(y>GUARD){increment(&clips);y=GUARD;}
        else if(y<-GUARD){increment(&clips);y=-GUARD;}
        x2=x1;x1=x;y2=y1;y1=y;*samples=y;samples+=stride;
    }
    h->x1=x1;h->x2=x2;h->y1=y1;h->y2=y2;dsp->internal_clips=clips;
}
#endif
static int16_t output(struct ln_dsp *dsp,int32_t value)
{
    /* Guarded Q8 values fit signed32 even after the rounding half-step. */
    int32_t sample=(value+128-(int32_t)(value<0))>>8;
    if(sample>32767){increment(&dsp->output_clips);return 32767;}
    if(sample<-32768){increment(&dsp->output_clips);return -32768;}
    return (int16_t)sample;
}
/* 128 stereo Q8 frames use 1024 bytes of bounded stack scratch. Processing
 * each stage across this block preserves its exact recurrence and rounding,
 * while avoiding coefficient/history reloads throughout the combined chain. */
#define DSP_BLOCK_FRAMES 128u
static __attribute__((always_inline)) inline void process_channels(struct ln_dsp *dsp,int16_t *pcm,uint32_t frames,uint32_t channels)
{
    const struct ln_dsp_config *c=&dsp->config;
    uint32_t left_scale=c->balance>0?(uint32_t)(1000-c->balance)*65536u/1000u:65536u;
    uint32_t right_scale=c->balance<0?(uint32_t)(1000+c->balance)*65536u/1000u:65536u;
    uint32_t remaining=frames,clips_before=dsp->internal_clips;
    int32_t samples[DSP_BLOCK_FRAMES*2];
    while(remaining){
        uint32_t count=remaining>DSP_BLOCK_FRAMES?DSP_BLOCK_FRAMES:remaining;
        for(uint32_t i=0;i<count;++i){
            int32_t left=(int32_t)pcm[i*2]*256,right=(int32_t)pcm[i*2+1]*256;
            if(channels==2){samples[i*2]=left;samples[i*2+1]=right;}
            else{
                switch(c->mode){
                case LINNET_DSP_MONO:samples[i]=(left+right)/2;break;
                case LINNET_DSP_LEFT:samples[i]=left;break;
                case LINNET_DSP_RIGHT:samples[i]=right;break;
                default:samples[i]=(left-right)/2;break;
                }
            }
        }
#if defined(__arm__)
        if(c->gain_q16!=LINNET_DSP_UNITY)
            ln_dsp_gain_arm(samples,count*channels,c->gain_q16,&dsp->internal_clips);
        if(c->replaygain_q16!=LINNET_DSP_UNITY)
            ln_dsp_gain_arm(samples,count*channels,c->replaygain_q16,&dsp->internal_clips);
#else
        if(c->gain_q16!=LINNET_DSP_UNITY)
            for(uint32_t i=0;i<count*channels;++i)samples[i]=guard(dsp,rounded((int64_t)samples[i]*(int32_t)c->gain_q16,16));
        if(c->replaygain_q16!=LINNET_DSP_UNITY)
            for(uint32_t i=0;i<count*channels;++i)samples[i]=guard(dsp,rounded((int64_t)samples[i]*(int32_t)c->replaygain_q16,16));
#endif
        for(uint32_t band=0;band<c->band_count;++band)
            for(uint32_t ch=0;ch<channels;++ch)filter_block(dsp,band,ch,samples+ch,count,channels);
        if(c->crossfeed_amount_q16){
#if defined(__arm__)
            if(channels==2)ln_dsp_crossfeed_stereo_arm(samples,count,dsp->crossfeed_low,
                c->crossfeed_coefficient_q16,c->crossfeed_amount_q16);
            else
#endif
            {
            int32_t low0=dsp->crossfeed_low[0],low1=dsp->crossfeed_low[1];
            int32_t coefficient=(int32_t)c->crossfeed_coefficient_q16;
            int32_t amount=(int32_t)c->crossfeed_amount_q16,own=65536-amount;
            for(uint32_t i=0;i<count;++i){
                int32_t left=samples[i*channels],right=channels==2?samples[i*2+1]:left;
                low0+=(int32_t)rounded((int64_t)(left-low0)*coefficient,16);
                if(channels==2)low1+=(int32_t)rounded((int64_t)(right-low1)*coefficient,16);
                else low1=low0;
                samples[i*channels]=(int32_t)rounded((int64_t)left*own+(int64_t)low1*amount,16);
                if(channels==2)samples[i*2+1]=(int32_t)rounded((int64_t)right*own+(int64_t)low0*amount,16);
            }
            dsp->crossfeed_low[0]=low0;dsp->crossfeed_low[1]=low1;
            }
        }
        for(uint32_t i=0;i<count;++i){
            int32_t left=samples[i*channels],right=channels==2?samples[i*2+1]:left;
            if(c->balance>0)left=(int32_t)rounded((int64_t)left*(int32_t)left_scale,16);
            if(c->balance<0)right=(int32_t)rounded((int64_t)right*(int32_t)right_scale,16);
            pcm[i*2]=output(dsp,left);pcm[i*2+1]=output(dsp,right);
        }
        pcm+=count*2;remaining-=count;
    }
    dsp->frames=frames>UINT32_MAX-dsp->frames?UINT32_MAX:dsp->frames+frames;
    if(channels==1){
        uint32_t duplicate_clips=dsp->internal_clips-clips_before;
        dsp->internal_clips=duplicate_clips>UINT32_MAX-dsp->internal_clips?
            UINT32_MAX:dsp->internal_clips+duplicate_clips;
        for(uint32_t band=0;band<c->band_count;++band)dsp->history[band][1]=dsp->history[band][0];
    }
}
LN_DSP_ICODE void ln_dsp_process(struct ln_dsp *dsp,int16_t *pcm,uint32_t frames)
{
    if(!dsp||!pcm||frames>UINT32_MAX/2u)return;
    const struct ln_dsp_config *c=&dsp->config;
    if(c->gain_q16==LINNET_DSP_UNITY&&c->replaygain_q16==LINNET_DSP_UNITY&&
       c->balance==0&&c->band_count==0&&c->crossfeed_amount_q16==0){
        if(c->mode!=LINNET_DSP_STEREO){
            for(uint32_t i=0;i<frames;++i){
                int32_t left=(int32_t)pcm[i*2]*256,right=(int32_t)pcm[i*2+1]*256,value;
                switch(c->mode){
                case LINNET_DSP_MONO:value=(left+right)/2;break;
                case LINNET_DSP_LEFT:value=left;break;
                case LINNET_DSP_RIGHT:value=right;break;
                default:value=(left-right)/2;break;
                }
                pcm[i*2]=output(dsp,value);pcm[i*2+1]=output(dsp,value);
            }
        }
        dsp->frames=frames>UINT32_MAX-dsp->frames?UINT32_MAX:dsp->frames+frames;return;
    }
    if(c->mode==LINNET_DSP_STEREO&&c->gain_q16<LINNET_DSP_UNITY&&
       c->replaygain_q16==LINNET_DSP_UNITY&&c->balance==0&&
       c->band_count==0&&c->crossfeed_amount_q16==0){
        /* Attenuation cannot clip. Keep the two Q8 rounding steps of the
         * general path; the signed product fits int32 below unity. */
        for(uint32_t i=0;i<frames*2u;++i){
            int32_t product=(int32_t)pcm[i]*(int32_t)c->gain_q16;
            int32_t q8=(product+128-(int32_t)(product<0))>>8;
            pcm[i]=(int16_t)((q8+128-(int32_t)(q8<0))>>8);
        }
        dsp->frames=frames>UINT32_MAX-dsp->frames?UINT32_MAX:dsp->frames+frames;return;
    }
    if(c->mode==LINNET_DSP_STEREO)process_channels(dsp,pcm,frames,2);
    else process_channels(dsp,pcm,frames,1);
}
