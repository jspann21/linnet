#include "boot/include/linnet_bytes.h"
#include "photos.h"
#include "pimg.h"
#include "jpeg.h"
#include "services/storage/storage.h"
#include "services/storage/block.h"
#include "services/music/player.h"
#include "hal/api/audio.h"
#include "hal/api/ata.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_boot.h"
#define GRID_PAGE (LINNET_TARGET_NUMBER==1238u?15u:LINNET_TARGET_NUMBER==25u?30u:25u)
#define ROWS (2u*GRID_PAGE)
#define THUMB_WIDTH (LINNET_TARGET_NUMBER==1238u?56u:LINNET_TARGET_NUMBER==25u?50u:40u)
#define THUMB_HEIGHT (LINNET_TARGET_NUMBER==1238u?56u:LINNET_TARGET_NUMBER==25u?38u:30u)
_Static_assert(ROWS*THUMB_WIDTH*THUMB_HEIGHT*2u<=262144u,"native grid cache exceeds photo arena");
#define WIDTH LINNET_PHOTO_WIDTH
#define HEIGHT LINNET_PHOTO_HEIGHT
/* Panning can straddle one extra tile on each axis. Keep a power-of-two
 * cache for the hint hash, large enough to retain the entire native viewport. */
#define VISIBLE_TILES (((WIDTH+62u)/32u)*((HEIGHT+62u)/32u))
#define GRID_TILES (VISIBLE_TILES<=64u?64u:VISIBLE_TILES<=128u?128u:256u)
/* In the viewer, thumbnail pixels are unused. Reuse their storage to retain
 * both sides of a tile-boundary reversal without growing the photo arena. */
#define TILES (GRID_TILES==64u?128u:GRID_TILES)
_Static_assert(VISIBLE_TILES<=TILES,"photo viewport exceeds tile cache capacity");
#define EMPTY_ALBUM_RESUME 65535u
enum phase {OPEN,HEADER,VERIFY,IDLE,RECORD,TITLE,THUMB,PHOTO_CHECK_LEVELS,PHOTO_CHECK_TILES,LEVEL,TILE_RECORD,PACK_OPEN,TILE_DATA,SOURCE,JPEG_OPEN,JPEG_LOAD,JPEG_DECODE,TILE_COMMIT,FILE_HEADER,FILE_PIXELS,GRID_OPEN,GRID_HEADER,GRID_VERIFY,GRID_THUMB_COMMIT,TXN_GUARD};
struct row {uint32_t index;bool valid,album,thumb,title_ready;char title[192];union {struct ln_pimg_album a;struct ln_pimg_photo p;} data;struct ln_ui_asset asset;};
struct tile {uint32_t index,age;bool valid;};
struct level_cache {uint32_t index,count;bool valid;};
static struct {
    union {uint16_t viewer_tiles[TILES][1024],thumbs[ROWS][THUMB_WIDTH*THUMB_HEIGHT];
        uint8_t jpeg[262144];uint32_t photo_info[65535];} pixels;
    union {uint16_t frame[2][WIDTH*HEIGHT];struct {uint32_t album_counts[4096];uint8_t metadata[65536];} verify;} surface;
} photo_buffers __attribute__((section(".media_workspace"),aligned(32)));
/* Row/tile validity and frame publication stay in cleared state. Verification
 * writes its album and photo tables in full before consuming them. */
static struct {
    struct ln_photo_jpeg jpeg;
    struct ln_pimg_header header;struct ln_pgrid_header grid;struct ln_fat32_file file,jpeg_file,grid_file;
#if LINNET_TARGET_NUMBER!=98u
    struct ln_fat32_file pack_file;uint32_t v_pack_at[LN_PIMG_PACK_COUNT],pack_bucket;bool pack_ready;char pack_path[96];
#endif
    struct row rows[ROWS];struct tile tiles[TILES];struct ln_pimg_photo photo;struct ln_pimg_level levels[6];
    struct level_cache level_cache[2];
    struct {uint32_t index;bool valid;} frame_cache[2];
    struct ln_ui_asset asset;
    uint32_t verify_cached,verify_cursor,grid_at;bool metadata_valid,pim_verified,grid_cache;
    uint8_t metadata[131072],pim_header[512],resume[16],tile_records[512],tile_hints[TILES];char title[192],album_title[192],source[128];
    uint32_t phase,view,album,first,count,photo_index,window_first,window_count,epoch,working,generation,revision,navigation;
    int32_t prefetch_direction;
    uint32_t io_bytes,verify_at,verify_crc,v_payload_at,photo_check_at,photo_check_end,photo_check_crc,job_index,job_slot,level_count,level,zoom,rotation,tile_index,tile_slot,tile_crc,age;
    uint32_t tile_records_first,tile_records_count,tile_group_count,tile_group_cursor,tile_group_at,tile_group_bytes,thumb_group_count,thumb_group_cursor;
    uint32_t visible[TILES],visible_count,front,render_y,slide_at,jpeg_at,ready_tiles,resume_stage,file_offset,file_payload_at;
    uint8_t verify_record[64];uint32_t v_photos,v_levels,v_tiles,v_album,v_photo,v_level,v_width,v_height,v_text_bytes,v_more,v_code,v_min,jpeg_crc;
    int32_t pan_x,pan_y;
    bool active,ready,pending,io_open,job_album,view_loaded,frame_valid,frame_complete,rendering,redraw,slide,slide_armed,resume_pending,jpeg_mode,stepping,album_known,direct,grid_available,prepared,prefetching,prefetch_job;
    int error;
} p;
/* Transient reads are fully filled before use; retain cache capacity while
 * using the spare tail of the artwork bank on targets that reserve it. */
static uint8_t photo_scratch[32768]
#if defined(LINNET_COVERFLOW)
    __attribute__((section(".cover_high"),aligned(32)))
#endif
;
/* Maps and cached descriptions are completely filled before their cleared
 * control tags permit reads. Use the artwork bank's spare tail on color iPods. */
static struct {int16_t x[WIDTH],y[HEIGHT];struct ln_pimg_level levels[2][6];} photo_maps
#if defined(LINNET_COVERFLOW)
    __attribute__((section(".cover_high"),aligned(32)))
#endif
;
static bool file_mode;
static bool background_allowed(void){
    struct ln_player_status player;ln_player_snapshot(&player);
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    return (!audio.playing||audio.paused)&&(player.phase==LN_PLAYER_IDLE||player.phase==LN_PLAYER_ERROR||player.phase==LN_PLAYER_PAUSE);
}
static bool background_disk_awake(void){
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    return disk.powered&&disk.initialized&&!disk.sleeping&&!disk.failed;
}
static bool background_disk_prefetch_ready(void){
    struct ln_ata_state disk;ln_ata_state_snapshot(&disk);
    if(!disk.powered||!disk.initialized||disk.initializing||disk.busy||disk.sleeping||disk.failed)return false;
    struct ln_block_info block;ln_block_snapshot(&block);
    return !block.power_transition;
}
bool ln_photos_file_mode(void){return file_mode;}

#if !defined(LINNET_PROFILE_RELEASE)
static struct {uint32_t view_started,frame_ready,io_started,max_open_us,max_read_us,render_us,jpeg_us;} timing;
#define PHOTO_DIAG(code) do {code;} while(0)
#define PHOTO_START() uint32_t started=ln_time_us()
#else
#define PHOTO_DIAG(code) do {} while(0)
#define PHOTO_START() do {} while(0)
#endif
static const char package[]="/.LINNET/PHOTOS/LIBRARY.PIM";
static const char grid_package[]="/.LINNET/PHOTOS/GRID.PGR";
static const char transaction_marker[]="/.LINNET/TXN/PHOTOS/STATE.TXN";
#define PHOTO_RECOVERY_REQUIRED 25
static void clear(void*v,uint32_t n){
#if defined(__arm__)
    extern void ln_memory_clear(void *,uint32_t);ln_memory_clear(v,n);
#else
    uint8_t*b=v;while(n--)*b++=0;
#endif
}
static void copy(void*d,const void*s,uint32_t n){
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);ln_memory_copy(d,s,n);
#else
    uint8_t*a=d;const uint8_t*b=s;while(n--)*a++=*b++;
#endif
}
#if LINNET_TARGET_NUMBER!=98u
static void pack_name(uint32_t bucket){
    static const char hex[]="0123456789ABCDEF",prefix[]="/.LINNET/PHOTOS/PACKS/";
    uint32_t n=sizeof prefix-1u;copy(p.pack_path,prefix,n);
    const uint8_t*id=p.header.packs+bucket*12u;
    for(uint32_t i=0;i<8u;++i){if(i==4u)p.pack_path[n++]='/';
        p.pack_path[n++]=hex[id[i]>>4u];p.pack_path[n++]=hex[id[i]&15u];}
    p.pack_path[n++]='.';p.pack_path[n++]='P';p.pack_path[n++]='A';p.pack_path[n++]='K';p.pack_path[n]=0;
}
static bool pack_coverage(void){
    for(uint32_t i=0;i<LN_PIMG_PACK_COUNT;++i){
        if(p.v_pack_at[i]!=ln_read_le32(p.header.packs+i*12u+8u))return false;
    }
    return true;
}
#else
static bool pack_coverage(void){return p.v_payload_at==p.header.size;}
#endif
static void text(char*d,const char*s){uint32_t i=0;while(i<191&&s[i]){d[i]=s[i];++i;}d[i]=0;}
static uint32_t next_generation(void){if(++p.generation>=0x70000000u)p.generation=0x60000001u;return p.generation;}
static bool allowed(void){
    struct ln_player_status player;struct ln_audio_status audio;ln_player_snapshot(&player);ln_audio_snapshot(&audio);
    if(player.phase==LN_PLAYER_IDLE||player.phase==LN_PLAYER_ERROR||player.phase==LN_PLAYER_PAUSE)return true;
    uint32_t reserve=ln_player_background_frames(audio.rate);
    /* Verified LPIC record/thumbnail/tile requests are capped at 8 KiB.
     * Keep 500 ms (and the existing 8192-frame floor) for those bounded reads;
     * catalog opening/verification and JPEG retain the larger reserve. */
    if(p.ready&&!p.jpeg_mode&&p.phase>=RECORD&&p.phase<=TILE_DATA){
        uint32_t prepared=audio.rate/2u;if(prepared<8192u)prepared=8192u;
        if(prepared<reserve)reserve=prepared;
    }
    return audio.playing&&!audio.paused&&audio.queued>=reserve&&!ln_player_prefetch_urgent();
}
static uint32_t metadata_end(void){
    if(p.grid_cache)return p.header.level_at;
    uint32_t strings=p.header.data_at-p.header.string_at;
    if(p.header.tile_at-512u+strings<=sizeof p.metadata)return p.header.tile_at;
    /* Large catalogs retain grid records and titles; viewer levels are read
     * separately rather than disabling the entire metadata cache. */
    return p.header.level_at-512u+strings<=sizeof p.metadata?p.header.level_at:0;
}
static bool cached_metadata_work(void){
    return ((p.phase==VERIFY||p.phase==GRID_VERIFY)&&p.verify_cached)||
        (p.phase==LEVEL&&p.level_cache[p.photo_index&1u].valid&&
         p.level_cache[p.photo_index&1u].index==p.photo_index&&p.level_cache[p.photo_index&1u].count==p.photo.levels)||
        (p.metadata_valid&&(p.phase==RECORD||p.phase==TITLE||p.phase==SOURCE||
         (p.phase==LEVEL&&metadata_end()==p.header.tile_at)));
}
/* Storage admission belongs to read_at/open_file. These bounded steps only
 * consume validated memory; waiting for disk runway here stalls each render
 * slice and even the publication of a frame whose tiles are already loaded. */
