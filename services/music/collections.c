#include "collections.h"
#include "services/storage/storage.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include <stddef.h>
extern void ln_memory_copy(void *,const void *,uint32_t);
/* Independent dual banks. Body first, header last; CRC covers the complete
 * snapshot. The previous bank remains intact until readback verifies its peer. */
#define BODY 512u
#define LISTS (BODY+4u+LN_COLLECTION_RESUME)
#define ENTRY (40u+8u*LINNET_QUEUE_CAPACITY)
#define RECENT (LISTS+LN_COLLECTION_SLOTS*ENTRY)
#define RECENT_MAX 256u
/* Full-bank CRCs can use word loads only when both snapshots are aligned. */
/* Byte I/O accesses the object representation of this aligned native view. */
struct saved_bank {
    uint8_t prefix[LISTS];
    struct ln_collection lists[LN_COLLECTION_SLOTS];
    uint8_t suffix[LN_COLLECTION_BYTES-RECENT];
};
_Static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__,"Collections view requires little endian");
_Static_assert(offsetof(struct ln_collection,count)==32u&&offsetof(struct ln_collection,ids)==40u&&
               sizeof(struct ln_collection)==ENTRY,"Collections entry layout");
_Static_assert(offsetof(struct saved_bank,lists)==LISTS&&offsetof(struct saved_bank,suffix)==RECENT&&
               sizeof(struct saved_bank)==LN_COLLECTION_BYTES,"Collections bank layout");
