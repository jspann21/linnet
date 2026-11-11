#include "boot/include/linnet_bytes.h"
#include "services/storage/fat32.h"
#include <stddef.h>

enum { MOUNT=1, HEADER, HEADER_MIRROR, OPEN_NAME, DIRECTORY, DIRECTORY_NEXT,
       POSITION, TRANSFER, WRITE_SECTOR, FLUSH, VERIFY_CHAIN, SET_BEGIN, SET_CHAIN,
       MUT_BEGIN, MUT_DIR, MUT_DIRTY, MUT_DIRTY_FLUSH, MUT_FSINFO, MUT_FSINFO_WRITE,
       MUT_SCAN, MUT_RESERVE, MUT_LINK, MUT_DATA, MUT_DATA_WRITE, MUT_OLD_NEXT,
       MUT_READY, MUT_PUBLISH_READ, MUT_PUBLISH_WRITE, MUT_PUBLISH_FLUSH,
       MUT_FREE_READ, MUT_FREE_WRITE, MUT_RECLAIM_FLUSH, MUT_CLEAN, MUT_CLEAN_FLUSH, LIST_SECTOR, LIST_NEXT, IDLE=0 };
enum { OP_MOUNT=1, OP_OPEN, OP_READ, OP_WRITE, OP_SYNC, OP_SET, OP_CREATE, OP_GROW, OP_LIST };
_Static_assert(OP_READ-LN_FAT32_READ==OP_WRITE-LN_FAT32_WRITE,"FAT transfer operation mapping");
#define EOC 0x0ffffff8u
#define MAX_CHAIN 65536u
#define READ_PROOF_EDGES 128u
static bool cluster_valid(const struct ln_fat32*f,uint32_t c){return c>=2&&c<f->clusters+2u;}
static uint64_t cluster_sector(const struct ln_fat32*f,uint32_t c){
    return f->data_start+(uint64_t)(c-2u)*f->sectors_per_cluster;
}
static void seek_maps_clear(struct ln_fat32*f){
    for(uint32_t i=0;i<LN_FAT32_SEEK_MAPS;i++)f->seek_maps[i].first=0;
    f->seek_slot=LN_FAT32_SEEK_MAPS;f->seek_clock=0;
}
static void seek_map_select(struct ln_fat32*f,const struct ln_fat32_file*file,bool reset){
    f->seek_slot=LN_FAT32_SEEK_MAPS;
    /* Short files need at most 63 edges to reach any byte. Do not let state
     * records and small indexes evict complete maps for large media files. */
    if(f->mounted!=1||!file->size||((file->size-1u)>>(9u+f->cluster_shift))<64u||
       !cluster_valid(f,file->cluster))return;
    uint32_t slot=0;
    for(uint32_t i=0;i<LN_FAT32_SEEK_MAPS;i++){
        if(f->seek_maps[i].first==file->cluster&&f->seek_maps[i].size==file->size){slot=i;goto selected;}
        if(!f->seek_maps[i].first||
           (f->seek_maps[slot].first&&(uint32_t)(f->seek_clock-f->seek_maps[i].age)>
            (uint32_t)(f->seek_clock-f->seek_maps[slot].age)))slot=i;
    }
    reset=true;
selected:;
    struct ln_fat32_seek_map*m=&f->seek_maps[slot];
    if(reset){
        m->first=file->cluster;m->size=file->size;m->shift=0;
        uint32_t clusters=((file->size-1u)>>(9u+f->cluster_shift))+1u;
        while(((clusters-1u)>>m->shift)>=LN_FAT32_SEEK_ANCHORS)++m->shift;
        for(uint32_t i=0;i<LN_FAT32_SEEK_ANCHORS;i++)m->anchors[i].cluster=m->anchors[i].run=0;
        m->anchors[0].cluster=file->cluster;m->anchors[0].run=1;
    }
    m->age=++f->seek_clock;f->seek_slot=slot;
}
static void seek_map_record(struct ln_fat32*f,uint32_t logical,uint32_t cluster){
    if(f->seek_slot>=LN_FAT32_SEEK_MAPS)return;
    struct ln_fat32_seek_map*m=&f->seek_maps[f->seek_slot];uint32_t at=logical>>m->shift;
    if(at>=LN_FAT32_SEEK_ANCHORS)return;
    uint32_t relative=logical-(at<<m->shift);
    if(!relative){
        if(m->anchors[at].cluster!=cluster){m->anchors[at].cluster=cluster;m->anchors[at].run=1;}
    }
    else if(m->anchors[at].cluster&&m->anchors[at].run==relative&&
            m->anchors[at].cluster+relative==cluster)++m->anchors[at].run;
}
static void seek_map_position(struct ln_fat32*f,uint32_t target){
    if(f->seek_slot>=LN_FAT32_SEEK_MAPS)return;
    const struct ln_fat32_seek_map*m=&f->seek_maps[f->seek_slot];uint32_t at=target>>m->shift;
    if(at>=LN_FAT32_SEEK_ANCHORS)at=LN_FAT32_SEEK_ANCHORS-1u;
    while(at&&!m->anchors[at].cluster)--at;
    if(!m->anchors[at].cluster||!m->anchors[at].run)return;
    uint32_t first=at<<m->shift,within=target-first;
    if(within>=m->anchors[at].run)within=m->anchors[at].run-1u;
    f->cluster=m->anchors[at].cluster+within;f->logical_cluster=first+within;
}
/* Sixteen fully associative, most-recently-used-first chain proofs. Keep the
 * existing cluster+size key within this exclusive mount; transfer validates
 * the file epoch first, and mount/mutations invalidate all proofs. */
