#include "decoder.h"
#include "services/audio/mp3/mp3.h"
#include "services/audio/flac/flac.h"
#include "services/audio/alac/alac.h"
#include "services/audio/aac/aac.h"
#include "services/audio/aac/m4a.h"
/* Keep incompatible vendor alignment macros inside the decoder integration.
 * Already-declared codec structures retain their native layout. */
#undef MEM_ALIGN_ATTR
#include "services/audio/vorbis/vorbis.h"
#undef MEM_ALIGN_ATTR
union ln_decoder_workspace {struct ln_pcm pcm;struct ln_mp3 mp3;struct ln_flac flac;struct ln_alac alac;
    struct ln_aac aac;struct ln_aac_m4a m4a;struct ln_vorbis vorbis;};
/* Each codec initializes only its selected arena. Avoid clearing megabytes at
 * boot; retain the same placement, alignment and two playback slots. */
static union ln_decoder_workspace workspaces[LINNET_DECODER_SLOTS]
    __attribute__((section(".media_workspace"),aligned(8)));
union ln_decoder_workspace *ln_decoder_workspace_at(uint32_t slot)
{return slot<LINNET_DECODER_SLOTS?&workspaces[slot]:0;}
uint32_t ln_decoder_workspace_bytes(void){return sizeof workspaces[0];}
static bool tag(const uint8_t *b,const char *s)
{return b[0]==(uint8_t)s[0]&&b[1]==(uint8_t)s[1]&&b[2]==(uint8_t)s[2]&&b[3]==(uint8_t)s[3];}
void ln_decoder_init(struct ln_decoder *d,union ln_decoder_workspace *workspace,ln_pcm_read_fn read,void *context,uint64_t size)
{
    /* The selected codec initializes its own storage; no 1MiB clearing just
     * for detection. close must precede reuse of a previously opened decoder. */
    d->data=workspace;d->read=read;d->context=context;d->size=size;d->kind=0;d->rate=0;d->total_frames=0;d->opened=false;
}
int ln_decoder_decode(struct ln_decoder *d,int16_t *out,uint32_t capacity,uint32_t *frames)
{
    *frames=0;
    if(!d->kind){
        if(d->size<12)return -2;
        int r=d->read(d->context,0,d->signature,12);if(r!=1)return r;
        if(tag(d->signature,"RIFF")||tag(d->signature,"FORM")){
            d->kind=LN_DEC_PCM;ln_pcm_init(&d->data->pcm,d->read,d->context,d->size);
        }else if(tag(d->signature,"fLaC")){
            d->kind=LN_DEC_FLAC;ln_flac_init(&d->data->flac,d->read,d->context,d->size);
        }else if(tag(d->signature,"OggS")){
            d->kind=LN_DEC_VORBIS;ln_vorbis_init(&d->data->vorbis,d->read,d->context,d->size);
        }else if(tag(d->signature+4,"ftyp")){
            d->kind=LN_DEC_AAC_M4A;ln_aac_m4a_init(&d->data->m4a,d->read,d->context,d->size);
        }else if(d->signature[0]==255&&(d->signature[1]&0xf6u)==0xf0u){
            d->kind=LN_DEC_ADTS;ln_aac_init(&d->data->aac,d->read,d->context,d->size);
        }else{d->kind=LN_DEC_MP3;ln_mp3_init(&d->data->mp3,d->read,d->context,d->size);}
        return 0;
    }
    int r;
    switch(d->kind){
    case LN_DEC_PCM:
        r=d->opened?ln_pcm_decode(&d->data->pcm,out,capacity,frames):ln_pcm_open_step(&d->data->pcm);
        if(!d->opened&&r==1)d->opened=true;
        d->rate=d->data->pcm.sample_rate;d->total_frames=d->data->pcm.total_frames;break;
    case LN_DEC_MP3:
        r=ln_mp3_decode(&d->data->mp3,out,capacity,frames);d->opened=d->data->mp3.rate!=0;
        d->rate=d->data->mp3.rate;d->total_frames=d->data->mp3.gapless?d->data->mp3.total_frames:0;break;
    case LN_DEC_FLAC:
        r=d->opened?ln_flac_decode(&d->data->flac,out,capacity,frames):ln_flac_open_step(&d->data->flac);
        if(!d->opened&&r==1)d->opened=true;
        d->rate=d->data->flac.sample_rate;d->total_frames=d->data->flac.total_frames;break;
    case LN_DEC_ALAC:
        r=d->opened?ln_alac_decode(&d->data->alac,out,capacity,frames):ln_alac_open_step(&d->data->alac);
        if(!d->opened&&r==1)d->opened=true;
        d->rate=d->data->alac.container.rate;d->total_frames=d->data->alac.container.total_frames;break;
    case LN_DEC_ADTS:
        r=ln_aac_decode(&d->data->aac,out,capacity,frames);d->opened=d->data->aac.configured;
        d->rate=d->data->aac.rate;break;
    case LN_DEC_AAC_M4A:
        r=d->opened?ln_aac_m4a_decode(&d->data->m4a,out,capacity,frames):ln_aac_m4a_open_step(&d->data->m4a);
        if(r<0&&d->data->m4a.container.codec==18){
            d->kind=LN_DEC_ALAC;ln_alac_init(&d->data->alac,d->read,d->context,d->size);return 0;
        }
        if(!d->opened&&r==1)d->opened=true;
        d->rate=d->data->m4a.rate;d->total_frames=d->data->m4a.total_frames;break;
    case LN_DEC_VORBIS:
        r=d->opened?ln_vorbis_decode(&d->data->vorbis,out,capacity,frames):ln_vorbis_open_step(&d->data->vorbis);
        if(!d->opened&&r==1)d->opened=true;
        d->rate=d->data->vorbis.sample_rate;d->total_frames=d->data->vorbis.total_frames;break;
    default:return -2;
    }
    return r;
}
void ln_decoder_read_drained(struct ln_decoder *d)
{
    switch(d->kind){
    case LN_DEC_PCM:d->data->pcm.pending=false;break;
    case LN_DEC_MP3:d->data->mp3.pending=false;break;
    case LN_DEC_FLAC:d->data->flac.pending=false;break;
    case LN_DEC_ALAC:d->data->alac.container.pending=d->data->alac.cache_pending=false;break;
    case LN_DEC_ADTS:d->data->aac.pending=false;break;
    case LN_DEC_AAC_M4A:d->data->m4a.pending=d->data->m4a.container.pending=d->data->m4a.core.pending=false;
        d->data->m4a.cache_pending=false;break;
    case LN_DEC_VORBIS:d->data->vorbis.pending=false;break;
    default:break;
    }
}
bool ln_decoder_seek(struct ln_decoder *d,uint64_t frame)
{
    switch(d->kind){
    case LN_DEC_PCM:return ln_pcm_seek(&d->data->pcm,frame);
    case LN_DEC_MP3:return ln_mp3_seek(&d->data->mp3,frame);
    case LN_DEC_FLAC:return ln_flac_seek(&d->data->flac,frame);
    case LN_DEC_ALAC:return ln_alac_seek(&d->data->alac,frame);
    case LN_DEC_ADTS:return ln_aac_seek(&d->data->aac,frame);
    case LN_DEC_AAC_M4A:return ln_aac_m4a_seek(&d->data->m4a,frame);
    case LN_DEC_VORBIS:return ln_vorbis_seek(&d->data->vorbis,frame);
    default:return false;
    }
}
void ln_decoder_close(struct ln_decoder *d)
{
    ln_decoder_read_drained(d);
    if(d->kind==LN_DEC_VORBIS)ln_vorbis_close(&d->data->vorbis);
    d->kind=0;d->opened=false;
}
bool ln_decoder_cache_layout(const struct ln_decoder *d,struct ln_decoder_cache_layout *out)
{
    *out=(struct ln_decoder_cache_layout){0};
    if(d->kind!=LN_DEC_AAC_M4A||!d->data)return false;
    const struct ln_m4a *m=&d->data->m4a.container;
    out->media_start=m->mdat_start;out->media_end=m->mdat_end;out->tables_ready=m->opened;
    out->tables[0]=(struct ln_decoder_range){m->chunks_offset,(uint64_t)m->chunks*m->chunk_width};
    if(!m->default_size)out->tables[1]=(struct ln_decoder_range){m->sizes_offset,(uint64_t)m->packets*4u};
    return true;
}
#if !defined(LINNET_PROFILE_RELEASE)
bool ln_decoder_cache_status(const struct ln_decoder *d,struct ln_decoder_cache_status *out)
{
    *out=(struct ln_decoder_cache_status){0};
    if(d->kind!=LN_DEC_AAC_M4A||!d->data)return false;
    const struct ln_aac_m4a *a=&d->data->m4a;const struct ln_m4a *m=&a->container;
    out->request_offset=a->cache_request_offset;out->request_bytes=a->cache_request_bytes;
    out->pending=a->cache_pending;out->phase=m->decode_phase;out->bank=a->cache_filling;
    out->read_offset=m->read_offset;out->read_bytes=m->read_total;out->packet=m->packet;
    return true;
}
#endif
