#include "services/video/catalog.h"
#include "services/video/video.h"
#include "services/storage/storage.h"
#include "hal/api/ata.h"
#include "services/storage/block.h"
#if defined(LINNET_AUDIO)
#include "services/music/player.h"
#endif
#include "boot/include/linnet_bytes.h"
/* The host prepares at most 256 video rows. Targets choose the cache policy
 * in their manifests without changing the shared catalog format. */
#define CATALOG_ROWS 4u
#define CATALOG_MAX_ROWS 256u
#define CATALOG_HEADER_SIZE 12u
#define CATALOG_ROW_SIZE 416u
#define CATALOG_PAGED LINNET_VIDEO_CATALOG_PAGED
static struct {
#if CATALOG_PAGED
    uint8_t header[CATALOG_HEADER_SIZE],pages[2][CATALOG_ROWS*CATALOG_ROW_SIZE];
#else
    uint8_t header[CATALOG_HEADER_SIZE],pages[CATALOG_MAX_ROWS*CATALOG_ROW_SIZE];
#endif
    struct ln_fat32_file file;
#if CATALOG_PAGED
    uint32_t phase,count,revision,wanted,slot,first[2],rows[2],read_size;
#else
    uint32_t phase,count,revision,wanted,read_page,next_page,read_size;
    uint64_t loaded;
#endif
    bool pending,want_page,warming;
    char selected[256];
} catalog;
#define CATALOG_GENERATION 0x73000001u
#if !CATALOG_PAGED
static bool awake(void){
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    struct ln_block_info block;ln_block_snapshot(&block);
    return disk.powered&&disk.initialized&&!disk.initializing&&!disk.busy&&
        !disk.sleeping&&!disk.failed&&!block.power_transition;
}
#endif

