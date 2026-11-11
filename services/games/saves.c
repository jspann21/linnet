#include "saves.h"
#include "services/games/package.h"
#include "services/state/store.h"
#include "services/storage/storage.h"
#include "boot/include/linnet_bytes.h"
enum { QUEUE=8 };
struct save_item {uint32_t slot,bank;uint8_t record[512];};
static struct save_item queue[QUEUE];
static uint32_t head,count,ticket,phase,read_bank,slot,schema,generation,bank;
static uint32_t best,progress_schema,progress_bytes;
static uint8_t copies[2][512] __attribute__((aligned(32)));
static uint8_t progress[LN_GAME_SAVE_BYTES],payload[LN_STATE_PAYLOAD_MAX];
static char identity[64];
static bool ready=true,available,poisoned,ending,verify_pending,best_dirty;
static uint32_t file(uint32_t copy){return copy?LN_STORAGE_GAMESAVE1:LN_STORAGE_GAMESAVE0;}
static bool equal(const char *a,const char *b){for(uint32_t i=0;i<64u;++i){if(a[i]!=b[i])return false;if(!a[i])return true;}return false;}
static bool decode(const uint8_t *record,uint32_t *g,uint32_t *length){
    if(ln_state_decode(record,g,length)!=1||*length<76u||*length>76u+LN_GAME_SAVE_BYTES)return false;
    const uint8_t *p=record+28u;uint32_t n=ln_read_le32(p+72u);
    bool ended=false;for(uint32_t i=0;i<64u;++i){if(!p[i])ended=true;else if(ended)return false;}
    return ln_game_identity((const char *)p)&&n<=LN_GAME_SAVE_BYTES&&*length==76u+n&&ln_read_le32(p+64u)!=0;
}
static void adopt(const uint8_t *record,uint32_t copy){
    uint32_t length=0;decode(record,&generation,&length);bank=copy;
    const uint8_t *p=record+28u;uint32_t score=ln_read_le32(p+68u);if(score>best)best=score;
    progress_schema=ln_read_le32(p+64u);progress_bytes=ln_read_le32(p+72u);
    for(uint32_t i=0;i<progress_bytes;++i)progress[i]=p[76u+i];
}
void ln_game_save_begin(const char *id,uint32_t index,uint32_t version,uint32_t old_best){
    /* Called only after the previous begin/read has reached a terminal state. */
    slot=index;schema=version;best=old_best;generation=bank=progress_bytes=0;progress_schema=schema;
    for(uint32_t i=0;i<64u;++i)identity[i]=id[i];
    ending=false;available=false;ready=false;read_bank=0;best_dirty=false;
    if(slot>=LN_GAME_CATALOGUE_MAX||!ln_storage_game_saves_available()||poisoned){ready=true;return;}
    for(uint32_t i=count;i>0;--i){struct save_item *item=&queue[(head+i-1u)%QUEUE];
        if(item->slot==slot){uint32_t g,n;if(!decode(item->record,&g,&n)||!equal(identity,(const char *)item->record+28u)){ready=true;return;}
            adopt(item->record,item->bank);available=true;ready=true;return;}}
}
bool ln_game_save_ready(void){return ready;}
uint32_t ln_game_save_best(void){return best;}
int32_t ln_game_save_status(void){return !available||poisoned?-1:count?1:0;}
static bool enqueue(uint32_t score,uint32_t version,const uint8_t *data,uint32_t bytes){
    if(!ready||!available||poisoned)return false;
    for(uint32_t i=0;i<64u;++i)payload[i]=(uint8_t)identity[i];
    ln_write_le32(payload+64u,version);ln_write_le32(payload+68u,score);ln_write_le32(payload+72u,bytes);
    for(uint32_t i=0;i<bytes;++i)payload[76u+i]=data[i];
    /* Coalesce only buffers that are not owned by an admitted write/readback. */
    for(uint32_t i=count;i>0;--i){uint32_t at=(head+i-1u)%QUEUE;struct save_item *old=&queue[at];
        if(old->slot!=slot)continue;
        if(at==head&&(ticket||verify_pending))break;
        return ln_state_encode(old->record,generation,payload,76u+bytes)!=0;
    }
    if(count==QUEUE||generation==UINT32_MAX)return false;
    struct save_item *item=&queue[(head+count)%QUEUE];item->slot=slot;item->bank=bank^1u;
    if(!ln_state_encode(item->record,generation+1u,payload,76u+bytes))return false;
    ++count;++generation;bank^=1u;return true;
}
bool ln_game_save_record(uint32_t score){
    if(score<=best&&!best_dirty)return true;
    if(score>best)best=score;
    bool accepted=enqueue(best,progress_schema,progress,progress_bytes);best_dirty=!accepted;return accepted;
}
int32_t ln_game_save_read(uint32_t version,void *data,uint32_t capacity){
    if(!ready||!available||poisoned||version!=progress_schema||capacity<progress_bytes||(!data&&progress_bytes))return -1;
    for(uint32_t i=0;i<progress_bytes;++i)((uint8_t *)data)[i]=progress[i];return (int32_t)progress_bytes;
}
bool ln_game_save_write(uint32_t version,const void *data,uint32_t bytes){
    if(version!=schema||version<progress_schema||bytes>LN_GAME_SAVE_BYTES||(!data&&bytes))return false;
    if(!enqueue(best,version,data,bytes))return false;
    best_dirty=false;
    progress_schema=version;progress_bytes=bytes;
    for(uint32_t i=0;i<bytes;++i)progress[i]=((const uint8_t *)data)[i];return true;
}
void ln_game_save_end(void){ending=true;}
bool ln_game_save_busy(void){return ticket||!ready||count;}
bool ln_game_save_inflight(void){return ticket||!ready||verify_pending;}
bool ln_game_save_failed(void){return poisoned;}
bool ln_game_save_work(bool may_write){return ln_game_save_inflight()||(!poisoned&&may_write&&count);}
static void finish_read(void){
    uint32_t g[2]={0},n[2]={0};bool valid[2];
    for(uint32_t i=0;i<2u;++i){valid[i]=decode(copies[i],g+i,n+i);
        if(valid[i]&&!equal(identity,(const char *)copies[i]+28u)){available=false;ready=true;return;}}
    if(valid[0]&&valid[1]&&g[0]==g[1]&&!ln_bytes_equal(copies[0],copies[1],512u)){ready=true;return;}
    if(valid[0]||valid[1]){uint32_t copy=!valid[0]||(valid[1]&&g[1]>g[0]);adopt(copies[copy],copy);available=true;}
    ready=true;
}
static void poison(void){
    poisoned=true;available=false;ready=true;count=0;phase=0;ticket=0;verify_pending=false;
}
void ln_game_save_step(bool may_write){
    uint32_t transferred=0;int result;
    if(ticket){result=ln_storage_async_poll_deferred(ticket,&transferred);if(!result)return;ticket=0;
        if(result<0||transferred!=512u){poison();return;}
        if(phase==1u){if(++read_bank==2u)finish_read();phase=0;return;}
        if(phase==2u){verify_pending=true;phase=0;return;}
        if(phase==4u){struct save_item *item=&queue[head];
            if(!ln_bytes_equal(copies[0],item->record,512u)){poison();return;}
            head=(head+1u)%QUEUE;--count;phase=0;verify_pending=false;return;}
    }
    if(verify_pending&&count){struct save_item *item=&queue[head];
        result=ln_storage_async_read(file(item->bank),item->slot*4096u,copies[0],512u,&ticket);
        if(result>0)phase=4u;else if(result<0)poison();
        return;
    }
    if(!ready){
        if(ending){ready=true;return;}
        result=ln_storage_async_read(file(read_bank),slot*4096u,copies[read_bank],512u,&ticket);
        if(result>0)phase=1u;else if(result<0){ready=true;available=false;}
        return;
    }
    if(!count||poisoned)return;
    struct save_item *item=&queue[head];
    if(may_write){
        result=ln_storage_async_write(file(item->bank),item->slot*4096u,item->record,512u,&ticket);
        if(result>0)phase=2u;
    }else return;
    if(result<0)poison();
}
