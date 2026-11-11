#include "library.h"
#include "queue.h"
#include "services/storage/storage.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#if defined(LINNET_COVERFLOW)
#include "coverflow.h"
#endif
extern void ln_memory_copy(void *,const void *,uint32_t);

#define LIBRARY_PAGES 24u
#define TITLE_TRACKS 20000u
#define TITLE_BYTES (704u*1024u)
#define TITLE_PENDING 0x80000000u
static struct {
    uint32_t offsets[TITLE_TRACKS];
    uint16_t heap[TITLE_TRACKS];
    char text[TITLE_BYTES];
} titles __attribute__((section(".music_titles"),aligned(32)));
static uint32_t title_count,title_used,title_length,title_source;
static bool title_reading;
static bool group_names_enabled;
#if defined(LINNET_MUSIC_METADATA_BYTES)
static bool group_names_decided;
#endif
static uint8_t title_previous;
static char title_pending[256];
#if defined(LINNET_MUSIC_METADATA_BYTES)
static uint8_t metadata_local[LINNET_MUSIC_METADATA_BYTES] __attribute__((section(".music_metadata"),aligned(32)));
static uint8_t *metadata=metadata_local;
static uint32_t metadata_capacity=sizeof metadata_local;
static uint32_t metadata_offset,metadata_bytes;
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
static uint32_t metadata_tail_offset,metadata_tail_bytes,metadata_tail_captured,metadata_tail_epoch;
static bool metadata_tail_decided,metadata_tail_ready;
static void metadata_tail_reset(void){
    ln_coverflow_metadata_release(metadata_tail_epoch);
    metadata_tail_offset=metadata_tail_bytes=metadata_tail_captured=metadata_tail_epoch=0;
    metadata_tail_decided=metadata_tail_ready=false;
}
static void metadata_tail_capture(uint32_t offset,const uint8_t *buffer,uint32_t bytes,uint32_t file_size){
    if(!metadata_tail_decided){
        metadata_tail_decided=true;
        if(!metadata_bytes||metadata_offset+metadata_bytes>=file_size)return;
        uint32_t overlap=metadata_bytes<4096u?metadata_bytes:4096u;
        metadata_tail_offset=metadata_offset+metadata_bytes-overlap;
        metadata_tail_bytes=file_size-metadata_tail_offset;
        if(!ln_coverflow_metadata_reserve(metadata_tail_bytes,&metadata_tail_epoch))return;
    }
    uint8_t *memory=ln_coverflow_metadata_memory(metadata_tail_epoch);
    if(!memory)return;
    uint32_t first=offset,end=offset+bytes;
    if(first<metadata_tail_offset)first=metadata_tail_offset;
    if(end>file_size)end=file_size;
    if(first>=end)return;
    if(first!=metadata_tail_offset+metadata_tail_captured){
        ln_coverflow_metadata_release(metadata_tail_epoch);return;
    }
    ln_memory_copy(memory+metadata_tail_captured,buffer+first-offset,end-first);
    metadata_tail_captured+=end-first;
}
#endif
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
void ln_music_library_memory_diagnostics(uint32_t out[4]){
    out[0]=(uint32_t)(uintptr_t)metadata;out[1]=metadata_capacity;
    out[2]=metadata_offset;out[3]=metadata_bytes;
}
#endif
#endif