void ln_video_library_load(void){catalog.warming=false;if(!catalog.phase){catalog.phase=1;++catalog.revision;}}
uint32_t ln_video_library_count(void){return catalog.count?catalog.count:1u;}
uint32_t ln_video_library_revision(void){return catalog.revision;}
bool ln_video_library_empty(void){return catalog.phase>=4u&&!catalog.count;}
static const uint8_t *catalog_row(uint32_t row){
#if CATALOG_PAGED
    for(uint32_t slot=0;slot<2u;++slot)
        if(row>=catalog.first[slot]&&row-catalog.first[slot]<catalog.rows[slot])
            return catalog.pages[slot]+(row-catalog.first[slot])*CATALOG_ROW_SIZE;
    catalog.wanted=(row/CATALOG_ROWS)*CATALOG_ROWS;catalog.want_page=true;return 0;
#else
    uint32_t page=row/CATALOG_ROWS;
    if(catalog.loaded&(1ull<<page))return catalog.pages+row*CATALOG_ROW_SIZE;
    catalog.wanted=page;catalog.want_page=true;return 0;
#endif
}
const char *ln_video_library_label(uint32_t row){
    if(catalog.phase<4)return "Loading videos...";
    if(!catalog.count)return catalog.phase==5?"Video library unavailable":"No videos. Use Media Manager.";
    if(row>=catalog.count)return "";
    const uint8_t*entry=catalog_row(row);
    return entry?(const char*)entry+288u:"Loading videos...";
}
const char *ln_video_library_path(uint32_t row){
    if((catalog.phase!=4&&catalog.phase!=6)||row>=catalog.count)return 0;
    const uint8_t*entry=catalog_row(row);if(!entry)return 0;
    const char*path=(const char*)entry+32u;uint32_t i=0;
    while(path[i]&&i<255u){catalog.selected[i]=path[i];++i;}
    catalog.selected[i]=0;return catalog.selected;
}
void ln_video_catalog_step(bool busy){
#if !CATALOG_PAGED
    if(!catalog.phase&&!busy&&ln_storage_mounted()&&!ln_storage_busy()){
        bool idle=true;
#if defined(LINNET_AUDIO)
        idle=!ln_player_busy();
#endif
        /* Fill the existing resident list during an already awake startup
         * window. Prefetch never powers up or wakes a disk on its own. */
        if(idle&&awake()){
            ln_video_library_load();catalog.warming=true;
        }
    }
#endif
    if(!catalog.phase||catalog.phase==5||busy)return;
    /* A terminal mount failure cannot admit an open. Do not leave the
     * placeholder loading forever; admitted I/O still drains below first. */
    if(!catalog.pending&&ln_storage_startup_complete()&&!ln_storage_mounted()){
        catalog.phase=5;catalog.count=0;catalog.warming=catalog.want_page=false;
        ++catalog.revision;return;
    }
    int result;
    if(catalog.pending){
        uint32_t transferred=0;
        result=ln_storage_media_poll_deferred(CATALOG_GENERATION,&catalog.file,&transferred);
        if(!result)return;catalog.pending=false;
        if(result<0){
            /* A missing index is an empty library. A missing page in an
             * already opened index is corruption and must not be retried. */
            catalog.phase=catalog.phase==1&&result==LN_FAT32_NOT_FOUND?4:5;
            if(catalog.phase==5)catalog.count=0;
            catalog.warming=false;
            ++catalog.revision;return;
        }
        if(catalog.phase==1){
            if(catalog.file.size<CATALOG_HEADER_SIZE||catalog.file.size>CATALOG_HEADER_SIZE+CATALOG_MAX_ROWS*CATALOG_ROW_SIZE){catalog.phase=5;++catalog.revision;return;}
            catalog.phase=2;return;
        }
        if(catalog.phase==2){
            const uint8_t*b=catalog.header;uint32_t count=ln_read_le32(b+4);
            bool valid=transferred==CATALOG_HEADER_SIZE&&ln_read_le32(b)==0x5849564cu&&count<=CATALOG_MAX_ROWS&&ln_read_le32(b+8)==CATALOG_ROW_SIZE&&catalog.file.size==CATALOG_HEADER_SIZE+count*CATALOG_ROW_SIZE;
            catalog.count=valid?count:0;catalog.phase=valid?4:5;
            if(!catalog.count)catalog.warming=false;
#if CATALOG_PAGED
            catalog.want_page=valid&&count;catalog.wanted=0;
#else
            catalog.loaded=0;catalog.next_page=0;catalog.want_page=false;
#endif
            ++catalog.revision;return;
        }
        bool valid=transferred==catalog.read_size;
        uint32_t rows=catalog.read_size/CATALOG_ROW_SIZE;
        for(uint32_t row=0;valid&&row<rows;++row){
#if CATALOG_PAGED
            const uint8_t*entry=catalog.pages[catalog.slot]+row*CATALOG_ROW_SIZE;const uint8_t*name=entry+32;
#else
            const uint8_t*entry=catalog.pages+(catalog.read_page*CATALOG_ROWS+row)*CATALOG_ROW_SIZE;const uint8_t*name=entry+32;
#endif
            bool terminated=false;if(name[0]!='/')valid=false;for(uint32_t j=0;j<256u;++j)if(!name[j]){terminated=true;break;}if(!terminated||!entry[288]||entry[415])valid=false;
        }
#if CATALOG_PAGED
        if(valid){catalog.rows[catalog.slot]=rows;catalog.phase=4;catalog.slot^=1u;}
        else{catalog.count=0;catalog.phase=5;catalog.rows[0]=catalog.rows[1]=0;}
#else
        if(valid){
            for(uint32_t page=0;page<(rows+CATALOG_ROWS-1u)/CATALOG_ROWS;++page)
                catalog.loaded|=1ull<<(catalog.read_page+page);
            uint32_t pages=(catalog.count+CATALOG_ROWS-1u)/CATALOG_ROWS;
            uint64_t complete=pages==64u?UINT64_MAX:(1ull<<pages)-1ull;
            if(catalog.loaded==complete)catalog.warming=false;
            catalog.phase=4;
        }
        else{catalog.count=0;catalog.phase=5;catalog.loaded=0;}
#endif
        ++catalog.revision;return;
    }
    /* Synchronous image validation can reenter the runtime between FAT reads.
     * Its owner keeps storage busy even while the current FAT lock is free. */
    if(ln_storage_busy())return;
#if defined(LINNET_AUDIO)
    if(catalog.warming&&ln_player_busy())return;
#endif
#if !CATALOG_PAGED
    if(catalog.warming){
        if(!awake())return;
    }
#endif
    if(catalog.phase==4){
#if CATALOG_PAGED
        if(!catalog.want_page)return;
        uint32_t first=catalog.wanted;catalog.want_page=false;
        if(first>=catalog.count)return;
        for(uint32_t slot=0;slot<2u;++slot)if(catalog.rows[slot]&&catalog.first[slot]==first)return;
        catalog.first[catalog.slot]=first;catalog.rows[catalog.slot]=0;
#else
        uint32_t page_count=(catalog.count+CATALOG_ROWS-1u)/CATALOG_ROWS;
        while(catalog.next_page<page_count&&(catalog.loaded&(1ull<<catalog.next_page)))++catalog.next_page;
        if(catalog.want_page&&catalog.wanted<page_count&&!(catalog.loaded&(1ull<<catalog.wanted)))catalog.read_page=catalog.wanted;
        else if(catalog.next_page<page_count)catalog.read_page=catalog.next_page;
        else return;
        catalog.want_page=false;
        uint32_t first=catalog.read_page*CATALOG_ROWS;
#endif
        uint32_t rows=catalog.count-first;
#if CATALOG_PAGED
        if(rows>CATALOG_ROWS)rows=CATALOG_ROWS;
#else
        /* Eight aligned pages fit even the smallest 16 KiB media read budget. */
        if(rows>CATALOG_ROWS*8u)rows=CATALOG_ROWS*8u;
        /* Never DMA over an already published page. A wanted page can sit
         * immediately before pages populated by earlier foreground reads. */
        for(uint32_t page=1;page<(rows+CATALOG_ROWS-1u)/CATALOG_ROWS;++page)
            if(catalog.loaded&(1ull<<(catalog.read_page+page))){rows=page*CATALOG_ROWS;break;}
#endif
        catalog.read_size=rows*CATALOG_ROW_SIZE;catalog.phase=6;
    }
    if(catalog.phase==2)result=ln_storage_media_read(&catalog.file,0,catalog.header,CATALOG_HEADER_SIZE,CATALOG_GENERATION);
#if CATALOG_PAGED
    else if(catalog.phase==6)result=ln_storage_media_read(&catalog.file,CATALOG_HEADER_SIZE+catalog.first[catalog.slot]*CATALOG_ROW_SIZE,catalog.pages[catalog.slot],catalog.read_size,CATALOG_GENERATION);
#else
    else if(catalog.phase==6)result=ln_storage_media_read(&catalog.file,CATALOG_HEADER_SIZE+catalog.read_page*CATALOG_ROWS*CATALOG_ROW_SIZE,catalog.pages+catalog.read_page*CATALOG_ROWS*CATALOG_ROW_SIZE,catalog.read_size,CATALOG_GENERATION);
#endif
    else result=ln_storage_media_open("/.LINNET/VIDEOS/INDEX.VID",CATALOG_GENERATION);
    if(result>0)catalog.pending=true;
    else if(result<0){catalog.count=0;catalog.phase=5;++catalog.revision;}
}
bool ln_video_catalog_pending(void){return catalog.pending;}
uint8_t *ln_video_catalog_files_memory(uint32_t *bytes,uint32_t *epoch){
    if(!bytes||!epoch)return 0;*bytes=*epoch=0;
#if defined(LINNET_SOC_PP5022) && !CATALOG_PAGED
    if(catalog.phase!=4u||catalog.pending)return 0;
    uint32_t pages=(catalog.count+CATALOG_ROWS-1u)/CATALOG_ROWS;
    uint64_t complete=pages==64u?UINT64_MAX:(1ull<<pages)-1ull;
    if(catalog.loaded!=complete)return 0;
    struct ln_fat32_file root;
    if(!ln_storage_media_root(&root)||catalog.file.epoch!=root.epoch)return 0;
    uintptr_t first=((uintptr_t)catalog.pages+catalog.count*CATALOG_ROW_SIZE+31u)&~(uintptr_t)31u;
    uintptr_t end=(uintptr_t)catalog.pages+sizeof catalog.pages;
    if(first>=end)return 0;
    *bytes=(uint32_t)(end-first);*epoch=root.epoch;return (uint8_t*)first;
#else
    return 0;
#endif
}
