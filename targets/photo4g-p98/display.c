#include "targets/photo4g-p98/lcd.h"
#include "hal/api/display.h"
#include "hal/pp502x/display_internal.h"
#include "hal/api/platform.h"
#include "hal/api/core.h"
#include "boot/include/linnet_boot.h"

#define REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define MB(offset) REG(LINNET_DISPLAY_MAILBOX + (offset))
#define PORT REG(0x70008a0c)
#define CONTROL REG(0x70008a20)
#define CONFIG REG(0x70008a24)
#define DATA REG(0x70008b00)
#define FRAME_DEADLINE 500000u
#define NATIVE(address) ((uintptr_t)(address) | (uintptr_t)0x10000000u)

struct rectangle { uint32_t x, y, width, height; };
static uint16_t draw_buffer[LINNET_DISPLAY_PIXELS] __attribute__((aligned(4)));
static uint16_t scanout_buffer[LINNET_DISPLAY_PIXELS] __attribute__((aligned(32)));
static bool column_mode, byte_panel;
static struct rectangle damage[LINNET_DISPLAY_QUEUE_CAPACITY];
static uint32_t damage_count, submitted, completed, frame_failures;
static uint32_t frame_started;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t max_present_us;
#define DISPLAY_RECORD(...) do { __VA_ARGS__; } while (0)
#else
#define DISPLAY_RECORD(...) ((void)0)
#endif
static bool initialized, cop_capable, frame_pending;
static bool direct_frame, draw_stale;
static bool clock_park_owned;
static uint32_t cop_error;
static uint32_t init_error;
static volatile bool sync_waiting;
static uint32_t sync_head;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t sync_frames,sync_timeouts,sync_wait_us,sync_max_wait_us,transfer_started;
static uint32_t transfer_us,transfer_max_us;
#endif

/* P98 GPIO A0: observed active-low frame pulse, 579 us every 16575 us.
 * Arm only while a frame is pending; idle display causes no extra IRQs. */
static void sync_cancel(void) {
    REG(0x6000d050u)&=~1u;
    REG(0x6000d070u)=1u;
    sync_waiting=false;
}
void ln_display_sync_irq(void) {
    if(!sync_waiting || !(REG(0x6000d040u)&1u))return;
    if(REG(0x6000d030u)&1u){
        /* We armed during blanking. Wait for the next complete low pulse. */
        REG(0x6000d060u)&=~1u;
        REG(0x6000d070u)=1u;
        return;
    }
    sync_cancel();
    DISPLAY_RECORD(transfer_started=ln_time_us();
    sync_wait_us=transfer_started-frame_started;
    if(sync_wait_us>sync_max_wait_us)sync_max_wait_us=sync_wait_us;
    ++sync_frames);
    __asm__ volatile("" ::: "memory");
    MB(LN_DISPLAY_HEAD)=sync_head;
}

static void barrier(void) { __asm__ volatile("" ::: "memory"); }
static volatile uint16_t *scanout(void) {
    return (volatile uint16_t *)NATIVE(scanout_buffer);
}

#include "hal/pp502x/cop_control.inc"

static void fail_job(uint32_t reason) {
    CONFIG = 0;
    MB(LN_DISPLAY_ERROR) = reason;
    MB(LN_DISPLAY_FAILURES)++;
    MB(LN_DISPLAY_STATE) = 0;
    MB(LN_DISPLAY_TAIL) = MB(LN_DISPLAY_HEAD);
}

/* Same five states and mailbox offsets as the resident COP worker. One step
 * performs one command write, pixel-pair write or readiness observation. */
