#include "services/storage/resident.h"
#include "services/storage/block.h"
#include "boot/include/linnet_bytes.h"
#include "hal/api/platform.h"

#define EOC 0x0ffffff8u
#define ADMISSION_US 20000u
void ln_resident_forget(struct ln_resident_binding *b){b->file.epoch=0;b->path[0]=0;}
bool ln_resident_remember(struct ln_resident_binding *b,const char *path,const struct ln_fat32_file *file){
    uint32_t n=0;
    if(!path||path[0]!='/'||!file||!file->epoch||!file->size||file->cluster<2u||(file->attributes&24u))goto invalid;
    while(n<=LN_FAT32_PATH_MAX&&path[n])++n;
    if(n<2u||n>LN_FAT32_PATH_MAX)goto invalid;
    b->file=*file;for(uint32_t i=0;i<=n;++i)b->path[i]=path[i];return true;
invalid:ln_resident_forget(b);return false;
}
bool ln_resident_match(const struct ln_resident_binding *b,const char *path,uint32_t epoch,struct ln_fat32_file *file){
    if(!epoch||b->file.epoch!=epoch||!b->path[0]||!path||!file)return false;
    for(uint32_t i=0;i<=LN_FAT32_PATH_MAX;++i){
        if(b->path[i]!=path[i])return false;
        if(!path[i]){*file=b->file;return true;}
    }
    return false;
}
static void copy(void *to,const void *from,uint32_t bytes){
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);ln_memory_copy(to,from,bytes);
#else
    for(uint32_t i=0;i<bytes;++i)((uint8_t *)to)[i]=((const uint8_t *)from)[i];
