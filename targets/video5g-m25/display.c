#include "hal/api/display.h"
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "hal/api/audio.h"
#include "hal/api/power.h"
#include "hal/pp502x/display_internal.h"
#include "targets/video5g-m25/lcd.h"
#include "boot/include/linnet_boot.h"
#if defined(LINNET_IMAGE_OS)
#include "targets/video5g-m25/coprocessor.h"
#include "services/music/player.h"
#if !defined(LINNET_PROFILE_RELEASE) && defined(LINNET_VIDEO) && defined(LINNET_AUDIO)
#include "services/music/controller.h"
#include "services/storage/storage.h"
#endif
#include "services/storage/block.h"
#include "services/video/video.h"
#include "services/photos/photos.h"
#include "services/music/coverflow.h"
#include "ui/runtime/ui.h"
#endif
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MB(o) REG(LINNET_DISPLAY_MAILBOX+(o))
static bool cop_capable,clock_park_owned;
static uint32_t cop_error;
static void barrier(void){__asm__ volatile("":::"memory");}
#include "hal/pp502x/cop_control.inc"
static uint16_t draw[LINNET_DISPLAY_PIXELS] __attribute__((aligned(32)));
static const uint16_t *presented=draw;
static bool initialized,dirty,direct,queued;
static uint32_t queued_started,queue_high_water=1u;
/* Keep the horizontal extent too: split menus and thumbnail borders do not
 * own the whole scanline. Queued damage stays separate until acknowledgement.
 * Pixel-pair alignment permits the same bounded BCM host-port writes. */
struct row_damage {uint16_t first,end;};
static struct row_damage dirty_rows[LINNET_UI_HEIGHT],transfer_rows[LINNET_UI_HEIGHT];
#if defined(LINNET_IMAGE_OS)
static struct row_damage previous_rows[LINNET_UI_HEIGHT],active_rows[LINNET_UI_HEIGHT];
#endif
static uint32_t phase,cursor,transfer_end,wait_started,poll_at,error,submitted,completed,failures,resets,generation,pairs;
static uint32_t started __attribute__((unused)); /* Timing is read only by diagnostics. */
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t max_present_us;
static uint32_t transfer_us,transfer_calls,transfer_max_us,last_present_us;
#endif
static void damage_full(void){
    for(uint32_t i=0;i<LINNET_UI_HEIGHT;++i)dirty_rows[i]=(struct row_damage){0,LINNET_UI_WIDTH};
    dirty=true;
}
static void skip_clean_rows(void){
    while(cursor<transfer_end){
        uint32_t row=cursor/LINNET_UI_WIDTH,x=cursor%LINNET_UI_WIDTH;
        struct row_damage r=transfer_rows[row];
        if(r.end<=r.first||x>=r.end){cursor=(row+1u)*LINNET_UI_WIDTH;continue;}
        if(x<r.first)cursor=row*LINNET_UI_WIDTH+r.first;
        return;
    }
}
static void merge_damage(struct row_damage *row,struct row_damage added){
    if(added.end<=added.first)return;
    if(row->end<=row->first){*row=added;return;}
    if(added.first<row->first)row->first=added.first;
    if(added.end>row->end)row->end=added.end;
}
static void prepare_damage(void){
#if defined(LINNET_IMAGE_OS)
    bool native=ln_m25_coprocessor_display_address()!=0;
#endif
    for(uint32_t i=0;i<LINNET_UI_HEIGHT;++i){
        transfer_rows[i]=dirty_rows[i];
#if defined(LINNET_IMAGE_OS)
        /* Back holds the frame before front. Repair both frames' damage;
         * history advances only after acknowledged presentation. */
        active_rows[i]=dirty_rows[i];
        if(native)merge_damage(&transfer_rows[i],previous_rows[i]);
        else previous_rows[i]=(struct row_damage){0,0};
#endif
        dirty_rows[i]=(struct row_damage){0,0};
    }
    cursor=0;transfer_end=LINNET_DISPLAY_PIXELS;skip_clean_rows();
}
bool ln_display_init(void){
    initialized=false;cop_capable=cop_probe();
    if(!pause_worker(true)){error=1;return false;}
    MB(LN_DISPLAY_ENABLED)=0;MB(LN_DISPLAY_STATE)=0;
    if(!pause_worker(false)){error=2;return false;}
    if(!ln_m25_lcd_init()){error=3;return false;}
    ++generation;phase=0;queued=false;damage_full();direct=false;initialized=true;error=0;return true;
}
uint32_t ln_display_init_error(void){return error;}
uint16_t *ln_display_framebuffer(void){return direct?0:draw;}
uint16_t *ln_display_direct_framebuffer(void){
    if(!initialized||phase||direct)return 0;direct=true;damage_full();return draw;
}
bool ln_display_damage(int32_t x,int32_t y,int32_t width,int32_t height){
    if(!initialized||width<=0||height<=0||x>=320||y>=240||x<=-width||y<=-height)return false;
    if(x<0){width+=x;x=0;}if(width>320-x)width=320-x;
    if(y<0){height+=y;y=0;}if(height>240-y)height=240-y;
    struct row_damage added={(uint16_t)((uint32_t)x&~1u),(uint16_t)((uint32_t)(x+width+1)&~1u)};
    for(uint32_t row=(uint32_t)y;row<(uint32_t)(y+height);++row)
        merge_damage(&dirty_rows[row],added);
    dirty=true;return true;
}
bool ln_display_can_render(void){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_owned())return false;
#endif
    if(!phase)return true;
