#include "coverflow.h"
#if defined(LINNET_COVERFLOW)
#include "services/music/library.h"
#include "services/music/browser.h"
#include "services/storage/storage.h"
#include "boot/include/linnet_boot.h"
#include "boot/include/linnet_bytes.h"
#include "hal/api/platform.h"
#if defined(LINNET_COVERFLOW_PACKED)
#include "services/music/player.h"
#include "hal/api/recovery.h"
#if LINNET_TARGET_NUMBER==25u
#include "services/files/files.h"
#endif
extern int ln_video_lz_decode_block(const uint8_t *,uint32_t,uint8_t *,uint32_t);
#endif
#define CATALOG_SLOTS LINNET_COVERFLOW_CATALOG_SLOTS
#define CATALOG_RECORD 1024u
#define ART_RECORD 13312u
#if LINNET_COVERFLOW_SCANLINE
#define COVER_MAGIC 0x4e46434cu
#define TILE_RECORD 23552u
#define TEXTURE_WIDTH 130u
#define TEXTURE_HEIGHT 174u
#define SLOTS 6u
#else
#define COVER_MAGIC 0x3246434cu
#define TILE_RECORD ART_RECORD
#define TEXTURE_WIDTH 96u
#define TEXTURE_HEIGHT 128u
#define SLOTS 16u
#endif
#define ART_LOW 630u
#if LINNET_COVERFLOW_EXPANDED
#define PAGE 64u
#define ART_PAGE 14u
#define ART_HIGH 1418u
#else
#define PAGE 16u
#define ART_PAGE 7u
#if LINNET_COVERFLOW_SCANLINE
/* Reserve three old tile units for the independent playback preview. */
#define ART_HIGH 466u
#else
#define ART_HIGH 469u
#endif
#endif
#define ART_SLOTS (ART_LOW+ART_HIGH)
#define PACKED_INPUT_MAX (TILE_RECORD+TILE_RECORD/255u+16u)
#define PACKED_INDEX_CRC 1u
#define NONE UINT32_MAX
#define COVER_WIDTH (96u*LINNET_UI_HEIGHT/176u)
enum {CLOSED,OPEN,HEADER,READY,CATALOG,ART,PACKED_HEADER,PACKED_DATA,PACKED_CHECK,EARLY_INDEX,TILTED};
/* Valid IDs publish only after a complete decode; pixel storage needs no reset. */
static union {
    uint16_t pixels[SLOTS][TEXTURE_WIDTH*TEXTURE_HEIGHT];
#if defined(LINNET_COVERFLOW_PACKED)
    struct {uint8_t tiles[7][TILE_RECORD];uint16_t playback[TEXTURE_WIDTH*TEXTURE_HEIGHT];
        uint8_t input[PACKED_INPUT_MAX];} packed;
#endif
} textures __attribute__((section(".media_workspace"),aligned(32)));
#if defined(LINNET_COVERFLOW_PACKED)
static uint16_t (*packed_pixels)[TEXTURE_WIDTH*TEXTURE_HEIGHT];
#define cache_pixels (packed_pixels?packed_pixels:textures.pixels)
#else
#define cache_pixels textures.pixels
#endif
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
#define CACHE_SLOTS 10u
#define EXTRA_RGB_STRIDE ((TEXTURE_WIDTH*TEXTURE_HEIGHT*sizeof(uint16_t)+31u)&~31u)
_Static_assert(TILE_RECORD%32u==0u,"RGB overlay alignment");
_Static_assert(TILE_RECORD+3u*EXTRA_RGB_STRIDE<=__builtin_offsetof(__typeof__(textures),packed.playback),"RGB overlay overlaps playback scratch");
#else
#define CACHE_SLOTS (SLOTS+1u)
#endif
static struct {uint32_t id,age;} cache[CACHE_SLOTS];
#if defined(LINNET_COVERFLOW_PACKED)
_Static_assert(sizeof textures.packed<=sizeof textures.pixels,"Packed textures exceed the existing budget");
static uint8_t packed_header[512] __attribute__((aligned(32)));
static uint32_t packed_at,packed_bytes,packed_offset,packed_crc,packed_index,packed_previous,packed_decode_max_us;
static bool packed_mode,packed_checked;
static bool packed_drawing;
static uint32_t packed_frame_art[7];
static uint8_t *packed_extra;
static uint32_t packed_extra_bytes;
static bool packed_uses_stage,packed_stage_inhibited;
#if LINNET_TARGET_NUMBER==25u
static uint32_t early_reads;
static bool packed_started,packed_first_done,early_index_ready;
static uint32_t early_slots,packed_index_crc;
#define EARLY_RAW_AT 135744u
_Static_assert(EARLY_RAW_AT>=3u*sizeof textures.pixels[0],"Early raw tiles overlap RGB previews");
_Static_assert(EARLY_RAW_AT%32u==0u,"Early raw tile alignment");
_Static_assert(EARLY_RAW_AT+3u*TILE_RECORD<=__builtin_offsetof(__typeof__(textures),packed.input),"Early raw tiles overlap decoder input");
static uint8_t *metadata_lease;
static uint32_t metadata_lease_bytes,metadata_lease_epoch=1u;
static uint8_t *history_lease;
static uint32_t history_lease_bytes,history_lease_epoch=1u,history_mount_epoch;
static void history_revoke(void){
    if(!history_lease)return;
    history_lease=0;history_lease_bytes=history_mount_epoch=0;
    if(++history_lease_epoch==0)++history_lease_epoch;
}
#endif
#else
#define packed_mode false
#define packed_checked true
#endif
static uint32_t age;
#if LINNET_COVERFLOW_EXPANDED
static uint8_t catalog[CATALOG_SLOTS][CATALOG_RECORD] __attribute__((section(".cover_catalog"),aligned(32)));
#else
/* Tags stay in cleared BSS and publish only validated, complete records.
 * The unread payload needs no boot-time clear, like the artwork banks. */
static uint8_t catalog[CATALOG_SLOTS][CATALOG_RECORD] __attribute__((section(".media_workspace"),aligned(32)));
#endif
static uint32_t tags[CATALOG_SLOTS];
static uint8_t *catalog_extra;
static uint32_t *catalog_extra_tags,catalog_capacity=CATALOG_SLOTS;
static struct ln_fat32_file file;
static uint8_t header[1024] __attribute__((aligned(32)))
#if defined(LINNET_COVERFLOW_HEADER_SECTION)
    __attribute__((section(LINNET_COVERFLOW_HEADER_SECTION)))
#endif
;
static uint8_t art_low[ART_LOW*ART_RECORD] __attribute__((section(".cover_low"),aligned(32)));
static uint8_t art_high[ART_HIGH*ART_RECORD] __attribute__((section(".cover_high"),aligned(32)));
static uint32_t art_tags[ART_SLOTS],art_warm,art_loaded,art_base,art_end,art_remaining;
static uint32_t arts;
static bool art_bulk;
#define ART_LOW_SLOTS (sizeof art_low/TILE_RECORD)
#define ART_CAPACITY (ART_LOW_SLOTS+sizeof art_high/TILE_RECORD)
#if LINNET_COVERFLOW_SCANLINE
extern const uint8_t ln_coverflow_preview_x[130],ln_coverflow_preview_y[174];
#endif
static uint8_t *art_record(uint32_t id){uint32_t slot=id%ART_CAPACITY;return slot<ART_LOW_SLOTS?art_low+slot*TILE_RECORD:art_high+(slot-ART_LOW_SLOTS)*TILE_RECORD;}
static bool art_ready(uint32_t id){return packed_mode?id<arts:art_tags[id%ART_CAPACITY]==id+1u;}
static uint32_t count,selected,phase,loading,length,revision,drawn,next,io_due,warm,moved_at;
static uint32_t catalog_loaded,full_reads,read_bytes,hits,preview_hits,misses,last_full_us,max_full_us,last_preview_us;
static int32_t position,direction=1;
static bool visible,pending,failed,canceling,moved_in_visit,mapped_art;
static enum ln_music_library_status displayed_library_status=LN_LIBRARY_CLOSED;
static const uint32_t generation=0x79000001u;
struct ln_coverflow_column {uint32_t step;uint8_t source,top,height,length;};
extern const struct ln_coverflow_column ln_coverflow_columns[33][COVER_WIDTH];
extern const uint8_t ln_coverflow_widths[33];
#if LINNET_COVERFLOW_SCANLINE
extern const uint8_t ln_coverflow_glow_pairs[190][160];
extern const uint8_t ln_coverflow_glow_colors[2][192][2];
#else
extern const uint8_t ln_coverflow_glow[LINNET_UI_WIDTH][LINNET_UI_HEIGHT*140u/176u];
#endif
static bool identity(void){uint8_t hash[32];if(!ln_music_library_identity(hash))return false;
    for(uint32_t i=0;i<32;i++)if(hash[i]!=header[16+i])return false;return true;}
