#include "hal/api/display.h"
#include "hal/pp502x/display_internal.h"
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "boot/include/linnet_boot.h"
#include "targets/ipod4g-q21/lcd.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MB(o) REG(LINNET_DISPLAY_MAILBOX+(o))

static uint16_t frame[LINNET_DISPLAY_PIXELS] __attribute__((aligned(32)));
static bool initialized,dirty,pending,direct_frame,clock_park_owned,cop_capable;
static bool startup_rewake_armed,startup_rewake_done;
#define BLOCKS_PER_ROW (LINNET_LCD_WIDTH/8u)
#define ALL_BLOCKS ((1u<<BLOCKS_PER_ROW)-1u)
static uint32_t damage[LINNET_LCD_HEIGHT],active[LINNET_LCD_HEIGHT];
static uint16_t sent[LINNET_LCD_HEIGHT*BLOCKS_PER_ROW];
static bool force_full=true;
static bool retrying;
static uint32_t cursor,submitted,completed,failures,resets,generation;
static uint32_t last_write,written_blocks,started;
static void damage_all(void)
{
    for(uint32_t y=0;y<LINNET_LCD_HEIGHT;++y)damage[y]=ALL_BLOCKS;
    dirty=true;
}
static uint32_t cop_error,startup_rewake_at;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t max_present_us;
#endif

static void barrier(void){__asm__ volatile("":::"memory");}

#include "hal/pp502x/cop_control.inc"

static uint32_t weighted_luma(uint16_t color)
{
    /* Exact weighted RGB565 conversion, with constant component contributions. */
    static const uint16_t red[32]={0,633,1266,1900,2533,3166,3800,4433,5067,5700,6333,6967,7600,8234,8867,9500,10134,10767,11400,12034,12667,13301,13934,14567,15201,15834,16468,17101,17734,18368,19001,19635};
    static const uint16_t green[64]={0,607,1214,1821,2428,3035,3642,4250,4857,5464,6071,6678,7285,7892,8500,9107,9714,10321,10928,11535,12142,12750,13357,13964,14571,15178,15785,16392,17000,17607,18214,18821,19428,20035,20642,21250,21857,22464,23071,23678,24285,24892,25500,26107,26714,27321,27928,28535,29142,29750,30357,30964,31571,32178,32785,33392,34000,34607,35214,35821,36428,37035,37642,38250};
    static const uint16_t blue[32]={0,238,477,715,954,1192,1431,1669,1908,2146,2385,2624,2862,3101,3339,3578,3816,4055,4293,4532,4770,5009,5248,5486,5725,5963,6202,6440,6679,6917,7156,7395};
    return (red[(color>>11)&31u]+green[(color>>5)&63u]+blue[color&31u]);
}

static uint8_t physical_gray(uint16_t color)
{
    /* Most menu pixels are black or white. Keep other RGB565 values exactly
     * equivalent to the previous conversion, without multiply/divide per pixel. */
    if(!color)return 3u;
    if(color==0xffffu)return 0u;
    if(color==0x52aau)return 2u;
    if(color==0xad55u)return 1u;
    uint32_t value=weighted_luma(color);
    return value<11008u?3u:value<32768u?2u:value<54528u?1u:0u;
}

bool ln_display_init(void)
{
    initialized=false;cop_error=0;cop_capable=cop_probe();
    if(!pause_worker(true))return false;
    generation=MB(LN_DISPLAY_MAGIC)==LINNET_DISPLAY_MAGIC?MB(LN_DISPLAY_GENERATION)+1u:1u;
    MB(LN_DISPLAY_ENABLED)=0;
    for(uint32_t offset=0;offset<120u;offset+=4u)
        if(offset!=LN_DISPLAY_PAUSE&&offset!=LN_DISPLAY_PAUSED)MB(offset)=0;
    MB(LN_DISPLAY_GENERATION)=generation;MB(LN_DISPLAY_MAGIC)=LINNET_DISPLAY_MAGIC;
    for(uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i)frame[i]=0;
    force_full=true;damage_all();pending=direct_frame=false;startup_rewake_armed=startup_rewake_done=false;
    cursor=submitted=completed=failures=resets=startup_rewake_at=written_blocks=0;
    if(!ln_q21_lcd_init())return false;
    initialized=true;
    if(!pause_worker(false)){initialized=false;return false;}
    return true;
}