static bool verified_touch(struct ln_fat32*f,const struct ln_fat32_file*file){
    for(uint32_t i=0;i<LN_FAT32_CACHE_SIZE;i++){
        if(f->verified[i].cluster!=file->cluster||f->verified[i].size!=file->size)continue;
        struct ln_fat32_verified found=f->verified[i];
        for(uint32_t j=i;j;j--)f->verified[j]=f->verified[j-1u];
        f->verified[0]=found;return true;
    }
    return false;
}
static void verified_remember(struct ln_fat32*f,const struct ln_fat32_file*file){
    if(verified_touch(f,file))return;
    for(uint32_t i=LN_FAT32_CACHE_SIZE-1u;i;i--)f->verified[i]=f->verified[i-1u];
    f->verified[0]=(struct ln_fat32_verified){file->cluster,file->size};
}
static int finish(struct ln_fat32*f,int result){
    f->state=IDLE;f->result=result;f->verified_fat_sector=0;
    if(result<0&&f->operation==OP_SET)f->proof_base_valid=false;
    if(result<0&&f->mutation_started){f->mounted=2;f->write_proven=false;}
    if(result==LN_FAT32_IO||result==LN_FAT32_CORRUPT){
        if(f->mounted)f->mounted=2;
        f->write_proven=false;
        f->proof_base_valid=false;
        seek_maps_clear(f);
    }
    return result;
}
static int io(struct ln_fat32*f,enum ln_fat32_io op,uint64_t lba,uint8_t*sector){
    if(op!=LN_FAT32_FLUSH&&(lba<f->partition||lba-f->partition>=f->partition_sectors))
        return LN_FAT32_BOUNDS;
    /* Retire primary-buffer provenance before the adapter can overwrite it. */
    if(op==LN_FAT32_READ&&sector==f->sector)f->verified_fat_sector=0;
    int result=f->config.io(f->config.context,op,lba,sector,f->generation);
    return result<0||result>1?LN_FAT32_IO:result;
}
void ln_fat32_init(struct ln_fat32*f,const struct ln_fat32_config*c){
    if(!f)return;
    uint8_t*p=(uint8_t*)f;for(size_t i=0;i<sizeof(*f);++i)p[i]=0;
    if(c)f->config=*c;
    f->result=LN_FAT32_DONE;
}
void ln_fat32_abandon(struct ln_fat32*f){
    if(!f)return;
    f->mounted=0;f->state=0;f->result=LN_FAT32_IO;f->write_proven=false;
    f->proof_base_valid=false;f->old_next=2;f->fat_phase=0;f->verified_fat_sector=0;
    seek_maps_clear(f);
    if(++f->epoch==0)++f->epoch;
}
static bool admit(struct ln_fat32*f,uint8_t op,uint16_t state,uint32_t generation){
    if(!f||f->state||!f->config.io||!generation)return false;
    f->operation=op;f->state=state;f->generation=generation;f->result=0;
    f->transferred=0;f->fat_phase=0;f->chain_steps=0;f->mutation_started=false;f->readonly_open=false;
    f->verified_fat_sector=0;
    return true;
}
bool ln_fat32_mount(struct ln_fat32*f,uint64_t lba,uint64_t sectors,uint32_t generation){
    if(!f||!sectors||lba>=f->config.disk_sectors||sectors>f->config.disk_sectors-lba)
        return false;
    if(!admit(f,OP_MOUNT,MOUNT,generation))return false;
    f->partition=lba;f->partition_sectors=sectors;f->mounted=0;
    f->write_proven=false;f->directory_count=0;f->set_count=0;
    f->proof_base_valid=false;f->proven_directory_count=0;
    /* old_next is idle outside old-chain reclamation; retain the mount-local
     * allocation hint there without growing the resident FAT context. */
    f->old_next=2;
    seek_maps_clear(f);
    for(unsigned i=0;i<LN_FAT32_CACHE_SIZE;++i){
        f->cache[i].cluster=0;f->verified[i].cluster=0;f->seeks[i].first=0;
    }
    return true;
}
bool ln_fat32_open(struct ln_fat32*f,const char*path,uint32_t generation){
    if(!f||!f->mounted||!path||path[0]!='/'||f->state)return false;
    uint32_t length=0;while(length<=LN_FAT32_PATH_MAX&&path[length])++length;
    if(length<2||length>LN_FAT32_PATH_MAX)return false;
    if(!admit(f,OP_OPEN,OPEN_NAME,generation))return false;
    f->open_directory_count=f->directory_count;
    f->open_write_proven=f->write_proven;
    for(uint32_t i=0;i<=length;++i)f->path[i]=path[i];
    f->path_cursor=1;f->component=0;f->cluster=f->root;
    return true;
}
bool ln_fat32_list(struct ln_fat32*f,struct ln_fat32_directory*d,void*buffer,uint32_t generation){
    if(!f||!f->mounted||!d||!buffer||(d->epoch&&d->epoch!=f->epoch)||
       (d->cluster&&!cluster_valid(f,d->cluster))||d->sector>f->sectors_per_cluster)return false;
    if(!admit(f,OP_LIST,d->sector==f->sectors_per_cluster?LIST_NEXT:LIST_SECTOR,generation))return false;
    if(!d->cluster)d->cluster=f->root;
    if(!d->power){d->anchor=d->cluster;d->power=1;}
    d->epoch=f->epoch;f->listing=d;f->buffer=buffer;f->cluster=d->cluster;f->chain_steps=d->steps;
    return true;
}
bool ln_fat32_open_readonly(struct ln_fat32*f,const struct ln_fat32_file*parent,const char*path,uint32_t generation){
    if(parent){
        if(!f||!parent->epoch||parent->epoch!=f->epoch||!(parent->attributes&16u)||
           (parent->attributes&8u)||!cluster_valid(f,parent->cluster)||!path||path[0]!='/')return false;
        for(uint32_t i=1;path[i];++i)if(i>12u||path[i]=='/')return false;
    }
    if(!ln_fat32_open(f,path,generation))return false;
    f->readonly_open=true;if(parent)f->cluster=parent->cluster;return true;
}
/* The parent must be a directory already reached by open() in this mount.
 * Only one leaf component is accepted, so no unregistered parent is skipped. */
