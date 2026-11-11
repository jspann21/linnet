#include "hal/api/display.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "panel.h"
#if defined(LINNET_IMAGE_OS)
#include "linnet_build.h"
static bool boot_panel_handoff(void) {
    /* Use the same CRC- and image-bound publication as storage confirmation.
     * A RAM launch has no matching publication and must initialize the panel. */
    const volatile uint8_t *record=(const volatile uint8_t *)
        ln_platform_uncached_address(LINNET_MAILBOX_ADDRESS+0x100u);
    uint8_t bytes[80];for(unsigned i=0;i<sizeof bytes;i++)bytes[i]=record[i];
    if(ln_read_le32(bytes)!=0x444e4148u || bytes[4]>1 ||
       ln_crc32(bytes,76)!=ln_read_le32(bytes+76))return false;
    const uint8_t build[32]=LINNET_BUILD_ID_BYTES;
    for(unsigned i=0;i<32;i++)if(bytes[8+i]!=build[i])return false;
    return ln_n25_panel_adopt();
}
#endif
extern uint32_t ln_time_us(void);
extern void ln_memory_copy(void *,const void *,uint32_t);
#ifdef LINNET_SRAM_MONITOR
#define FRAME_MEMORY __attribute__((section(".dram.framebuffers"),aligned(32)))
#else
#define FRAME_MEMORY __attribute__((aligned(32)))
#endif
static uint16_t buffers[2][LINNET_DISPLAY_PIXELS] FRAME_MEMORY;
static uint16_t *draw=buffers[0],*presented=buffers[1];
static bool draw_valid;
static bool initialized,busy,dirty;
static uint32_t submitted,completed,failures,resets,generation,error,started,max_us;
static uint32_t first_row,last_row,active_pairs,pairs_written;
static bool fail(uint32_t reason){error=reason;failures++;busy=false;return false;}
bool ln_display_init(void) {
    initialized=false;busy=false;error=0;
    bool inherited=false;
#if defined(LINNET_IMAGE_OS)
    inherited=boot_panel_handoff();
#endif
    if(!inherited){
        if(!ln_n25_panel_start(false))return fail(1);
        uint32_t deadline=ln_time_us()+2000000u;
        for(;;) {
            uint32_t phase=ln_n25_panel_poll();
            if(phase==3)break;
            if(phase==0xffffffffu || (int32_t)(ln_time_us()-deadline)>=0 || !ln_core_delay_us(100))return fail(2);
        }
    }
    for(unsigned i=0;i<LINNET_DISPLAY_PIXELS;i++)draw[i]=presented[i]=0;
    initialized=true;dirty=true;draw_valid=true;first_row=0;last_row=LINNET_UI_HEIGHT;generation++;
#if defined(LINNET_IMAGE_OS)
    /* Preserve Boot's artwork without a reset/clear flash; a standalone RAM
     * entry presents the same artwork after full panel initialization. */
    if(!ln_lcd_boot())return fail(6);
#endif
    return true;
}
uint32_t ln_display_init_error(void){return error;}
uint16_t *ln_display_framebuffer(void){
    if(!initialized)return 0;
    if(!draw_valid){ln_memory_copy(draw,presented,LINNET_DISPLAY_BYTES);draw_valid=true;}
    return draw;
}
uint16_t *ln_display_direct_framebuffer(void){
    if(!initialized||busy)return 0;
    draw_valid=true;dirty=true;first_row=0;last_row=LINNET_UI_HEIGHT;return draw;
}
bool ln_display_damage(int32_t x,int32_t y,int32_t w,int32_t h) {
    if(!initialized || w<=0 || h<=0 || x>=(int32_t)LINNET_UI_WIDTH || y>=(int32_t)LINNET_UI_HEIGHT ||
       (int64_t)x+w<=0 || (int64_t)y+h<=0)return false;
    uint32_t top=y<0?0u:(uint32_t)y;
    uint32_t bottom=(int64_t)y+h>LINNET_UI_HEIGHT?LINNET_UI_HEIGHT:(uint32_t)((int64_t)y+h);
    if(!dirty){first_row=top;last_row=bottom;}
    else{if(top<first_row)first_row=top;if(bottom>last_row)last_row=bottom;}
    dirty=true;return true;
}
bool ln_display_present(void) {
    if(!initialized || busy || !dirty)return false;
    if(!draw_valid)(void)ln_display_framebuffer();
    uint32_t top=0,bottom=LINNET_UI_HEIGHT;
#if defined(LINNET_IMAGE_OS)
    uint32_t panel=ln_n25_panel_type();
    if((panel==1u||panel==2u)&&first_row<last_row&&last_row<=LINNET_UI_HEIGHT&&last_row-first_row<=120u){
        top=first_row;bottom=last_row;
    }
    if(!ln_n25_panel_present_rows(draw,top,bottom))return fail(3);
#else
    if(!ln_n25_panel_present(draw))return fail(3);
#endif
    active_pairs=(bottom-top)*LINNET_UI_WIDTH/2u;
    uint16_t *previous=presented;presented=draw;draw=previous;draw_valid=false;
    started=ln_time_us();busy=true;dirty=false;first_row=LINNET_UI_HEIGHT;last_row=0;submitted++;return true;
}
bool ln_display_service(uint32_t max_pairs) {
    if(!initialized || error)return false;
    if(!busy || !max_pairs)return true;
    if(max_pairs>256)max_pairs=256;
    uint32_t phase=ln_n25_panel_service(max_pairs*2);
    if(phase==0xffffffffu)return fail(4);
    if(phase==3){busy=false;completed++;pairs_written+=active_pairs;uint32_t elapsed=ln_time_us()-started;if(elapsed>max_us)max_us=elapsed;}
    return true;
}
bool ln_display_busy(void){return busy;}
bool ln_display_quiesce(void) {
    uint32_t deadline=ln_time_us()+2000000u;
    while(busy)if(!ln_display_service(256) || (int32_t)(ln_time_us()-deadline)>=0)return fail(5);
    return !error;
}
bool ln_display_reset(void){if(!ln_display_quiesce())return false;resets++;generation++;dirty=true;first_row=0;last_row=LINNET_UI_HEIGHT;return true;}
int ln_recovery_presentation_stop(void){return ln_display_quiesce();}
void ln_display_column_mode(bool enabled){(void)enabled;}
void ln_display_sync_irq(void){}
bool ln_display_cop_capable(void){return false;}
bool ln_display_upgrade_monitor(void){return false;}
bool ln_display_upgrade_blit(void){return false;}
uint32_t ln_display_cop_error(void){return 1;}
void ln_display_cop_diagnostics(uint32_t out[8]){if(out){for(unsigned i=0;i<8;i++)out[i]=0;out[0]=1;}}
void ln_display_presentation_snapshot(struct ln_display_presentation *out) {
    if(out)*out=(struct ln_display_presentation){submitted,completed,failures,resets,generation};
}
void ln_display_get_metrics(struct ln_display_metrics *out) {
    if(!out)return;
    *out=(struct ln_display_metrics){0};out->frames_submitted=submitted;out->frames_completed=completed;
    out->jobs_completed=completed;out->pairs_written=pairs_written;
    out->queue_high_water=submitted?1:0;out->failures=failures;out->last_error=error;out->resets=resets;
    out->generation=generation;out->queued_jobs=busy;out->max_present_us=max_us;out->head=submitted;out->tail=completed;
}
const uint16_t *ln_display_snapshot(void){return initialized?presented:0;}
uint32_t ln_display_snapshot_crc32(void){return initialized?ln_crc32((const uint8_t *)presented,LINNET_DISPLAY_BYTES):0;}
size_t ln_display_capture(uint8_t *out,size_t capacity) {
    if(!initialized || !out || capacity<LINNET_DISPLAY_BYTES)return 0;
    for(unsigned i=0;i<LINNET_DISPLAY_PIXELS;i++){out[i*2]=(uint8_t)presented[i];out[i*2+1]=(uint8_t)(presented[i]>>8);}
    return LINNET_DISPLAY_BYTES;
}
void ln_display_sync_diagnostics(uint32_t out[16]) {
    if(!out)return;for(unsigned i=0;i<16;i++)out[i]=0;
    out[0]=0x36325344;out[1]=busy;out[2]=error;out[3]=submitted;out[4]=completed;out[5]=max_us;
    out[6]=ln_n25_panel_type();
}
void ln_display_blit_diagnostics(uint32_t out[16]){ln_display_sync_diagnostics(out);}
uint32_t ln_display_column_mismatches(void){return 0;}
uint32_t ln_display_target_diagnostics(uint32_t selector,uint32_t out[32]) {
    if(selector!=0x36325344 || !out)return 0;
    ln_display_sync_diagnostics(out);return 16;
}