#if LINNET_COVERFLOW_SCANLINE
#define TILTED_STRIDE 54u
#define TILTED_HEIGHT 190u
#else
#define TILTED_STRIDE 39u
#define TILTED_HEIGHT 140u
#endif
#define TILTED_DATA (2u*TILTED_STRIDE*TILTED_HEIGHT*2u)
#define TILTED_RECORD ((TILTED_DATA+4u+31u)&~31u)
#define TILTED_CAPACITY (sizeof art_low/(TILE_RECORD+TILTED_RECORD))
static uint8_t *tilted_tiles;
static uint8_t tilted_valid[TILTED_CAPACITY];
static uint32_t tilted_at,tilted_bytes,tilted_loaded,tilted_warm;
static bool tilted_waiting(void){return tilted_tiles&&tilted_loaded<arts;}
static void tilted_revoke(void){tilted_tiles=0;tilted_at=tilted_bytes=tilted_loaded=tilted_warm=0;}
static void tilted_admit(void){
    tilted_tiles=0;tilted_bytes=tilted_loaded=tilted_warm=0;
    if(!tilted_at||ln_read_le32(header+72)||!arts||arts>TILTED_CAPACITY)return;
    /* This tail is beyond every indexed artwork DMA extent. Keep the high
     * bank free for Files/history; never borrow the firmware staging area. */
    tilted_tiles=art_low+arts*TILE_RECORD;tilted_bytes=arts*TILTED_RECORD;
    for(uint32_t i=0;i<arts;i++)tilted_valid[i]=0;
}
static const uint16_t *tilted_pixels(uint32_t id){
    struct ln_fat32_file root;
    if(!tilted_tiles||id>=arts||!tilted_valid[id]||!identity()||
       !ln_storage_media_root(&root)||root.epoch!=file.epoch)return 0;
    return (const uint16_t *)(tilted_tiles+id*TILTED_RECORD);
}
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
/* Small raw libraries leave this entire declared array outside every raw DMA
 * extent. Keep direct RGB records here, with validity in unused artwork tags. */