#if defined(LINNET_IMAGE_OS)
    /* PIO has finished in phase 3, so menus can overlap BCM scanout too.
     * The runtime separately gates the first acknowledged frame of a theme. */
    return phase==3u&&!queued&&ln_m25_coprocessor_display_can_overlap();
#else
    return false;
#endif
}
bool ln_display_present(void){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_owned())return false;
#endif
    if(!initialized||!dirty||!ln_display_can_render())return false;
    /* The draw buffer remains untouched while its pixels are transferred.
     * Queued scenes may draw again only after PIO finishes and BCM owns the
     * submitted frame. No second full-frame copy is needed. */
    presented=draw;
    direct=false;dirty=false;++submitted;
    if(phase==3u){queued=true;queued_started=ln_time_us();queue_high_water=2u;return true;}
    prepare_damage();phase=1;started=wait_started=poll_at=ln_time_us();
    /* Start the surface request now, so BCM can prepare it during the next
     * decoder turn. A zero pixel budget cannot upload or change draw memory. */
    return ln_display_service(0);
}
static bool fail(uint32_t code){error=code;++failures;phase=0;queued=false;damage_full();return false;}
bool ln_display_service(uint32_t max_pairs){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_owned())return true;
#endif
    if(!initialized)return false;
    if(!phase)return true;
    bool ready;
    if(phase==1||phase==3){
        if(max_pairs==LINNET_DISPLAY_SERVICE_PAIRS&&(int32_t)(ln_time_us()-poll_at)<0)return true;
        if(phase==1u&&!ln_m25_lcd_prepare())return fail(7);
        if(!ln_m25_lcd_ready(&ready))return fail(5);
        /* A long foreground image hash can postpone this service. Only a
         * still-busy BCM transaction is a timeout, not the CPU's scheduling
         * gap or an already completed LCD update. */
        if(!ready){
            uint32_t now=ln_time_us();poll_at=now+250u;
            return now-wait_started>=500000u?fail(4):true;
        }
        if(phase==3){
#if !defined(LINNET_PROFILE_RELEASE)
            uint32_t elapsed=ln_time_us()-started;
            last_present_us=elapsed;
            if(elapsed>max_present_us)max_present_us=elapsed;
#endif
            phase=0;++completed;
#if defined(LINNET_IMAGE_OS)
            bool native=ln_m25_coprocessor_display_address()!=0;
            for(uint32_t i=0;i<LINNET_UI_HEIGHT;++i)
                previous_rows[i]=native?active_rows[i]:(struct row_damage){0,0};
            ln_m25_coprocessor_display_presented();
#endif
            if(!queued)return true;
            queued=false;prepare_damage();started=queued_started;
#if defined(LINNET_IMAGE_OS)
            if(ln_m25_coprocessor_display_address()){phase=1u;wait_started=poll_at=ln_time_us();return true;}
#endif
        }
        phase=2;
    }
    if(!max_pairs)return true;if(max_pairs>LINNET_DISPLAY_SERVICE_PAIRS)max_pairs=LINNET_DISPLAY_SERVICE_PAIRS;
#if defined(LINNET_IMAGE_OS) && defined(LINNET_AUDIO)
    if(max_pairs==LINNET_DISPLAY_SERVICE_PAIRS){
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        /* A full frame takes about 13 ms with IRQs enabled. Reserve at least
         * half the PCM ring before avoiding decoder gaps between LCD chunks. */
        if(!audio.playing||audio.paused||audio.queued>=LINNET_AUDIO_RING_FRAMES/2u)max_pairs=LINNET_DISPLAY_PIXELS/2u;
    }