static bool memory_work(void){
    return cached_metadata_work()||p.phase==TILE_COMMIT||p.phase==GRID_THUMB_COMMIT||
        (p.phase==VERIFY&&p.verify_at==p.header.data_at)||
        (p.phase==GRID_VERIFY&&p.grid_at==p.grid.data_at-1024u)||
        (p.phase==TILE_RECORD&&p.tile_records_count&&p.tile_index>=p.tile_records_first&&
         p.tile_index-p.tile_records_first<p.tile_records_count)||
        (p.phase==IDLE&&(p.resume_pending||
         (p.view==LN_PHOTOS_VIEWER&&p.view_loaded&&!p.jpeg_mode&&
          (p.rendering||p.redraw||!p.frame_complete))));
}
static void change(void){++p.epoch;++p.revision;}
static void fail(int error){p.error=error;p.phase=IDLE;p.rendering=false;p.slide=p.prefetching=p.prefetch_job=false;++p.revision;}
static int poll_file(struct ln_fat32_file *file,uint32_t *bytes){
    /* Drain a small burst of ready FAT/cache transitions before another full
     * decoder turn. Real hardware waits still return to the executive. */
    for(uint32_t step=0;step<4u;++step){
        int r=ln_storage_media_poll_deferred(p.generation,file,bytes);if(r)return r;
        uint32_t now=ln_time_us(),deadline;
        if(ln_storage_idle_wait(now,&deadline)&&(int32_t)(deadline-now)>0)return 0;
    }
    return 0;
}
/* Admission and completion remain in this phase until the exact request drains.
 * Bounded reads return to the executive; avoid recursive runtime work here. */