#endif
}
void ln_resident_end(struct ln_resident_media *r){ln_block_cache_end(r->cookie);r->cookie=0;}
bool ln_resident_valid(const struct ln_resident_media *r,uint32_t epoch){
    return r->epoch==epoch&&ln_block_cache_valid(r->cookie);
}
static bool bounded(struct ln_resident_media *r){
    if(ln_time_us()-r->started<ADMISSION_US)return true;
    r->failure=2u;return false;
}
static int cached_io(void *context,enum ln_fat32_io op,uint64_t lba,uint8_t *out,uint32_t generation){
    struct ln_resident_media *r=context;(void)generation;
    if(op!=LN_FAT32_READ||!bounded(r))return -1;
    if(ln_block_cache_copy(r->cookie,lba,1,out))return 1;
    r->missing_lba=lba;return -1;
}
static bool cluster_valid(const struct ln_resident_media *r,uint32_t cluster){return cluster>=2u&&cluster-2u<r->clusters;}
static const uint8_t *sector(const struct ln_resident_media *r,uint64_t lba){
    if(lba<r->partition||lba-r->partition>=r->sectors)return 0;
    return ln_block_cache_sector(r->cookie,lba);
}
static bool next_cluster(const struct ln_resident_media *r,uint32_t cluster,uint32_t *next,uint32_t *verified){
    if(!cluster_valid(r,cluster)||cluster/128u>=r->fat_sectors)return false;
    uint64_t lba=r->fat_start+(uint64_t)r->active_fat*r->fat_sectors+cluster/128u;
    const uint8_t *a=sector(r,lba);if(!a)return false;
    if(verified&&*verified!=cluster/128u&&r->mirrored&&r->fats==2u){
        const uint8_t *b=sector(r,lba+r->fat_sectors);
        if(!b||!ln_bytes_equal(a,b,512u))return false;
    }
    /* Both sectors stay pinned and immutable. Repeated edges within the same
     * sector need no second whole-sector comparison in this proof walk. */
    if(verified)*verified=cluster/128u;
    *next=ln_read_le32(a+(cluster%128u)*4u)&0x0fffffffu;
    return *next>=EOC||cluster_valid(r,*next);
}
static bool file_equal(const struct ln_fat32_file *a,const struct ln_fat32_file *b){
    return a->cluster==b->cluster&&a->size==b->size&&a->epoch==b->epoch&&
        a->directory_sector==b->directory_sector&&a->directory_offset==b->directory_offset&&a->attributes==b->attributes;
}
static bool prove_file(struct ln_resident_media *r,uint32_t index,bool directory){
    __typeof__(r->entry[0]) *entry=&r->entry[index];const struct ln_fat32_file *file=&entry->file;
    if(file->epoch!=r->epoch||!file->size||(file->attributes&0x18u)||
       file->directory_offset>480u||(file->directory_offset&31u))return false;
    if(directory){
        const uint8_t *entry=sector(r,file->directory_sector);if(!entry)return false;
        entry+=file->directory_offset;
        if(!entry[0]||entry[0]==0xe5u||entry[11]!=file->attributes||
           ln_read_le32(entry+28)!=file->size||
           (((uint32_t)ln_read_le16(entry+20)<<16)|ln_read_le16(entry+26))!=file->cluster)return false;
    }
    uint32_t count=((file->size-1u)>>(9u+r->shift))+1u;
    /* At most 63 residual edges between anchors. Larger files retain the
     * ordinary cooperative reader instead of adding a long synchronous walk. */
    if(count>4096u||count>r->clusters)return false;
    entry->shift=0;while(((count-1u)>>entry->shift)>=64u)++entry->shift;
    for(uint32_t i=0;i<64u;++i)entry->anchor[i]=0;
    uint32_t cluster=file->cluster,verified=UINT32_MAX;
    for(uint32_t i=0;i<count;++i){
        if(!bounded(r)||!cluster_valid(r,cluster))return false;
        if(!(i&((1u<<entry->shift)-1u)))entry->anchor[i>>entry->shift]=cluster;
        uint64_t lba=r->data_start+(uint64_t)(cluster-2u)*(1u<<r->shift);
        /* Cover only published file sectors, including its final partial sector. */
        uint32_t n=1u<<r->shift;
        if(i==count-1u)n=(((file->size-1u)>>9)&((1u<<r->shift)-1u))+1u;
        if(lba<r->partition||lba-r->partition>=r->sectors||n>r->sectors-(lba-r->partition))return false;
        if(!ln_block_cache_pin(r->cookie,lba,n)){
            for(uint32_t j=0;j<n;++j)if(!sector(r,lba+j)){r->missing_lba=lba+j;break;}
            return false;
        }
        uint32_t next;if(!next_cluster(r,cluster,&next,&verified)){
            uint64_t fat=r->fat_start+(uint64_t)r->active_fat*r->fat_sectors+cluster/128u;
            if(!sector(r,fat))r->missing_lba=fat;
            else if(r->mirrored&&r->fats==2u&&!sector(r,fat+r->fat_sectors))r->missing_lba=fat+r->fat_sectors;
            return false;
        }
        if(i==count-1u)return next>=EOC;
        if(next>=EOC)return false;cluster=next;
    }
    return false;
}
static bool admit(struct ln_resident_media *r,const struct ln_fat32 *volume,
    const char *const *paths,const struct ln_fat32_file *files,uint32_t count,struct ln_fat32 *scratch,bool append){
    if(!append){ln_resident_end(r);r->count=0;}
    r->failure=1u;r->started=ln_time_us();r->elapsed=0;r->missing_lba=0;r->result=0;uint32_t base=r->count;
    if(!volume||(!files&&(!scratch||scratch==volume||!paths))||!count||base>LN_RESIDENT_FILES||count>LN_RESIDENT_FILES-base||
       volume->state||volume->mounted!=1u||!volume->epoch||volume->cluster_shift>7u||
       volume->sectors_per_cluster!=(1u<<volume->cluster_shift))goto failed;
    if(append){if(!ln_resident_valid(r,volume->epoch)||!ln_block_cache_extend(r->cookie))goto failed;}
    else {r->cookie=ln_block_cache_begin();if(!r->cookie)goto failed;}
    r->epoch=volume->epoch;r->partition=volume->partition;r->sectors=volume->partition_sectors;
    r->fat_start=volume->fat_start;r->data_start=volume->data_start;r->clusters=volume->clusters;
    r->fat_sectors=volume->fat_sectors;r->shift=volume->cluster_shift;r->fats=volume->fats;
    r->active_fat=volume->active_fat;r->mirrored=volume->mirrored;
    if(!files){
        copy(scratch,volume,sizeof(*scratch));
        scratch->config.io=cached_io;scratch->config.sectors_io=0;scratch->config.context=r;scratch->config.writable=false;
        scratch->buffer=0;scratch->listing=0;
        /* Every directory edge must visit the pinned sector adapter. Never inherit
         * an edge/proof shortcut from an unrelated prior filesystem operation. */
        for(uint32_t i=0;i<LN_FAT32_CACHE_SIZE;++i){scratch->cache[i].cluster=0;scratch->verified[i].cluster=0;scratch->seeks[i].first=0;}
        for(uint32_t i=0;i<LN_FAT32_SEEK_MAPS;++i)scratch->seek_maps[i].first=0;
        scratch->seek_slot=LN_FAT32_SEEK_MAPS;
    }
    for(uint32_t i=0;i<count;++i){
        r->failure=16u+base+i;
        if(files)r->entry[base+i].file=files[i];
        else{
            if(!paths[i]||!ln_fat32_open_readonly(scratch,0,paths[i],1u))goto failed;
            int result=0;uint32_t polls=0;
            while(!result&&++polls<=16384u&&bounded(r))result=ln_fat32_poll(scratch);
            if(result!=1){r->result=result;goto failed;}
            r->entry[base+i].file=scratch->file;
        }
        r->failure=32u+base+i;if(!prove_file(r,base+i,!files))goto failed;
    }
    if(!bounded(r)||!ln_resident_valid(r,volume->epoch)||volume->state||volume->mounted!=1u)goto failed;
    r->count=base+count;r->failure=0;r->elapsed=ln_time_us()-r->started;return true;
failed:
    r->elapsed=ln_time_us()-r->started;ln_resident_end(r);return false;
}
bool ln_resident_admit(struct ln_resident_media *r,const struct ln_fat32 *v,
    const char *const *p,uint32_t n,struct ln_fat32 *scratch){return admit(r,v,p,0,n,scratch,false);}
