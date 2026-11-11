#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "services/music/queue.h"
static uint32_t find(const uint32_t *values,uint32_t count,uint32_t track)
{for(uint32_t i=0;i<count;++i)if(values[i]==track)return i;return count;}
void ln_queue_init(struct ln_queue *q,uint32_t seed)
{if(q){*q=(struct ln_queue){0};q->seed=seed?seed:0x4c4e5155u;q->volume_db_tenths=-400;}}
static uint32_t div32(uint32_t a,uint32_t b){uint32_t q=0,r=0;for(uint32_t i=32;i;i--){r=(r<<1)|((a>>(i-1))&1u);if(r>=b){r-=b;q|=1u<<(i-1);}}return q;}
static uint32_t mod(uint64_t a,uint32_t b){
    uint32_t high=(uint32_t)(a>>32),low=(uint32_t)a,r=high;
    /* The upper word is already a remainder for bounded queue products. */
    if(high>=b){r=0;for(uint32_t i=32;i;i--){r=(r<<1)|((high>>(i-1))&1u);if(r>=b)r-=b;}}
    for(uint32_t i=32;i;i--){r=(r<<1)|((low>>(i-1))&1u);if(r>=b)r-=b;}
    return r;
}
static uint32_t gcd(uint32_t a,uint32_t b){while(b){uint32_t r=mod(a,b);a=b;b=r;}return a;}
static uint32_t inverse(uint32_t a,uint32_t n){
    int64_t t=0,next=1;uint32_t r=n,nr=a;
    while(nr){uint32_t q=div32(r,nr),old=r;r=nr;nr=old-q*nr;
        int64_t ot=t;t=next;next=ot-(int64_t)q*next;}
    if(t<0)t+=n;return (uint32_t)t;
}
bool ln_queue_set_source(struct ln_queue*q,uint32_t source,uint32_t group,uint32_t count,uint32_t ordinal){
    if(!q||source<LN_QUEUE_ALL||source>LN_QUEUE_GROUP||!count||count>LINNET_QUEUE_SOURCE_MAX||ordinal>=count)return false;
    q->source_count=count;q->excluded_count=0;q->source=source;q->source_group=source==LN_QUEUE_GROUP?group:group&LN_QUEUE_REVERSE;q->count=count;q->cursor=ordinal;
    q->stride=q->inverse=1;q->offset=0;q->shuffled=false;q->position_samples=0;return true;
}
static uint32_t source_ordinal(const struct ln_queue*q,uint32_t ordinal){return q->source_group&LN_QUEUE_REVERSE?q->source_count-1-ordinal:ordinal;}
static uint32_t raw_rank(const struct ln_queue*q,uint32_t ordinal){return q->shuffled?mod((uint64_t)(source_ordinal(q,ordinal)+q->source_count-q->offset)*q->inverse,q->source_count):source_ordinal(q,ordinal);}
bool ln_queue_track_at(const struct ln_queue*q,uint32_t at,uint32_t*out){
    if(!q||!out||at>=q->count)return false;
    if(!q->source){if(q->order[at]>=q->count)return false;*out=q->tracks[q->order[at]];return true;}
    uint32_t ranks[32];
    for(uint32_t i=0;i<q->excluded_count;i++){uint32_t r=raw_rank(q,q->excluded[i]),j=i;
        while(j&&ranks[j-1]>r){ranks[j]=ranks[j-1];j--;}ranks[j]=r;}
    for(uint32_t i=0;i<q->excluded_count;i++)if(ranks[i]<=at)at++;
    *out=source_ordinal(q,q->shuffled?mod((uint64_t)at*q->stride+q->offset,q->source_count):at);return true;
}
bool ln_queue_select_at(struct ln_queue*q,uint32_t at){if(!q||at>=q->count)return false;q->cursor=at;q->position_samples=0;return true;}
bool ln_queue_exclusion_ranks(const struct ln_queue*q,uint32_t ranks[32]){
    if(!q||!ranks||q->source<LN_QUEUE_ALL||q->source>LN_QUEUE_GROUP||
       !q->source_count||q->source_count>LINNET_QUEUE_SOURCE_MAX||q->excluded_count>32u||
       q->excluded_count>q->source_count||q->count!=q->source_count-q->excluded_count||
       !q->stride||q->stride>q->source_count||q->inverse>q->source_count||q->offset>=q->source_count)return false;
    for(uint32_t i=0;i<q->excluded_count;i++){
        if(q->excluded[i]>=q->source_count)return false;
        uint32_t r=raw_rank(q,q->excluded[i]),j=i;
        while(j&&ranks[j-1]>r){ranks[j]=ranks[j-1];j--;}ranks[j]=r;
    }
    return true;
}
bool ln_queue_track_at_ranks(const struct ln_queue*q,uint32_t at,const uint32_t ranks[32],uint32_t*out){
    if(!q||!out||!ranks||q->source<LN_QUEUE_ALL||q->source>LN_QUEUE_GROUP||
       !q->source_count||q->excluded_count>32u||at>=q->count)return false;
    for(uint32_t i=0;i<q->excluded_count;i++)if(ranks[i]<=at)at++;
    *out=source_ordinal(q,q->shuffled?mod((uint64_t)at*q->stride+q->offset,q->source_count):at);return true;
}
bool ln_queue_current(const struct ln_queue*q,uint32_t*out){return q&&ln_queue_track_at(q,q->cursor,out);}
bool ln_queue_add(struct ln_queue *q,uint32_t track)
{if(!q||q->source||q->count>=LINNET_QUEUE_CAPACITY)return false;q->tracks[q->count]=track;q->order[q->count]=q->count;++q->count;return true;}
bool ln_queue_select_occurrence(struct ln_queue *q,uint32_t occurrence)
{if(!q)return false;if(q->source){
 if(occurrence>=q->source_count||find(q->excluded,q->excluded_count,occurrence)!=q->excluded_count)return false;
 uint32_t rank=raw_rank(q,occurrence),at=rank;for(uint32_t i=0;i<q->excluded_count;i++)if(raw_rank(q,q->excluded[i])<rank)at--;
 return ln_queue_select_at(q,at);}if(occurrence>=q->count)return false;uint32_t i=find(q->order,q->count,occurrence);if(i==q->count)return false;q->cursor=i;q->position_samples=0;return true;}