static void cpu_step(void) {
    uint32_t state = MB(LN_DISPLAY_STATE);
    MB(LN_DISPLAY_TICKS)++;
    if (state == 0) {
        uint32_t head = MB(LN_DISPLAY_HEAD), tail = MB(LN_DISPLAY_TAIL);
        if (head == tail) return;
        if (head - tail > LINNET_DISPLAY_QUEUE_CAPACITY) { fail_job(3); return; }
        uint32_t slot = LN_DISPLAY_QUEUE + (tail & 7u) * 16u;
        uint32_t x = MB(slot), y = MB(slot + 4), width = MB(slot + 8), height = MB(slot + 12);
        uint32_t buffer = MB(LN_DISPLAY_BUFFER);
        if (x >= LINNET_UI_WIDTH || y >= LINNET_UI_HEIGHT || !width || !height ||
            width > LINNET_UI_WIDTH-x || height > LINNET_UI_HEIGHT-y ||
            ((x | width) & 1u) || (buffer & 3u) ||
            buffer < 0x10000000u || buffer > 0x10800000u-LINNET_DISPLAY_BYTES) { fail_job(4); return; }
        MB(LN_DISPLAY_X) = x; MB(LN_DISPLAY_Y) = y;
        MB(LN_DISPLAY_WIDTH) = width; MB(LN_DISPLAY_HEIGHT) = height;
        MB(LN_DISPLAY_ROWS_LEFT) = height;
        MB(LN_DISPLAY_X_CURSOR) = MB(LN_DISPLAY_Y_CURSOR) = MB(LN_DISPLAY_COMMAND) = 0;
        MB(LN_DISPLAY_BUDGET) = 1000000u;
        MB(LN_DISPLAY_STARTED) = ln_time_us();
        MB(LN_DISPLAY_STATE) = 1;
        return;
    }
    if (ln_time_us() - MB(LN_DISPLAY_STARTED) >= FRAME_DEADLINE) { fail_job(1); return; }
    if (!MB(LN_DISPLAY_BUDGET)) { fail_job(2); return; }
    MB(LN_DISPLAY_BUDGET)--;
    if (state == 1) {
        if (PORT & 0x80000000u) return;
        uint32_t command = MB(LN_DISPLAY_COMMAND), value;
        if (byte_panel) {
            /* Keep both bytes of each bus transfer together. */
            uint32_t x1 = LINNET_UI_WIDTH-1u-MB(LN_DISPLAY_X);
            uint32_t y0 = MB(LN_DISPLAY_Y);
            if (command < 6u) {
                uint32_t reg = command < 2u ? 0x44u : command < 4u ? 0x45u : 0x21u;
                uint32_t data = command < 2u ? ((y0+MB(LN_DISPLAY_HEIGHT)-1u)<<8)|y0 :
                    command < 4u ? (x1<<8)|(x1+1u-MB(LN_DISPLAY_WIDTH)) : (x1<<8)|y0;
                if (!(command & 1u)) {
                    PORT = 0x80000000u;
                    PORT = 0x80000000u | reg;
                } else {
                    PORT = 0x81000000u | (data >> 8);
                    PORT = 0x81000000u | (data & 255u);
                }
            } else {
                PORT = 0x80000000u;
                PORT = 0x80000022u;
            }
            MB(LN_DISPLAY_COMMAND) = command+1u;
            if (command == 6u) MB(LN_DISPLAY_STATE) = 2;
            return;
        }
        if (command == 0) value = 0x12u;
        else if (command == 1) value = MB(LN_DISPLAY_Y);
        else if (command == 2) value = 0x13u;
        else if (command == 3) value = LINNET_UI_WIDTH-1u-MB(LN_DISPLAY_X);
        else if (command == 4) value = 0x15u;
        else if (command == 5) value = MB(LN_DISPLAY_Y)+MB(LN_DISPLAY_HEIGHT)-1u;
        else if (command == 6) value = 0x16u;
        else value = LINNET_UI_WIDTH-MB(LN_DISPLAY_X)-MB(LN_DISPLAY_WIDTH);
        PORT = 0x80000000u | value;
        MB(LN_DISPLAY_COMMAND) = command+1u;
        if (command == 7) MB(LN_DISPLAY_STATE) = 2;
        return;
    }
    if (state == 2) {
        if (PORT & 0x80000000u) return;
        uint32_t rows = MB(LN_DISPLAY_ROWS_LEFT);
        if (rows > 128u) rows = 128u;
        uint32_t bytes = rows * MB(LN_DISPLAY_WIDTH) * 2u;
        MB(LN_DISPLAY_BLOCK_ROWS) = rows;
        MB(LN_DISPLAY_PAIRS_LEFT) = bytes / 4u;
        CONTROL = 0x10000080u; CONFIG = 0xc0010000u | (bytes-1u); CONTROL = 0x34000000u;
        MB(LN_DISPLAY_STATE) = 3;
        return;
    }
    if (state == 3) {
        if (!(CONTROL & 0x01000000u)) return;
        uint32_t offset = (MB(LN_DISPLAY_Y)+MB(LN_DISPLAY_Y_CURSOR))*LINNET_UI_WIDTH +
                          MB(LN_DISPLAY_X)+MB(LN_DISPLAY_X_CURSOR);
        const volatile uint16_t *pixels = (const volatile uint16_t *)(uintptr_t)MB(LN_DISPLAY_BUFFER);
        uint16_t first = pixels[offset], second = pixels[offset+1u];
        DATA = ((uint32_t)(first & 255u)<<8) | (first>>8) |
               ((uint32_t)(second & 255u)<<24) | ((uint32_t)(second>>8)<<16);
        uint32_t column = MB(LN_DISPLAY_X_CURSOR)+2u;
        if (column == MB(LN_DISPLAY_WIDTH)) { column=0; MB(LN_DISPLAY_Y_CURSOR)++; }
        MB(LN_DISPLAY_X_CURSOR)=column;
        MB(LN_DISPLAY_PAIRS)++;
        MB(LN_DISPLAY_PAIRS_LEFT)--;
        if (!MB(LN_DISPLAY_PAIRS_LEFT)) MB(LN_DISPLAY_STATE)=4;
        return;
    }
    if (state == 4) {
        if (!(CONTROL & 0x04000000u)) return;
        CONFIG=0;
        MB(LN_DISPLAY_ROWS_LEFT)-=MB(LN_DISPLAY_BLOCK_ROWS);
        if (MB(LN_DISPLAY_ROWS_LEFT)) { MB(LN_DISPLAY_STATE)=2; return; }
        MB(LN_DISPLAY_STATE)=0;
        barrier();
        MB(LN_DISPLAY_TAIL)++;
        MB(LN_DISPLAY_COMPLETED)++;
        return;
    }
    fail_job(5);
}