enum job { JOB_NONE=0,JOB_WINDOW,JOB_SELECTION,JOB_SEARCH,JOB_GROUP,JOB_MEMBER,JOB_PREFETCH,JOB_QUEUE,JOB_VIEW,JOB_VIEW_GROUP,JOB_GROUP_RECORD,JOB_TRACK_RECORD,JOB_TRACK_IDS,JOB_MEMBERS };
/* Page validity lives in the reset state; payload is filled before publication. */
static uint8_t pages[LIBRARY_PAGES][4096] __attribute__((section(".media_workspace"),aligned(32)));
struct library {
    struct ln_music_db db;
    /* Bounded shared page cache: titles, group/member tables and search indexes.
     * Tags publish only after a complete read; reload invalidates every page. */
    uint32_t page_tags[LIBRARY_PAGES],page_used[LIBRARY_PAGES],page_clock;
    bool page_valid[LIBRARY_PAGES];
    struct ln_fat32_file file;
    struct ln_music_library_row rows[LN_MUSIC_LIBRARY_ROWS],scratch,selection,prefetch;
    struct ln_music_group group;
    union {char prefix[256];struct ln_music_track record;};
    union {char group_name[256];uint64_t ids[LN_MUSIC_ID_BATCH];uint32_t members[LN_MUSIC_ID_BATCH];};
    uint32_t generation,revision,first,job_index,selection_index,selection_token,job_token;
    uint64_t selection_id;
    uint32_t query_index,query_ordinal,query_result,io_offset,io_size;
    uint8_t *io_buffer;
    bool valid[LN_MUSIC_LIBRARY_ROWS],io_active,paused,reopen;
    bool selection_pending,selection_ready,query_pending,query_ready;
    bool prefetch_pending,prefetch_ready;
    uint32_t prefetch_index,prefetch_token;
    uint32_t queue_group,queue_ordinal,queue_token,queue_result,queue_done_token,queue_job_group,queue_job_ordinal,queue_job_token;
    bool queue_pending,queue_ready;
    enum ln_music_library_status status;
    enum job job,query;
    uint8_t stage,guard_stage;
    int error;
#if defined(LINNET_TARGET_VIDEO5G_M25)
    uint32_t verify_window_first,verify_window_bytes;
    bool verify_window_valid,io_harvest;
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
    uint32_t verify_flags,verify_start_us,verify_hash_us,verify_ready_us;
    uint32_t verify_reads,verify_bytes,verify_io_start_us,verify_io_max_us;
#endif
};
static struct library library;
#if defined(LINNET_TARGET_VIDEO5G_M25)
void ln_music_library_verification_reset(void){library.verify_window_valid=false;}
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
void ln_music_library_verification_diagnostics(uint32_t words[8]){
    words[0]=0x57424456u;words[1]=library.verify_flags;
    words[2]=library.verify_start_us;words[3]=library.verify_hash_us;words[4]=library.verify_ready_us;
    words[5]=library.verify_reads;words[6]=library.verify_bytes;words[7]=library.verify_io_max_us;
}
#endif
static uint32_t next_generation(void);
#if LINNET_MUSIC_BROWSE_INDEX
/* Verification's title heap is no longer needed once the PODB is verified. */
static uint8_t *browse_buffer;
static uint8_t *browse_data(void){return browse_buffer;}
static struct ln_fat32_file browse_file;
static uint32_t browse_phase,browse_at,browse_size,browse_file_size,browse_base_size;
static uint32_t browse_prefix_at,browse_prefix_count;
static uint32_t browse_fence_at,browse_fence_count;
static bool browse_valid;
static uint32_t browse_crc;
static uint32_t browse_id_at,browse_id_crc,browse_id_scan;
static uint64_t browse_id_previous;
static bool browse_ids_valid;
static bool browse_check(void){
    const uint8_t *data=browse_data();
    if(browse_base_size<128u||ln_read_le32(data)!=0x4952424cu||ln_read_le32(data+4)!=browse_file_size||
       ln_read_le32(data+20)!=library.db.groups||ln_crc32(data,60)!=ln_read_le32(data+60))return false;
    if((browse_crc^UINT32_MAX)!=ln_read_le32(data+56))return false;
    for(uint32_t i=0;i<32;i++)if(data[24+i]!=library.db.expected_hash[i])return false;
    uint32_t artists=ln_read_le32(data+8),links=ln_read_le32(data+12),owners=ln_read_le32(data+16);
    uint64_t end=128ull+12ull*artists+4ull*links+4ull*owners;
    if(end>browse_base_size)return false;
    browse_prefix_count=browse_fence_count=0;
    {
        if(browse_base_size-end<12u)return false;
        uint32_t count=ln_read_le32(data+(uint32_t)end);
        if(!count||count>65537u||end+4ull+8ull*count>browse_base_size)return false;
        uint32_t prior_key=0,prior_ordinal=0;
        for(uint32_t i=0;i<count;i++){
            const uint8_t *r=data+(uint32_t)end+4u+i*8u;
            uint32_t key=ln_read_le32(r),ordinal=ln_read_le32(r+4);
            if(key>65536u||ordinal>library.db.searches||(!i&&ordinal)||
               (i&&(key<=prior_key||ordinal<=prior_ordinal)))return false;
            prior_key=key;prior_ordinal=ordinal;
        }
        if(prior_key!=65536u||prior_ordinal!=library.db.searches)return false;
        browse_prefix_at=(uint32_t)end+4u;browse_prefix_count=count;
        end+=4ull+8ull*count;
        {
            if(browse_base_size-end<4u)return false;
            count=ln_read_le32(data+(uint32_t)end);
            if(count!=(library.db.searches+63u)/64u||end+4ull+36ull*count!=browse_base_size)return false;
            for(uint32_t i=0;i<count;i++){
                const uint8_t *r=data+(uint32_t)end+4u+i*36u;
                if(ln_read_le32(r)!=i*64u)return false;
                if(i){for(uint32_t j=4;j<36;j++){
                    if(r[j]<(r-36)[j])return false;
                    if(r[j]>(r-36)[j])break;
                }}
            }
            browse_fence_at=(uint32_t)end+4u;browse_fence_count=count;
        }
    }
    for(uint32_t i=0;i<8;i++){
        uint32_t first=ln_read_le32(data+64+i*8),count=ln_read_le32(data+68+i*8);
        if(first>library.db.groups||count>library.db.groups-first)return false;
    }
    uint32_t next=0,previous=0;
    for(uint32_t i=0;i<artists;i++){
        const uint8_t *r=data+128+i*12;uint32_t id=ln_read_le32(r),first=ln_read_le32(r+4),count=ln_read_le32(r+8);
        if(id>=library.db.groups||(i&&id<=previous)||first!=next||count>links-next)return false;
        previous=id;next+=count;
    }
    if(next!=links)return false;
    for(uint32_t i=0;i<links+owners;i++)if(ln_read_le32(data+128+artists*12+i*4)>=library.db.groups)return false;
    return true;
}
bool ln_music_library_track_id_lookup(uint64_t id,uint32_t *ordinal){
    if(library.status!=LN_LIBRARY_READY||!browse_valid||!browse_ids_valid||!ordinal)return false;
    const uint8_t *data=browse_data()+browse_id_at;
    uint32_t lo=0,hi=library.db.tracks;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2u;
        if(ln_read_le64(data+mid*12u)<id)lo=mid+1u;else hi=mid;}
    *ordinal=lo<library.db.tracks&&ln_read_le64(data+lo*12u)==id?
        ln_read_le32(data+lo*12u+8u):UINT32_MAX;
    return true;
}
bool ln_music_library_id_entry(uint32_t position,uint64_t *id,uint32_t *ordinal){
    if(library.status!=LN_LIBRARY_READY||!browse_valid||!browse_ids_valid||
       position>=library.db.tracks||!id||!ordinal)return false;
    const uint8_t *row=browse_data()+browse_id_at+position*12u;
    *id=ln_read_le64(row);*ordinal=ln_read_le32(row+8u);return true;
}
static uint32_t browse_prefix_lower(uint32_t key){
    const uint8_t *data=browse_data()+browse_prefix_at;
    uint32_t lo=0,hi=browse_prefix_count-1u;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2u;
        if(ln_read_le32(data+mid*8u)<key)lo=mid+1u;else hi=mid;}
    return ln_read_le32(data+lo*8u+4u);
}
static int browse_fence_compare(uint32_t ordinal){
    const uint8_t *key=browse_data()+browse_fence_at+ordinal*36u+4u;
    for(uint32_t i=0;i<32u&&library.db.prefix[i];i++){
        if(key[i]<(uint8_t)library.db.prefix[i])return -1;
        if(key[i]>(uint8_t)library.db.prefix[i])return 1;
    }
    return 0;
}
static void browse_search_bounds(void){
    if(!browse_valid||!browse_prefix_count||!library.db.prefix[0])return;
    uint32_t key=(uint32_t)(uint8_t)library.db.prefix[0]<<8;
    uint32_t end=key+256u;
    if(library.db.prefix[1]){key|=(uint8_t)library.db.prefix[1];end=key+1u;}
    uint32_t lo=browse_prefix_lower(key),hi=browse_prefix_lower(end);
    /* The host stores the first ordinal of every two-byte key. Short
     * prefixes already have an exact lower bound; still validate the final
     * candidate reference, string and prefix through the normal DB query. */
    if(library.db.search_prefix_length<=2u){library.db.low=library.db.high=lo;return;}
    if(browse_fence_count){
        uint32_t first=0,last=browse_fence_count;
        while(first<last){uint32_t mid=first+(last-first)/2u;
            if(browse_fence_compare(mid)<0)first=mid+1u;else last=mid;}
        uint32_t lower=first?(first-1u)*64u:0u;
        if(lo<lower)lo=lower;
        last=browse_fence_count;
        while(first<last){uint32_t mid=first+(last-first)/2u;
            if(browse_fence_compare(mid)<=0)first=mid+1u;else last=mid;}
        uint32_t upper=first<browse_fence_count?first*64u:library.db.searches;
        if(hi>upper)hi=upper;
    }
    if(library.db.low<lo)library.db.low=lo;
    if(library.db.high>hi)library.db.high=hi;
}
static bool browse_load(void){
    if(browse_phase==3)return true;
    if(library.paused&&!library.io_active)return false;
    if(browse_phase==2){
        const uint8_t *ids=browse_data()+browse_id_at;
        uint32_t end=browse_id_scan+64u;if(end>library.db.tracks)end=library.db.tracks;
        for(;browse_id_scan<end;browse_id_scan++){
            const uint8_t *row=ids+browse_id_scan*12u;
            uint64_t id=ln_read_le64(row);
            if((browse_id_scan&&id<=browse_id_previous)||ln_read_le32(row+8)>=library.db.tracks){
                browse_phase=3;return true;
            }
            browse_id_previous=id;
        }
        if(browse_id_scan==library.db.tracks){browse_ids_valid=true;browse_phase=3;return true;}
        return false;
    }
    if(!library.io_active){
        uint32_t bytes=browse_size-browse_at;if(bytes>4096u)bytes=4096u;
        int result=browse_phase==0?ln_storage_media_open("/.LINNET/BROWSE.BIN",library.generation):
            ln_storage_media_read(&browse_file,browse_at,browse_data()+browse_at,bytes,library.generation);
        if(result<0){browse_phase=3;return true;}if(!result)return false;
        library.io_active=true;return false;
    }
    uint32_t bytes=0;int result=ln_storage_media_poll(library.generation,&browse_file,&bytes);
    if(!result)return false;
    library.io_active=false;library.generation=next_generation();
    if(result<0){browse_phase=3;return true;}
    if(!browse_phase){
        browse_file_size=browse_file.size;
        uint64_t footer=4ull+12ull*library.db.tracks;
        if(browse_file_size<footer+128u){browse_phase=3;return true;}
        browse_base_size=browse_file_size-(uint32_t)footer;
        browse_size=browse_file_size;
        if(browse_size>sizeof titles.heap&&browse_size>TITLE_BYTES-title_used)browse_size=browse_base_size;
        if(browse_size<=sizeof titles.heap)browse_buffer=(uint8_t*)titles.heap;
        else if(browse_size<=TITLE_BYTES-title_used)browse_buffer=(uint8_t*)titles.text+title_used;
        else {browse_phase=3;return true;}
        browse_crc=UINT32_MAX;
        browse_id_crc=UINT32_MAX;
        browse_phase=1;return false;
    }
    uint32_t expected=browse_size-browse_at;if(expected>4096u)expected=4096u;
    if(bytes!=expected){browse_phase=3;return true;}
    uint32_t start=browse_at,end=start+bytes;
    if(end>64u&&start<browse_base_size){
        uint32_t first=start<64u?64u:start,last=end>browse_base_size?browse_base_size:end;
        browse_crc=ln_crc32_update(browse_crc,browse_data()+first,last-first);
    }
    if(browse_size==browse_file_size&&end>browse_base_size+4u){
        uint32_t first=start<browse_base_size+4u?browse_base_size+4u:start;
        browse_id_crc=ln_crc32_update(browse_id_crc,browse_data()+first,end-first);
    }
    browse_at+=bytes;if(browse_at<browse_size)return false;
    browse_valid=browse_check();
    if(browse_valid&&browse_size==browse_file_size&&
       (browse_id_crc^UINT32_MAX)==ln_read_le32(browse_data()+browse_base_size)){
        browse_id_at=browse_base_size+4u;browse_id_scan=0;browse_id_previous=0;
        browse_phase=2;return false;
    }
    browse_phase=3;return true;
}
#else
bool ln_music_library_track_id_lookup(uint64_t id,uint32_t *ordinal){
    (void)id;(void)ordinal;return false;
}
bool ln_music_library_id_entry(uint32_t position,uint64_t *id,uint32_t *ordinal){
    (void)position;(void)id;(void)ordinal;return false;
}
#endif
bool ln_music_library_albums(uint32_t artist,uint32_t *out,uint32_t capacity,uint32_t *count){
#if LINNET_MUSIC_BROWSE_INDEX
    if(library.status!=LN_LIBRARY_READY||!browse_valid||!out||!count)return false;
    const uint8_t *data=browse_data();uint32_t artists=ln_read_le32(data+8),links=ln_read_le32(data+12),first=0,n=0;
    if(artist==UINT32_MAX){first=links;n=ln_read_le32(data+16);}
    else{
        uint32_t lo=0,hi=artists;
        while(lo<hi){uint32_t mid=lo+(hi-lo)/2;if(ln_read_le32(data+128+mid*12)<artist)lo=mid+1;else hi=mid;}
        if(lo==artists||ln_read_le32(data+128+lo*12)!=artist)return false;
        first=ln_read_le32(data+132+lo*12);n=ln_read_le32(data+136+lo*12);
    }
    if(n>capacity)return false;
    for(uint32_t i=0;i<n;i++)out[i]=ln_read_le32(data+128+artists*12+(first+i)*4);
    *count=n;return true;
#else
    (void)artist;(void)out;(void)capacity;(void)count;return false;
#endif
}
bool ln_music_library_group_range(uint32_t kind,uint32_t *first,uint32_t *count){
#if LINNET_MUSIC_BROWSE_INDEX
    if(library.status!=LN_LIBRARY_READY||!browse_valid||kind<1||kind>8||!first||!count)return false;
    *first=ln_read_le32(browse_data()+64+(kind-1)*8);*count=ln_read_le32(browse_data()+68+(kind-1)*8);return true;
#else
    (void)kind;(void)first;(void)count;return false;
#endif
}
static bool window_enabled=true;
static uint32_t generation=0x10000000u;
static void clear(void *memory,uint32_t count){
#if defined(__arm__)
    extern void ln_memory_clear(void *,uint32_t);ln_memory_clear(memory,count);
#else
    uint8_t *p=memory;for(uint32_t i=0;i<count;i++)p[i]=0;
#endif
}
static void text_copy(char out[256],const char *in){uint32_t i=0;while(i<255&&in[i]){out[i]=in[i];i++;}out[i]=0;}
static uint32_t next_generation(void){if(++generation>=0x20000000u)generation=0x10000000u;return generation;}
static void changed(void){library.revision++;}
static void fail(int error)
{
#if defined(LINNET_TARGET_VIDEO5G_M25)
    ln_music_library_verification_reset();
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
    library.verify_flags|=8u;
#endif
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    metadata_tail_ready=false;
#endif
    library.error=error;library.job=JOB_NONE;library.reopen=false;
    library.status=library.io_active?LN_LIBRARY_DRAINING:LN_LIBRARY_ERROR;
    if(library.io_active)ln_storage_media_cancel(library.generation);
    changed();
}
static bool ready(void){return library.status==LN_LIBRARY_READY;}
static void title_push(uint32_t track,uint32_t offset){
    titles.offsets[track]=offset|TITLE_PENDING;
    uint32_t at=title_count++;
    while(at){uint32_t parent=(at-1u)/2u;if(titles.offsets[titles.heap[parent]]<=titles.offsets[track])break;
        titles.heap[at]=titles.heap[parent];at=parent;}
    titles.heap[at]=(uint16_t)track;
}
static uint32_t title_pop(void){
    uint32_t result=titles.heap[0],last=titles.heap[--title_count],at=0;
    while(at*2u+1u<title_count){uint32_t child=at*2u+1u;
        if(child+1u<title_count&&titles.offsets[titles.heap[child+1u]]<titles.offsets[titles.heap[child]])child++;
        if(titles.offsets[last]<=titles.offsets[titles.heap[child]])break;
        titles.heap[at]=titles.heap[child];at=child;}
    if(title_count)titles.heap[at]=(uint16_t)last;return result;
}
/* Harvest only completed verification reads. The cache is inaccessible until
 * the database hash passes; no extra disk reads or shared audio/artwork memory. */