static uint8_t *raw_rgb;
static uint32_t raw_rgb_bytes;
_Static_assert(ART_CAPACITY<ART_SLOTS,"Raw RGB validity tag capacity");
static void raw_rgb_revoke(void){raw_rgb=0;raw_rgb_bytes=0;}
static void raw_rgb_admit(void){
    raw_rgb_revoke();
    uint32_t extended=0;
    if(tilted_tiles||packed_at||packed_stage_inhibited||!arts||arts>ART_LOW_SLOTS||
       arts>ART_SLOTS-ART_CAPACITY||arts>sizeof art_high/EXTRA_RGB_STRIDE||
       !ln_platform_extended_memory(&extended)||!extended)return;
    uint32_t low=sizeof art_low-arts*TILE_RECORD;
    /* Preserve the winning Files region after both current 32-page history
     * windows, even when the high RGB reservation is withdrawn. */
    if(low<2u*32u*4096u||low-2u*32u*4096u<=sizeof art_high)return;
    raw_rgb=art_high;raw_rgb_bytes=arts*EXTRA_RGB_STRIDE;
    for(uint32_t i=0;i<arts;i++)art_tags[ART_CAPACITY+i]=0;
}
static const uint16_t *raw_rgb_pixels(uint32_t id){
    struct ln_fat32_file root;
    if(!raw_rgb||id>=arts||!art_ready(id)||art_tags[ART_CAPACITY+id]!=id+1u||
       !identity()||!ln_storage_media_root(&root)||root.epoch!=file.epoch)return 0;
    return (const uint16_t*)(raw_rgb+id*EXTRA_RGB_STRIDE);
}
#else
#define raw_rgb_revoke() ((void)0)
#define raw_rgb_admit() ((void)0)
#define raw_rgb_pixels(id) ((const uint16_t*)0)
#define raw_rgb_bytes 0u
#endif
#if defined(LINNET_SOC_PP5022)
static uint32_t files_epoch=1u;
static void files_revoke(void){if(++files_epoch==0)++files_epoch;}
static void files_tail(uint8_t *memory,uint32_t capacity,uint32_t used,uint8_t **best,uint32_t *bytes){
    if(used>capacity)return;
    uintptr_t first=((uintptr_t)memory+used+31u)&~(uintptr_t)31u,end=(uintptr_t)memory+capacity;
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    uintptr_t loan=(uintptr_t)history_lease;
    if(history_lease&&loan>=first&&loan<end&&history_lease_bytes<=end-loan)end=loan;
#endif
    if(first<end&&end-first>*bytes){*best=(uint8_t*)first;*bytes=(uint32_t)(end-first);}
}
uint8_t *ln_coverflow_files_memory(uint32_t *bytes,uint32_t *epoch){
    if(!bytes||!epoch)return 0;*bytes=*epoch=0;
    struct ln_fat32_file root;
    if(phase!=READY||failed||pending||canceling||!packed_checked||!identity()||
       !ln_storage_media_root(&root)||root.epoch!=file.epoch)return 0;
    uint8_t *best=0;
#if defined(LINNET_COVERFLOW_PACKED)
    if(packed_mode){
        uint32_t remaining=packed_bytes-512u;
        uint32_t low=remaining<sizeof art_low?remaining:sizeof art_low;remaining-=low;
        uint32_t high=remaining<sizeof art_high?remaining:sizeof art_high;remaining-=high;
        files_tail(art_low,sizeof art_low,low,&best,bytes);
        files_tail(art_high,sizeof art_high,high,&best,bytes);
        if(packed_extra){
            uint32_t used=(remaining+31u)&~31u;
            if(packed_pixels)used+=7u*TEXTURE_WIDTH*TEXTURE_HEIGHT*sizeof(uint16_t);
            files_tail(packed_extra,packed_extra_bytes,used,&best,bytes);
        }
    }else
#endif
    {
        /* A raw request can reach only slots belonging to declared artwork.
         * Slot remainders and unused arrays stay outside every DMA extent. */
        uint32_t reachable=arts<ART_CAPACITY?arts:ART_CAPACITY;
        uint32_t low=reachable<ART_LOW_SLOTS?reachable:ART_LOW_SLOTS;
        files_tail(art_low,sizeof art_low,low*TILE_RECORD+tilted_bytes,&best,bytes);
        files_tail(art_high,sizeof art_high,(reachable-low)*TILE_RECORD+raw_rgb_bytes,&best,bytes);
    }
    if(best)*epoch=files_epoch;
    return best;
}
#else
#define files_revoke() ((void)0)
#endif
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
uint8_t *ln_coverflow_metadata_reserve(uint32_t bytes,uint32_t *epoch){
    if(!epoch)return 0;*epoch=0;
    if(!bytes||bytes>UINT32_MAX-31u||metadata_lease||phase!=CLOSED||pending||canceling||packed_stage_inhibited)return 0;
    uint32_t capacity=0;uint8_t *memory=ln_player_art_memory(&capacity);
    uint32_t rounded=(bytes+31u)&~31u;
    uintptr_t end=((uintptr_t)memory+capacity)&~(uintptr_t)31u;
    if(!memory||end<(uintptr_t)memory||rounded>end-(uintptr_t)memory)return 0;
    /* Revoke every old Files advertisement before the library writes here. */
    files_revoke();metadata_lease=(uint8_t*)(end-rounded);metadata_lease_bytes=rounded;
    *epoch=metadata_lease_epoch;return metadata_lease;
}
uint8_t *ln_coverflow_metadata_memory(uint32_t epoch){
    return epoch&&epoch==metadata_lease_epoch?metadata_lease:0;
}
void ln_coverflow_metadata_release(uint32_t epoch){
    if(!metadata_lease||epoch!=metadata_lease_epoch)return;
    files_revoke();metadata_lease=0;metadata_lease_bytes=0;
    if(++metadata_lease_epoch==0)++metadata_lease_epoch;
}
uint8_t *ln_coverflow_history_reserve(uint32_t bytes,uint32_t *epoch){
    if(!epoch)return 0;*epoch=0;
    if(!bytes||bytes>UINT32_MAX-31u||history_lease||packed_stage_inhibited)return 0;
    uint32_t capacity=0,files_generation=0;
    uint8_t *memory=ln_coverflow_files_memory(&capacity,&files_generation);
    uint32_t rounded=(bytes+31u)&~31u,floor=ln_files_cache_maximum_bytes();
    if(!memory||!files_generation||capacity<floor||rounded>capacity-floor)return 0;
    uintptr_t end=((uintptr_t)memory+capacity)&~(uintptr_t)31u;
    if(end<(uintptr_t)memory||rounded>end-(uintptr_t)memory)return 0;
    history_lease=(uint8_t*)(end-rounded);history_lease_bytes=rounded;history_mount_epoch=file.epoch;
    uint32_t remaining=0,after_generation=0;
    uint8_t *after=ln_coverflow_files_memory(&remaining,&after_generation);
    /* A different winning region would displace warm Files pages. Admission
     * is RAM-only, so withdraw this tentative reservation before returning. */
    if(after!=memory||remaining<floor||after_generation!=files_generation){history_revoke();return 0;}
    *epoch=history_lease_epoch;return history_lease;
}
uint8_t *ln_coverflow_history_memory(uint32_t epoch){
    struct ln_fat32_file root;
    return epoch&&epoch==history_lease_epoch&&history_lease&&
        ln_storage_media_root(&root)&&root.epoch==history_mount_epoch?history_lease:0;
}
void ln_coverflow_history_release(uint32_t epoch){
    if(epoch==history_lease_epoch)history_revoke();
}
void ln_coverflow_history_reset(void){history_revoke();}
#endif
static uint8_t *catalog_record(uint32_t index){
    uint32_t slot=index%catalog_capacity;
    return slot<CATALOG_SLOTS?catalog[slot]:catalog_extra+(slot-CATALOG_SLOTS)*CATALOG_RECORD;
}
static uint32_t *catalog_tag(uint32_t index){
    uint32_t slot=index%catalog_capacity;
    return slot<CATALOG_SLOTS?tags+slot:catalog_extra_tags+slot-CATALOG_SLOTS;
}
static uint8_t *row_checked(uint32_t index){
    return index<count&&*catalog_tag(index)==index+1u?catalog_record(index):0;
}
static uint8_t *row(uint32_t index){
    if(catalog_extra&&!identity())return 0;
    return row_checked(index);
}
static void catalog_extend(void){
    uint32_t bytes=0;uint8_t *memory=ln_music_library_catalog_memory(&bytes);
    uint32_t extra=bytes/(CATALOG_RECORD+sizeof(uint32_t));extra=extra/PAGE*PAGE;
    if(!memory||!extra)return;
    catalog_extra_tags=(uint32_t*)memory;catalog_extra=memory+extra*sizeof(uint32_t);
    for(uint32_t i=0;i<extra;i++)catalog_extra_tags[i]=0;
    catalog_capacity=CATALOG_SLOTS+extra;
}
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
static uint32_t packed_cache_slots(void){return packed_pixels?CACHE_SLOTS:7u;}
static uint16_t *cache_slot_pixels(uint32_t slot){
    if(packed_mode&&packed_pixels&&slot>=7u)
        return (uint16_t*)((uint8_t*)&textures+TILE_RECORD+(slot-7u)*EXTRA_RGB_STRIDE);
    return cache_pixels[slot];
}
#else
#define packed_cache_slots() 7u
#define cache_slot_pixels(slot) cache_pixels[slot]
#endif
static int32_t find_art(uint32_t id){for(uint32_t i=0;i<(packed_mode?packed_cache_slots():SLOTS);i++)if(cache[i].id==id+1u)return (int32_t)i;return -1;}
static bool close_to_selection(uint32_t art);
static bool selected_full(void){
    uint8_t *r=row(selected);if(!r)return false;uint32_t id=ln_read_le32(r+248);
    if(art_ready(id))return true;
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    return find_art(id)>=0;
#else
    return false;
#endif
}
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
void ln_coverflow_preview_reset(void){
    if(early_reads&&!packed_mode&&!packed_pixels)
        for(uint32_t i=0;i<3u;i++)cache[i].id=0;
    early_reads=3u;
    early_slots=0;
}
#endif
static void invalidate(void){
    raw_rgb_revoke();
    tilted_revoke();
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    history_revoke();
    ln_coverflow_preview_reset();early_reads=0;
#endif
    files_revoke();
    /* The library may already be reusing its tail for a new verification.
     * Drop the lease without clearing memory owned by that new generation. */
    catalog_extra=0;catalog_extra_tags=0;catalog_capacity=CATALOG_SLOTS;
    for(uint32_t i=0;i<CATALOG_SLOTS;i++)tags[i]=0;
    for(uint32_t i=0;i<CACHE_SLOTS;i++)cache[i].id=0;
    for(uint32_t i=0;i<ART_SLOTS;i++)art_tags[i]=0;
    art_warm=art_loaded=art_base=art_end=art_remaining=0;art_bulk=false;
    count=arts=selected=warm=catalog_loaded=0;position=0;phase=CLOSED;++revision;
#if defined(LINNET_COVERFLOW_PACKED)
    packed_mode=packed_checked=false;packed_at=packed_bytes=packed_offset=packed_index=0;
#if LINNET_TARGET_NUMBER==25u
    packed_started=packed_first_done=early_index_ready=false;early_slots=0;
#endif
    packed_extra=0;packed_extra_bytes=0;packed_uses_stage=false;packed_pixels=0;
#endif
}
bool ln_coverflow_release_catalog(void){
    if(pending){
        if(!canceling){ln_storage_media_cancel(generation);canceling=true;}
        return false;
    }
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    ln_coverflow_metadata_release(metadata_lease_epoch);
#endif
    invalidate();failed=false;canceling=false;io_due=ln_time_us();return true;
}
#if defined(LINNET_COVERFLOW_PACKED)
bool ln_coverflow_stage_claim(void){
    /* Upload/verification can span many executive turns. This inhibition is
     * retained until reboot, even if a transfer is abandoned after BEGIN. */
    packed_stage_inhibited=true;
#if LINNET_TARGET_NUMBER==25u
    raw_rgb_revoke();
    ln_coverflow_preview_reset();
    history_revoke();
    ln_coverflow_metadata_release(metadata_lease_epoch);
#endif
    if(!packed_uses_stage)return true;
    if(pending){
        if(!canceling){ln_storage_media_cancel(generation);canceling=true;}
        return false;
    }
    files_revoke();
    packed_mode=false;packed_checked=true;packed_extra=0;packed_extra_bytes=0;packed_uses_stage=false;packed_pixels=0;
#if LINNET_TARGET_NUMBER==25u
    packed_started=packed_first_done=early_index_ready=false;early_slots=0;
#endif
    for(uint32_t i=0;i<CACHE_SLOTS;i++)cache[i].id=0;
    for(uint32_t i=0;i<ART_SLOTS;i++)art_tags[i]=0;
    art_warm=art_loaded=art_base=art_end=art_remaining=0;art_bulk=false;
    phase=count?READY:CLOSED;io_due=ln_time_us();++revision;return true;
}
bool ln_coverflow_stage_prepare(void){
    uint32_t began=ln_time_us();
    while(!ln_coverflow_stage_claim()){
        /* Deferred polling cannot dispatch Link recursively or admit new
         * artwork. Firmware writes begin only after canceled DMA drains. */
        ln_coverflow_step(false);ln_recovery_feed();
        if(ln_time_us()-began>=1000000u)return false;
    }
    return true;
}
#endif
static void rewind_art_warm(void){
    if(!art_bulk||loading<art_base||loading>=art_end)return;
    uint32_t n=length/TILE_RECORD;
    if(n>art_end-art_base-art_remaining)n=art_end-art_base-art_remaining;
    art_remaining+=n;art_warm=loading;
}
void ln_coverflow_visible(bool value){
    if(value&&!visible){moved_in_visit=false;io_due=ln_time_us();}
    if(!value)moved_in_visit=false;
    visible=value;
}
uint32_t ln_coverflow_count(void){return count;}
bool ln_coverflow_active(void){return visible;}
bool ln_coverflow_move(int32_t delta){
    if(!count||!delta)return false;int32_t n=(int32_t)selected+(delta>64?64:delta< -64?-64:delta);
    uint32_t target=n<0?0u:(uint32_t)n>=count?count-1u:(uint32_t)n;if(target==selected)return false;
    int32_t moving=target>selected?1:-1;
    direction=moving;
    if(target/catalog_capacity!=selected/catalog_capacity)warm=(target/catalog_capacity)*catalog_capacity;
    selected=target;
    if(phase==ART&&!canceling){
        bool needed=false;
        for(uint32_t i=0;i<length/TILE_RECORD;i++)if(close_to_selection(loading+i)){needed=true;break;}
        if(!needed){
            if(pending){ln_storage_media_cancel(generation);canceling=true;}
            else {rewind_art_warm();phase=READY;}
        }
    }
    int32_t distance=(int32_t)(selected*256u)-position;
    if(distance>512)position=(int32_t)(selected*256u)-512;
    else if(distance< -512)position=(int32_t)(selected*256u)+512;
    moved_at=ln_time_us();io_due=moved_at;
    moved_in_visit=visible;
    if(selected_full()){++hits;last_full_us=0;}else if(row(selected))++preview_hits;else ++misses;
    last_preview_us=row(selected)?0u:NONE;
    ++revision;next=0;return true;
}
const char *ln_coverflow_text(bool artist){
    enum ln_music_library_status status=ln_music_library_status();
    if(status==LN_LIBRARY_ERROR&&ln_music_library_error()==LN_MUSIC_LIBRARY_RECOVERY_REQUIRED)
        return artist?"":"Recover music in Media Manager";
    if(status==LN_LIBRARY_ERROR&&ln_music_library_error()==LN_FAT32_NOT_FOUND)
        return artist?"":"No albums";
    if(status==LN_LIBRARY_ERROR)return artist?"":"Music library unavailable";
    if(status==LN_LIBRARY_READY&&!ln_music_library_count())return artist?"":"No albums";
    if(failed)return artist?"Sync music to prepare artwork":"Cover Flow needs a sync";
    if(!count)return artist?"":phase>=READY?"No albums":"Albums";
    uint8_t *r=row(selected);return r?(const char*)r+(artist?128:8):(artist?"":"Albums");
}
bool ln_coverflow_select(void){uint8_t *r=row(selected);return r&&identity()&&
    ln_music_browser_open_album(ln_read_le32(r),ln_read_le32(r+4),(const char*)r+8);}