bool ln_display_init(void) {
    initialized=false;
    init_error=0;
    byte_panel=(ln_lcd_panel_type() & 1u)!=0u;
    cop_capable=cop_probe();
    if (!pause_worker(true)) {init_error=0x01000000u|((MB(LN_DISPLAY_PAUSED)&255u)<<16)|cop_error;return false;}
    uint32_t generation=MB(LN_DISPLAY_MAGIC)==LINNET_DISPLAY_MAGIC ? MB(LN_DISPLAY_GENERATION)+1u : 1u;
    MB(LN_DISPLAY_ENABLED)=0;
    CONFIG=0;
    for (uint32_t offset=0;offset<120u;offset+=4u)
        if (offset!=LN_DISPLAY_PAUSE && offset!=LN_DISPLAY_PAUSED) MB(offset)=0;
    if (!cop_capable) MB(LN_DISPLAY_PAUSE)=MB(LN_DISPLAY_PAUSED)=0;
    MB(LN_DISPLAY_GENERATION)=generation;
    MB(LN_DISPLAY_BUFFER)=(uint32_t)NATIVE(scanout_buffer);
    MB(LN_DISPLAY_MAGIC)=LINNET_DISPLAY_MAGIC;
    for (uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i) { draw_buffer[i]=0; scanout()[i]=0; }
    damage_count=submitted=completed=frame_failures=0;
    DISPLAY_RECORD(max_present_us=0);
    frame_pending=direct_frame=draw_stale=false;
    sync_cancel();
    DISPLAY_RECORD(sync_frames=sync_timeouts=sync_wait_us=sync_max_wait_us=0;
    transfer_us=transfer_max_us=0);
#if !(defined(LINNET_NATIVE_UI) && defined(LINNET_STORAGE))
    if (!ln_lcd_fill(0)) {
        init_error=0x02000000u;
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t lcd[8];ln_lcd_diagnostics(lcd);init_error|=((lcd[1]&255u)<<16)|(lcd[0]&65535u);
#endif
        return false;
    }
#endif
    initialized=true;
    MB(LN_DISPLAY_ENABLED)=(cop_capable&&!byte_panel)?1u:0u;
    if (!pause_worker(false)) {init_error=0x03000000u|((MB(LN_DISPLAY_PAUSED)&255u)<<16)|cop_error;initialized=false;return false;}
    return ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT);
}
uint32_t ln_display_init_error(void){return init_error;}

