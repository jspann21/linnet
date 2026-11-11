#include "services/audio/flac/flac.h"
#include "services/audio/flac/output.h"
static uint32_t be(const uint8_t*p,uint32_t n){uint32_t v=0;while(n--)v=(v<<8)|*p++;return v;}
static uint64_t be64(const uint8_t*p){return ((uint64_t)be(p,4)<<32)|be(p+4,4);}
static int fail(struct ln_flac*s,int e){return s->error=e;}
static int read_bytes(struct ln_flac*s,uint64_t offset,uint8_t*out,uint32_t n)
{
    if(offset>s->file_size||n>s->file_size-offset)return fail(s,LN_PCM_FORMAT);
    int result=s->read(s->context,offset,out,n);s->pending=result==0;
    return result<0?fail(s,LN_PCM_IO):result;
}
void ln_flac_init(struct ln_flac*s,ln_pcm_read_fn read,void*context,uint64_t bytes)
{
    if(!s)return;*s=(struct ln_flac){0};s->read=read;s->context=context;s->file_size=bytes;
    s->decoder.decoded[0]=s->samples[0];s->decoder.decoded[1]=s->samples[1];
}
static int finish_metadata(struct ln_flac*s)
{
    if(s->last_metadata){if(!s->sample_rate)return fail(s,LN_PCM_FORMAT);s->audio_offset=s->cursor;s->phase=4;return LN_PCM_READY;}
    s->phase=1;return LN_PCM_PENDING;
}
int ln_flac_open_step(struct ln_flac*s)
{
    if(!s||!s->read)return LN_PCM_ARGUMENT;if(s->error)return s->error;
    if(s->phase==4)return LN_PCM_READY;int result;
    if(s->phase==0){result=read_bytes(s,0,s->header,4);if(result!=1)return result;
        if(be(s->header,4)!=0x664c6143u)return fail(s,LN_PCM_FORMAT);s->cursor=4;s->phase=1;return 0;}
    if(s->phase==1){result=read_bytes(s,s->cursor,s->header,4);if(result!=1)return result;
        s->last_metadata=(s->header[0]&128u)!=0;s->metadata_type=s->header[0]&127u;s->metadata_left=be(s->header+1,3);s->cursor+=4;
        if(s->metadata_left>s->file_size-s->cursor)return fail(s,LN_PCM_FORMAT);
        if(!s->sample_rate&&s->metadata_type!=0)return fail(s,LN_PCM_FORMAT);
        if(s->metadata_type==0){if(s->sample_rate||s->metadata_left!=34)return fail(s,LN_PCM_FORMAT);s->phase=2;}
        else if(s->metadata_type==3){if(s->metadata_left%18u)return fail(s,LN_PCM_FORMAT);s->phase=3;}
        else{s->cursor+=s->metadata_left;return finish_metadata(s);}return 0;}
    if(s->phase==2){result=read_bytes(s,s->cursor,s->header,34);if(result!=1)return result;const uint8_t*p=s->header;
        s->decoder.min_blocksize=(int)be(p,2);s->decoder.max_blocksize=(int)be(p+2,2);
        s->decoder.min_framesize=(int)be(p+4,3);s->decoder.max_framesize=(int)be(p+7,3);
        s->sample_rate=be(p+10,3)>>4;s->channels=((p[12]>>1)&7u)+1u;s->bits=(((p[12]&1u)<<4)|(p[13]>>4))+1u;
        s->total_frames=((uint64_t)(p[13]&15u)<<32)|be(p+14,4);
        if(!s->sample_rate||s->channels>2||s->bits<8||s->bits>24||
           s->decoder.min_blocksize<16||s->decoder.max_blocksize<s->decoder.min_blocksize||
           s->decoder.max_blocksize>MAX_BLOCKSIZE||s->decoder.max_framesize>MAX_FRAMESIZE)return fail(s,LN_PCM_UNSUPPORTED);
        s->decoder.samplerate=(int)s->sample_rate;s->decoder.channels=(int)s->channels;s->decoder.bps=(int)s->bits;
        s->cursor+=34;return finish_metadata(s);}
    if(s->phase==3){
        if(!s->metadata_left||s->seek_count==LINNET_FLAC_SEEK_POINTS){s->cursor+=s->metadata_left;s->metadata_left=0;return finish_metadata(s);}
        uint32_t points=s->metadata_left/18u;
        if(points>LINNET_FLAC_SEEK_POINTS-s->seek_count)points=LINNET_FLAC_SEEK_POINTS-s->seek_count;
        uint32_t bytes=points*18u;
        result=read_bytes(s,s->cursor,s->buffer,bytes);if(result!=1)return result;
        for(uint32_t i=0;i<points;i++){
            const uint8_t *p=s->buffer+i*18u;
            uint64_t sample=be64(p),offset=be64(p+8);
            if(sample==UINT64_MAX)continue;
            if((s->total_frames&&sample>=s->total_frames)||
               (s->seek_count&&(sample<=s->seek[s->seek_count-1u].sample||offset<s->seek[s->seek_count-1u].offset)))return fail(s,LN_PCM_FORMAT);
            s->seek[s->seek_count++]=(struct ln_flac_point){sample,offset};
        }
        s->cursor+=bytes;s->metadata_left-=bytes;return 0;
    }return fail(s,LN_PCM_FORMAT);
}
static void yield_decoder(void){}
static uint16_t crc16(const uint8_t*p,uint32_t n)
{
    static const uint16_t table[256]={
        0x0000u,0x8005u,0x800fu,0x000au,0x801bu,0x001eu,0x0014u,0x8011u,
        0x8033u,0x0036u,0x003cu,0x8039u,0x0028u,0x802du,0x8027u,0x0022u,
        0x8063u,0x0066u,0x006cu,0x8069u,0x0078u,0x807du,0x8077u,0x0072u,
        0x0050u,0x8055u,0x805fu,0x005au,0x804bu,0x004eu,0x0044u,0x8041u,
        0x80c3u,0x00c6u,0x00ccu,0x80c9u,0x00d8u,0x80ddu,0x80d7u,0x00d2u,
        0x00f0u,0x80f5u,0x80ffu,0x00fau,0x80ebu,0x00eeu,0x00e4u,0x80e1u,
        0x00a0u,0x80a5u,0x80afu,0x00aau,0x80bbu,0x00beu,0x00b4u,0x80b1u,
        0x8093u,0x0096u,0x009cu,0x8099u,0x0088u,0x808du,0x8087u,0x0082u,
        0x8183u,0x0186u,0x018cu,0x8189u,0x0198u,0x819du,0x8197u,0x0192u,
        0x01b0u,0x81b5u,0x81bfu,0x01bau,0x81abu,0x01aeu,0x01a4u,0x81a1u,
        0x01e0u,0x81e5u,0x81efu,0x01eau,0x81fbu,0x01feu,0x01f4u,0x81f1u,
        0x81d3u,0x01d6u,0x01dcu,0x81d9u,0x01c8u,0x81cdu,0x81c7u,0x01c2u,
        0x0140u,0x8145u,0x814fu,0x014au,0x815bu,0x015eu,0x0154u,0x8151u,
        0x8173u,0x0176u,0x017cu,0x8179u,0x0168u,0x816du,0x8167u,0x0162u,
        0x8123u,0x0126u,0x012cu,0x8129u,0x0138u,0x813du,0x8137u,0x0132u,
        0x0110u,0x8115u,0x811fu,0x011au,0x810bu,0x010eu,0x0104u,0x8101u,
        0x8303u,0x0306u,0x030cu,0x8309u,0x0318u,0x831du,0x8317u,0x0312u,
        0x0330u,0x8335u,0x833fu,0x033au,0x832bu,0x032eu,0x0324u,0x8321u,
        0x0360u,0x8365u,0x836fu,0x036au,0x837bu,0x037eu,0x0374u,0x8371u,
        0x8353u,0x0356u,0x035cu,0x8359u,0x0348u,0x834du,0x8347u,0x0342u,
        0x03c0u,0x83c5u,0x83cfu,0x03cau,0x83dbu,0x03deu,0x03d4u,0x83d1u,
        0x83f3u,0x03f6u,0x03fcu,0x83f9u,0x03e8u,0x83edu,0x83e7u,0x03e2u,
        0x83a3u,0x03a6u,0x03acu,0x83a9u,0x03b8u,0x83bdu,0x83b7u,0x03b2u,
        0x0390u,0x8395u,0x839fu,0x039au,0x838bu,0x038eu,0x0384u,0x8381u,
        0x0280u,0x8285u,0x828fu,0x028au,0x829bu,0x029eu,0x0294u,0x8291u,
        0x82b3u,0x02b6u,0x02bcu,0x82b9u,0x02a8u,0x82adu,0x82a7u,0x02a2u,
        0x82e3u,0x02e6u,0x02ecu,0x82e9u,0x02f8u,0x82fdu,0x82f7u,0x02f2u,
        0x02d0u,0x82d5u,0x82dfu,0x02dau,0x82cbu,0x02ceu,0x02c4u,0x82c1u,
        0x8243u,0x0246u,0x024cu,0x8249u,0x0258u,0x825du,0x8257u,0x0252u,
        0x0270u,0x8275u,0x827fu,0x027au,0x826bu,0x026eu,0x0264u,0x8261u,
        0x0220u,0x8225u,0x822fu,0x022au,0x823bu,0x023eu,0x0234u,0x8231u,
        0x8213u,0x0216u,0x021cu,0x8219u,0x0208u,0x820du,0x8207u,0x0202u
    };
    uint32_t value=0;
    for(uint32_t i=0;i<n;++i)
        value=((value<<8)^table[(value>>8)^p[i]])&0xffffu;
    return (uint16_t)value;
}

