#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "profile.h"
struct preset_rate {uint32_t rate,crossfeed;};
static const struct preset_rate presets[]={
{8000,27717},{11025,21559},{12000,20110},{16000,15751},{22050,11851},{24000,10974},{32000,8416},{44100,6221},{48000,5738},{88200,3188},{96000,2935}
};
/* Q1.30 sin(w)/(2Q), cos(w) for 100/250/1k/4k/8k Hz, Q=1. */
static const int32_t custom_poles[11][5][2]={
{{42122406,1070431836},{104738319,1053110176},{379625062,759250125},{0,0},{0,0}},
{{30579898,1071998593},{76232619,1062862106},{289669100,904040240},{407556002,-698937712},{0,0}},
{{28097653,1072270298},{70075716,1064555814},{268435456,929887697},{464943848,-536870912},{0,0}},
{{21077453,1072914008},{52622552,1068571464},{205451603,992008094},{536870912,0},{0,0}},
{{15296158,1073305928},{38213232,1071018441},{150920380,1030443429},{487775583,448577112},{407556002,-698937712}},
{{14053642,1073373879},{35113038,1071442860},{138952417,1037154959},{464943848,536870912},{464943848,-536870912}},
{{10540758,1073534850},{26343007,1072448455},{104738319,1053110176},{379625062,759250125},{536870912,0}},
{{7648856,1073632844},{19118743,1073060762},{76232619,1062862106},{289669100,904040240},{487775583,448577112}},
{{7027423,1073649834},{17565924,1073166929},{70075716,1064555814},{268435456,929887697},{464943848,536870912}},
{{3824525,1073714579},{9560887,1073571545},{38213232,1071018441},{150920380,1030443429},{289669100,904040240}},
{{3513787,1073718826},{8784138,1073598091},{35113038,1071442860},{138952417,1037154959},{268435456,929887697}}
};
static const int8_t preset_gains[11][5]={
    {3,2,0,2,3},{6,4,0,0,0},{3,2,-1,2,3},{4,3,0,3,4},
    {5,3,0,2,3},{5,3,0,3,5},{-2,1,3,1,-2},{-4,-2,2,4,5},
    {-3,-1,4,3,-2},{0,0,0,4,6},{-2,0,4,3,0}
};
/* Rounded-up peak responses of these original Q=1 filter cascades across all
 * native rates. Adjacent bands overlap: the largest slider alone is not enough
 * headroom (Bass Booster reaches 6.78 dB; all +6 reaches 8.06 dB).
 * Custom bounds use all five bands at the largest positive gain, which bounds
 * any smaller boosts/cuts. Also cap attenuation at the sum of active boosts. */
