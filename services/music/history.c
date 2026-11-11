#include "history.h"
#include "services/storage/storage.h"
#include "history_pages.h"

static struct {
    bool initialized,stepping,frozen;
    bool may_read,may_write;
    int error;
} history;

static bool capacities(void)
{
    uint32_t a=ln_storage_capacity(LN_STORAGE_MUSIC0),b=ln_storage_capacity(LN_STORAGE_MUSIC1);
    if(!a||!b){history.error=LN_HISTORY_MISSING;return false;}
    if(a!=LN_HISTORY_PAGED_BANK_BYTES||b!=a){history.error=LN_HISTORY_CAPACITY;return false;}
    return true;
}
void ln_music_history_init(void)
{
    if(history.initialized)return;
    history.initialized=true;
}
void ln_music_history_pin_current(bool present,uint64_t id){if(!history.frozen)ln_paged_history_pin_current(present,id);}
void ln_music_history_freeze(void){history.frozen=true;}
bool ln_music_history_ready(void){return history.initialized&&ln_paged_history_ready();}
bool ln_music_history_finishing(void){return ln_paged_history_finishing();}
int ln_music_history_error(void){return history.error?history.error:ln_paged_history_error();}
bool ln_music_history_busy(void){return history.stepping||ln_paged_history_busy();}
bool ln_music_history_dirty(void){return ln_paged_history_dirty();}
uint32_t ln_music_history_revision(void){return ln_paged_history_revision();}
const struct ln_music_journal_record *ln_music_history_get(uint64_t id){return ln_paged_history_get(id);}
bool ln_music_history_prefetch(uint64_t id){return !history.frozen&&ln_paged_history_prefetch(id);}
bool ln_music_history_record_ready(uint64_t id){return ln_paged_history_record_ready(id);}
int ln_music_history_record_error(uint64_t id){return ln_paged_history_record_error(id);}
static bool writable_events(void){return ln_music_history_ready()&&!ln_music_history_error()&&!history.frozen;}
static bool event(uint64_t id,uint32_t flag,bool present,uint64_t frame){
    if(!writable_events()||!ln_paged_history_record_ready(id))return false;
    const struct ln_music_journal_record *old=ln_paged_history_get(id);
    if(!old&&!present&&flag)return true;
    struct ln_music_journal_record r=old?*old:(struct ln_music_journal_record){.id=id};
    if(!flag){if(r.play_count!=UINT32_MAX)r.play_count++;}
    else{if(present)r.flags|=flag;else r.flags&=~flag;
        if(flag==LN_MUSIC_JOURNAL_BOOKMARK)r.bookmark_frame=present?frame:0;
        if(flag==LN_MUSIC_JOURNAL_RESUME)r.resume_frame=present?frame:0;}
    return ln_paged_history_put(&r);
}
bool ln_music_history_set_favorite(uint64_t id,bool favorite){return event(id,LN_MUSIC_JOURNAL_FAVORITE,favorite,0);}
bool ln_music_history_set_bookmark(uint64_t id,bool present,uint64_t frame){return event(id,LN_MUSIC_JOURNAL_BOOKMARK,present,frame);}
bool ln_music_history_set_resume(uint64_t id,bool present,uint64_t frame){return event(id,LN_MUSIC_JOURNAL_RESUME,present,frame);}
bool ln_music_history_played(uint64_t id){return event(id,0,true,0);}
int ln_music_history_flush(void)
{
    int error=ln_music_history_error();
    return error?error:ln_paged_history_ready()&&!ln_paged_history_busy()&&!ln_paged_history_dirty()?1:0;
}
static void step(bool may_read,bool may_write)
{
    if(!history.initialized||history.error)return;
    if(!ln_paged_history_io_pending()&&((!may_read&&!may_write)||ln_storage_busy()||!ln_storage_mounted()))return;
    if(!capacities())return;
    ln_paged_history_step(may_read,may_write);
}
void ln_music_history_step(bool may_write){ln_music_history_service(may_write,may_write,false);}
uint32_t ln_music_history_deadline(uint32_t now)
{
#if LINNET_TARGET_NUMBER==25u
    if((!history.initialized||history.error)&&!ln_paged_history_io_pending())return now+200000u;
#endif
    return ln_paged_history_deadline(now,history.may_read,history.may_write);
}
void ln_music_history_service(bool may_read,bool may_write,bool playback)
{
    (void)playback;
    if(history.stepping)return;
    history.may_read=may_read;history.may_write=may_write;
    history.stepping=true;step(may_read,may_write);history.stepping=false;
}
