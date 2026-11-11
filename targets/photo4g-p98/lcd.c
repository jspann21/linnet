#include "hal/api/platform.h"
#include "hal/api/core.h"
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#endif
#include "targets/photo4g-p98/disk_screen_runs.h"
#if defined(LINNET_IMAGE_BOOT)
#include "targets/photo4g-p98/boot_wordmark.h"
#include "targets/photo4g-p98/boot_menu_font.h"
#endif

/* Register addresses and bus protocol provenance: docs/architecture.md. */
#define LINNET_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define LINNET_LCD_PORT LINNET_REG(0x70008a0cu)
#define LINNET_LCD_CONTROL LINNET_REG(0x70008a20u)
#define LINNET_LCD_CONFIG LINNET_REG(0x70008a24u)
#define LINNET_LCD_DATA LINNET_REG(0x70008b00u)
/* Physical P98 diagnostic: a complete 19,360-pair frame took 109,382 us with
 * only nine unsuccessful polls. The former 100 ms limit could therefore fail
 * on an incidental late FIFO/READY poll. Allow 250 ms for the complete frame,
 * below the independent 500 ms watchdog; do not refresh watchdog progress here. */
#define LINNET_LCD_FRAME_TIMEOUT_US 250000u
static bool ln_panel_ready;
static bool ln_disk_symbol_visible = true;
static uint16_t ln_disk_blink_pixel(uint16_t bus_color)
{
    uint16_t color = (uint16_t)((bus_color << 8) | (bus_color >> 8));
    /* The symbol is the only warm tint in the generated artwork. */
    if ((((color >> 11) & 31u) * 2u) > (((color >> 5) & 63u) + 2u))
        return 0xdfffu; /* Byte-swapped RGB565 paper (0xffdf). */
    return bus_color;
}
#if defined(LINNET_IMAGE_BOOT)
static uint32_t ln_text_pairs[LINNET_LCD_WIDTH*LINNET_LCD_HEIGHT/2u];
#endif
uint32_t ln_lcd_panel_type(void)
{
    static uint32_t panel;
    static bool detected;
    if (!detected) {
        /* Original Photo revision 0x60000 has no reliable panel straps.
         * Map its physical boot ROM into the CPU-only ROM window before
         * reading the revision; Linnet normally maps only SDRAM at zero. */
        uint32_t logical = LINNET_REG(0xf000f008u);
        uint32_t physical = LINNET_REG(0xf000f00cu);
        LINNET_REG(0xf000f008u) = 0x20003c00u;
        LINNET_REG(0xf000f00cu) = 0x00003f84u;
        __asm__ volatile("" ::: "memory");
        uint32_t revision = LINNET_REG(0x20002084u);
        LINNET_REG(0xf000f008u) = logical;
        LINNET_REG(0xf000f00cu) = physical;
        __asm__ volatile("" ::: "memory");
        uint32_t gpio = LINNET_REG(0x6000d030u);
        panel = revision == 0x60000u ? 0u : (gpio & 2u) | ((gpio & 0x10u) >> 4);
        detected = true;
    }
    return panel;
}
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t ln_lcd_trace[8];
void ln_lcd_diagnostics(uint32_t output[8])
{
    if (output)
        for (uint32_t i = 0; i < 8u; ++i) output[i] = ln_lcd_trace[i];
}
#define LINNET_LCD_RECORD(index, value) (ln_lcd_trace[(index)] = (value))
#else
#define LINNET_LCD_RECORD(index, value) ((void)0)
#endif

extern uint32_t ln_time_us(void);

/* Both elapsed time and instruction-count bounds: a stopped timer cannot hang
 * an LCD operation. One budget covers the complete frame, including commands. */
static bool ln_wait(volatile uint32_t *reg, uint32_t mask, bool set,
                    uint32_t started, uint32_t timeout_us, uint32_t *budget)
{
    while (((*reg & mask) != 0u) != set) {
        if (*budget == 0u || ln_time_us() - started >= timeout_us) return false;
        --*budget;
    }
    return true;
}

