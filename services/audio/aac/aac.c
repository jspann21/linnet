#include "aac.h"
#include "third_party/aac/workspace.h"
#include "third_party/aac/faad/decoder.h"
_Static_assert(sizeof(struct ln_faad_workspace)<=LINNET_AAC_WORKSPACE_BYTES,"AAC workspace capacity");
static struct ln_faad_workspace *active;
struct ln_faad_workspace *ln_faad_current(void){return active;}
static int fail(struct ln_aac *d,int error){d->error=error;return error;}
static bool bind(struct ln_aac *d)
{if(active)return false;active=(struct ln_faad_workspace *)(void *)d->workspace;return true;}
static const uint32_t rates[13]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
void ln_aac_init(struct ln_aac *d,ln_aac_read_fn read,void *context,uint64_t size)
{
    ln_faad_set(d,0,sizeof(*d));d->read=read;d->context=context;d->file_size=size;
    struct ln_faad_workspace *w=(struct ln_faad_workspace *)(void *)d->workspace;
    w->random1=1;w->random2=1;
}
bool ln_aac_seek(struct ln_aac *d,uint64_t frame)
{
    if(!d||!d->read||d->pending)return false;
    /* Reuse forward state only with valid overlap. A retarget after skipped
     * synthesis restarts, as do backward seeks and prior decode failures. */
    if(d->configured&&!d->error&&!d->overlap_invalid&&frame>=d->decoded_frames){
        d->seek_target=frame;d->seeking=frame!=d->decoded_frames;return true;
    }
    ln_aac_read_fn read=d->read;void *context=d->context;uint64_t size=d->file_size;
    ln_aac_init(d,read,context,size);d->seek_target=frame;d->seeking=frame!=0;return true;
}
static int configure(struct ln_aac *d,const uint8_t *asc,uint32_t bytes)
{
    if(!asc||bytes<2||bytes>64)return fail(d,LN_AAC_ARGUMENT);
    uint32_t object=asc[0]>>3,index=((asc[0]&7u)<<1)|(asc[1]>>7),channels=(asc[1]>>3)&15u;
    if(object!=2||index>=13||channels<1||channels>2||(asc[1]&7))return fail(d,LN_AAC_UNSUPPORTED);
    if(!bind(d))return LN_AAC_ARGUMENT;
    NeAACDecHandle decoder=NeAACDecOpen();
    NeAACDecConfigurationPtr config=NeAACDecGetCurrentConfiguration(decoder);
    config->defObjectType=2;config->dontUpSampleImplicitSBR=1;config->outputFormat=FAAD_FMT_FIXED;
    uint32_t rate=0;uint8_t got_channels=0;
    int result=NeAACDecInit2(decoder,(uint8_t *)(uintptr_t)asc,bytes,&rate,&got_channels);
    bool valid=result==0&&decoder->object_type==2&&decoder->frameLength==1024&&rate==rates[index]&&got_channels==channels;
    active=0;
    if(!valid)return fail(d,LN_AAC_UNSUPPORTED);
    d->rate=rate;d->channels=channels;d->frequency_index=(uint8_t)index;d->configured=true;return 1;
}
int ln_aac_raw_init(struct ln_aac *d,const uint8_t *asc,uint32_t bytes)
{if(!d)return LN_AAC_ARGUMENT;ln_aac_init(d,0,0,0);return configure(d,asc,bytes);}
static int16_t pcm16(int32_t value)
/* FAAD fixed output has fourteen fractional bits relative to PCM16. */
{if(value>=0x20000000)return 32767;if(value<=-0x20000000)return -32768;return (int16_t)(value>>14);}
static int raw_decode(struct ln_aac *d,const uint8_t *au,uint32_t bytes,bool materialize,bool skip_synthesis)
{
    if(!d||!au||!bytes||bytes>8191||!d->configured||d->pcm_cursor<d->pcm_frames)return LN_AAC_ARGUMENT;
    if(d->error)return d->error;
    if(materialize&&d->overlap_invalid)return LN_AAC_ARGUMENT;
    if(!bind(d))return LN_AAC_ARGUMENT;
    active->skip_synthesis=skip_synthesis;
    active->discard_output=!materialize;
    NeAACDecFrameInfo info;void *result=NeAACDecDecode(&active->decoder,&info,(uint8_t *)(uintptr_t)au,bytes);
    active->discard_output=false;active->skip_synthesis=false;
    bool valid=result&&!info.error&&info.object_type==2&&info.channels==d->channels&&info.samplerate==d->rate&&info.bytesconsumed<=bytes;
    if(valid&&materialize){
        for(uint32_t i=0;i<1024;i++){
            d->pcm[i*2]=pcm16(active->decoder.time_out[0][i]);
            d->pcm[i*2+1]=d->channels==1?d->pcm[i*2]:pcm16(active->decoder.time_out[1][i]);
        }
    }
    active=0;
    if(!valid){d->overlap_invalid=true;return fail(d,LN_AAC_FORMAT);}
    /* Publish validity only after the entire AU and every channel succeeded. */
    d->overlap_invalid=skip_synthesis;
    /* Preserve the first AU's actual PCM too. Upstream hides that block in
     * hInfo.samples; container/ADTS callers need an explicit untrimmed timeline. */
    d->pcm_frames=1024;d->pcm_cursor=materialize?0u:1024u;d->access_units++;return 1;
}
int ln_aac_raw_decode(struct ln_aac *d,const uint8_t *au,uint32_t bytes)
{return raw_decode(d,au,bytes,true,false);}
int ln_aac_raw_discard(struct ln_aac *d,const uint8_t *au,uint32_t bytes)
{return raw_decode(d,au,bytes,false,false);}
int ln_aac_raw_skip_synthesis(struct ln_aac *d,const uint8_t *au,uint32_t bytes)
{return raw_decode(d,au,bytes,false,true);}
bool ln_aac_checkpoint(struct ln_aac *d,uint32_t state[3],bool restore)
{
    if(!d||!state||!d->configured||d->pending||d->error||active)return false;
    struct ln_faad_workspace *w=(struct ln_faad_workspace *)(void *)d->workspace;
    if(restore){
        if((state[2]&~0x101u)||!state[0]||!state[1])return false;
        w->random1=state[0];w->random2=state[1];
        w->decoder.window_shape_prev[0]=state[2]&1u;
        w->decoder.window_shape_prev[1]=(state[2]>>8)&1u;
        d->overlap_invalid=true;
    }else {
        state[0]=w->random1;state[1]=w->random2;
        state[2]=w->decoder.window_shape_prev[0]|((uint32_t)w->decoder.window_shape_prev[1]<<8);
    }
    return true;
}
static int read_step(struct ln_aac *d,uint64_t offset,uint8_t *buffer,uint32_t bytes)
{
    if(offset>d->file_size||bytes>d->file_size-offset)return fail(d,LN_AAC_FORMAT);
    int r=d->read(d->context,offset,buffer,bytes);d->pending=r==0;
    return r<0?fail(d,LN_AAC_IO):r;
}
int ln_aac_decode(struct ln_aac *d,int16_t *output,uint32_t capacity,uint32_t *frames)
{
    if(frames)*frames=0;
    if(!d||!d->read||!output||!capacity||!frames)return LN_AAC_ARGUMENT;
    if(d->error)return d->error;
    if(d->pcm_cursor<d->pcm_frames){
        if(d->seeking){
            uint32_t count=d->pcm_frames-d->pcm_cursor;
            if(count>d->seek_target-d->decoded_frames)count=(uint32_t)(d->seek_target-d->decoded_frames);
            d->pcm_cursor+=count;d->decoded_frames+=count;
            if(d->decoded_frames==d->seek_target)d->seeking=false;
            return 0;
        }
        uint32_t count=d->pcm_frames-d->pcm_cursor;if(count>capacity)count=capacity;
        ln_faad_copy(output,d->pcm+d->pcm_cursor*2u,count*4u);d->pcm_cursor+=count;d->decoded_frames+=count;*frames=count;return 1;
    }
    if(d->offset==d->file_size)return d->seeking?fail(d,LN_AAC_ARGUMENT):(d->access_units?LN_AAC_EOF:fail(d,LN_AAC_FORMAT));
    if(d->phase==0){
        int r=read_step(d,d->offset,d->input,7);if(r!=1)return r;
        const uint8_t *b=d->input;
        if(b[0]!=255||(b[1]&0xf6u)!=0xf0u)return fail(d,LN_AAC_FORMAT);
        uint32_t profile=b[2]>>6,index=(b[2]>>2)&15u,channels=((b[2]&1u)<<2)|(b[3]>>6);
        if(!(b[1]&1)||profile!=1||index>=13||channels<1||channels>2||(b[6]&3))return fail(d,LN_AAC_UNSUPPORTED);
        d->frame_bytes=((uint32_t)(b[3]&3u)<<11)|((uint32_t)b[4]<<3)|(b[5]>>5);
        if(d->frame_bytes<8||d->frame_bytes>d->file_size-d->offset)return fail(d,LN_AAC_FORMAT);
        if(d->configured&&(d->rate!=rates[index]||d->channels!=channels))return fail(d,LN_AAC_UNSUPPORTED);
        if(!d->configured){
            uint8_t asc[2]={(uint8_t)((2u<<3)|(index>>1)),(uint8_t)(((index&1u)<<7)|(channels<<3))};
            r=configure(d,asc,2);if(r!=1)return r;
        }
        d->phase=1;return 0;
    }
    if(d->phase==1){
        int r=read_step(d,d->offset+7,d->input+7,d->frame_bytes-7);if(r!=1)return r;
        d->phase=2;return 0;
    }
    bool discard=d->seeking&&d->seek_target-d->decoded_frames>=1024u;
    bool intermediate=discard&&d->seek_target-d->decoded_frames>=2048u;
    int r=raw_decode(d,d->input+7,d->frame_bytes-7,!discard,intermediate);if(r!=1)return r;
    /* Retire unmaterialized output before yielding: another seek may retarget
     * this context between calls, so it must never see stale buffered PCM. */
    if(discard){d->decoded_frames+=1024u;if(d->decoded_frames==d->seek_target)d->seeking=false;}
    d->offset+=d->frame_bytes;d->phase=0;return 0;
}