static int read_at(const struct ln_fat32_file*f,uint32_t at,void*buffer,uint32_t n){
    /* Retain verified catalog bytes across viewer and grid transitions. */
    if(!p.pending&&p.metadata_valid&&f==&p.file){
        uint32_t end=metadata_end();
        if(at>=512u&&at<=end&&n<=end-at){
            copy(buffer,p.metadata+at-512u,n);return 1;
        }
        if(at>=p.header.string_at&&at<=p.header.data_at&&n<=p.header.data_at-at){
            copy(buffer,p.metadata+end-512u+at-p.header.string_at,n);return 1;
        }
    }
    if(!p.pending){if(!allowed())return 0;int r=ln_storage_media_read(f,at,buffer,n,next_generation());if(r<=0)return r;PHOTO_DIAG(timing.io_started=ln_time_us());p.pending=true;p.io_open=false;p.io_bytes=n;return 0;}
    uint32_t bytes=0;int r=poll_file(0,&bytes);if(!r)return 0;p.pending=false;
    PHOTO_DIAG(uint32_t elapsed=ln_time_us()-timing.io_started;if(elapsed>timing.max_read_us)timing.max_read_us=elapsed);
    if(p.working!=p.epoch)return 0;return r==1&&bytes==p.io_bytes?1:-1;
}
static int tile_record(void){
    if(p.tile_index>=p.header.tiles)return -1;
    if(!p.tile_records_count||p.tile_index<p.tile_records_first||p.tile_index-p.tile_records_first>=p.tile_records_count){
        uint32_t first=p.tile_index/42u*42u,count=p.header.tiles-first;if(count>42u)count=42u;
        /* A partial/canceled read must never publish a cache page. */
        p.tile_records_count=0;
        int r=read_at(&p.file,p.header.tile_at+first*12u,p.tile_records,count*12u);if(r<=0)return r;
        p.tile_records_first=first;p.tile_records_count=count;
    }
    uint32_t at,length;return ln_pimg_tile(&p.header,p.tile_records+(p.tile_index-p.tile_records_first)*12u,p.tile_index,&at,&length,&p.tile_crc)?1:-1;
}
static int open_file(const char*path,struct ln_fat32_file*f){
    if(!p.pending){if(!allowed())return 0;int r=ln_storage_media_open(path,next_generation());if(r<=0)return r;PHOTO_DIAG(timing.io_started=ln_time_us());p.pending=p.io_open=true;return 0;}
    int r=poll_file(f,0);if(r)p.pending=false;
    if(r){PHOTO_DIAG(uint32_t elapsed=ln_time_us()-timing.io_started;if(elapsed>timing.max_open_us)timing.max_open_us=elapsed);}
    if(p.working!=p.epoch)return 0;return r;
}
static struct row *cached(uint32_t index,bool album){struct row*r=&p.rows[index%ROWS];return r->valid&&r->index==index&&r->album==album?r:0;}
static void begin_row(uint32_t index,bool album){p.job_index=index;p.job_slot=index%ROWS;p.job_album=album;p.rows[p.job_slot].valid=p.rows[p.job_slot].thumb=p.rows[p.job_slot].title_ready=false;p.phase=RECORD;}
static bool path_safe(const uint8_t*s,uint32_t n){
    if(!ln_pimg_text(s,n,true)||n<5||s[n-4]!='.'||s[n-3]!='J'||s[n-2]!='P'||s[n-1]!='G')return false;
    uint32_t component=0,dots=0,extension=0;
    for(uint32_t i=1;i<=n;i++){uint32_t c=s[i];if(!c||c=='/'){if(!component||component>12||dots>1||(dots&&(!extension||extension>3)))return false;component=dots=extension=0;}
        else {++component;if(c=='.'){if(component==1||component>9)return false;++dots;}else {if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;if(dots)++extension;else if(component>8)return false;}}}
    return true;
}
static int string_read(uint32_t at,uint32_t bytes,bool path){
    uint32_t skip=at?1u:0u;int r=read_at(&p.file,p.header.string_at+at-skip,photo_scratch,bytes+skip+1u);if(r!=1)return r;
    if((skip&&photo_scratch[0])||!ln_pimg_text(photo_scratch+skip,bytes,false)||(path&&!path_safe(photo_scratch+skip,bytes)))return -2;
    if(skip)for(uint32_t i=0;i<=bytes;i++)photo_scratch[i]=photo_scratch[i+1];return 1;
}
static void verify_begin(bool grid){
    p.metadata_valid=p.grid_cache=false;p.tile_records_count=0;p.verify_cached=p.verify_cursor=p.grid_at=0;
    p.verify_at=512u;p.verify_crc=UINT32_MAX;p.v_payload_at=p.header.data_at+p.header.photos*8192u;
#if LINNET_TARGET_NUMBER!=98u
    clear(p.v_pack_at,sizeof p.v_pack_at);p.pack_ready=false;
#endif
    p.v_photos=p.v_levels=p.v_tiles=p.v_album=p.v_photo=p.v_level=p.v_width=p.v_height=p.v_text_bytes=p.v_more=p.v_code=p.v_min=0;
    /* Verification uses the thumbnail/tile arena for bounded photo dimensions.
     * Canceled verification must regenerate pixels and the compact cache. */
    for(uint32_t i=0;i<ROWS;++i)p.rows[i].valid=p.rows[i].thumb=p.rows[i].title_ready=false;
    for(uint32_t i=0;i<TILES;++i)p.tiles[i].valid=false;
    for(uint32_t i=0;i<2u;++i)p.level_cache[i].valid=false;
    for(uint32_t i=0;i<2u;++i)p.frame_cache[i].valid=false;
    p.phase=grid?GRID_VERIFY:VERIFY;
}
static void grid_fallback(void){
    p.grid_available=p.prefetch_job=false;
    for(uint32_t i=0;i<ROWS;++i)p.rows[i].thumb=false;
    if(!p.active){p.prefetching=p.ready=p.metadata_valid=false;p.phase=OPEN;return;}
    if(!p.pim_verified)verify_begin(false);else p.phase=IDLE;++p.revision;
}
static bool verify_bytes(uint32_t at,const uint8_t*b,uint32_t n){
    const struct ln_pimg_header*h=&p.header;
    /* Tile descriptors carry no per-record CRC; once a read contains only
     * aligned complete descriptors, validate them in place. The caller still
     * performs the same metadata CRC over every byte, while split/partial
     * records retain the existing scratch-buffer path. */
    if(at>=h->tile_at&&at<=h->string_at&&n<=h->string_at-at&&
       (at-h->tile_at)%12u==0&&n%12u==0){
        uint32_t index=(at-h->tile_at)/12u;
        if(index>h->tiles||n/12u>h->tiles-index)return false;
        for(uint32_t i=0;i<n;i+=12u){uint32_t offset,length,crc;
            if(!ln_pimg_tile(h,b+i,index+i/12u,&offset,&length,&crc))return false;
#if LINNET_TARGET_NUMBER==98u
            if(offset!=p.v_payload_at)return false;p.v_payload_at+=length;
#else
            uint32_t bucket=offset>>27u,local=offset&(LN_PIMG_PACK_LIMIT-1u);
            if(local!=p.v_pack_at[bucket])return false;p.v_pack_at[bucket]+=length;
#endif
        }
        return true;
    }
    for(uint32_t i=0;i<n;i++,at++){
        if(at<h->string_at){uint32_t base,width;
            if(at<h->photo_at){base=h->album_at;width=32;}else if(at<h->level_at){base=h->photo_at;width=64;}else if(at<h->tile_at){base=h->level_at;width=24;}else{base=h->tile_at;width=12;}
            /* Metadata chunks can split a record. Compute its position once
             * per fragment, keeping the same record validation at its end. */
            uint32_t offset=(at-base)%width,index=(at-base)/width,take=width-offset;
            if(take>n-i)take=n-i;
            for(uint32_t k=0;k<take;k++)p.verify_record[offset+k]=b[i+k];
            i+=take-1u;at+=take-1u;if(offset+take!=width)continue;
            if(width==32){struct ln_pimg_album a;if(!ln_pimg_album(h,p.verify_record,index,&a)||a.first!=p.v_photos)return false;p.v_photos+=a.count;photo_buffers.surface.verify.album_counts[index]=a.count;}
            else if(width==64){struct ln_pimg_photo photo;if(!ln_pimg_photo(h,p.verify_record,index,&photo)||photo.first!=p.v_levels)return false;
                while(p.v_album<h->albums&&!photo_buffers.surface.verify.album_counts[p.v_album])++p.v_album;
                if(p.v_album>=h->albums||photo.album!=p.v_album)return false;--photo_buffers.surface.verify.album_counts[p.v_album];p.v_levels+=photo.levels;
                photo_buffers.pixels.photo_info[index]=(photo.width-1u)|((photo.height-1u)<<14)|((photo.levels-1u)<<28);}
            else if(width==24){struct ln_pimg_level l;if(!ln_pimg_level(h,p.verify_record,index,&l)||l.photo!=p.v_photo||l.first!=p.v_tiles||p.v_photo>=h->photos)return false;
                uint32_t info=photo_buffers.pixels.photo_info[p.v_photo],w=(info&16383u)+1u,height=((info>>14)&16383u)+1u,levels=(info>>28)+1u;
                if(l.width>w||l.height>height||(!p.v_level&&(l.width>WIDTH||l.height>HEIGHT))||(p.v_level&&(l.width<p.v_width||l.height<p.v_height||(l.width==p.v_width&&l.height==p.v_height))))return false;
                p.v_width=l.width;p.v_height=l.height;p.v_tiles+=l.count;if(++p.v_level==levels){++p.v_photo;p.v_level=0;}}
            else {uint32_t offset,length,crc;if(!ln_pimg_tile(h,p.verify_record,index,&offset,&length,&crc))return false;
#if LINNET_TARGET_NUMBER==98u
                if(offset!=p.v_payload_at)return false;p.v_payload_at+=length;
#else
                uint32_t bucket=offset>>27u,local=offset&(LN_PIMG_PACK_LIMIT-1u);
                if(local!=p.v_pack_at[bucket])return false;p.v_pack_at[bucket]+=length;
#endif
            }
        }else if(at<h->string_at+h->string_bytes){uint32_t c=b[i];
            if(at==h->string_at&&c)return false;
            if(!c){if(p.v_more)return false;p.v_text_bytes=0;continue;}
            if(++p.v_text_bytes>191||c<32||c==127)return false;
            if(p.v_more){if((c&0xc0u)!=0x80u)return false;p.v_code=(p.v_code<<6)|(c&63u);if(!--p.v_more&&(p.v_code<p.v_min||p.v_code>0x10ffffu||(p.v_code>=0xd800u&&p.v_code<=0xdfffu)))return false;}
            else if(c>=128){if(c>=0xc2&&c<=0xdf){p.v_more=1;p.v_code=c&31u;p.v_min=0x80;}else if(c>=0xe0&&c<=0xef){p.v_more=2;p.v_code=c&15u;p.v_min=0x800;}else if(c>=0xf0&&c<=0xf4){p.v_more=3;p.v_code=c&7u;p.v_min=0x10000;}else return false;}
        }else if(b[i])return false;
    }return true;
}
static int tile_find(uint32_t index){
    /* A hint is never proof: eviction, JPEG reuse and hash collisions all
     * fall back to the authoritative valid/index pair before touching pixels. */
    uint32_t bucket=index&(TILES-1u),slot=p.tile_hints[bucket];
    if(p.tiles[slot].valid&&p.tiles[slot].index==index)return (int)slot;
    for(uint32_t i=0;i<TILES;i++)if(p.tiles[i].valid&&p.tiles[i].index==index){p.tile_hints[bucket]=(uint8_t)i;return (int)i;}
    return -1;
}
static bool is_visible(uint32_t index){for(uint32_t i=0;i<p.visible_count;i++)if(p.visible[i]==index)return true;return false;}
static uint32_t victim(void){uint32_t best=UINT32_MAX,age=UINT32_MAX;for(uint32_t i=0;i<TILES;i++){if(!p.tiles[i].valid)return i;if(!is_visible(p.tiles[i].index)&&p.tiles[i].age<age){best=i;age=p.tiles[i].age;}}return best;}
static void source_xy(int32_t rx,int32_t ry,int32_t*sx,int32_t*sy){
    const struct ln_pimg_level*l=&p.levels[p.level];
    switch(p.rotation&3u){case 0:*sx=rx;*sy=ry;break;case 1:*sx=ry;*sy=(int32_t)l->height-1-rx;break;
    case 2:*sx=(int32_t)l->width-1-rx;*sy=(int32_t)l->height-1-ry;break;default:*sx=(int32_t)l->width-1-ry;*sy=rx;break;}
}
static void layout(void){
    if(!p.view_loaded||p.jpeg_mode)return;p.level=p.zoom<p.photo.levels?p.zoom:p.photo.levels-1u;
    const struct ln_pimg_level*l=&p.levels[p.level];uint32_t w=(p.rotation&1u)?l->height:l->width,h=(p.rotation&1u)?l->width:l->height;
    uint32_t dw=w,dh=h;if(!p.zoom){if(w*HEIGHT>h*WIDTH){dw=WIDTH;dh=h*WIDTH/w;}else{dh=HEIGHT;dw=w*HEIGHT/h;}if(!dw)dw=1;if(!dh)dh=1;}
    p.direct=!p.rotation&&(p.zoom||(dw==w&&dh==h));
    if(p.pan_x>(int32_t)w/2)p.pan_x=(int32_t)w/2;if(p.pan_x<-(int32_t)w/2)p.pan_x=-(int32_t)w/2;
    if(p.pan_y>(int32_t)h/2)p.pan_y=(int32_t)h/2;if(p.pan_y<-(int32_t)h/2)p.pan_y=-(int32_t)h/2;
    for(uint32_t x=0;x<WIDTH;x++){int32_t v=(int32_t)x-((int32_t)WIDTH-(int32_t)dw)/2;photo_maps.x[x]=v<0||v>=(int32_t)dw?-1:(int16_t)((uint32_t)v*w/dw);if(p.zoom)photo_maps.x[x]=(int16_t)((int32_t)x-(int32_t)WIDTH/2+(int32_t)w/2+p.pan_x);}
    for(uint32_t y=0;y<HEIGHT;y++){int32_t v=(int32_t)y-((int32_t)HEIGHT-(int32_t)dh)/2;photo_maps.y[y]=v<0||v>=(int32_t)dh?-1:(int16_t)((uint32_t)v*h/dh);if(p.zoom)photo_maps.y[y]=(int16_t)((int32_t)y-(int32_t)HEIGHT/2+(int32_t)h/2+p.pan_y);}
    p.visible_count=0;
    /* Source ranges are separable under quarter-turns; inspect boundaries at
     * tile crossings through the small display grid, never the whole image. */
    int32_t minx=2048,miny=2048,maxx=-1,maxy=-1;
    for(uint32_t x=0;x<WIDTH;x++)for(uint32_t y=0;y<HEIGHT;y+=HEIGHT-1u){int32_t sx,sy;source_xy(photo_maps.x[x],photo_maps.y[y],&sx,&sy);if(sx>=0&&sx<(int32_t)l->width){if(sx<minx)minx=sx;if(sx>maxx)maxx=sx;}}
    for(uint32_t y=0;y<HEIGHT;y++)for(uint32_t x=0;x<WIDTH;x+=WIDTH-1u){int32_t sx,sy;source_xy(photo_maps.x[x],photo_maps.y[y],&sx,&sy);if(sy>=0&&sy<(int32_t)l->height){if(sy<miny)miny=sy;if(sy>maxy)maxy=sy;}}
    /* Rotated axes may have margins at both sampled edges. Compute extrema
     * using each independent coordinate as well. */
    if(p.rotation&1u){minx=miny=2048;maxx=maxy=-1;
        for(uint32_t y=0;y<HEIGHT;y++){int32_t sx,sy;source_xy(0,photo_maps.y[y],&sx,&sy);if(sx>=0&&sx<(int32_t)l->width){if(sx<minx)minx=sx;if(sx>maxx)maxx=sx;}}
        for(uint32_t x=0;x<WIDTH;x++){int32_t sx,sy;source_xy(photo_maps.x[x],0,&sx,&sy);if(sy>=0&&sy<(int32_t)l->height){if(sy<miny)miny=sy;if(sy>maxy)maxy=sy;}}}
    if(maxx>=minx&&maxy>=miny)for(uint32_t y=(uint32_t)miny/32u;y<=(uint32_t)maxy/32u;y++)for(uint32_t x=(uint32_t)minx/32u;x<=(uint32_t)maxx/32u;x++){
        if(p.visible_count==TILES){fail(20);return;}p.visible[p.visible_count++]=l->first+y*l->columns+x;}
    p.redraw=false;p.rendering=false;p.frame_complete=false;p.ready_tiles=0;
}
static void slide_ready(void){
    if(p.slide&&!p.slide_armed){p.slide_at=ln_time_us()+5000000u;p.slide_armed=true;}
}
static void render_step(void){
    PHOTO_START();
    if(!p.rendering){p.frame_cache[p.front^1u].valid=false;p.rendering=true;p.render_y=0;p.redraw=false;}
    const struct ln_pimg_level*l=&p.levels[p.level];uint16_t*out=photo_buffers.surface.frame[p.front^1u];
    uint32_t end=p.render_y+8u;if(end>HEIGHT)end=HEIGHT;
    for(uint32_t y=p.render_y;y<end;y++){
        if(p.direct){
            uint16_t*row=out+y*WIDTH;int32_t sy=photo_maps.y[y];
            /* A complete source row overwrites every destination pixel. The
             * frame is rendered only after all visible tiles are cached. */
            bool covered=p.frame_complete&&sy>=0&&sy<(int32_t)l->height&&
                photo_maps.x[0]>=0&&photo_maps.x[WIDTH-1]<(int32_t)l->width;
            if(!covered)clear(row,WIDTH*sizeof *row);
            if(sy<0||sy>=(int32_t)l->height)continue;
            uint32_t x=0;while(x<WIDTH&&photo_maps.x[x]<0)++x;
            while(x<WIDTH){int32_t sx=photo_maps.x[x];if(sx<0||sx>=(int32_t)l->width)break;
                uint32_t count=32u-((uint32_t)sx&31u);
                if(count>WIDTH-x)count=WIDTH-x;if(count>l->width-(uint32_t)sx)count=l->width-(uint32_t)sx;
                uint32_t tile=l->first+(uint32_t)sy/32u*l->columns+(uint32_t)sx/32u;
                int slot=tile_find(tile);
                if(slot>=0)copy(row+x,photo_buffers.pixels.viewer_tiles[slot]+((uint32_t)sy&31u)*32u+((uint32_t)sx&31u),count*sizeof *row);
                x+=count;
            }
            continue;
        }
        uint32_t last=UINT32_MAX;int slot=-1;
        /* Quarter-turn transforms are affine in x. Hoist the transform and
         * level geometry out of the pixel loop without changing scaled maps. */
        int32_t origin_x,origin_y,step_x,step_y;
        source_xy(0,photo_maps.y[y],&origin_x,&origin_y);source_xy(1,photo_maps.y[y],&step_x,&step_y);
        step_x-=origin_x;step_y-=origin_y;
        for(uint32_t x=0;x<WIDTH;x++){int32_t sx=origin_x+photo_maps.x[x]*step_x,sy=origin_y+photo_maps.x[x]*step_y;uint16_t color=0;
            if(photo_maps.x[x]>=0&&photo_maps.y[y]>=0&&sx>=0&&sy>=0&&sx<(int32_t)l->width&&sy<(int32_t)l->height){uint32_t tile=l->first+(uint32_t)sy/32u*l->columns+(uint32_t)sx/32u;
                if(tile!=last){last=tile;slot=tile_find(tile);}if(slot>=0)color=photo_buffers.pixels.viewer_tiles[slot][((uint32_t)sy&31u)*32u+((uint32_t)sx&31u)];}
            out[y*WIDTH+x]=color;}}
    p.render_y=end;if(end==HEIGHT){p.front^=1u;p.asset.pixels=photo_buffers.surface.frame[p.front];p.frame_valid=true;p.rendering=false;
        p.frame_cache[p.front].index=p.photo_index;p.frame_cache[p.front].valid=p.frame_complete&&!p.zoom&&!p.rotation&&!p.pan_x&&!p.pan_y;
        PHOTO_DIAG(timing.frame_ready=ln_time_us());slide_ready();++p.revision;}
    PHOTO_DIAG(timing.render_us+=ln_time_us()-started);
}
static void begin_view(uint32_t index){PHOTO_DIAG(timing.view_started=ln_time_us();timing.frame_ready=timing.render_us=timing.jpeg_us=0);p.slide_armed=false;p.photo_index=index;p.view=LN_PHOTOS_VIEWER;p.view_loaded=p.frame_valid=p.frame_complete=p.jpeg_mode=p.rendering=false;p.error=0;p.zoom=p.rotation=0;p.pan_x=p.pan_y=0;p.level_count=0;for(uint32_t i=0;i<ROWS;i++)p.rows[i].thumb=false;change();++p.navigation;}
void ln_photos_init(void){
    /* The runtime calls this once after startup has cleared BSS. Clearing
     * the control state again would duplicate that startup work. */
    p.generation=0x60000000u;p.phase=TXN_GUARD;p.view=LN_PHOTOS_LIST;p.prefetch_direction=1;
    p.window_count=ROWS;p.asset=(struct ln_ui_asset){WIDTH,HEIGHT,photo_buffers.surface.frame[0]};
}
void ln_photos_prepare(bool eligible){
    if(!eligible){if(p.prefetching){p.prefetching=false;change();}return;}
    if(p.prepared)return;p.prepared=true;
    if(!p.active&&!file_mode&&!p.ready&&!p.error)p.prefetching=true;
}
void ln_photos_open(void){if(!p.active){bool joining=p.prefetching;p.prefetching=false;if(file_mode){file_mode=false;p.ready=p.jpeg_mode=p.frame_valid=p.view_loaded=p.redraw=false;
    p.view=LN_PHOTOS_LIST;p.window_first=p.photo_index=0;}p.active=true;p.error=0;if(joining)++p.revision;else change();++p.navigation;}}