bool ln_queue_remove_occurrence(struct ln_queue *q,uint32_t occurrence)
{
    if(!q)return false;
    if(q->source){
        if(occurrence>=q->source_count||q->excluded_count>=32||find(q->excluded,q->excluded_count,occurrence)!=q->excluded_count)return false;
        uint32_t current;if(!ln_queue_current(q,&current))return false;
        q->excluded[q->excluded_count++]=occurrence;q->count--;
        if(!q->count){q->cursor=0;q->position_samples=0;return true;}
        if(current!=occurrence){uint64_t position=q->position_samples;ln_queue_select_occurrence(q,current);q->position_samples=position;}
        else{if(q->cursor>=q->count)q->cursor=q->count-1;q->position_samples=0;}return true;
    }
    if(occurrence>=q->count)return false;
    uint32_t play=find(q->order,q->count,occurrence);if(play==q->count)return false;
    for(uint32_t i=occurrence+1;i<q->count;++i)q->tracks[i-1]=q->tracks[i];
    for(uint32_t i=play+1;i<q->count;++i)q->order[i-1]=q->order[i];
    --q->count;for(uint32_t i=0;i<q->count;++i)if(q->order[i]>occurrence)--q->order[i];
    if(play<q->cursor)--q->cursor;
    else if(play==q->cursor){q->position_samples=0;if(q->cursor&&q->cursor==q->count)--q->cursor;}
    q->tracks[q->count]=q->order[q->count]=0;return true;
}
bool ln_queue_move_occurrence(struct ln_queue *q,uint32_t old,uint32_t index)
{
    if(!q||q->source||old>=q->count||index>=q->count)return false;
    uint32_t track=q->tracks[old],current=q->order[q->cursor];
    if(old<index)for(uint32_t i=old;i<index;++i)q->tracks[i]=q->tracks[i+1];
    else for(uint32_t i=old;i>index;--i)q->tracks[i]=q->tracks[i-1];
    q->tracks[index]=track;
    for(uint32_t i=0;i<q->count;++i){uint32_t n=q->order[i];
        q->order[i]=n==old?index:old<index&&n>old&&n<=index?n-1:old>index&&n>=index&&n<old?n+1:n;}
    current=current==old?index:old<index&&current>old&&current<=index?current-1:old>index&&current>=index&&current<old?current+1:current;
    if(!q->shuffled){for(uint32_t i=0;i<q->count;++i)q->order[i]=i;q->cursor=current;}
    return true;
}
bool ln_queue_next(struct ln_queue *q,bool manual)
{
    if(!q||!q->count)return false;
    if(!manual&&q->repeat==LINNET_QUEUE_REPEAT_ONE){q->position_samples=0;return true;}
    if(q->cursor+1u<q->count)++q->cursor;
    else if(q->repeat==LINNET_QUEUE_REPEAT_ALL)q->cursor=0;
    else return false;
    q->position_samples=0;return true;
}
bool ln_queue_previous(struct ln_queue *q)
{
    if(!q||!q->count)return false;
    if(q->cursor)--q->cursor;
    else if(q->repeat==LINNET_QUEUE_REPEAT_ALL)q->cursor=q->count-1u;
    else return false;
    q->position_samples=0;return true;
}
static uint32_t random_word(struct ln_queue *q)
{
    uint32_t x=q->seed?q->seed:0x4c4e5155u;x^=x<<13;x^=x>>17;x^=x<<5;return q->seed=x;
}
void ln_queue_shuffle(struct ln_queue *q,bool enabled)
{
    if(!q||q->shuffled==enabled)return;
    if(!q->count){q->shuffled=enabled;q->stride=q->inverse=1;q->offset=q->cursor=0;return;}
    if(q->source){uint32_t current; if(!ln_queue_current(q,&current))return;
        q->shuffled=enabled;q->stride=q->inverse=1;q->offset=0;q->cursor=0;
        if(enabled){uint32_t stride=1+mod(random_word(q),q->source_count);
            for(uint32_t i=0;i<64&&gcd(stride,q->source_count)!=1;i++)stride=stride==q->source_count?1:stride+1;
            if(gcd(stride,q->source_count)!=1)stride=1;
            q->stride=stride;q->inverse=inverse(stride,q->source_count);q->offset=source_ordinal(q,current);q->cursor=0;}
        uint64_t position=q->position_samples;
        (void)ln_queue_select_occurrence(q,current);q->position_samples=position;return;
    }
    uint32_t current=q->count?q->order[q->cursor]:0;bool selected=q->count!=0;
    for(uint32_t i=0;i<q->count;++i)q->order[i]=i;
    if(enabled&&selected){
        uint32_t old=find(q->order,q->count,current);q->order[old]=q->order[0];q->order[0]=current;
        /* Current item stays first. Multiplication maps each random word to
         * [1,i] in one bounded operation; no rejection/retry loops. */
        for(uint32_t i=q->count-1u;i>1u;--i){uint32_t j=1u+(uint32_t)(((uint64_t)random_word(q)*i)>>32);
            uint32_t tmp=q->order[i];q->order[i]=q->order[j];q->order[j]=tmp;}
        q->cursor=0;
    }else if(selected)q->cursor=find(q->order,q->count,current);
    q->shuffled=enabled;
}
bool ln_queue_set_repeat(struct ln_queue *q,uint32_t repeat)
{if(!q||repeat>LINNET_QUEUE_REPEAT_ONE)return false;q->repeat=repeat;return true;}
static bool valid(const struct ln_queue *q)
{
    if(q->repeat>LINNET_QUEUE_REPEAT_ONE||
       q->cursor>=(q->count?q->count:1u)||q->volume_db_tenths < -730||q->volume_db_tenths>0)return false;
    if(q->source){
        if(q->source>LN_QUEUE_GROUP||!q->source_count||q->source_count>LINNET_QUEUE_SOURCE_MAX||q->excluded_count>32||q->excluded_count>q->source_count||q->count!=q->source_count-q->excluded_count||
           (q->source==LN_QUEUE_ALL&&(q->source_group&~LN_QUEUE_REVERSE))||!q->stride||q->stride>q->source_count||
           q->offset>=q->source_count||q->inverse>q->source_count||mod((uint64_t)q->stride*q->inverse,q->source_count)!=(q->source_count==1?0u:1u))return false;
        for(uint32_t i=0;i<q->excluded_count;i++)if(q->excluded[i]>=q->source_count||find(q->excluded,i,q->excluded[i])!=i)return false;
        return q->shuffled||(q->stride==1&&q->inverse==1&&!q->offset);
    }
    if(q->count>LINNET_QUEUE_CAPACITY)return false;
    if(!q->shuffled){
        for(uint32_t i=0;i<q->count;++i)if(q->order[i]!=i)return false;
        return true;
    }
    uint32_t seen[LINNET_QUEUE_CAPACITY/32u]={0};
    for(uint32_t i=0;i<q->count;++i){
        uint32_t at=q->order[i];if(at>=q->count)return false;
        uint32_t mask=1u<<(at&31u);
        if(seen[at>>5]&mask)return false;
        seen[at>>5]|=mask;
    }return true;
}
uint32_t ln_queue_encode(const struct ln_queue *q,uint8_t *out,uint32_t capacity)
{
    if(!q||!out||!valid(q))return 0;
    uint32_t width=q->count<=64?5u:6u;
    uint32_t length=q->source?108u+4u*q->excluded_count:80u+q->count*width;if(capacity<length)return 0;
    ln_write_le32(out,0x55455551u);ln_write_le32(out+4,q->source?1u:width==5u?0u:2u);ln_write_le32(out+8,length);ln_write_le32(out+12,q->count);
    ln_write_le32(out+16,q->cursor);ln_write_le32(out+20,q->repeat);ln_write_le32(out+24,q->seed);ln_write_le32(out+28,q->shuffled?1u:0u);
    ln_write_le32(out+32,(uint32_t)q->position_samples);ln_write_le32(out+36,(uint32_t)(q->position_samples>>32));
    ln_write_le32(out+40,(uint32_t)q->volume_db_tenths);
    for(uint32_t i=0;i<32u;++i)out[44u+i]=q->library_identity[i];
    if(q->source){ln_write_le32(out+76,q->source);ln_write_le32(out+80,q->source_group);ln_write_le32(out+84,q->stride);ln_write_le32(out+88,q->offset);ln_write_le32(out+92,q->inverse);ln_write_le32(out+96,q->source_count);ln_write_le32(out+100,q->excluded_count);for(uint32_t i=0;i<q->excluded_count;i++)ln_write_le32(out+104+4*i,q->excluded[i]);}
    else for(uint32_t i=0;i<q->count;++i){ln_write_le32(out+76u+4u*i,q->tracks[i]);if(width==5u)out[76u+4u*q->count+i]=(uint8_t)q->order[i];else {out[76u+4u*q->count+2*i]=(uint8_t)q->order[i];out[77u+4u*q->count+2*i]=(uint8_t)(q->order[i]>>8);}}
    ln_write_le32(out+length-4u,ln_crc32(out,length-4u));return length;
}
bool ln_queue_decode(struct ln_queue *q,const uint8_t *data,uint32_t length)
{
    if(!q||!data||length<80u||length>LINNET_QUEUE_RESUME_MAX||ln_read_le32(data)!=0x55455551u||
       ln_read_le32(data+4)>2u||ln_read_le32(data+8)!=length||
       (ln_read_le32(data+4)!=1u?(ln_read_le32(data+12)>LINNET_QUEUE_CAPACITY||length!=80u+ln_read_le32(data+12)*(ln_read_le32(data+4)==2u?6u:5u)):(length<108u||length>236u))||ln_read_le32(data+28)>1u||ln_read_le32(data+length-4u)!=ln_crc32(data,length-4u))return false;
    static struct ln_queue candidate;candidate=(struct ln_queue){0};candidate.count=ln_read_le32(data+12);candidate.cursor=ln_read_le32(data+16);
    candidate.repeat=ln_read_le32(data+20);candidate.seed=ln_read_le32(data+24);candidate.shuffled=ln_read_le32(data+28)!=0;
    candidate.position_samples=ln_read_le32(data+32)|((uint64_t)ln_read_le32(data+36)<<32);
    uint32_t volume=ln_read_le32(data+40); /* Explicit two's-complement decoding. */
    if(volume&&volume<0xfffffd26u)return false;
    candidate.volume_db_tenths=volume?-(int32_t)(~volume+1u):0;
    for(uint32_t i=0;i<32u;++i)candidate.library_identity[i]=data[44u+i];
    if(ln_read_le32(data+4)==1){candidate.source=ln_read_le32(data+76);candidate.source_group=ln_read_le32(data+80);
        candidate.stride=ln_read_le32(data+84);candidate.offset=ln_read_le32(data+88);candidate.inverse=ln_read_le32(data+92);candidate.source_count=ln_read_le32(data+96);candidate.excluded_count=ln_read_le32(data+100);
        if(candidate.excluded_count>32||length!=108u+4u*candidate.excluded_count)return false;
        for(uint32_t i=0;i<candidate.excluded_count;i++)candidate.excluded[i]=ln_read_le32(data+104+4*i);
        if(!candidate.source||!valid(&candidate))return false;*q=candidate;return true;}
    for(uint32_t i=0;i<candidate.count;++i)candidate.tracks[i]=ln_read_le32(data+76u+4u*i);
    for(uint32_t i=0;i<candidate.count;++i){uint32_t index=ln_read_le32(data+4)==2u?((uint32_t)data[76u+4u*candidate.count+2*i]|((uint32_t)data[77u+4u*candidate.count+2*i]<<8)):data[76u+4u*candidate.count+i];
        if(index>=candidate.count)return false;candidate.order[i]=index;}
    if(!valid(&candidate))return false;*q=candidate;return true;
}
