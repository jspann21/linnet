#include "services/audio/vorbis/vorbis.h"
#include "third_party/vorbis/codec_internal.h"
void ln_vorbis_pool_bind(struct ln_vorbis*);
void ln_vorbis_pool_reset(struct ln_vorbis*);
void ln_vorbis_scratch_clear(struct ln_vorbis*);
#if defined(__arm__)
int ln_vorbis_guard(uintptr_t*,int(*)(struct ln_vorbis*),struct ln_vorbis*);
#endif
static int guarded(struct ln_vorbis*s,int(*fn)(struct ln_vorbis*))
{
    ln_vorbis_pool_bind(s);int result;
#if defined(__arm__)
    result=ln_vorbis_guard(s->jump,fn,s);
#else
    result=setjmp(s->jump)?LN_PCM_UNSUPPORTED:fn(s);
#endif
    if(result==LN_PCM_UNSUPPORTED&&s->error){ln_vorbis_pool_reset(s);return result;}
    ln_vorbis_scratch_clear(s);return result;
}
static uint32_t le(const uint8_t*p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int fail(struct ln_vorbis*s,int e){return s->error=e;}
static uint32_t checksum(const uint8_t*p,uint32_t n)
{
    /* Ogg's MSB-first CRC over page bytes; the stored checksum is zeroed. */
    static const uint32_t table[256]={
        0x00000000u,0x04c11db7u,0x09823b6eu,0x0d4326d9u,
        0x130476dcu,0x17c56b6bu,0x1a864db2u,0x1e475005u,
        0x2608edb8u,0x22c9f00fu,0x2f8ad6d6u,0x2b4bcb61u,
        0x350c9b64u,0x31cd86d3u,0x3c8ea00au,0x384fbdbdu,
        0x4c11db70u,0x48d0c6c7u,0x4593e01eu,0x4152fda9u,
        0x5f15adacu,0x5bd4b01bu,0x569796c2u,0x52568b75u,
        0x6a1936c8u,0x6ed82b7fu,0x639b0da6u,0x675a1011u,
        0x791d4014u,0x7ddc5da3u,0x709f7b7au,0x745e66cdu,
        0x9823b6e0u,0x9ce2ab57u,0x91a18d8eu,0x95609039u,
        0x8b27c03cu,0x8fe6dd8bu,0x82a5fb52u,0x8664e6e5u,
        0xbe2b5b58u,0xbaea46efu,0xb7a96036u,0xb3687d81u,
        0xad2f2d84u,0xa9ee3033u,0xa4ad16eau,0xa06c0b5du,
        0xd4326d90u,0xd0f37027u,0xddb056feu,0xd9714b49u,
        0xc7361b4cu,0xc3f706fbu,0xceb42022u,0xca753d95u,
        0xf23a8028u,0xf6fb9d9fu,0xfbb8bb46u,0xff79a6f1u,
        0xe13ef6f4u,0xe5ffeb43u,0xe8bccd9au,0xec7dd02du,
        0x34867077u,0x30476dc0u,0x3d044b19u,0x39c556aeu,
        0x278206abu,0x23431b1cu,0x2e003dc5u,0x2ac12072u,
        0x128e9dcfu,0x164f8078u,0x1b0ca6a1u,0x1fcdbb16u,
        0x018aeb13u,0x054bf6a4u,0x0808d07du,0x0cc9cdcau,
        0x7897ab07u,0x7c56b6b0u,0x71159069u,0x75d48ddeu,
        0x6b93dddbu,0x6f52c06cu,0x6211e6b5u,0x66d0fb02u,
        0x5e9f46bfu,0x5a5e5b08u,0x571d7dd1u,0x53dc6066u,
        0x4d9b3063u,0x495a2dd4u,0x44190b0du,0x40d816bau,
        0xaca5c697u,0xa864db20u,0xa527fdf9u,0xa1e6e04eu,
        0xbfa1b04bu,0xbb60adfcu,0xb6238b25u,0xb2e29692u,
        0x8aad2b2fu,0x8e6c3698u,0x832f1041u,0x87ee0df6u,
        0x99a95df3u,0x9d684044u,0x902b669du,0x94ea7b2au,
        0xe0b41de7u,0xe4750050u,0xe9362689u,0xedf73b3eu,
        0xf3b06b3bu,0xf771768cu,0xfa325055u,0xfef34de2u,
        0xc6bcf05fu,0xc27dede8u,0xcf3ecb31u,0xcbffd686u,
        0xd5b88683u,0xd1799b34u,0xdc3abdedu,0xd8fba05au,
        0x690ce0eeu,0x6dcdfd59u,0x608edb80u,0x644fc637u,
        0x7a089632u,0x7ec98b85u,0x738aad5cu,0x774bb0ebu,
        0x4f040d56u,0x4bc510e1u,0x46863638u,0x42472b8fu,
        0x5c007b8au,0x58c1663du,0x558240e4u,0x51435d53u,
        0x251d3b9eu,0x21dc2629u,0x2c9f00f0u,0x285e1d47u,
        0x36194d42u,0x32d850f5u,0x3f9b762cu,0x3b5a6b9bu,
        0x0315d626u,0x07d4cb91u,0x0a97ed48u,0x0e56f0ffu,
        0x1011a0fau,0x14d0bd4du,0x19939b94u,0x1d528623u,
        0xf12f560eu,0xf5ee4bb9u,0xf8ad6d60u,0xfc6c70d7u,
        0xe22b20d2u,0xe6ea3d65u,0xeba91bbcu,0xef68060bu,
        0xd727bbb6u,0xd3e6a601u,0xdea580d8u,0xda649d6fu,
        0xc423cd6au,0xc0e2d0ddu,0xcda1f604u,0xc960ebb3u,
        0xbd3e8d7eu,0xb9ff90c9u,0xb4bcb610u,0xb07daba7u,
        0xae3afba2u,0xaafbe615u,0xa7b8c0ccu,0xa379dd7bu,
        0x9b3660c6u,0x9ff77d71u,0x92b45ba8u,0x9675461fu,
        0x8832161au,0x8cf30badu,0x81b02d74u,0x857130c3u,
        0x5d8a9099u,0x594b8d2eu,0x5408abf7u,0x50c9b640u,
        0x4e8ee645u,0x4a4ffbf2u,0x470cdd2bu,0x43cdc09cu,
        0x7b827d21u,0x7f436096u,0x7200464fu,0x76c15bf8u,
        0x68860bfdu,0x6c47164au,0x61043093u,0x65c52d24u,
        0x119b4be9u,0x155a565eu,0x18197087u,0x1cd86d30u,
        0x029f3d35u,0x065e2082u,0x0b1d065bu,0x0fdc1becu,
        0x3793a651u,0x3352bbe6u,0x3e119d3fu,0x3ad08088u,
        0x2497d08du,0x2056cd3au,0x2d15ebe3u,0x29d4f654u,
        0xc5a92679u,0xc1683bceu,0xcc2b1d17u,0xc8ea00a0u,
        0xd6ad50a5u,0xd26c4d12u,0xdf2f6bcbu,0xdbee767cu,
        0xe3a1cbc1u,0xe760d676u,0xea23f0afu,0xeee2ed18u,
        0xf0a5bd1du,0xf464a0aau,0xf9278673u,0xfde69bc4u,
        0x89b8fd09u,0x8d79e0beu,0x803ac667u,0x84fbdbd0u,
        0x9abc8bd5u,0x9e7d9662u,0x933eb0bbu,0x97ffad0cu,
        0xafb010b1u,0xab710d06u,0xa6322bdfu,0xa2f33668u,
        0xbcb4666du,0xb8757bdau,0xb5365d03u,0xb1f740b4u
    };
    uint32_t v=0;
    for(uint32_t i=0;i<n;++i){
        uint32_t b=i>=22u&&i<26u?0u:p[i];
        v=(v<<8)^table[((v>>24)^b)&255u];
    }
    return v;
}
static int packet_step(struct ln_vorbis*s)
{
    if(s->page_ready){
        while(s->segment<s->segments){uint32_t n=s->page[27u+s->segment++];
            if(n>sizeof(s->packet)-s->packet_size)return fail(s,LN_PCM_UNSUPPORTED);
            for(uint32_t i=0;i<n;++i)s->packet[s->packet_size+i]=s->page[s->body+i];s->body+=n;s->packet_size+=n;
            if(n<255u){bool last=true;for(uint32_t i=s->segment;i<s->segments;++i)if(s->page[27u+i]<255u)last=false;
                s->op=(ogg_packet){s->packet,(long)s->packet_size,s->packets==0,last&&s->page_eos,
                    last?(int64_t)s->page_granule:-1,(int64_t)s->packets++};
                s->packet_size=0;return 1;}
        }
        s->page_ready=false;s->page_have=0;s->page_need=27;
        if(s->page_eos){if(s->packet_size||s->cursor!=s->file_size)return fail(s,LN_PCM_UNSUPPORTED);
            /* Granules may include a nonzero origin. Publish the zero-based
             * total only after every EOS packet and its PCM have drained. */
            s->total_frames=s->frame;s->ended=true;return LN_PCM_EOF;}
    }
    if(s->cursor==s->file_size)return fail(s,LN_PCM_FORMAT);
    if(s->page_need>s->file_size-s->cursor+s->page_have)return fail(s,LN_PCM_FORMAT);
    uint32_t n=s->page_need-s->page_have;if(n>4096u)n=4096u;
    if(n){int result=s->read(s->context,s->cursor,s->page+s->page_have,n);s->pending=result==0;
        if(result<0)return fail(s,LN_PCM_IO);if(!result)return 0;s->page_have+=n;s->cursor+=n;if(s->page_have<s->page_need)return 0;}
    if(s->page_need==27u){
        if(le(s->page)!=0x5367674fu||s->page[4]||(s->page[5]&~7u))return fail(s,LN_PCM_FORMAT);
        uint32_t serial=le(s->page+14),sequence=le(s->page+18);
        if(!s->packets){if(!(s->page[5]&2u)||sequence)return fail(s,LN_PCM_FORMAT);s->serial=serial;}
        else if(serial!=s->serial||(s->page[5]&2u)||sequence!=s->sequence)return fail(s,LN_PCM_UNSUPPORTED);
        s->sequence=sequence+1u;s->segments=s->page[26];
        if(!s->segments)return fail(s,LN_PCM_UNSUPPORTED);
        if(((s->page[5]&1u)!=0)!=(s->packet_size!=0))return fail(s,LN_PCM_FORMAT);
        s->page_need=27u+s->segments;return 0;
    }
    if(s->page_need==27u+s->segments){uint32_t bytes=0;for(uint32_t i=0;i<s->segments;++i)bytes+=s->page[27u+i];
        if(!bytes)return fail(s,LN_PCM_FORMAT);s->page_need+=bytes;return 0;}
    if(checksum(s->page,s->page_need)!=le(s->page+22))return fail(s,LN_PCM_FORMAT);
    s->page_granule=le(s->page+6)|((uint64_t)le(s->page+10)<<32);s->page_eos=(s->page[5]&4u)!=0;
    if(s->page_granule!=UINT64_MAX&&s->page_granule>INT64_MAX)return fail(s,LN_PCM_FORMAT);
    if(s->page_eos&&s->page_granule==UINT64_MAX)return fail(s,LN_PCM_FORMAT);
    s->page_ready=true;s->segment=0;s->body=27u+s->segments;return 0;
}
void ln_vorbis_init(struct ln_vorbis*s,ln_pcm_read_fn read,void*context,uint64_t bytes)
{if(!s)return;*s=(struct ln_vorbis){0};s->read=read;s->context=context;s->file_size=bytes;s->page_need=27;ln_vorbis_pool_reset(s);}
static int header(struct ln_vorbis*s)
{
    if(!s->headers)vorbis_info_init(&s->info);
    uint8_t expected=s->headers==0?1u:s->headers==1?3u:5u;
    if(s->op.bytes<7||s->packet[0]!=expected||vorbis_synthesis_headerin(&s->info,&s->op))return LN_PCM_FORMAT;
    if(s->info.channels<1||s->info.channels>2||s->info.rate<8000||s->info.rate>96000)return LN_PCM_UNSUPPORTED;
    codec_setup_info*c=s->info.codec_setup;if(c->blocksizes[1]>8192)return LN_PCM_UNSUPPORTED;
    ++s->headers;
    if(s->headers==3){if(vorbis_synthesis_init(&s->dsp,&s->info)||vorbis_block_init(&s->dsp,&s->block))return LN_PCM_FORMAT;
        s->sample_rate=(uint32_t)s->info.rate;s->channels=(uint32_t)s->info.channels;s->opened=true;}
    return 1;
}
int ln_vorbis_open_step(struct ln_vorbis*s)
{
    if(!s||!s->read)return LN_PCM_ARGUMENT;if(s->error)return s->error;if(s->opened)return 1;
    int result=packet_step(s);if(result!=1)return result==LN_PCM_EOF?fail(s,LN_PCM_FORMAT):result;
    result=guarded(s,header);if(result<0)return fail(s,result);return s->opened?1:0;
}
static int synthesize(struct ln_vorbis*s)
{return vorbis_synthesis(&s->block,&s->op)||vorbis_synthesis_blockin(&s->dsp,&s->block)?LN_PCM_FORMAT:1;}
int ln_vorbis_decode(struct ln_vorbis*s,int16_t*out,uint32_t capacity,uint32_t*frames)
{
    if(!s||!out||!frames||!capacity)return LN_PCM_ARGUMENT;*frames=0;if(s->error)return s->error;if(!s->opened){int r=ln_vorbis_open_step(s);return r<0?r:0;}
    int32_t**pcm;int available=vorbis_synthesis_pcmout(&s->dsp,&pcm);
    if(available>0){uint32_t n=(uint32_t)available;if(n>capacity)n=capacity;
        if(s->frame<s->discard_to){uint64_t remaining=s->discard_to-s->frame;if(remaining<n)n=(uint32_t)remaining;
            vorbis_synthesis_read(&s->dsp,(int)n);s->frame+=n;return 0;}
        for(uint32_t i=0;i<n;++i)for(uint32_t ch=0;ch<2;++ch){int32_t raw=pcm[s->channels==1?0:ch][i];
            int32_t value=raw>=0?raw/512:-(int32_t)(((uint32_t)(-(int64_t)raw)+511u)/512u);
            if(value>32767)value=32767;if(value<-32768)value=-32768;out[2u*i+ch]=(int16_t)value;}
        vorbis_synthesis_read(&s->dsp,(int)n);s->frame+=n;*frames=n;return 1;}
    if(s->ended)return s->frame<s->discard_to?fail(s,LN_PCM_FORMAT):LN_PCM_EOF;
    int result=packet_step(s);if(result!=1)return result==LN_PCM_EOF&&s->frame<s->discard_to?fail(s,LN_PCM_FORMAT):result;
    result=guarded(s,synthesize);return result<0?fail(s,result):0;
}
void ln_vorbis_close(struct ln_vorbis*s){if(!s)return;ln_vorbis_pool_reset(s);s->opened=false;s->ended=true;}
bool ln_vorbis_seek(struct ln_vorbis*s,uint64_t frame)
{if(!s||s->pending||(s->total_frames&&frame>s->total_frames))return false;
    if(s->opened&&!s->error&&s->frame>=s->discard_to&&frame>=s->frame){
        /* Retain page position, setup headers and overlap on forward seeks. */
        s->discard_to=frame;return true;
    }
    ln_pcm_read_fn read=s->read;void*context=s->context;uint64_t bytes=s->file_size;
    ln_vorbis_init(s,read,context,bytes);s->discard_to=frame;return true;}