static bool begin_file(const struct ln_fat32_file*file,uint32_t offset,uint32_t bytes,const char*title){
    if(p.pending||!file||!bytes||bytes>sizeof photo_buffers.pixels.jpeg||(file->attributes&24u)||offset>file->size||bytes>file->size-offset)return false;
    p.prefetching=p.prefetch_job=false;p.ready=p.metadata_valid=false;p.active=p.jpeg_mode=file_mode=true;
    for(uint32_t i=0;i<2u;++i)p.frame_cache[i].valid=false;
    p.view=LN_PHOTOS_VIEWER;p.frame_valid=p.view_loaded=p.rendering=p.redraw=p.slide=p.slide_armed=p.resume_pending=false;
    p.error=0;p.count=0;p.jpeg_file=*file;p.file_offset=offset;p.photo.jpeg_bytes=bytes;p.photo.orientation=1;
    text(p.title,title);p.jpeg_at=0;p.jpeg_crc=UINT32_MAX;change();p.working=p.epoch;p.phase=JPEG_LOAD;
    for(uint32_t i=0;i<ROWS;++i)p.rows[i].valid=p.rows[i].thumb=false;
    for(uint32_t i=0;i<TILES;++i)p.tiles[i].valid=false;
    return true;
}
bool ln_photos_open_file(const struct ln_fat32_file*file,const char*title){return file&&begin_file(file,0,file->size,title);}
void ln_photos_close(void){p.active=p.prefetching=false;p.slide=false;change();}
bool ln_photos_open_prepared(const struct ln_fat32_file*file,uint32_t offset,uint32_t bytes,const char*title){
#if LINNET_TARGET_NUMBER==98u
    if(bytes!=512u+WIDTH*HEIGHT*2u||!begin_file(file,offset,bytes,title))return false;
#else
    if(bytes<512u||!begin_file(file,offset,bytes,title))return false;
#endif
    p.phase=FILE_HEADER;return true;
}
bool ln_photos_active(void){return p.active;}
bool ln_photos_pending(void){return p.pending;}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_photos_diagnostics(uint32_t out[32]){
    const uint32_t values[32]={0x544f4850u,0,p.phase,(uint32_t)p.error,p.active,p.ready,p.pending,p.view,
        p.file.size,p.verify_at,p.header.data_at,p.verify_crc,p.header.metadata_crc,p.v_photos,p.v_levels,p.v_tiles,
        p.photo_index,p.visible_count,p.revision,p.navigation,
        p.frame_valid,p.frame_complete,p.slide_armed,p.slide_at,ln_time_us(),timing.view_started,timing.frame_ready,
        timing.io_started,timing.max_open_us,timing.max_read_us,timing.render_us,timing.jpeg_us};
    for(uint32_t i=0;i<32u;++i)out[i]=values[i];
}
#endif
uint32_t ln_photos_view(void){return p.view;}
uint32_t ln_photos_count(void){return !p.ready?1u:p.view==LN_PHOTOS_ALBUMS?p.header.albums:p.view==LN_PHOTOS_LIST?p.count:0u;}
const char *ln_photos_label(uint32_t row){if(!p.ready)return p.error==PHOTO_RECOVERY_REQUIRED?"Recover photos in Media Manager":p.error?"Photos unavailable":"Loading photos...";uint32_t index=p.view==LN_PHOTOS_ALBUMS?row:p.first+row;struct row*r=cached(index,p.view==LN_PHOTOS_ALBUMS);return r?r->title:"Loading...";}
void ln_photos_window(uint32_t first,uint32_t count){
    if(count>ROWS)count=ROWS;
    if(p.window_first==first&&p.window_count==count)return;
    if(first!=p.window_first)p.prefetch_direction=first>p.window_first?1:-1;
    p.window_first=first;p.window_count=count;if(p.ready)change();
}
const struct ln_ui_asset *ln_photos_thumbnail(uint32_t row){if(!p.ready||p.view!=LN_PHOTOS_LIST||row>=p.count||p.jpeg_mode)return 0;struct row*r=cached(p.first+row,false);return r&&r->thumb?&r->asset:0;}
const struct ln_ui_asset *ln_photos_asset(void){return p.view==LN_PHOTOS_VIEWER&&p.frame_valid?&p.asset:0;}
bool ln_photos_select(uint32_t row){
    if(!p.ready||row>=ln_photos_count())return false;
    /* The viewer can fetch its own record. Admit an uncached selection now
     * instead of discarding Select while grid thumbnails are loading. */
    if(p.view==LN_PHOTOS_LIST){p.resume_pending=false;begin_view(p.first+row);return true;}
    struct row*r=cached(row,true);if(!r)return false;
    p.resume_pending=false;
    if(p.view==LN_PHOTOS_ALBUMS){p.album=row;p.first=r->data.a.first;p.count=r->data.a.count;p.photo_index=p.first;text(p.album_title,r->title);p.album_known=true;p.view=LN_PHOTOS_LIST;p.window_first=0;p.error=0;change();++p.navigation;}
    return true;
}
bool ln_photos_back(void){p.resume_pending=false;if(p.view==LN_PHOTOS_VIEWER){p.view=LN_PHOTOS_LIST;p.slide=p.jpeg_mode=false;p.error=0;p.window_first=(p.photo_index-p.first)/GRID_PAGE*GRID_PAGE;change();++p.navigation;return true;}ln_photos_close();return false;}
void ln_photos_next(int direction){if(p.view!=LN_PHOTOS_VIEWER||!p.count)return;uint32_t i=p.photo_index-p.first;if(direction<0)i=i?i-1:p.count-1;else i=i+1==p.count?0:i+1;bool slide=p.slide;begin_view(p.first+i);p.slide=slide;}
bool ln_photos_zoom(int direction){if(!p.view_loaded||p.jpeg_mode)return false;uint32_t z=p.zoom,max=p.photo.levels-1u;
    if(direction>0){uint32_t delta=(uint32_t)direction;z+=delta>max-z?max-z:delta;}
    else if(direction<0)z=direction<-(int)z?0u:(uint32_t)((int)z+direction);
    if(z==p.zoom)return false;p.zoom=z;p.pan_x=p.pan_y=0;change();layout();return true;}