#endif
    uint32_t remaining=max_pairs*2u;
    uint32_t transfer_started=ln_time_us();
    do {
        uint32_t row=cursor/LINNET_UI_WIDTH,end=row*LINNET_UI_WIDTH+transfer_rows[row].end;
        /* Coalesce full-width runs, but retain a new address for each partial
         * row. Service several small spans per turn instead of inserting a
         * decoder/scheduler roundtrip between every thumbnail scanline. */
        while(end<transfer_end&&end%LINNET_UI_WIDTH==0u&&end-cursor<remaining){
            struct row_damage next=transfer_rows[end/LINNET_UI_WIDTH];
            if(next.first||!next.end)break;
            end+=next.end;
        }
        uint32_t count=end-cursor;if(count>remaining)count=remaining;
        if(!ln_m25_lcd_write(cursor,presented+cursor,count))return fail(6);
        cursor+=count;pairs+=count/2u;remaining-=count;skip_clean_rows();
    }while(remaining&&cursor<transfer_end&&ln_time_us()-transfer_started<13000u);
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t elapsed=ln_time_us()-transfer_started;++transfer_calls;transfer_us+=elapsed;
    if(elapsed>transfer_max_us)transfer_max_us=elapsed;
#endif
    if(cursor==transfer_end){if(!ln_m25_lcd_commit())return fail(7);phase=3;wait_started=ln_time_us();poll_at=wait_started+250u;}
    return true;
}
bool ln_display_wait_deadline(uint32_t *deadline){
#if defined(LINNET_IMAGE_OS)
    /* BCM playback owns scanout; pending UI damage cannot be drawn yet. */
    if(ln_m25_coprocessor_owned()){
        if(deadline)*deadline=ln_time_us()+1000u;
        return true;
    }
#endif
    if(phase!=1u&&phase!=3u)return false;
    if(deadline)*deadline=poll_at;
    return true;
}
void ln_display_sync_irq(void){}
void ln_display_column_mode(bool enabled){(void)enabled;}
bool ln_display_upgrade_blit(void){return false;}
bool ln_display_busy(void){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_owned())return true;
#endif
    return phase!=0;
}
bool ln_display_cop_capable(void){return false;}
bool ln_display_quiesce(void){
#if defined(LINNET_IMAGE_OS)
    if(ln_m25_coprocessor_owned())return false;
#endif
    uint32_t start=ln_time_us();
    for(uint32_t budget=0;phase&&budget<100000u;++budget){
        if(!ln_display_service(256u))return false;
        if(ln_time_us()-start>=500000u)return false;
    }
    return !phase;
}
bool ln_display_reset(void){
    if(!ln_display_quiesce())return false;++resets;damage_full();direct=false;return ln_m25_lcd_init();
}
bool ln_display_diagnostic(uint32_t pattern){
#if defined(LINNET_IMAGE_OS)
#if !defined(LINNET_PROFILE_RELEASE)
    if(pattern==0x4d250028u){ln_platform_click(0u,4u);return true;}
    if(pattern==0x4d25002bu)return ln_m25_coprocessor_control(6u);
    if(pattern>=0x4d250015u&&pattern<=0x4d250016u)return ln_m25_coprocessor_fault(pattern-0x4d250014u);
#endif
    if(pattern>=0x4d250010u&&pattern<=0x4d250014u)return ln_m25_coprocessor_control(pattern-0x4d25000fu);
    if(pattern>=0x4d250002u&&pattern<=0x4d250004u){
        if(ln_player_busy())return false;
#if defined(LINNET_PHOTOS)
        if(ln_photos_active()||ln_photos_pending())return false;
#endif
#if defined(LINNET_COVERFLOW)
        if(ln_coverflow_active())return false;
#endif
#if defined(LINNET_VIDEO)
        if(ln_video_active()||ln_video_pending())return false;
#endif
        return ln_m25_coprocessor_begin(pattern-0x4d250002u);
    }
#endif
    if(pattern||!ln_display_quiesce())return false;damage_full();return ln_lcd_fill(0x07e0u);
}
void ln_display_presentation_snapshot(struct ln_display_presentation *out){
    if(out)*out=(struct ln_display_presentation){submitted,completed,failures,resets,generation};
}
void ln_display_get_metrics(struct ln_display_metrics *out){
    if(out)*out=(struct ln_display_metrics){submitted,completed,completed,pairs,queue_high_water,failures,error,resets,
        generation,(phase?1u:0u)+(queued?1u:0u),0,0,
#if !defined(LINNET_PROFILE_RELEASE)
        max_present_us,
#else
        0,
#endif
        submitted,completed};
}
const uint16_t *ln_display_snapshot(void){return direct?0:presented;}
uint32_t ln_display_snapshot_crc32(void){return direct?0:ln_crc32((const uint8_t *)presented,LINNET_DISPLAY_BYTES);}
size_t ln_display_capture(uint8_t *out,size_t capacity){
    if(!initialized||direct||!out||capacity<LINNET_DISPLAY_BYTES)return 0;
    for(uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i){out[2*i]=presented[i];out[2*i+1]=presented[i]>>8;}
    return LINNET_DISPLAY_BYTES;
}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_display_sync_diagnostics(uint32_t out[16]){
    if(!out)return;for(uint32_t i=0;i<16u;++i)out[i]=0;
    out[0]=0x35595344u;out[1]=phase;out[2]=cursor;out[3]=error;out[4]=submitted;out[5]=completed;out[6]=failures;
    out[7]=transfer_us;out[8]=transfer_calls;out[9]=transfer_max_us;out[10]=last_present_us;out[11]=max_present_us;
}
void ln_display_blit_diagnostics(uint32_t out[16]){ln_display_sync_diagnostics(out);}
uint32_t ln_display_column_mismatches(void){return 0;}
int ln_recovery_presentation_stop(void){return ln_display_quiesce();}
#endif