bool ln_fat32_open_at(struct ln_fat32*f,const struct ln_fat32_file*parent,
                      const char*leaf,uint32_t generation){
    if(!f||!parent||!leaf||leaf[0]!='/'||!leaf[1]||parent->epoch!=f->epoch||
       !(parent->attributes&16u)||(parent->attributes&8u)||!cluster_valid(f,parent->cluster))return false;
    uint32_t n=1;for(;n<=LN_FAT32_PATH_MAX&&leaf[n];++n)if(leaf[n]=='/')return false;
    if(n>LN_FAT32_PATH_MAX)return false;
    bool registered=false;for(uint32_t i=0;i<f->directory_count;++i)
        if(f->directories[i]==parent->cluster)registered=true;
    if(!registered||!ln_fat32_open(f,leaf,generation))return false;
    f->cluster=parent->cluster;return true;
}
static bool transfer(struct ln_fat32*f,const struct ln_fat32_file*file,
                     uint32_t offset,void*buffer,uint32_t length,uint32_t generation,bool write){
    if(!f||!f->mounted||!file||(length&&!buffer)||file->epoch!=f->epoch||
       (file->attributes&0x18u)||offset>file->size||length>file->size-offset||
       (write&&(!f->config.writable||f->mounted!=1||(file->attributes&1u))))return false;
    if(write){
        bool proven=false;
        if(f->write_proven)for(uint32_t i=0;i<f->set_count;++i){
            const struct ln_fat32_file*v=&f->validated[i];
            if(v->cluster==file->cluster&&v->size==file->size&&
               v->directory_sector==file->directory_sector&&v->directory_offset==file->directory_offset&&
               v->attributes==file->attributes)
                proven=true;
        }
        if(!proven)return false;
    }
    if(length&&!cluster_valid(f,file->cluster))return false;
    if(!admit(f,write?OP_WRITE:OP_READ,VERIFY_CHAIN,generation))return false;
    f->file=*file;f->offset=offset;f->length=length;f->buffer=buffer;
    f->cluster=file->cluster;f->skip=offset>>(9u+f->cluster_shift);
    f->within=offset&((512u*f->sectors_per_cluster)-1u);
    f->verified_clusters=1;f->cycle_anchor=file->cluster;f->cycle_distance=0;f->cycle_power=1;
    f->logical_cluster=0;
    if(!length||verified_touch(f,file))f->state=POSITION;
    seek_map_select(f,file,f->state!=POSITION);
    if(f->state==POSITION){
        seek_map_position(f,offset>>(9u+f->cluster_shift));
        for(unsigned i=0;i<LN_FAT32_CACHE_SIZE;++i){
            const struct ln_fat32_seek*s=&f->seeks[i];
            if(s->first==file->cluster&&s->index<=(offset>>(9u+f->cluster_shift))&&
               s->index>f->logical_cluster){f->cluster=s->cluster;f->logical_cluster=s->index;}
        }
        f->skip-=f->logical_cluster;
    }
    return true;
}
bool ln_fat32_read(struct ln_fat32*f,const struct ln_fat32_file*file,uint32_t offset,
                  void*buffer,uint32_t length,uint32_t generation){
    return transfer(f,file,offset,buffer,length,generation,false);
}
bool ln_fat32_write(struct ln_fat32*f,const struct ln_fat32_file*file,uint32_t offset,
                   const void*buffer,uint32_t length,uint32_t generation){
    return transfer(f,file,offset,(void*)buffer,length,generation,true);
}
bool ln_fat32_sync(struct ln_fat32*f,uint32_t generation){
    return f&&f->mounted&&admit(f,OP_SYNC,FLUSH,generation);
}
static bool mutable(const struct ln_fat32*f,uint32_t size){
    return f&&f->mounted==1&&f->config.writable&&f->write_proven&&size&&
        (((size-1u)>>(9u+f->cluster_shift))+1u)<=MAX_CHAIN;
}
bool ln_fat32_create(struct ln_fat32*f,const char*path,uint32_t size,uint32_t generation){
    if(!mutable(f,size)||!ln_fat32_open(f,path,generation))return false;
    f->operation=OP_CREATE;f->new_size=size;f->slot_found=false;return true;
}
bool ln_fat32_grow(struct ln_fat32*f,const struct ln_fat32_file*file,uint32_t size,uint32_t generation){
    if(!mutable(f,size)||!file||size<=file->size||
       !transfer(f,file,0,NULL,0,generation,true))return false;
    f->operation=OP_GROW;f->new_size=size;f->slot_sector=file->directory_sector;
    f->slot_offset=file->directory_offset;f->slot_found=true;f->slot_end=false;
    f->state=MUT_BEGIN;return true;
}
bool ln_fat32_validate_set(struct ln_fat32*f,const struct ln_fat32_file*files,
                         uint32_t count,uint32_t generation){
    if(!f||!f->mounted||(count&&!files)||count>LN_FAT32_SET_MAX||f->state)return false;
    for(uint32_t i=0;i<count;++i)
        if(files[i].epoch!=f->epoch||(files[i].attributes&0x18u)||
           (files[i].size&&!cluster_valid(f,files[i].cluster)))return false;
    if(!admit(f,OP_SET,SET_BEGIN,generation))return false;
    f->write_proven=false;f->set_count=count;f->set_index=0;
    f->proof_base_valid=false;f->proven_directory_count=0;
    for(uint32_t i=0;i<count;++i)f->validated[i]=files[i];
    return true;
}
bool ln_fat32_proof_pending(const struct ln_fat32*f){
    return f&&f->mounted==1&&f->proof_base_valid&&!f->write_proven&&
        f->proven_directory_count<f->directory_count;
}
bool ln_fat32_refresh_proof(struct ln_fat32*f,uint32_t generation){
    if(!ln_fat32_proof_pending(f)||!admit(f,OP_SET,SET_BEGIN,generation))return false;
    f->set_index=f->set_count+f->proven_directory_count;
    return true;
}
static bool directory_register(struct ln_fat32*f,uint32_t cluster){
    for(uint32_t i=0;i<f->directory_count;++i)if(f->directories[i]==cluster)return true;
    if(f->directory_count==LN_FAT32_DIR_MAX)return false;
    f->directories[f->directory_count++]=cluster;f->write_proven=false;return true;
}
/* Single-sector read/modify/write, preserving FAT's reserved high nibble.
 * Both mirrors are compared BEFORE modification and receive identical bytes.
 * A partial mirror update is deliberately not repaired or retried here. */
static int edit_fat(struct ln_fat32*f,uint32_t cluster,bool write,uint32_t value){
    uint64_t lba=f->fat_start+(uint64_t)f->active_fat*f->fat_sectors+(cluster>>7);
    uint32_t offset=(cluster&127u)*4u;int r;
    switch(f->edit_phase){
    case 0:r=io(f,LN_FAT32_READ,lba,f->sector);if(r!=1)return r;
        f->edit_phase=f->mirrored&&f->fats==2?1:2;return 0;
    case 1:r=io(f,LN_FAT32_READ,lba+f->fat_sectors,f->mirror);if(r!=1)return r;
        if(!ln_bytes_equal(f->sector,f->mirror,512))return LN_FAT32_CORRUPT;
        f->edit_phase=2;return 0;
    case 2:f->edit_value=ln_read_le32(f->sector+offset)&0x0fffffffu;
        if(!write){f->edit_phase=0;return 1;}
        ln_write_le32(f->sector+offset,(ln_read_le32(f->sector+offset)&0xf0000000u)|(value&0x0fffffffu));
        f->edit_phase=3;return 0;
    case 3:r=io(f,LN_FAT32_WRITE,lba,f->sector);if(r!=1)return r;
        f->edit_phase=f->mirrored&&f->fats==2?4:5;return 0;
    case 4:r=io(f,LN_FAT32_WRITE,lba+f->fat_sectors,f->sector);if(r!=1)return r;
        f->edit_phase=5;return 0;
    default:f->edit_phase=0;return 1;
    }
}
/* Scan each FAT sector once, comparing its mirror before using any entry.
 * The caller preserves candidate across polls and bounds a wrapped scan at
 * its start, so a hint can never turn a full-volume search into a
 * false no-space result. Reserved, bad and nonzero entries remain occupied. */
static int scan_free_fat(struct ln_fat32*f){
    uint64_t lba=f->fat_start+(uint64_t)f->active_fat*f->fat_sectors+(f->candidate>>7);
    int result;
    if(!f->fat_phase){
        result=io(f,LN_FAT32_READ,lba,f->sector);if(result!=1)return result;
        f->fat_phase=f->mirrored&&f->fats==2?1:2;return 0;
    }
    if(f->fat_phase==1){
        result=io(f,LN_FAT32_READ,lba+f->fat_sectors,f->mirror);if(result!=1)return result;
        if(!ln_bytes_equal(f->sector,f->mirror,512))return LN_FAT32_CORRUPT;
        f->fat_phase=2;return 0;
    }
    uint32_t end=(f->candidate|127u)+1u,limit=f->clusters+2u;
    if(end>limit)end=limit;
    if(f->slot_found&&end>f->directory_entry_index)end=f->directory_entry_index;
    while(f->candidate<end){
        if(!(ln_read_le32(f->sector+(f->candidate&127u)*4u)&0x0fffffffu)){
            f->fat_phase=0;return 1;
        }
        ++f->candidate;
    }
    f->fat_phase=0;return 0;
}
/* directory_entry_index and slot_found have completed their directory/FSInfo
 * duties before this point; reuse them as the scan origin and wrap marker. */
