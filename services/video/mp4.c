#include "boot/include/linnet_bytes.h"
#include "linnet_target.h"
#include "services/video/mp4.h"
#define TAG(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))
static int bad(struct ln_mp4 *m,int error){return m->error=error;}
static int read_at(struct ln_mp4 *m,uint64_t at,uint32_t bytes){
    if(bytes>sizeof m->buffer||at>m->bytes||bytes>m->bytes-at)return bad(m,LN_MP4_FORMAT);
    int result=m->read(m->context,at,m->buffer,bytes);
    return result<0?bad(m,LN_MP4_IO):result;
}
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t table_attempts,table_completed,table_pending,table_failed,table_bytes,table_kinds[4],table_seeks;
static void table_add(uint32_t *value,uint32_t amount){
    *value=UINT32_MAX-*value<amount?UINT32_MAX:*value+amount;
}
void ln_mp4_table_diagnostics(uint32_t output[12]){
    output[0]=0x4254504du;output[1]=table_attempts;output[2]=table_completed;
    output[3]=table_pending;output[4]=table_failed;output[5]=table_bytes;
    for(uint32_t i=0;i<4u;++i)output[6+i]=table_kinds[i];
    output[10]=table_seeks;output[11]=0;
}
#endif
static int read_table(struct ln_mp4 *m,uint64_t at,uint32_t bytes,uint32_t kind){
    int result=read_at(m,at,bytes);
#if !defined(LINNET_PROFILE_RELEASE)
    table_add(&table_attempts,1u);
    if(result>0){table_add(&table_completed,1u);table_add(&table_bytes,bytes);table_add(&table_kinds[kind],1u);}
    else if(!result)table_add(&table_pending,1u);
    else table_add(&table_failed,1u);
#else
    (void)kind;
#endif
    return result;
}
void ln_mp4_init(struct ln_mp4 *m,ln_mp4_read_fn read,void *context,uint64_t bytes){
    *m=(struct ln_mp4){0};m->read=read;m->context=context;m->bytes=bytes;m->ends[0]=bytes;
}
void ln_mp4_init_audio(struct ln_mp4 *m,ln_mp4_read_fn read,void *context,uint64_t bytes){ln_mp4_init(m,read,context,bytes);m->audio=true;}
static bool once(struct ln_mp4 *m,uint32_t bit){if(m->seen&bit)return false;m->seen|=bit;return true;}
static bool descriptors(struct ln_mp4 *m,const uint8_t *p,uint32_t bytes,uint32_t depth){
    if(depth>4u)return false;
    while(bytes){
        uint32_t tag=*p++,length=0,n=0;--bytes;
        uint8_t v;
        do{if(!bytes||n++==4u)return false;v=*p++;--bytes;length=(length<<7)|(v&127u);}while(v&128u);
        if(length>bytes)return false;
        uint32_t skip=0;
        if(tag==3u){
            if(length<3u)return false;skip=3u;
            if(p[2]&128u)skip+=2u;
            if(p[2]&64u){if(skip>=length)return false;skip+=1u+p[skip];}
            if(p[2]&32u)skip+=2u;
        }else if(tag==4u)skip=13u;
        else if(tag==5u){
            if(!length||length>sizeof m->config||m->config_bytes)return false;
            for(uint32_t i=0;i<length;++i)m->config[i]=p[i];m->config_bytes=length;
        }
        if(skip&&(skip>length||!descriptors(m,p+skip,length-skip,depth+1u)))return false;
        p+=length;bytes-=length;
    }
    return true;
}
static bool audio_config(struct ln_mp4 *m,const uint8_t *p,uint32_t bytes,unsigned depth){
    if(depth>3)return false;
    while(bytes){
        if(bytes<8)return false;uint32_t size=ln_read_be32(p),type=ln_read_be32(p+4);
        if(size<8||size>bytes)return false;
        if(type==TAG('e','s','d','s')){if(size<12||!descriptors(m,p+12,size-12,0))return false;}
        else if(type==TAG('w','a','v','e')&&!audio_config(m,p+8,size-8,depth+1))return false;
        p+=size;bytes-=size;
    }return true;
}
static int body(struct ln_mp4 *m){
    const uint8_t *b=m->buffer;uint32_t n=m->body_bytes;
    switch(m->type){
    case TAG('h','d','l','r'):
        /* QuickTime also places a data-reference handler inside minf. */
        if(m->parents[m->depth]!=TAG('m','d','i','a'))break;
        if(n<12u)return bad(m,LN_MP4_FORMAT);
        if(ln_read_be32(b+8)!=(m->audio?TAG('s','o','u','n'):TAG('v','i','d','e'))){
            for(uint32_t i=m->depth;i;i--)if(m->parents[i]==TAG('t','r','a','k')){m->cursor=m->ends[i];m->depth=i;m->phase=0;return 0;}
            return bad(m,LN_MP4_FORMAT);
        }
        break;
    case TAG('e','l','s','t'):
        if(n<20u||b[0]||ln_read_be32(b+4)!=1u||ln_read_be32(b+16)!=0x10000u||(ln_read_be32(b+12)&0x80000000u))return bad(m,LN_MP4_UNSUPPORTED);
        m->lead=ln_read_be32(b+12);break;
    case TAG('m','d','h','d'):
        if(n<24u||!once(m,1u))return bad(m,LN_MP4_FORMAT);
        if(b[0]==0u){m->timescale=ln_read_be32(b+12);m->duration=ln_read_be32(b+16);}
        else if(b[0]==1u&&n>=32u){m->timescale=ln_read_be32(b+20);m->duration=ln_read_be64(b+24);}
        else return bad(m,LN_MP4_UNSUPPORTED);
        break;
    case TAG('s','t','s','d'):{
        if(n<16u||ln_read_be32(b+4)!=1u)return bad(m,LN_MP4_UNSUPPORTED);
        uint32_t codec=ln_read_be32(b+12),entry=ln_read_be32(b+8);
        if(m->audio){
            if(codec!=TAG('m','p','4','a'))return bad(m,LN_MP4_UNSUPPORTED);
            if(!once(m,2u)||entry<36u||entry>n-8u)return bad(m,LN_MP4_UNSUPPORTED);
            uint32_t version=ln_read_be16(b+24),children=version==0?44u:version==1?60u:0u;
            if(!children||entry+8u<children)return bad(m,LN_MP4_UNSUPPORTED);
            m->video=true;m->codec=codec;m->channels=ln_read_be16(b+32);m->rate=ln_read_be32(b+40)>>16;
            if(m->channels<1u||m->channels>2u||!m->rate)return bad(m,LN_MP4_UNSUPPORTED);
            if(!audio_config(m,b+children,entry+8u-children,0))return bad(m,LN_MP4_FORMAT);
            break;
        }
        if(codec!=TAG('a','v','c','1')&&codec!=TAG('m','p','4','v'))break;
        if(!once(m,2u)||entry<86u||entry>n-8u)return bad(m,LN_MP4_FORMAT);
        m->video=true;m->codec=codec;m->width=ln_read_be16(b+40);m->height=ln_read_be16(b+42);
        for(uint32_t at=94u;at<entry+8u;){
            if(entry+8u-at<8u)return bad(m,LN_MP4_FORMAT);
            uint32_t bytes=ln_read_be32(b+at),type=ln_read_be32(b+at+4);
            if(bytes<8u||bytes>entry+8u-at)return bad(m,LN_MP4_FORMAT);
            if(type==TAG('a','v','c','C')&&codec==TAG('a','v','c','1')){
                if(bytes<15u||bytes-8u>sizeof m->config||m->config_bytes)return bad(m,LN_MP4_UNSUPPORTED);
                m->config_bytes=bytes-8u;
                for(uint32_t i=0;i<m->config_bytes;++i)m->config[i]=b[at+8u+i];
                if(m->config[0]!=1u)return bad(m,LN_MP4_FORMAT);
                m->nal_bytes=(m->config[4]&3u)+1u;
            }else if(type==TAG('e','s','d','s')&&codec==TAG('m','p','4','v')){
                if(bytes<12u||!descriptors(m,b+at+12u,bytes-12u,0))return bad(m,LN_MP4_FORMAT);
            }
            at+=bytes;
        }
        break;}
    case TAG('s','t','s','z'):
        if(n<12u||!once(m,4u))return bad(m,LN_MP4_FORMAT);
        m->default_bytes=ln_read_be32(b+4);m->samples=ln_read_be32(b+8);m->sizes=m->cursor+12u;
        if(!m->samples||m->samples>1000000u||(!m->default_bytes&&(uint64_t)m->samples*4u>m->atom_end-m->sizes))return bad(m,LN_MP4_UNSUPPORTED);
        break;
    case TAG('s','t','c','o'):case TAG('c','o','6','4'):
        if(n<8u||!once(m,8u))return bad(m,LN_MP4_FORMAT);
        m->chunk_count=ln_read_be32(b+4);m->chunks=m->cursor+8u;m->chunk_width=m->type==TAG('c','o','6','4')?8u:4u;
        if(!m->chunk_count||m->chunk_count>1000000u||(uint64_t)m->chunk_count*m->chunk_width>m->atom_end-m->chunks)return bad(m,LN_MP4_UNSUPPORTED);
        break;
    case TAG('s','t','s','c'):
        if(n<8u||!once(m,16u))return bad(m,LN_MP4_FORMAT);
        m->map_count=ln_read_be32(b+4);
        m->maps=m->cursor+8u;
        if(!m->map_count||m->map_count>1000000u||(uint64_t)m->map_count*12u!=m->atom_end-m->maps)return bad(m,LN_MP4_UNSUPPORTED);
        break;
    case TAG('s','t','t','s'):
        if(n<8u||!once(m,32u))return bad(m,LN_MP4_FORMAT);
        m->time_count=ln_read_be32(b+4);
        if(!m->time_count||m->time_count>128u||n!=8u+8u*m->time_count)return bad(m,LN_MP4_UNSUPPORTED);
        for(uint32_t i=0;i<m->time_count;++i){m->times[i].count=ln_read_be32(b+8u+i*8u);m->times[i].delta=ln_read_be32(b+12u+i*8u);if(!m->times[i].count||!m->times[i].delta)return bad(m,LN_MP4_FORMAT);}
        break;
    case TAG('s','t','s','s'):
        if(n<8u||!once(m,64u))return bad(m,LN_MP4_FORMAT);
        m->sync_count=ln_read_be32(b+4);m->sync=m->cursor+8u;
        if((uint64_t)m->sync_count*4u>m->atom_end-m->sync)return bad(m,LN_MP4_FORMAT);
        break;
    case TAG('c','t','t','s'):
        /* Baseline streams have no reordered presentation timestamps. */
        if(n<8u||ln_read_be32(b+4)>128u||n!=8u+ln_read_be32(b+4)*8u)return bad(m,LN_MP4_UNSUPPORTED);
        for(uint32_t at=8u;at<n;at+=8u)if(ln_read_be32(b+at+4u))return bad(m,LN_MP4_UNSUPPORTED);
        break;
    default:break;
    }
    m->cursor=m->atom_end;m->phase=0;return 0;
}
static int map_run(struct ln_mp4 *m,uint32_t index){
    if(index>=m->map_count)return bad(m,LN_MP4_FORMAT);
    int r=read_table(m,m->maps+(uint64_t)index*12u,index+1u<m->map_count?24u:12u,0u);if(r<=0)return r;
    uint32_t first=ln_read_be32(m->buffer),count=ln_read_be32(m->buffer+4),end=index+1u<m->map_count?ln_read_be32(m->buffer+12):m->chunk_count+1u;
    if(!first||(!index&&first!=1u)||first>=end||end>m->chunk_count+1u||!count||count>65536u||ln_read_be32(m->buffer+8)!=1u)return bad(m,LN_MP4_FORMAT);
    m->map_index=index;m->map_first=first;m->map_end=end;m->map_samples=count;return 1;
}
static int finish(struct ln_mp4 *m){
    if((m->seen&63u)!=63u||!m->timescale||(!m->audio&&(!m->width||!m->height))||!m->config_bytes)return bad(m,LN_MP4_FORMAT);
    uint64_t count=0,duration=0;
    for(uint32_t i=0;i<m->time_count;++i){count+=m->times[i].count;duration+=(uint64_t)m->times[i].count*m->times[i].delta;}
    if(count!=m->samples||duration!=m->duration||m->lead>=duration||(m->audio&&m->rate!=m->timescale))return bad(m,LN_MP4_UNSUPPORTED);
    if(m->sync_count>m->samples)return bad(m,LN_MP4_FORMAT);
    int r=map_run(m,0);if(r<=0)return r;
    m->opened=true;m->phase=0;m->chunk_samples=m->map_samples;return 1;
}
int ln_mp4_open_step(struct ln_mp4 *m){
    if(!m||!m->read)return LN_MP4_FORMAT;if(m->error)return m->error;if(m->opened)return 1;
    if(m->phase==2u)return finish(m);
    if(m->phase){int result=read_at(m,m->cursor,m->body_bytes);return result>0?body(m):result;}
    while(m->depth&&m->cursor==m->ends[m->depth]){
        uint32_t type=m->parents[m->depth--];
        if(type==TAG('t','r','a','k')&&m->video){m->phase=2u;return finish(m);}
    }
    if(m->cursor==m->bytes)return m->audio?LN_MP4_EOF:bad(m,LN_MP4_UNSUPPORTED);
    if(m->cursor>m->ends[m->depth]||m->ends[m->depth]-m->cursor<8u)return bad(m,LN_MP4_FORMAT);
    uint32_t header_read=m->ends[m->depth]-m->cursor>=16u?16u:8u;
    int result=read_at(m,m->cursor,header_read);if(result<=0)return result;
    uint64_t size=ln_read_be32(m->buffer);uint32_t header=8u,type=ln_read_be32(m->buffer+4);
    if(size==1u){if(header_read<16u)return bad(m,LN_MP4_FORMAT);size=ln_read_be64(m->buffer+8);header=16u;}
    else if(!size)size=m->ends[m->depth]-m->cursor;
    if(size<header||size>m->ends[m->depth]-m->cursor||++m->atoms>16384u)return bad(m,LN_MP4_FORMAT);
    m->atom_end=m->cursor+size;m->type=type;m->cursor+=header;
    if(type==TAG('m','o','o','f'))return bad(m,LN_MP4_UNSUPPORTED);
    if(type==TAG('m','o','o','v')||type==TAG('t','r','a','k')||type==TAG('m','d','i','a')||type==TAG('m','i','n','f')||type==TAG('s','t','b','l')||type==TAG('e','d','t','s')){
        if(m->depth==7u)return bad(m,LN_MP4_UNSUPPORTED);
        if(type==TAG('t','r','a','k')){if(++m->tracks>16u)return bad(m,LN_MP4_UNSUPPORTED);m->seen=0;m->video=false;m->config_bytes=0;m->sync_count=0;m->sync=0;m->nal_bytes=0;m->lead=0;}
        m->parents[++m->depth]=type;m->ends[m->depth]=m->atom_end;return 0;
    }
    uint32_t wanted=0;
    if(type==TAG('h','d','l','r'))wanted=12u;
    else if(type==TAG('e','l','s','t'))wanted=size-header>20u?20u:(uint32_t)(size-header);
    else if(type==TAG('m','d','h','d'))wanted=size-header>32u?32u:(uint32_t)(size-header);
    else if(type==TAG('s','t','s','d')||type==TAG('s','t','t','s')||type==TAG('c','t','t','s')){
        if(size-header>sizeof m->buffer)return bad(m,LN_MP4_UNSUPPORTED);wanted=(uint32_t)(size-header);
    }else if(type==TAG('s','t','s','z'))wanted=12u;
    else if(type==TAG('s','t','c','o')||type==TAG('c','o','6','4')||type==TAG('s','t','s','s')||type==TAG('s','t','s','c'))wanted=8u;
    if(!wanted){m->cursor=m->atom_end;return 0;}
    if(wanted>size-header)return bad(m,LN_MP4_FORMAT);
    m->body_bytes=wanted;m->phase=1;return 0;
}
int ln_mp4_next(struct ln_mp4 *m,struct ln_mp4_packet *out){
    if(!m||!out||!m->opened)return LN_MP4_FORMAT;if(m->error)return m->error;if(m->packet==m->samples){
        if(m->chunk+1u!=m->chunk_count||m->map_index+1u!=m->map_count||
           m->chunk_first+m->chunk_samples!=m->samples)return bad(m,LN_MP4_FORMAT);
        return LN_MP4_EOF;
    }
    int result;
    if(!m->packet_phase){
        if(m->packet==m->chunk_first+m->chunk_samples){
            m->chunk_first=m->packet;++m->chunk;
        }
        m->packet_phase=3u;
    }
    if(m->packet_phase==3u){
        if(m->chunk>=m->chunk_count)return bad(m,LN_MP4_FORMAT);
        if(m->chunk+1u>=m->map_end){result=map_run(m,m->map_index+1u);if(result<=0)return result;}
        if(m->chunk+1u<m->map_first)return bad(m,LN_MP4_FORMAT);
        m->chunk_samples=m->map_samples;
        if(m->packet==m->chunk_first){result=read_table(m,m->chunks+(uint64_t)m->chunk*m->chunk_width,m->chunk_width,1u);if(result<=0)return result;m->packet_offset=m->chunk_width==8u?ln_read_be64(m->buffer):ln_read_be32(m->buffer);}
        m->packet_phase=1;
    }
    if(m->packet_phase==1u){
        m->packet_bytes=m->default_bytes;
        if(!m->packet_bytes){result=read_table(m,m->sizes+(uint64_t)m->packet*4u,4u,2u);if(result<=0)return result;m->packet_bytes=ln_read_be32(m->buffer);}
        if(!m->packet_bytes||m->packet_bytes>LINNET_VIDEO_PACKET_BYTES||m->packet_offset>m->bytes||m->packet_bytes>m->bytes-m->packet_offset)return bad(m,LN_MP4_UNSUPPORTED);
        m->packet_phase=2;
    }
    if(m->sync_count&&m->sync_index<m->sync_count&&m->next_sync<=m->packet){
        result=read_table(m,m->sync+(uint64_t)m->sync_index*4u,4u,3u);if(result<=0)return result;
        uint32_t next=ln_read_be32(m->buffer);
        if(!next||next>m->samples||next<=m->next_sync)return bad(m,LN_MP4_FORMAT);
        m->next_sync=next;++m->sync_index;if(next<=m->packet)return 0;
    }
    uint32_t ordinal=m->packet,delta=0;
    for(uint32_t i=0;i<m->time_count;++i){if(ordinal<m->times[i].count){delta=m->times[i].delta;break;}ordinal-=m->times[i].count;}
    if(!delta)return bad(m,LN_MP4_FORMAT);
    *out=(struct ln_mp4_packet){m->packet_offset,m->decode_time,m->packet_bytes,delta,!m->sync_count||m->next_sync==m->packet+1u};
    m->decode_time+=delta;m->packet_offset+=m->packet_bytes;++m->packet;m->packet_phase=0;return 1;
}