static void fail(void){
    raw_rgb_revoke();
    tilted_revoke();
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    history_revoke();
    ln_coverflow_preview_reset();
#endif
    failed=true;phase=CLOSED;io_due=NONE;++revision;
}
static void request_catalog(uint32_t index){
    loading=(index/PAGE)*PAGE;length=count-loading;if(length>PAGE)length=PAGE;
    uint32_t slot=loading%catalog_capacity,available=slot<CATALOG_SLOTS?CATALOG_SLOTS-slot:catalog_capacity-slot;
    if(length>available)length=available;
    for(uint32_t i=0;i<length;i++)*catalog_tag(loading+i)=0;
    length*=CATALOG_RECORD;phase=CATALOG;
}
static bool close_to_selection(uint32_t art){
    for(int32_t d=-3;d<=3;d++){int32_t n=(int32_t)selected+d;if(n<0)continue;
        uint8_t *r=row((uint32_t)n);if(r&&ln_read_le32(r+248)==art)return true;}
    return false;
}
static bool warm_protected(uint32_t art){
    uint32_t tag=art_tags[art%ART_CAPACITY];
    return tag&&tag!=art+1u&&close_to_selection(tag-1u);
}
static void art_warm_window(void){
    uint32_t base=0,end=arts<ART_CAPACITY?arts:ART_CAPACITY,anchor=0;
    if(arts>ART_CAPACITY){
        bool found=false;
        /* New image IDs follow album order; shared covers can point backward.
         * Nearby novel artwork keeps late albums in their own cache region. */
        for(int32_t d=-3;d<=3;d++){
            int32_t index=(int32_t)selected+d;if(index<0)continue;
            uint8_t *r=row((uint32_t)index);if(!r)continue;
            uint32_t id=ln_read_le32(r+248);if(!found||id>anchor)anchor=id;found=true;
        }
        if(!found)return;
        base=anchor>ART_CAPACITY/2u?anchor-ART_CAPACITY/2u:0u;
        if(base>arts-ART_CAPACITY)base=arts-ART_CAPACITY;
        end=base+ART_CAPACITY;
    }
    if(base==art_base&&end==art_end)return;
    art_base=base;art_end=end;art_remaining=end-base;
    /* Start near the selection, then wrap once through the bounded region. */
    art_warm=direction<0&&anchor>base+64u?anchor-64u:anchor;
    if(art_warm<base||art_warm>=end)art_warm=base;
}
static void advance_art_warm(uint32_t n){
    art_remaining-=n;art_warm+=n;if(art_warm==art_end)art_warm=art_base;
}
static uint32_t decode_priority(uint32_t art){
#if LINNET_COVERFLOW_SCANLINE
    static const int8_t ahead[]={0,1,-1,2,-2,3};
    uint32_t limit=position==(int32_t)(selected*256u)?6u:3u;
    for(uint32_t i=0;i<limit;i++){
        int32_t index=(int32_t)selected+ahead[i]*direction;
        if(index<0)continue;uint8_t *r=row((uint32_t)index);
        if(r&&ln_read_le32(r+248)==art)return i<3u?2u:1u;
    }
#else
    (void)art;
#endif
    return 0;
}
#if defined(LINNET_COVERFLOW_PACKED)
static uint8_t *packed_location(uint32_t offset,uint32_t *available){
    if(offset<sizeof art_low){*available=sizeof art_low-offset;return art_low+offset;}
    offset-=sizeof art_low;
    if(offset<sizeof art_high){*available=sizeof art_high-offset;return art_high+offset;}
    offset-=sizeof art_high;
    if(!packed_extra||offset>=packed_extra_bytes){*available=0;return 0;}
    *available=packed_extra_bytes-offset;return packed_extra+offset;
}
#if LINNET_TARGET_NUMBER==25u
static const uint8_t *packed_input(uint32_t offset,uint32_t bytes){
    if(packed_bytes<512u||!bytes||bytes>PACKED_INPUT_MAX||offset>packed_bytes-512u||bytes>packed_bytes-512u-offset)return 0;
    uint32_t available;const uint8_t *source=packed_location(offset,&available);
    /* The decoder reads byte-aligned tokens and its literal copier accepts
     * unaligned sources. A complete block in one bank needs no staging copy. */
    if(source&&bytes<=available)return source;
    uint32_t copied=0;
    while(copied<bytes){
        source=packed_location(offset+copied,&available);
        uint32_t n=bytes-copied;if(n>available)n=available;if(!source||!n)return 0;
        __builtin_memcpy(textures.packed.input+copied,source,n);copied+=n;
    }
    return textures.packed.input;
}
static bool decode_early_packed(bool allow_read){
    if(!early_index_ready||early_reads>=3u||!visible||!allow_read||ln_player_busy()||
       packed_stage_inhibited||packed_checked||packed_mode||packed_pixels||!identity())return false;
    struct ln_fat32_file root;
    if(!ln_storage_media_root(&root)||root.epoch!=file.epoch)return false;
    static const int8_t ahead[]={0,1,-1};
    for(uint32_t d=0;d<3u;d++){
        int32_t index=(int32_t)selected+ahead[d]*direction;
        uint8_t *r=index<0?0:row((uint32_t)index);if(!r)continue;
        uint32_t id=ln_read_le32(r+248);if(id>=arts||find_art(id)>=0)continue;
        const uint8_t *entry=art_low+id*8u;
        uint32_t offset=ln_read_le32(entry),bytes=ln_read_le32(entry+4);
        if(offset<arts*8u||!bytes||bytes>PACKED_INPUT_MAX||offset>packed_offset||bytes>packed_offset-offset)continue;
        const uint8_t *input=packed_input(offset,bytes);if(!input){fail();return false;}
        uint32_t slot=early_reads;uint8_t *tile=(uint8_t*)textures.pixels+EARLY_RAW_AT+slot*TILE_RECORD;
        uint32_t began=ln_time_us();
        int result=ln_video_lz_decode_block(input,bytes,tile,TILE_RECORD);
        uint32_t elapsed=ln_time_us()-began;if(elapsed>packed_decode_max_us)packed_decode_max_us=elapsed;
        uint32_t end=512u+TEXTURE_WIDTH*TEXTURE_HEIGHT;
        if(result!=1||ln_crc32(tile,end)!=ln_read_le32(tile+end)){fail();return false;}
        const uint16_t *palette=(const uint16_t*)tile;const uint32_t *indices=(const uint32_t*)(tile+512u);
        for(uint32_t i=0;i<TEXTURE_WIDTH*TEXTURE_HEIGHT;i+=4u){uint32_t p=*indices++;
            textures.pixels[slot][i]=palette[p&255u];textures.pixels[slot][i+1u]=palette[(p>>8)&255u];
            textures.pixels[slot][i+2u]=palette[(p>>16)&255u];textures.pixels[slot][i+3u]=palette[p>>24];}
        ++early_reads;early_slots|=1u<<slot;
        cache[slot].id=id+1u;cache[slot].age=++age;++revision;return true;
    }
    return false;
}
#endif
static void packed_frame_begin(int32_t center){
    packed_drawing=packed_mode;
    if(!packed_drawing)return;
    for(int32_t d=-3;d<=3;d++){
        int32_t index=center+d;uint8_t *r=index<0?0:row((uint32_t)index);
        packed_frame_art[d+3]=r?ln_read_le32(r+248):NONE;
    }
}
static bool packed_frame_uses(uint32_t id){
    if(!packed_drawing)return false;
    for(uint32_t i=0;i<7u;i++)if(packed_frame_art[i]==id)return true;
    return false;
}
static int32_t decode_packed(uint32_t id){
    int32_t found=find_art(id);if(found>=0){cache[found].age=++age;return found;}
    if(id>=arts)return -1;
    uint32_t victim=NONE,oldest=NONE,priority=3u;
    for(uint32_t i=0;i<packed_cache_slots();i++){
        if(!cache[i].id){victim=i;break;}
        /* Every card keeps its pointer through the whole native draw. Pin all
         * seven frame IDs before decoding any newly exposed outer card. */
        if(packed_frame_uses(cache[i].id-1u))continue;
        uint32_t rank=decode_priority(cache[i].id-1u);
        if(rank<priority||(rank==priority&&cache[i].age<oldest)){priority=rank;oldest=cache[i].age;victim=i;}
    }
    if(victim==NONE)return -1;
    cache[victim].id=0;
    const uint8_t *entry=art_low+id*8u;
    uint32_t offset=ln_read_le32(entry),bytes=ln_read_le32(entry+4);
#if LINNET_TARGET_NUMBER==25u
    const uint8_t *input=packed_input(offset,bytes);if(!input)return -1;
#else
    uint32_t copied=0;
    while(copied<bytes){
        uint32_t available;const uint8_t *source=packed_location(offset+copied,&available);
        uint32_t n=bytes-copied;if(n>available)n=available;if(!source||!n)return -1;
        __builtin_memcpy(textures.packed.input+copied,source,n);copied+=n;
    }
    const uint8_t *input=textures.packed.input;
#endif
    uint32_t began=ln_time_us();uint8_t *tile;
#if LINNET_TARGET_NUMBER==25u
    tile=textures.packed.tiles[packed_pixels?0u:victim];
#else
    tile=textures.packed.tiles[victim];
#endif
    int result=ln_video_lz_decode_block(input,bytes,tile,TILE_RECORD);
    uint32_t elapsed=ln_time_us()-began;if(elapsed>packed_decode_max_us)packed_decode_max_us=elapsed;
    uint32_t end=512u+TEXTURE_WIDTH*TEXTURE_HEIGHT;
    if(result!=1||ln_crc32(tile,end)!=ln_read_le32(tile+end)){fail();return -1;}
    if(packed_pixels){
        uint16_t *pixels=cache_slot_pixels(victim);
        const uint16_t *palette=(const uint16_t*)tile;const uint32_t *indices=(const uint32_t*)(tile+512u);
        for(uint32_t i=0;i<TEXTURE_WIDTH*TEXTURE_HEIGHT;i+=4u){uint32_t p=*indices++;
            pixels[i]=palette[p&255u];pixels[i+1u]=palette[(p>>8)&255u];
            pixels[i+2u]=palette[(p>>16)&255u];pixels[i+3u]=palette[p>>24];}
    }
    cache[victim].id=id+1u;cache[victim].age=++age;return (int32_t)victim;
}
#endif
#if LINNET_TARGET_NUMBER==25u
__attribute__((noinline))
#endif
static void expand_art(const uint8_t *packed,uint16_t *pixels){
    union {uint16_t colors[256];uint32_t words[128];} palette;
    const uint32_t *words=(const uint32_t*)packed;
    for(uint32_t i=0;i<128u;i++)palette.words[i]=words[i];
    words+=128;
    for(uint32_t i=0;i<TEXTURE_WIDTH*TEXTURE_HEIGHT;i+=4){
        uint32_t indices=*words++;
        pixels[i]=palette.colors[indices&255u];
        pixels[i+1]=palette.colors[(indices>>8)&255u];
        pixels[i+2]=palette.colors[(indices>>16)&255u];
        pixels[i+3]=palette.colors[indices>>24];
    }
}
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
static bool raw_rgb_prepare(uint32_t id){
    struct ln_fat32_file root;
    if(!raw_rgb||id>=arts||!art_ready(id)||!identity()||
       !ln_storage_media_root(&root)||root.epoch!=file.epoch)return false;
    expand_art(art_record(id),(uint16_t*)(raw_rgb+id*EXTRA_RGB_STRIDE));
    art_tags[ART_CAPACITY+id]=id+1u;
    return true;
}
#endif
static int32_t decode_art(uint32_t id){
#if defined(LINNET_COVERFLOW_PACKED)
    if(packed_mode)return decode_packed(id);
#endif
    int32_t found=find_art(id);if(found>=0)return found;
    if(!art_ready(id))return -1;
    uint32_t victim=0,oldest=NONE,priority=3u;
    for(uint32_t i=0;i<SLOTS;i++){
        if(!cache[i].id){victim=i;break;}
        uint32_t rank=decode_priority(cache[i].id-1u);
        if(rank<priority||(rank==priority&&cache[i].age<oldest)){
            priority=rank;oldest=cache[i].age;victim=i;
        }
    }
    expand_art(art_record(id),cache_pixels[victim]);
    cache[victim].id=id+1u;cache[victim].age=++age;
    return (int32_t)victim;
}
static const uint8_t *retained_art(uint32_t id){
#if defined(LINNET_COVERFLOW_PACKED)
    if(packed_mode){int32_t slot=decode_packed(id);
#if LINNET_TARGET_NUMBER==25u
        /* RGB cards retain independent buffers. The shared raw decode scratch
         * must never escape while the renderer gathers all seven cards. */
        if(packed_pixels)return 0;
#endif
        return slot<0?0:textures.packed.tiles[slot];}
#endif
    return art_ready(id)?art_record(id):0;
}
const uint16_t *ln_coverflow_playback_preview(const char *path,uint32_t crc){
#if LINNET_COVERFLOW_SCANLINE
    static const char prefix[]="/.LINNET/ART/";
    if(!mapped_art||failed||!identity()||!path)return 0;
    for(uint32_t i=0;i<sizeof prefix-1u;i++)if(path[i]!=prefix[i])return 0;
    uint32_t hash=0;
    for(uint32_t i=0;i<8u;i++){
        uint32_t c=(uint8_t)path[sizeof prefix-1u+i],v;
        if(c>='0'&&c<='9')v=c-'0';else if(c>='A'&&c<='F')v=c-'A'+10u;else return 0;
        hash|=v<<((i/2u)*8u+(i%2u?0u:4u));
    }
    const char *tail=path+sizeof prefix-1u+8u;
    if(tail[0]!='.'||tail[1]!='R'||tail[2]!='G'||tail[3]!='B'||tail[4])return 0;
    /* The entry identity check owns this non-yielding lookup. Keep normal
     * row() guarded, but avoid repeating the same identity for every album. */
    uint8_t *r=row_checked(selected);int32_t slot=-1;
    if(r&&ln_read_le32(r+240)==hash&&ln_read_le32(r+244)==crc){
        uint32_t id=ln_read_le32(r+248);const uint16_t *pixels=raw_rgb_pixels(id);
        if(pixels)return pixels;slot=decode_art(id);}
    /* Songs, queue changes and resume can request any retained album. Only
     * published catalog rows may bind its exact playback path and checksum. */
    for(uint32_t i=0;slot<0&&i<catalog_capacity;i++){
        uint32_t tag=*catalog_tag(i);if(!tag||tag>count)continue;
        r=row_checked(tag-1u);if(!r)continue;
        if(ln_read_le32(r+240)==hash&&ln_read_le32(r+244)==crc){
            uint32_t id=ln_read_le32(r+248);const uint16_t *pixels=raw_rgb_pixels(id);
            if(pixels)return pixels;slot=decode_art(id);}
    }
    if(slot>=0){cache[slot].age=++age;
#if defined(LINNET_COVERFLOW_PACKED)
        if(packed_mode&&!packed_pixels){const uint8_t *tile=textures.packed.tiles[slot];const uint16_t *palette=(const uint16_t*)tile;
            for(uint32_t i=0;i<TEXTURE_WIDTH*TEXTURE_HEIGHT;i++)textures.packed.playback[i]=palette[tile[512u+i]];
            return textures.packed.playback;}
#endif
        return cache_slot_pixels((uint32_t)slot);}
#else
    (void)path;(void)crc;
#endif
    return 0;
}
static void request_art(uint32_t id,bool bulk){
    loading=id;length=1u;art_bulk=bulk;
    uint32_t slot=id%ART_CAPACITY,available=slot<ART_LOW_SLOTS?ART_LOW_SLOTS-slot:ART_CAPACITY-slot;
    if(bulk)while((length+1u)*TILE_RECORD<=ART_PAGE*ART_RECORD&&length<available&&
        length<art_remaining&&loading+length<art_end&&!art_ready(loading+length)&&
        !warm_protected(loading+length))++length;
    for(uint32_t i=0;i<length;i++)art_tags[(loading+i)%ART_CAPACITY]=0;
    length*=TILE_RECORD;phase=ART;
}
static void accept_art(void){
    for(uint32_t i=0;i<length/TILE_RECORD;i++){
        uint32_t id=loading+i;const uint8_t *packed=art_record(id);
        uint32_t end=512u+TEXTURE_WIDTH*TEXTURE_HEIGHT;
        if(ln_crc32(packed,end)!=ln_read_le32(packed+end)){fail();return;}
        art_tags[id%ART_CAPACITY]=id+1u;++art_loaded;
        if(close_to_selection(id))++revision;
        uint8_t *r=row(selected);
        if(r&&ln_read_le32(r+248)==id&&moved_in_visit){
            last_full_us=ln_time_us()-moved_at;
            if(last_full_us>max_full_us)max_full_us=last_full_us;
        }
    }
}
void ln_coverflow_step(bool allow_read){
    if(pending){
        uint32_t transferred=0,began=ln_time_us();int result;
        for(uint32_t step=0;;step++){
            result=ln_storage_media_poll_deferred(generation,phase==OPEN?&file:0,&transferred);
            if(result||step==3u)break;
            uint32_t now=ln_time_us(),deadline;
            if(now-began>=500u||(ln_storage_idle_wait(now,&deadline)&&(int32_t)(deadline-now)>0))break;
        }
        if(!result){io_due=ln_time_us()+1000u;return;}pending=false;
        if(result<0){
            if(canceling&&result==LN_FAT32_STALE){
                canceling=false;rewind_art_warm();
                phase=READY;io_due=ln_time_us();return;
            }
            fail();return;
        }
        canceling=false;
        if(phase==OPEN){phase=HEADER;}
        else if(phase==HEADER){
            count=ln_read_le32(header+4);arts=ln_read_le32(header+52);
            uint32_t magic=ln_read_le32(header);
            mapped_art=LINNET_COVERFLOW_SCANLINE;
            uint64_t raw_end=1024ull+(uint64_t)count*CATALOG_RECORD+(uint64_t)arts*TILE_RECORD;
#if defined(LINNET_COVERFLOW_PACKED)
            packed_at=ln_read_le32(header+72);packed_bytes=ln_read_le32(header+76);
            bool geometry=(!packed_at&&!packed_bytes&&file.size==raw_end)||
                (packed_at&&packed_at==raw_end&&packed_bytes>=512ull+(uint64_t)arts*8u&&file.size==raw_end+packed_bytes);

#else
            bool geometry=file.size==raw_end;
#endif
            tilted_at=ln_read_le32(header+80);
            if(tilted_at)geometry=!ln_read_le32(header+72)&&!ln_read_le32(header+76)&&tilted_at==raw_end&&
                ln_read_le32(header+84)==TILTED_RECORD&&
                arts&&arts<=TILTED_CAPACITY&&file.size==raw_end+(uint64_t)arts*TILTED_RECORD;
            else if(ln_read_le32(header+84))geometry=false;
            if(transferred!=1024||magic!=COVER_MAGIC||count>100000u||arts>count||
               ln_read_le32(header+8)!=TILE_RECORD||ln_read_le32(header+12)!=TEXTURE_WIDTH||ln_read_le32(header+48)!=CATALOG_RECORD||
               (LINNET_COVERFLOW_SCANLINE&&ln_read_le32(header+68)!=1u)||
               ln_read_le32(header+56)!=24||ln_read_le32(header+60)!=32||
               ln_crc32(header,508)!=ln_read_le32(header+508)||ln_crc32(header+512,512)!=ln_read_le32(header+64)||
               !geometry||!identity()){
                count=0;fail();return;}
#if defined(LINNET_COVERFLOW_PACKED)
            /* Commit ownership only after the complete header is validated. */
            packed_extra_bytes=0;packed_extra=ln_music_library_art_memory(&packed_extra_bytes);
            packed_uses_stage=false;
            if(!packed_extra&&!packed_stage_inhibited){
                packed_extra=ln_player_art_memory(&packed_extra_bytes);packed_uses_stage=packed_extra!=0;
            }
#if LINNET_TARGET_NUMBER==25u
            if(packed_uses_stage&&metadata_lease&&packed_at){
                uint32_t payload=packed_bytes-512u,banks=sizeof art_low+sizeof art_high;
                if(payload>banks&&payload-banks<=packed_extra_bytes){
                    uint32_t used=(payload-banks+31u)&~31u;
                    uint32_t frames=7u*TEXTURE_WIDTH*TEXTURE_HEIGHT*sizeof(uint16_t);
                    if(used>packed_extra_bytes||frames>packed_extra_bytes-used)frames=0;
                    uintptr_t base=(uintptr_t)packed_extra,pin=(uintptr_t)metadata_lease;
                    uint32_t capacity=pin>=base&&pin-base<=packed_extra_bytes?(uint32_t)(pin-base):0;
                    uint32_t files=ln_files_cache_maximum_bytes();
                    /* Preserve packed eligibility, all previously allocated
                     * RGB frames, and the existing maximum Files cache. */
                    if(metadata_lease_bytes<=packed_extra_bytes&&used<=capacity&&
                       frames<=capacity-used&&files<=capacity-used-frames)
                        packed_extra_bytes=capacity;
                    else ln_coverflow_metadata_release(metadata_lease_epoch);
                }
            }
#endif
            if(packed_bytes>=512u&&packed_bytes-512u<=sizeof art_low+sizeof art_high){
                packed_extra=0;packed_extra_bytes=0;packed_uses_stage=false;
            }
            packed_checked=!packed_at||packed_bytes-512u>sizeof art_low+sizeof art_high+packed_extra_bytes;
            if(packed_checked){packed_extra=0;packed_extra_bytes=0;packed_uses_stage=false;}
#endif
            tilted_admit();
            raw_rgb_admit();catalog_extend();phase=READY;++revision;
        }else if(phase==CATALOG){
            if(transferred!=length){fail();return;}
            for(uint32_t i=0;i<length/CATALOG_RECORD;i++){
                uint32_t index=loading+i;uint8_t *r=catalog_record(index);
                uint32_t crc=ln_crc32_update(ln_crc32_update(0xffffffffu,r,252),r+256,768);
                if((~crc)!=ln_read_le32(r+252)||r[127]||r[LINNET_COVERFLOW_SCANLINE?239u:247u]||ln_read_le32(r+248)>=arts||
                   ln_read_le32(r)>=ln_music_library_group_count()||!ln_read_le32(r+4)||ln_read_le32(r+4)>ln_music_library_count()){
                    fail();return;}
                *catalog_tag(index)=index+1u;++catalog_loaded;
            }
            if(selected>=loading&&selected<loading+length/CATALOG_RECORD){
                if(moved_in_visit)
                    last_preview_us=ln_time_us()-moved_at;
                ++revision;
            }
            else if(visible&&loading<selected+4u&&loading+length/CATALOG_RECORD+3u>selected)++revision;
            phase=READY;
        }
        else if(phase==TILTED){
            if(!tilted_tiles||loading>=arts||transferred!=TILTED_RECORD){fail();return;}
            const uint8_t *tile=tilted_tiles+loading*TILTED_RECORD;
            if(ln_crc32(tile,TILTED_DATA)!=ln_read_le32(tile+TILTED_DATA)){fail();return;}
            tilted_valid[loading]=1;++tilted_loaded;phase=READY;
        }
        else if(phase==ART){
            if(transferred!=length){fail();return;}
            accept_art();if(failed)return;phase=READY;
        }
#if defined(LINNET_COVERFLOW_PACKED)
        else if(phase==PACKED_HEADER){
            if(transferred!=512u||ln_read_le32(packed_header)!=0x315a434cu||
               ln_read_le32(packed_header+4)!=arts||ln_read_le32(packed_header+8)!=TILE_RECORD||
               ln_read_le32(packed_header+12)!=packed_bytes-512u||ln_read_le32(packed_header+16)!=1u||
               ln_crc32(packed_header,508)!=ln_read_le32(packed_header+508)||
               ln_read_le32(packed_header+20)!=PACKED_INDEX_CRC){fail();return;}
            packed_offset=0;packed_crc=UINT32_MAX;phase=PACKED_DATA;
#if LINNET_TARGET_NUMBER==25u
            packed_started=true;
#endif
        }else if(phase==PACKED_DATA){
            if(transferred!=length){fail();return;}
            uint32_t available;const uint8_t *source=packed_location(packed_offset,&available);
            if(!source||length>available){fail();return;}
            packed_crc=ln_crc32_update(packed_crc,source,length);packed_offset+=length;
            if(packed_offset==packed_bytes-512u){
                if((~packed_crc)!=ln_read_le32(packed_header+24)){fail();return;}
#if LINNET_TARGET_NUMBER==25u
                /* Later DMA starts beyond the completed prefix and cannot
                 * replace its sealed index. Reuse that admitted proof, while
                 * retaining the final payload and index comparisons. */
                if(!early_index_ready){
                    packed_index_crc=UINT32_MAX;
                    packed_index=0;packed_previous=arts*8u;
                }
#else
                packed_index=0;packed_previous=arts*8u;
#endif
                phase=PACKED_CHECK;
            }
#if LINNET_TARGET_NUMBER==25u
            else if(!packed_first_done){
                packed_first_done=true;
                if(packed_offset>=arts*8u){
                    packed_index_crc=UINT32_MAX;
                    packed_index=0;packed_previous=arts*8u;phase=EARLY_INDEX;
                }else phase=READY;
            }
            else if(!early_index_ready&&packed_offset>=arts*8u){
                packed_index_crc=UINT32_MAX;
                packed_index=0;packed_previous=arts*8u;phase=EARLY_INDEX;
            }
#endif
        }
#endif
    }
    uint32_t now=ln_time_us();
#if defined(LINNET_COVERFLOW_PACKED)
    if(packed_stage_inhibited&&packed_uses_stage){
        if(!ln_coverflow_stage_claim()){io_due=now+1000u;return;}
    }
#endif
    if(failed){io_due=NONE;return;}
    enum ln_music_library_status status=ln_music_library_status();
    if(status==LN_LIBRARY_ERROR||(status==LN_LIBRARY_READY&&!ln_music_library_count())){
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
        ln_coverflow_metadata_release(metadata_lease_epoch);
#endif
        if(phase!=CLOSED||count)invalidate();
        io_due=NONE;return;
    }
    if(!allow_read||status!=LN_LIBRARY_READY){
        io_due=now+10000u;return;
    }
    if(phase>=READY&&!identity())invalidate();
#if defined(LINNET_COVERFLOW_PACKED)
#if LINNET_TARGET_NUMBER==25u
    if(phase==EARLY_INDEX){
        uint32_t first=packed_index,end=packed_index+32u;if(end>arts)end=arts;
        for(;packed_index<end;packed_index++){
            const uint8_t *entry=art_low+packed_index*8u;
            uint32_t offset=ln_read_le32(entry),size=ln_read_le32(entry+4);
            if(offset!=packed_previous||!size||size>PACKED_INPUT_MAX||offset>packed_bytes-512u||
               size>packed_bytes-512u-offset){fail();return;}
            packed_previous+=size;
        }
        packed_index_crc=ln_crc32_update(packed_index_crc,art_low+first*8u,(packed_index-first)*8u);
        if(packed_index<arts){io_due=now+1000u;return;}
        if(packed_previous!=packed_bytes-512u||(~packed_index_crc)!=ln_read_le32(packed_header+28)){fail();return;}
        early_index_ready=true;phase=READY;
    }
#endif
    if(phase==PACKED_CHECK){
        uint32_t end=packed_index+32u;if(end>arts)end=arts;
#if LINNET_TARGET_NUMBER==25u
        uint32_t first=packed_index;
#endif
        for(;packed_index<end;packed_index++){
            const uint8_t *entry=art_low+packed_index*8u;
            uint32_t offset=ln_read_le32(entry),size=ln_read_le32(entry+4);
            if(offset!=packed_previous||!size||size>PACKED_INPUT_MAX||offset>packed_bytes-512u||
               size>packed_bytes-512u-offset){fail();return;}
            packed_previous+=size;
        }
#if LINNET_TARGET_NUMBER==25u
        packed_index_crc=ln_crc32_update(packed_index_crc,art_low+first*8u,(packed_index-first)*8u);
#endif
        if(packed_index==arts){
            if(packed_previous!=packed_bytes-512u){fail();return;}
#if LINNET_TARGET_NUMBER==25u
            if((~packed_index_crc)!=ln_read_le32(packed_header+28)){fail();return;}
#endif
            /* Use a contiguous unoccupied lease tail for all seven decoded
             * cards. The compressed payload remains immutable underneath. */
            uint32_t used=packed_bytes-512u,base=sizeof art_low+sizeof art_high;
            used=used>base?used-base:0u;used=(used+31u)&~31u;
            if(packed_extra&&used<=packed_extra_bytes&&
               packed_extra_bytes-used>=7u*TEXTURE_WIDTH*TEXTURE_HEIGHT*sizeof(uint16_t))
                packed_pixels=(void*)(packed_extra+used);
#if LINNET_TARGET_NUMBER==25u
            /* Save RGB before changing the union interpretation. Retained
             * raw sources are disjoint from all three destination slots. */
            if(packed_pixels)for(uint32_t i=0;i<3u;i++)
                if((early_slots&(1u<<i))&&cache[i].id)
                    __builtin_memcpy(packed_pixels[i],textures.pixels[i],sizeof textures.pixels[i]);
            for(uint32_t i=0;!packed_pixels&&i<3u;i++)if((early_slots&(1u<<i))&&cache[i].id)
                __builtin_memcpy(textures.packed.tiles[i],(uint8_t*)textures.pixels+EARLY_RAW_AT+i*TILE_RECORD,TILE_RECORD);
            for(uint32_t i=0;i<CACHE_SLOTS;i++)if(i>=3u||!(early_slots&(1u<<i)))cache[i].id=0;
#else
            for(uint32_t i=0;i<CACHE_SLOTS;i++)cache[i].id=0;
#endif
            packed_mode=packed_checked=true;art_loaded=arts;art_warm=arts;art_remaining=0;
            phase=READY;++revision;
        }else{io_due=now+1000u;return;}
    }
#endif
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
    if((phase==READY||phase==PACKED_DATA)&&decode_early_packed(allow_read)){io_due=now+1000u;return;}
    if(failed)return;
#endif
    if(phase==READY){
        if(count&&!row(selected))request_catalog(selected);
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
        else if(count&&!packed_checked&&!packed_started)phase=PACKED_HEADER;
#endif
        else if(count){
            static const int8_t ahead[]={0,1,-1,2,-2,3,-3};
            bool decoded=false;
            for(uint32_t d=0;d<sizeof(ahead);d++){
                if(d>=3u&&((!visible&&!packed_mode)||(uint32_t)(now-moved_at)<120000u))break;
                int32_t index=(int32_t)selected+ahead[d]*direction;
                if(index<0||(uint32_t)index>=count)continue;
                uint8_t *r=row((uint32_t)index);if(!r){request_catalog((uint32_t)index);break;}
                uint32_t art=ln_read_le32(r+248);
                if(!packed_checked)continue;
                if(!art_ready(art)){request_art(art,false);break;}
                if(tilted_tiles&&!tilted_valid[art]){loading=art;length=TILTED_RECORD;phase=TILTED;break;}
#if LINNET_COVERFLOW_SCANLINE
                uint32_t decode_limit=packed_mode?7u:position==(int32_t)(selected*256u)?6u:3u;
#else
                uint32_t decode_limit=7u;
#endif
                bool ready=find_art(art)>=0;
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
                if(raw_rgb)ready=raw_rgb_pixels(art)!=0;
#endif
                if((visible||packed_mode)&&d<decode_limit&&!ready){
#if defined(LINNET_COVERFLOW_PACKED) && LINNET_TARGET_NUMBER==25u
                    if(raw_rgb){if(!raw_rgb_prepare(art))continue;}else
#endif
                    (void)decode_art(art);
#if !LINNET_COVERFLOW_SCANLINE
                    ++revision;
#endif
                    /* Indexed and decoded native pixels are identical. Cache
                     * preparation alone does not require another LCD frame. */
                    decoded=true;break;
                }
            }
            if(decoded){io_due=now+1000u;return;}
        }
        if(phase==READY
#if LINNET_TARGET_NUMBER==25u
           /* A complete resident catalog cannot evict its validated pages. */
           &&(count>catalog_capacity||catalog_loaded<count)
#endif
        ){
            uint32_t base=count<=catalog_capacity?0u:(selected>128u?((selected-128u)/PAGE)*PAGE:0u);
            uint32_t end=count<=catalog_capacity?count:selected+128u;if(end>count)end=count;
            if(warm<base||warm>=end)warm=base;
            for(uint32_t n=0;warm<end&&n<catalog_capacity/PAGE;n++){
                uint32_t index=warm;warm+=PAGE;
                if(!row(index)){request_catalog(index);break;}
            }
        }
        if(phase==READY&&!packed_checked){
#if defined(LINNET_COVERFLOW_PACKED)
            for(uint32_t i=0;i<ART_SLOTS;i++)art_tags[i]=0;
            for(uint32_t i=0;i<CACHE_SLOTS;i++){
#if LINNET_TARGET_NUMBER==25u
                if(i<3u&&early_reads&&cache[i].id)continue;
#endif
                cache[i].id=0;
            }
#if LINNET_TARGET_NUMBER==25u
            phase=packed_started?PACKED_DATA:PACKED_HEADER;
#else
            phase=PACKED_HEADER;
#endif
#endif
        }
        if(phase==READY&&!packed_mode){
            art_warm_window();
            while(art_remaining&&(art_ready(art_warm)||warm_protected(art_warm)))advance_art_warm(1u);
            if(art_remaining&&(!visible||(uint32_t)(now-moved_at)>=300000u)){
                request_art(art_warm,true);advance_art_warm(length/TILE_RECORD);
            }
        }
        if(phase==READY&&tilted_tiles&&(!visible||(uint32_t)(now-moved_at)>=300000u)){
            while(tilted_warm<arts&&tilted_valid[tilted_warm])++tilted_warm;
            if(tilted_warm<arts){loading=tilted_warm;length=TILTED_RECORD;phase=TILTED;}
        }
        if(phase==READY){
            uint32_t elapsed=now-moved_at;
            io_due=visible&&count&&elapsed<120000u?moved_at+120000u:
                visible&&(art_remaining||tilted_waiting())&&elapsed<300000u?
                moved_at+300000u:NONE;
            return;
        }
    }
    int result;
    if(phase==CLOSED||phase==OPEN){phase=OPEN;result=ln_storage_media_open("/.LINNET/COVERS.CFL",generation);}
    else if(phase==HEADER)result=ln_storage_media_read(&file,0,header,1024,generation);
    else if(phase==CATALOG)result=ln_storage_media_read(&file,1024+loading*CATALOG_RECORD,catalog_record(loading),length,generation);
    else if(phase==TILTED)result=ln_storage_media_read(&file,tilted_at+loading*TILTED_RECORD,
        tilted_tiles+loading*TILTED_RECORD,TILTED_RECORD,generation);
#if defined(LINNET_COVERFLOW_PACKED)
    else if(phase==PACKED_HEADER)result=ln_storage_media_read(&file,packed_at,packed_header,512u,generation);
    else if(phase==PACKED_DATA){
        uint32_t available;uint8_t *destination=packed_location(packed_offset,&available);
        length=packed_bytes-512u-packed_offset;if(length>ART_PAGE*ART_RECORD)length=ART_PAGE*ART_RECORD;
        if(length>available)length=available;
        if(!destination||!length){fail();return;}
        result=ln_storage_media_read(&file,packed_at+512u+packed_offset,destination,length,generation);
    }
#endif
    else result=ln_storage_media_read(&file,1024+count*CATALOG_RECORD+loading*TILE_RECORD,art_record(loading),length,generation);
    if(result>0){pending=true;if(phase==ART||phase==PACKED_DATA||phase==TILTED){++full_reads;read_bytes+=length;}else if(phase==CATALOG)read_bytes+=length;}
    else if(result<0){fail();return;}
    io_due=now+1000u;
}
uint32_t ln_coverflow_io_deadline(void){return io_due;}
uint32_t ln_coverflow_deadline(void){return visible?next:NONE;}
uint32_t ln_coverflow_tick(uint32_t now){
    if(!visible)return 0;
    enum ln_music_library_status status=ln_music_library_status();
    if(status!=displayed_library_status){displayed_library_status=status;++revision;}
    if(next!=NONE&&next&&(int32_t)(now-next)<0)return 0;
    int32_t diff=(int32_t)(selected*256u)-position;
    /* Keep the 50 Hz easing timeline anchored to elapsed time: scheduling each step from
     * 'now' stretched the animation whenever a frame exceeded 20 ms. Catch
     * up at most eight cheap position steps, then draw only the current pose. */
    uint32_t due=next&&next!=NONE?next:now;
    for(uint32_t steps=0;diff&&steps<8u;++steps){
        int32_t distance=(int32_t)(selected*256u)-position;
        int32_t step=distance*2/3;
        position+=(step?step:distance);due+=20000u;
        if(position==(int32_t)(selected*256u)||(int32_t)(now-due)<0)break;
    }

    uint32_t changed=drawn!=revision?1u:diff?8u:0u;drawn=revision;
    next=position!=(int32_t)(selected*256u)?due:NONE;
    return changed;
}
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t render_calls,render_us,render_max_us;
void ln_coverflow_render_diagnostics(uint32_t words[16]){
    uint32_t values[16]={0x52464c43u,render_calls,render_us,arts,art_warm,art_remaining,art_base,art_end};
#if defined(LINNET_COVERFLOW_PACKED)
    values[8]=packed_mode;values[9]=packed_bytes;values[10]=packed_decode_max_us;values[11]=catalog_capacity;
#if LINNET_TARGET_NUMBER==25u
    values[13]=(packed_started?1u:0u)|(early_index_ready?2u:0u)|(packed_first_done?4u:0u)|(early_slots<<8);
    values[13]|=(packed_mode?packed_cache_slots():raw_rgb?arts:SLOTS)<<16;
    values[14]=packed_offset;values[15]=early_reads;
    if(raw_rgb){values[14]=raw_rgb_bytes;values[15]=0;
        for(uint32_t i=0;i<arts;i++)if(art_tags[ART_CAPACITY+i]==i+1u)++values[15];}
#endif
#endif
    if(tilted_tiles){values[13]|=16u;values[14]=tilted_bytes;values[15]=tilted_loaded;}
    values[12]=render_max_us;
    for(uint32_t i=0;i<16;i++)words[i]=values[i];
}
void ln_coverflow_diagnostics(uint32_t words[16]){
    uint32_t values[16]={0x574f4c46u,phase,count,selected,catalog_loaded,hits,preview_hits,misses,
        full_reads,read_bytes,last_full_us,art_loaded,max_full_us,selected_full(),row(selected)!=0,pending};
    for(uint32_t i=0;i<16;i++)words[i]=values[i];
}
#endif
static void ambient_color(uint32_t index,uint32_t color[3]){
    const uint8_t *r=row(index);color[0]=color[1]=color[2]=0;if(!r)return;
    for(uint32_t x=3;x<24;x+=6)for(uint32_t y=3;y<24;y+=6){
        uint32_t p=r[256+x*32+y];
        color[0]+=p>>5;color[1]+=(p>>2)&7;color[2]+=p&3;
    }
    color[0]=color[0]*255/112;color[1]=color[1]*255/112;color[2]=color[2]*255/48;
}
#if LINNET_COVERFLOW_SCANLINE
#include "services/music/coverflow_scanline.inc"
#else
void ln_coverflow_draw(const struct ln_ui_canvas_context *c){
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t began=ln_time_us();
#endif
    uint16_t painted_top[LINNET_UI_WIDTH],painted_end[LINNET_UI_WIDTH];
    for(uint32_t x=0;x<LINNET_UI_WIDTH;x++){painted_top[x]=LINNET_UI_HEIGHT;painted_end[x]=0;}
    int32_t center=(position+128)/256;
    for(int32_t layer=0;layer<=3;layer++)for(int32_t side=1;side>=-1;side-=2){
        if(!layer&&side==1)continue;
        int32_t index=center+layer*side;if(index<0||(uint32_t)index>=count)continue;
        uint8_t *r=row((uint32_t)index);
        uint32_t art=r?ln_read_le32(r+248):NONE;
        int32_t distance=index*256-position,ad=distance<0?-distance:distance;
        int32_t pose=ad>=256?32:(ad+4)/8,width=ln_coverflow_widths[pose];
        const uint16_t *tilted=pose==32?tilted_pixels(art):0;
        int32_t cached=r&&!tilted?find_art(art):-1;
        if(!tilted&&cached<0&&r&&layer<2)cached=decode_art(art);
        const uint16_t *pixels=cached>=0?cache_pixels[cached]:0;
        const uint8_t *indexed=!tilted&&!pixels&&r?retained_art(art):0;
        const uint16_t *palette=(const uint16_t*)indexed;
        if(cached>=0)cache[cached].age=++age;

        int32_t xcenter=LINNET_UI_WIDTH/2+(distance<0?-1:1)*(ad<=256?ad*69/256:69+(ad-256)*25/256)*(int32_t)LINNET_UI_WIDTH/220;
        for(int32_t x=0;x<width;x++){
            int32_t dx=xcenter-width/2+x;if(dx<c->clip_x||dx>=c->clip_x+c->clip_w)continue;
            const struct ln_coverflow_column *column=&ln_coverflow_columns[pose][distance<0?x:width-1-x];
            int32_t sx=distance<0?column->source:95-column->source;
            const uint16_t *source=pixels?pixels+sx*128:0;
            int32_t first=c->clip_y-column->top;if(first<0)first=0;
            int32_t end=c->clip_y+c->clip_h-column->top;if(end>column->length)end=column->length;
            if(first>=end)continue;
            int32_t ends[4]={first,end,0,0};
            if(painted_end[dx]>painted_top[dx]){
                int32_t hidden_top=painted_top[dx]-column->top,hidden_end=painted_end[dx]-column->top;
                if(ends[1]>hidden_top)ends[1]=hidden_top;
                ends[2]=first>hidden_end?first:hidden_end;ends[3]=end;
            }
            if(column->top+first<painted_top[dx])painted_top[dx]=(uint16_t)(column->top+first);
            if(column->top+end>painted_end[dx])painted_end[dx]=(uint16_t)(column->top+end);
            const uint8_t *indices=indexed?indexed+512+sx*128:0;
            for(uint32_t segment=0;segment<4;segment+=2){
                int32_t from=ends[segment],to=ends[segment+1];if(from>=to)continue;
                uint16_t *dest=c->pixels+(column->top+from)*LINNET_UI_WIDTH+dx;
                uint32_t step=column->step,sy=(step>>1)+(uint32_t)from*step;
                if(tilted){
                    const uint16_t *prepared=tilted+(distance>=0?TILTED_STRIDE*TILTED_HEIGHT:0u)+
                        (uint32_t)x*TILTED_HEIGHT+column->top+(uint32_t)from;
                    for(int32_t y=from;y<to;y++,dest+=LINNET_UI_WIDTH)*dest=*prepared++;
                }else if(source){
                    for(int32_t y=from;y<to;y++,sy+=step,dest+=LINNET_UI_WIDTH)*dest=source[sy>>16];
                }else if(indices){
                    for(int32_t y=from;y<to;y++,sy+=step,dest+=LINNET_UI_WIDTH)*dest=palette[indices[sy>>16]];
                }else if(r){
                    const uint8_t *preview=r+256+(sx/4)*32;
                    for(int32_t y=from;y<to;y++,sy+=step,dest+=LINNET_UI_WIDTH)
                        *dest=ln_read_le16(header+512+2u*preview[sy>>18]);
                }else{
                    for(int32_t y=from;y<to;y++,sy+=step,dest+=LINNET_UI_WIDTH)*dest=sy<(96u<<16)?0x18e4u:0u;
                }
            }
        }
    }
    /* Paint only exposed background; the host supplies the soft halo mask. */
    if(count){
        uint32_t left=(uint32_t)position/256u,blend=(uint32_t)position&255u;
        uint32_t a[3],b[3];ambient_color(left,a);ambient_color(left+1u<count?left+1u:left,b);
        for(uint32_t channel=0;channel<3;channel++)a[channel]=(a[channel]*(256u-blend)+b[channel]*blend)>>8;
        int32_t mean=(int32_t)(a[0]+a[1]+a[2])/3;
        for(uint32_t channel=0;channel<3;channel++){
            int32_t value=mean+((int32_t)a[channel]-mean)*3/2;
            a[channel]=value<0?0u:value>255?255u:(uint32_t)value;
        }
        uint16_t colors[256];
        uint32_t rs=a[0]*70u*31u*2048u/(63u*100u*255u);
        uint32_t gs=a[1]*70u*63u*2048u/(63u*100u*255u);
        uint32_t bs=a[2]*70u*31u*2048u/(63u*100u*255u);
        for(uint32_t k=0;k<64;k++){
            uint32_t r=rs*k,g=gs*k,b=bs*k;
            for(uint32_t d=0;d<4;d++){
                uint32_t bias=d*512u+256u;
                colors[k*4+d]=(uint16_t)(((r+bias)>>11)<<11|((g+bias)>>11)<<5|((b+bias)>>11));
            }
        }
        int32_t bottom=c->clip_y+c->clip_h;if(bottom>(int32_t)(LINNET_UI_HEIGHT*140u/176u))bottom=LINNET_UI_HEIGHT*140u/176u;
        for(int32_t x=c->clip_x;x<c->clip_x+c->clip_w;x++){
            int32_t spans[4]={c->clip_y,painted_top[x],painted_end[x],bottom};
            if(painted_end[x]<=painted_top[x]){spans[1]=bottom;spans[2]=bottom;}
            for(uint32_t part=0;part<4;part+=2){
                int32_t from=spans[part],to=spans[part+1];
                if(from<c->clip_y)from=c->clip_y;if(to>bottom)to=bottom;if(from>=to)continue;
                uint16_t *dest=c->pixels+from*LINNET_UI_WIDTH+x;
                const uint8_t *mask=ln_coverflow_glow[x]+from;
                for(int32_t y=from;y<to;y++,dest+=LINNET_UI_WIDTH)*dest=colors[*mask++];
            }
        }
    }

#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-began;
    ++render_calls;render_us+=elapsed;if(elapsed>render_max_us)render_max_us=elapsed;
#endif
}
#endif

#else
void ln_coverflow_draw(const struct ln_ui_canvas_context *c){(void)c;}
#endif