bool ln_photos_pan(int64_t dx,int64_t dy){
    if(!p.view_loaded||!p.zoom||p.jpeg_mode)return false;
    const struct ln_pimg_level*l=&p.levels[p.level];
    int32_t max_x=(int32_t)((p.rotation&1u)?l->height:l->width)/2;
    int32_t max_y=(int32_t)((p.rotation&1u)?l->width:l->height)/2;
    /* Compare remaining travel before adding, even for extreme deltas. */
    int32_t x=dx>max_x-p.pan_x?max_x:dx< -max_x-p.pan_x?-max_x:p.pan_x+(int32_t)dx;
    int32_t y=dy>max_y-p.pan_y?max_y:dy< -max_y-p.pan_y?-max_y:p.pan_y+(int32_t)dy;
    if(x==p.pan_x&&y==p.pan_y)return false;
    p.pan_x=x;p.pan_y=y;change();layout();return true;
}
void ln_photos_rotate(void){if(!p.view_loaded||p.jpeg_mode)return;p.rotation=(p.rotation+1u)&3u;p.pan_x=p.pan_y=0;change();layout();}
void ln_photos_slideshow(bool enabled){p.slide=enabled&&p.view==LN_PHOTOS_VIEWER;p.slide_armed=false;if(p.frame_valid)slide_ready();++p.revision;}
bool ln_photos_slideshow_active(void){return p.slide;}
uint32_t ln_photos_revision(void){return p.revision;}
uint32_t ln_photos_navigation(void){return p.navigation;}
uint32_t ln_photos_selection(void){return p.view==LN_PHOTOS_ALBUMS?(p.album<p.header.albums?p.album:0):p.view==LN_PHOTOS_LIST&&p.photo_index>=p.first&&p.photo_index-p.first<p.count?p.photo_index-p.first:0;}
bool ln_photos_focus(uint32_t row){
    if(!p.ready||p.resume_pending||p.view!=LN_PHOTOS_LIST||row>=p.count||p.photo_index==p.first+row)return false;
    p.photo_index=p.first+row;return true;
}
const char *ln_photos_title(void){return p.view==LN_PHOTOS_ALBUMS?"Photo albums":p.view==LN_PHOTOS_LIST?"Photo Library":p.title;}
const char *ln_photos_status(void){return p.error==PHOTO_RECOVERY_REQUIRED?"Recover photos in Media Manager":p.error?"Image unavailable":file_mode&&!p.frame_valid?"Loading image":!p.ready?"Loading catalog":!p.pim_verified&&!p.metadata_valid&&!file_mode?(p.view==LN_PHOTOS_VIEWER?"Loading image":"Loading catalog"):p.jpeg_mode&&!p.frame_valid?"Decoding JPEG":p.view==LN_PHOTOS_VIEWER&&!p.frame_valid?"Loading image":"";}
void ln_photos_resume_get(uint8_t out[16]){
    if(p.resume_pending){copy(out,p.resume,16);return;}
    bool empty=p.ready&&p.album_known&&p.view!=LN_PHOTOS_VIEWER&&!p.count;
    ln_write_le32(out,p.ready?p.header.identity:0);ln_write_le32(out+4,empty?EMPTY_ALBUM_RESUME:p.photo_index);
    ln_write_le32(out+8,(empty?0:p.zoom|(p.rotation<<8))|(p.view<<16));
    ln_write_le32(out+12,empty?p.album:(uint16_t)p.pan_x|((uint32_t)(uint16_t)p.pan_y<<16));
}
bool ln_photos_resume_set(const uint8_t in[16]){
    uint32_t flags=ln_read_le32(in+8),index=ln_read_le32(in+4);
    if(index>EMPTY_ALBUM_RESUME||(flags&0xffu)>5||((flags>>8)&255u)>3||(flags>>16)>2)return false;
    if(index==EMPTY_ALBUM_RESUME&&((flags&65535u)||(flags>>16)==LN_PHOTOS_VIEWER||ln_read_le32(in+12)>=4096u))return false;
    copy(p.resume,in,16);p.resume_pending=true;p.resume_stage=0;return true;
}
static bool resume_matches(void){
    return ln_read_le32(p.resume)==p.header.identity&&(ln_read_le32(p.resume+4)==EMPTY_ALBUM_RESUME?
        ln_read_le32(p.resume+12)<p.header.albums:ln_read_le32(p.resume+4)<p.header.photos);
}
static void restore_resume(void){
    if(!resume_matches()){p.resume_pending=false;return;}
    uint32_t index=ln_read_le32(p.resume+4),view=ln_read_le32(p.resume+8)>>16;bool empty=index==EMPTY_ALBUM_RESUME;
    p.first=0;p.count=p.header.photos;
    if(view!=LN_PHOTOS_VIEWER||empty){
        p.view=LN_PHOTOS_LIST;p.photo_index=empty?0:index;
        p.window_first=p.photo_index/GRID_PAGE*GRID_PAGE;
        p.resume_pending=false;++p.navigation;++p.revision;return;
    }
    begin_view(index);
}
static bool plan_grid_rows(uint32_t first,uint32_t visible,bool apply){
    uint32_t count=ln_photos_count();
    /* Native previews form a contiguous host-prepared atlas. Load the records
     * first so a read can fill several cells and publish one grid update. */
    if(p.grid_available&&p.view==LN_PHOTOS_LIST){
        for(uint32_t i=0;i<visible&&i<ROWS&&first+i<count;++i){
            uint32_t index=p.first+first+i;
            if(!cached(index,false)){if(apply)begin_row(index,false);return true;}
        }
    }
    for(uint32_t i=0;i<visible&&i<ROWS&&first+i<count;i++){
        uint32_t index=p.view==LN_PHOTOS_ALBUMS?first+i:p.first+first+i;
        struct row*row=cached(index,p.view==LN_PHOTOS_ALBUMS);
        if(!row){if(apply)begin_row(index,p.view==LN_PHOTOS_ALBUMS);return true;}
        if(p.view==LN_PHOTOS_LIST&&!row->thumb){if(apply){
            p.job_index=index;p.job_slot=index%ROWS;p.thumb_group_count=1;p.thumb_group_cursor=0;
            if(p.grid_available){uint32_t bytes=background_allowed()?sizeof photo_scratch:8192u;
                uint32_t maximum=bytes/p.grid.stride;
                while(p.thumb_group_count<maximum&&i+p.thumb_group_count<visible&&i+p.thumb_group_count<ROWS&&
                      first+i+p.thumb_group_count<count){
                    struct row*next=cached(index+p.thumb_group_count,false);if(!next||next->thumb)break;++p.thumb_group_count;
                }
            }
            p.phase=THUMB;
        }return true;}
    }
    return false;
}
/* Inspect at most ROWS cached entries, or one unloaded viewer row. Planning
 * never submits I/O; the selected phase retains the normal admission gate. */