static void begin_allocation_scan(struct ln_fat32*f){
    f->candidate=cluster_valid(f,f->old_next)?f->old_next:2;
    f->directory_entry_index=f->candidate;f->slot_found=false;f->fat_phase=0;f->state=MUT_SCAN;
}
/* FAT lookups compare mirrored sectors before caching a chain edge. A volume
 * with mirroring disabled uses only the BPB-selected active FAT. No repairs
 * or allocation updates are attempted on inconsistent metadata. */
static int next_cluster(struct ln_fat32*f){
    if(!f->fat_phase){
        uint32_t limit=MAX_CHAIN;
#if defined(LINNET_TARGET_PHOTO4G_P98) && defined(LINNET_IMAGE_OS)
        /* IPVF episodes can exceed 65,536 clusters on the Photo's FAT32
         * volume. Keep mutation/directory limits; read proofs remain bounded
         * by the volume and retain mirror and cycle validation. */
        if(f->operation==OP_READ)limit=f->clusters;
#endif
        if(!cluster_valid(f,f->cluster)||++f->chain_steps>f->clusters||f->chain_steps>limit)
            return LN_FAT32_CORRUPT;
        struct ln_fat32_cache*c=&f->cache[f->cluster%LN_FAT32_CACHE_SIZE];
        if(c->cluster==f->cluster){f->fat_value=c->next;return 1;}
        f->fat_cluster=f->cluster;
        /* Chain and set proofs may reuse this request's mirror-checked sector. */
        if((f->state==VERIFY_CHAIN||f->state==SET_CHAIN)&&
           f->verified_fat_sector==f->cluster/128u+1u){
            f->fat_value=ln_read_le32(f->sector+(f->cluster%128u)*4u)&0x0fffffffu;
            f->fat_phase=3;
        }else f->fat_phase=1;
    }
    uint64_t sector=f->fat_start+(uint64_t)f->active_fat*f->fat_sectors+f->fat_cluster/128u;
    int result;
    if(f->fat_phase==1){
        result=io(f,LN_FAT32_READ,sector,f->sector);if(result!=1)return result;
        f->fat_value=ln_read_le32(f->sector+(f->fat_cluster%128u)*4u)&0x0fffffffu;
        f->fat_phase=f->mirrored&&f->fats==2?2:3;
        return 0;
    }
    if(f->fat_phase==2){
        result=io(f,LN_FAT32_READ,sector+f->fat_sectors,f->mirror);if(result!=1)return result;
        if(!ln_bytes_equal(f->sector,f->mirror,512))return LN_FAT32_CORRUPT;
        f->fat_phase=3;return 0;
    }
    if(f->fat_value<EOC&&!cluster_valid(f,f->fat_value))return LN_FAT32_CORRUPT;
    if(f->fat_value==f->fat_cluster)return LN_FAT32_CORRUPT;
    /* Phase3 follows the required full mirror comparison, or a proven hit. */
    if(f->state==VERIFY_CHAIN||f->state==SET_CHAIN)
        f->verified_fat_sector=f->fat_cluster/128u+1u;
    /* The whole sector (and mirror when enabled) is already validated. Fill
     * the existing cache with its aligned group of16 edges, so consecutive
     * clusters avoid repeated sector copies, mirror comparisons and polls.
     * Do not cache invalid neighbors: they must still fail when traversed.
     * Mount and every FAT mutation retain their existing cache invalidation. */
    _Static_assert(LN_FAT32_CACHE_SIZE==16u,"FAT edge group divides128 entries");
    uint32_t first=f->fat_cluster&~(LN_FAT32_CACHE_SIZE-1u);
    for(uint32_t i=0;i<LN_FAT32_CACHE_SIZE;++i){
        uint32_t cluster=first+i,next=ln_read_le32(f->sector+(cluster%128u)*4u)&0x0fffffffu;
        struct ln_fat32_cache*c=&f->cache[i];
        c->cluster=0;
        if(cluster_valid(f,cluster)&&next!=cluster&&(next>=EOC||cluster_valid(f,next))){
            c->next=next;c->cluster=cluster;
        }
    }
    f->fat_phase=0;
    return 1;
}
static int bpb(struct ln_fat32*f){
    const uint8_t*p=f->sector;
    uint32_t spc=p[13],reserved=ln_read_le16(p+14),fats=p[16],total=ln_read_le32(p+32),fat=ln_read_le32(p+36);
    uint32_t sector_bytes=ln_read_le16(p+11),fsinfo=ln_read_le16(p+48),backup=ln_read_le16(p+50);
    uint16_t flags=ln_read_le16(p+40);
    if(p[510]!=0x55||p[511]!=0xaa||sector_bytes<512||sector_bytes>4096||(sector_bytes&(sector_bytes-1u))||!spc||spc>128||(spc&(spc-1u))||
       !reserved||(fats!=1&&fats!=2)||ln_read_le16(p+17)||ln_read_le16(p+19)||ln_read_le16(p+22)||
       !total||total>f->partition_sectors||!fat||ln_read_le16(p+42)||
       (flags&0xff70u)||((flags&0x80u)&&(flags&15u)>=fats)||
       (fsinfo!=0xffffu&&fsinfo>=reserved)||(backup!=0xffffu&&backup>=reserved))
        return LN_FAT32_FORMAT;
    /* The block API always addresses 512-byte units. Normalize BPB geometry
     * once so FAT pages, directory entries and file offsets share that unit. */
    uint32_t scale=sector_bytes>>9;
    if((uint64_t)total*scale>UINT32_MAX||(uint64_t)total*scale>f->partition_sectors||
       (uint64_t)fat*scale>UINT32_MAX)return LN_FAT32_FORMAT;
    total*=scale;fat*=scale;reserved*=scale;spc*=scale;
    if(fsinfo!=0xffffu)fsinfo*=scale;
    if(backup!=0xffffu)backup*=scale;
    uint64_t metadata=reserved+(uint64_t)fats*fat;
    if(metadata>=total)return LN_FAT32_FORMAT;
    uint8_t shift=0;while((1u<<shift)!=spc)++shift;
    uint32_t clusters=(total-(uint32_t)metadata)>>shift;
    if(clusters<65525u||clusters>0x0fffffedu||(uint64_t)fat*128u<clusters+2u)
        return LN_FAT32_FORMAT;
    f->root=ln_read_le32(p+44);f->clusters=clusters;
    if(!cluster_valid(f,f->root))return LN_FAT32_FORMAT;
    f->sectors_per_cluster=(uint16_t)spc;f->cluster_shift=shift;
    f->reserved=reserved;f->fats=(uint8_t)fats;
    f->mirrored=(flags&0x80u)?0:1;f->active_fat=f->mirrored?0:(uint8_t)(flags&15u);
    f->fat_sectors=fat;f->total=total;f->fat_start=f->partition+reserved;
    f->data_start=f->partition+metadata;
    f->fsinfo=fsinfo;f->backup=backup;
    return 1;
}
static int component(struct ln_fat32*f){
    if(++f->component>16)return LN_FAT32_BOUNDS;
    for(unsigned i=0;i<11;++i)f->name[i]=' ';
    f->system_long_name=false;f->system_long_match=false;
    static const char system_name[]=".LINNET";
    unsigned match=0;
    while(match<sizeof system_name-1u){
        uint8_t c=(uint8_t)f->path[f->path_cursor+match];
        if(c>='a'&&c<='z')c-=32;
        if(c!=(uint8_t)system_name[match])break;
        ++match;
    }
    if(match==sizeof system_name-1u&&f->cluster==f->root&&f->component==1u&&
       (!f->path[f->path_cursor+match]||f->path[f->path_cursor+match]=='/')){
        f->system_long_name=true;f->path_cursor+=match;
        if(f->operation==OP_CREATE&&!f->path[f->path_cursor])return LN_FAT32_UNSUPPORTED;
    }
    unsigned base=0,extension=0;bool dot=false;
    while(f->path[f->path_cursor]&&f->path[f->path_cursor]!='/'){
        uint8_t c=(uint8_t)f->path[f->path_cursor++];
        if(c=='.'){if(dot||!base)return LN_FAT32_UNSUPPORTED;dot=true;continue;}
        if(c>='a'&&c<='z')c-=32;
        if(c<=32||c>=127||c=='"'||c=='*'||c=='+'||c==','||c==':'||c==';'||
           c=='<'||c=='='||c=='>'||c=='?'||c=='['||c=='\\'||c==']'||c=='|')
            return LN_FAT32_UNSUPPORTED;
        if(!dot){if(base==8)return LN_FAT32_UNSUPPORTED;f->name[base++]=c;}
        else {if(extension==3)return LN_FAT32_UNSUPPORTED;f->name[8+extension++]=c;}
    }
    if(!f->system_long_name&&(!base||(dot&&!extension)))return LN_FAT32_UNSUPPORTED;
    if(f->path[f->path_cursor]=='/'&&!f->path[f->path_cursor+1])return LN_FAT32_ARGUMENT;
    f->directory_sector_index=0;f->directory_entry_index=0;f->chain_steps=0;
    if(f->operation==OP_CREATE)f->slot_found=false;
    return 1;
}
int ln_fat32_poll(struct ln_fat32*f){
    if(!f)return LN_FAT32_ARGUMENT;
    if(!f->state)return f->result;
    int result;uint32_t bytes,index,proof_edges=0;uint8_t*p;
    switch(f->state){
    case LIST_NEXT:{
        struct ln_fat32_directory*d=f->listing;
        result=next_cluster(f);if(result!=1)break;
        d->steps=f->chain_steps;
        if(f->fat_value>=EOC){d->end=true;return finish(f,1);}
        if(f->fat_value==d->anchor){result=LN_FAT32_CORRUPT;break;}
        if(++d->distance==d->power){d->anchor=f->fat_value;d->power*=2u;d->distance=0;}
        d->cluster=f->cluster=f->fat_value;d->sector=0;f->state=LIST_SECTOR;return 0;
    }
    case LIST_SECTOR:{
        struct ln_fat32_directory*d=f->listing;if(d->end)return finish(f,1);
        uint64_t lba=cluster_sector(f,d->cluster)+d->sector;
        result=io(f,LN_FAT32_READ,lba,f->buffer);if(result!=1)break;
        d->lba=lba;++d->sector;f->transferred=512u;return finish(f,1);
    }
    case MOUNT:
        result=io(f,LN_FAT32_READ,f->partition,f->sector);if(result!=1)break;
        result=bpb(f);if(result!=1)break;
        f->state=HEADER;return 0;
    case HEADER:
        result=io(f,LN_FAT32_READ,f->fat_start+(uint64_t)f->active_fat*f->fat_sectors,f->sector);
        if(result!=1)break;
        if((ln_read_le32(f->sector)&0x0ffffff8u)!=0x0ffffff8u||
           (ln_read_le32(f->sector+4)&0x03ffffffu)!=0x03ffffffu){result=LN_FAT32_CORRUPT;break;}
        f->state=HEADER_MIRROR;return 0;
    case HEADER_MIRROR:
        if(f->mirrored&&f->fats==2){
            result=io(f,LN_FAT32_READ,f->fat_start+f->fat_sectors,f->mirror);
            if(result!=1)break;
            if(!ln_bytes_equal(f->sector,f->mirror,512)){result=LN_FAT32_CORRUPT;break;}
        }
        /* Dirty/error-marked volumes are readable but cannot be mutated. */
        f->mounted=(ln_read_le32(f->sector+4)&0x0c000000u)==0x0c000000u?1:2;
        f->volume_flags=ln_read_le32(f->sector+4)&0x0fffffffu;
        (void)directory_register(f,f->root);
        if(++f->epoch==0)++f->epoch;
        return finish(f,1);
    case OPEN_NAME:
        result=component(f);if(result!=1)break;
        f->state=DIRECTORY;return 0;
    case DIRECTORY:
        f->io_lba=cluster_sector(f,f->cluster)+f->directory_sector_index;
        result=io(f,LN_FAT32_READ,f->io_lba,f->sector);if(result!=1)break;
        for(index=0;index<16;++index){
            p=f->sector+index*32u;
            if(f->operation==OP_CREATE&&!f->path[f->path_cursor]&&!f->slot_found&&
               (p[0]==0xe5||(!p[0]&&index<15))){
                f->slot_found=true;f->slot_sector=f->io_lba;f->slot_offset=(uint16_t)(index*32u);
                f->slot_end=!p[0];
            }
            if(!p[0]){
                if(f->operation==OP_CREATE&&!f->path[f->path_cursor]){
                    if(!f->slot_found)return finish(f,LN_FAT32_NO_SPACE);
                    f->state=MUT_BEGIN;return 0;
                }
                return finish(f,LN_FAT32_NOT_FOUND);
            }
            if(f->system_long_name){
                if(p[11]==15u){
                    static const uint8_t positions[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
                    static const char system_name[]=".LINNET";
                    bool valid=p[0]==0x41u&&!p[12]&&!ln_read_le16(p+26);
                    for(uint32_t i=0;i<13u;++i){
                        uint16_t c=ln_read_le16(p+positions[i]);
                        if(c>='a'&&c<='z')c-=32;
                        uint16_t expected=i<7u?(uint8_t)system_name[i]:i==7u?0u:0xffffu;
                        if(c!=expected)valid=false;
                    }
                    f->system_long_match=valid;f->system_long_checksum=p[13];continue;
                }
                uint8_t checksum=0;
                for(uint32_t i=0;i<11u;++i)checksum=(uint8_t)(((checksum&1u)<<7)+(checksum>>1)+p[i]);
                bool valid=f->system_long_match&&checksum==f->system_long_checksum&&
                           p[0]!=0xe5&&!(p[11]&8u)&&(p[11]&16u);
                f->system_long_match=false;if(!valid)continue;
            }else if(p[0]==0xe5||(p[11]&8u)||!ln_bytes_equal(p,f->name,11))continue;
            uint32_t cluster=((uint32_t)ln_read_le16(p+20)<<16)|ln_read_le16(p+26);
            uint32_t size=ln_read_le32(p+28);
            if((size||(p[11]&16u))&&!cluster_valid(f,cluster))return finish(f,LN_FAT32_CORRUPT);
            if(f->path[f->path_cursor]=='/'){
                if(!(p[11]&16u))return finish(f,LN_FAT32_NOT_FOUND);
                if(!f->readonly_open&&!directory_register(f,cluster))return finish(f,LN_FAT32_BOUNDS);
                ++f->path_cursor;f->cluster=cluster;f->state=OPEN_NAME;return 0;
            }
            if(f->operation==OP_CREATE)return finish(f,LN_FAT32_EXISTS);
            if((p[11]&16u)&&!f->readonly_open&&!directory_register(f,cluster))return finish(f,LN_FAT32_BOUNDS);
            f->file=(struct ln_fat32_file){cluster,size,f->epoch,f->io_lba,(uint16_t)(index*32u),p[11]};
            return finish(f,1);
        }
        if(++f->directory_sector_index==f->sectors_per_cluster){f->state=DIRECTORY_NEXT;}
        return 0;
    case DIRECTORY_NEXT:
        result=next_cluster(f);if(result!=1)break;
        if(f->fat_value>=EOC){
            if(f->operation==OP_CREATE&&!f->path[f->path_cursor]){
                if(!f->slot_found)return finish(f,LN_FAT32_NO_SPACE);
                f->state=MUT_BEGIN;return 0;
            }
            return finish(f,LN_FAT32_NOT_FOUND);
        }
        f->cluster=f->fat_value;f->directory_sector_index=0;f->state=DIRECTORY;return 0;
    case VERIFY_CHAIN:
verify_chain_cached:
        /* Brent's cycle detector needs constant memory. Complete the entire
         * chain before touching file data: a cycle cannot make a write alias
         * an earlier logical offset. Traversal remains bounded by next_cluster.
         * Cache this proof only for the lifetime of this exclusive mount. */
        seek_map_record(f,f->verified_clusters-1u,f->cluster);
        result=next_cluster(f);if(result!=1)break;
        if(f->fat_value>=EOC){
            if((uint64_t)f->verified_clusters*512u*f->sectors_per_cluster<f->file.size){
                result=LN_FAT32_CORRUPT;break;
            }
            verified_remember(f,&f->file);
            f->cluster=f->file.cluster;f->logical_cluster=0;
            seek_map_position(f,f->offset>>(9u+f->cluster_shift));
            f->skip=(f->offset>>(9u+f->cluster_shift))-f->logical_cluster;
            f->chain_steps=0;f->verified_fat_sector=0;f->state=POSITION;return 0;
        }
        if(f->fat_value==f->cycle_anchor){result=LN_FAT32_CORRUPT;break;}
        if(++f->cycle_distance==f->cycle_power){
            f->cycle_anchor=f->fat_value;f->cycle_power*=2u;f->cycle_distance=0;
        }
        f->cluster=f->fat_value;++f->verified_clusters;
        /* Drain at most one sector of already mirror-checked read edges.
         * Yield before fresh I/O; yielding between cached groups unnecessarily
         * holds the shared reader while the current audio reserve drains. */
        if(++proof_edges<(f->operation==OP_READ?READ_PROOF_EDGES:4u)&&!f->fat_phase&&
           (f->cache[f->cluster%LN_FAT32_CACHE_SIZE].cluster==f->cluster||
            (f->operation==OP_READ&&f->verified_fat_sector==f->cluster/128u+1u)))goto verify_chain_cached;
        return 0;
    case SET_BEGIN:
        if(f->set_index==f->set_count+f->directory_count){
            f->proven_directory_count=f->directory_count;f->proof_base_valid=true;
            f->write_proven=true;return finish(f,1);
        }
        f->cluster=f->set_index<f->set_count?f->validated[f->set_index].cluster:
            f->directories[f->set_index-f->set_count];
        if(f->set_index<f->set_count)seek_map_select(f,&f->validated[f->set_index],true);
        else f->seek_slot=LN_FAT32_SEEK_MAPS;
        if(!f->cluster&&f->set_index<f->set_count&&!f->validated[f->set_index].size){
            f->terminals[f->set_index++]=0;return 0;
        }
        if(!cluster_valid(f,f->cluster)){result=LN_FAT32_CORRUPT;break;}
        f->cycle_anchor=f->cluster;f->cycle_power=1;f->cycle_distance=0;
        f->verified_clusters=1;f->chain_steps=0;f->fat_phase=0;f->state=SET_CHAIN;return 0;
    case SET_CHAIN:
set_chain_cached:
        seek_map_record(f,f->verified_clusters-1u,f->cluster);
        result=next_cluster(f);if(result!=1)break;
        if(f->fat_value>=EOC){
            if(f->set_index<f->set_count&&(uint64_t)f->verified_clusters*512u*f->sectors_per_cluster<
               f->validated[f->set_index].size){result=LN_FAT32_CORRUPT;break;}
            /* With cycles excluded, any shared cluster forces an identical
             * suffix and terminal cluster. Compare files AND path directories. */
            for(uint32_t i=0;i<f->set_index;++i)
                if(f->terminals[i]==f->cluster)return finish(f,LN_FAT32_CORRUPT);
            f->terminals[f->set_index]=f->cluster;
            if(f->set_index<f->set_count){
                const struct ln_fat32_file*file=&f->validated[f->set_index];
                verified_remember(f,file);
            }
            ++f->set_index;f->state=SET_BEGIN;return 0;
        }
        if(f->fat_value==f->cycle_anchor){result=LN_FAT32_CORRUPT;break;}
        if(++f->cycle_distance==f->cycle_power){
            f->cycle_anchor=f->fat_value;f->cycle_power*=2u;f->cycle_distance=0;
        }
        f->cluster=f->fat_value;++f->verified_clusters;
        if(++proof_edges<4u&&!f->fat_phase&&
           f->cache[f->cluster%LN_FAT32_CACHE_SIZE].cluster==f->cluster)goto set_chain_cached;
        return 0;
    case POSITION:
        if(!f->length)return finish(f,1);
        if(f->skip){
            result=next_cluster(f);if(result!=1)break;
            if(f->fat_value>=EOC){result=LN_FAT32_CORRUPT;break;}
            f->cluster=f->fat_value;--f->skip;++f->logical_cluster;
            seek_map_record(f,f->logical_cluster,f->cluster);
            f->seeks[f->logical_cluster%LN_FAT32_CACHE_SIZE]=(struct ln_fat32_seek){
                f->file.cluster,f->logical_cluster,f->cluster};
            return 0;
        }
        f->state=TRANSFER;return 0;
    case TRANSFER:
        if(f->transferred==f->length){
            if(f->operation==OP_WRITE){f->state=FLUSH;return 0;}
            return finish(f,1);
        }
        if(f->within==512u*f->sectors_per_cluster){
            result=next_cluster(f);if(result!=1)break;
            if(f->fat_value>=EOC){result=LN_FAT32_CORRUPT;break;}
            f->cluster=f->fat_value;f->within=0;++f->logical_cluster;
            seek_map_record(f,f->logical_cluster,f->cluster);
            f->seeks[f->logical_cluster%LN_FAT32_CACHE_SIZE]=(struct ln_fat32_seek){
                f->file.cluster,f->logical_cluster,f->cluster};
            return 0;
        }
        f->io_lba=cluster_sector(f,f->cluster)+f->within/512u;
        index=f->within%512u;bytes=512u-index;
        if(bytes>f->length-f->transferred)bytes=f->length-f->transferred;
        if(!index&&bytes==512u&&f->config.sectors_io){
            uint32_t sectors=(f->length-f->transferred)/512u;
            uint32_t cluster_sectors=(512u*f->sectors_per_cluster-f->within)/512u;
            if(sectors>cluster_sectors)sectors=cluster_sectors;
            uint32_t limit=f->operation==OP_READ?192u:8u;
            if(sectors>limit)sectors=limit;
            if(sectors){
                result=f->config.sectors_io(f->config.context,(enum ln_fat32_io)(f->operation-(OP_READ-LN_FAT32_READ)),f->io_lba,
                    f->buffer+f->transferred,sectors,f->generation);
                if(result!=1){if(result)result=LN_FAT32_IO;break;}
                bytes=sectors*512u;f->transferred+=bytes;f->within+=bytes;return 0;
            }
        }
        if(f->operation==OP_READ||index||bytes<512){
            result=io(f,LN_FAT32_READ,f->io_lba,f->sector);if(result!=1)break;
        }
        if(f->operation==OP_WRITE){
            for(uint32_t i=0;i<bytes;++i)f->sector[index+i]=f->buffer[f->transferred+i];
            f->state=WRITE_SECTOR;return 0;
        }
#if defined(__arm__)
        {extern void ln_memory_copy(void *,const void *,uint32_t);ln_memory_copy(f->buffer+f->transferred,f->sector+index,bytes);}
#else
        for(uint32_t i=0;i<bytes;++i)f->buffer[f->transferred+i]=f->sector[index+i];
#endif
        f->transferred+=bytes;f->within+=bytes;return 0;
    case WRITE_SECTOR:
        result=io(f,LN_FAT32_WRITE,f->io_lba,f->sector);if(result!=1)break;
        bytes=512u-f->within%512u;
        if(bytes>f->length-f->transferred)bytes=f->length-f->transferred;
        f->transferred+=bytes;f->within+=bytes;f->state=TRANSFER;return 0;
    case FLUSH:
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        return finish(f,1);
    case MUT_BEGIN:
        if(!mutable(f,f->new_size))return finish(f,LN_FAT32_READ_ONLY);
        if(f->slot_offset>480||(f->slot_offset&31u))return finish(f,LN_FAT32_CORRUPT);
        /* A declared FSInfo sector may never alias the boot sector. */
        if(f->fsinfo!=0xffffu&&(!f->fsinfo||
           (f->backup&&f->backup!=0xffffu&&(uint32_t)f->backup+f->fsinfo>=f->reserved)))
            return finish(f,LN_FAT32_FORMAT);
        f->state=MUT_DIR;return 0;
    case MUT_DIR:
        result=io(f,LN_FAT32_READ,f->slot_sector,f->sector);if(result!=1)break;
        p=f->sector+f->slot_offset;
        if(f->operation==OP_CREATE){
            if(p[0]!=(f->slot_end?0:0xe5))return finish(f,LN_FAT32_STALE);
            f->file=(struct ln_fat32_file){0,0,f->epoch,f->slot_sector,f->slot_offset,0x20};
        }else if((((uint32_t)ln_read_le16(p+20)<<16)|ln_read_le16(p+26))!=f->file.cluster||
                  ln_read_le32(p+28)!=f->file.size||p[11]!=f->file.attributes||!p[0]||p[0]==0xe5)
            return finish(f,LN_FAT32_STALE);
        for(index=0;index<32;++index)f->entry[index]=p[index];
        f->mutation_started=true;f->write_proven=false;f->edit_phase=0;
        f->proof_base_valid=false;
        seek_maps_clear(f);
        /* Publication may succeed even when its completion/flush is lost.
         * Retire old handles before ANY mutation, including failed operations. */
        if(++f->epoch==0)++f->epoch;
        for(index=0;index<LN_FAT32_CACHE_SIZE;++index){
            f->cache[index].cluster=0;f->verified[index].cluster=0;f->seeks[index].first=0;
        }
        f->new_first=f->new_tail=f->allocation_count=0;
        f->allocation_needed=((f->new_size-1u)>>(9u+f->cluster_shift))+1u;
        f->source_cluster=f->file.cluster;f->source_sector=0;f->copy_offset=0;
        f->state=MUT_DIRTY;return 0;
    case MUT_DIRTY:
        result=edit_fat(f,1,true,f->volume_flags&~0x08000000u);if(result!=1)break;
        f->state=MUT_DIRTY_FLUSH;return 0;
    case MUT_DIRTY_FLUSH:
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        f->directory_entry_index=0;
        if(f->fsinfo==0xffffu)begin_allocation_scan(f);else f->state=MUT_FSINFO;
        return 0;
    case MUT_FSINFO:
        f->io_lba=f->partition+f->fsinfo+(f->directory_entry_index?f->backup:0u);
        result=io(f,LN_FAT32_READ,f->io_lba,f->sector);if(result!=1)break;
        if(ln_read_le32(f->sector)!=0x41615252u||ln_read_le32(f->sector+484)!=0x61417272u||
           ln_read_le32(f->sector+508)!=0xaa550000u){result=LN_FAT32_CORRUPT;break;}
        ln_write_le32(f->sector+488,0xffffffffu);ln_write_le32(f->sector+492,0xffffffffu);
        f->state=MUT_FSINFO_WRITE;return 0;
    case MUT_FSINFO_WRITE:
        result=io(f,LN_FAT32_WRITE,f->io_lba,f->sector);if(result!=1)break;
        if(!f->directory_entry_index&&f->backup&&f->backup!=0xffffu){
            f->directory_entry_index=1;f->state=MUT_FSINFO;return 0;
        }
        begin_allocation_scan(f);return 0;
    case MUT_SCAN:
        if(f->allocation_count==f->allocation_needed){f->state=MUT_READY;return 0;}
        if(f->slot_found&&f->candidate>=f->directory_entry_index){result=LN_FAT32_NO_SPACE;break;}
        if(!cluster_valid(f,f->candidate)){
            if(!f->slot_found&&f->directory_entry_index>2){
                f->candidate=2;f->slot_found=true;f->fat_phase=0;return 0;
            }
            result=LN_FAT32_NO_SPACE;break;
        }
        result=scan_free_fat(f);if(result!=1)break;
        f->state=MUT_RESERVE;return 0;
    case MUT_RESERVE:
        result=edit_fat(f,f->candidate,true,0x0fffffffu);if(result!=1)break;
        if(!f->new_first)f->new_first=f->candidate;
        f->state=f->new_tail?MUT_LINK:MUT_DATA;
        f->destination_sector=0;return 0;
    case MUT_LINK:
        result=edit_fat(f,f->new_tail,true,f->candidate);if(result!=1)break;
        f->state=MUT_DATA;return 0;
    case MUT_DATA:
        if(f->destination_sector==f->sectors_per_cluster){
            f->new_tail=f->candidate;++f->candidate;++f->allocation_count;f->state=MUT_SCAN;return 0;
        }
        if(f->copy_offset<f->file.size){
            if(f->source_sector==f->sectors_per_cluster){f->cluster=f->source_cluster;f->state=MUT_OLD_NEXT;return 0;}
            result=io(f,LN_FAT32_READ,cluster_sector(f,f->source_cluster)+f->source_sector,f->sector);
            if(result!=1)break;
            bytes=(uint32_t)(f->file.size-f->copy_offset);if(bytes>512)bytes=512;
            for(index=bytes;index<512;++index)f->sector[index]=0;
        }else for(index=0;index<512;++index)f->sector[index]=0;
        f->state=MUT_DATA_WRITE;return 0;
    case MUT_OLD_NEXT:
        result=next_cluster(f);if(result!=1)break;
        if(f->fat_value>=EOC){result=LN_FAT32_CORRUPT;break;}
        f->source_cluster=f->fat_value;f->source_sector=0;f->state=MUT_DATA;return 0;
    case MUT_DATA_WRITE:
        result=io(f,LN_FAT32_WRITE,cluster_sector(f,f->candidate)+f->destination_sector,f->sector);
        if(result!=1)break;
        ++f->destination_sector;++f->source_sector;f->copy_offset+=512u;
        f->transferred=f->copy_offset>f->new_size?f->new_size:(uint32_t)f->copy_offset;
        f->state=MUT_DATA;return 0;
    case MUT_READY:
        /* Includes every FAT mirror, zero/copy sector and unknown FSInfo hint. */
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        f->state=MUT_PUBLISH_READ;return 0;
    case MUT_PUBLISH_READ:
        result=io(f,LN_FAT32_READ,f->slot_sector,f->sector);if(result!=1)break;
        p=f->sector+f->slot_offset;
        if(!ln_bytes_equal(p,f->entry,32)){result=LN_FAT32_STALE;break;}
        if(f->operation==OP_CREATE){
            for(index=0;index<32;++index)p[index]=0;
            for(index=0;index<11;++index)p[index]=f->name[index];
            p[11]=0x20;
            if(f->slot_end)p[32]=0; /* end slots at index15 were refused */
        }
        ln_write_le16(p+20,(uint16_t)(f->new_first>>16));ln_write_le16(p+26,(uint16_t)f->new_first);
        ln_write_le32(p+28,f->new_size);f->state=MUT_PUBLISH_WRITE;return 0;
    case MUT_PUBLISH_WRITE:
        result=io(f,LN_FAT32_WRITE,f->slot_sector,f->sector);if(result!=1)break;
        f->state=MUT_PUBLISH_FLUSH;return 0;
    case MUT_PUBLISH_FLUSH:
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        f->source_cluster=f->file.cluster;f->chain_steps=0;
        f->state=cluster_valid(f,f->source_cluster)?MUT_FREE_READ:MUT_RECLAIM_FLUSH;return 0;
    case MUT_FREE_READ:
        result=edit_fat(f,f->source_cluster,false,0);if(result!=1)break;
        if(++f->chain_steps>MAX_CHAIN||
           (f->edit_value<EOC&&!cluster_valid(f,f->edit_value))||f->edit_value==f->source_cluster){
            result=LN_FAT32_CORRUPT;break;
        }
        f->old_next=f->edit_value;f->state=MUT_FREE_WRITE;return 0;
    case MUT_FREE_WRITE:
        result=edit_fat(f,f->source_cluster,true,0);if(result!=1)break;
        f->source_cluster=f->old_next;
        f->state=f->old_next>=EOC?MUT_RECLAIM_FLUSH:MUT_FREE_READ;return 0;
    case MUT_RECLAIM_FLUSH:
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        f->state=MUT_CLEAN;return 0;
    case MUT_CLEAN:
        result=edit_fat(f,1,true,f->volume_flags);if(result!=1)break;
        f->state=MUT_CLEAN_FLUSH;return 0;
    case MUT_CLEAN_FLUSH:
        result=io(f,LN_FAT32_FLUSH,0,NULL);if(result!=1)break;
        f->old_next=cluster_valid(f,f->candidate)?f->candidate:2;
        if(++f->epoch==0)++f->epoch;
        seek_maps_clear(f);
        for(index=0;index<LN_FAT32_CACHE_SIZE;++index){
            f->cache[index].cluster=0;f->verified[index].cluster=0;f->seeks[index].first=0;
        }
        f->file.cluster=f->new_first;f->file.size=f->new_size;f->file.epoch=f->epoch;
        f->mutation_started=false;return finish(f,1);
    default:return finish(f,LN_FAT32_CORRUPT);
    }
    return result<0?finish(f,result):LN_FAT32_BUSY;
}
int ln_fat32_retire_read(struct ln_fat32*f,int result){
    if(!f||(f->operation!=OP_OPEN&&f->operation!=OP_READ&&f->operation!=OP_LIST)||
       (result!=LN_FAT32_STALE&&result!=LN_FAT32_IO))return LN_FAT32_ARGUMENT;
    /* Do not relabel evidence that FAT or the adapter had already rejected
     * before cancellation won the race. */
    if(!f->state&&f->result<0)return f->result;
    if(result==LN_FAT32_IO)return finish(f,result);
    if(f->operation==OP_OPEN){
        f->directory_count=f->open_directory_count;
        f->write_proven=f->open_write_proven;
        f->file=(struct ln_fat32_file){0};
    }else if(f->state==VERIFY_CHAIN&&f->seek_slot<LN_FAT32_SEEK_MAPS){
        /* This map was reset on admission and has not reached EOC, so none of
         * its sparse anchors may be published as a complete chain proof. */
        f->seek_maps[f->seek_slot].first=0;
        f->seek_slot=LN_FAT32_SEEK_MAPS;
    }
    f->state=IDLE;f->result=result;f->fat_phase=0;f->verified_fat_sector=0;
    f->buffer=NULL;return f->result;
}