static void title_capture(uint32_t offset,const uint8_t *data,uint32_t bytes){
    struct ln_music_db *d=&library.db;
    if(d->phase!=1)return;
    uint32_t limit=d->tracks<TITLE_TRACKS?d->tracks:TITLE_TRACKS;
    uint32_t first=offset>d->track_offset?(offset-d->track_offset)/LN_MUSIC_TRACK_BYTES:0;
    for(uint32_t i=first;i<limit;i++){
        uint32_t field=d->track_offset+i*LN_MUSIC_TRACK_BYTES+52u;
        if(field>=offset+bytes)break;
        if(field>=offset&&field+4u<=offset+bytes){uint32_t value=ln_read_le32(data+field-offset);if(value<d->pool_bytes)title_push(i,value);}
    }
    /* Queue labels under the existing heap bound; targets without a resident
     * group table retain the entire title budget for their original consumer. */
#if defined(LINNET_MUSIC_METADATA_BYTES)
    uint32_t groups=d->groups;if(groups>TITLE_TRACKS-limit)groups=TITLE_TRACKS-limit;
    first=offset>d->group_offset?(offset-d->group_offset)/32u:0u;
    for(uint32_t i=first;i<groups;i++){
        uint32_t field=d->group_offset+i*32u+4u;
        if(field>=offset+bytes)break;
        if(field>=offset&&field+4u<=offset+bytes){uint32_t value=ln_read_le32(data+field-offset);if(value<d->pool_bytes)title_push(limit+i,value);}
    }
#endif
    if(offset+bytes<=d->pool_offset)return;
#if defined(LINNET_MUSIC_METADATA_BYTES)
    if(!group_names_decided){
        group_names_decided=true;
        uint32_t table=d->groups*32u;
        if(d->group_offset>=metadata_offset&&d->group_offset-metadata_offset<=metadata_bytes&&
           table<=metadata_bytes-(d->group_offset-metadata_offset)){
            uint64_t artists=0,albums=0,links=0;
            const uint8_t *p=metadata+d->group_offset-metadata_offset;
            for(uint32_t i=0;i<d->groups;i++,p+=32u){
                uint32_t kind=ln_read_le32(p);
                if(kind==1u){artists++;links+=ln_read_le32(p+16);}
                else if(kind==2u)albums++;
            }
            /* The compiler gives each track one album. An artist therefore
             * has at most one relation per member, plus its owned albums.
             * Reserve the browse footer, fences and worst-case prefix table,
             * as well as the entire pool, before admitting extra strings. */
            uint64_t prefixes=d->searches<65536u?(uint64_t)d->searches+1u:65537u;
            uint64_t browse=140u+artists*12u+(links+albums)*4u+albums*4u+
                prefixes*8u+((uint64_t)d->searches+63u)/64u*36u+(uint64_t)d->tracks*12u;
            group_names_enabled=d->pool_bytes<=TITLE_BYTES&&
                (browse<=sizeof titles.heap||browse<=TITLE_BYTES-d->pool_bytes);
        }
    }
#endif
    uint32_t at=offset<d->pool_offset?d->pool_offset-offset:0;
    uint8_t previous=title_previous;title_previous=data[bytes-1u];
    while(at<bytes&&title_count){
        uint32_t source=offset+at-d->pool_offset;
        if(!title_reading){
            while(title_count&&!group_names_enabled&&titles.heap[0]>=limit)(void)title_pop();
            if(!title_count)break;
            title_source=titles.offsets[titles.heap[0]]&~TITLE_PENDING;
            if(title_source>source){uint32_t skip=title_source-source;if(skip>=bytes-at)return;at+=skip;}
            else if(title_source<source){(void)title_pop();continue;}
            if(title_source&&(at?data[at-1u]:previous)){(void)title_pop();continue;}
            title_reading=true;title_length=0;
        }
        uint8_t c=data[at++];
        if(title_length<sizeof title_pending)title_pending[title_length]=c;
        ++title_length;
        if(c)continue;
        bool valid=title_length<=sizeof title_pending&&ln_music_db_utf8((const uint8_t*)title_pending,title_length-1u)&&title_length<=TITLE_BYTES-title_used;
        uint32_t saved=title_used;
        if(valid){for(uint32_t i=0;i<title_length;i++)titles.text[title_used++]=title_pending[i];}
        do{uint32_t track=title_pop();if(valid)titles.offsets[track]=saved;}
        while(title_count&&(titles.offsets[titles.heap[0]]&~TITLE_PENDING)==title_source);
        title_reading=false;
    }
}
const char *ln_music_library_title(uint32_t index){
    if(!ready()||index>=library.db.tracks||index>=TITLE_TRACKS||titles.offsets[index]&TITLE_PENDING)return 0;
    return titles.text+titles.offsets[index];
}
static const uint8_t *resident_metadata(uint32_t offset,uint32_t bytes){
#if defined(LINNET_MUSIC_METADATA_BYTES)
    if(ready()&&offset>=metadata_offset&&offset-metadata_offset<=metadata_bytes&&
       bytes<=metadata_bytes-(offset-metadata_offset))return metadata+(offset-metadata_offset);
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    if(ready()&&metadata_tail_ready&&offset>=metadata_tail_offset&&
       offset-metadata_tail_offset<=metadata_tail_bytes&&bytes<=metadata_tail_bytes-(offset-metadata_tail_offset)){
        struct ln_fat32_file root;
        uint8_t *memory=ln_coverflow_metadata_memory(metadata_tail_epoch);
        if(memory&&ln_storage_media_root(&root)&&root.epoch==library.file.epoch)
            return memory+offset-metadata_tail_offset;
    }
#endif
#else
    (void)offset;(void)bytes;
#endif
    return 0;
}
bool ln_music_library_cached_group(uint32_t index,struct ln_music_group *out,const char **name){
    if(!out||!ready()||index>=library.db.groups)return false;
    const uint8_t *p=resident_metadata(library.db.group_offset+index*32u,32u);if(!p)return false;
    struct ln_music_group g={.kind=ln_read_le32(p),.name=ln_read_le32(p+4),.parent=ln_read_le32(p+8),
        .first=ln_read_le32(p+12),.count=ln_read_le32(p+16),.id=ln_read_le64(p+24)};
    if(g.kind<1u||g.kind>8u||g.name>=library.db.pool_bytes||g.first>library.db.members||
       g.count>library.db.members-g.first||ln_read_le32(p+20)||
       (g.kind==2u?(g.parent>=library.db.groups||g.parent==index):g.parent!=UINT32_MAX))return false;
    if(name){
        uint32_t base=library.db.tracks<TITLE_TRACKS?library.db.tracks:TITLE_TRACKS;
        if(index>=TITLE_TRACKS-base||titles.offsets[base+index]&TITLE_PENDING)return false;
        *name=titles.text+titles.offsets[base+index];
    }
    *out=g;return true;
}
bool ln_music_library_cached_member_at(uint32_t member,uint32_t *out){
    if(!out||!ready()||member>=library.db.members)return false;
    const uint8_t *p=resident_metadata(library.db.member_offset+member*4u,4u);
    if(!p)return false;uint32_t index=ln_read_le32(p);if(index>=library.db.tracks)return false;
    *out=index;return true;
}
bool ln_music_library_cached_member(uint32_t group,uint32_t ordinal,uint32_t *out){
    struct ln_music_group g;
    if(!out||!ln_music_library_cached_group(group,&g,0)||ordinal>=g.count)return false;
    return ln_music_library_cached_member_at(g.first+ordinal,out);
}
uint8_t *ln_music_library_catalog_memory(uint32_t *bytes){
    if(!bytes||!ready())return 0;
    uintptr_t first=(uintptr_t)titles.text+title_used,end=(uintptr_t)titles.text+TITLE_BYTES;
#if LINNET_MUSIC_BROWSE_INDEX
    uintptr_t browse=(uintptr_t)browse_buffer;
    if(browse>=first&&browse<end)first=browse+browse_size;
#endif
    first=(first+31u)&~(uintptr_t)31u;
    if(first>=end){*bytes=0;return 0;}
    *bytes=(uint32_t)(end-first);return (uint8_t*)first;
}
uint8_t *ln_music_library_art_memory(uint32_t *bytes){
#if defined(LINNET_MUSIC_METADATA_BYTES)
    if(bytes&&ready()&&metadata!=metadata_local&&metadata_bytes<=metadata_capacity){
        uint32_t at=(metadata_bytes+31u)&~31u;
        if(at<metadata_capacity){*bytes=metadata_capacity-at;return metadata+at;}
    }
#else
    (void)bytes;
#endif
    return 0;
}
uint8_t *ln_music_library_files_memory(uint32_t *bytes,uint32_t *epoch){
    if(!bytes||!epoch)return 0;*bytes=*epoch=0;
#if defined(LINNET_SOC_PP5022) && defined(LINNET_MUSIC_METADATA_BYTES) && defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
    if(ready()&&metadata!=metadata_local){
        struct ln_fat32_file root;
        if(ln_storage_media_root(&root)&&library.file.epoch==root.epoch){
            *bytes=sizeof metadata_local;*epoch=root.epoch;return metadata_local;
        }
    }
#endif
    return 0;
}

