#include "boot/include/linnet_bytes.h"
#include "linnet_target.h"
#include "database.h"
#include "boot/include/linnet_boot.h"
static void copy(void *to,const void *from,uint32_t n){uint8_t *d=to;const uint8_t *s=from;for(uint32_t i=0;i<n;i++)d[i]=s[i];}
static int bad(struct ln_music_db *d,int error){d->error=error;d->busy=false;d->search_lower_valid=false;return error;}
static int read_bytes(struct ln_music_db *d,uint32_t offset,uint32_t count)
{
    if(offset>d->size||count>d->size-offset||count>sizeof(d->buffer))return bad(d,LN_DB_FORMAT);
    int r=d->read(d->context,offset,d->buffer,count);d->pending=r==0;
    return r<0?bad(d,LN_DB_IO):r;
}
bool ln_music_db_utf8(const uint8_t *s,uint32_t n)
{
    for(uint32_t i=0;i<n;){
        uint32_t c=s[i++],more,min;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){more=1;min=0x80;c&=31;}
        else if(c>=0xe0&&c<=0xef){more=2;min=0x800;c&=15;}
        else if(c>=0xf0&&c<=0xf4){more=3;min=0x10000;c&=7;}
        else return false;
        if(more>n-i)return false;
        while(more--){uint32_t b=s[i++];if((b&0xc0)!=0x80)return false;c=(c<<6)|(b&63);}
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    }return true;
}
void ln_music_db_init(struct ln_music_db *d,ln_music_db_read_fn read,void *context,uint32_t size,uint32_t generation)
{
    uint8_t *p=(uint8_t *)d;for(uint32_t i=0;i<sizeof(*d);i++)p[i]=0;
    d->read=read;d->context=context;d->size=size;d->generation=generation;
}
static bool section(uint32_t *cursor,uint32_t offset,uint32_t count,uint32_t width,uint32_t limit)
{
    uint64_t bytes=(uint64_t)count*width;
    if(offset!=*cursor||offset>limit||bytes>limit-offset)return false;
    *cursor+=(uint32_t)bytes;return true;
}
int ln_music_db_open_step(struct ln_music_db *d)
{
    if(!d||!d->read)return LN_DB_ARGUMENT;
    if(d->error)return d->error;
    if(d->verified)return 1;
    if(d->size<129||d->size>134217728u)return bad(d,LN_DB_FORMAT);
    int r;uint8_t *b=d->buffer;
    if(d->phase==0){
        r=read_bytes(d,0,128);if(r!=1)return r;
        if(b[0]!='P'||b[1]!='O'||b[2]!='D'||b[3]!='B'||ln_read_le16(b+4)!=1||ln_read_le16(b+6)!=128||
           ln_read_le32(b+8)!=d->size||ln_read_le32(b+12)||ln_crc32(b,124)!=ln_read_le32(b+124))return bad(d,LN_DB_FORMAT);
        for(uint32_t i=96;i<124;i++)if(b[i])return bad(d,LN_DB_FORMAT);
        d->tracks=ln_read_le32(b+16);d->groups=ln_read_le32(b+20);d->members=ln_read_le32(b+24);d->searches=ln_read_le32(b+28);
        d->track_offset=ln_read_le32(b+32);d->group_offset=ln_read_le32(b+36);d->member_offset=ln_read_le32(b+40);
        d->search_offset=ln_read_le32(b+44);d->pool_offset=ln_read_le32(b+48);d->pool_bytes=ln_read_le32(b+52);
        d->arts=ln_read_le32(b+56);d->art_offset=ln_read_le32(b+60);
        uint32_t cursor=128;
        if(d->tracks>100000u||d->searches<d->tracks||
           d->searches-d->tracks>d->groups||!d->pool_bytes||
           !section(&cursor,d->track_offset,d->tracks,LN_MUSIC_TRACK_BYTES,d->size)||
           !section(&cursor,d->group_offset,d->groups,32,d->size)||
           !section(&cursor,d->member_offset,d->members,4,d->size)||
           !section(&cursor,d->search_offset,d->searches,8,d->size)||
           !section(&cursor,d->art_offset,d->arts,32,d->size)||
           !section(&cursor,d->pool_offset,d->pool_bytes,1,d->size)||cursor!=d->size)return bad(d,LN_DB_FORMAT);
        copy(d->expected_hash,b+64,32);ln_sha256_init(&d->hash);d->cursor=128;d->phase=1;return 0;
    }
    if(d->phase==1){
        /* Finish the header's page, then verify whole aligned pages so FAT
         * can use direct sector batches instead of two partial-sector copies. */
        uint32_t limit=4096u-(d->cursor&4095u);
        uint32_t bytes=d->size-d->cursor;if(bytes>limit)bytes=limit;
        r=read_bytes(d,d->cursor,bytes);if(r!=1)return r;
        if(d->cursor<=d->pool_offset&&d->pool_offset-d->cursor<bytes&&b[d->pool_offset-d->cursor])return bad(d,LN_DB_FORMAT);
        if(bytes==d->size-d->cursor&&b[bytes-1])return bad(d,LN_DB_FORMAT);
        ln_sha256_update(&d->hash,b,bytes);d->cursor+=bytes;
        if(d->cursor!=d->size)return 0;
        uint8_t digest[32];ln_sha256_finish(&d->hash,digest);
        for(uint32_t i=0;i<32;i++)if(digest[i]!=d->expected_hash[i])return bad(d,LN_DB_FORMAT);
        d->verified=true;d->phase=2;return 1;
    }return bad(d,LN_DB_FORMAT);
}
static bool string_start(struct ln_music_db *d,uint32_t offset)
{
    if(offset>=d->pool_bytes)return false;
    d->string_offset=offset;d->read_offset=d->pool_offset+offset-(offset?1u:0u);
    d->read_size=d->size-d->read_offset;if(d->read_size>257)d->read_size=257;
    return true;
}
static int string_step(struct ln_music_db *d)
{
    int r=read_bytes(d,d->read_offset,d->read_size);if(r!=1)return r;
    uint32_t skip=d->string_offset?1u:0u;
    if(skip&&d->buffer[0])return bad(d,LN_DB_FORMAT);
    uint32_t n=0;while(n<d->read_size-skip&&d->buffer[n+skip])n++;
    if(n>255||n==d->read_size-skip||!ln_music_db_utf8(d->buffer+skip,n))return bad(d,LN_DB_FORMAT);
    copy(d->text,d->buffer+skip,n);d->text[n]=0;return 1;
}
bool ln_music_db_request(struct ln_music_db *d,enum ln_music_db_request operation,uint32_t index)
{
    if(!d||!d->verified||d->busy||d->error)return false;
    uint32_t offset,width;
    switch(operation){
    case LN_DB_TRACK:if(index>=d->tracks)return false;offset=d->track_offset;width=LN_MUSIC_TRACK_BYTES;break;
    case LN_DB_GROUP:if(index>=d->groups)return false;offset=d->group_offset;width=32;break;
    case LN_DB_MEMBER:if(index>=d->members)return false;offset=d->member_offset;width=4;break;
    case LN_DB_ART:if(index>=d->arts)return false;offset=d->art_offset;width=32;break;
    case LN_DB_STRING:if(!string_start(d,index))return false;offset=d->read_offset;width=d->read_size;index=0;break;
    default:return false;
    }
    d->index=index;d->operation=(uint8_t)operation;d->read_offset=offset+index*width;d->read_size=width;d->busy=true;return true;
}
static bool track_record(struct ln_music_db *d)
{
    uint8_t *b=d->buffer;struct ln_music_track *t=&d->track;
    t->id=ln_read_le64(b);t->file_bytes=ln_read_le64(b+8);t->data_offset=ln_read_le64(b+16);t->data_bytes=ln_read_le64(b+24);t->frames=ln_read_le64(b+32);
    t->rate=ln_read_le32(b+40);t->channels=ln_read_le16(b+44);t->bits=ln_read_le16(b+46);t->codec=ln_read_le32(b+48);
    t->title=ln_read_le32(b+52);t->artist=ln_read_le32(b+56);t->album=ln_read_le32(b+60);t->path=ln_read_le32(b+64);t->genre=ln_read_le32(b+68);t->composer=ln_read_le32(b+72);
    t->year=ln_read_le32(b+76);t->number=ln_read_le32(b+80);t->disc=ln_read_le32(b+84);t->art=ln_read_le32(b+88);t->replaygain=(int32_t)ln_read_le32(b+92);copy(t->sha256,b+96,32);
    t->album_replaygain=(int32_t)ln_read_le32(b+128);
    if(t->replaygain!=INT32_MIN&&(t->replaygain< -60*65536||t->replaygain>60*65536))return false;
    if(t->album_replaygain!=INT32_MIN&&(t->album_replaygain< -60*65536||t->album_replaygain>60*65536))return false;
    for(uint32_t i=52;i<=72;i+=4)if(ln_read_le32(b+i)>=d->pool_bytes)return false;
    if(!t->rate||t->rate>384000||!t->channels||t->channels>8||t->data_offset>t->file_bytes||
       t->data_bytes>t->file_bytes-t->data_offset||(t->art!=UINT32_MAX&&t->art>=d->arts))return false;
    if(t->codec>=1&&t->codec<=4){
        if(t->codec>=3?t->bits!=32:(t->bits!=8&&t->bits!=16&&t->bits!=24&&t->bits!=32))return false;
        if(t->frames>(UINT64_MAX>>5)||t->frames*(t->channels*(t->bits>>3))!=t->data_bytes)return false;
    }else if(t->codec<16||t->codec>22||t->data_offset||t->data_bytes!=t->file_bytes)return false;
    return true;
}
static int prefix_compare(const char *key,const char *prefix)
{
    for(uint32_t i=0;prefix[i];i++){
        if((uint8_t)key[i]<(uint8_t)prefix[i])return -1;
        if((uint8_t)key[i]>(uint8_t)prefix[i])return 1;
    }return 0;
}
bool ln_music_db_search_at(struct ln_music_db *d,const char *prefix,uint32_t ordinal)
{
    if(!d||!prefix||!d->verified||d->busy||d->error)return false;
    uint32_t n=0;while(n<256&&prefix[n])n++;if(n==256||!ln_music_db_utf8((const uint8_t *)prefix,n))return false;
    d->search_prefix_length=n;
    bool cached=d->search_lower_valid,extension=d->search_lower_valid;
    /* An extended prefix cannot sort before its completed shorter prefix.
     * Compare before replacing the stored query; deletion/replacement resets. */
    for(uint32_t i=0;extension&&d->prefix[i];i++){
        if(i>=n){extension=false;break;}
        char folded=(prefix[i]>='A'&&prefix[i]<='Z')?(char)(prefix[i]+32):prefix[i];
        if(d->prefix[i]!=folded)extension=false;
    }
    for(uint32_t i=0;i<=n;i++){
        char folded=(prefix[i]>='A'&&prefix[i]<='Z')?(char)(prefix[i]+32):prefix[i];
        if(d->prefix[i]!=folded)cached=false;
        d->prefix[i]=folded;
    }
    if(!cached)d->search_lower_valid=false;
    d->search_ordinal=ordinal;
    d->low=extension?d->search_lower:0;d->high=cached?d->search_lower:d->searches;
    d->query_phase=0;d->operation=6;d->busy=true;d->search_index=UINT32_MAX;d->search_track=UINT32_MAX;return true;
}
static int search_step(struct ln_music_db *d)
{
    bool cached_title=false;
    if(d->query_phase==0){
        if(d->low==d->high){
            if(d->low==d->searches||d->search_ordinal>=d->searches-d->low){
                d->search_lower=d->low;d->search_lower_valid=true;d->busy=false;return 1;
            }
            d->middle=d->low+d->search_ordinal;d->query_phase=3;}
        else{d->middle=d->low+((d->high-d->low)>>1);d->query_phase=1;}
    }
    if(d->query_phase==3&&d->search_cache_valid&&
       d->search_prefix_length<=(uint32_t)sizeof(d->search_cache_key)-1u&&
       d->search_cache_key[sizeof(d->search_cache_key)-1u]==0&&
       d->search_cache_middle==d->middle){
        d->member=d->search_cache_member;copy(d->text,d->search_cache_key,sizeof(d->search_cache_key));
        d->query_phase=4;cached_title=true;
    }else if(d->query_phase==1||d->query_phase==3){
        int r=read_bytes(d,d->search_offset+d->middle*8u,8);if(r!=1)return r;
        d->member=ln_read_le32(d->buffer);
        bool group=(d->member&LN_MUSIC_SEARCH_GROUP)!=0;
        if((group?(d->member&~LN_MUSIC_SEARCH_GROUP)>=d->groups:d->member>=d->tracks)||
           !string_start(d,ln_read_le32(d->buffer+4)))return bad(d,LN_DB_FORMAT);
        d->query_phase++;return 0;
    }
    int r=cached_title?1:string_step(d);if(r!=1)return r;
    if(!cached_title){
        uint32_t i=0;
        for(;i<sizeof(d->search_cache_key);++i){
            d->search_cache_key[i]=d->text[i];
            if(!d->text[i]){++i;break;}
        }
        for(;i<sizeof(d->search_cache_key);++i)d->search_cache_key[i]=0;
        d->search_cache_middle=d->middle;d->search_cache_member=d->member;d->search_cache_valid=true;
    }
    int compare=prefix_compare(d->text,d->prefix);
    if(d->query_phase==4){
        if(!compare){d->search_index=d->middle;d->search_track=d->member;}
        /* Publish only after all admitted reads complete. A stale UI query
         * may finish and be discarded; it never changes an in-flight key. */
        d->search_lower=d->low;d->search_lower_valid=true;d->busy=false;return 1;
    }
    if(compare<0)d->low=d->middle+1;else d->high=d->middle;
    d->query_phase=0;return 0;
}
int ln_music_db_step(struct ln_music_db *d)
{
    if(!d)return LN_DB_ARGUMENT;if(d->error)return d->error;if(!d->busy)return LN_DB_ARGUMENT;
    if(d->operation==6)return search_step(d);
    int r=d->operation==LN_DB_STRING?string_step(d):read_bytes(d,d->read_offset,d->read_size);if(r!=1)return r;
    uint8_t *b=d->buffer;
    if(d->operation==LN_DB_TRACK){if(!track_record(d))return bad(d,LN_DB_FORMAT);}
    else if(d->operation==LN_DB_GROUP){
        struct ln_music_group *g=&d->group;g->kind=ln_read_le32(b);g->name=ln_read_le32(b+4);g->parent=ln_read_le32(b+8);g->first=ln_read_le32(b+12);g->count=ln_read_le32(b+16);g->id=ln_read_le64(b+24);
        if(g->kind<1||g->kind>8||g->name>=d->pool_bytes||g->first>d->members||g->count>d->members-g->first||ln_read_le32(b+20)||
           (g->kind==2?(g->parent>=d->groups||g->parent==d->index):g->parent!=UINT32_MAX))return bad(d,LN_DB_FORMAT);
    }else if(d->operation==LN_DB_MEMBER){d->member=ln_read_le32(b);if(d->member>=d->tracks)return bad(d,LN_DB_FORMAT);}
    else if(d->operation==LN_DB_ART){
        d->art.id=ln_read_le64(b);d->art.path=ln_read_le32(b+8);d->art.crc=ln_read_le32(b+24);
        uint32_t side=ln_read_le16(b+12);
        if(d->art.path>=d->pool_bytes||
#if LINNET_ART_MAX_SIDE == 160u
           (side!=64u&&side!=128u&&side!=160u)||
#elif LINNET_ART_MAX_SIDE == 128u
           (side!=64u&&side!=128u)||
#else
           side!=64u||
#endif
           ln_read_le16(b+14)!=side||ln_read_le32(b+28))return bad(d,LN_DB_FORMAT);
        uint32_t encoding=ln_read_le32(b+16),bytes=ln_read_le32(b+20);
        if(encoding==1u){if(bytes!=side*side*2u)return bad(d,LN_DB_FORMAT);}
#if LINNET_ART_MAX_SIDE >= 160u
        else if(encoding==2u){if(side!=160u||(bytes!=76288u
#if LINNET_TARGET_NUMBER == 1238u
            &&bytes!=108040u
#endif
            ))return bad(d,LN_DB_FORMAT);}
#endif
        else return bad(d,LN_DB_FORMAT);
    }
    d->busy=false;return 1;
}
bool ln_music_db_safe_path(const char *path)
{
    if(!path||path[0]!='/')return false;
    uint32_t i=1,components=0;
    if(path[1]=='.'){
        static const char prefix[]="/.LINNET/";
        for(uint32_t j=0;j<sizeof prefix-1u;++j)if(path[j]!=prefix[j])return false;
        i=sizeof prefix-1u;components=1;
        if(!path[i])return false;
    }
    while(path[i]){
        uint32_t name=0,ext=0;bool dot=false;
        while(path[i]&&path[i]!='/'){
            char c=path[i++];if(i>255)return false;
            if(c=='.'){if(dot||!name)return false;dot=true;continue;}
            if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='~'||c=='!'||c=='#'||c=='$'||c=='%'||c=='&'||c=='\''||c=='('||c==')'||c=='@'||c=='^'||c=='`'||c=='{'||c=='}'))return false;
            if(dot){if(++ext>3)return false;}else if(++name>8)return false;
        }
        if(!name||(dot&&!ext)||++components>16)return false;
        if(path[i]=='/'){i++;if(!path[i])return false;}
    }return components>0;
}