uint32_t ln_display_target_diagnostics(uint32_t selector,uint32_t out[32]){
    if(!out)return 0;
#if defined(LINNET_IMAGE_OS)
#if !defined(LINNET_PROFILE_RELEASE) && defined(LINNET_VIDEO) && defined(LINNET_AUDIO)
    if(selector==0x4d25002cu){
        uint32_t now=ln_time_us();out[0]=selector;out[1]=now;
        out[2]=ln_music_deadline(now);out[3]=UINT32_MAX;
#if defined(LINNET_COVERFLOW)
        out[3]=ln_coverflow_io_deadline();
#endif
        out[4]=ln_video_deadline(now);out[5]=ln_video_active();
        out[6]=ln_video_playback_pending();out[7]=ln_storage_busy();return 8u;
    }
#endif
    if(selector==0x4d250028u){out[0]=selector;ln_platform_click_diagnostics(out+1);return 5u;}
    if(selector==0x4d250029u){
        struct ln_power_state p;ln_power_snapshot(&p);
        out[0]=selector;out[1]=p.samples;out[2]=p.failures;out[3]=p.bus_resets;
        out[4]=p.battery_mv;out[5]=p.battery_raw;out[6]=p.charge_limit_ma;out[7]=p.flags;
        out[8]=p.battery_percent;out[9]=p.sleeping;out[10]=p.backlight_on;out[11]=p.brightness;
        return 12u;
    }
    if(selector==0x4d250027u){
        extern void ln_music_library_memory_diagnostics(uint32_t output[4]);
        uint32_t bytes;void *extra=ln_platform_extended_memory(&bytes);
        out[0]=selector;out[1]=32u*1024u*1024u+bytes;out[2]=(uint32_t)(uintptr_t)extra;out[3]=bytes;
        ln_music_library_memory_diagnostics(out+4);ln_block_extended_diagnostics(out+8);return 12u;
    }
    if(selector==0x4d250002u)return ln_m25_coprocessor_snapshot(out);
    if(selector>=0x4d250010u&&selector<0x4d250018u)return ln_m25_coprocessor_channel(selector-0x4d250010u,out);
    if(selector>=0x4d250030u&&selector<0x4d250038u)return ln_m25_coprocessor_channel(16u+selector-0x4d250030u,out);
    if(selector==0x4d250020u)return ln_m25_coprocessor_channel(8u,out);
    if(selector==0x4d250021u)return ln_m25_coprocessor_channel(9u,out);
    if(selector==0x4d250022u)return ln_m25_coprocessor_channel(10u,out);
    if(selector==0x4d250023u)return ln_m25_coprocessor_channel(11u,out);
    if(selector==0x4d250026u)return ln_m25_coprocessor_channel(14u,out);
    if(selector==0x4d25002au)return ln_m25_coprocessor_channel(15u,out);
    if(selector==0x4d250025u)return ln_m25_coprocessor_channel(13u,out);
    if(selector==0x4d250024u)return ln_m25_coprocessor_channel(12u,out);
#endif
    if(selector!=0x4d250001u||!out)return 0;
    for(uint32_t i=0;i<8u;++i)out[i]=0;
    out[0]=selector;out[1]=1u;
    if(!ln_display_quiesce()){out[2]=1u;return 8u;}
    if(!ln_m25_lcd_runtime_header(out+4u))out[2]=2u;
    return 8u;
}