uint16_t *ln_display_framebuffer(void) {
    if(direct_frame)return NULL;
    if(draw_stale){
        /* Import the retained video once before ordinary incremental UI
         * resumes. The COP only reads scanout, so this is also safe when busy. */
        barrier();
#if defined(__arm__)
        extern void ln_memory_copy(void *,const void *,uint32_t);
        ln_memory_copy(draw_buffer,(const void *)(uintptr_t)scanout(),LINNET_DISPLAY_BYTES);
#else
        for(uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i)draw_buffer[i]=scanout()[i];
#endif
        barrier();draw_stale=false;
    }
    return draw_buffer;
}

uint16_t *ln_display_direct_framebuffer(void) {
    if(!initialized||direct_frame||frame_pending||damage_count||
       MB(LN_DISPLAY_HEAD)!=MB(LN_DISPLAY_TAIL))return NULL;
    /* A synchronous full repaint owns scanout until present publishes it.
     * Seed full damage even if the painter coalesces its own damage callbacks. */
    direct_frame=true;draw_stale=!column_mode;
    damage[0]=(struct rectangle){0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT};damage_count=1;
    barrier();
    return column_mode?draw_buffer:(uint16_t *)(uintptr_t)scanout();
}

bool ln_display_damage(int32_t x,int32_t y,int32_t width,int32_t height) {
    if (!initialized || width<=0 || height<=0 ||
        x>=(int32_t)LINNET_UI_WIDTH || y>=(int32_t)LINNET_UI_HEIGHT) return false;
    if (x<0) { if (x<=-width) return false; width+=x; x=0; }
    if (y<0) { if (y<=-height) return false; height+=y; y=0; }
    if (width>(int32_t)LINNET_UI_WIDTH-x) width=(int32_t)LINNET_UI_WIDTH-x;
    if (height>(int32_t)LINNET_UI_HEIGHT-y) height=(int32_t)LINNET_UI_HEIGHT-y;
    uint32_t right=((uint32_t)(x+width)+1u)&~1u;
    x&=~1;
    if(column_mode){uint32_t bottom=((uint32_t)(y+height)+1u)&~1u;y&=~1;height=(int32_t)bottom-y;}
    struct rectangle rectangle={(uint32_t)x,(uint32_t)y,right-(uint32_t)x,(uint32_t)height};
    for (uint32_t i=0;i<damage_count;++i) {
        struct rectangle *other=&damage[i];
        if (rectangle.x<=other->x+other->width && other->x<=rectangle.x+rectangle.width &&
            rectangle.y<=other->y+other->height && other->y<=rectangle.y+rectangle.height) {
            uint32_t left=rectangle.x<other->x?rectangle.x:other->x;
            uint32_t top=rectangle.y<other->y?rectangle.y:other->y;
            uint32_t end_x=rectangle.x+rectangle.width>other->x+other->width?rectangle.x+rectangle.width:other->x+other->width;
            uint32_t end_y=rectangle.y+rectangle.height>other->y+other->height?rectangle.y+rectangle.height:other->y+other->height;
            /* A scrollbar touching a selection row must not expand that row
             * into a full list transfer. Merge only when the bounding box
             * costs no more pixels than the two existing jobs. */
            if ((end_x-left)*(end_y-top)>
                rectangle.width*rectangle.height+other->width*other->height) continue;
            *other=(struct rectangle){left,top,end_x-left,end_y-top};
            return true;
        }
    }
    if (damage_count==LINNET_DISPLAY_QUEUE_CAPACITY) {
        damage[0]=(struct rectangle){0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT};damage_count=1;
    } else damage[damage_count++]=rectangle;
    return true;
}