static struct saved_bank saved_bank;
#define saved ((uint8_t *)&saved_bank)
static uint8_t io[LN_COLLECTION_BYTES] __attribute__((aligned(4)));
static uint32_t bank,generation,revision,durable,snapshot,snapshot_crc,phase,offset,ticket,loading;
static uint32_t running_crc;
static bool ready,pending,failed,available,stepping;
static uint32_t crc_chunk(uint8_t *p,uint32_t bytes,bool header){
    uint32_t old=header?ln_read_le32(p+12):0;if(header)ln_write_le32(p+12,0);
    uint32_t value=ln_crc32_update(running_crc,p,bytes);if(header)ln_write_le32(p+12,old);return value;
}
static bool valid(uint8_t *p){
    if(ln_read_le32(p)!=0x54534c4cu||ln_read_le32(p+4)!=1||!ln_read_le32(p+8)||ln_read_le32(p+12)!=(running_crc^UINT32_MAX)||ln_read_le32(p+BODY)>LN_COLLECTION_RESUME||ln_read_le32(p+RECENT)>RECENT_MAX)return false;
    for(uint32_t n=0;n<LN_COLLECTION_SLOTS;n++){uint8_t *e=p+LISTS+n*ENTRY;
        if(e[31]||ln_read_le32(e+32)>LINNET_QUEUE_CAPACITY||(!e[0]&&ln_read_le32(e+32)))return false;}
    return true;
}
bool ln_collections_ready(void){return ready;}
bool ln_collections_writable(void){return ready&&available&&!failed;}
bool ln_collections_busy(void){return pending||phase!=0;}
bool ln_collections_dirty(void){return revision!=durable;}
uint32_t ln_collections_revision(void){return revision+durable+(ready?1u:0u)+(failed?1u:0u);}
const char *ln_collections_status(void){return failed?"Save failed; reconnect USB":!ready?"Loading playlists...":!available?"Sync to enable saving":revision!=durable?"Saving...":"Saved";}
const char *ln_collection_name(uint32_t slot){
    return ready&&slot<LN_COLLECTION_SLOTS?(const char *)(saved+LISTS+slot*ENTRY):0;
}
const struct ln_collection *ln_collection_get(uint32_t slot){
    return ready&&slot<LN_COLLECTION_SLOTS?&saved_bank.lists[slot]:0;
}
int ln_collection_free(void){if(!ready)return -1;for(uint32_t i=0;i<LN_COLLECTION_SLOTS;i++)if(!saved[LISTS+i*ENTRY])return (int)i;return -1;}
bool ln_collection_put(uint32_t slot,const struct ln_collection *value){
    if(!ready||!available||failed||slot>=LN_COLLECTION_SLOTS||!value||value->name[31]||value->count>LINNET_QUEUE_CAPACITY||(!value->name[0]&&value->count))return false;
    uint32_t count=value->count;uint8_t *p=saved+LISTS+slot*ENTRY;
    /* Copy before clearing the unused tail so put(get(slot)) remains valid. */
    for(uint32_t i=0;i<32;i++)p[i]=(uint8_t)value->name[i];ln_write_le32(p+32,count);ln_write_le32(p+36,0);
    for(uint32_t i=0;i<count;i++)ln_write_le64(p+40+8*i,value->ids[i]);
    __builtin_memset(p+40u+8u*count,0,ENTRY-40u-8u*count);++revision;return true;
}
bool ln_collections_resume_set(const uint8_t *p,uint32_t n){
    if(!ready||!available||failed||n>LN_COLLECTION_RESUME)return false;
    bool changed=ln_read_le32(saved+BODY)!=n;for(uint32_t i=0;i<n;i++)if(saved[BODY+4+i]!=p[i])changed=true;
    if(changed){ln_write_le32(saved+BODY,n);for(uint32_t i=0;i<n;i++)saved[BODY+4+i]=p[i];++revision;}return true;
}
uint32_t ln_collections_resume_get(uint8_t *p,uint32_t cap){uint32_t n=ln_read_le32(saved+BODY);if(!ready||n>cap)return 0;for(uint32_t i=0;i<n;i++)p[i]=saved[BODY+4+i];return n;}
static void step(bool read,bool write){
    if(failed)return;
    if(!ready&&!phase){
        if(!read||!ln_storage_startup_complete())return;
        available=ln_storage_capacity(LN_STORAGE_LIST0)==LN_COLLECTION_BYTES&&ln_storage_capacity(LN_STORAGE_LIST1)==LN_COLLECTION_BYTES;
        if(!available){ready=true;return;}phase=1;loading=offset=0;running_crc=UINT32_MAX;
    }
    if(!phase){
        if(!write||revision==durable)return;
        int proof=ln_storage_prepare_writes();if(!proof)return;if(proof<0){failed=true;return;}
        if(generation==UINT32_MAX){failed=true;return;}
        ln_memory_copy(io,saved,LN_COLLECTION_BYTES);
        ln_write_le32(io,0x54534c4cu);ln_write_le32(io+4,1);ln_write_le32(io+8,generation+1u);ln_write_le32(io+12,0);
        snapshot=revision;running_crc=UINT32_MAX;offset=0;phase=6;return;
    }
    /* Snapshot bytes stay fixed while the live bank accepts newer edits. Read
     * verification uses the same bounded CRC slices as snapshot preparation. */
    if(phase==6){
        if(!write)return;
        uint32_t bytes=LN_COLLECTION_BYTES-offset;if(bytes>4096u)bytes=4096u;
        running_crc=crc_chunk(io+offset,bytes,offset==0);offset+=bytes;
        if(offset==LN_COLLECTION_BYTES){snapshot_crc=running_crc^UINT32_MAX;ln_write_le32(io+12,snapshot_crc);offset=BODY;phase=2;}
        return;
    }
    uint32_t size=phase==5?0:phase==3?BODY:4096u;
    if(size>LN_COLLECTION_BYTES-offset)size=LN_COLLECTION_BYTES-offset;
    if(pending){uint32_t transferred=0;int result=ln_storage_async_poll_deferred(ticket,&transferred);if(!result)return;pending=false;
        if(result!=1||transferred!=size){failed=true;return;}
        if(phase==1||phase==4)running_crc=crc_chunk(io+offset,size,offset==0);
        offset+=size;
        if(phase==1&&offset==LN_COLLECTION_BYTES){
            if(valid(io)){uint32_t seq=ln_read_le32(io+8);
                if(seq==generation&&generation){for(uint32_t i=0;i<LN_COLLECTION_BYTES;i++)if(saved[i]!=io[i]){failed=true;return;}}
                if(seq>generation){ln_memory_copy(saved,io,LN_COLLECTION_BYTES);generation=seq;bank=loading;}}
            if(!loading){loading=1;offset=0;running_crc=UINT32_MAX;}else{ready=true;phase=0;if(!generation)failed=true;}
        }else if(phase==2&&offset==LN_COLLECTION_BYTES){phase=3;offset=0;}
        else if(phase==3){phase=5;offset=0;}
        else if(phase==5){phase=4;offset=0;running_crc=UINT32_MAX;}
        else if(phase==4&&offset==LN_COLLECTION_BYTES){
            if(!valid(io)||ln_read_le32(io+8)!=generation+1u||ln_read_le32(io+12)!=snapshot_crc){failed=true;return;}
            generation++;bank^=1u;durable=snapshot;phase=0;
        }
        return;
    }
    if((phase==1&&!read)||(phase!=1&&!write))return;
    /* Opening a media file can invalidate the FAT proof between chunks. */
    if((phase==2||phase==3)&&!ln_storage_prepared()){
        int proof=ln_storage_prepare_writes();if(!proof)return;if(proof<0){failed=true;return;}
    }
    int result=phase==5?ln_storage_async_flush(&ticket):phase==1||phase==4?ln_storage_async_read(LN_STORAGE_LIST0+(phase==1?loading:bank^1u),offset,io+offset,size,&ticket):
        ln_storage_async_write(LN_STORAGE_LIST0+(bank^1u),offset,io+offset,size,&ticket);
    if(result<0)failed=true;else pending=result==1;
}
void ln_collections_step(bool read,bool write){if(stepping)return;stepping=true;step(read,write);stepping=false;}

void ln_collections_played(uint64_t id){
    if(!ln_collections_writable())return;uint32_t count=ln_read_le32(saved+RECENT),at=0;
    while(at<count&&ln_read_le64(saved+RECENT+8+8*at)!=id)at++;
    if(!at&&count)return;
    if(at==count&&count<RECENT_MAX)count++;
    if(at>=RECENT_MAX)at=RECENT_MAX-1;
    while(at){ln_write_le64(saved+RECENT+8+8*at,ln_read_le64(saved+RECENT+8+8*(at-1)));at--;}
    ln_write_le64(saved+RECENT+8,id);ln_write_le32(saved+RECENT,count);++revision;
}
uint32_t ln_collections_recent_rank(uint64_t id){
    if(!ready)return 0;uint32_t count=ln_read_le32(saved+RECENT);
    for(uint32_t i=0;i<count;i++)if(ln_read_le64(saved+RECENT+8+8*i)==id)return RECENT_MAX-i;return 0;
}
uint32_t ln_collections_recent_count(void){return ready?ln_read_le32(saved+RECENT):0;}
bool ln_collections_recent_id(uint32_t position,uint64_t *id){
    if(!ready||!id||position>=ln_read_le32(saved+RECENT))return false;
    *id=ln_read_le64(saved+RECENT+8u+position*8u);return true;
}