static bool ln_lcd_wait(volatile uint32_t *reg, uint32_t mask, bool set,
                        uint32_t started, uint32_t *budget)
{
    if (ln_wait(reg, mask, set, started, LINNET_LCD_FRAME_TIMEOUT_US, budget)) return true;
    LINNET_LCD_RECORD(1, *budget == 0u ? 2u : 1u);
    return false;
}

static void ln_lcd_record_end(uint32_t started, uint32_t budget, uint32_t pairs)
{
#if !defined(LINNET_PROFILE_RELEASE)
    ln_lcd_trace[2] = ln_time_us() - started;
    ln_lcd_trace[3] = pairs;
    ln_lcd_trace[4] = budget;
    /* Read status before failure cleanup cancels the active controller block. */
    ln_lcd_trace[5] = LINNET_LCD_CONTROL;
    ln_lcd_trace[6] = LINNET_LCD_CONFIG;
    ln_lcd_trace[7] = LINNET_LCD_PORT;
#else
    (void)started;
    (void)budget;
    (void)pairs;
#endif
}

static bool ln_lcd_command(uint32_t command, uint32_t value,
                           uint32_t started, uint32_t *budget)
{
    LINNET_LCD_RECORD(0, 3u | (command << 16));
    if (!ln_lcd_wait(&LINNET_LCD_PORT, 0x80000000u, false, started, budget)) return false;
    if (ln_lcd_panel_type() & 1u) {
        LINNET_LCD_PORT = 0x80000000u;
        LINNET_LCD_PORT = 0x80000000u | command;
        if (!ln_lcd_wait(&LINNET_LCD_PORT, 0x80000000u, false, started, budget)) return false;
        LINNET_LCD_PORT = 0x81000000u | (value >> 8);
        LINNET_LCD_PORT = 0x81000000u | (value & 255u);
        return true;
    }
    LINNET_LCD_PORT = 0x80000000u | command;
    if (!ln_lcd_wait(&LINNET_LCD_PORT, 0x80000000u, false, started, budget)) return false;
    LINNET_LCD_PORT = 0x80000000u | value;
    return true;
}

/* Self-contained 5x7 uppercase glyphs: no UI, heap, storage or font pointer
 * is needed when the application itself has faulted. Rows are bit masks. */