static bool plan_content(bool apply){
    if(file_mode)return false;
    if(!p.pim_verified&&((p.view==LN_PHOTOS_VIEWER&&!p.grid.photo_crc_bytes)||!p.metadata_valid)){
        if(apply)verify_begin(p.view!=LN_PHOTOS_VIEWER&&p.grid_available);return true;
    }
    if(p.view==LN_PHOTOS_VIEWER){
        if(p.view_loaded)return false;
        struct row*row=cached(p.photo_index,false);if(!apply)return true;
        if(!row){begin_row(p.photo_index,false);return true;}
        if(!row->title_ready){p.job_slot=p.photo_index%ROWS;p.phase=TITLE;return true;}
        if(p.photo_index<p.first||p.photo_index-p.first>=p.count){fail(18);return true;}
        p.photo=row->data.p;text(p.title,row->title);p.level_count=0;
        p.phase=!p.pim_verified&&p.grid.photo_crc_bytes?PHOTO_CHECK_LEVELS:LEVEL;return true;
    }
    if(apply)p.prefetch_job=false;
    if(plan_grid_rows(p.window_first,p.window_count,apply))return true;
    /* The grid cache has room for two complete pages. Fill its spare half
     * while storage is awake, after all cells on the visible page are ready. */
    if(p.grid_available&&p.view==LN_PHOTOS_LIST&&p.window_count==GRID_PAGE&&
       p.window_first%GRID_PAGE==0&&background_allowed()&&background_disk_prefetch_ready()){
        uint32_t adjacent=UINT32_MAX;
        if(p.prefetch_direction<0&&p.window_first>=GRID_PAGE)adjacent=p.window_first-GRID_PAGE;
        else if(p.prefetch_direction>0&&p.window_first+GRID_PAGE<p.count)adjacent=p.window_first+GRID_PAGE;
        if(adjacent!=UINT32_MAX){
            if(apply)p.prefetch_job=true;
            bool work=plan_grid_rows(adjacent,GRID_PAGE,apply);
            if(apply&&!work)p.prefetch_job=false;
            return work;
        }
    }
    return false;
}
static bool idle_plan(bool apply){
    if((!p.active&&!p.prefetching)||!p.ready||p.error||p.pending||p.phase!=IDLE||p.working!=p.epoch||
       (p.active&&p.resume_pending&&p.view!=LN_PHOTOS_VIEWER))return false;
    return plan_content(apply);
}
bool ln_photos_busy(void){
    return p.active&&!p.error&&(!p.ready||p.pending||p.working!=p.epoch||
        p.phase!=IDLE||p.resume_pending||plan_content(false)||
        (p.view==LN_PHOTOS_VIEWER&&(!p.frame_valid||p.rendering||p.redraw||
         (!p.jpeg_mode&&!p.frame_complete))));
}
uint32_t ln_photos_deadline(uint32_t now){
    /* Storage owns an admitted read. Its deadline wakes the executive when
     * progress is possible; another photo poll cannot advance the disk. */
    if(p.pending){
        if(p.working!=p.epoch)return now;
        return now+200000u;
    }
    if((!p.active&&!p.prefetching)||p.error)return now+200000u;
    if(!p.active&&(!background_allowed()||!background_disk_awake()))return now+200000u;
    if(idle_plan(false)||memory_work())return now;
    if(!allowed())return now+200000u;
    if(p.phase!=IDLE||p.rendering||p.redraw||
       (p.view==LN_PHOTOS_VIEWER&&!p.jpeg_mode&&!p.frame_complete))return now;
    if(p.slide&&p.slide_armed&&(int32_t)(p.slide_at-now)>0)return p.slide_at;
    return now+200000u;
}
static void image_fail(int error){
    if(p.photo.source_bytes&&!p.jpeg_mode){p.phase=SOURCE;p.rendering=false;p.frame_valid=false;return;}fail(error);
}
static void photo_step(void){
    if(p.working!=p.epoch){
        if(p.pending){ln_storage_media_cancel(p.generation);if(!ln_storage_media_poll(p.generation,0,0))return;p.pending=false;}
        p.working=p.epoch;p.phase=p.ready?IDLE:TXN_GUARD;p.prefetch_job=false;if(p.rendering&&!p.jpeg_mode)p.redraw=true;p.rendering=false;
        if(p.jpeg_mode&&!p.frame_valid){p.jpeg_mode=false;p.view_loaded=false;}
    }
    if(!p.active&&!p.prefetching)return;
    if(!p.active&&!p.pending&&!background_disk_awake()){p.prefetching=false;return;}
    if(!p.active&&!p.pending&&!background_allowed())return;
    /* A neighbor job may wait between planning and admission. Stop it if
     * playback starts or standby closes the disk before its next read. */
    if(p.prefetch_job&&!p.pending&&!memory_work()&&
       (!background_allowed()||!background_disk_prefetch_ready())){
        p.prefetch_job=false;p.phase=IDLE;return;
    }
    if(p.slide&&p.slide_armed&&p.frame_valid&&(int32_t)(ln_time_us()-p.slide_at)>=0){ln_photos_next(1);return;}
    if(idle_plan(true))return;
    if(!p.pending&&!memory_work()&&!allowed())return;
    int r;
#if LINNET_TARGET_NUMBER==98u
    if(p.phase==FILE_HEADER){r=read_at(&p.jpeg_file,p.file_offset,photo_scratch,512u);if(!r)return;
        if(r<0||ln_read_le32(photo_scratch)!=0x474d4946u||ln_read_le32(photo_scratch+4)!=1u||
           ln_read_le32(photo_scratch+8)!=WIDTH||ln_read_le32(photo_scratch+12)!=HEIGHT||
           ln_crc32(photo_scratch,508u)!=ln_read_le32(photo_scratch+508u)){fail(22);return;}
        p.photo.jpeg_crc=ln_read_le32(photo_scratch+16);p.jpeg_at=0;p.jpeg_crc=UINT32_MAX;p.phase=FILE_PIXELS;return;}
    if(p.phase==FILE_PIXELS){uint32_t n=WIDTH*HEIGHT*2u-p.jpeg_at;if(n>8192u)n=8192u;
        if(n){uint8_t*destination=(uint8_t*)photo_buffers.surface.frame[p.front^1u]+p.jpeg_at;
            r=read_at(&p.jpeg_file,p.file_offset+512u+p.jpeg_at,destination,n);if(!r)return;if(r<0){fail(23);return;}
            p.jpeg_crc=ln_pimg_crc_update(p.jpeg_crc,destination,n);p.jpeg_at+=n;return;}
        if((p.jpeg_crc^UINT32_MAX)!=p.photo.jpeg_crc){fail(24);return;}
        p.front^=1u;p.asset.pixels=photo_buffers.surface.frame[p.front];p.frame_valid=p.view_loaded=p.ready=true;p.phase=IDLE;++p.revision;return;}
#else
    if(p.phase==FILE_HEADER){r=read_at(&p.jpeg_file,p.file_offset,photo_scratch,512u);if(!r)return;
        const uint32_t count=(WIDTH*HEIGHT*2u+2047u)/2048u;
        if(r<0||ln_read_le32(photo_scratch)!=0x4d49464cu||
           ln_read_le32(photo_scratch+4)!=p.photo.jpeg_bytes||
           ln_read_le32(photo_scratch+8)!=WIDTH||ln_read_le32(photo_scratch+12)!=HEIGHT||
           ln_read_le32(photo_scratch+20)!=count||count>(508u-24u)/2u||
           ln_crc32(photo_scratch,508u)!=ln_read_le32(photo_scratch+508u)){fail(22);return;}
        uint32_t encoded=0;
        for(uint32_t i=0;i<count;++i){uint32_t n=ln_read_le16(photo_scratch+24u+i*2u);
            if(!n||n>2048u||n>p.photo.jpeg_bytes-512u-encoded){fail(22);return;}encoded+=n;}
        if(encoded!=p.photo.jpeg_bytes-512u){fail(22);return;}
        for(uint32_t i=24u+count*2u;i<508u;++i)if(photo_scratch[i]){fail(22);return;}
        copy(p.pim_header,photo_scratch,512u);
        p.photo.jpeg_crc=ln_read_le32(photo_scratch+16);p.jpeg_at=p.file_payload_at=0;
        p.jpeg_crc=UINT32_MAX;p.phase=FILE_PIXELS;return;}
    if(p.phase==FILE_PIXELS){
        if(p.jpeg_at<WIDTH*HEIGHT*2u){
            uint32_t first=p.jpeg_at/2048u,count=ln_read_le32(p.pim_header+20),bytes=0,blocks=0;
            while(first+blocks<count&&blocks<4u){uint32_t n=ln_read_le16(p.pim_header+24u+(first+blocks)*2u);
                if(bytes+n>8192u)break;bytes+=n;++blocks;}
            r=read_at(&p.jpeg_file,p.file_offset+512u+p.file_payload_at,photo_scratch,bytes);
            if(!r)return;if(r<0){fail(23);return;}
            uint32_t at=0;
            for(uint32_t i=0;i<blocks;++i){
                uint32_t stored=ln_read_le16(p.pim_header+24u+(first+i)*2u);
                uint32_t visible=WIDTH*HEIGHT*2u-p.jpeg_at;if(visible>2048u)visible=2048u;
                uint8_t*destination=(uint8_t*)photo_buffers.surface.frame[p.front^1u]+p.jpeg_at;
                uint8_t*decoded=visible==2048u?destination:photo_buffers.pixels.jpeg;
                if(!ln_pimg_decode_tile(photo_scratch+at,stored,decoded)){fail(23);return;}
                if(visible!=2048u){for(uint32_t j=visible;j<2048u;++j)if(decoded[j]){fail(23);return;}
                    copy(destination,decoded,visible);}
                p.jpeg_crc=ln_pimg_crc_update(p.jpeg_crc,destination,visible);
                p.jpeg_at+=visible;at+=stored;
            }
            p.file_payload_at+=bytes;return;
        }
        if(p.file_payload_at+512u!=p.photo.jpeg_bytes||(p.jpeg_crc^UINT32_MAX)!=p.photo.jpeg_crc){fail(24);return;}
        p.front^=1u;p.asset.pixels=photo_buffers.surface.frame[p.front];p.frame_valid=p.view_loaded=p.ready=true;p.phase=IDLE;++p.revision;return;}
#endif
    if(p.phase==TXN_GUARD){
        /* A missing marker is the only result that permits catalog reads.
         * Direct file viewing enters its own phase and bypasses this guard. */
        r=open_file(transaction_marker,0);if(!r)return;
        if(r!=LN_FAT32_NOT_FOUND){fail(PHOTO_RECOVERY_REQUIRED);return;}
        p.phase=OPEN;return;
    }
    if(p.phase==OPEN){
        r=open_file(package,&p.file);if(!r)return;
        /* A device with no prepared photos has no catalog. Publish the same
         * settled empty list as an empty catalog, after the transaction guard
         * has ruled out an interrupted publication. Other open failures remain
         * errors, so a damaged or unreadable library is never hidden. */
        if(r==LN_FAT32_NOT_FOUND){
            clear(&p.header,sizeof p.header);clear(&p.file,sizeof p.file);
            p.first=p.count=p.photo_index=p.window_first=0;p.view=LN_PHOTOS_LIST;
            p.ready=p.pim_verified=true;p.metadata_valid=p.grid_available=p.grid_cache=false;
            p.resume_pending=p.prefetching=p.prefetch_job=false;p.phase=IDLE;++p.revision;return;
        }
        if(r<0){fail(1);return;}p.phase=HEADER;return;
    }
    if(p.phase==HEADER){r=read_at(&p.file,0,photo_scratch,512);if(!r)return;if(r<0){fail(2);return;}
        copy(p.pim_header,photo_scratch,512u);if(!ln_pimg_header(p.pim_header,p.file.size,&p.header)){fail(2);return;}
        p.grid_available=p.pim_verified=false;verify_begin(false);
        /* A tiny catalog is cheaper to verify than opening a second file. */
        if(p.header.data_at>4096u)p.phase=GRID_OPEN;return;}
    if(p.phase==GRID_OPEN){r=open_file(grid_package,&p.grid_file);if(!r)return;
        if(r<0){grid_fallback();return;}p.phase=GRID_HEADER;return;}
    if(p.phase==GRID_HEADER){r=read_at(&p.grid_file,0,photo_scratch,1024u);if(!r)return;
        p.grid_available=r>0&&ln_pgrid_header(photo_scratch,p.grid_file.size,p.pim_header,&p.header,
            LINNET_TARGET_NUMBER,THUMB_WIDTH,THUMB_HEIGHT,&p.grid);
        if(!p.active&&(!p.grid_available||p.grid.prefix_bytes+p.grid.string_bytes+p.grid.photo_crc_bytes>sizeof p.metadata)){grid_fallback();return;}
        verify_begin(p.grid_available&&p.grid.prefix_bytes+p.grid.string_bytes+p.grid.photo_crc_bytes<=sizeof p.metadata);return;}
    if(p.phase==GRID_VERIFY){
        uint32_t left=p.grid.data_at-1024u-p.grid_at;
        if(left){
            if(!p.verify_cached){uint32_t n=left;if(n>sizeof photo_scratch)n=sizeof photo_scratch;
                struct ln_audio_status audio;ln_audio_snapshot(&audio);if(audio.playing&&!audio.paused&&n>8192u)n=8192u;
                if(p.pending)n=p.io_bytes;
                r=read_at(&p.grid_file,1024u+p.grid_at,photo_scratch,n);if(!r)return;
                if(r<0){grid_fallback();return;}p.verify_cached=n;p.verify_cursor=0;
            }
            uint32_t n=p.verify_cached;if(n>4096u)n=4096u;
            const uint8_t*bytes=photo_scratch+p.verify_cursor;
            uint32_t cursor=0,end=p.grid.prefix_bytes+p.grid.string_bytes,table_end=end+p.grid.photo_crc_bytes;
            while(cursor<n){uint32_t at=p.grid_at+cursor,take=n-cursor;bool valid;
                if(at<p.grid.prefix_bytes){if(take>p.grid.prefix_bytes-at)take=p.grid.prefix_bytes-at;valid=verify_bytes(512u+at,bytes+cursor,take);}
                else if(at<end){if(take>end-at)take=end-at;valid=verify_bytes(p.header.string_at+at-p.grid.prefix_bytes,bytes+cursor,take);}
                else if(at<table_end){if(take>table_end-at)take=table_end-at;valid=true;}
                else valid=ln_bytes_zero(bytes+cursor,take);
                if(!valid){grid_fallback();return;}
                if(at<table_end)copy(p.metadata+at,bytes+cursor,take);cursor+=take;
            }
            p.verify_crc=ln_pimg_crc_update(p.verify_crc,bytes,n);p.grid_at+=n;p.verify_cursor+=n;p.verify_cached-=n;return;
        }
        if((p.verify_crc^UINT32_MAX)!=p.grid.metadata_crc||p.v_photos!=p.header.photos||p.v_levels!=p.header.levels||p.v_text_bytes||p.v_more){
            grid_fallback();return;
        }
        p.grid_cache=p.metadata_valid=true;
        if(!p.ready){p.first=0;p.count=p.header.photos;}p.ready=true;p.phase=IDLE;++p.revision;
        if(p.resume_pending&&!resume_matches())p.resume_pending=false;
        if(!p.active&&p.resume_pending){uint32_t index=ln_read_le32(p.resume+4);p.photo_index=index==EMPTY_ALBUM_RESUME?0:index;p.window_first=p.photo_index/GRID_PAGE*GRID_PAGE;}return;
    }
    if(p.phase==VERIFY){
        /* Coalesce catalog reads. When audio is idle, validate a larger memory
         * slice; keep the existing bound while audio is running. */
        uint32_t left=p.header.data_at-p.verify_at;
        if(left){
            if(!p.verify_cached){
                uint32_t bytes=left;if(bytes>sizeof photo_buffers.surface.verify.metadata)bytes=sizeof photo_buffers.surface.verify.metadata;
                if(p.pending)bytes=p.io_bytes;
                else {
                    struct ln_audio_status audio;ln_audio_snapshot(&audio);
                    /* Return the shared reader within the PCM reserve. */
                    if(audio.playing&&!audio.paused&&bytes>8192u)bytes=8192u;
                }
                uint32_t at=0;
                r=read_at(&p.file,p.verify_at,photo_buffers.surface.verify.metadata+at,bytes);
                if(!r)return;if(r<0){fail(3);return;}
                p.verify_cached=bytes;p.verify_cursor=at;
            }
            struct ln_audio_status audio;ln_audio_snapshot(&audio);
            uint32_t limit=audio.playing&&!audio.paused?4096u:16384u;
            uint32_t n=p.verify_cached;if(n>limit)n=limit;
            if(p.verify_at<p.header.tile_at&&p.verify_at+n>p.header.tile_at)n=p.header.tile_at-p.verify_at;
            else if(p.verify_at>=p.header.tile_at&&p.verify_at<p.header.string_at){
                if(p.verify_at+n>p.header.string_at)n=p.header.string_at-p.verify_at;
                if(n>=12u)n-=n%12u;
            }
            const uint8_t *bytes=photo_buffers.surface.verify.metadata+p.verify_cursor;
            if(!verify_bytes(p.verify_at,bytes,n)){fail(4);return;}
            uint32_t cached_end=metadata_end();
            if(cached_end){
                uint32_t end=p.verify_at+n;
                if(p.verify_at<cached_end){uint32_t count=end<cached_end?n:cached_end-p.verify_at;
                    copy(p.metadata+p.verify_at-512u,bytes,count);}
                if(end>p.header.string_at){uint32_t start=p.verify_at>p.header.string_at?p.verify_at:p.header.string_at;
                    copy(p.metadata+cached_end-512u+start-p.header.string_at,bytes+start-p.verify_at,end-start);}
            }
            p.verify_crc=ln_pimg_crc_update(p.verify_crc,bytes,n);
            p.verify_at+=n;p.verify_cursor+=n;p.verify_cached-=n;return;
        }
        if((p.verify_crc^UINT32_MAX)!=p.header.metadata_crc||p.v_photos!=p.header.photos||p.v_levels!=p.header.levels||p.v_tiles!=p.header.tiles||p.v_photo!=p.header.photos||!pack_coverage()||p.v_text_bytes||p.v_more){fail(5);return;}p.metadata_valid=metadata_end()!=0;p.pim_verified=true;
        if(!p.ready){p.first=0;p.count=p.header.photos;}p.ready=true;p.phase=IDLE;++p.revision;
        if(p.resume_pending&&!resume_matches())p.resume_pending=false;
        if(!p.active&&p.resume_pending){uint32_t index=ln_read_le32(p.resume+4);p.photo_index=index==EMPTY_ALBUM_RESUME?0:index;p.window_first=p.photo_index/GRID_PAGE*GRID_PAGE;}return;
    }
    if(p.phase==RECORD){
        uint32_t size=p.job_album?32u:64u,at=(p.job_album?p.header.album_at:p.header.photo_at)+p.job_index*size;
        r=read_at(&p.file,at,photo_scratch,size);if(!r)return;struct row*row=&p.rows[p.job_slot];
        bool valid=r>0&&(p.job_album?ln_pimg_album(&p.header,photo_scratch,p.job_index,&row->data.a):ln_pimg_photo(&p.header,photo_scratch,p.job_index,&row->data.p));
        if(!valid){fail(6);return;}row->index=p.job_index;row->album=p.job_album;
        row->asset=(struct ln_ui_asset){THUMB_WIDTH,THUMB_HEIGHT,photo_buffers.pixels.thumbs[p.job_slot]};
        if(!row->album&&p.view==LN_PHOTOS_LIST){row->title[0]=0;row->valid=true;p.phase=IDLE;}else p.phase=TITLE;return;
    }
    if(p.phase==TITLE){struct row*row=&p.rows[p.job_slot];r=string_read(row->album?row->data.a.title:row->data.p.title,row->album?row->data.a.bytes:row->data.p.bytes,false);
        if(!r)return;if(r<0){fail(7);return;}text(row->title,(char*)photo_scratch);row->valid=row->title_ready=true;p.phase=IDLE;++p.revision;return;}
    if(p.phase==THUMB){struct row*row=&p.rows[p.job_slot];
        if(p.grid_available){
            /* Playback may start between planning and admission. Rebound an
             * unsubmitted batch; an admitted request must retain its size. */
            if(!p.pending&&!background_allowed()&&p.thumb_group_count>8192u/p.grid.stride)p.thumb_group_count=8192u/p.grid.stride;
            r=read_at(&p.grid_file,p.grid.data_at+p.job_index*p.grid.stride,photo_scratch,p.thumb_group_count*p.grid.stride);if(!r)return;
            if(r<0){grid_fallback();return;}
            p.phase=GRID_THUMB_COMMIT;return;
        }
        r=read_at(&p.file,row->data.p.thumb,photo_scratch,8192);
        if(!r)return;if(r<0||ln_crc32(photo_scratch,8192)!=row->data.p.thumb_crc){fail(8);return;}
        /* The LPIC fallback keeps square, letterboxed previews. Crop their fitted
         * image before scaling into the native grid cell, preserving aspect. */
        /* Thumbnail pixels overlap viewer tiles, so their metadata can no
         * longer certify the cached tile contents after this write. */
        for(uint32_t i=0;i<TILES;i++)p.tiles[i].valid=false;
        uint32_t sw=row->data.p.width,sh=row->data.p.height;
        if(sw>64u||sh>64u){if(sw>=sh){sh=sh*64u/sw;sw=64u;}else{sw=sw*64u/sh;sh=64u;}}
        if(!sw)sw=1;if(!sh)sh=1;uint32_t sx=(64u-sw)/2u,sy=(64u-sh)/2u;
        if(sw*THUMB_HEIGHT>sh*THUMB_WIDTH){uint32_t width=sh*THUMB_WIDTH/THUMB_HEIGHT;sx+=(sw-width)/2u;sw=width?width:1u;}
        else{uint32_t height=sw*THUMB_HEIGHT/THUMB_WIDTH;sy+=(sh-height)/2u;sh=height?height:1u;}
        for(uint32_t y=0;y<THUMB_HEIGHT;++y)for(uint32_t x=0;x<THUMB_WIDTH;++x)
            photo_buffers.pixels.thumbs[p.job_slot][y*THUMB_WIDTH+x]=ln_read_le16(photo_scratch+2u*((sy+y*sh/THUMB_HEIGHT)*64u+sx+x*sw/THUMB_WIDTH));
        row->thumb=true;p.phase=IDLE;++p.revision;return;}
    if(p.phase==GRID_THUMB_COMMIT){
        const uint8_t*bytes=photo_scratch+p.thumb_group_cursor*p.grid.stride;
        if(ln_crc32(bytes,p.grid.stride-4u)!=ln_read_le32(bytes+p.grid.stride-4u)){
            grid_fallback();return;
        }
        if(++p.thumb_group_cursor<p.thumb_group_count)return;
        /* Publish only after every CRC in the exact, uncanceled batch passes. */
        for(uint32_t i=0;i<TILES;++i)p.tiles[i].valid=false;
        for(uint32_t i=0;i<p.thumb_group_count;++i){uint32_t slot=(p.job_index+i)%ROWS;
            copy(photo_buffers.pixels.thumbs[slot],photo_scratch+i*p.grid.stride,p.grid.stride-4u);p.rows[slot].thumb=true;}
        p.phase=IDLE;++p.revision;return;
    }
    if(p.phase==PHOTO_CHECK_LEVELS){
        uint32_t bytes=p.photo.levels*24u;
        r=read_at(&p.file,p.header.level_at+p.photo.first*24u,photo_scratch,bytes);if(!r)return;
        if(r<0){image_fail(9);return;}
        p.photo_check_crc=ln_pimg_crc_update(UINT32_MAX,photo_scratch,bytes);
        uint32_t first=ln_read_le32(photo_scratch+12u);
        const uint8_t*last=photo_scratch+(p.photo.levels-1u)*24u;
        uint32_t final=ln_read_le32(last+12u),count=ln_read_le32(last+16u);
        if(final<first||final>=p.header.tiles||count>p.header.tiles-final){image_fail(9);return;}
        p.photo_check_at=p.header.tile_at+first*12u;
        p.photo_check_end=p.header.tile_at+(final+count)*12u;p.phase=PHOTO_CHECK_TILES;return;
    }
    if(p.phase==PHOTO_CHECK_TILES){
        if(p.photo_check_at<p.photo_check_end){
            uint32_t bytes=p.photo_check_end-p.photo_check_at;if(bytes>8184u)bytes=8184u;
            r=read_at(&p.file,p.photo_check_at,photo_scratch,bytes);if(!r)return;
            if(r<0){image_fail(10);return;}
            p.photo_check_crc=ln_pimg_crc_update(p.photo_check_crc,photo_scratch,bytes);
            p.photo_check_at+=bytes;return;
        }
        uint32_t at=p.grid.prefix_bytes+p.grid.string_bytes+p.photo_index*4u;
        if((p.photo_check_crc^UINT32_MAX)!=ln_read_le32(p.metadata+at)){image_fail(10);return;}
        p.phase=LEVEL;return;
    }
    if(p.phase==LEVEL){
        /* Descriptors are contiguous; retain the same request until it drains. */
        if(!p.photo.levels||p.photo.levels>6u||p.photo.first>p.header.levels||p.photo.levels>p.header.levels-p.photo.first){image_fail(9);return;}
        struct level_cache*entry=&p.level_cache[p.photo_index&1u];
        if(!p.pending&&entry->valid&&entry->index==p.photo_index&&entry->count==p.photo.levels){
            copy(p.levels,photo_maps.levels[p.photo_index&1u],p.photo.levels*sizeof p.levels[0]);p.level_count=p.photo.levels;
        }else{
        r=read_at(&p.file,p.header.level_at+p.photo.first*24u,photo_scratch,p.photo.levels*24u);if(!r)return;
        if(r<0){image_fail(9);return;}
        for(p.level_count=0;p.level_count<p.photo.levels;++p.level_count){
        struct ln_pimg_level*l=&p.levels[p.level_count];
        if(!ln_pimg_level(&p.header,photo_scratch+p.level_count*24u,p.photo.first+p.level_count,l)||l->photo!=p.photo_index||
           l->width>p.photo.width||l->height>p.photo.height||(!p.level_count&&(l->width>WIDTH||l->height>HEIGHT))||
           (p.level_count&&(l->width<p.levels[p.level_count-1].width||l->height<p.levels[p.level_count-1].height||(l->width==p.levels[p.level_count-1].width&&l->height==p.levels[p.level_count-1].height)||l->first!=p.levels[p.level_count-1].first+p.levels[p.level_count-1].count))){image_fail(9);return;}
        }
        /* Keep the two adjacent viewer descriptions alongside their tile cache.
         * Reversing Next/Previous then needs no repeated FAT/record request. */
        entry->valid=false;entry->index=p.photo_index;entry->count=p.photo.levels;
        copy(photo_maps.levels[p.photo_index&1u],p.levels,p.photo.levels*sizeof p.levels[0]);entry->valid=true;
        }
        p.view_loaded=true;p.phase=IDLE;
        if(p.resume_pending&&p.photo_index==ln_read_le32(p.resume+4)){uint32_t flags=ln_read_le32(p.resume+8),pan=ln_read_le32(p.resume+12);p.zoom=flags&255u;p.rotation=(flags>>8)&3u;p.pan_x=(int16_t)pan;p.pan_y=(int16_t)(pan>>16);p.resume_pending=false;}
        if(p.zoom>=p.photo.levels)p.zoom=p.photo.levels-1u;layout();
        /* The existing two frame buffers can retain an adjacent image pair.
         * Reversals reuse the composed native view, including scaled photos. */
        if(!p.zoom&&!p.rotation&&!p.pan_x&&!p.pan_y){for(uint32_t i=0;i<2u;++i){
            if(p.frame_cache[i].valid&&p.frame_cache[i].index==p.photo_index){
                p.front=i;p.asset.pixels=photo_buffers.surface.frame[i];p.frame_valid=p.frame_complete=true;
                PHOTO_DIAG(timing.frame_ready=ln_time_us());slide_ready();++p.revision;break;
            }
        }}return;
    }
    if(p.phase==TILE_RECORD){r=tile_record();if(!r)return;
        if(r<0){image_fail(10);return;}
        /* Adjacent visible tiles share one read, using the existing scratch
         * buffer. Stop at the level/page boundary and never fetch hidden tiles. */
        const struct ln_pimg_level*l=&p.levels[p.level];p.tile_group_count=1;
        uint32_t first_length,first_crc;
        if(!ln_pimg_tile(&p.header,p.tile_records+(p.tile_index-p.tile_records_first)*12u,p.tile_index,&p.tile_group_at,&first_length,&first_crc)){image_fail(10);return;}
        p.tile_group_bytes=first_length;
        struct ln_player_status player;ln_player_snapshot(&player);
        uint32_t batch=(player.phase==LN_PLAYER_IDLE||player.phase==LN_PLAYER_ERROR||player.phase==LN_PLAYER_PAUSE)?sizeof photo_scratch/2048u:4u;
        while(p.tile_group_count<batch){
            uint32_t index=p.tile_index+p.tile_group_count;
            if(index>=p.header.tiles||index-l->first>=l->count||
               index-p.tile_records_first>=p.tile_records_count||
               !is_visible(index)||tile_find(index)>=0)break;
            uint32_t at,length,crc;
            if(!ln_pimg_tile(&p.header,p.tile_records+(index-p.tile_records_first)*12u,index,&at,&length,&crc)||
               at!=p.tile_group_at+p.tile_group_bytes){image_fail(10);return;}
            if(length>batch*2048u-p.tile_group_bytes)break;
            p.tile_group_bytes+=length;
            ++p.tile_group_count;
        }
        p.tile_group_cursor=0;
#if LINNET_TARGET_NUMBER==98u
        p.phase=TILE_DATA;
#else
        uint32_t bucket=p.tile_group_at>>27u;
        p.phase=p.pack_ready&&p.pack_bucket==bucket?TILE_DATA:PACK_OPEN;
#endif
        return;}
#if LINNET_TARGET_NUMBER!=98u
    if(p.phase==PACK_OPEN){uint32_t bucket=p.tile_group_at>>27u;pack_name(bucket);
        r=open_file(p.pack_path,&p.pack_file);if(!r)return;
        if(r<0||p.pack_file.size!=ln_read_le32(p.header.packs+bucket*12u+8u)){image_fail(11);return;}
        p.pack_bucket=bucket;p.pack_ready=true;p.phase=TILE_DATA;return;}
#endif
    if(p.phase==TILE_DATA){
#if LINNET_TARGET_NUMBER==98u
        r=read_at(&p.file,p.tile_group_at,photo_scratch,p.tile_group_bytes);
#else
        r=read_at(&p.pack_file,p.tile_group_at&(LN_PIMG_PACK_LIMIT-1u),photo_scratch,p.tile_group_bytes);
#endif
        if(!r)return;
        if(r<0){image_fail(11);return;}p.phase=TILE_COMMIT;return;}
    if(p.phase==TILE_COMMIT){
        uint32_t index=p.tile_index+p.tile_group_cursor,at,length,crc;
        if(!ln_pimg_tile(&p.header,p.tile_records+(index-p.tile_records_first)*12u,index,&at,&length,&crc)||
           at<p.tile_group_at||at-p.tile_group_at>p.tile_group_bytes||length>p.tile_group_bytes-(at-p.tile_group_at)){image_fail(11);return;}
        const uint8_t*bytes=photo_scratch+(at-p.tile_group_at);
        p.tile_slot=victim();if(p.tile_slot==UINT32_MAX){fail(19);return;}
        p.tiles[p.tile_slot].valid=false;
        uint8_t*decoded=(uint8_t*)photo_buffers.pixels.viewer_tiles[p.tile_slot];
        if(!ln_pimg_decode_tile(bytes,length,decoded)||ln_crc32(decoded,2048u)!=crc){image_fail(11);return;}
        const struct ln_pimg_level*l=&p.levels[p.level];uint32_t local=index-l->first,tx=local%l->columns*32u,ty=local/l->columns*32u;
        /* Interior tiles have no padding; edge tiles inspect only padding.
         * The full payload CRC above still covers every pixel. */
        if(tx+32u>l->width||ty+32u>l->height){
            uint32_t used_x=l->width-tx,used_y=l->height-ty;if(used_x>32u)used_x=32u;
            for(uint32_t y=0;y<32u;y++)for(uint32_t x=y<used_y?used_x:0u;x<32u;x++)
                if(photo_buffers.pixels.viewer_tiles[p.tile_slot][y*32u+x]){image_fail(21);return;}
        }
        p.tiles[p.tile_slot]=(struct tile){index,++p.age,true};++p.ready_tiles;
        if(++p.tile_group_cursor==p.tile_group_count)p.phase=IDLE;return;}
    if(p.phase==SOURCE){r=string_read(p.photo.source,p.photo.source_bytes,true);if(!r)return;if(r<0){fail(12);return;}copy(p.source,photo_scratch,p.photo.source_bytes+1u);p.jpeg_mode=true;p.frame_valid=false;p.redraw=p.rendering=false;
        for(uint32_t i=0;i<ROWS;i++)p.rows[i].thumb=false;for(uint32_t i=0;i<TILES;i++)p.tiles[i].valid=false;p.phase=JPEG_OPEN;return;}
    if(p.phase==JPEG_OPEN){r=open_file(p.source,&p.jpeg_file);if(!r)return;if(r<0||p.jpeg_file.size!=p.photo.jpeg_bytes){fail(13);return;}p.jpeg_at=0;p.jpeg_crc=UINT32_MAX;p.phase=JPEG_LOAD;return;}
    if(p.phase==JPEG_LOAD){uint32_t n=p.photo.jpeg_bytes-p.jpeg_at;if(n>4096)n=4096;
        if(n){r=read_at(&p.jpeg_file,p.jpeg_at,photo_buffers.pixels.jpeg+p.jpeg_at,n);if(!r)return;if(r<0){fail(14);return;}p.jpeg_crc=ln_pimg_crc_update(p.jpeg_crc,photo_buffers.pixels.jpeg+p.jpeg_at,n);p.jpeg_at+=n;return;}
        p.frame_cache[p.front^1u].valid=false;
        if((!file_mode&&(p.jpeg_crc^UINT32_MAX)!=p.photo.jpeg_crc)||ln_photo_jpeg_init(&p.jpeg,photo_buffers.pixels.jpeg,p.photo.jpeg_bytes,photo_buffers.surface.frame[p.front^1u],WIDTH,HEIGHT,WIDTH,p.photo.orientation)<0){fail(15);return;}
        p.phase=JPEG_DECODE;return;}
    if(p.phase==JPEG_DECODE){PHOTO_START();r=ln_photo_jpeg_step(&p.jpeg);PHOTO_DIAG(timing.jpeg_us+=ln_time_us()-started);if(!r)return;uint32_t w=p.photo.orientation>=5?p.jpeg.height:p.jpeg.width,h=p.photo.orientation>=5?p.jpeg.width:p.jpeg.height;
        if(r<0||(!file_mode&&(w!=p.photo.width||h!=p.photo.height))){fail(16);return;}p.front^=1u;p.asset.pixels=photo_buffers.surface.frame[p.front];p.frame_valid=p.view_loaded=true;p.ready=true;p.phase=IDLE;PHOTO_DIAG(timing.frame_ready=ln_time_us());slide_ready();++p.revision;return;}
    if(p.error)return;
    if(p.active&&p.resume_pending&&p.view!=LN_PHOTOS_VIEWER){
        restore_resume();return;
    }
    if(p.view!=LN_PHOTOS_VIEWER||!p.view_loaded){if(!plan_content(true)&&!p.active)p.prefetching=false;return;}
    if(p.view==LN_PHOTOS_VIEWER){
        if(p.jpeg_mode)return;
        if(p.rendering||p.redraw){render_step();return;}
        if(p.frame_complete)return;
        for(uint32_t i=0;i<p.visible_count;i++)if(tile_find(p.visible[i])<0){p.tile_slot=victim();if(p.tile_slot==UINT32_MAX){fail(19);return;}p.tile_index=p.visible[i];p.phase=TILE_RECORD;return;}
        p.frame_complete=true;p.ready_tiles=0;p.redraw=true;
        return;
    }
}
void ln_photos_step(void){
    /* Storage progress services the runtime, which may call us again while an
     * outer poll still owns its phase and buffer. Only that outer call advances. */
    if(p.stepping)return;p.stepping=true;
    uint32_t started=ln_time_us();
    for(uint32_t steps=0;steps<8u;steps++){
        photo_step();
        /* Finish short in-memory transitions without another executive turn.
         * Keep the existing I/O yields and a one-millisecond burst budget. */
        if(p.pending||(!p.active&&!p.prefetching)||p.error||ln_time_us()-started>=1000u||
           (!memory_work()&&!idle_plan(false)))break;
    }
    p.stepping=false;
}
