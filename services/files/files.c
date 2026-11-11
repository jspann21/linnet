#include "files.h"
#include "linnet_scene_ids.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "services/storage/storage.h"
#include "services/music/controller.h"
#include "services/music/player.h"
#include "hal/api/audio.h"
#include "hal/api/platform.h"
#include "kernel/runtime.h"
#if defined(LINNET_SOC_PP5022) && defined(LINNET_COVERFLOW)
#include "hal/api/ata.h"
#include "services/storage/block.h"
#include "services/music/library.h"
#include "services/music/coverflow.h"
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#include "services/video/catalog.h"
#endif
#endif
#if defined(LINNET_PHOTOS)
#include "services/photos/photos.h"
#endif
#define ROWS 16u
#define DEPTH 16u
#define RECORD 256u
#define TEXT_COLUMNS (LINNET_UI_WIDTH<220u?20u:24u)
#define GENERATION 0x46490001u
enum { CATALOG_OPEN=1,CATALOG_HEADER,CATALOG_ROOT,CATALOG_PAGE,ENTRY_OPEN,PREPARED_OPEN,LIVE,PREVIEW,READY };
enum { DIRECTORY,DETAILS,TEXT,HEX };
struct position {struct ln_fat32_directory cursor;uint32_t entry;};
struct folder {struct ln_fat32_file file;struct position position;uint32_t first,count,page,selected,record,parent;bool indexed;char title[96];};
struct entry {struct ln_fat32_file file;char name[128],extension[6];};
static struct {
    union {struct entry live[ROWS];uint8_t catalog[ROWS*RECORD];} rows;
    struct folder folder,parents[DEPTH];
    struct position cursor,sector_start,entry_start,next_page,checkpoints[16];
    struct ln_fat32_file catalog,chosen,original;
    uint8_t io[512];uint16_t long_name[260];char chosen_name[128],extension[6],leaf[14],label[96];
    uint32_t phase,view,depth,count,records,validate,offset,bytes,selected,lfn_sequence,lfn_checksum;
    uint32_t revision,drawn,target_page,request_bytes,preview_base,preview_size;
    uint32_t checkpoint_cluster[16],checkpoint_page[16],restore_row,prepared_kind;
    uint32_t prepared_epoch;
    bool active,pending,discard,done,sector_loaded,more,replaying;
    bool restoring,back_pending,prepared_text,info_only,returning;
    bool warming,warm_eligible;
    const char *message;
} f
#if defined(LINNET_COVERFLOW)
    /* The artwork bank has spare capacity, checked by the linker. Explicit
     * initialization is required because this region is not boot-cleared. */
    __attribute__((section(".cover_high"),aligned(32)))
#endif
;
static bool initialized;
/* Retain the playing folder independently of later Files navigation. Skip
 * walks its existing paged reader, keeping the current song until a validated
 * audio neighbor is ready. It never materializes an unbounded folder queue. */
static struct {
    struct folder folder,parents[DEPTH];
    uint32_t depth,selected;
    int32_t row;
    bool valid,pending,starting,previous,last;
} audio;
bool ln_files_audio_pending(void){return audio.pending;}
void ln_files_audio_cancel(void){
    bool pending=audio.pending;audio.pending=audio.starting=false;
    if(pending){
        ln_files_close();f.folder=audio.folder;f.depth=audio.depth;f.selected=audio.selected;
        for(uint32_t i=0;i<audio.depth;i++)f.parents[i]=audio.parents[i];
        f.done=f.replaying=f.restoring=false;f.validate=0;f.returning=true;f.view=DIRECTORY;
    }
}
bool ln_files_audio_next(bool previous){
    struct ln_fat32_file root;
    if(!initialized||!audio.valid||!ln_storage_media_root(&root)||root.epoch!=audio.folder.file.epoch)return false;
    if(audio.pending)return true;
    audio.pending=audio.starting=true;audio.previous=previous;
    if(f.pending){f.discard=true;ln_storage_media_cancel(GENERATION);}
    return true;
}
static void audio_remember(void){
    audio.folder=f.folder;audio.depth=f.depth;audio.selected=f.selected;
    for(uint32_t i=0;i<f.depth;i++)audio.parents[i]=f.parents[i];
    audio.valid=true;audio.pending=audio.starting=false;
}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_COVERFLOW)
/* Retain validated host pages and the live handles used to reach them. This
 * bounded reservation fits the unused tail of the existing artwork bank. */
