/* Linnet MPEG audio framing; vendor structures are the Layer III boundary. */
#include "third_party/libmad/codeclib.h"
#include "third_party/libmad/frame.h"
#include "third_party/libmad/layer3.h"

static int syncword(const uint8_t *p)
{return p[0]==255&&(p[1]&224u)==224u;}

/* Kilobits per second indexed by version, layer and the four-bit header code.
 * I/II lengths remain recognizable so the facade can report UNSUPPORTED. */
static const uint16_t rates[5][15]={
    {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
    {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},
    {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}
};

void ln_mp3_header_init(struct mad_header *h)
{*h=(struct mad_header){0};}

void ln_mp3_stream_init(struct mad_stream *s)
{
    s->buffer=s->bufend=s->this_frame=s->next_frame=NULL;
    s->sync=0;s->freerate=0;s->md_len=0;s->anc_bitlen=0;
    s->ptr=(struct ln_mp3_bits){0};s->anc_ptr=s->ptr;
    s->options=0;s->error=MAD_ERROR_NONE;
}
void ln_mp3_stream_buffer(struct mad_stream *s,const uint8_t *data,unsigned long size)
{
    s->buffer=s->this_frame=s->next_frame=data;
    s->bufend=data?data+size:NULL;s->sync=1;
    ln_mp3_bits_init(&s->ptr,data);
}
int ln_mp3_stream_sync(struct mad_stream *s)
{
    if(!s->ptr.base||!s->bufend)return -1;
    const uint8_t *p=ln_mp3_bits_nextbyte(&s->ptr);
    while(p<=s->bufend&&s->bufend-p>=MAD_BUFFER_GUARD){
        if(syncword(p)){ln_mp3_bits_init(&s->ptr,p);return 0;}
        ++p;
    }
    return -1;
}

static enum mad_error unpack(struct mad_header *h,struct ln_mp3_bits *cursor,const uint8_t *p)
{
    unsigned version=(p[1]>>3)&3u,layer=(p[1]>>1)&3u;
    unsigned rate=p[2]>>4,frequency=(p[2]>>2)&3u;
    h->flags=0;h->private_bits=0;
    if(version<2)h->flags|=MAD_FLAG_MPEG_2_5_EXT;
    if(!(version&1u))h->flags|=MAD_FLAG_LSF_EXT;
    if(version==1)return MAD_ERROR_LOSTSYNC;
    h->layer=(enum mad_layer)(4u-layer);
    if(!layer)return MAD_ERROR_BADLAYER;
    if(!(p[1]&1u)){
        h->flags|=MAD_FLAG_PROTECTION;
        struct ln_mp3_bits crc={p,16};h->crc_check=ln_mp3_bits_crc(crc,16,65535);
    }
    if(rate==15)return MAD_ERROR_BADBITRATE;
    unsigned table=version==3?(unsigned)h->layer-1u:3u+((unsigned)h->layer>>1);
    h->bitrate=(uint32_t)rates[table][rate]*1000u;
    if(frequency==3)return MAD_ERROR_BADSAMPLERATE;
    static const uint32_t sample_rates[3]={44100,48000,32000};
    h->samplerate=sample_rates[frequency]>>(version==3?0:version==2?1:2);
    if(p[2]&2u)h->flags|=MAD_FLAG_PADDING;
    if(p[2]&1u)h->private_bits=MAD_PRIVATE_HEADER;
    h->mode=(enum mad_mode)(3u-(p[3]>>6));
    h->mode_extension=(p[3]>>4)&3u;
    if(p[3]&8u)h->flags|=MAD_FLAG_COPYRIGHT;
    if(p[3]&4u)h->flags|=MAD_FLAG_ORIGINAL;
    h->emphasis=(enum mad_emphasis)(p[3]&3u);
#ifdef OPT_STRICT
    if(h->emphasis==MAD_EMPHASIS_RESERVED)return MAD_ERROR_BADEMPHASIS;
#endif
    unsigned bytes=4;
    if(h->flags&MAD_FLAG_PROTECTION){h->crc_target=(uint16_t)((p[4]<<8)|p[5]);bytes=6;}
    *cursor=(struct ln_mp3_bits){p,bytes*8u};
    return MAD_ERROR_NONE;
}

static uint32_t slots(const struct mad_header *h)
{return h->layer==MAD_LAYER_III&&(h->flags&MAD_FLAG_LSF_EXT)?72u:144u;}
static uint32_t free_rate(const struct mad_header *h,const uint8_t *start,
                          const uint8_t *search,const uint8_t *end)
{
    unsigned padding=(h->flags&MAD_FLAG_PADDING)!=0;
    for(const uint8_t *p=search;end-p>=MAD_BUFFER_GUARD;++p){
        if(!syncword(p))continue;
        struct mad_header candidate=*h;struct ln_mp3_bits cursor;
        if(unpack(&candidate,&cursor,p)!=MAD_ERROR_NONE||
           candidate.layer!=h->layer||candidate.samplerate!=h->samplerate)continue;
        uint32_t distance=(uint32_t)(p-start),kbps;
        if(h->layer==MAD_LAYER_I)
            kbps=ln_mad_div(h->samplerate*(distance+4u-4u*padding),48)/1000u;
        else kbps=ln_mad_div(h->samplerate*(distance+1u-padding),slots(h))/1000u;
        if(kbps<8)continue;
        return h->layer==MAD_LAYER_III&&kbps>640?0:kbps*1000u;
    }
    return 0;
}
static int header_error(struct mad_stream *s,enum mad_error error)
{s->sync=0;s->error=error;return -1;}

int ln_mp3_header_decode(struct mad_header *h,struct mad_stream *s)
{
    const uint8_t *p=s->next_frame,*end=s->bufend;
    if(!p||!end)return header_error(s,MAD_ERROR_BUFPTR);
    for(;;){
        if(s->sync){
            if(end-p<MAD_BUFFER_GUARD){s->next_frame=p;return header_error(s,MAD_ERROR_BUFLEN);}
            if(!syncword(p)){
                s->this_frame=p;s->next_frame=p+1;return header_error(s,MAD_ERROR_LOSTSYNC);
            }
        }else{
            ln_mp3_bits_init(&s->ptr,p);
            if(ln_mp3_stream_sync(s)){
                if(end-s->next_frame>=MAD_BUFFER_GUARD)s->next_frame=end-MAD_BUFFER_GUARD;
                return header_error(s,MAD_ERROR_BUFLEN);
            }
            p=ln_mp3_bits_nextbyte(&s->ptr);
        }
        s->this_frame=p;s->next_frame=p+1;
        ln_mp3_bits_init(&s->ptr,p);
        enum mad_error error=unpack(h,&s->ptr,p);
        if(error)return header_error(s,error);
        if(!h->bitrate){
            if(!s->freerate||!s->sync||(h->layer==MAD_LAYER_III&&s->freerate>640000)){
                uint32_t rate=free_rate(h,p,ln_mp3_bits_nextbyte(&s->ptr),end);
                if(!rate)return header_error(s,MAD_ERROR_LOSTSYNC);
                s->freerate=rate;
            }
            h->bitrate=s->freerate;h->flags|=MAD_FLAG_FREEFORMAT;
        }
        uint32_t padding=(h->flags&MAD_FLAG_PADDING)!=0;
        uint32_t bytes=h->layer==MAD_LAYER_I?
            (ln_mad_div(12u*h->bitrate,h->samplerate)+padding)*4u:
            ln_mad_div(slots(h)*h->bitrate,h->samplerate)+padding;
        if((uint32_t)(end-p)<bytes+MAD_BUFFER_GUARD){
            s->next_frame=p;return header_error(s,MAD_ERROR_BUFLEN);
        }
        s->next_frame=p+bytes;
        if(!s->sync&&!syncword(s->next_frame)){p=s->this_frame+1;s->next_frame=p;continue;}
        s->sync=1;h->flags|=MAD_FLAG_INCOMPLETE;return 0;
    }
}
void ln_mp3_frame_mute(struct mad_frame *f)
{
    ln_mad_set(f->sbsample,0,sizeof(*f->sbsample));
    ln_mad_set(f->sbsample_prev,0,sizeof(*f->sbsample_prev));
    ln_mad_set(f->overlap,0,sizeof(*f->overlap));
}
void ln_mp3_frame_init(struct mad_frame *f)
{ln_mp3_header_init(&f->header);f->options=0;ln_mp3_frame_mute(f);}
int ln_mp3_frame_decode(struct mad_frame *f,struct mad_stream *s)
{
    f->options=s->options;
    int result=0;
    if(!(f->header.flags&MAD_FLAG_INCOMPLETE))result=ln_mp3_header_decode(&f->header,s);
    if(!result){
        if(f->header.layer!=MAD_LAYER_III){s->error=MAD_ERROR_UNSUPPORTED_LAYER;result=-1;}
        else{
            f->header.flags&=~MAD_FLAG_INCOMPLETE;
            result=mad_layer_III(s,f);
            if(result&&!MAD_RECOVERABLE(s->error))s->next_frame=s->this_frame;
        }
    }
    if(result)s->anc_bitlen=0;
    return result;
}
