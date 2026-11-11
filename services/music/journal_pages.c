#include "boot/include/linnet_bytes.h"
#include "journal_pages.h"
#include "boot/include/linnet_boot.h"
static uint32_t crc(const uint8_t *p){return ln_crc32(p,4092);}
#if defined(LINNET_TARGET_VIDEO5G_M25)
/* Completed page buffers can be compared a word at a time. memcpy preserves
 * aliasing rules; unaligned inputs and every trailing byte retain byte checks. */
static bool bytes_equal(const uint8_t *a,const uint8_t *b,uint32_t bytes){
    if((((uintptr_t)a|(uintptr_t)b)&3u)==0u){
        a=__builtin_assume_aligned(a,4);b=__builtin_assume_aligned(b,4);
        while(bytes>=4u){uint32_t x,y;__builtin_memcpy(&x,a,4);__builtin_memcpy(&y,b,4);
            if(x!=y)return false;a+=4;b+=4;bytes-=4;}
    }
    return ln_bytes_equal(a,b,bytes);
}
static bool bytes_zero(const uint8_t *p,uint32_t bytes){
    if(((uintptr_t)p&3u)==0u){
        p=__builtin_assume_aligned(p,4);
        while(bytes>=4u){uint32_t word;__builtin_memcpy(&word,p,4);
            if(word)return false;p+=4;bytes-=4;}
    }
    return ln_bytes_zero(p,bytes);
}
/* Host-generated linear advancement by 4028 verified zero bytes, reflected
 * IEEE CRC32 polynomial 0xedb88320. The prefix retains the ordinary raw seed. */