bool ln_mp4_seek(struct ln_mp4 *m,uint64_t time,int direction){
    if(!m||!m->opened||m->error||m->packet_phase||m->seek_phase)return false;
#if !defined(LINNET_PROFILE_RELEASE)
    table_add(&table_seeks,1u);
#endif
    if(time>=m->duration)time=m->duration-1u;
    uint32_t packet=0;
    for(uint32_t i=0;i<m->time_count;++i){
        uint64_t span=(uint64_t)m->times[i].count*m->times[i].delta;
        if(time<span){
            /* ARMv4T has no runtime 64-bit division helper. */
            uint32_t count=0;uint64_t remainder=time;
            for(uint32_t bit=32u;bit;--bit){uint64_t value=(uint64_t)m->times[i].delta<<(bit-1u);if(remainder>=value){remainder-=value;count|=1u<<(bit-1u);}}
            packet+=count;break;
        }
        time-=span;packet+=m->times[i].count;
    }
    m->sync_index=m->next_sync=0;
    m->seek_packet=packet;m->seek_lo=0;m->seek_hi=m->sync_count;
    m->seek_map=m->seek_base=0;m->seek_forward=direction>0;m->seek_phase=direction&&m->sync_count?1u:3u;return true;
}
int ln_mp4_seek_step(struct ln_mp4 *m){
    if(!m||!m->opened)return LN_MP4_FORMAT;if(m->error)return m->error;
    if(!m->seek_phase)return LN_MP4_READY;
    if(m->seek_phase==1u){
        if(m->seek_lo==m->seek_hi){if(m->seek_forward&&m->seek_lo<m->sync_count)++m->seek_lo;if(!m->seek_lo)return bad(m,LN_MP4_FORMAT);m->seek_phase=2u;return 0;}
        uint32_t mid=m->seek_lo+(m->seek_hi-m->seek_lo)/2u;
        int r=read_table(m,m->sync+(uint64_t)mid*4u,4u,3u);if(r<=0)return r;
        uint32_t sample=ln_read_be32(m->buffer);if(!sample||sample>m->samples)return bad(m,LN_MP4_FORMAT);
        if(sample<m->seek_packet+1u||(!m->seek_forward&&sample==m->seek_packet+1u))m->seek_lo=mid+1u;else m->seek_hi=mid;return 0;
    }
    if(m->seek_phase==2u){
        int r=read_table(m,m->sync+(uint64_t)(m->seek_lo-1u)*4u,4u,3u);if(r<=0)return r;
        m->seek_packet=ln_read_be32(m->buffer)-1u;m->sync_index=m->seek_lo;m->next_sync=m->seek_packet+1u;m->seek_phase=3u;
    }
    if(m->seek_phase==3u){
        int r=map_run(m,m->seek_map);if(r<=0)return r;
        uint64_t count=(uint64_t)(m->map_end-m->map_first)*m->map_samples;
        if(count>m->samples-m->seek_base)return bad(m,LN_MP4_FORMAT);
        if(m->seek_packet-m->seek_base>=count){m->seek_base+=(uint32_t)count;++m->seek_map;return 0;}
        uint32_t relative=(m->seek_packet-m->seek_base)/m->map_samples;
        m->chunk=m->map_first-1u+relative;m->chunk_samples=m->map_samples;
        m->chunk_first=m->seek_base+relative*m->map_samples;m->packet=m->chunk_first;
        m->decode_time=0;uint32_t ordinal=m->packet;
        for(uint32_t i=0;i<m->time_count&&ordinal;++i){uint32_t count=ordinal<m->times[i].count?ordinal:m->times[i].count;m->decode_time+=(uint64_t)count*m->times[i].delta;ordinal-=count;}
        m->packet_phase=0;m->seek_phase=4u;
    }
    if(m->packet<m->seek_packet){struct ln_mp4_packet ignored;int r=ln_mp4_next(m,&ignored);return r<0?r:0;}
    m->seek_phase=0;return LN_MP4_READY;
}