struct cached_page {struct folder folder;uint32_t rows,age;bool valid;uint8_t data[ROWS*RECORD];};
static struct {
    struct cached_page page[2],*extra;
    uint32_t age,extra_count,extra_epoch;
} pages __attribute__((section(".cover_high"),aligned(32)));
_Static_assert(sizeof pages<=9000u,"Files page cache exceeds the artwork-bank allowance");
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
uint32_t ln_files_cache_maximum_bytes(void){return 512u*sizeof(struct cached_page);}
#endif
static struct cached_page *page_at(uint32_t i){return i<2u?&pages.page[i]:pages.extra+(i-2u);}
static uint32_t page_count(void){return 2u+pages.extra_count;}
static void pages_sync(void){
    uint32_t bytes=0,epoch=0;
    uint8_t *memory=ln_music_library_files_memory(&bytes,&epoch);
#if defined(LINNET_VIDEO)
    uint32_t video_bytes=0,video_epoch=0;uint8_t *video=ln_video_catalog_files_memory(&video_bytes,&video_epoch);
    if(video&&video_bytes>bytes){memory=video;bytes=video_bytes;epoch=video_epoch;}
#endif
    uint32_t art_bytes=0,art_epoch=0;uint8_t *art=ln_coverflow_files_memory(&art_bytes,&art_epoch);
    if(art&&art_bytes>bytes){memory=art;bytes=art_bytes;epoch=art_epoch;}
    uint32_t count=bytes/sizeof(struct cached_page);
    if(count>512u)count=512u;
    if((uint8_t*)pages.extra==memory&&pages.extra_count==count&&pages.extra_epoch==epoch)return;
    /* Forget a revoked pointer without touching bytes returned to its owner.
     * All scans/copies below are RAM-only and cannot reenter the catalog. */
    pages.extra=(struct cached_page*)memory;pages.extra_count=count;pages.extra_epoch=epoch;
    for(uint32_t i=0;i<count;++i)pages.extra[i].valid=false;
}
static void pages_clear(void){pages_sync();for(uint32_t i=0;i<page_count();++i)page_at(i)->valid=false;pages.age=0;}
static uint32_t pages_age(void){
    pages_sync();
    if(++pages.age==0){for(uint32_t i=0;i<page_count();++i)page_at(i)->age=0;pages.age=1;}
    return pages.age;
}
static void pages_save(void){
    if(!f.folder.indexed||!f.done||f.validate||f.pending||f.message)return;
    pages_sync();
    uint32_t slot=0;
    for(uint32_t i=0;i<page_count();++i){
        struct cached_page *p=page_at(i);
        if(p->valid&&p->folder.file.epoch==f.folder.file.epoch&&
           p->folder.record==f.folder.record&&p->folder.page==f.folder.page){slot=i;break;}
        if(!p->valid){slot=i;break;}
        if(p->age<page_at(slot)->age)slot=i;
    }
    struct cached_page *p=page_at(slot);p->valid=false;p->folder=f.folder;p->rows=f.count;
    __builtin_memcpy(p->data,f.rows.catalog,f.count*RECORD);
    p->age=pages_age();p->valid=true;
}
static bool pages_load(void){
    if(!f.folder.indexed)return false;
    struct ln_fat32_file root;
    if(!ln_storage_media_root(&root)||root.epoch!=f.folder.file.epoch){pages_clear();return false;}
    pages_sync();
    for(uint32_t i=0;i<page_count();++i){struct cached_page *p=page_at(i);if(p->valid&&
       p->folder.file.epoch==f.folder.file.epoch&&p->folder.record==f.folder.record&&
       p->folder.page==f.folder.page&&p->folder.first==f.folder.first&&
       p->folder.count==f.folder.count&&p->folder.file.cluster==f.folder.file.cluster){
        f.count=p->rows;__builtin_memcpy(f.rows.catalog,p->data,f.count*RECORD);
        f.done=true;f.more=f.folder.page*ROWS+f.count<f.folder.count;f.phase=READY;
        p->age=pages_age();return true;
    }}
    return false;
}
static bool pages_folder(uint32_t record,struct ln_fat32_file*out){
    struct ln_fat32_file root;
    if(!ln_storage_media_root(&root)||root.epoch!=f.folder.file.epoch){pages_clear();return false;}
    pages_sync();
    for(uint32_t i=0;i<page_count();++i){struct cached_page *p=page_at(i);if(p->valid&&
       p->folder.file.epoch==root.epoch&&p->folder.record==record&&
       p->folder.parent==f.folder.file.cluster){
        *out=p->folder.file;return true;
    }}
    return false;
}
#else
static void pages_clear(void){}
static void pages_save(void){}
static bool pages_load(void){return false;}
static bool pages_folder(uint32_t record,struct ln_fat32_file*out){(void)record;(void)out;return false;}
#endif
#if !defined(LINNET_PROFILE_RELEASE)
static struct {uint32_t started,last_us,reads,bytes,loads;bool measuring;} timing;
static void begin_timing(void){if(!timing.measuring){timing.started=ln_time_us();timing.reads=timing.bytes=0;timing.measuring=true;}}
void ln_files_diagnostics(uint32_t words[16]){
    uint32_t values[16]={0x454c4946u,f.phase,f.view,f.depth,f.count,f.folder.page,f.folder.indexed,f.pending,
        f.validate,f.done,timing.last_us,timing.reads,timing.bytes,timing.loads,ln_time_us(),timing.measuring};
    for(uint32_t i=0;i<16u;++i)words[i]=values[i];
}
#define FILES_DIAG(code) do {code;} while(0)
#else
#define FILES_DIAG(code) do {} while(0)
#endif
void ln_files_init(void){uint8_t*p=(uint8_t*)&f;for(uint32_t i=0;i<sizeof f;++i)p[i]=0;
    audio.valid=audio.pending=audio.starting=false;pages_clear();initialized=true;}

static void text(char*out,const char*in,uint32_t capacity){uint32_t n=0;while(in[n]&&n+1u<capacity){out[n]=in[n];++n;}out[n]=0;}
static char *number(char*out,uint32_t n){char digits[10];uint32_t count=0;do{digits[count++]=(char)('0'+n%10u);n/=10u;}while(n);while(count)*out++=digits[--count];*out=0;return out;}
static bool scene(const struct ln_ui_state*s){return s&&s->scene<ln_ui_scene_count&&s->scene==LN_UI_SCENE_FILES;}
static bool allowed(void){struct ln_player_status p;struct ln_audio_status a;ln_player_snapshot(&p);ln_audio_snapshot(&a);
    return p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR||p.phase==LN_PLAYER_PAUSE||
        (a.playing&&!a.paused&&a.queued>=ln_player_background_frames(a.rate)&&!ln_player_prefetch_urgent());}