void ln_display_column_mode(bool enabled) {
    column_mode=enabled&&!byte_panel;
    MB(LN_DISPLAY_BUFFER)=(uint32_t)NATIVE(scanout_buffer);
}

bool ln_display_present(void) {
    if (!initialized || !damage_count || frame_pending || MB(LN_DISPLAY_HEAD)!=MB(LN_DISPLAY_TAIL)) return false;
    /* A reset/retry may publish without another ordinary UI render. */
    if(!direct_frame&&draw_stale)(void)ln_display_framebuffer();
    /* The scanout buffer retains the previous complete frame. Publish only
     * damaged pixels while the COP is idle; small focus changes must not
     * copy an entire framebuffer through uncached SDRAM. */
    for (uint32_t i=0;!column_mode&&!direct_frame&&i<damage_count;++i) {
        const struct rectangle *r=&damage[i];
#if defined(__arm__)
        /* The external assembly routine performs every store to the uncached
         * alias. Barriers retain volatile publication ordering; ownership
         * remains with the CPU until the existing mailbox head publication. */
        extern void ln_memory_copy(void *,const void *,uint32_t);
        barrier();
        if(r->x==0u&&r->width==LINNET_UI_WIDTH){
            uint32_t begin=r->y*LINNET_UI_WIDTH;
            ln_memory_copy((void *)(uintptr_t)(scanout()+begin),
                draw_buffer+begin,r->height*LINNET_UI_WIDTH*2u);
        }else for(uint32_t y=r->y;y<r->y+r->height;++y){
            uint32_t begin=y*LINNET_UI_WIDTH+r->x;
            ln_memory_copy((void *)(uintptr_t)(scanout()+begin),
                draw_buffer+begin,r->width*2u);
        }
        barrier();
#else
        for(uint32_t y=r->y;y<r->y+r->height;++y) {
            uint32_t begin=y*LINNET_UI_WIDTH+r->x, remaining=r->width;
            /* Both buffers are word aligned. Keep scalar edges so this copy
             * also remains exact if a future damage producer uses odd spans. */
            if((begin&1u)&&remaining){scanout()[begin]=draw_buffer[begin];++begin;--remaining;}
            typedef uint32_t copy_word __attribute__((__may_alias__));
            const copy_word *source=(const copy_word *)(draw_buffer+begin);
            volatile copy_word *destination=(volatile copy_word *)(scanout()+begin);
            while(remaining>=2u){*destination++=*source++;remaining-=2u;}
            if(remaining)*(volatile uint16_t *)destination=*(const uint16_t *)source;
        }
#endif
    }
    if(column_mode){
        for(uint32_t i=0;i<damage_count;++i){
            const struct rectangle *r=&damage[i];
#if defined(__arm__)
            extern void ln_memory_columns(void *,const void *,uint32_t,uint32_t);
            ln_memory_columns((void *)(uintptr_t)(scanout()+
                (LINNET_UI_WIDTH-1u-r->x)*LINNET_UI_HEIGHT+r->y),
                draw_buffer+r->y*LINNET_UI_WIDTH+r->x,r->width,r->height);
#else
            volatile uint32_t *destination=(volatile uint32_t *)NATIVE(scanout_buffer);
            for(uint32_t x=r->x;x<r->x+r->width;++x){
                uint32_t out=((LINNET_UI_WIDTH-1u-x)*LINNET_UI_HEIGHT+r->y)/2u;
                for(uint32_t y=r->y;y<r->y+r->height;y+=2u){
                    uint32_t at=y*LINNET_UI_WIDTH+x;
                    destination[out++]=(uint32_t)draw_buffer[at]|
                        ((uint32_t)draw_buffer[at+LINNET_UI_WIDTH]<<16);
                }
            }
#endif
        }
    }
    /* Keep every damaged rectangle contiguous. Partitioning wide selection
     * rows made one logical highlight arrive as several visible strips and
     * repeated the panel command setup for each piece. Multiple independent
     * rectangles remain ordered from the panel's right edge toward the left. */
    struct rectangle jobs[LINNET_DISPLAY_QUEUE_CAPACITY];
    uint32_t job_count=damage_count;
    for(uint32_t i=0;i<damage_count;++i)jobs[i]=damage[i];
    for(uint32_t i=1;i<job_count;++i){
        struct rectangle r=jobs[i];uint32_t j=i;
        while(j&&jobs[j-1u].x<r.x){jobs[j]=jobs[j-1u];--j;}
        jobs[j]=r;
    }
    uint32_t head=MB(LN_DISPLAY_HEAD);
    for (uint32_t i=0;i<job_count;++i) {
        uint32_t slot=LN_DISPLAY_QUEUE+((head+i)&7u)*16u;
        MB(slot)=jobs[i].x;MB(slot+4)=jobs[i].y;
        MB(slot+8)=jobs[i].width;MB(slot+12)=jobs[i].height;
    }
    if (job_count>MB(LN_DISPLAY_HIGH_WATER)) MB(LN_DISPLAY_HIGH_WATER)=job_count;
    frame_failures=MB(LN_DISPLAY_FAILURES);
    frame_started=ln_time_us();
    frame_pending=true;submitted++;
    barrier();
    uint32_t irq=ln_core_irq_save();
    sync_head=head+job_count;
    sync_waiting=true;
    REG(0x6000d060u)=(REG(0x6000d060u)&~1u)|((REG(0x6000d030u)&1u)?0u:1u);
    REG(0x6000d070u)=1u;
    REG(0x6000d050u)|=1u;
    ln_core_input_rearm();
    ln_core_irq_restore(irq);
    damage_count=0;
    direct_frame=false;
    return true;
}