static bool superseded_job(void){
    return (library.job==JOB_WINDOW&&!window_enabled)||
        (library.job==JOB_SELECTION&&library.job_token!=library.selection_token)||
        (library.job==JOB_PREFETCH&&library.job_token!=library.prefetch_token)||
        (library.job==JOB_QUEUE&&library.queue_job_token!=library.queue_token);
}
static bool retire_superseded_job(void){
    if(!superseded_job())return false;
    /* The DB scratch buffer still belongs to an admitted storage request.
     * Retire that owner before abandoning its parser operation; the newest
     * selection/prefetch/resolver intent remains pending. */
    if(library.io_active){
        ln_storage_media_cancel(library.generation);
        if(!ln_storage_media_poll(library.generation,0,0))return true;
        library.io_active=false;library.io_buffer=0;
        library.generation=next_generation();
    }
    library.db.busy=library.db.pending=false;
    library.job=JOB_NONE;library.stage=0;
    return true;
}
uint32_t ln_music_library_deadline(uint32_t now){
    /* An admitted media request is advanced by storage, not another library
     * turn. Keep its deadline out of the executive's immediate-work path. */
    if(library.io_active)return superseded_job()?now:now+200000u;
    if(library.paused)return now+200000u;
    if(library.status==LN_LIBRARY_OPENING||library.status==LN_LIBRARY_VERIFYING||
       library.status==LN_LIBRARY_DRAINING)return now;
    if(!ready())return now+200000u;
    if(library.job!=JOB_NONE||library.selection_pending||library.selection_ready||
       library.prefetch_pending||library.prefetch_ready||library.queue_pending||library.queue_ready||library.query_pending||library.query_ready)return now;
    for(uint32_t i=0;window_enabled&&i<LN_MUSIC_LIBRARY_ROWS&&library.first+i<library.db.tracks;++i)
        if(!library.valid[i])return now;
    return now+200000u;
}
static void start(void)
{
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    metadata_tail_reset();
#endif
#if LINNET_MUSIC_BROWSE_INDEX
    browse_phase=browse_at=browse_size=browse_file_size=browse_base_size=0;
    browse_valid=false;browse_buffer=0;
    browse_ids_valid=false;
#endif
#if defined(LINNET_MUSIC_METADATA_BYTES)
    metadata_offset=metadata_bytes=0;
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
    uint32_t extra_bytes;uint8_t *extra=ln_platform_extended_memory(&extra_bytes);
    metadata=extra?extra+65536u:metadata_local;
    metadata_capacity=extra?16u*1024u*1024u-65536u:sizeof metadata_local;
#endif
#endif
    uint32_t revision=library.revision+1u;
    title_count=title_used=title_length=0;title_reading=false;
    group_names_enabled=false;
#if defined(LINNET_MUSIC_METADATA_BYTES)
    group_names_decided=false;
#endif
    for(uint32_t i=0;i<TITLE_TRACKS;i++)titles.offsets[i]=TITLE_PENDING;
    bool paused=library.paused;
    clear(&library,sizeof(library));library.status=LN_LIBRARY_OPENING;
    library.revision=revision;library.paused=paused;library.generation=next_generation();
}
void ln_music_library_init(void){start();}
static void capture_database_chunk(uint32_t offset,const uint8_t *buffer,uint32_t bytes)
{
    if(library.status!=LN_LIBRARY_VERIFYING)return;
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
    if(library.db.phase==1)library.verify_bytes+=bytes;
#endif
#if defined(LINNET_MUSIC_METADATA_BYTES)
    /* Reuse verification I/O, publishing only after the database hash passes.
     * The independent title and LRU caches cover any nonresident data. */
    if(library.file.size<=metadata_capacity){metadata_offset=0;metadata_bytes=library.file.size;}
    else if(library.db.phase==1){
        metadata_offset=library.db.group_offset;
        metadata_bytes=library.file.size-metadata_offset;
        if(metadata_bytes>metadata_capacity)metadata_bytes=metadata_capacity;
    }
    uint32_t first=offset,end=first+bytes;
    if(first<metadata_offset)first=metadata_offset;
    if(end>metadata_offset+metadata_bytes)end=metadata_offset+metadata_bytes;
    if(first<end)ln_memory_copy(metadata+(first-metadata_offset),buffer+(first-offset),end-first);
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    if(library.db.phase==1)metadata_tail_capture(offset,buffer,bytes,library.file.size);
#endif
#endif
    title_capture((uint32_t)offset,buffer,bytes);
}
static int read_database_io(uint64_t offset,uint8_t *buffer,uint32_t bytes,bool harvest)
{
    if(offset>library.file.size||!bytes||bytes>(harvest?4096u:16384u)||bytes>library.file.size-offset)return LN_DB_ARGUMENT;
    if(library.io_active){
        if(library.io_offset!=(uint32_t)offset||library.io_size!=bytes||library.io_buffer!=buffer)return LN_DB_ARGUMENT;
#if defined(LINNET_TARGET_VIDEO5G_M25)
        if(library.io_harvest!=harvest)return LN_DB_ARGUMENT;
#endif
        uint32_t transferred=0;
        int result=0;
        /* A metadata request is at most 16 KiB. Group ready FAT/cache transitions
         * without recursive runtime work between each sector; yield as soon
         * as hardware really waits. Cancellation retains its ordinary drain. */
        for(uint32_t step=0;step<4u;++step){
            result=ln_storage_media_poll_deferred(library.generation,0,&transferred);
            if(result)break;
            uint32_t now=ln_time_us(),deadline;
            if(ln_storage_idle_wait(now,&deadline)&&(int32_t)(deadline-now)>0)break;
        }
        if(!result)return 0;
        library.io_active=false;library.io_buffer=0;
        library.generation=next_generation();
        if(result==1&&transferred==bytes){
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
            if(library.status==LN_LIBRARY_VERIFYING&&library.db.phase==1){
                uint32_t elapsed=ln_time_us()-library.verify_io_start_us;
                if(elapsed>library.verify_io_max_us)library.verify_io_max_us=elapsed;
            }
#endif
            if(harvest)capture_database_chunk((uint32_t)offset,buffer,bytes);
            return 1;
        }
        return LN_DB_IO;
    }
    if(library.paused)return 0;
    int result=ln_storage_media_read(&library.file,(uint32_t)offset,buffer,bytes,library.generation);
    if(result!=1)return result;
#if defined(LINNET_TARGET_VIDEO5G_M25)
    library.io_harvest=harvest;
#endif
    library.io_active=true;library.io_offset=(uint32_t)offset;library.io_size=bytes;library.io_buffer=buffer;
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
    if(library.status==LN_LIBRARY_VERIFYING&&library.db.phase==1){
        ++library.verify_reads;library.verify_io_start_us=ln_time_us();
    }
#endif
    return 0;
}
static int read_database_raw(void *context,uint64_t offset,uint8_t *buffer,uint32_t bytes)
{
    (void)context;return read_database_io(offset,buffer,bytes,true);
}
#if defined(LINNET_TARGET_VIDEO5G_M25)
static int read_verification_window(uint64_t offset,uint8_t *buffer,uint32_t bytes){
    if(offset>library.file.size||!bytes||bytes>4096u||bytes>library.file.size-offset)return LN_DB_ARGUMENT;
    /* Pause cached hashing/capture work too. An already admitted request may
     * finish and return its one logical chunk, matching the ordinary reader. */
    if(library.paused&&!library.io_active)return 0;
    uint32_t first=(uint32_t)offset&~16383u;
    if(!library.verify_window_valid||library.verify_window_first!=first){
        library.verify_window_valid=false;
        uint32_t count=library.file.size-first;if(count>16384u)count=16384u;
        int result=read_database_io(first,(uint8_t *)pages,count,false);
        if(result!=1)return result;
        library.verify_window_first=first;library.verify_window_bytes=count;
        library.verify_window_valid=true;
    }
    uint32_t within=(uint32_t)offset-first;
    if(within>library.verify_window_bytes||bytes>library.verify_window_bytes-within)return LN_DB_ARGUMENT;
    /* Page tags remain invalid. The parser still hashes and captures only its
     * original <=4 KiB chunk, in order, after exact physical read completion. */
    ln_memory_copy(buffer,(const uint8_t *)pages+within,bytes);
    capture_database_chunk((uint32_t)offset,buffer,bytes);
    return 1;
}
#endif
static int read_database(void *context,uint64_t offset,uint8_t *buffer,uint32_t bytes)
{
    if(!ready()){
#if defined(LINNET_TARGET_VIDEO5G_M25)
        if(library.status==LN_LIBRARY_VERIFYING&&library.db.phase==1)
            return read_verification_window(offset,buffer,bytes);
#endif
        return read_database_raw(context,offset,buffer,bytes);
    }
    if(offset>library.file.size||!bytes||bytes>4096u||bytes>library.file.size-offset)return LN_DB_ARGUMENT;
#if defined(LINNET_MUSIC_METADATA_BYTES)
    const uint8_t *resident=resident_metadata((uint32_t)offset,bytes);
    if(resident){
        ln_memory_copy(buffer,resident,bytes);
        return 1;
    }
#endif
    uint32_t copied=0;
    while(copied<bytes){
        uint32_t address=(uint32_t)offset+copied,page=address/4096u,slot=LIBRARY_PAGES;
        /* Fully associative replacement keeps record and string pages together,
         * even when their file offsets differ by a multiple of cache capacity. */
        for(uint32_t i=0;i<LIBRARY_PAGES;i++)
            if(library.page_valid[i]&&library.page_tags[i]==page){slot=i;break;}
        if(slot==LIBRARY_PAGES){
            if(library.io_active){
                for(uint32_t i=0;i<LIBRARY_PAGES;i++)if(library.io_buffer==pages[i]){slot=i;break;}
                if(slot==LIBRARY_PAGES)return LN_DB_ARGUMENT;
            }else{
                slot=0;
                for(uint32_t i=0;i<LIBRARY_PAGES;i++){
                    if(!library.page_valid[i]){slot=i;break;}
                    if((uint32_t)(library.page_clock-library.page_used[i])>
                       (uint32_t)(library.page_clock-library.page_used[slot]))slot=i;
                }
            }
        }
        uint32_t base=page*4096u,size=library.file.size-base;
        if(size>4096u)size=4096u;
        if(!library.page_valid[slot]||library.page_tags[slot]!=page){
            library.page_valid[slot]=false;
            int result=read_database_raw(context,base,pages[slot],size);
            if(result!=1)return result;
            library.page_tags[slot]=page;library.page_valid[slot]=true;
        }
        library.page_used[slot]=++library.page_clock;
        uint32_t within=address-base,count=size-within;
        if(count>bytes-copied)count=bytes-copied;
        ln_memory_copy(buffer+copied,pages[slot]+within,count);
        copied+=count;
    }
    return 1;
}
static void stop(bool reopen)
{
#if defined(LINNET_TARGET_VIDEO5G_M25)
    ln_music_library_verification_reset();
#endif
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    metadata_tail_ready=false;
#endif
    library.reopen=reopen;library.status=LN_LIBRARY_DRAINING;
    library.error=0;
    library.selection_ready=library.query_ready=false;
    library.prefetch_ready=library.prefetch_pending=false;library.queue_pending=library.queue_ready=false;
    if(library.io_active)ln_storage_media_cancel(library.generation);
    changed();
}
void ln_music_library_reload(void){stop(true);}
void ln_music_library_cancel(void){stop(false);}
void ln_music_library_pause(bool pause){library.paused=pause;}
enum ln_music_library_status ln_music_library_status(void){return library.status;}
int ln_music_library_error(void){return library.error;}
uint32_t ln_music_library_count(void){return ready()?library.db.tracks:0;}
uint32_t ln_music_library_group_count(void){return ready()?library.db.groups:0;}
uint32_t ln_music_library_revision(void){return library.revision;}
uint32_t ln_music_library_verified_bytes(void){return library.db.cursor;}
bool ln_music_library_identity(uint8_t out[32])
{
    if(!out||!ready()||!library.db.verified)return false;
    for(uint32_t i=0;i<32;i++)out[i]=library.db.expected_hash[i];
    return true;
}
void ln_music_library_window_enable(bool enabled){window_enabled=enabled;}
void ln_music_library_window(uint32_t first)
{
    if(library.first==first)return;
    library.first=first;
    for(uint32_t i=0;i<LN_MUSIC_LIBRARY_ROWS;i++)library.valid[i]=false;
    changed();
}
const struct ln_music_library_row *ln_music_library_row(uint32_t index)
{
    if(!ready()||index<library.first||index-library.first>=LN_MUSIC_LIBRARY_ROWS)return 0;
    uint32_t slot=index-library.first;
    return library.valid[slot]?&library.rows[slot]:0;
}
bool ln_music_library_select(uint32_t index)
{
    if(!ready()||index>=library.db.tracks)return false;
    library.selection_index=index;library.selection_token++;
    library.selection_id=0;
    library.selection_ready=false;library.selection_pending=true;return true;
}
bool ln_music_library_select_id(uint64_t id)
{
    if(!ready()||!id)return false;
    library.selection_id=id;library.selection_index=0;library.selection_token++;
    library.selection_ready=false;library.selection_pending=true;return true;
}
bool ln_music_library_take_selection(struct ln_music_library_row *out)
{
    if(!out||!ready()||!library.selection_ready)return false;
    *out=library.selection;library.selection_ready=false;return true;
}
bool ln_music_library_prefetch(uint32_t index){
    if(!ready()||index>=library.db.tracks)return false;
    library.prefetch_index=index;++library.prefetch_token;
    library.prefetch_ready=false;library.prefetch_pending=true;return true;
}
bool ln_music_library_take_prefetch(struct ln_music_library_row *out){
    if(!out||!ready()||!library.prefetch_ready)return false;
    *out=library.prefetch;library.prefetch_ready=false;return true;
}
void ln_music_library_cancel_prefetch(void){
    ++library.prefetch_token;library.prefetch_pending=library.prefetch_ready=false;
}
bool ln_music_library_queue_member(uint32_t group,uint32_t ordinal,uint32_t token){
    group&=~LN_QUEUE_REVERSE;
    if(!ready()||!token||group>=library.db.groups)return false;
    library.queue_group=group;library.queue_ordinal=ordinal;library.queue_token=token;
    library.queue_pending=true;library.queue_ready=false;return true;
}
bool ln_music_library_take_queue_member(uint32_t *track,uint32_t *token){
    if(!ready()||!track||!token||!library.queue_ready)return false;
    *track=library.queue_result;*token=library.queue_done_token;library.queue_ready=false;return true;
}
static void queue_complete(uint32_t track){
    if(library.queue_job_token==library.queue_token){library.queue_result=track;
        library.queue_done_token=library.queue_job_token;library.queue_pending=false;library.queue_ready=true;}
    library.job=JOB_NONE;
}
static void queue_step(void){
    if(!library.stage){if(!ln_music_db_request(&library.db,LN_DB_GROUP,library.queue_job_group)){fail(LN_DB_ARGUMENT);return;}library.stage=1;return;}
    int result=ln_music_db_step(&library.db);if(result<0){fail(result);return;}if(!result)return;
    if(library.stage==1){if(library.queue_job_ordinal>=library.db.group.count){queue_complete(UINT32_MAX);return;}
        if(!ln_music_db_request(&library.db,LN_DB_MEMBER,library.db.group.first+library.queue_job_ordinal)){fail(LN_DB_ARGUMENT);return;}
        library.stage=2;return;}
    queue_complete(library.db.member);
}
static bool query_admit(enum job job,uint32_t index,uint32_t ordinal)
{
    if(!ready()||library.query_pending||library.query_ready)return false;
    library.query=job;library.query_index=index;library.query_ordinal=ordinal;
    library.query_pending=true;return true;
}
bool ln_music_library_search_at(const char *prefix,uint32_t ordinal)
{
    if(!prefix)return false;
    uint32_t n=0;while(n<256&&prefix[n])n++;
    if(n==256||!query_admit(JOB_SEARCH,0,ordinal))return false;
    text_copy(library.prefix,prefix);return true;
}
bool ln_music_library_take_search_row(struct ln_music_library_search_row *out)
{
    if(!out||!ready()||library.query!=JOB_SEARCH||!library.query_ready)return false;
    out->index=library.query_result;text_copy(out->title,library.group_name);
    out->kind=library.query_result!=UINT32_MAX&&(library.query_result&LN_MUSIC_SEARCH_GROUP)?library.group.kind:0;
    out->count=out->kind?library.group.count:0;
    library.query_ready=false;return true;
}
bool ln_music_library_view_row(uint32_t source,uint32_t group,uint32_t ordinal)
{
    group&=~LN_QUEUE_REVERSE;
    if(!ready()||source>2)return false;
    if(source==2)return group<library.db.groups&&query_admit(JOB_VIEW_GROUP,group,ordinal);
    return ordinal<library.db.tracks&&query_admit(JOB_VIEW,ordinal,0);
}
bool ln_music_library_take_view_row(struct ln_music_library_search_row *out)
{
    if(!out||!ready()||!library.query_ready||
       (library.query!=JOB_VIEW&&library.query!=JOB_VIEW_GROUP))return false;
    out->index=library.query_result;text_copy(out->title,library.group_name);
    out->kind=out->count=0;
    library.query_ready=false;return true;
}
bool ln_music_library_track_record(uint32_t index){return ready()&&index<library.db.tracks&&query_admit(JOB_TRACK_RECORD,index,0);}
bool ln_music_library_track_ids(uint32_t first){return ready()&&first<library.db.tracks&&query_admit(JOB_TRACK_IDS,first,0);}
bool ln_music_library_take_track_ids(uint64_t ids[LN_MUSIC_ID_BATCH],uint32_t *count){
    if(!ids||!count||!ready()||library.query!=JOB_TRACK_IDS||!library.query_ready)return false;
    *count=library.query_result;for(uint32_t i=0;i<*count;i++)ids[i]=library.ids[i];library.query_ready=false;return true;
}
bool ln_music_library_take_track_record(struct ln_music_track *out){
    if(!out||!ready()||library.query!=JOB_TRACK_RECORD||!library.query_ready)return false;
    *out=library.record;library.query_ready=false;return true;
}
bool ln_music_library_group(uint32_t index)
{
    return ready()&&index<library.db.groups&&query_admit(JOB_GROUP,index,0);
}
bool ln_music_library_group_record(uint32_t index)
{
    return ready()&&index<library.db.groups&&query_admit(JOB_GROUP_RECORD,index,0);
}
bool ln_music_library_take_group(struct ln_music_group *out,char name[256])
{
    if(!out||!name||!ready()||(library.query!=JOB_GROUP&&library.query!=JOB_GROUP_RECORD)||!library.query_ready)return false;
    *out=library.group;text_copy(name,library.group_name);library.query_ready=false;return true;
}
bool ln_music_library_group_member(uint32_t index,uint32_t ordinal)
{
    index&=~LN_QUEUE_REVERSE;return ready()&&index<library.db.groups&&query_admit(JOB_MEMBER,index,ordinal);
}
bool ln_music_library_group_members(uint32_t index,uint32_t ordinal){
    index&=~LN_QUEUE_REVERSE;return ready()&&index<library.db.groups&&query_admit(JOB_MEMBERS,index,ordinal);
}
bool ln_music_library_take_members(uint32_t indices[LN_MUSIC_ID_BATCH],uint32_t *count){
    if(!indices||!count||!ready()||library.query!=JOB_MEMBERS||!library.query_ready)return false;
    *count=library.query_result;for(uint32_t i=0;i<*count;i++)indices[i]=library.members[i];library.query_ready=false;return true;
}
bool ln_music_library_take_member(uint32_t *index)
{
    if(!index||!ready()||library.query!=JOB_MEMBER||!library.query_ready)return false;
    *index=library.query_result;library.query_ready=false;return true;
}
static bool request(enum ln_music_db_request operation,uint32_t index)
{
    if(ln_music_db_request(&library.db,operation,index))return true;
    fail(LN_DB_FORMAT);return false;
}
static void row_complete(void)
{
    if(!ln_music_db_safe_path(library.scratch.path)){fail(LN_DB_FORMAT);return;}
    uint32_t index=library.scratch.index;
    if(index>=library.first&&index-library.first<LN_MUSIC_LIBRARY_ROWS){
        uint32_t slot=index-library.first;
        library.rows[slot]=library.scratch;library.valid[slot]=true;
        if(slot%6u==5u||index+1u==library.db.tracks||library.job==JOB_SELECTION)changed();
    }
    if(library.job==JOB_SELECTION&&library.job_token==library.selection_token){
        library.selection=library.scratch;library.selection_ready=true;
        library.selection_pending=false;
    }
    if(library.job==JOB_PREFETCH&&library.job_token==library.prefetch_token){
        library.prefetch=library.scratch;library.prefetch_ready=true;library.prefetch_pending=false;
    }
    library.job=JOB_NONE;
}
static void row_step(void)
{
    if(library.job==JOB_SELECTION&&library.selection_id){
        uint32_t ordinal;
        if(ln_music_library_track_id_lookup(library.selection_id,&ordinal)){
            library.selection_id=0;
            if(ordinal==UINT32_MAX){
                clear(&library.selection,sizeof library.selection);library.selection.index=UINT32_MAX;
                library.selection_pending=false;library.selection_ready=true;library.job=JOB_NONE;return;
            }
            library.job_index=ordinal;return;
        }
        uint32_t count=library.db.tracks-library.job_index;
        if(count>LN_MUSIC_ID_BATCH)count=LN_MUSIC_ID_BATCH;
        if(count){
            int result=read_database(0,library.db.track_offset+library.job_index*LN_MUSIC_TRACK_BYTES,library.db.buffer,count*LN_MUSIC_TRACK_BYTES);
            if(result<0){fail(result);return;}if(!result)return;
            for(uint32_t i=0;i<count;i++)if(ln_read_le64(library.db.buffer+i*LN_MUSIC_TRACK_BYTES)==library.selection_id){
                library.job_index+=i;library.selection_id=0;return;
            }
            library.job_index+=count;return;
        }
        clear(&library.selection,sizeof library.selection);library.selection.index=UINT32_MAX;
        library.selection_pending=false;library.selection_ready=true;library.selection_id=0;library.job=JOB_NONE;return;
    }
    if(!library.stage){
        clear(&library.scratch,sizeof(library.scratch));library.scratch.index=library.job_index;
        if(request(LN_DB_TRACK,library.job_index))library.stage=1;
        return;
    }
    int result=ln_music_db_step(&library.db);
    if(result<0){fail(result);return;}if(!result)return;
    if(library.stage==1){
        library.scratch.track=library.db.track;
        const char *title=ln_music_library_title(library.job_index);
        if(title){text_copy(library.scratch.title,title);if(request(LN_DB_STRING,library.scratch.track.artist))library.stage=3;}
        else if(request(LN_DB_STRING,library.scratch.track.title))library.stage=2;
    }
    else if(library.stage==2){text_copy(library.scratch.title,library.db.text);if(request(LN_DB_STRING,library.scratch.track.artist))library.stage=3;}
    else if(library.stage==3){
        text_copy(library.scratch.artist,library.db.text);
        if(library.job!=JOB_WINDOW){if(request(LN_DB_STRING,library.scratch.track.album))library.stage=7;}
        else if(request(LN_DB_STRING,library.scratch.track.path))library.stage=4;
    }
    else if(library.stage==7){
        text_copy(library.scratch.album,library.db.text);library.scratch.album_ready=true;
        if(request(LN_DB_STRING,library.scratch.track.path))library.stage=4;
    }
    else if(library.stage==8){
        text_copy(library.scratch.album,library.db.text);library.scratch.album_ready=true;
        if(library.scratch.track.art!=UINT32_MAX&&!library.scratch.art_path[0]){
            if(request(LN_DB_ART,library.scratch.track.art))library.stage=5;
        }else row_complete();
    }
    else if(library.stage==4){
        text_copy(library.scratch.path,library.db.text);
        if(library.job!=JOB_WINDOW&&library.scratch.track.art!=UINT32_MAX){
            if(request(LN_DB_ART,library.scratch.track.art))library.stage=5;
        }else row_complete();
    }else if(library.stage==5){library.scratch.art_crc=library.db.art.crc;
        if(request(LN_DB_STRING,library.db.art.path))library.stage=6;
    }else{
        static const char prefix[]="/.LINNET/ART/";
        bool valid=ln_music_db_safe_path(library.db.text);
        for(uint32_t i=0;i<sizeof prefix-1u;++i)if(library.db.text[i]!=prefix[i])valid=false;
        if(!valid){fail(LN_DB_FORMAT);return;}
        text_copy(library.scratch.art_path,library.db.text);row_complete();
    }
}
static void query_complete(uint32_t result)
{
    library.query_result=result;library.query_pending=false;library.query_ready=true;
    library.job=JOB_NONE;
    /* The consuming view publishes visible changes; offscreen prefetch must not repaint. */
}
static void query_step(void)
{
    if(library.job==JOB_MEMBERS&&library.stage==2){
        uint32_t count=library.group.count-library.query_ordinal;if(count>LN_MUSIC_ID_BATCH)count=LN_MUSIC_ID_BATCH;
        int result=read_database(0,library.db.member_offset+(library.group.first+library.query_ordinal)*4u,library.db.buffer,count*4u);
        if(result<0){fail(result);return;}if(!result)return;
        for(uint32_t i=0;i<count;i++){
            uint32_t member=ln_read_le32(library.db.buffer+i*4u);
            if(member>=library.db.tracks){fail(LN_DB_FORMAT);return;}library.members[i]=member;
        }
        query_complete(count);return;
    }
    if(library.job==JOB_TRACK_IDS){
        uint32_t count=library.db.tracks-library.query_index;if(count>LN_MUSIC_ID_BATCH)count=LN_MUSIC_ID_BATCH;
        int result=read_database(0,library.db.track_offset+library.query_index*LN_MUSIC_TRACK_BYTES,library.db.buffer,count*LN_MUSIC_TRACK_BYTES);
        if(result<0){fail(result);return;}if(!result)return;
        for(uint32_t i=0;i<count;i++)library.ids[i]=ln_read_le64(library.db.buffer+i*LN_MUSIC_TRACK_BYTES);
        query_complete(count);return;
    }
    if(!library.stage){
        /* Resident titles share the ordinary query completion path. A cached
         * group member avoids parsing the same verified records again. */
        if(library.job==JOB_VIEW){library.query_result=library.query_index;goto view_title;}
        if(library.job==JOB_VIEW_GROUP&&
           ln_music_library_cached_member(library.query_index,library.query_ordinal,&library.query_result))goto view_title;
        if(library.job==JOB_TRACK_RECORD){
            library.query_result=library.query_index;library.group_name[0]=0;
            if(request(LN_DB_TRACK,library.query_index))library.stage=2;
            return;
        }
        if(library.job==JOB_SEARCH){
            if(!ln_music_db_search_at(&library.db,library.prefix,library.query_ordinal)){fail(LN_DB_ARGUMENT);return;}
#if LINNET_MUSIC_BROWSE_INDEX
            browse_search_bounds();
#endif
        }
        else if(!request(LN_DB_GROUP,library.query_index))return;
        library.stage=1;return;
    }
    int result=ln_music_db_step(&library.db);
    if(result<0){fail(result);return;}if(!result)return;
    if(library.job==JOB_TRACK_RECORD){library.record=library.db.track;query_complete(library.query_index);return;}
    if(library.job==JOB_VIEW_GROUP&&library.stage==1){
        library.group_name[0]=0;
        if(library.query_ordinal>=library.db.group.count){query_complete(UINT32_MAX);return;}
        if(request(LN_DB_MEMBER,library.db.group.first+library.query_ordinal))library.stage=4;
        return;
    }
    if(library.job==JOB_VIEW_GROUP&&library.stage==4){
        library.query_result=library.db.member;
view_title:
        library.group_name[0]=0;
        const char *title=ln_music_library_title(library.query_result);
        if(title){text_copy(library.group_name,title);query_complete(library.query_result);return;}
        if(request(LN_DB_TRACK,library.query_result))library.stage=2;
        return;
    }
    if(library.job==JOB_SEARCH||library.job==JOB_VIEW||library.job==JOB_VIEW_GROUP){
        if(library.stage==1){
            library.query_result=library.db.search_track;library.group_name[0]=0;
            if(library.query_result==UINT32_MAX){query_complete(UINT32_MAX);return;}
            if(library.query_result&LN_MUSIC_SEARCH_GROUP){
                if(request(LN_DB_GROUP,library.query_result&~LN_MUSIC_SEARCH_GROUP))library.stage=5;
                return;
            }
            const char *title=ln_music_library_title(library.query_result);
            if(title){text_copy(library.group_name,title);query_complete(library.query_result);return;}
            if(request(LN_DB_TRACK,library.query_result))library.stage=2;
        }else if(library.stage==5){
            library.group=library.db.group;
            if(library.group.kind!=1&&library.group.kind!=2){fail(LN_DB_FORMAT);return;}
            if(request(LN_DB_STRING,library.group.name))library.stage=3;
        }else if(library.stage==2){
            if(request(LN_DB_STRING,library.db.track.title))library.stage=3;
        }else {text_copy(library.group_name,library.db.text);query_complete(library.query_result);}
        return;
    }
    if(library.stage==1){
        library.group=library.db.group;
        if(library.job==JOB_MEMBERS){
            if(library.query_ordinal>=library.group.count)query_complete(0);else library.stage=2;
            return;
        }
        if(library.job==JOB_GROUP_RECORD){library.group_name[0]=0;query_complete(library.query_index);return;}
        if(library.job==JOB_GROUP){if(request(LN_DB_STRING,library.group.name))library.stage=2;}
        else if(library.query_ordinal>=library.group.count)query_complete(UINT32_MAX);
        else if(request(LN_DB_MEMBER,library.group.first+library.query_ordinal))library.stage=2;
        return;
    }
    if(library.job==JOB_GROUP){text_copy(library.group_name,library.db.text);query_complete(library.query_index);}
    else query_complete(library.db.member);
}
static void library_step_once(void)
{
    if(library.status==LN_LIBRARY_DRAINING){
        if(library.io_active){
            if(!ln_storage_media_poll(library.generation,0,0))return;
            library.io_active=false;library.io_buffer=0;
        }
#if defined(LINNET_COVERFLOW)
        /* A catalog reader can own the unused title tail. Its normal outer
         * step must drain that read before verification reuses the buffer. */
        if(!ln_coverflow_release_catalog())return;
#endif
        if(library.reopen)start();
        else{library.status=library.error?LN_LIBRARY_ERROR:LN_LIBRARY_CLOSED;library.job=JOB_NONE;changed();}
        return;
    }
    if(library.status==LN_LIBRARY_CLOSED||library.status==LN_LIBRARY_ERROR)return;
    if(library.status==LN_LIBRARY_OPENING){
        /* The host retains this marker throughout a grouped index switch.
         * Complete the bounded lookup before opening any music index. */
        if(library.guard_stage<2u){
            if(!library.io_active){
                if(library.paused)return;
                int result=ln_storage_media_open("/.LINNET/TXN/MUSIC/STATE.TXN",library.generation);
                if(result<0){fail(LN_MUSIC_LIBRARY_RECOVERY_REQUIRED);return;}
                if(!result)return;
                library.io_active=true;library.guard_stage=1u;return;
            }
            int result=ln_storage_media_poll(library.generation,0,0);
            if(!result)return;
            library.io_active=false;
            if(result!=LN_FAT32_NOT_FOUND){fail(LN_MUSIC_LIBRARY_RECOVERY_REQUIRED);return;}
            library.guard_stage=2u;library.generation=next_generation();return;
        }
        if(!library.io_active){
            if(library.paused)return;
            int result=ln_storage_media_open("/.LINNET/LIBRARY.PDB",library.generation);
            if(result<0){fail(result);return;}if(!result)return;
            library.io_active=true;return;
        }
        int result=ln_storage_media_poll(library.generation,&library.file,0);
        if(!result)return;
        library.io_active=false;
        if(result<0){fail(result);return;}
        ln_music_db_init(&library.db,read_database,0,library.file.size,library.generation);
        library.generation=next_generation();
        library.status=LN_LIBRARY_VERIFYING;changed();return;
    }
    if(library.status==LN_LIBRARY_VERIFYING){
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
        if(library.db.phase==1&&!(library.verify_flags&1u)){
            library.verify_flags|=1u;library.verify_start_us=ln_time_us();
        }
#endif
        int result=ln_music_db_open_step(&library.db);
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
        if(library.db.verified&&!(library.verify_flags&2u)){
            library.verify_flags|=2u;library.verify_hash_us=ln_time_us();
        }
#endif
        if(result<0)fail(result);
        else if(result==1){
#if defined(LINNET_TARGET_VIDEO5G_M25)
            ln_music_library_verification_reset();
#endif
#if LINNET_MUSIC_BROWSE_INDEX
            if(!browse_load())return;
#endif
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
            metadata_tail_ready=metadata_tail_bytes&&metadata_tail_captured==metadata_tail_bytes&&
                ln_coverflow_metadata_memory(metadata_tail_epoch)!=0;
#endif
            library.status=LN_LIBRARY_READY;changed();
#if defined(LINNET_TARGET_VIDEO5G_M25) && !defined(LINNET_PROFILE_RELEASE)
            library.verify_flags|=4u;library.verify_ready_us=ln_time_us();
#endif
        }
        return;
    }
    if(retire_superseded_job())return;
    if(library.job==JOB_NONE){
        library.stage=0;
        if(library.selection_pending){
            const struct ln_music_library_row *cached=library.selection_id?0:ln_music_library_row(library.selection_index);
            if(cached&&cached->album_ready&&(cached->track.art==UINT32_MAX||cached->art_path[0])){library.selection=*cached;library.selection_pending=false;library.selection_ready=true;return;}
            library.job=JOB_SELECTION;library.job_index=library.selection_index;library.job_token=library.selection_token;
        }else if(library.prefetch_pending){
            const struct ln_music_library_row *cached=ln_music_library_row(library.prefetch_index);
            if(cached&&cached->album_ready&&(cached->track.art==UINT32_MAX||cached->art_path[0])){library.prefetch=*cached;library.prefetch_pending=false;library.prefetch_ready=true;return;}
            library.job=JOB_PREFETCH;library.job_index=library.prefetch_index;library.job_token=library.prefetch_token;
            if(cached){
                /* Window rows already prove track, title, artist and media path.
                 * Resolve only missing playback metadata under the same owner. */
                library.scratch=*cached;
                if(!cached->album_ready){
                    if(request(LN_DB_STRING,cached->track.album))library.stage=8;
                }else if(request(LN_DB_ART,cached->track.art))library.stage=5;
                return;
            }
        }else if(library.queue_pending){library.job=JOB_QUEUE;
            library.queue_job_group=library.queue_group;library.queue_job_ordinal=library.queue_ordinal;library.queue_job_token=library.queue_token;
        }else if(library.query_pending)library.job=library.query;
        else if(window_enabled){
            for(uint32_t i=0;i<LN_MUSIC_LIBRARY_ROWS;i++){
                if(library.first>=library.db.tracks||i>=library.db.tracks-library.first)break;
                if(!library.valid[i]){library.job=JOB_WINDOW;library.job_index=library.first+i;break;}
            }
        }
    }
    if(library.job==JOB_WINDOW||library.job==JOB_SELECTION||library.job==JOB_PREFETCH)row_step();
    else if(library.job==JOB_QUEUE)queue_step();
    else if(library.job!=JOB_NONE)query_step();
}

void ln_music_library_step(void)
{
    for(uint32_t step=0;step<32u;step++){
        library_step_once();
        if(!ready()||library.io_active||library.paused||library.job==JOB_NONE)break;
    }
}

/* Only the outer controller may pump additional metadata work. */
bool ln_music_library_can_continue(void)
{
    return ready()&&!library.paused&&!library.io_active;
}