#if defined(LINNET_SOC_PP5022) && defined(LINNET_COVERFLOW)
static bool warm_allowed(const struct ln_ui_state*s){
    if(!s||s->scene!=LN_UI_SCENE_HOME||ln_storage_busy()||ln_player_busy()||
       ln_music_library_status()!=LN_LIBRARY_READY)return false;
#if defined(LINNET_VIDEO)
    if(ln_video_active()||ln_video_pending())return false;
#endif
#if defined(LINNET_PHOTOS)
    if(ln_photos_active()||ln_photos_pending())return false;
#endif
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    struct ln_block_info block;ln_block_snapshot(&block);
    return disk.powered&&disk.initialized&&!disk.initializing&&!disk.busy&&
        !disk.sleeping&&!disk.failed&&!block.power_transition;
}
#else
static bool warm_allowed(const struct ln_ui_state*s){(void)s;return false;}
#endif
static void changed(void){++f.revision;}
static void fail(const char*message){f.message=message;f.phase=READY;f.done=true;changed();}
static void reset_selection(struct ln_ui_state*s,uint32_t row){
    f.restore_row=row;f.restoring=f.phase!=READY||f.validate;
    s->selected=row;s->first_visible[s->scene]=0;ln_ui_refresh(s);
    /* A cached page is final before this input paints. Do not publish the
     * same revision again on the next Files step. Async loads still restore
     * selection after their actual row count becomes known. */
    if(!f.restoring)f.drawn=f.revision;
}
static const uint8_t *record(uint32_t row){return f.rows.catalog+row*RECORD;}
static const char *name(uint32_t row){return f.folder.indexed?(const char*)record(row)+32:f.rows.live[row].name;}
static bool directory(uint32_t row){return ((f.folder.indexed?ln_read_le32(record(row)):f.rows.live[row].file.attributes)&16u)!=0;}
static void load_page(void){
    FILES_DIAG(begin_timing());
    f.count=f.validate=0;f.done=f.sector_loaded=false;f.lfn_sequence=0;f.message=0;f.view=DIRECTORY;
    f.cursor=f.folder.position;f.more=false;f.phase=f.folder.indexed?CATALOG_PAGE:LIVE;changed();
    (void)pages_load();
}
static void live_folder(void){
    pages_clear();
    f.folder.indexed=false;f.folder.page=0;f.folder.position=(struct position){.cursor={.cluster=f.folder.file.cluster,.epoch=f.folder.file.epoch}};
    f.checkpoints[0]=f.folder.position;f.checkpoint_cluster[0]=f.folder.file.cluster;f.checkpoint_page[0]=0;load_page();
}
static void audio_finish(bool found){
    audio.pending=audio.starting=false;
    if(!found){
        f.folder=audio.folder;f.depth=audio.depth;f.selected=audio.selected;
        f.done=f.replaying=f.restoring=false;f.validate=0;f.phase=READY;f.returning=true;f.view=DIRECTORY;
    }
    ln_music_file_transport_finished(found);
}
static bool record_valid(const uint8_t*r){
    if(ln_read_le32(r+252)!=ln_crc32(r,252)||r[159]||r[173]||!r[32]||!r[160]||r[251])return false;
    if(ln_read_le32(r+16)>3u||ln_read_le32(r+24)>2u||ln_read_le32(r+28)>3u)return false;
    if((!ln_read_le32(r+16)||ln_read_le32(r+16)==3u)&&ln_read_le16(r+174))return false;
    if(ln_read_le32(r+16)&&r[176]!='/')return false;
    uint32_t first=ln_read_le32(r+8),count=ln_read_le32(r+12),attributes=ln_read_le32(r);
    if(attributes&~63u)return false;
    if(attributes&16u){if(first>f.records||count>f.records-first||(count&&!first))return false;}
    for(uint32_t i=160;r[i];++i)if(i>=173u||r[i]<33u||r[i]>126u||r[i]=='/'||r[i]=='\\')return false;
    return true;
}
static void alias_name(const uint8_t*r,char*out){
    uint32_t n=0;for(uint32_t i=0;i<8&&r[i]!=' ';++i){uint8_t c=r[i];if(!i&&c==5)c=0xe5;out[n++]=(char)(c>=32&&c<127?c:'?');}
    if(r[8]!=' '){out[n++]='.';for(uint32_t i=8;i<11&&r[i]!=' ';++i)out[n++]=(char)(r[i]>=32&&r[i]<127?r[i]:'?');}out[n]=0;
}
static void long_slot(const uint8_t*r){
    static const uint8_t positions[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t seq=r[0]&31u;
    if(r[0]&64u){f.lfn_sequence=seq;f.lfn_checksum=r[13];f.entry_start=f.sector_start;f.entry_start.entry=f.cursor.entry-1u;
        for(uint32_t i=0;i<260u;++i)f.long_name[i]=0xffffu;}
    if(!seq||seq>20u||r[0]!=(seq|(r[0]&64u))||seq!=f.lfn_sequence||r[13]!=f.lfn_checksum||r[12]||ln_read_le16(r+26)){f.lfn_sequence=0;return;}
    for(uint32_t i=0;i<13;++i)f.long_name[(seq-1u)*13u+i]=ln_read_le16(r+positions[i]);
    --f.lfn_sequence;if(!f.lfn_sequence)f.lfn_sequence=UINT32_MAX;
}
static void entry_name(const uint8_t*r,char*out){
    uint8_t sum=0;for(uint32_t i=0;i<11;++i)sum=(uint8_t)(((sum&1u)<<7)+(sum>>1)+r[i]);
    if(f.lfn_sequence!=UINT32_MAX||sum!=f.lfn_checksum){alias_name(r,out);return;}
    uint32_t n=0;
    for(uint32_t i=0;i<255u&&f.long_name[i]&&f.long_name[i]!=0xffffu;++i){uint32_t c=f.long_name[i];
        if(c>=0xd800u&&c<=0xdbffu&&i+1<255u&&f.long_name[i+1]>=0xdc00u&&f.long_name[i+1]<=0xdfffu)c=0x10000u+((c-0xd800u)<<10)+f.long_name[++i]-0xdc00u;
        else if(c<32u||(c>=0xd800u&&c<=0xdfffu))c='?';
        uint32_t bytes=c<128u?1u:c<2048u?2u:c<65536u?3u:4u;if(n+bytes>=128u)break;
        if(bytes==1)out[n++]=(char)c;else{out[n++]=(char)((bytes==2?0xc0u:bytes==3?0xe0u:0xf0u)|(c>>(6u*(bytes-1u))));
            for(uint32_t j=bytes-1u;j;--j)out[n++]=(char)(0x80u|((c>>(6u*(j-1u)))&63u));}
    }out[n]=0;if(!n)alias_name(r,out);
}
static void live_entry(void){
    if(f.cursor.entry==16u){f.sector_loaded=false;f.cursor.entry=0;return;}
    uint32_t at=f.cursor.entry++;const uint8_t*r=f.io+at*32u;
    if(!r[0]){f.done=true;f.phase=READY;changed();return;}
    if(r[0]==0xe5u){f.lfn_sequence=0;return;}
    if(r[11]==15u){long_slot(r);return;}
    if((r[11]&8u)||r[0]=='.'){f.lfn_sequence=0;return;}
    if(f.lfn_sequence!=UINT32_MAX){f.entry_start=f.sector_start;f.entry_start.entry=at;}
    if(f.count==ROWS){f.next_page=f.entry_start;f.more=f.done=true;f.phase=READY;changed();return;}
    struct entry*e=&f.rows.live[f.count++];entry_name(r,e->name);
    uint8_t checksum=0;for(uint32_t i=0;i<11u;++i)checksum=(uint8_t)(((checksum&1u)<<7)+(checksum>>1)+r[i]);
    uint32_t at_ext=0,n=0;e->extension[0]=0;
    if(f.lfn_sequence==UINT32_MAX&&checksum==f.lfn_checksum){
        for(uint32_t i=0;i<255u&&f.long_name[i]&&f.long_name[i]!=0xffffu;++i)if(f.long_name[i]=='.')at_ext=i+1u;
        if(at_ext)for(;n<5u&&at_ext+n<255u&&f.long_name[at_ext+n]&&f.long_name[at_ext+n]!=0xffffu;++n)e->extension[n]=(char)f.long_name[at_ext+n];
    }else for(;n<3u&&r[8u+n]!=' ';++n)e->extension[n]=(char)r[8u+n];
    e->extension[n]=0;f.lfn_sequence=0;
    e->file=(struct ln_fat32_file){.cluster=((uint32_t)ln_read_le16(r+20)<<16)|ln_read_le16(r+26),.size=ln_read_le32(r+28),
        .epoch=f.cursor.cursor.epoch,.directory_sector=f.cursor.cursor.lba,.directory_offset=(uint16_t)(at*32u),.attributes=r[11]};
}
static void preview(void){FILES_DIAG(begin_timing());f.phase=PREVIEW;f.bytes=0;f.message=0;changed();}
static bool suffix(const char*extension){const char*dot=f.extension;
    while(*dot&&*extension){char c=*dot++;if(c>='A'&&c<='Z')c+=32;if(c!=*extension++)return false;}return !*dot&&!*extension;}
static uint32_t kind(void){
    if(f.folder.indexed)return ln_read_le32(record(f.selected)+28);
    if(suffix("mp3")||suffix("m4a")||suffix("m4b")||suffix("aac")||suffix("flac")||suffix("flc")||suffix("wav")||suffix("aif")||suffix("aiff")||suffix("aifc")||suffix("ogg")||suffix("opus")||suffix("ops")||suffix("wv"))return 1;
    if(suffix("mp4")||suffix("m4v")||suffix("mov")||suffix("ipvf"))return 2;
    return suffix("jpg")||suffix("jpeg")?3u:0u;
}
static void open_chosen(struct ln_ui_state*s){
    bool transport=audio.pending;
    if(f.chosen.attributes&16u){
        if(f.depth==DEPTH){fail("Folder depth limit (16)");return;}
        f.folder.selected=f.selected;f.parents[f.depth++]=f.folder;
        struct folder next={.file=f.chosen,.indexed=f.folder.indexed,.record=f.folder.first+f.folder.page*ROWS+f.selected,.parent=f.folder.file.cluster};text(next.title,f.chosen_name,sizeof next.title);
        if(next.indexed){next.first=ln_read_le32(record(f.selected)+8);next.count=ln_read_le32(record(f.selected)+12);if(ln_read_le32(record(f.selected)+24)==2u)next.indexed=false;}
        f.folder=next;if(next.indexed)load_page();else live_folder();reset_selection(s,0);return;
    }
    f.original=f.chosen;f.prepared_text=false;f.prepared_kind=0;f.preview_base=0;f.preview_size=f.chosen.size;
    f.view=DETAILS;f.message=0;f.offset=0;f.phase=READY;if(!transport)reset_selection(s,0);changed();
    if(f.info_only){f.info_only=false;return;}
    if(f.folder.indexed&&(f.prepared_kind=ln_read_le32(record(f.selected)+16))!=0){f.phase=PREPARED_OPEN;return;}
    if(kind()==1u){
        /* Replacement now owns a validated file. Its normal music Stop must
         * not cancel this completed lookup and restore the previous row. */
        if(transport)audio.pending=false;
        if(ln_music_open_file(&f.chosen,f.chosen_name)){
            audio_remember();ln_music_file_transport_finished(true);
            f.view=DIRECTORY;f.returning=true;if(!transport)(void)ln_ui_show(s,"linnet.system.now-playing");return;
        }
        if(transport)audio_finish(false);else f.message="Audio unavailable";
        return;
    }
#if defined(LINNET_VIDEO)
    if(kind()==2u){
        if(ln_runtime_video_open_file(&f.chosen,f.chosen_name)){f.view=DIRECTORY;f.returning=true;return;}f.message="Video unavailable";
    }
#endif
#if defined(LINNET_PHOTOS)
    if(kind()==3u){
        if(ln_photos_open_file(&f.chosen,f.chosen_name)){f.view=DIRECTORY;f.returning=true;(void)ln_ui_show(s,"linnet.system.photos-viewer");return;}
        f.message="JPEG exceeds viewer limits";
    }
#endif
    /* Text detection is bounded to the first page; unknown binaries always
     * retain a byte viewer rather than attempting to execute their contents. */
    f.view=TEXT;preview();
}
static void choose_row(struct ln_ui_state*s,uint32_t row,bool info){
    f.info_only=info&&!directory(row);f.selected=row;
    text(f.chosen_name,name(row),sizeof f.chosen_name);
    if(f.folder.indexed){
        if(ln_read_le32(record(row)+24)==1u){
            if(audio.pending)audio_finish(false);else{live_folder();reset_selection(s,0);}
            return;
        }
        if(directory(row)&&ln_read_le32(record(row)+24)==0u&&pages_folder(f.folder.first+f.folder.page*ROWS+row,&f.chosen)){open_chosen(s);return;}
        f.leaf[0]='/';text(f.leaf+1,(const char*)record(row)+160,sizeof f.leaf-1u);
        f.phase=ENTRY_OPEN;FILES_DIAG(begin_timing());
    }else{
        f.chosen=f.rows.live[row].file;text(f.extension,f.rows.live[row].extension,sizeof f.extension);open_chosen(s);
    }
}
static void audio_advance(struct ln_ui_state*s){
    if(!audio.pending||audio.starting||f.pending||f.phase!=READY||!f.done||f.validate||f.replaying)return;
    if(f.message){audio_finish(false);return;}
    if(audio.last){audio.row=(int32_t)f.count-1;audio.last=false;}
    for(uint32_t n=0;n<ROWS;n++){
        if(audio.row<0){
            if(!f.folder.page){audio_finish(false);return;}
            --f.folder.page;audio.last=true;
            if(!f.folder.indexed){
                uint32_t at=f.folder.page%16u;
                if(f.checkpoint_cluster[at]==f.folder.file.cluster&&f.checkpoint_page[at]==f.folder.page)f.folder.position=f.checkpoints[at];
                else{f.target_page=f.folder.page;live_folder();f.replaying=true;return;}
            }
            load_page();return;
        }
        if((uint32_t)audio.row>=f.count){
            if(!f.more){audio_finish(false);return;}
            ++f.folder.page;audio.row=0;
            if(!f.folder.indexed){
                f.folder.position=f.next_page;uint32_t at=f.folder.page%16u;
                f.checkpoints[at]=f.next_page;f.checkpoint_cluster[at]=f.folder.file.cluster;f.checkpoint_page[at]=f.folder.page;
            }
            load_page();return;
        }
        uint32_t row=(uint32_t)audio.row;f.selected=row;
        if(!f.folder.indexed)text(f.extension,f.rows.live[row].extension,sizeof f.extension);
        if(!directory(row)&&kind()==1u){choose_row(s,row,false);return;}
        audio.row+=audio.previous?-1:1;
    }
}
void ln_files_close(void){
    if(!initialized)return;FILES_DIAG(timing.measuring=false);
    audio.pending=audio.starting=false;
    /* A canceled hidden first-page load must not masquerade as a live folder
     * on the next open. Its I/O buffer still belongs to the drain below. */
    if(f.warming&&!f.done){
        f.folder=(struct folder){0};f.count=f.validate=0;
        f.done=f.more=f.restoring=f.replaying=false;f.message=0;
    }
    f.active=f.warming=f.warm_eligible=false;f.back_pending=false;
    if(f.pending){f.discard=true;ln_storage_media_cancel(GENERATION);}else f.phase=READY;
}
bool ln_files_pending(void){return initialized&&(f.pending||(f.active&&(f.phase!=READY||f.validate)));}
uint32_t ln_files_deadline(uint32_t now){
    if(audio.pending&&!f.pending)return now;
    if(!initialized||(!f.active&&!f.warming)||f.pending)return now+200000u;
    if(f.warming&&!f.warm_eligible&&!f.validate)return now+200000u;
    if(f.validate||(f.phase==LIVE&&f.sector_loaded)||(f.phase!=READY&&allowed()))return now;
    return now+200000u;
}
const char *ln_files_title(void){if(!initialized)return "Files";return f.view==DIRECTORY?(f.folder.title[0]?f.folder.title:"Files"):f.chosen_name;}
const char *ln_files_hint(void){if(!initialized)return "Select: Open";return f.view==DIRECTORY?"Left: Info  Right: Live":f.view==DETAILS?"Select: View  Menu: Back":"Left/Right: Page";}
uint32_t ln_files_count(void){
    if(!initialized)return 1;
    if(f.view==DETAILS)return 4;
    if(f.view==TEXT||f.view==HEX)return f.bytes?(f.view==HEX?(f.bytes+3u)/4u:(f.bytes+TEXT_COLUMNS-1u)/TEXT_COLUMNS):1u;
    return (f.folder.page?1u:0u)+f.count+((!f.done||f.more||!f.count||f.message)?1u:0u);
}
bool ln_files_navigates(uint32_t row){if(!initialized)return false;if(f.view!=DIRECTORY)return false;if(f.folder.page){if(!row)return true;--row;}return row<f.count?directory(row):f.more;}
const char *ln_files_label(uint32_t row){
    if(!initialized)return "Loading...";
    if(f.view==DIRECTORY){if(f.folder.page){if(!row)return "Previous page";--row;}if(row<f.count)return name(row);
        return f.message?f.message:!f.done?"Loading...":f.more?"Next page":"Empty folder";}
    if(f.view==DETAILS){if(!row){char*p=f.label;text(p,"Size: ",sizeof f.label);p=number(p+6,f.original.size);text(p," bytes",16);return f.label;}
        if(row==1)return "View as text";if(row==2)return "View as bytes";return f.message?f.message:"Menu: Return to folder";}
    if(!f.bytes)return f.message?f.message:f.phase==PREVIEW?"Loading...":"Empty file";
    uint32_t stride=f.view==HEX?4u:TEXT_COLUMNS,at=row*stride;if(at>=f.bytes)return "";
    char*p=f.label;
    if(f.view==HEX){static const char hex[]="0123456789ABCDEF";uint32_t offset=f.offset+at;
        for(uint32_t i=8;i;--i)*p++=hex[(offset>>((i-1u)*4u))&15u];*p++=' ';
        for(uint32_t i=0;i<4u&&at+i<f.bytes;++i){uint32_t c=f.io[at+i];*p++=hex[c>>4];*p++=hex[c&15u];*p++=' ';}
    }else for(uint32_t i=0;i<stride&&at+i<f.bytes;++i){uint32_t c=f.io[at+i];*p++=(char)(c>=32u&&c<127u?c:' ');}
    *p=0;return f.label;
}
static void go_back(struct ln_ui_state*s){
    f.validate=0;f.replaying=false;
    if(f.view!=DIRECTORY){f.view=DIRECTORY;f.phase=READY;f.message=0;changed();
        FILES_DIAG(timing.measuring=false;timing.last_us=timing.reads=timing.bytes=0;++timing.loads);
        reset_selection(s,f.selected+(f.folder.page?1u:0u));}
    else if(f.depth){f.folder=f.parents[--f.depth];load_page();reset_selection(s,f.folder.selected+(f.folder.page?1u:0u));}
    else{ln_files_close();(void)ln_ui_back(s);}
}
void ln_files_step(struct ln_ui_state*s){
    if(audio.pending&&!ln_music_file_mode())ln_files_audio_cancel();
    if(audio.pending&&scene(s))ln_files_audio_cancel();
    /* Recovery can remount storage without leaving Files. Drain the old read
     * before dropping handles or reusing a directory buffer. */
    struct ln_fat32_file root;
    if(!f.pending&&f.folder.file.epoch&&ln_storage_media_root(&root)&&f.folder.file.epoch!=root.epoch){
        if(audio.pending)audio_finish(false);audio.valid=false;
        f.folder=(struct folder){0};f.depth=0;f.view=DIRECTORY;f.count=f.validate=0;
        f.done=f.more=f.returning=f.replaying=f.restoring=false;f.message=0;pages_clear();
        for(uint32_t i=0;i<16u;++i)f.checkpoint_cluster[i]=0;
        f.active=f.warming=f.warm_eligible=false;
    }
    if(audio.pending&&audio.starting&&!f.pending){
        f.folder=audio.folder;f.depth=audio.depth;
        for(uint32_t i=0;i<audio.depth;i++)f.parents[i]=audio.parents[i];
        for(uint32_t i=0;i<16u;i++)f.checkpoint_cluster[i]=0;
        uint32_t at=f.folder.page%16u;
        f.checkpoints[at]=f.folder.position;f.checkpoint_cluster[at]=f.folder.file.cluster;f.checkpoint_page[at]=f.folder.page;
        audio.row=(int32_t)audio.selected+(audio.previous?-1:1);audio.last=false;audio.starting=false;
        f.active=true;f.warming=f.restoring=f.replaying=f.returning=false;load_page();
    }
    f.warm_eligible=false;
    if(scene(s)&&f.warming){f.warming=false;f.active=true;}
    else if(!scene(s)&&!f.active&&!f.pending){
        if(f.warming)f.warm_eligible=warm_allowed(s);
        else if(ln_storage_media_root(&root)&&f.prepared_epoch!=root.epoch&&warm_allowed(s)){
            f.warm_eligible=true;
            f.prepared_epoch=root.epoch;
            /* Retained foreground navigation is already prepared. Never
             * replace the user's chosen folder with a background root. */
            if(!f.folder.file.epoch){
                f.folder=(struct folder){.file=root};f.view=DIRECTORY;
                f.depth=f.count=f.validate=0;f.done=f.more=f.restoring=f.replaying=false;
                f.message=0;
                text(f.folder.title,"Files",sizeof f.folder.title);
                f.phase=CATALOG_OPEN;f.warming=true;FILES_DIAG(begin_timing());
            }
        }
    }
    if(!scene(s)){if(f.active&&!audio.pending)ln_files_close();}
    else if(!f.active&&!f.pending){
        struct ln_fat32_file root;
        if(ln_storage_media_root(&root)&&f.folder.file.epoch!=root.epoch){f.folder=(struct folder){0};f.depth=0;f.view=DIRECTORY;
            f.count=f.validate=0;f.done=f.more=f.returning=false;f.message=0;
            for(uint32_t i=0;i<16u;++i)f.checkpoint_cluster[i]=0;pages_clear();}
        f.active=true;if(!f.folder.file.epoch){if(!ln_storage_media_root(&f.folder.file)){fail("Storage unavailable");return;}
            text(f.folder.title,"Files",sizeof f.folder.title);f.phase=CATALOG_OPEN;FILES_DIAG(begin_timing());}
        else if(f.view==DIRECTORY){uint32_t row=f.returning?f.selected+(f.folder.page?1u:0u):s->selected;f.returning=false;
            /* The mount epoch above invalidates retained handles. The current
             * page is otherwise still valid; Right explicitly refreshes it. */
            if(f.done&&!f.validate){changed();FILES_DIAG(timing.last_us=timing.reads=timing.bytes=0;++timing.loads);}
            else load_page();reset_selection(s,row);
        }else if(f.view==TEXT||f.view==HEX)preview();
    }
    if(f.pending){uint32_t bytes=0;struct ln_fat32_file opened;int r=ln_storage_media_poll_deferred(GENERATION,&opened,&bytes);if(!r)return;f.pending=false;
        FILES_DIAG(timing.bytes+=bytes);
        if(f.discard||(!f.active&&!f.warming)){f.discard=false;f.phase=READY;if(f.back_pending){f.back_pending=false;go_back(s);}return;}
        if(r<0){if(audio.pending)audio_finish(false);else if(f.phase<=CATALOG_PAGE){live_folder();}else if(f.phase==ENTRY_OPEN){live_folder();reset_selection(s,0);}else fail("Read failed - reopen Files");}
        else if(f.phase==CATALOG_OPEN){f.catalog=opened;f.phase=CATALOG_HEADER;}
        else if(f.phase==CATALOG_HEADER){
            f.records=ln_read_le32(f.io+8);
            uint32_t version=ln_read_le32(f.io+4);
            if(bytes!=512u||ln_read_le32(f.io)!=0x534c4946u||version!=2u||!f.records||f.records>1000000u||
               ln_read_le32(f.io+12)!=RECORD||f.catalog.size!=512u+f.records*RECORD||ln_crc32(f.io,508)!=ln_read_le32(f.io+508))live_folder();
            else {f.phase=CATALOG_ROOT;}
        }else if(f.phase==CATALOG_ROOT){
            if(bytes!=RECORD||!record_valid(f.io)||!(ln_read_le32(f.io)&16u))live_folder();
            else{f.folder.first=ln_read_le32(f.io+8);f.folder.count=ln_read_le32(f.io+12);f.folder.indexed=true;load_page();}
        }else if(f.phase==CATALOG_PAGE){if(bytes!=f.request_bytes){if(audio.pending)audio_finish(false);else live_folder();}else {f.validate=bytes/RECORD;f.phase=READY;}}
        else if(f.phase==ENTRY_OPEN){
            if(((opened.attributes^ln_read_le32(record(f.selected)))&16u)||(!(opened.attributes&16u)&&opened.size!=ln_read_le32(record(f.selected)+4))){
                if(audio.pending)audio_finish(false);else{live_folder();reset_selection(s,0);}
            }
            else{f.chosen=opened;open_chosen(s);}
        }else if(f.phase==PREPARED_OPEN){
            uint32_t base=(uint32_t)ln_read_le16(record(f.selected)+174)*512u,size=ln_read_le32(record(f.selected)+20);
            if(base>opened.size||size>opened.size-base||(f.prepared_kind==3u&&size!=opened.size))fail("Preview changed - prepare Files");
            else {
#if defined(LINNET_VIDEO)
                if(f.prepared_kind==3u&&ln_runtime_video_open_file(&opened,f.chosen_name)){f.view=DIRECTORY;f.phase=READY;f.returning=true;}
                else
#endif
#if defined(LINNET_PHOTOS)
                if(f.prepared_kind==1u&&ln_photos_open_prepared(&opened,base,size,f.chosen_name)){f.view=DIRECTORY;f.phase=READY;f.returning=true;(void)ln_ui_show(s,"linnet.system.photos-viewer");}
                else
#endif
                if(f.prepared_kind==2u){f.chosen=opened;f.preview_base=base;f.preview_size=size;f.prepared_text=true;f.view=TEXT;preview();}
                else fail("Preview unavailable");
            }
        }else if(f.phase==LIVE){
            if(!bytes){f.done=true;f.phase=READY;changed();}else if(bytes!=512u)fail("Directory read failed");else f.sector_loaded=true;
        }else if(f.phase==PREVIEW){f.bytes=bytes;f.phase=READY;
            if(f.view==TEXT&&!f.prepared_text&&f.offset==0)for(uint32_t i=0;i<bytes;++i)if(!f.io[i]||(f.io[i]<32u&&f.io[i]!=9u&&f.io[i]!=10u&&f.io[i]!=13u)){f.view=HEX;break;}changed();}
    }
    if(!f.active&&!f.warming)return;
    if(f.validate){for(uint32_t step=0;step<4u&&f.validate;++step){uint32_t row=f.count;
        if(!record_valid(record(row))){f.validate=0;if(audio.pending)audio_finish(false);else live_folder();break;}
        ++f.count;--f.validate;if(!f.validate){f.done=true;f.more=f.folder.page*ROWS+f.count<f.folder.count;pages_save();changed();}}}
    else if(f.phase==LIVE&&f.sector_loaded){for(uint32_t i=0;i<16u&&f.phase==LIVE&&f.sector_loaded;++i)live_entry();}
    /* Check after validation too: a bad page must not admit even the first
     * live-directory read as hidden preparation. */
    if(f.warming&&(f.phase==LIVE||f.message)){
        f.folder=(struct folder){0};f.warming=f.warm_eligible=false;f.phase=READY;
        f.count=f.validate=0;f.done=f.more=f.restoring=f.replaying=false;f.message=0;
        FILES_DIAG(timing.measuring=false);return;
    }
    if(f.warming&&!f.pending)f.warm_eligible=warm_allowed(s);
    if(!f.pending&&f.phase!=READY&&!ln_storage_busy()&&allowed()&&(!f.warming||f.warm_eligible)){
        int r=0;
        if(f.phase==CATALOG_OPEN)r=ln_storage_media_open("/.LINNET/FILES/INDEX.FIL",GENERATION);
        else if(f.phase==CATALOG_HEADER)r=ln_storage_media_read(&f.catalog,0,f.io,512,GENERATION);
        else if(f.phase==CATALOG_ROOT)r=ln_storage_media_read(&f.catalog,512,f.io,RECORD,GENERATION);
        else if(f.phase==CATALOG_PAGE){uint32_t first=f.folder.page*ROWS,n=f.folder.count-first;if(n>ROWS)n=ROWS;
            if(!n){f.done=true;f.phase=READY;pages_save();changed();}else{f.request_bytes=n*RECORD;r=ln_storage_media_read(&f.catalog,512u+(f.folder.first+first)*RECORD,f.rows.catalog,f.request_bytes,GENERATION);}}
        else if(f.phase==ENTRY_OPEN)r=ln_storage_media_open_at(&f.folder.file,f.leaf,GENERATION);
        else if(f.phase==PREPARED_OPEN)r=ln_storage_media_open((const char*)record(f.selected)+176,GENERATION);
        else if(f.phase==LIVE&&!f.sector_loaded){f.sector_start=f.cursor;r=ln_storage_media_list(&f.cursor.cursor,f.io,GENERATION);}
        else if(f.phase==PREVIEW){uint32_t n=f.preview_size-f.offset;if(n>480u)n=480u;
            if(n)r=ln_storage_media_read(&f.chosen,f.preview_base+f.offset,f.io,n,GENERATION);else{f.phase=READY;changed();}}
        if(r>0){f.pending=true;FILES_DIAG(++timing.reads);}else if(r<0){if(audio.pending)audio_finish(false);else if(f.phase<=CATALOG_PAGE)live_folder();else fail("File unavailable");}
    }
    if(f.replaying&&f.done){
        if(f.folder.page<f.target_page&&f.more){++f.folder.page;f.folder.position=f.next_page;f.checkpoints[f.folder.page%16u]=f.next_page;f.checkpoint_cluster[f.folder.page%16u]=f.folder.file.cluster;f.checkpoint_page[f.folder.page%16u]=f.folder.page;load_page();}
        else{f.replaying=false;if(!audio.pending)reset_selection(s,f.folder.selected);}
    }
    audio_advance(s);
    if(f.restoring&&f.phase==READY&&!f.validate&&scene(s)){f.restoring=false;s->selected=f.restore_row;ln_ui_refresh(s);}
    if(scene(s)&&f.drawn!=f.revision){f.drawn=f.revision;ln_ui_refresh(s);}
    FILES_DIAG(if(timing.measuring&&f.phase==READY&&!f.pending&&!f.validate){timing.last_us=ln_time_us()-timing.started;++timing.loads;timing.measuring=false;});
    if(f.warming&&f.phase==READY&&!f.pending&&!f.validate)f.warming=f.warm_eligible=false;
}
bool ln_files_input(struct ln_ui_state*s,uint32_t kind,int32_t value){
    if(!initialized||!scene(s)||s->hold||kind!=2u)return false;
    uint32_t pressed=(uint32_t)value&~s->buttons;if(!(pressed&29u))return false;s->buttons=(uint32_t)value;
    /* A transport lookup shares this paged reader. Cancel it before accepting
     * browsing changes; an admitted read must drain before buffer reuse. */
    if(audio.pending)ln_files_audio_cancel();
    /* Cancel and drain before reusing either the directory or preview buffer. */
    if(f.pending){if(pressed&1u){f.discard=f.back_pending=true;ln_storage_media_cancel(GENERATION);}return true;}
    if(pressed&1u){go_back(s);return true;}
    if(f.view==DETAILS){if(pressed&16u){if(s->selected==1||s->selected==2){f.view=s->selected==1?TEXT:HEX;f.chosen=f.original;f.prepared_text=false;f.preview_base=0;f.preview_size=f.chosen.size;f.offset=0;preview();reset_selection(s,0);}}return true;}
    if(f.view==TEXT||f.view==HEX){
        if(pressed&16u){f.view=DETAILS;changed();reset_selection(s,0);}
        else if((pressed&4u)&&f.preview_size-f.offset>480u){f.offset+=480u;preview();reset_selection(s,0);}
        else if((pressed&8u)&&f.offset){f.offset-=480u;preview();reset_selection(s,0);}return true;
    }
    if(pressed&4u){live_folder();reset_selection(s,0);return true;}
    if(!(pressed&(16u|8u))||!f.done||f.replaying)return true;
    uint32_t row=s->selected;
    if(f.folder.page&&!row){
        --f.folder.page;
        if(!f.folder.indexed){/* Recent pages are O(1); older pages replay bounded sectors. */
            if(f.checkpoint_cluster[f.folder.page%16u]==f.folder.file.cluster&&f.checkpoint_page[f.folder.page%16u]==f.folder.page)f.folder.position=f.checkpoints[f.folder.page%16u];
            else {f.target_page=f.folder.page;f.folder.page=0;live_folder();f.replaying=true;}}
        load_page();reset_selection(s,0);return true;
    }
    if(f.folder.page)--row;
    if(row==f.count&&f.more){++f.folder.page;if(!f.folder.indexed){f.folder.position=f.next_page;f.checkpoints[f.folder.page%16u]=f.next_page;f.checkpoint_cluster[f.folder.page%16u]=f.folder.file.cluster;f.checkpoint_page[f.folder.page%16u]=f.folder.page;}load_page();reset_selection(s,0);return true;}
    if(row>=f.count)return true;
    choose_row(s,row,(pressed&8u)!=0);return true;
}
