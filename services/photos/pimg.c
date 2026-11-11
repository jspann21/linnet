#include "boot/include/linnet_bytes.h"
#include "pimg.h"
#include "boot/include/linnet_boot.h"
static bool section(uint32_t *at,uint32_t offset,uint32_t count,uint32_t width,uint32_t limit){
    if(offset!=*at||offset>limit||count>(limit-offset)/width)return false;*at+=count*width;return true;
}
uint32_t ln_pimg_crc_update(uint32_t crc,const uint8_t*p,uint32_t n){
    return ln_crc32_update(crc,p,n);
}
__attribute__((minsize)) bool ln_pimg_header(const uint8_t*b,uint32_t size,struct ln_pimg_header*h){
    if(!b||!h||size<512||size>0x7fffffffu||b[0]!='L'||b[1]!='P'||b[2]!='I'||b[3]!='C'||
#if LINNET_TARGET_NUMBER==98u
       ln_read_le16(b+4)!=0||!ln_bytes_zero(b+72,436u)||
#else
       ln_read_le16(b+4)!=1||!ln_bytes_zero(b+456,52u)||
#endif
       ln_read_le16(b+6)!=512||ln_read_le32(b+8)||ln_read_le32(b+12)!=size||
       ln_read_le16(b+64)!=64||ln_read_le16(b+66)!=64||ln_read_le16(b+68)!=32||ln_read_le16(b+70)!=1||
       ln_crc32(b,508)!=ln_read_le32(b+508))return false;
    *h=(struct ln_pimg_header){size,ln_read_le32(b+16),ln_read_le32(b+20),ln_read_le32(b+24),ln_read_le32(b+28),ln_read_le32(b+32),ln_read_le32(b+36),ln_read_le32(b+40),ln_read_le32(b+44),ln_read_le32(b+48),ln_read_le32(b+52),ln_read_le32(b+56),ln_read_le32(b+60),ln_read_le32(b+508)
#if LINNET_TARGET_NUMBER!=98u
       ,b+72
#endif
    };
    if(!h->albums||h->albums>4096||h->photos>65535||h->levels<h->photos||h->levels>h->photos*6u||h->tiles>1000000u||!h->string_bytes||h->string_bytes>16777216u||h->data_at%512u)return false;
    uint32_t at=512;
    if(!section(&at,h->album_at,h->albums,32,size)||!section(&at,h->photo_at,h->photos,64,size)||
       !section(&at,h->level_at,h->levels,24,size)||!section(&at,h->tile_at,h->tiles,12,size)||
       !section(&at,h->string_at,h->string_bytes,1,size)||h->data_at<at||h->data_at-at>=512u)return false;
    at=h->data_at;
    if(!section(&at,h->data_at,h->photos,8192,size))return false;
#if LINNET_TARGET_NUMBER==98u
    return h->tiles<=size-at&&size-at<=h->tiles*2048u;
#else
    if(at!=size)return false;
    for(uint32_t i=0;i<LN_PIMG_PACK_COUNT;++i){const uint8_t*entry=b+72u+i*12u;uint32_t bytes=ln_read_le32(entry+8u);
        if(bytes>LN_PIMG_PACK_LIMIT||(!bytes&&!ln_bytes_zero(entry,8u))||(bytes&&ln_bytes_zero(entry,8u)))return false;}
    return true;
#endif
}
static bool string_range(const struct ln_pimg_header*h,uint32_t at,uint32_t bytes,uint32_t max){return bytes<=max&&at<h->string_bytes&&bytes<h->string_bytes-at;}
__attribute__((minsize)) bool ln_pgrid_header(const uint8_t*b,uint32_t size,const uint8_t*binding,const struct ln_pimg_header*pim,uint32_t target,uint32_t width,uint32_t height,struct ln_pgrid_header*h){
    if(size<1024u||size>0x7fffffffu||b[0]!='L'||b[1]!='P'||b[2]!='G'||b[3]!='R'||
       ln_read_le16(b+4)!=2u||ln_read_le16(b+6)!=1024u||ln_read_le32(b+8)!=size||
       ln_read_le32(b+12)!=target||ln_read_le16(b+16)!=width||ln_read_le16(b+18)!=height||
       ln_read_le32(b+20)!=pim->photos||ln_read_le32(b+40)!=pim->size||
       !ln_bytes_zero(b+44,464u)||ln_crc32(b,508u)!=ln_read_le32(b+508u))return false;
    for(uint32_t i=0;i<512u;++i)if(b[512u+i]!=binding[i])return false;
    uint32_t prefix=pim->level_at-512u,strings=pim->data_at-pim->string_at;
    uint32_t checksums=pim->photos*4u;
    uint32_t data=(1024u+prefix+strings+checksums+511u)&~511u,stride=width*height*2u+4u;
    if(ln_read_le32(b+24)!=prefix||ln_read_le32(b+28)!=strings||ln_read_le32(b+32)!=data||
       data>size||pim->photos>(size-data)/stride||data+pim->photos*stride!=size)return false;
    *h=(struct ln_pgrid_header){prefix,strings,checksums,data,ln_read_le32(b+36),stride};return true;
}
__attribute__((minsize)) bool ln_pimg_album(const struct ln_pimg_header*h,const uint8_t*b,uint32_t index,struct ln_pimg_album*a){
    if(index>=h->albums||ln_crc32(b,28)!=ln_read_le32(b+28)||!ln_bytes_zero(b+20,8))return false;
    *a=(struct ln_pimg_album){ln_read_le32(b),ln_read_le32(b+4),ln_read_le32(b+8),ln_read_le32(b+12),ln_read_le32(b+16)};
    return a->bytes&&string_range(h,a->title,a->bytes,191)&&a->first<=h->photos&&a->count<=h->photos-a->first&&(index?a->parent<index:a->parent==UINT32_MAX);
}
__attribute__((minsize)) bool ln_pimg_photo(const struct ln_pimg_header*h,const uint8_t*b,uint32_t index,struct ln_pimg_photo*p){
    if(index>=h->photos||ln_crc32(b,60)!=ln_read_le32(b+60)||ln_read_le32(b+56))return false;
    *p=(struct ln_pimg_photo){ln_read_le32(b),ln_read_le32(b+4),ln_read_le32(b+8),ln_read_le32(b+12),ln_read_le32(b+16),ln_read_le32(b+20),ln_read_le32(b+24),ln_read_le32(b+28),ln_read_le32(b+32),ln_read_le32(b+36),ln_read_le32(b+40),ln_read_le32(b+44),ln_read_le32(b+48),ln_read_le32(b+52)};
    if(p->source_bytes?(!p->jpeg_bytes||p->jpeg_bytes>262144||!p->orientation||p->orientation>8||p->width>2048||p->height>2048||p->width*p->height>4000000u):(p->source||p->jpeg_bytes||p->jpeg_crc||p->orientation))return false;
    return p->album<h->albums&&p->bytes&&string_range(h,p->title,p->bytes,191)&&string_range(h,p->source,p->source_bytes,127)&&
        p->width&&p->height&&p->width<=16384&&p->height<=16384&&p->width*p->height<=32000000u&&p->levels&&p->levels<=6&&p->first<h->levels&&p->levels<=h->levels-p->first&&p->thumb==h->data_at+index*8192u;
}
__attribute__((minsize)) bool ln_pimg_level(const struct ln_pimg_header*h,const uint8_t*b,uint32_t index,struct ln_pimg_level*l){
    if(index>=h->levels||ln_crc32(b,20)!=ln_read_le32(b+20))return false;
    *l=(struct ln_pimg_level){ln_read_le32(b),ln_read_le16(b+4),ln_read_le16(b+6),ln_read_le16(b+8),ln_read_le16(b+10),ln_read_le32(b+12),ln_read_le32(b+16)};
    return l->photo<h->photos&&l->width&&l->height&&l->width<=2048&&l->height<=2048&&
        l->columns==(l->width+31u)/32u&&l->rows==(l->height+31u)/32u&&l->count==l->columns*l->rows&&l->first<h->tiles&&l->count<=h->tiles-l->first;
}
__attribute__((minsize)) bool ln_pimg_tile(const struct ln_pimg_header*h,const uint8_t*b,uint32_t index,uint32_t*offset,uint32_t*length,uint32_t*crc){
    if(index>=h->tiles)return false;*offset=ln_read_le32(b);*length=ln_read_le32(b+4);*crc=ln_read_le32(b+8);
#if LINNET_TARGET_NUMBER==98u
    uint32_t base=h->data_at+h->photos*8192u;
    return *offset>=base&&*offset<h->size&&*length>=1u&&*length<=2048u&&*length<=h->size-*offset;
#else
    uint32_t bucket=*offset>>27u,local=*offset&(LN_PIMG_PACK_LIMIT-1u),bytes=ln_read_le32(h->packs+bucket*12u+8u);
    return bytes&&local<bytes&&*length>=1u&&*length<=2048u&&*length<=bytes-local;
#endif
}
bool ln_pimg_decode_tile(const uint8_t*input,uint32_t bytes,uint8_t*output){
    if(!input||!output||!bytes||bytes>2048u)return false;
    if(bytes==2048u){for(uint32_t i=0;i<2048u;++i)output[i]=input[i];return true;}
    uint32_t src=0,dst=0;bool matched=false;
    while(src<bytes){
        uint32_t token=input[src++],literal=token>>4u;
        if(literal==15u){uint32_t more;do{if(src>=bytes)return false;more=input[src++];if(more>2048u-literal)return false;literal+=more;}while(more==255u);}
        if(literal>bytes-src||literal>2048u-dst)return false;
        for(uint32_t i=0;i<literal;++i)output[dst+i]=input[src+i];src+=literal;dst+=literal;
        if(src==bytes)return dst==2048u&&(literal>=5u||(!matched&&bytes<5u));
        if(bytes-src<2u)return false;
        uint32_t offset=(uint32_t)input[src]|((uint32_t)input[src+1u]<<8u);src+=2u;
        if(!offset||offset>dst)return false;
        uint32_t match=token&15u;
        if(match==15u){uint32_t more;do{if(src>=bytes)return false;more=input[src++];if(more>2048u-match)return false;match+=more;}while(more==255u);}
        if(match>2044u)return false;match+=4u;
        if(match>2048u-dst||2048u-dst<12u)return false;
        for(uint32_t i=0;i<match;++i)output[dst+i]=output[dst+i-offset];dst+=match;matched=true;
    }
    return false;
}
__attribute__((minsize)) bool ln_pimg_text(const uint8_t*s,uint32_t n,bool path){
    if(s[n]||(!n&&path))return false;
    for(uint32_t i=0;i<n;){uint32_t c=s[i++],more,min;
        if(!c||c<32||c==127)return false;
        if(path){if(c>126||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;continue;}
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){more=1;min=0x80;c&=31;}else if(c>=0xe0&&c<=0xef){more=2;min=0x800;c&=15;}else if(c>=0xf0&&c<=0xf4){more=3;min=0x10000;c&=7;}else return false;
        if(more>n-i)return false;while(more--){uint32_t x=s[i++];if((x&0xc0)!=0x80)return false;c=c<<6|(x&63);}
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    }
    if(path){if(s[0]!='/')return false;for(uint32_t i=0;i<n;i++)if(s[i]=='.'&&i+1<n&&s[i+1]=='.')return false;}
    return true;
}