bool ln_resident_append(struct ln_resident_media *r,const struct ln_fat32 *v,
    const char *const *p,uint32_t n,struct ln_fat32 *scratch){return admit(r,v,p,0,n,scratch,true);}
bool ln_resident_admit_files(struct ln_resident_media *r,const struct ln_fat32 *v,
    const struct ln_fat32_file *files,uint32_t n,bool append){
    if(!files){ln_resident_end(r);return false;}return admit(r,v,0,files,n,0,append);
}
static bool locate(const struct ln_resident_media *r,uint32_t index,uint32_t logical,uint32_t *cluster){
    const __typeof__(r->entry[0]) *entry=&r->entry[index];uint32_t anchor=logical>>entry->shift;
    if(anchor>=64u||!entry->anchor[anchor])return false;
    *cluster=entry->anchor[anchor];uint32_t at=anchor<<entry->shift;
    while(at++<logical){uint32_t next;if(!next_cluster(r,*cluster,&next,0)||next>=EOC)return false;*cluster=next;}
    return true;
}
int ln_resident_read(const struct ln_resident_media *r,const struct ln_fat32_file *file,
    uint32_t offset,void *output,uint32_t bytes){
    if(!file||!output||!bytes||bytes>16384u||!ln_resident_valid(r,file->epoch)||
       offset>file->size||bytes>file->size-offset)return -1;
    uint32_t index=0;while(index<r->count&&!file_equal(file,&r->entry[index].file))++index;
    if(index==r->count)return -1;
    /* Preflight all sectors before copying any output. No callbacks or executive
     * reentry occur between these passes; the lease protects every source page. */
    for(uint32_t pass=0;pass<2u;++pass){
        uint32_t position=offset,left=bytes,cluster;
        if(!locate(r,index,position>>(9u+r->shift),&cluster))return -1;
        while(left){
            uint32_t within=(position>>9)&((1u<<r->shift)-1u);
            uint64_t lba=r->data_start+(uint64_t)(cluster-2u)*(1u<<r->shift)+within;
            const uint8_t *source=sector(r,lba);if(!source)return -1;
            uint32_t skip=position&511u,n=512u-skip;if(n>left)n=left;
            if(pass)copy((uint8_t *)output+(bytes-left),source+skip,n);
            left-=n;position+=n;
            if(left&&!((position>>9)&((1u<<r->shift)-1u))){
                uint32_t next;if(!next_cluster(r,cluster,&next,0)||next>=EOC)return -1;cluster=next;
            }
        }
    }
    return 1;
}