uint32_t ln_display_init_error(void){return initialized?0u:0x21000000u|cop_error;}
uint16_t *ln_display_framebuffer(void){return direct_frame?0:frame;}
uint16_t *ln_display_direct_framebuffer(void)
{
    if(!initialized||pending||direct_frame)return 0;
    direct_frame=true;damage_all();return frame;
}
bool ln_display_damage(int32_t x,int32_t y,int32_t width,int32_t height)
{
    if(!initialized||width<=0||height<=0||x>=(int32_t)LINNET_UI_WIDTH||y>=(int32_t)LINNET_UI_HEIGHT)return false;
    if(x<0){if(x<=-width)return false;width+=x;x=0;}
    if(y<0){if(y<=-height)return false;height+=y;y=0;}
    if(width>(int32_t)LINNET_UI_WIDTH-x)width=(int32_t)LINNET_UI_WIDTH-x;
    if(height>(int32_t)LINNET_UI_HEIGHT-y)height=(int32_t)LINNET_UI_HEIGHT-y;
    uint32_t first=(uint32_t)x/8u,end=((uint32_t)(x+width)+7u)/8u;
    uint32_t mask=((1u<<end)-1u)&~((1u<<first)-1u);
    for(uint32_t row=(uint32_t)y;row<(uint32_t)(y+height);++row)damage[row]|=mask;
    dirty=true;return true;
}
void ln_display_column_mode(bool enabled){(void)enabled;}
bool ln_display_upgrade_blit(void){return false;}

bool ln_display_present(void)
{
    if(!initialized||!dirty||pending)return false;
    for(uint32_t y=0;y<LINNET_LCD_HEIGHT;++y){active[y]=damage[y];damage[y]=0;}
    pending=true;dirty=false;direct_frame=false;retrying=false;cursor=0;last_write=UINT32_MAX;++submitted;
    started=ln_time_us();
    return true;
}

bool ln_display_service(uint32_t max_blocks)
{
    if(!initialized)return false;
    if(!pending){
        /* The HD66753 can acknowledge early OS writes while retaining the ROM
         * Apple frame on the glass.  Once the first complete Linnet frame has
         * settled, wake the controller once more and replay that framebuffer.
         * This is confined to the monochrome target and ordinary later frames
         * never pay the reset cost. */
        if(startup_rewake_armed&&(int32_t)(ln_time_us()-startup_rewake_at)>=0){
            startup_rewake_armed=false;startup_rewake_done=true;
            if(!ln_display_reset()||!ln_display_present()){++failures;return false;}
        }else return true;
    }
    if(ln_time_us()-started>=500000u)goto failed;
    uint32_t service_started=ln_time_us();
    if(max_blocks>256u)max_blocks=256u;
    const uint32_t blocks_per_row=LINNET_LCD_WIDTH/8u;
    const uint32_t total=blocks_per_row*LINNET_LCD_HEIGHT;
    while(max_blocks--&&cursor<total){
        uint32_t py=cursor/blocks_per_row,bx=cursor%blocks_per_row;
        if(ln_time_us()-service_started>=2000u)break;
        uint32_t remaining=active[py]& (ALL_BLOCKS<<bx);
        if(!remaining){cursor=(py+1u)*blocks_per_row;continue;}
        /* Jump directly to the next damaged block; ARM7 has no CLZ instruction. */
        while(!(remaining&(1u<<bx)))++bx;
        cursor=py*blocks_per_row+bx;
        /* Native grayscale artwork and UI share this palette. Fixed shifts
         * pack eight pixels without per-pixel coordinate/shift arithmetic. */
        const uint16_t *source=frame+py*LINNET_UI_WIDTH+bx*8u;
        uint16_t packed=(uint16_t)((physical_gray(source[0])<<14)|(physical_gray(source[1])<<12)|
            (physical_gray(source[2])<<10)|(physical_gray(source[3])<<8)|
            (physical_gray(source[4])<<6)|(physical_gray(source[5])<<4)|
            (physical_gray(source[6])<<2)|physical_gray(source[7]));
        if(force_full||sent[cursor]!=packed){
            if((!bx||last_write==UINT32_MAX||last_write+1u!=cursor)&&
               !ln_q21_lcd_begin_span(py,bx))goto failed;
            if(!ln_q21_lcd_write((uint8_t)(packed>>8),(uint8_t)packed))goto failed;
            sent[cursor]=packed;last_write=cursor;++written_blocks;
        }
        ++cursor;
    }
    if(cursor==total){
        /* Completion must include the final bridge write before idle clocks. */
        if(!ln_q21_lcd_flush())goto failed;
        pending=false;force_full=false;++completed;
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t elapsed=ln_time_us()-started;
        if(elapsed>max_present_us)max_present_us=elapsed;
#endif
        if(!startup_rewake_done&&!startup_rewake_armed){
            startup_rewake_armed=true;startup_rewake_at=ln_time_us()+500000u;
        }
    }
    return true;
failed:
    /* Keep the same submission pending during one full replay. A second
     * failure is terminal, so a broken controller cannot trap the runtime. */
    if(!retrying){
        retrying=true;++failures;
        if(ln_q21_lcd_reinitialize()){
            ++resets;++generation;MB(LN_DISPLAY_GENERATION)=generation;
            for(uint32_t y=0;y<LINNET_LCD_HEIGHT;++y)active[y]=ALL_BLOCKS;
            force_full=true;cursor=0;last_write=UINT32_MAX;started=ln_time_us();
            return true;
        }
    }
    pending=false;force_full=true;damage_all();++failures;return false;
}

