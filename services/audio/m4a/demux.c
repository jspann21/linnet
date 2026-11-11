#include "boot/include/linnet_bytes.h"
#include "demux.h"
#if defined(__arm__)
extern void ln_memory_clear(void *,uint32_t);
#endif
#define TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))
static uint16_t be16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t divide_small(uint32_t value,uint32_t divisor,uint32_t *remainder)
{
    /* Sequential packets commonly remain inside the first chunk. */
    if(value<divisor){if(remainder)*remainder=value;return 0;}
    if(divisor==1u){if(remainder)*remainder=0;return value;}
    uint32_t quotient=0,r=0;
    for(uint32_t i=32;i;i--){r=(r<<1)|((value>>(i-1u))&1u);if(r>=divisor){r-=divisor;quotient|=1u<<(i-1u);}}
    if(remainder)*remainder=r;return quotient;
}
static int bad(struct ln_m4a *a,int result){a->error=result;return result;}
static int read_bytes(struct ln_m4a *a,uint64_t off,uint32_t count)
{
    if(!count||count>sizeof(a->buffer)||off>a->file_size||count>a->file_size-off)return bad(a,LN_M4A_FORMAT);
    if(!a->read_total){a->read_offset=off;a->read_total=count;a->read_done=0;}
    if(a->read_offset!=off||a->read_total!=count)return bad(a,LN_M4A_ARGUMENT);
    uint32_t bytes=count-a->read_done;if(bytes>16384)bytes=16384;
    int result=a->read(a->context,off+a->read_done,a->buffer+a->read_done,bytes);a->pending=result==0;
    if(result<0){a->read_total=0;return bad(a,LN_M4A_IO);}if(!result)return 0;
    a->read_done+=bytes;if(a->read_done!=count)return 0;a->read_total=0;return 1;
}
void ln_m4a_init(struct ln_m4a *a,ln_m4a_read_fn read,void *context,uint64_t size)
{
#if defined(__arm__)
    ln_memory_clear(a,sizeof(*a));
#else
    uint8_t *p=(uint8_t *)a;for(uint32_t i=0;i<sizeof(*a);i++)p[i]=0;
#endif
    a->read=read;a->context=context;a->file_size=size;a->ends[0]=size;a->allow_aac=true;
}
static bool parent(struct ln_m4a *a,uint32_t type){return a->depth&&a->parents[a->depth]==type;}
static bool once(struct ln_m4a *a,uint32_t bit){if(a->seen&bit)return false;a->seen|=bit;return true;}
static bool descriptors(struct ln_m4a *a,const uint8_t *b,uint32_t bytes,uint32_t depth)
{
    if(depth>4)return false;
    uint32_t at=0;
    while(at<bytes){
        uint32_t tag=b[at++],size=0,n=0;uint8_t byte;
        do{if(at==bytes||++n>4)return false;byte=b[at++];size=(size<<7)|(byte&127u);}while(byte&128u);
        if(size>bytes-at)return false;
        const uint8_t *p=b+at;uint32_t skip=0;
        if(tag==3){
            if(size<3)return false;uint8_t flags=p[2];skip=3;
            if(flags&128u)skip+=2;
            if(flags&64u){if(skip>=size)return false;skip+=1u+p[skip];}
            if(flags&32u)skip+=2;
        }else if(tag==4){if(size<13||p[0]!=0x40||((p[1]>>2)&63u)!=5)return false;skip=13;}
        else if(tag==5){
            if(a->asc_bytes||size<2||size>64)return false;
            for(uint32_t i=0;i<size;i++)a->asc[i]=p[i];a->asc_bytes=size;
        }
        if(skip&&(skip>size||!descriptors(a,p+skip,size-skip,depth+1)))return false;
        at+=size;
    }return true;
}
static bool asc_info(struct ln_m4a *a)
{
    static const uint32_t rates[13]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if(a->asc_bytes<2||(a->asc[0]>>3)!=2)return false;
    uint32_t index=((a->asc[0]&7u)<<1)|(a->asc[1]>>7);
    if(index>=13)return false;
    a->rate=rates[index];a->channels=(a->asc[1]>>3)&15u;
    return a->channels>=1&&a->channels<=2&&a->rate>=8000;
}
static int body(struct ln_m4a *a)
{
    uint8_t *b=a->buffer;uint32_t n=a->body_bytes,t=a->atom_type;
    if(t==TAG('s','t','s','d')){
        if(n!=44||ln_read_be32(b)||ln_read_be32(b+4)!=1||!once(a,1))return bad(a,LN_M4A_FORMAT);
        uint32_t entry=ln_read_be32(b+8);
        uint32_t type=ln_read_be32(b+12);
        if(entry!=a->atom_end-a->cursor-8||entry<44||be16(b+22)!=1||be16(b+24))return bad(a,LN_M4A_UNSUPPORTED);
        if(type==TAG('a','l','a','c'))a->codec=18;
        else if(type==TAG('m','p','4','a')&&a->allow_aac)a->codec=17;
        else return bad(a,LN_M4A_UNSUPPORTED);
        a->channels=be16(b+32);
        if(a->codec==18&&be16(b+34)!=16)return bad(a,LN_M4A_FORMAT);
        /* Walk sample-entry children with the bounded atom reader. iTunes can
         * attach large purchase metadata; only codec configuration needs RAM. */
        if(a->depth>=7)return bad(a,LN_M4A_UNSUPPORTED);
        a->depth++;a->ends[a->depth]=a->atom_end;a->parents[a->depth]=type;
        a->cursor+=44;a->phase=0;return 0;
    }else if(t==TAG('a','l','a','c')&&parent(a,TAG('a','l','a','c'))){
        if(n!=28||ln_read_be32(b)||a->rate)return bad(a,LN_M4A_FORMAT);
        uint8_t *c=b+4;
        if(a->channels!=c[9])return bad(a,LN_M4A_FORMAT);
        a->max_packet_frames=ln_read_be32(c);
        for(uint32_t i=0;i<sizeof(a->alac_config);i++)a->alac_config[i]=c[i];
        a->rate=ln_read_be32(c+20);
        if(c[4]||c[5]!=16||!a->channels||a->channels>2||a->rate<8000||a->rate>96000||!ln_read_be32(c)||ln_read_be32(c)>4096||c[8]>14)return bad(a,LN_M4A_UNSUPPORTED);
    }else if(t==TAG('e','s','d','s')&&parent(a,TAG('m','p','4','a'))){
        if(n<4||ln_read_be32(b)||a->rate||!descriptors(a,b+4,n-4,0)||!asc_info(a))return bad(a,LN_M4A_UNSUPPORTED);
        a->max_packet_frames=1024;
    }else if(t==TAG('s','t','s','z')){
        if(n!=12||ln_read_be32(b)||!once(a,2))return bad(a,LN_M4A_FORMAT);
        a->default_size=ln_read_be32(b+4);a->packets=ln_read_be32(b+8);a->sizes_offset=a->cursor+12;
        if(!a->packets||a->packets>1000000||a->default_size>32768||a->atom_end-a->cursor!=12u+(a->default_size?0u:(uint64_t)a->packets*4u))return bad(a,LN_M4A_UNSUPPORTED);
    }else if(t==TAG('s','t','c','o')||t==TAG('c','o','6','4')){
        if(n!=8||ln_read_be32(b)||!once(a,4))return bad(a,LN_M4A_FORMAT);
        a->chunks=ln_read_be32(b+4);a->chunk_width=t==TAG('s','t','c','o')?4u:8u;a->chunks_offset=a->cursor+8;
        if(!a->chunks||a->chunks>1000000||a->atom_end-a->cursor!=8u+(uint64_t)a->chunks*a->chunk_width)return bad(a,LN_M4A_UNSUPPORTED);
    }else if(t==TAG('s','t','s','c')){
        if(n<20||ln_read_be32(b)||!once(a,8))return bad(a,LN_M4A_FORMAT);
        a->chunk_count=ln_read_be32(b+4);
        if(!a->chunk_count||a->chunk_count>1024||n!=8+12*a->chunk_count)return bad(a,LN_M4A_UNSUPPORTED);
        for(uint32_t i=0;i<a->chunk_count;i++){
            uint8_t *c=b+8+i*12;a->map[i].first=ln_read_be32(c);a->map[i].count=ln_read_be32(c+4);
            if(!a->map[i].first||!a->map[i].count||a->map[i].count>1000000||ln_read_be32(c+8)!=1||(!i&&a->map[i].first!=1)||(i&&a->map[i].first<=a->map[i-1].first))return bad(a,LN_M4A_UNSUPPORTED);
        }
    }else if(t==TAG('s','t','t','s')){
        if(a->codec==17){
            if(n!=8||ln_read_be32(b)||!once(a,16))return bad(a,LN_M4A_FORMAT);
            uint32_t runs=ln_read_be32(b+4);
            if(!runs||runs>1000000||a->atom_end-a->cursor!=8u+(uint64_t)runs*8u)return bad(a,LN_M4A_UNSUPPORTED);
            a->times[0].delta=a->times[1].delta=1024;a->time_regular=true;
            a->cursor+=8;a->phase=2;return 0;
        }
        if(n<16||ln_read_be32(b)||!once(a,16))return bad(a,LN_M4A_FORMAT);
        a->time_count=ln_read_be32(b+4);
        if(!a->time_count||a->time_count>128||n!=8+8*a->time_count)return bad(a,LN_M4A_UNSUPPORTED);
        for(uint32_t i=0;i<a->time_count;i++){
            a->times[i].count=ln_read_be32(b+8+i*8);a->times[i].delta=ln_read_be32(b+12+i*8);
            if(!a->times[i].count||!a->times[i].delta||a->times[i].delta>4096)return bad(a,LN_M4A_UNSUPPORTED);
        }
    }else if(t==TAG('m','v','h','d')){
        if(n<20||ln_read_be32(b)||a->movie_scale)return bad(a,LN_M4A_UNSUPPORTED);
        a->movie_scale=ln_read_be32(b+12);
        if(!a->movie_scale||a->movie_scale>1000000)return bad(a,LN_M4A_UNSUPPORTED);
    }else if(t==TAG('m','d','h','d')){
        if(n<24||ln_read_be32(b)||!once(a,32))return bad(a,LN_M4A_UNSUPPORTED);
        /* sample timescale is compared to the ALAC rate after all atoms. */
        a->skip=ln_read_be32(b+12);
    }else if(t==TAG('h','d','l','r')){
        if(a->track_probe){
            if(n<12||ln_read_be32(b))return bad(a,LN_M4A_FORMAT);
            bool audio=ln_read_be32(b+8)==TAG('s','o','u','n');
            if(audio&&++a->tracks>1)return bad(a,LN_M4A_UNSUPPORTED);
            a->depth=a->track_depth;a->cursor=audio?a->track_start:a->ends[a->depth];
            a->track_probe=false;a->phase=0;return 0;
        }
        if(n<12||ln_read_be32(b)||ln_read_be32(b+8)!=TAG('s','o','u','n')||!once(a,64))return bad(a,LN_M4A_UNSUPPORTED);
    }else if(t==TAG('e','l','s','t')){
        if(n!=20||ln_read_be32(b)||ln_read_be32(b+4)!=1||(ln_read_be32(b+12)&0x80000000u)||ln_read_be32(b+16)!=0x10000||!once(a,256))return bad(a,LN_M4A_UNSUPPORTED);
        a->lead_trim=ln_read_be32(b+12);
        a->edit_duration=ln_read_be32(b+8);
    }else if(t==TAG('d','r','e','f')){
        if(n!=20||ln_read_be32(b)||ln_read_be32(b+4)!=1||ln_read_be32(b+8)!=12||ln_read_be32(b+12)!=TAG('u','r','l',' ')||ln_read_be32(b+16)!=1||!once(a,128))return bad(a,LN_M4A_UNSUPPORTED);
    }
    a->cursor=a->atom_end;a->phase=0;return 0;
}
static int aac_times(struct ln_m4a *a)
{
    /* AAC-LC emits 1024 samples per packet. Reduce its timing table as it is
     * read, preserving a final short packet and tolerating rounded timestamps
     * only when their complete timeline differs by less than one packet. */
    uint32_t bytes=a->atom_end-a->cursor>4096?4096:(uint32_t)(a->atom_end-a->cursor);
    int r=read_bytes(a,a->cursor,bytes);if(r!=1)return r;
    for(uint32_t i=0;i<bytes;i+=8){
        uint32_t count=ln_read_be32(a->buffer+i),delta=ln_read_be32(a->buffer+i+4);
        if(!count||count>1000000-a->times[0].count||!delta||delta>4096)return bad(a,LN_M4A_UNSUPPORTED);
        if(a->times[1].delta!=1024)a->time_regular=false;
        a->times[0].count+=count;a->total_frames+=(uint64_t)count*delta;
        a->times[1].count=count;a->times[1].delta=delta;
    }
    a->cursor+=bytes;if(a->cursor<a->atom_end)return 0;
    a->time_count=1;
    if(a->time_regular&&a->times[1].count==1&&a->times[1].delta<1024){
        if(a->times[0].count==1)a->times[0]=a->times[1];
        else {a->times[0].count--;a->time_count=2;}
    }else{
        uint64_t nominal=(uint64_t)a->times[0].count*1024u,actual=a->total_frames;
        if((nominal>actual?nominal-actual:actual-nominal)>=1024)return bad(a,LN_M4A_UNSUPPORTED);
    }
    a->phase=0;return 0;
}
static int finish_open(struct ln_m4a *a)
{
    if((a->seen&255u)!=255||a->tracks!=1||a->skip!=a->rate||!a->media_count)return bad(a,LN_M4A_UNSUPPORTED);
    uint64_t packets=0,frames=0;
    for(uint32_t i=0;i<a->time_count;i++){
        if(a->times[i].delta>a->max_packet_frames)return bad(a,LN_M4A_FORMAT);
        packets+=a->times[i].count;frames+=(uint64_t)a->times[i].count*a->times[i].delta;
    }
    if(packets!=a->packets)return bad(a,LN_M4A_FORMAT);
    packets=0;
    for(uint32_t i=0;i<a->chunk_count;i++){
        uint32_t end=i+1<a->chunk_count?a->map[i+1].first:a->chunks+1;
        if(a->map[i].first>a->chunks||end<=a->map[i].first)return bad(a,LN_M4A_FORMAT);
        packets+=(uint64_t)(end-a->map[i].first)*a->map[i].count;
    }
    if(packets!=a->packets)return bad(a,LN_M4A_FORMAT);
    if(a->lead_trim>=frames||(a->codec==18&&a->lead_trim))return bad(a,LN_M4A_UNSUPPORTED);
    if(a->seen&256u){
        if(!a->movie_scale)return bad(a,LN_M4A_FORMAT);
        uint64_t movie=(uint64_t)a->edit_duration*a->rate,media=(frames-a->lead_trim)*a->movie_scale;
        if((movie>media?movie-media:media-movie)>=a->rate){
            if(a->codec!=17||movie>media||media-movie>=1024u*(uint64_t)a->movie_scale)return bad(a,LN_M4A_UNSUPPORTED);
            a->tail_trim=divide_small((uint32_t)(media-movie),a->movie_scale,0);
            if(a->tail_trim>=frames-a->lead_trim)return bad(a,LN_M4A_FORMAT);
        }
    }
    a->total_frames=frames;a->skip=0;a->opened=true;return 1;
}
int ln_m4a_open_step(struct ln_m4a *a)
{
    if(!a||!a->read)return LN_M4A_ARGUMENT;if(a->error)return a->error;if(a->opened)return 1;
    if(a->phase==2)return aac_times(a);
    if(a->phase){int result=read_bytes(a,a->cursor,a->body_bytes);return result==1?body(a):result;}
    while(a->cursor==a->ends[a->depth]){
        if(a->track_probe&&a->depth==a->track_depth)return bad(a,LN_M4A_FORMAT);
        if(!a->depth)return finish_open(a);a->depth--;
    }
    uint64_t remain=a->ends[a->depth]-a->cursor;
    if(a->cursor>a->ends[a->depth]||remain<8)return bad(a,LN_M4A_FORMAT);
    int result=read_bytes(a,a->cursor,remain>=16?16:8);if(result!=1)return result;
    if(++a->atoms>4096)return bad(a,LN_M4A_UNSUPPORTED);
    uint32_t head=8,t=ln_read_be32(a->buffer+4);uint64_t size=ln_read_be32(a->buffer);
    if(size==1){if(remain<16)return bad(a,LN_M4A_FORMAT);size=ln_read_be64(a->buffer+8);head=16;}
    else if(!size){size=remain;if(!a->depth)a->extends_to_eof=true;}
    if(size<head||size>remain)return bad(a,LN_M4A_FORMAT);
    a->atom_start=a->cursor;a->atom_end=a->cursor+size;a->atom_type=t;
    if(t==TAG('m','o','o','f')||t==TAG('s','e','n','c'))return bad(a,LN_M4A_UNSUPPORTED);
    bool sample_entry=parent(a,TAG('m','p','4','a'))||parent(a,TAG('a','l','a','c'));
    bool container=!sample_entry&&(!a->track_probe||t==TAG('m','d','i','a'))&&
        (t==TAG('m','o','o','v')||t==TAG('t','r','a','k')||t==TAG('m','d','i','a')||t==TAG('m','i','n','f')||t==TAG('s','t','b','l')||t==TAG('e','d','t','s')||t==TAG('d','i','n','f'));
    if(container){
        if(a->depth>=7)return bad(a,LN_M4A_UNSUPPORTED);
        a->depth++;a->ends[a->depth]=a->atom_end;a->parents[a->depth]=t;a->cursor+=head;
        if(t==TAG('t','r','a','k')){a->track_probe=true;a->track_start=a->cursor;a->track_depth=a->depth;}
        return 0;
    }
    if(t==TAG('m','d','a','t')){
        if(a->depth||a->media_count==8)return bad(a,LN_M4A_UNSUPPORTED);
        a->media_start[a->media_count]=a->cursor+head;a->media_end[a->media_count++]=a->atom_end;
    }
    uint64_t bytes=size-head;uint32_t take=0;
    if(a->track_probe){
        if(parent(a,TAG('m','d','i','a'))&&t==TAG('h','d','l','r'))take=12;
    }else if(sample_entry){
        if((a->codec==17&&t==TAG('e','s','d','s'))||(a->codec==18&&t==TAG('a','l','a','c'))){
            if(bytes>4096)return bad(a,LN_M4A_UNSUPPORTED);take=(uint32_t)bytes;
            if(!take)return bad(a,LN_M4A_FORMAT);
        }
    }else if(parent(a,TAG('s','t','b','l'))){
        if(t==TAG('s','t','s','d'))take=44;
        else if(t==TAG('s','t','t','s')&&a->codec==17)take=8;
        else if(t==TAG('s','t','s','c')){if(bytes>8+12*1024)return bad(a,LN_M4A_UNSUPPORTED);take=(uint32_t)bytes;}
        else if(t==TAG('s','t','t','s')){if(bytes>4096)return bad(a,LN_M4A_UNSUPPORTED);take=(uint32_t)bytes;}
        else if(t==TAG('s','t','s','z'))take=12;
        else if(t==TAG('s','t','c','o')||t==TAG('c','o','6','4'))take=8;
    }else if(parent(a,TAG('m','d','i','a'))&&(t==TAG('m','d','h','d')||t==TAG('h','d','l','r'))){take=t==TAG('m','d','h','d')?24:12;}
    else if(parent(a,TAG('m','o','o','v'))&&t==TAG('m','v','h','d'))take=20;
    else if((parent(a,TAG('e','d','t','s'))&&t==TAG('e','l','s','t'))||(parent(a,TAG('d','i','n','f'))&&t==TAG('d','r','e','f'))){if(bytes>4096)return bad(a,LN_M4A_UNSUPPORTED);take=(uint32_t)bytes;}
    if(take){if(take>bytes)return bad(a,LN_M4A_FORMAT);a->cursor+=head;a->body_bytes=take;a->phase=1;return 0;}
    a->cursor=a->atom_end;return 0;
}
static bool media_extent(struct ln_m4a *a,uint64_t offset)
{
    for(uint32_t i=0;i<a->media_count;i++)if(offset>=a->media_start[i]&&offset<a->media_end[i]){
        a->mdat_start=a->media_start[i];a->mdat_end=a->media_end[i];return true;
    }
    return false;
}
static uint32_t packet_frames(struct ln_m4a *a,uint32_t packet)
{
    for(uint32_t i=0;i<a->time_count;i++){if(packet<a->times[i].count)return a->times[i].delta;packet-=a->times[i].count;}return 0;
}
static bool locate(struct ln_m4a *a)
{
    if(a->chunk_packets&&a->packet>=a->chunk_first&&a->packet-a->chunk_first<a->chunk_packets){
        a->chunk_ordinal=a->packet-a->chunk_first;return true;
    }
    uint32_t first=0;
    for(uint32_t i=0;i<a->chunk_count;i++){
        uint32_t end=i+1<a->chunk_count?a->map[i+1].first:a->chunks+1;
        uint32_t count=(end-a->map[i].first)*a->map[i].count;
        if(a->packet-first<count){uint32_t rel=a->packet-first;
            a->chunk_index=a->map[i].first-1+divide_small(rel,a->map[i].count,&a->chunk_ordinal);
            a->chunk_first=a->packet-a->chunk_ordinal;a->chunk_packets=a->map[i].count;return true;}
        first+=count;
    }return false;
}
int ln_m4a_read_packet(struct ln_m4a *a)
{
    if(!a)return LN_M4A_ARGUMENT;
    if(a->error)return a->error;if(!a->opened){int result=ln_m4a_open_step(a);return result==1?0:result;}
    if(a->packet==a->packets)return 2;
    if(!a->decode_phase){
        if(!locate(a))return bad(a,LN_M4A_FORMAT);
        a->size_scan=0;a->decode_phase=1;
        if(!a->default_size&&a->next_valid&&a->next_packet==a->packet&&a->next_chunk==a->chunk_index){
            a->packet_offset=a->next_offset;a->size_scan=a->chunk_ordinal;a->decode_phase=2;
        }
    }
    if(a->decode_phase==1){
        int result=read_bytes(a,a->chunks_offset+(uint64_t)a->chunk_index*a->chunk_width,a->chunk_width);if(result!=1)return result;
        a->packet_offset=a->chunk_width==4?ln_read_be32(a->buffer):ln_read_be64(a->buffer);
        if(!media_extent(a,a->packet_offset))return bad(a,LN_M4A_FORMAT);
        a->decode_phase=2;
    }
    if(a->decode_phase==2){
        if(a->default_size){
            a->packet_size=a->default_size;uint64_t skip=(uint64_t)a->chunk_ordinal*a->packet_size;
            if(skip>a->mdat_end-a->packet_offset)return bad(a,LN_M4A_FORMAT);a->packet_offset+=skip;
        }
        else{
            /* Sequential packets reuse the preceding end offset. Seeking scans
             * at most 256 sizes per call, keeping large single-chunk files bounded. */
            uint32_t count=a->chunk_ordinal+1-a->size_scan;if(count>256)count=256;
            int result=read_bytes(a,a->sizes_offset+(uint64_t)(a->chunk_first+a->size_scan)*4,count*4);if(result!=1)return result;
            for(uint32_t i=0;i<count;i++){
                uint32_t size=ln_read_be32(a->buffer+i*4);if(!size||size>32768)return bad(a,LN_M4A_UNSUPPORTED);
                if(size>a->mdat_end-a->packet_offset)return bad(a,LN_M4A_FORMAT);
                if(a->size_scan+i==a->chunk_ordinal)a->packet_size=size;else a->packet_offset+=size;
            }
            a->size_scan+=count;if(a->size_scan<=a->chunk_ordinal)return 0;
        }
        if(a->packet_offset<a->mdat_start||a->packet_offset>a->mdat_end||a->packet_size>a->mdat_end-a->packet_offset)return bad(a,LN_M4A_FORMAT);
        if(a->codec==17&&a->packet_size>8191)return bad(a,LN_M4A_UNSUPPORTED);
        a->decode_phase=3;
    }
    int result=read_bytes(a,a->packet_offset,a->packet_size);if(result!=1)return result;
    a->current_packet_frames=packet_frames(a,a->packet);a->packet++;
    a->next_packet=a->packet;a->next_chunk=a->chunk_index;
    a->next_offset=a->packet_offset+a->packet_size;a->next_valid=true;
    a->decode_phase=0;return 1;
}
bool ln_m4a_seek(struct ln_m4a *a,uint64_t frame)
{
    if(!a||!a->opened||a->pending||a->error||frame>a->total_frames)return false;
    uint64_t remaining=frame;uint32_t packet=0;
    for(uint32_t i=0;i<a->time_count;i++){
        uint64_t span=(uint64_t)a->times[i].count*a->times[i].delta;
        if(remaining<span){uint32_t residue;packet+=divide_small((uint32_t)remaining,a->times[i].delta,&residue);remaining=residue;break;}
        remaining-=span;packet+=a->times[i].count;
    }
    a->packet=packet;a->skip=(uint32_t)remaining;a->decode_phase=0;a->read_total=0;return true;
}
bool ln_m4a_seek_indexed(struct ln_m4a *a,uint32_t packet,uint64_t offset){
    if(!a||a->codec!=17||packet>=a->packets||!media_extent(a,offset)||
       !ln_m4a_seek(a,(uint64_t)packet*1024u)||a->packet!=packet||!locate(a))return false;
    a->next_valid=true;a->next_packet=packet;a->next_chunk=a->chunk_index;a->next_offset=offset;
    return true;
}