bool ln_display_service(uint32_t max_pairs) {
    if (!initialized) return false;
    /* A missing panel pulse must not stall display ownership or recovery. */
    if(sync_waiting){
        if(ln_time_us()-frame_started<25000u)return true;
        uint32_t irq=ln_core_irq_save();
        if(sync_waiting){sync_cancel();DISPLAY_RECORD(++sync_timeouts;transfer_started=ln_time_us());barrier();MB(LN_DISPLAY_HEAD)=sync_head;}
        ln_core_irq_restore(irq);
    }
    if (max_pairs>256u) max_pairs=256u;
    if (!cop_capable || byte_panel) for (uint32_t i=0;i<max_pairs && MB(LN_DISPLAY_HEAD)!=MB(LN_DISPLAY_TAIL);++i) cpu_step();
    if (frame_pending && MB(LN_DISPLAY_HEAD)==MB(LN_DISPLAY_TAIL)) {
        frame_pending=false;
        if (frame_failures!=MB(LN_DISPLAY_FAILURES)) {
            (void)ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT);return false;
        }
        DISPLAY_RECORD(uint32_t elapsed=ln_time_us()-frame_started;
        transfer_us=ln_time_us()-transfer_started;
        if(transfer_us>transfer_max_us)transfer_max_us=transfer_us;
        if (elapsed>max_present_us) max_present_us=elapsed);
        completed++;
    }
    return true;
}

bool ln_display_busy(void) { return frame_pending || MB(LN_DISPLAY_HEAD)!=MB(LN_DISPLAY_TAIL); }
bool ln_display_cop_capable(void) { return cop_capable; }

bool ln_display_quiesce(void) {
    if (!initialized) return true;
    uint32_t irq=ln_core_irq_save();sync_cancel();ln_core_irq_restore(irq);
    if (!pause_worker(true)) return false;
    MB(LN_DISPLAY_ENABLED)=0;CONFIG=0;
    MB(LN_DISPLAY_STATE)=0;MB(LN_DISPLAY_TAIL)=MB(LN_DISPLAY_HEAD);
    frame_pending=direct_frame=false;
    return true;
}

bool ln_display_reset(void) {
    if (!initialized || !ln_display_quiesce()) return false;
    MB(LN_DISPLAY_GENERATION)++;MB(LN_DISPLAY_RESETS)++;
    MB(LN_DISPLAY_HEAD)=MB(LN_DISPLAY_TAIL)=0;
    damage_count=0;
    MB(LN_DISPLAY_ENABLED)=(cop_capable&&!byte_panel)?1u:0u;
    if (!pause_worker(false)) return false;
    return ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT);
}