static uint32_t empty_page_crc(const uint8_t *p){
    static const uint32_t zero_tail[32]={
        0x3a37d26eu,0x746fa4dcu,0xe8df49b8u,0x0acf9531u,
        0x159f2a62u,0x2b3e54c4u,0x567ca988u,0xacf95310u,
        0x8283a061u,0xde764683u,0x679d8b47u,0xcf3b168eu,
        0x45072b5du,0x8a0e56bau,0xcf6dab35u,0x45aa502bu,
        0x8b54a056u,0xcdd846edu,0x40c18b9bu,0x81831736u,
        0xd877282du,0x6b9f561bu,0xd73eac36u,0x750c5e2du,
        0xea18bc5au,0x0f407ef5u,0x1e80fdeau,0x3d01fbd4u,
        0x7a03f7a8u,0xf407ef50u,0x337ed8e1u,0x66fdb1c2u,
    };
    uint32_t raw=ln_crc32_update(UINT32_MAX,p,64),shifted=0;
    for(uint32_t bit=0;raw;raw>>=1,++bit)if(raw&1u)shifted^=zero_tail[bit];
    return shifted^UINT32_MAX;
}
#else
#define bytes_equal ln_bytes_equal
#define bytes_zero ln_bytes_zero
#endif
bool ln_history_superblock(const uint8_t *p,uint8_t epoch[16]){
    if(!p||!epoch||ln_read_le32(p)!=0x54534948u||ln_read_le32(p+4)!=0x00400000u||ln_read_le32(p+8)!=LN_HISTORY_PAGED_BANK_BYTES||
       ln_read_le32(p+12)!=1024||ln_read_le32(p+16)!=125||ln_read_le32(p+20)!=32||!bytes_zero(p+40,4052)||ln_read_le32(p+4092)!=crc(p))return false;
    for(uint32_t i=0;i<16;i++)epoch[i]=p[24+i];return true;
}
uint32_t ln_history_page_hash(uint64_t id){uint32_t x=(uint32_t)id^(uint32_t)(id>>32);x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x&1023u;}
static bool valid_record(const struct ln_music_journal_record *r){return r&&!(r->flags&~7u)&&((r->flags&2u)||!r->bookmark_frame)&&((r->flags&4u)||!r->resume_frame);}
static void record(const uint8_t *p,struct ln_music_journal_record *r){r->id=ln_read_le64(p);r->flags=ln_read_le32(p+8);r->play_count=ln_read_le32(p+12);r->bookmark_frame=ln_read_le64(p+16);r->resume_frame=ln_read_le64(p+24);}
static bool valid(const uint8_t *p,uint32_t index,const uint8_t epoch[16]){
    if(!p||ln_read_le32(p)!=0x45475048u||ln_read_le32(p+4)!=0x00400000u||ln_read_le32(p+8)!=index||ln_read_le32(p+16)>125||
       !bytes_equal(p+40,epoch,16)||!bytes_zero(p+56,8)
#if !defined(LINNET_TARGET_VIDEO5G_M25)
       ||ln_read_le32(p+4092)!=crc(p)
#endif
       )return false;
    uint32_t n=ln_read_le32(p+16);
#if defined(LINNET_TARGET_VIDEO5G_M25)
    if(!n){
        if(!bytes_zero(p+64,4028)||ln_read_le32(p+4092)!=empty_page_crc(p))return false;
    }else if(ln_read_le32(p+4092)!=crc(p))return false;
#endif
    for(uint32_t i=0;i<128;i++)if(((p[24+i/8]>>(i&7))&1u)!=(i<n))return false;
    for(uint32_t i=0;i<n;i++){struct ln_music_journal_record r;record(p+64+i*32,&r);if(!valid_record(&r))return false;
        for(uint32_t j=0;j<i;j++)if(ln_read_le64(p+64+j*32)==r.id)return false;}
#if defined(LINNET_TARGET_VIDEO5G_M25)
    if(!n)return true; /* The entire empty tail was already proved zero. */
#endif
    return bytes_zero(p+64+n*32,4092-64-n*32);
}
int ln_history_page_load(struct ln_history_page *s,const uint8_t *a,const uint8_t *b,uint32_t index,const uint8_t epoch[16]){
    if(!s||!epoch||index>=1024)return -3;bool equal=a&&b&&bytes_equal(a,b,4096),av=valid(a,index,epoch),bv=equal?av:valid(b,index,epoch);if(!av&&!bv)return -1;
    uint32_t bank=av?0u:1u;if(av&&bv){uint32_t d=ln_read_le32(b+12)-ln_read_le32(a+12);if(d==0x80000000u||(!d&&!equal))return -2;if(d&&d<0x80000000u)bank=1;}
    const uint8_t *p=bank?b:a;s->index=index;s->count=ln_read_le32(p+16);s->sequence=ln_read_le32(p+12);s->revision=s->durable_revision=ln_read_le32(p+20);s->bank=bank;
    for(uint32_t i=0;i<16;i++)s->epoch[i]=epoch[i];for(uint32_t i=0;i<s->count;i++)record(p+64+i*32,&s->records[i]);s->ready=true;s->staged=false;return 1;
}
const struct ln_music_journal_record *ln_history_page_get(const struct ln_history_page *s,uint64_t id){if(!s||!s->ready)return 0;for(uint32_t i=0;i<s->count;i++)if(s->records[i].id==id)return &s->records[i];return 0;}
bool ln_history_page_put(struct ln_history_page *s,const struct ln_music_journal_record *r){
    if(!s||!s->ready||!valid_record(r))return false;uint32_t i=0;while(i<s->count&&s->records[i].id!=r->id)i++;
    if(i==125)return false;
    if(i<s->count){const struct ln_music_journal_record *a=&s->records[i];if(a->flags==r->flags&&a->play_count==r->play_count&&a->bookmark_frame==r->bookmark_frame&&a->resume_frame==r->resume_frame)return true;}
    else s->count++;s->records[i]=*r;s->revision++;return true;
}
bool ln_history_page_encode(struct ln_history_page *s,uint8_t out[4096]){
    if(!s||!out||!s->ready||s->staged||s->revision==s->durable_revision)return false;
    for(uint32_t i=0;i<4096;i++)out[i]=0;ln_write_le32(out,0x45475048u);ln_write_le32(out+4,0x00400000u);ln_write_le32(out+8,s->index);ln_write_le32(out+12,s->sequence+1);ln_write_le32(out+16,s->count);ln_write_le32(out+20,s->revision);
    for(uint32_t i=0;i<16;i++)out[40+i]=s->epoch[i];for(uint32_t i=0;i<s->count;i++){out[24+i/8]|=(uint8_t)(1u<<(i&7));const struct ln_music_journal_record *r=&s->records[i];uint8_t *p=out+64+i*32;ln_write_le64(p,r->id);ln_write_le32(p+8,r->flags);ln_write_le32(p+12,r->play_count);ln_write_le64(p+16,r->bookmark_frame);ln_write_le64(p+24,r->resume_frame);}
    ln_write_le32(out+4092,crc(out));s->staged=true;s->staged_revision=s->revision;s->staged_sequence=s->sequence+1;s->staged_bank=s->bank^1u;return true;
}
bool ln_history_page_ack(struct ln_history_page *s){if(!s||!s->staged)return false;s->bank=s->staged_bank;s->sequence=s->staged_sequence;s->durable_revision=s->staged_revision;s->staged=false;return true;}