static uint16_t ln_fault_pixel(const char lines[8][33],uint32_t x,uint32_t y)
{
    static const uint8_t glyphs[36][7]={
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
        {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
        {16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},
        {30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,27,17},
        {17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
    const uint16_t background=0x1800u;
    if(x<14u||x>=206u||y<8u||y>=168u)return background;
    x-=14u;y-=8u;uint32_t line=y/20u,row=(y%20u)/2u,column=x/6u,bit=x%6u;
    if(row>=7u||bit>=5u)return background;
    uint8_t c=(uint8_t)lines[line][column],mask=0;
    if(c>='0'&&c<='9')mask=glyphs[c-'0'][row];
    else if(c>='A'&&c<='Z')mask=glyphs[c-'A'+10u][row];
    else if(c=='+')mask=row==3u?14u:(row==2u||row==4u?4u:0u);
    else if(c==':')mask=row==2u||row==5u?4u:0u;
    return mask&(16u>>bit)?0xffffu:background;
}

static bool ln_lcd_draw(uint16_t color, const char (*fault_rows)[33], const uint16_t *runs,
    const uint32_t *palette,const uint32_t *prepared_pairs)
{
    bool disk_runs = runs == ln_disk_runs;
#if defined(LINNET_IMAGE_BOOT)
    if(fault_rows&&!prepared_pairs){
        /* Render text before opening the LCD transaction. The panel's FIFO
         * deadline then covers only transfer, as it does for the Boot art. */
        for(uint32_t y=0;y<LINNET_LCD_HEIGHT;++y){
            for(uint32_t x=0;x<LINNET_LCD_WIDTH;x+=2u){
                uint16_t a=ln_fault_pixel(fault_rows,x,y);
                uint16_t b=ln_fault_pixel(fault_rows,x+1u,y);
                ln_text_pairs[(y*LINNET_LCD_WIDTH+x)/2u]=
                    (uint32_t)(uint16_t)((a<<8)|(a>>8))|
                    ((uint32_t)(uint16_t)((b<<8)|(b>>8))<<16);
            }
#if !defined(LINNET_PROFILE_RELEASE)
            ln_recovery_feed();
#else
            ln_core_watchdog_feed();
#endif
        }
    }
#endif
    uint32_t started = ln_time_us(), budget = 1000000u;
    uint32_t pairs_written = 0;
    LINNET_LCD_RECORD(0, 0u);
    LINNET_LCD_RECORD(1, 0u);
    if (!ln_panel_ready) {
        LINNET_LCD_RECORD(0, 1u);
        LINNET_REG(0x6000600cu) |= 0x04000000u;
        LINNET_LCD_CONFIG = 0;
        LINNET_REG(0x60006004u) |= 0x04000000u;
        uint32_t reset_started = ln_time_us();
        for (uint32_t i = 0; ln_time_us() - reset_started < 1u; ++i)
            if (i == 99999u) { LINNET_LCD_RECORD(1, 3u); goto failed; }
        LINNET_REG(0x60006004u) &= ~0x04000000u;
        uint32_t delay_start = ln_time_us();
        LINNET_LCD_RECORD(0, 2u);
        for (uint32_t i = 0; ln_time_us() - delay_start < 1000u; ++i)
            if (i == 999999u) { LINNET_LCD_RECORD(1, 3u); goto failed; }
        static const uint16_t registers[8][2] = {
            {0xefu,0}, {1u,0}, {0x80u,1u}, {0x10u,0xcu},
            {0x18u,6u}, {0x7eu,4u}, {0x7eu,5u}, {0x7fu,1u}
        };
        for (uint32_t i = 0; !(ln_lcd_panel_type() & 1u) && i < 8u; ++i)
            if (!ln_lcd_command(registers[i][0], registers[i][1], started, &budget)) goto failed;
        /* PP5020 atomic GPIO alias packs mask in high byte, value in low. */
        LINNET_REG(0x6000d804u) = 0x0c0cu;
        LINNET_REG(0x6000d824u) = 0x0808u;
        LINNET_REG(0x70000084u) &= ~0x02000000u;
        LINNET_REG(0x6000600cu) |= 0x00020000u;
        LINNET_REG(0x7000a010u) = 0x80640000u;
        ln_panel_ready = true;
    }
    /* Program the full panel window once. The panel cursor continues across
     * bridge blocks; restarting it for every row adds 176 command/flush cycles.
     * PP5020 blocks have a 16-bit byte count, so 220x176 needs 148 + 28 rows. */
    if (ln_lcd_panel_type() & 1u) {
        if (!ln_lcd_command(0x44u, (LINNET_LCD_HEIGHT - 1u) << 8, started, &budget) ||
            !ln_lcd_command(0x45u, (LINNET_LCD_WIDTH - 1u) << 8, started, &budget) ||
            !ln_lcd_command(0x21u, (LINNET_LCD_WIDTH - 1u) << 8, started, &budget)) goto failed;
        if (!ln_lcd_wait(&LINNET_LCD_PORT, 0x80000000u, false, started, &budget)) goto failed;
        LINNET_LCD_PORT = 0x80000000u;
        LINNET_LCD_PORT = 0x80000022u;
    } else if (!ln_lcd_command(0x18u, 6u, started, &budget) ||
        !ln_lcd_command(0x12u, 0u, started, &budget) ||
        !ln_lcd_command(0x13u, LINNET_LCD_WIDTH - 1u, started, &budget) ||
        !ln_lcd_command(0x15u, LINNET_LCD_HEIGHT - 1u, started, &budget) ||
        !ln_lcd_command(0x16u, 0u, started, &budget)) goto failed;
    /* The old Photo panel consumes byte-swapped RGB565 on this bus. */
    uint16_t bus_color = (uint16_t)((color << 8) | (color >> 8));
    uint32_t pixel_pair = (uint32_t)bus_color | ((uint32_t)bus_color << 16);
    uint32_t rows_left = LINNET_LCD_HEIGHT;
    uint32_t run_left = 0;
    bool fast_pixels=!fault_rows||prepared_pairs;
#if defined(LINNET_IMAGE_BOOT)
    if(fault_rows)fast_pixels=true;
#endif
    while (rows_left != 0u) {
        uint32_t rows = rows_left > 148u ? 148u : rows_left;
        uint32_t bytes = rows * LINNET_LCD_WIDTH * 2u;
        if (!ln_lcd_wait(&LINNET_LCD_PORT, 0x80000000u, false, started, &budget)) goto failed;
        LINNET_LCD_CONTROL = 0x10000080u;
        LINNET_LCD_CONFIG = 0xc0010000u | (bytes - 1u);
        LINNET_LCD_CONTROL = 0x34000000u;
        LINNET_LCD_RECORD(0, 5u | ((rows_left == LINNET_LCD_HEIGHT ? 1u : 2u) << 8));
        uint32_t pair = 0;
        while (pair < bytes / 4u) {
            uint32_t span = bytes / 4u - pair;
            if (runs) {
                if (!run_left) {
                    uint16_t run = *runs++;
                    run_left = run & 255u;
                    pixel_pair = palette[run >> 8];
                    if (disk_runs && !ln_disk_symbol_visible) {
                        uint16_t first = ln_disk_blink_pixel((uint16_t)pixel_pair);
                        uint16_t second = ln_disk_blink_pixel((uint16_t)(pixel_pair >> 16));
                        pixel_pair = (uint32_t)first | ((uint32_t)second << 16);
                    }
                }
                if (span > run_left) span = run_left;
                run_left -= span;
            }
            uint32_t end = pair + span;
            if (fast_pixels) {
                /* Keep span decoding and deadline reads outside the FIFO loop.
                 * Bound every chunk by the original 256-pair deadline boundary. */
                while (pair < end) {
                    uint32_t chunk_end = (pair | 255u) + 1u;
                    if (chunk_end > end) chunk_end = end;
                    if (ln_time_us() - started >= LINNET_LCD_FRAME_TIMEOUT_US) {
                        LINNET_LCD_RECORD(1, 1u);
                        goto failed;
                    }
                    for (; pair < chunk_end; ++pair) {
                        /* Avoid out-of-line polling overhead for a ready FIFO,
                         * especially while Boot runs from uncached SDRAM. */
                        if (!(LINNET_LCD_CONTROL & 0x01000000u) &&
                            !ln_lcd_wait(&LINNET_LCD_CONTROL, 0x01000000u, true, started, &budget)) goto failed;
                        LINNET_LCD_DATA =
#if defined(LINNET_IMAGE_BOOT)
                            prepared_pairs?prepared_pairs[((LINNET_LCD_HEIGHT-rows_left)*LINNET_LCD_WIDTH)/2u+pair]:
                            fault_rows?ln_text_pairs[((LINNET_LCD_HEIGHT-rows_left)*LINNET_LCD_WIDTH)/2u+pair]:
#endif
                            pixel_pair;
                        ++pairs_written;
                    }
                }
                continue;
            }
            for (; pair < end; ++pair) {
                if ((pair & 255u) == 0u && ln_time_us() - started >= LINNET_LCD_FRAME_TIMEOUT_US) {
                    LINNET_LCD_RECORD(1, 1u);
                    goto failed;
                }
                if (!ln_lcd_wait(&LINNET_LCD_CONTROL, 0x01000000u, true, started, &budget)) goto failed;
                uint32_t pixel=pairs_written*2u;
                uint32_t row=pixel/LINNET_LCD_WIDTH,x=pixel%LINNET_LCD_WIDTH;
                uint16_t a=ln_fault_pixel(fault_rows,x,row);
                uint16_t b=ln_fault_pixel(fault_rows,x+1u,row);
                pixel_pair=(uint32_t)(uint16_t)((a<<8)|(a>>8)) |
                    ((uint32_t)(uint16_t)((b<<8)|(b>>8))<<16);
                LINNET_LCD_DATA = pixel_pair;
                ++pairs_written;
            }
        }
        LINNET_LCD_RECORD(0, 6u | ((rows_left == LINNET_LCD_HEIGHT ? 1u : 2u) << 8));
        if (!ln_lcd_wait(&LINNET_LCD_CONTROL, 0x04000000u, true, started, &budget)) goto failed;
        if (ln_time_us() - started >= LINNET_LCD_FRAME_TIMEOUT_US) {
            LINNET_LCD_RECORD(1, 1u);
            goto failed;
        }
        LINNET_LCD_CONFIG = 0;
        rows_left -= rows;
    }
    LINNET_LCD_RECORD(0, 7u);
    ln_lcd_record_end(started, budget, pairs_written);
    return true;
failed:
    ln_lcd_record_end(started, budget, pairs_written);
    LINNET_LCD_CONFIG = 0;
    ln_panel_ready = false;
    return false;
}

bool ln_lcd_fill(uint16_t color){return ln_lcd_draw(color,0,0,0,0);}
bool ln_target_disk_screen(bool symbol_visible){
    /* Disk Mode enables the cache after its first frame. Keep the uncached
     * write pacing here: cached redraws drift on the original A1099 panel. */
    bool cached = (LINNET_REG(0x6000c000u) & 1u) != 0u;
    if (cached && !ln_core_cache_disable()) return false;
    ln_disk_symbol_visible = symbol_visible;
    bool drawn = ln_lcd_draw(0,0,ln_disk_runs,ln_disk_palette,0);
    if (cached && !ln_core_cache_enable()) ln_platform_hardware_reset();
    return drawn;
}
#if defined(LINNET_IMAGE_BOOT)
bool ln_lcd_boot(void){return ln_lcd_draw(0,0,ln_boot_runs,ln_boot_palette,0);}
bool ln_lcd_boot_apple(void)
{
    static const char lines[8][33]={"APPLE IPOD SOFTWARE","","STARTING"};
    return ln_lcd_draw(0x1800u,lines,0,0,0);
}
static uint16_t ln_menu_bus_color(uint16_t color)
{
    return (uint16_t)((color<<8)|(color>>8));
}
static void ln_menu_set(uint32_t x,uint32_t y,uint16_t color)
{
    uint32_t *pair=&ln_text_pairs[(y*LINNET_LCD_WIDTH+x)/2u];
    uint32_t bus=ln_menu_bus_color(color);
    *pair=x&1u?(*pair&0xffffu)|(bus<<16):(*pair&0xffff0000u)|bus;
}
static void ln_menu_rect(uint32_t left,uint32_t top,uint32_t right,uint32_t bottom,uint16_t color)
{
    uint32_t bus=ln_menu_bus_color(color),pair=bus|(bus<<16);
    for(uint32_t y=top;y<bottom;++y)
        for(uint32_t x=left;x<right;x+=2u)
            ln_text_pairs[(y*LINNET_LCD_WIDTH+x)/2u]=pair;
}
static uint16_t ln_menu_blend(uint16_t background,uint16_t ink,uint32_t level)
{
    uint32_t back_r=(background>>11)&31u,back_g=(background>>5)&63u,back_b=background&31u;
    uint32_t ink_r=(ink>>11)&31u,ink_g=(ink>>5)&63u,ink_b=ink&31u;
    return (uint16_t)((((back_r*(3u-level)+ink_r*level+1u)/3u)<<11)|
                      (((back_g*(3u-level)+ink_g*level+1u)/3u)<<5)|
                      ((back_b*(3u-level)+ink_b*level+1u)/3u));
}
static uint32_t ln_menu_text_width(const char *message)
{
    uint32_t width=0;
    for(;*message;++message)if(*message>=32&&*message<=126)
        width+=ln_boot_menu_advances[(uint8_t)*message-32u];
    return width;
}
static void ln_menu_text(uint32_t top,const char *message,uint16_t ink,uint16_t background,uint32_t left)
{
    if(left==UINT32_MAX){
        uint32_t width=ln_menu_text_width(message);
        left=width<LINNET_LCD_WIDTH?(LINNET_LCD_WIDTH-width)/2u:0u;
    }
    uint16_t shades[4]={background,ln_menu_blend(background,ink,1u),
        ln_menu_blend(background,ink,2u),ink};
    for(;*message;++message){
        uint8_t c=(uint8_t)*message;
        if(c<32u||c>126u)continue;
        uint32_t glyph=c-32u;
        for(uint32_t y=0;y<LINNET_BOOT_MENU_FONT_HEIGHT;++y){
            if(top+y>=LINNET_LCD_HEIGHT)break;
            for(uint32_t x=0;x<LINNET_BOOT_MENU_FONT_WIDTH;++x){
                if(left+x>=LINNET_LCD_WIDTH)break;
                uint32_t pixel=y*LINNET_BOOT_MENU_FONT_WIDTH+x;
                uint8_t packed=ln_boot_menu_font[glyph][pixel>>2];
                uint32_t level=(packed>>(6u-2u*(pixel&3u)))&3u;
                if(level)ln_menu_set(left+x,top+y,shades[level]);
            }
        }
        left+=ln_boot_menu_advances[glyph];
    }
}
bool ln_lcd_boot_menu(uint32_t available,uint32_t selected,bool starting)
{
    /* Match the Linnet Boot artwork's charcoal, silver, and cyan palette. */
    const uint16_t paper=0x0841u,header=0x1083u,ink=0xefbfu,muted=0xa5d8u;
    ln_menu_rect(0u,0u,LINNET_LCD_WIDTH,LINNET_LCD_HEIGHT,paper);
    ln_menu_rect(0u,0u,LINNET_LCD_WIDTH,37u,header);
    ln_menu_rect(0u,36u,LINNET_LCD_WIDTH,37u,0x328cu);
    if(starting){
        ln_menu_text(9u,"Starting",ink,header,14u);
        ln_menu_text(78u,selected==3u?"Apple iPod software":
            selected==2u?"Rockbox":"Linnet",ink,paper,UINT32_MAX);
    }else if(!available){
        ln_menu_text(9u,"Choose startup",ink,header,14u);
        ln_menu_text(82u,"Checking startup files",muted,paper,UINT32_MAX);
    }else{
        ln_menu_text(9u,"Choose startup",ink,header,14u);
        uint32_t row=0u;
        static const char *names[3]={"Linnet","Rockbox","Apple iPod software"};
        for(uint32_t choice=1u;choice<=3u;++choice){
            if(!(available&(1u<<(choice-1u))))continue;
            uint32_t top=43u+row*31u;
            uint16_t background=choice==selected?0x118au:0x10a4u;
            ln_menu_rect(8u,top,212u,top+29u,background);
            ln_menu_text(top+6u,names[choice-1u],choice==selected?ink:muted,background,22u);
            ++row;
        }
        ln_menu_rect(8u,143u,212u,144u,0x328cu);
        ln_menu_text(151u,"Wheel: choose  Center: start",muted,paper,UINT32_MAX);
    }
    return ln_lcd_draw(0u,0,0,0,ln_text_pairs);
}
#endif
bool ln_lcd_fault(uint32_t reason,const uint8_t build_id[32])
{
    static const char hex[]="0123456789ABCDEF";
    char lines[8][33]={"LINNET STOPPED","CODE 00000000","BUILD 0000000000000000",
        "RESTARTING AUTOMATICALLY","IF THIS REPEATS:","HOLD MENU + CENTER TO RESTART",
        "THEN CENTER + PLAY FOR","DISK MODE AND CONNECT USB"};
    for(uint32_t i=0;i<8u;i++)lines[1][5u+i]=hex[(reason>>(28u-i*4u))&15u];
    for(uint32_t i=0;i<8u;i++){lines[2][6u+i*2u]=hex[build_id[i]>>4];lines[2][7u+i*2u]=hex[build_id[i]&15u];}
    return ln_lcd_draw(0x1800u,lines,0,0,0);
}
