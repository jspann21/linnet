#include "boot/include/linnet_bytes.h"
#include "history_pages.h"
#include "history.h"
#include "services/storage/storage.h"
#include "hal/api/platform.h"
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
#include "coverflow.h"
extern void ln_memory_copy(void *,const void *,uint32_t);
#define BULK_BANK_BYTES (32u*4096u)
#endif
enum {SUPER_A,SUPER_B,IDLE,PAGE_A,PAGE_B,PUBLISH,FLUSH,CHECK,ACK,FAILED};
static struct {
    struct ln_history_page cache[4];uint32_t age[4],clock,revision;
    uint8_t a[16384],b[16384],epoch[16];
    uint32_t window_first,window_count,window_revision;bool window_valid;
    uint32_t seen[32],bookmark_pages[32],favorite_pages[32],played_pages[32],seen_count,bookmarks;
    uint32_t phase,index,probe,slot;int error;bool loaded,wanted,scanning;
    uint32_t ticket,io_bytes;int io_error;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    uint32_t bulk_epoch,bulk_at;
    bool bulk,bulk_active,bulk_allowed;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t bulk_windows,bulk_copy_bytes,bulk_aborts;
#endif
#endif
    uint64_t id,full_id,pinned_id;bool full,pinned;
    struct {uint64_t id;uint32_t slot,index;bool valid;} proofs[4];uint32_t proof_cursor;
} h;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
static uint8_t *bulk_memory(void){return ln_coverflow_history_memory(h.bulk_epoch);}
static void bulk_release(void){
    ln_coverflow_history_release(h.bulk_epoch);h.bulk_epoch=0;
    if(h.bulk)h.window_valid=false;
    h.bulk=false;h.bulk_at=0;
}
static bool bulk_cleanup(void){
    if(!h.bulk_epoch||h.ticket)return false;
    bool lost=bulk_memory()==0,reading=h.bulk&&(h.phase==PAGE_A||h.phase==PAGE_B);
    if(reading&&(!h.bulk_allowed||lost)){
        bool abandon=!h.bulk_allowed;
        bulk_release();h.phase=IDLE;
        if(abandon&&h.scanning)h.wanted=h.scanning=false;
#if !defined(LINNET_PROFILE_RELEASE)
        ++h.bulk_aborts;
#endif
        return true;
    }
    if(lost||!h.bulk_active){
        bulk_release();
        if(!h.bulk_active&&h.scanning&&h.phase==IDLE)h.wanted=h.scanning=false;
    }
    return false;
}
void ln_paged_history_scan_policy(bool active,bool allowed){
    h.bulk_active=active;h.bulk_allowed=active&&allowed;
    (void)bulk_cleanup();
}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_paged_history_window_diagnostics(uint32_t words[8]){
    words[0]=0x4b4c4248u;words[1]=h.bulk_epoch;words[2]=h.window_count;
    words[3]=(uint32_t)h.bulk|((uint32_t)h.bulk_active<<1)|((uint32_t)h.bulk_allowed<<2)|
        ((uint32_t)h.window_valid<<3)|((uint32_t)(h.ticket!=0)<<4)|((uint32_t)(bulk_memory()!=0)<<5);
    words[4]=h.bulk_windows;words[5]=h.bulk_copy_bytes;words[6]=h.bulk_aborts;words[7]=h.bulk_at;
}
#endif
#endif
static void fail(int error){
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    bulk_release();
#endif
    h.error=error;h.phase=FAILED;
}
bool ln_paged_history_ready(void){return h.loaded;}
int ln_paged_history_error(void){return h.error;}
bool ln_paged_history_dirty(void){for(uint32_t i=0;i<4;i++)if(h.cache[i].ready&&(h.cache[i].staged||h.cache[i].revision!=h.cache[i].durable_revision))return true;return false;}
bool ln_paged_history_busy(void){return !h.loaded||h.wanted||h.phase!=IDLE;}
bool ln_paged_history_io_pending(void){return h.ticket!=0;}
bool ln_paged_history_finishing(void){return h.phase>=FLUSH&&h.phase<=ACK;}
uint32_t ln_paged_history_revision(void){return h.revision;}
int ln_paged_history_bookmark_count(void){return h.seen_count==LN_HISTORY_PAGE_COUNT?(int)h.bookmarks:-1;}
static bool pages_empty(const uint32_t pages[32]){
    if(h.seen_count!=LN_HISTORY_PAGE_COUNT)return false;
    for(uint32_t i=0;i<32u;++i)if(pages[i])return false;
    return true;
}
bool ln_paged_history_favorites_empty(void){return pages_empty(h.favorite_pages);}
bool ln_paged_history_played_empty(void){return pages_empty(h.played_pages);}
bool ln_paged_history_bookmark_page(uint32_t index){return index<LN_HISTORY_PAGE_COUNT&&(!(h.seen[index/32]&(1u<<(index&31)))||(h.bookmark_pages[index/32]&(1u<<(index&31))));}
bool ln_paged_history_favorite_page(uint32_t index){return index<LN_HISTORY_PAGE_COUNT&&(!(h.seen[index/32]&(1u<<(index&31)))||(h.favorite_pages[index/32]&(1u<<(index&31))));}
bool ln_paged_history_played_page(uint32_t index){return index<LN_HISTORY_PAGE_COUNT&&(!(h.seen[index/32]&(1u<<(index&31)))||(h.played_pages[index/32]&(1u<<(index&31))));}
static void summarize_page(const struct ln_history_page *p){
    uint32_t mask=1u<<(p->index&31),word=p->index/32;
    h.bookmark_pages[word]&=~mask;h.favorite_pages[word]&=~mask;h.played_pages[word]&=~mask;
    for(uint32_t i=0;i<p->count;i++){
        if(p->records[i].flags&LN_MUSIC_JOURNAL_BOOKMARK)h.bookmark_pages[word]|=mask;
        if(p->records[i].flags&LN_MUSIC_JOURNAL_FAVORITE)h.favorite_pages[word]|=mask;
        if(p->records[i].play_count)h.played_pages[word]|=mask;
    }
}
static int location(uint64_t id){
    if(!h.loaded)return -1;
    for(uint32_t i=0;i<4;i++)if(ln_history_page_get(&h.cache[i],id))return (int)i;
    for(uint32_t i=0;i<4;i++)if(h.proofs[i].valid&&h.proofs[i].id==id){uint32_t s=h.proofs[i].slot;
        if(h.cache[s].ready&&h.cache[s].index==h.proofs[i].index&&h.cache[s].count<125)return (int)s;}
    return -1;
}
void ln_paged_history_pin_current(bool present,uint64_t id){h.pinned=present;h.pinned_id=id;}
static bool pinned_slot(uint32_t slot){
    if(!h.pinned||!h.cache[slot].ready)return false;
    if(ln_history_page_get(&h.cache[slot],h.pinned_id))return true;
    for(uint32_t i=0;i<4;i++)if(h.proofs[i].valid&&h.proofs[i].id==h.pinned_id&&
        h.proofs[i].slot==slot&&h.proofs[i].index==h.cache[slot].index)return true;
    return false;
}
bool ln_paged_history_record_ready(uint64_t id){return location(id)>=0;}
int ln_paged_history_record_error(uint64_t id){return h.full&&h.full_id==id?LN_HISTORY_CAPACITY:0;}
const struct ln_music_journal_record *ln_paged_history_get(uint64_t id){
    if(!h.loaded)return 0;
    for(uint32_t i=0;i<4u;++i){
        const struct ln_music_journal_record *record=ln_history_page_get(&h.cache[i],id);
        if(record)return record;
    }
    return 0;
}
bool ln_paged_history_prefetch(uint64_t id){
    if(h.error||!h.loaded)return false;int slot=location(id);if(slot>=0){h.age[slot]=++h.clock;return true;}
    if(h.full&&h.full_id==id)return false;
    if(h.wanted)return !h.scanning&&h.id==id;h.wanted=true;h.id=id;h.index=ln_history_page_hash(id);h.probe=0;return true;
}
bool ln_paged_history_put(const struct ln_music_journal_record *r){
    if(h.error||!r)return false;int slot=location(r->id);if(slot<0)return false;
    const struct ln_music_journal_record *old=ln_history_page_get(&h.cache[slot],r->id);
    bool had=old&&(old->flags&LN_MUSIC_JOURNAL_BOOKMARK);
    bool had_favorite=old&&(old->flags&LN_MUSIC_JOURNAL_FAVORITE),had_played=old&&old->play_count;
    uint32_t before=h.cache[slot].revision;if(!ln_history_page_put(&h.cache[slot],r))return false;
    bool has=(r->flags&LN_MUSIC_JOURNAL_BOOKMARK)!=0;if(had!=has){if(has)h.bookmarks++;else h.bookmarks--;}
    if(had!=has||had_favorite!=((r->flags&LN_MUSIC_JOURNAL_FAVORITE)!=0)||had_played!=(r->play_count!=0))
        summarize_page(&h.cache[slot]);
    if(before!=h.cache[slot].revision){h.revision++;h.window_valid=false;}h.age[slot]=++h.clock;return true;
}
const struct ln_history_page *ln_paged_history_page(uint32_t index){
    if(!h.loaded||h.error||index>=LN_HISTORY_PAGE_COUNT)return 0;
    for(uint32_t i=0;i<4;i++)if(h.cache[i].ready&&h.cache[i].index==index){h.age[i]=++h.clock;return &h.cache[i];}
    if(!h.wanted&&h.phase==IDLE){h.wanted=h.scanning=true;h.index=index;h.probe=0;}
    return 0;
}
#if LINNET_TARGET_NUMBER==25u
bool ln_paged_history_page_waiting(uint32_t index){
    if(h.error||index>=LN_HISTORY_PAGE_COUNT)return false;
    for(uint32_t i=0;i<4;i++)if(h.cache[i].ready&&h.cache[i].index==index)return false;
    return h.ticket||h.wanted||h.phase!=IDLE;
}
bool ln_paged_history_played_cached(uint64_t *ids,uint32_t *scores,uint32_t capacity,uint32_t *count,uint32_t began){
    if(!h.loaded||h.error||h.wanted||h.ticket||h.phase!=IDLE||
       h.seen_count!=LN_HISTORY_PAGE_COUNT||!ids||!scores||!count)return false;
    *count=0;
    /* Unknown or evicted positive pages must take the original scan path. */
    for(uint32_t word=0;word<32u;++word){
        uint32_t covered=0;
        for(uint32_t slot=0;slot<4u;++slot){
            const struct ln_history_page *p=&h.cache[slot];
            if(p->ready&&p->index/32u==word)covered|=1u<<(p->index&31u);
        }
        if((h.played_pages[word]&~covered)||(uint32_t)(ln_time_us()-began)>=250u)return false;
    }
    for(uint32_t slot=0;slot<4u;++slot){
        const struct ln_history_page *p=&h.cache[slot];
        if(!p->ready)continue;
        for(uint32_t at=0;at<p->count;++at){
            if((uint32_t)(ln_time_us()-began)>=250u)return false;
            const struct ln_music_journal_record *r=&p->records[at];
            if(!r->play_count)continue;
            if(*count==capacity||ln_paged_history_get(r->id)!=r)return false;
            for(uint32_t n=0;n<*count;++n)if(ids[n]==r->id)return false;
            ids[*count]=r->id;scores[*count]=r->play_count;++*count;
        }
    }
    return (uint32_t)(ln_time_us()-began)<250u;
}
#endif
static void resolve(void){
    if(h.scanning){h.scanning=h.wanted=false;return;}
    struct ln_history_page *p=&h.cache[h.slot];
    if(ln_history_page_get(p,h.id)||p->count<125){
        uint32_t proof=h.proof_cursor++&3u;
        /* Preserve the audible ID's proof even when other IDs share its page. */
        for(uint32_t i=0;i<4;i++){
            uint32_t candidate=(proof+i)&3u;
            if(!h.proofs[candidate].valid||!h.pinned||h.proofs[candidate].id!=h.pinned_id||h.id==h.pinned_id){proof=candidate;break;}
        }
        h.proofs[proof]=(typeof(h.proofs[0])){h.id,h.slot,p->index,true};h.wanted=false;h.revision++;return;
    }
    if(++h.probe>=1024){h.full=true;h.full_id=h.id;h.wanted=false;h.revision++;return;}h.index=(h.index+1)&1023u;
}
static void load_window(void){
    uint32_t offset=(h.index-h.window_first)*4096;
        const uint8_t *a=h.a,*b=h.b;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
        if(h.bulk){
            uint8_t *memory=bulk_memory();
            if(!memory){bulk_release();h.phase=IDLE;return;}
            a=memory+offset;b=memory+BULK_BANK_BYTES+offset;
        }else
#endif
        {a+=offset;b+=offset;}
        int result=ln_history_page_load(&h.cache[h.slot],a,b,h.index,h.epoch);
        if(result<0){fail(result==-2?LN_HISTORY_CONFLICT:LN_HISTORY_BOTH_CORRUPT);return;}
        uint32_t mask=1u<<(h.index&31);if(!(h.seen[h.index/32]&mask)){
            const struct ln_history_page *p=&h.cache[h.slot];
            for(uint32_t i=0;i<p->count;i++)if(p->records[i].flags&LN_MUSIC_JOURNAL_BOOKMARK)h.bookmarks++;
            summarize_page(p);
            h.seen[h.index/32]|=mask;h.seen_count++;
        }
        h.age[h.slot]=++h.clock;h.phase=IDLE;resolve();return;
}
static void start_io(uint32_t operation,uint32_t file,uint32_t offset,uint8_t *buffer,int error){
    uint32_t bytes=(h.phase==PAGE_A||h.phase==PAGE_B)?h.window_count*4096u:4096u;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    if(h.bulk&&(h.phase==PAGE_A||h.phase==PAGE_B)){
        bytes-=h.bulk_at;offset+=h.bulk_at;
        if(bytes>16384u)bytes=16384u;
    }
#endif
    int result=operation==LN_FAT32_FLUSH?ln_storage_async_flush(&h.ticket):
        operation==LN_FAT32_WRITE?ln_storage_async_write(file,offset,buffer,bytes,&h.ticket):
        ln_storage_async_read(file,offset,buffer,bytes,&h.ticket);
    if(result<0){fail(error);return;}
    if(result>0){h.io_bytes=operation==LN_FAT32_FLUSH?0u:bytes;h.io_error=error;}
}
uint32_t ln_paged_history_deadline(uint32_t now,bool may_read,bool may_write){
    if(h.ticket)return now+200000u;
    uint32_t idle=now+200000u;
    if(h.error||ln_storage_busy()||!ln_storage_mounted())return idle;
    if(h.phase==SUPER_A||h.phase==SUPER_B||h.phase==PAGE_A||h.phase==PAGE_B){
#if LINNET_TARGET_NUMBER==25u
        if(may_read&&ln_storage_async_read_available())return now;
#endif
        return may_read?now+1000u:idle;
    }
    if(h.phase>=PUBLISH&&h.phase<=ACK)return may_write?now+1000u:idle;
    if(h.phase!=IDLE)return idle;
#if LINNET_TARGET_NUMBER==25u
    /* Match step's read-before-write priority. RAM windows can advance even
     * while another asynchronous media owner prevents a new fixed read. */
    if(may_read&&h.wanted){
        for(uint32_t i=0;i<4;i++){
            if(h.cache[i].ready&&h.cache[i].index==h.index)return now;
            if(!h.cache[i].ready||(!pinned_slot(i)&&!h.cache[i].staged&&
               h.cache[i].revision==h.cache[i].durable_revision)){
                if(h.window_valid&&h.index>=h.window_first&&h.index-h.window_first<h.window_count)return now;
                if(ln_storage_async_read_available())return now;
            }
        }
    }
#endif
    if(may_write&&ln_paged_history_dirty())return now+1000u;
    if(!may_read)return idle;
    if(h.pinned&&!h.wanted&&location(h.pinned_id)<0)return now+1000u;
    if(!h.wanted)return idle;
    for(uint32_t i=0;i<4;i++){
        if(!h.cache[i].ready||h.cache[i].index==h.index)return now+1000u;
        if(!pinned_slot(i)&&!h.cache[i].staged&&h.cache[i].revision==h.cache[i].durable_revision)return now+1000u;
    }
    /* A read waiting for a clean victim cannot advance while write admission
     * is withheld. Let playback refill its ring instead of keeping us hot. */
    return idle;
}
void ln_paged_history_step(bool may_read,bool may_write){
    bool completed=false;
    /* Admission policy can change while DMA owns a scratch buffer. Always
     * drain that exact ticket before applying the next operation's policy. */
    if(h.ticket){
        bool read=h.phase==SUPER_A||h.phase==SUPER_B||h.phase==PAGE_A||h.phase==PAGE_B;
        uint32_t bytes=0;int result=0;
        /* Only catalog/page reads group ready transitions. Publications,
         * flushes and their readback retain ordinary runtime service. */
        for(uint32_t step=0;step<4u;++step){
            result=read?ln_storage_async_poll_deferred(h.ticket,&bytes):ln_storage_async_poll(h.ticket,&bytes);
            if(result||!read)break;
            uint32_t now=ln_time_us(),deadline;
            if(ln_storage_idle_wait(now,&deadline)&&(int32_t)(deadline-now)>0)break;
        }
        if(!result)return;
        h.ticket=0;if(result<0||bytes!=h.io_bytes){fail(h.io_error);return;}completed=true;
    }
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    if(bulk_cleanup())return;
#endif
    if(h.error)return;
    if(!completed){
        if((!may_read&&!may_write)||ln_storage_busy()||!ln_storage_mounted())return;
        if(h.phase>=PUBLISH&&h.phase<=ACK&&!may_write)return;
        if((h.phase==SUPER_A||h.phase==SUPER_B||h.phase==PAGE_A||h.phase==PAGE_B)&&!may_read)return;
    }
    switch(h.phase){
    case SUPER_A:
        if(!completed){start_io(LN_FAT32_READ,LN_STORAGE_MUSIC0,0,h.a,LN_HISTORY_READ_A);return;}h.phase=SUPER_B;return;
    case SUPER_B:{
        if(!completed){start_io(LN_FAT32_READ,LN_STORAGE_MUSIC1,0,h.b,LN_HISTORY_READ_B);return;}
        uint8_t e[16];bool a=ln_history_superblock(h.a,h.epoch),b=ln_history_superblock(h.b,e);
        if(!a&&!b){fail(LN_HISTORY_BOTH_CORRUPT);return;}if(a&&b&&!ln_bytes_equal(e,h.epoch,16)){fail(LN_HISTORY_CONFLICT);return;}
        if(!a)for(uint32_t i=0;i<16;i++)h.epoch[i]=e[i];h.loaded=true;h.revision=1;h.phase=IDLE;return;
    }
    case IDLE:
        if(h.pinned&&!h.wanted&&location(h.pinned_id)<0)(void)ln_paged_history_prefetch(h.pinned_id);
        if(h.wanted&&may_read){
            for(uint32_t i=0;i<4;i++)if(h.cache[i].ready&&h.cache[i].index==h.index){h.slot=i;h.age[i]=++h.clock;resolve();return;}
            h.slot=4;for(uint32_t i=0;i<4;i++){if(!h.cache[i].ready){h.slot=i;break;}
                if(!pinned_slot(i)&&!h.cache[i].staged&&h.cache[i].revision==h.cache[i].durable_revision&&(h.slot==4||h.age[i]<h.age[h.slot]))h.slot=i;}
            if(h.slot<4){
                h.cache[h.slot].ready=false;for(uint32_t i=0;i<4;i++)if(h.proofs[i].slot==h.slot)h.proofs[i].valid=false;
                if(h.window_valid&&h.index>=h.window_first&&h.index-h.window_first<h.window_count){load_window();return;}
                h.window_valid=false;h.window_first=h.index;h.window_count=h.scanning?4u:1u;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
                h.bulk=false;h.bulk_at=0;
                /* Preserve the first four-page admission and all playback
                 * lookups. Extend only still-unverified, visible idle scans. */
                if(h.scanning&&h.index>=4u&&h.bulk_allowed&&
                   !(h.seen[h.index/32]&(1u<<(h.index&31)))){
                    uint32_t pages=LN_HISTORY_PAGE_COUNT-h.index;if(pages>32u)pages=32u;
                    for(uint32_t n=1;n<pages;n++)if(h.seen[(h.index+n)/32]&(1u<<((h.index+n)&31))){pages=n;break;}
                    if(pages>4u){
                        uint8_t *memory=bulk_memory();
                        if(!memory){h.bulk_epoch=0;memory=ln_coverflow_history_reserve(2u*BULK_BANK_BYTES,&h.bulk_epoch);}
                        if(memory){h.bulk=true;h.window_count=pages;}
                    }
                }
#endif
                if(h.window_count>LN_HISTORY_PAGE_COUNT-h.index)h.window_count=LN_HISTORY_PAGE_COUNT-h.index;
                h.window_revision=h.revision;h.phase=PAGE_A;return;
            }
        }
        /* Dirty pages are never evicted. Under pressure publish the oldest
         * unpinned page first, then admit the waiting read before more writes. */
        if(!may_write)return;
        h.slot=4;for(uint32_t i=0;i<4;i++)if(h.cache[i].ready&&!h.cache[i].staged&&h.cache[i].revision!=h.cache[i].durable_revision){
            if(h.slot==4||(!pinned_slot(i)&&pinned_slot(h.slot))||
               (pinned_slot(i)==pinned_slot(h.slot)&&h.age[i]<h.age[h.slot]))h.slot=i;
        }
        if(h.slot==4)return;
        if(!ln_storage_prepared()){if(ln_storage_prepare_writes()<0)fail(LN_HISTORY_READ_ONLY);return;}
        h.window_valid=false;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
        h.bulk=false;h.bulk_at=0;
#endif
        if(!ln_history_page_encode(&h.cache[h.slot],h.a)){fail(LN_HISTORY_ENCODE);return;}h.phase=PUBLISH;return;
    case PAGE_A:
        if(!completed){start_io(LN_FAT32_READ,LN_STORAGE_MUSIC0,(h.index+1)*4096,h.a,LN_HISTORY_READ_A);return;}
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
        if(h.bulk){
            uint8_t *memory=bulk_memory();
            if(!memory){(void)bulk_cleanup();return;}
            ln_memory_copy(memory+h.bulk_at,h.a,h.io_bytes);h.bulk_at+=h.io_bytes;
#if !defined(LINNET_PROFILE_RELEASE)
            h.bulk_copy_bytes+=h.io_bytes;
#endif
            if(h.bulk_at<h.window_count*4096u)return;
            h.bulk_at=0;
        }
#endif
        h.phase=PAGE_B;return;
    case PAGE_B:{
        if(!completed){start_io(LN_FAT32_READ,LN_STORAGE_MUSIC1,(h.index+1)*4096,h.b,LN_HISTORY_READ_B);return;}
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
        if(h.bulk){
            uint8_t *memory=bulk_memory();
            if(!memory){(void)bulk_cleanup();return;}
            ln_memory_copy(memory+BULK_BANK_BYTES+h.bulk_at,h.b,h.io_bytes);h.bulk_at+=h.io_bytes;
#if !defined(LINNET_PROFILE_RELEASE)
            h.bulk_copy_bytes+=h.io_bytes;
#endif
            if(h.bulk_at<h.window_count*4096u)return;
            h.bulk_at=0;
#if !defined(LINNET_PROFILE_RELEASE)
            ++h.bulk_windows;
#endif
        }
#endif
        h.window_valid=h.window_revision==h.revision;load_window();return;
    }
    case PUBLISH:
        if(!completed){
            if(!ln_storage_prepared()){if(ln_storage_prepare_writes()<0)fail(LN_HISTORY_READ_ONLY);return;}
            start_io(LN_FAT32_WRITE,LN_STORAGE_MUSIC0+h.cache[h.slot].staged_bank,(h.cache[h.slot].index+1)*4096,h.a,LN_HISTORY_WRITE);return;
        }h.phase=FLUSH;return;
    case FLUSH:if(!completed){start_io(LN_FAT32_FLUSH,0,0,0,LN_HISTORY_FLUSH);return;}h.phase=CHECK;return;
    case CHECK:
        if(!completed){start_io(LN_FAT32_READ,LN_STORAGE_MUSIC0+h.cache[h.slot].staged_bank,(h.cache[h.slot].index+1)*4096,h.b,LN_HISTORY_READBACK);return;}h.phase=ACK;return;
    case ACK:
        if(!ln_bytes_equal(h.a,h.b,4096)){fail(LN_HISTORY_MISMATCH);return;}
        if(!ln_history_page_ack(&h.cache[h.slot])){fail(LN_HISTORY_ACK);return;}h.phase=IDLE;return;
    default:return;
    }
}
