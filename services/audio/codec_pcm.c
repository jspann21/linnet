#include "codec_pcm.h"
static void copy_bytes(void *destination,const void *source,uint32_t bytes)
{
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);
    ln_memory_copy(destination,source,bytes);
#else
    uint8_t *to=destination;const uint8_t *from=source;
    for(uint32_t i=0;i<bytes;i++)to[i]=from[i];
#endif
}
static uint16_t u16(const uint8_t *p,bool be)
{ return be?(uint16_t)((p[0]<<8)|p[1]):(uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t u32(const uint8_t *p,bool be)
{ return be?((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]:
    p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static bool tag(const uint8_t *p,const char *s)
{ return p[0]==(uint8_t)s[0]&&p[1]==(uint8_t)s[1]&&p[2]==(uint8_t)s[2]&&p[3]==(uint8_t)s[3]; }
static int fail(struct ln_pcm *p,int error) { p->error=error;return error; }
/* Validated PCM frame sizes are 1,2,3,4,6,8 bytes. Division by three uses
 * an exact reciprocal multiply over the full uint32 range (ARM7 UMULL).
 * No target runtime division support or data-dependent loop is required. */
static uint32_t frame_count(uint32_t bytes,uint16_t align)
{
    switch(align){
    case 1:return bytes;
    case 2:return bytes>>1;
    case 3:return (uint32_t)(((uint64_t)bytes*UINT64_C(0xaaaaaaab))>>33);
    case 4:return bytes>>2;
    case 6:return (uint32_t)(((uint64_t)bytes*UINT64_C(0xaaaaaaab))>>34);
    case 8:return bytes>>3;
    default:return 0;
    }
}
static int read_at(struct ln_pcm *p,uint64_t offset,uint32_t length)
{
    if(offset>p->file_size||length>p->file_size-offset||length>sizeof(p->buffer))
        return fail(p,LN_PCM_FORMAT);
    if(p->pending){
        if(offset!=p->cache_request_offset||length!=p->cache_request_bytes)
            return fail(p,LN_PCM_ARGUMENT);
    }else{
        if(offset>=p->cache_offset&&offset-p->cache_offset<=p->cache_bytes&&
           length<=p->cache_bytes-(uint32_t)(offset-p->cache_offset)){
            uint32_t at=(uint32_t)(offset-p->cache_offset);
            copy_bytes(p->buffer,p->read_cache+at,length);
            return 1;
        }
        /* WAV/AIFF headers need not end on a sector boundary. Keep file
         * reads aligned so each PCM block does not incur partial-sector
         * reads and read-ahead before the contiguous middle of the window.
         * The 4 KiB decoder request plus alignment prefix fits the cache. */
        p->cache_offset=offset&~UINT64_C(4095);p->cache_bytes=0;
        p->cache_request_offset=offset;p->cache_request_bytes=length;
    }
    uint32_t fill=p->file_size-p->cache_offset>sizeof(p->read_cache)?
        sizeof(p->read_cache):(uint32_t)(p->file_size-p->cache_offset);
    int r=p->read(p->context,p->cache_offset,p->read_cache,fill);
    p->pending=r==0;
    if(r!=1)return r<0?fail(p,LN_PCM_IO):r;
    p->cache_bytes=fill;
    copy_bytes(p->buffer,p->read_cache+(uint32_t)(offset-p->cache_offset),length);
    return 1;
}
void ln_pcm_init(struct ln_pcm *p,ln_pcm_read_fn read,void *context,uint64_t size)
{ /* Cache bytes become readable only after a completed read. Initialize its
   * metadata without clearing tens of KiB on every track transition. */
  uint8_t *bytes=(uint8_t *)p;for(uint32_t i=0;i<__builtin_offsetof(struct ln_pcm,buffer);i++)bytes[i]=0;
  p->cache_offset=p->cache_request_offset=0;
  p->cache_bytes=p->cache_request_bytes=0;
  p->read=read;p->context=context;p->file_size=size; }
/* Integer-only decoding of positive integral IEEE extended sample rates. */
static uint32_t extended_rate(const uint8_t *b)
{
    uint16_t exponent=u16(b,true);
    if(exponent<16383u||exponent>16400u)return 0;
    uint64_t mantissa=((uint64_t)u32(b+2,true)<<32)|u32(b+6,true);
    if(!(mantissa&(UINT64_C(1)<<63)))return 0;
    uint32_t shift=63u-(exponent-16383u);
    uint64_t mask=(UINT64_C(1)<<shift)-1u;
    if(mantissa&mask)return 0;
    return (uint32_t)(mantissa>>shift);
}
int ln_pcm_open_step(struct ln_pcm *p)
{
    if(!p||!p->read)return LN_PCM_ARGUMENT;
    if(p->error)return p->error;
    if(p->phase==4)return LN_PCM_READY;
    int r;uint8_t *b=p->buffer;
    if(p->phase==0){
        r=read_at(p,0,12);if(r!=1)return r;
        if(tag(b,"RIFF")&&tag(b+8,"WAVE")){p->container=1;p->unsigned_8=true;}
        else if(tag(b,"FORM")&&tag(b+8,"AIFF")){p->container=2;p->big_endian=true;}
        else return fail(p,LN_PCM_UNSUPPORTED);
        p->limit=(uint64_t)u32(b+4,p->big_endian)+8u;
        if(p->limit<12||p->limit>p->file_size)return fail(p,LN_PCM_FORMAT);
        p->cursor=12;p->phase=1;return 0;
    }
    if(p->phase==1){
        if(p->have_format&&p->have_data){
            if(!p->block_align||p->data_size>UINT32_MAX)return fail(p,LN_PCM_FORMAT);
            uint32_t count=frame_count((uint32_t)p->data_size,p->block_align);
            if(count*p->block_align!=p->data_size)return fail(p,LN_PCM_FORMAT);
            p->total_frames=count;
            if(p->container==2&&p->total_frames!=p->declared_frames)return fail(p,LN_PCM_FORMAT);
            p->phase=4;return 1;
        }
        if(p->cursor>p->limit||p->limit-p->cursor<8)return fail(p,LN_PCM_FORMAT);
        r=read_at(p,p->cursor,8);if(r!=1)return r;
        p->chunk_size=u32(b+4,p->big_endian);
        p->next=p->cursor+8u+p->chunk_size+(p->chunk_size&1u);
        if(p->next>p->limit)return fail(p,LN_PCM_FORMAT);
        bool format=tag(b,p->container==1?"fmt ":"COMM");
        bool data=tag(b,p->container==1?"data":"SSND");
        if(format){if(p->have_format)return fail(p,LN_PCM_FORMAT);p->phase=2;}
        else if(data){
            if(p->have_data)return fail(p,LN_PCM_FORMAT);
            if(p->container==1){p->data_offset=p->cursor+8;p->data_size=p->chunk_size;p->have_data=true;p->cursor=p->next;}
            else p->phase=3;
        }else p->cursor=p->next;
        return 0;
    }
    if(p->phase==2){
        uint32_t need=p->container==1?16u:18u;
        if(p->chunk_size<need)return fail(p,LN_PCM_FORMAT);
        r=read_at(p,p->cursor+8,need);if(r!=1)return r;
        if(p->container==1){
            if(u16(b,false)!=1)return fail(p,LN_PCM_UNSUPPORTED);
            p->channels=u16(b+2,false);p->sample_rate=u32(b+4,false);
            p->bits=u16(b+14,false);p->block_align=u16(b+12,false);
        }else{
            p->channels=u16(b,true);p->declared_frames=u32(b+2,true);
            p->bits=u16(b+6,true);p->sample_rate=extended_rate(b+8);
            p->block_align=(uint16_t)(p->channels*(p->bits/8u));
        }
        if(p->channels<1||p->channels>2||p->sample_rate<8000||p->sample_rate>192000||
           (p->bits!=8&&p->bits!=16&&p->bits!=24&&p->bits!=32))return fail(p,LN_PCM_UNSUPPORTED);
        if(p->block_align!=p->channels*(p->bits/8u))return fail(p,LN_PCM_FORMAT);
        if(p->container==1&&u32(b+8,false)!=p->sample_rate*p->block_align)return fail(p,LN_PCM_FORMAT);
        p->have_format=true;p->cursor=p->next;p->phase=1;return 0;
    }
    if(p->chunk_size<8)return fail(p,LN_PCM_FORMAT);
    r=read_at(p,p->cursor+8,8);if(r!=1)return r;
    uint32_t offset=u32(b,true);
    if(offset>p->chunk_size-8u)return fail(p,LN_PCM_FORMAT);
    p->data_offset=p->cursor+16u+offset;p->data_size=p->chunk_size-8u-offset;
    p->have_data=true;p->cursor=p->next;p->phase=1;return 0;
}
static int16_t sample(const struct ln_pcm *p,const uint8_t *b)
{
    if(p->bits==8)return (int16_t)((p->unsigned_8?(int)b[0]-128:(int)(int8_t)b[0])*256);
    /* Retain the high signed 16 bits; no implementation-defined signed shift. */
    return (int16_t)u16(b+(p->big_endian?0:p->bits/8u-2u),p->big_endian);
}
int ln_pcm_decode(struct ln_pcm *p,int16_t *output,uint32_t capacity,uint32_t *frames)
{
    if(frames)*frames=0;
    if(!p||!output||!frames||!capacity||p->phase!=4)return LN_PCM_ARGUMENT;
    if(p->error)return p->error;
    if(p->frame==p->total_frames)return LN_PCM_EOF;
    if(!p->pending){
        uint32_t count=frame_count(sizeof(p->buffer),p->block_align);
        if(count>capacity)count=capacity;
        if(count>p->total_frames-p->frame)count=(uint32_t)(p->total_frames-p->frame);
        p->pending_bytes=count*p->block_align;
    }
    uint32_t count=frame_count(p->pending_bytes,p->block_align);
    if(count>capacity)return LN_PCM_ARGUMENT;
    int r=read_at(p,p->data_offset+p->frame*p->block_align,p->pending_bytes);
    if(r!=1)return r;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(p->bits==16u&&p->channels==2u&&!p->big_endian){
        copy_bytes(output,p->buffer,count*4u);
    }else{
#endif
        uint32_t bytes=p->bits/8u;
        for(uint32_t i=0;i<count;i++){
            const uint8_t *b=p->buffer+i*p->block_align;
            output[i*2]=sample(p,b);output[i*2+1]=p->channels==1?output[i*2]:sample(p,b+bytes);
        }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    }
#endif
    p->frame+=count;*frames=count;return LN_PCM_READY;
}
bool ln_pcm_seek(struct ln_pcm *p,uint64_t frame)
{
    if(!p||p->error||p->phase!=4||p->pending||frame>p->total_frames)return false;
    p->frame=frame;return true;
}