void ln_display_sync_irq(void){}
bool ln_display_busy(void){return pending;}
bool ln_display_cop_capable(void){return false;}
bool ln_display_quiesce(void)
{
    if(!initialized)return true;
    pending=false;MB(LN_DISPLAY_ENABLED)=0;
    return pause_worker(true);
}
bool ln_display_reset(void)
{
    if(!initialized||!ln_display_quiesce())return false;
    if(!ln_q21_lcd_reinitialize())return false;
    ++resets;++generation;MB(LN_DISPLAY_GENERATION)=generation;force_full=true;damage_all();
    return pause_worker(false);
}
bool ln_display_diagnostic(uint32_t pattern)
{
#if !defined(LINNET_PROFILE_RELEASE)
    /* Link diagnostic 1 exercises the ordinary frame-timeout recovery path. */
    if(pattern==1u){
        if(!initialized||pending||dirty||direct_frame)return false;
        damage_all();
        if(!ln_display_present())return false;
        started=ln_time_us()-500000u;return true;
    }
#endif
    if(pattern)return false;
    /* A direct panel diagnostic has no queued completion record.  Admit it
     * only between ordinary frames so it cannot cancel a submitted frame or
     * leave presentation telemetry permanently out of balance. */
    if(!initialized||pending||dirty||direct_frame||!ln_display_quiesce())return false;
    bool ok=ln_q21_lcd_diagnostic();
    force_full=true;damage_all();
    return pause_worker(false)&&ok;
}
void ln_display_presentation_snapshot(struct ln_display_presentation *output)
{
    if(output)*output=(struct ln_display_presentation){submitted,completed,failures,resets,generation};
}
void ln_display_get_metrics(struct ln_display_metrics *output)
{
    if(output)*output=(struct ln_display_metrics){submitted,completed,completed,
        written_blocks*4u,1u,failures,failures?1u:0u,resets,
        generation,pending?1u:0u,0u,0u,
#if !defined(LINNET_PROFILE_RELEASE)
        max_present_us,
#else
        0,
#endif
        submitted,completed};
}
const uint16_t *ln_display_snapshot(void){return direct_frame?0:frame;}
uint32_t ln_display_snapshot_crc32(void)
{
    const uint16_t *value=ln_display_snapshot();
    return value?ln_crc32((const uint8_t *)value,LINNET_DISPLAY_BYTES):0;
}
size_t ln_display_capture(uint8_t *output,size_t capacity)
{
    if(!initialized||direct_frame||!output||capacity<LINNET_DISPLAY_BYTES)return 0;
    for(uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i){output[i*2u]=(uint8_t)frame[i];output[i*2u+1u]=(uint8_t)(frame[i]>>8);}
    return LINNET_DISPLAY_BYTES;
}
#if !defined(LINNET_PROFILE_RELEASE)
uint32_t ln_display_column_mismatches(void){return 0;}
void ln_display_sync_diagnostics(uint32_t output[16])
{
    if(!output)return;uint32_t values[16]={0x31595344u,2u,completed,0,0,0,pending,pending,
        submitted,completed,failures,written_blocks*4u,max_present_us,
        pending?ln_time_us()-started:0,max_present_us,ln_time_us()};
    for(uint32_t i=0;i<16u;++i)output[i]=values[i];
}
void ln_display_blit_diagnostics(uint32_t output[16])
{
    if(!output)return;for(uint32_t i=0;i<16u;++i)output[i]=0;output[0]=0x314c4251u;
}
int ln_recovery_presentation_stop(void){return ln_display_quiesce()?1:0;}
#endif

uint32_t ln_display_target_diagnostics(uint32_t selector,uint32_t out[32]){
    (void)selector;(void)out;return 0;
}