int ln_flac_decode(struct ln_flac*s,int16_t*out,uint32_t capacity,uint32_t*frames)
{
    if(!s||!out||!frames||!capacity)return LN_PCM_ARGUMENT;*frames=0;
    if(s->error)return s->error;if(s->phase!=4)return LN_PCM_PENDING;
    if(s->decoded_cursor<s->decoded_frames){
        uint32_t n=s->decoded_frames-s->decoded_cursor;if(n>capacity)n=capacity;
        ln_flac_output(out,s->samples[0]+s->decoded_cursor,s->samples[1]+s->decoded_cursor,
                       n,s->channels,s->bits,(unsigned)s->decoder.ch_mode);
        s->decoded_cursor+=n;s->frame+=n;*frames=n;return LN_PCM_READY;
    }
    if(s->total_frames&&s->frame>=s->total_frames)return LN_PCM_EOF;
    if(!s->have&&s->cursor==s->file_size){
        /* A valid final frame CRC does not prove a complete stream. Require
         * the declared sample count and any admitted seek target at EOF. */
        if(s->total_frames&&s->frame<s->total_frames)return fail(s,LN_PCM_FORMAT);
        if(s->frame<s->discard_to)return fail(s,LN_PCM_ARGUMENT);
        return LN_PCM_EOF;
    }
    uint32_t target=s->decoder.max_framesize?(uint32_t)s->decoder.max_framesize:MAX_FRAMESIZE;
    uint64_t available=s->file_size-s->cursor;if(available<target)target=(uint32_t)available;
    if(s->have<target){uint32_t n=target-s->have;if(n>4096)n=4096;
        int result=read_bytes(s,s->cursor+s->have,s->buffer+s->have,n);if(result!=1)return result;s->have+=n;return 0;}
    if(s->have<MIN_FRAME_SIZE)return fail(s,LN_PCM_FORMAT);
    int result=flac_decode_frame(&s->decoder,s->buffer,(int)s->have,yield_decoder);
    uint32_t used=(uint32_t)s->decoder.framesize;
    if(result<0||s->decoder.gb.error||!used||used>s->have||crc16(s->buffer,used))return fail(s,LN_PCM_FORMAT);
    if(s->decoder.blocksize<1||s->decoder.blocksize>MAX_BLOCKSIZE)return fail(s,LN_PCM_FORMAT);
    s->decoded_frames=(uint32_t)s->decoder.blocksize;s->decoded_cursor=0;
    if(s->total_frames&&s->decoded_frames>s->total_frames-s->frame)return fail(s,LN_PCM_FORMAT);
    s->cursor+=used;s->have-=used;for(uint32_t i=0;i<s->have;++i)s->buffer[i]=s->buffer[i+used];
    if(s->discard_to>s->frame){uint64_t discard=s->discard_to-s->frame;uint32_t n=discard<s->decoded_frames?(uint32_t)discard:s->decoded_frames;
        s->decoded_cursor=n;s->frame+=n;}
    return LN_PCM_PENDING;
}
bool ln_flac_seek(struct ln_flac*s,uint64_t frame)
{
    if(!s||s->phase!=4||s->pending||s->error||(s->total_frames&&frame>s->total_frames))return false;
    uint64_t sample=0,offset=0;
    for(uint32_t i=0;i<s->seek_count&&s->seek[i].sample<=frame;++i){sample=s->seek[i].sample;offset=s->seek[i].offset;}
    if(offset>s->file_size-s->audio_offset)return false;
    if(s->frame>=sample&&s->frame<=frame){
        uint64_t skip=frame-s->frame;
        uint32_t buffered=s->decoded_frames-s->decoded_cursor;
        if(skip>buffered)skip=buffered;
        s->decoded_cursor+=(uint32_t)skip;s->frame+=skip;
        s->discard_to=frame;
        return true;
    }
    s->cursor=s->audio_offset+offset;s->frame=sample;s->discard_to=frame;s->have=0;s->decoded_frames=s->decoded_cursor=0;return true;
}
