#include "alac.h"
#include "services/audio/alac/output.h"
#if defined(__arm__)
extern void ln_memory_clear(void *,uint32_t);
#endif
static int bad(struct ln_alac *a,int result){a->container.error=result;return result;}
/* The outer decoder owns exactly one outstanding immutable read. Keep its
 * identity while a larger, bounded source read fills one of two windows. */
static int cached_read(void *context,uint64_t offset,uint8_t *buffer,uint32_t bytes)
{
    struct ln_alac *d=context;uint64_t size=d->container.file_size;
    if(!bytes||bytes>16384||offset>size||bytes>size-offset)return -1;
    uint32_t slot=d->cache_filling;
    if(d->cache_pending){
        if(offset!=d->cache_request_offset||bytes!=d->cache_request_bytes||buffer!=d->cache_request_buffer)return -1;
    }else{
        uint32_t overlap=2;
        for(uint32_t i=0;i<2;i++){
            if(offset>=d->cache_offset[i]&&offset-d->cache_offset[i]<=d->cache_bytes[i]&&
               bytes<=d->cache_bytes[i]-(uint32_t)(offset-d->cache_offset[i])){
                uint32_t at=(uint32_t)(offset-d->cache_offset[i]);
                for(uint32_t j=0;j<bytes;j++)buffer[j]=d->read_cache[i][at+j];
                d->cache_victim=(uint8_t)(i^1u);return 1;
            }
            if(offset>=d->cache_offset[i]&&offset-d->cache_offset[i]<d->cache_bytes[i])overlap=i;
        }
        if(overlap!=d->cache_victim)overlap=2;
        slot=d->cache_victim;d->cache_filling=(uint8_t)slot;
        uint32_t retained=0;
        if(overlap<2){
            uint32_t at=(uint32_t)(offset-d->cache_offset[slot]);retained=d->cache_bytes[slot]-at;
            /* A packet can straddle the media window. Keep its cached prefix
             * and read only the new suffix, preserving the other table window. */
            for(uint32_t j=0;j<retained;j++)d->read_cache[slot][j]=d->read_cache[slot][at+j];
        }
        d->cache_offset[slot]=offset;d->cache_bytes[slot]=retained;
        d->cache_request_offset=offset;d->cache_request_bytes=bytes;d->cache_request_buffer=buffer;
    }
    uint32_t fill=size-d->cache_offset[slot]>16384?16384:(uint32_t)(size-d->cache_offset[slot]);
    uint32_t retained=d->cache_bytes[slot];
    int result=d->source_read(d->source_context,d->cache_offset[slot]+retained,d->read_cache[slot]+retained,fill-retained);
    d->cache_pending=result==0;if(result!=1)return result;
    d->cache_bytes[slot]=fill;d->cache_victim=(uint8_t)(slot^1u);
    for(uint32_t j=0;j<bytes;j++)buffer[j]=d->read_cache[slot][j];
    return 1;
}
void ln_alac_init(struct ln_alac *a,ln_alac_read_fn read,void *context,uint64_t size)
{
#if defined(__arm__)
    ln_memory_clear(a,sizeof(*a));
#else
    uint8_t *p=(uint8_t *)a;for(uint32_t i=0;i<sizeof(*a);i++)p[i]=0;
#endif
    a->source_read=read;a->source_context=context;
    a->container.read=cached_read;a->container.context=a;
    a->container.file_size=size;a->container.ends[0]=size;
}
int ln_alac_open_step(struct ln_alac *a)
{
    if(!a)return LN_ALAC_ARGUMENT;
    if(a->container.opened)return a->container.error?a->container.error:LN_ALAC_READY;
    int result=ln_m4a_open_step(&a->container);if(result!=LN_M4A_READY)return result;
    const uint8_t *c=a->container.alac_config;
    a->core.setinfo_max_samples_per_frame=a->container.max_packet_frames;
    a->core.setinfo_7a=c[4];a->core.setinfo_sample_size=c[5];
    a->core.setinfo_rice_historymult=c[6];a->core.setinfo_rice_initialhistory=c[7];
    a->core.setinfo_rice_kmodifier=c[8];a->core.setinfo_7f=c[9];
    return LN_ALAC_READY;
}
static void nothing(void){}
int ln_alac_decode(struct ln_alac *a,int16_t *out,uint32_t capacity,uint32_t *frames)
{
    if(!a||!out||!frames||!capacity)return LN_ALAC_ARGUMENT;*frames=0;
    if(a->container.error)return a->container.error;
    if(a->pcm_cursor<a->pcm_count){
        uint32_t count=a->pcm_count-a->pcm_cursor;if(count>capacity)count=capacity;
        ln_alac_output(out,a->pcm[0]+a->pcm_cursor,a->pcm[1]+a->pcm_cursor,count,
                       a->core.output_channels,a->core.output_shift,a->core.output_weight);
        a->pcm_cursor+=count;a->frame+=count;*frames=count;return 1;
    }
    if(!a->container.opened){int result=ln_alac_open_step(a);return result==1?0:result;}
    int result=ln_m4a_read_packet(&a->container);if(result!=1)return result;
    if(a->container.codec!=18)return bad(a,LN_ALAC_UNSUPPORTED);
    int count=alac_decode_frame(&a->core,a->container.buffer,a->container.packet_size,a->pcm,nothing);
    if(count<=0||(uint32_t)count!=a->container.current_packet_frames||a->container.skip>(uint32_t)count)return bad(a,LN_ALAC_FORMAT);
    a->pcm_count=(uint32_t)count;a->pcm_cursor=a->container.skip;a->container.skip=0;return 0;
}
bool ln_alac_seek(struct ln_alac *a,uint64_t frame)
{
    if(!a||!ln_m4a_seek(&a->container,frame))return false;
    a->frame=frame;a->pcm_count=a->pcm_cursor=0;return true;
}