static const uint8_t preset_headroom[]={4,7,4,5,6,6,4,7,5,8,5};
static const uint8_t custom_headroom[]={0,2,3,5,6,7,9};
static __attribute__((minsize)) int32_t ratio(int64_t numerator,uint32_t denominator){
    bool negative=numerator<0;uint64_t value=(uint64_t)(negative?-numerator:numerator)+denominator/2u;
    uint64_t divisor=(uint64_t)denominator<<31;uint32_t result=0;
    for(uint32_t bit=0x80000000u;bit;bit>>=1,divisor>>=1)if(value>=divisor){value-=divisor;result|=bit;}
    return negative?-(int32_t)result:(int32_t)result;
}
static __attribute__((minsize)) struct ln_dsp_biquad custom_band(uint32_t rate_index,uint32_t band,int32_t gain){
    static const uint32_t amplitudes[]={46396,49145,52057,55142,58409,61870,65536,69419,73533,77890,82505,87394,92572};
    int32_t alpha=custom_poles[rate_index][band][0],cosine=custom_poles[rate_index][band][1];
    uint32_t amplitude=amplitudes[gain+6];
    int32_t cut=ratio((int64_t)alpha*65536,amplitude);
    int32_t boost=(int32_t)(((int64_t)alpha*amplitude+32768)>>16);
    uint32_t a0=1073741824u+(uint32_t)cut;
    int32_t b0=ratio((1073741824LL+boost)*1073741824LL,a0);
    int32_t b1=ratio(-2LL*cosine*1073741824LL,a0);
    int32_t a2=ratio((1073741824LL-cut)*1073741824LL,a0);
    int32_t b2=(int32_t)(1073741824LL+a2-b0);
    return (struct ln_dsp_biquad){b0,b1,b2,b1,a2};
}
void ln_music_profile_default(struct ln_music_profile *p){if(p)*p=(struct ln_music_profile){0};}
__attribute__((minsize)) bool ln_music_profile_valid(const struct ln_music_profile *p){
    if(!p)return false;
    for(uint32_t i=0;i<5;i++)if(p->eq_db[i]<-6||p->eq_db[i]>6)return false;
    return p->replaygain<=2&&p->max_volume_db_tenths>=-730&&p->max_volume_db_tenths<=0&&p->max_volume_db_tenths%10==0&&p->preset<=LN_MUSIC_EQ_CUSTOM&&p->mode<=LINNET_DSP_KARAOKE&&
        p->balance>=-1000&&p->balance<=1000&&p->preamp_db_q16>=-24*65536&&p->preamp_db_q16<=0;
}
__attribute__((minsize)) bool ln_music_profile_configure(const struct ln_music_profile *p,uint32_t rate,
                               int32_t replaygain_db_q16,struct ln_dsp_config *out){
    if(!out||!ln_music_profile_valid(p))return false;
    const struct preset_rate *entry=0;
    for(uint32_t i=0;i<sizeof(presets)/sizeof(presets[0]);i++)if(presets[i].rate==rate){entry=&presets[i];break;}
    if(!entry)return false;
    struct ln_dsp dsp;ln_dsp_init(&dsp);struct ln_dsp_config c=dsp.config;
    static const uint32_t frequencies[]={100,250,1000,4000,8000};
    int32_t headroom=0,boost_sum=0;
    if(p->preset){
        const int8_t *gains=p->preset==LN_MUSIC_EQ_CUSTOM?p->eq_db:preset_gains[p->preset-1u];
        for(uint32_t i=0;i<5;i++)if(gains[i]&&frequencies[i]<rate/2){
            c.bands[c.band_count++]=custom_band((uint32_t)(entry-presets),i,gains[i]);
            if(gains[i]>headroom)headroom=gains[i];
            if(gains[i]>0)boost_sum+=gains[i];
        }
        headroom=p->preset==LN_MUSIC_EQ_CUSTOM?custom_headroom[headroom]:preset_headroom[p->preset-1u];
        if(headroom>boost_sum)headroom=boost_sum;
    }
    c.gain_q16=ln_dsp_gain_from_db(p->preamp_db_q16-headroom*65536);
    c.headroom_db_tenths=(uint32_t)headroom*10u;
    uint32_t replaygain=p->replaygain?ln_dsp_gain_from_db(replaygain_db_q16):LINNET_DSP_UNITY;
    c.gain_q16=(uint32_t)(((uint64_t)c.gain_q16*replaygain+32768u)>>16);
    if(!c.gain_q16)c.gain_q16=1;
    c.mode=p->mode;c.balance=p->balance;
    if(p->crossfeed){c.crossfeed_amount_q16=16384;c.crossfeed_coefficient_q16=entry->crossfeed;}
    if(!ln_dsp_configure(&dsp,&c))return false;*out=c;return true;
}
__attribute__((minsize)) bool ln_music_profile_encode(const struct ln_music_profile *p,uint8_t out[LN_MUSIC_PROFILE_BYTES]){
    if(!out||!ln_music_profile_valid(p))return false;
    uint32_t flags=(p->replaygain==1?1u:p->replaygain==2?4u:0u)|(p->crossfeed?2u:0u);
    for(uint32_t i=0;i<5;i++)flags|=(uint32_t)(p->eq_db[i]+6)<<(3+i*4);
    flags|=(uint32_t)(-p->max_volume_db_tenths/10)<<23;
    if(p->lineout)flags|=1u<<30;
    ln_write_le32(out,0x464f5250u);ln_write_le32(out+4,0x00200002u);ln_write_le32(out+8,flags);
    ln_write_le32(out+12,p->preset);ln_write_le32(out+16,p->mode);ln_write_le32(out+20,(uint32_t)p->balance);ln_write_le32(out+24,(uint32_t)p->preamp_db_q16);ln_write_le32(out+28,ln_crc32(out,28));return true;
}
static int32_t signed32(uint32_t x){return x<=INT32_MAX?(int32_t)x:-1-(int32_t)(~x);}
__attribute__((minsize)) bool ln_music_profile_decode(struct ln_music_profile *p,const uint8_t *data,uint32_t bytes){
    if(!p||!data||bytes!=32||ln_read_le32(data)!=0x464f5250u||
       ln_read_le32(data+4)!=0x00200002u||ln_read_le32(data+28)!=ln_crc32(data,28))return false;
    uint32_t flags=ln_read_le32(data+8);
    if((flags&0x80000000u)||(flags&5u)==5u)return false;
    struct ln_music_profile candidate={.preset=ln_read_le32(data+12),.mode=ln_read_le32(data+16),
        .balance=signed32(ln_read_le32(data+20)),.preamp_db_q16=signed32(ln_read_le32(data+24)),
        .replaygain=(flags&4u)?2u:flags&1u,.crossfeed=(flags&2u)!=0};
    for(uint32_t i=0;i<5;i++)candidate.eq_db[i]=(int8_t)((flags>>(3+i*4))&15u)-6;
    candidate.max_volume_db_tenths=-(int32_t)((flags>>23)&127u)*10;
    candidate.lineout=(flags&(1u<<30))!=0;
    if(!ln_music_profile_valid(&candidate))return false;*p=candidate;return true;
}