void ln_display_presentation_snapshot(struct ln_display_presentation *output) {
    if (!output) return;
    *output=(struct ln_display_presentation){submitted,completed,
        MB(LN_DISPLAY_FAILURES),MB(LN_DISPLAY_RESETS),MB(LN_DISPLAY_GENERATION)};
}

void ln_display_get_metrics(struct ln_display_metrics *output) {
    if (!output) return;
    *output=(struct ln_display_metrics){submitted,completed,MB(LN_DISPLAY_COMPLETED),MB(LN_DISPLAY_PAIRS),
        MB(LN_DISPLAY_HIGH_WATER),MB(LN_DISPLAY_FAILURES),MB(LN_DISPLAY_ERROR),MB(LN_DISPLAY_RESETS),
        MB(LN_DISPLAY_GENERATION),MB(LN_DISPLAY_HEAD)-MB(LN_DISPLAY_TAIL),cop_capable,
        cop_capable && MB(LN_DISPLAY_ENABLED) && !MB(LN_DISPLAY_PAUSED),
#if !defined(LINNET_PROFILE_RELEASE)
        max_present_us,
#else
        0,
#endif
        MB(LN_DISPLAY_HEAD),MB(LN_DISPLAY_TAIL)};
}

const uint16_t *ln_display_snapshot(void) { return direct_frame?NULL:column_mode?draw_buffer:(const uint16_t *)NATIVE(scanout_buffer); }
uint32_t ln_display_snapshot_crc32(void) { const uint16_t *frame=ln_display_snapshot();return frame?ln_crc32((const uint8_t *)frame,LINNET_DISPLAY_BYTES):0; }
size_t ln_display_capture(uint8_t *output,size_t capacity) {
    if (!initialized || direct_frame || !output || capacity<LINNET_DISPLAY_BYTES) return 0;
    for (uint32_t i=0;i<LINNET_DISPLAY_PIXELS;++i) {
        uint16_t pixel=column_mode?scanout()[(LINNET_UI_WIDTH-1u-i%LINNET_UI_WIDTH)*
            LINNET_UI_HEIGHT+i/LINNET_UI_WIDTH]:scanout()[i];
        output[2u*i]=(uint8_t)pixel;output[2u*i+1u]=(uint8_t)(pixel>>8);
    }
    return LINNET_DISPLAY_BYTES;
}

#if !defined(LINNET_PROFILE_RELEASE)
uint32_t ln_display_column_mismatches(void) {
    if(!column_mode||direct_frame)return 0xffffffffu;
    uint32_t mismatches=0;
    for(uint32_t y=0;y<LINNET_UI_HEIGHT;++y)
        for(uint32_t x=0;x<LINNET_UI_WIDTH;++x)
            if(scanout()[(LINNET_UI_WIDTH-1u-x)*LINNET_UI_HEIGHT+y]!=
               draw_buffer[y*LINNET_UI_WIDTH+x])++mismatches;
    return mismatches;
}
void ln_display_sync_diagnostics(uint32_t output[16]) {
    uint32_t irq=ln_core_irq_save();
    uint32_t words[16]={0x4e595344u,0u,sync_frames,sync_timeouts,sync_wait_us,
        sync_max_wait_us,sync_waiting,frame_pending,submitted,completed,
        MB(LN_DISPLAY_FAILURES),MB(LN_DISPLAY_PAIRS),max_present_us,
        transfer_us,transfer_max_us,ln_time_us()};
    for(uint32_t i=0;i<16u;++i)output[i]=words[i];
    ln_core_irq_restore(irq);
}
#endif

/* Called before any RAM replacement, including panic recovery. */
#if !defined(LINNET_PROFILE_RELEASE)
int ln_recovery_presentation_stop(void) { return ln_display_quiesce()?1:0; }
#endif

uint32_t ln_display_target_diagnostics(uint32_t selector,uint32_t out[32]){
    if(selector!=9u||!out)return 0;
    out[0]=0x5044434cu;out[1]=0;out[2]=REG(0x6000d030u);out[3]=ln_lcd_panel_type();return 4;
}
