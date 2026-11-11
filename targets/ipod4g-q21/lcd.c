#include "hal/api/platform.h"
#include "hal/pp502x/disk_screen.h"
#include "targets/ipod4g-q21/lcd.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define LCD_CONTROL REG(0x70003000u)
#define LCD_COMMAND REG(0x70003008u)
#define LCD_DATA REG(0x70003010u)
#define LCD_BUSY 0x8000u
#define LCD_TIMEOUT_US 250000u

static bool ready;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t trace[8];
#define LCD_RECORD(index, value) (trace[(index)] = (value))
#define LCD_RECORD_FAILURE() do { if (!trace[1]) trace[1] = 4u; } while (0)
#else
#define LCD_RECORD(index, value) ((void)0)
#define LCD_RECORD_FAILURE() ((void)0)
#endif
static bool draw(uint16_t color,const char (*lines)[24],bool disk_screen,bool symbol_visible);

static bool delay_us(uint32_t delay)
{
    uint32_t started = ln_time_us();
    for (uint32_t budget = 4000000u; budget; --budget)
        if (ln_time_us() - started >= delay) return true;
    return false;
}

static bool wait_ready(uint32_t started, uint32_t *budget, uint32_t timeout)
{
    while (LCD_CONTROL & LCD_BUSY) {
        if (!*budget || ln_time_us() - started >= timeout) {
            LCD_RECORD(1, *budget ? 1u : 2u);
            return false;
        }
        --*budget;
    }
    return true;
}

static bool prepare(uint32_t command, uint32_t started, uint32_t *budget, uint32_t timeout)
{
    LCD_RECORD(0, 3u | (command << 16));
    if (!wait_ready(started, budget, timeout)) return false;
    LCD_COMMAND = 0;
    if (!wait_ready(started, budget, timeout)) return false;
    LCD_COMMAND = command;
    return true;
}

static bool command_data(uint32_t command, uint32_t value,
                         uint32_t started, uint32_t *budget, uint32_t timeout)
{
    if (!prepare(command, started, budget, timeout) || !wait_ready(started, budget, timeout)) return false;
    LCD_DATA = value >> 8;
    if (!wait_ready(started, budget, timeout)) return false;
    LCD_DATA = value & 255u;
    return true;
}

static void finish(uint32_t started, uint32_t budget, uint32_t blocks)
{
#if !defined(LINNET_PROFILE_RELEASE)
    LCD_RECORD(2, ln_time_us() - started);
    LCD_RECORD(3, blocks);
    LCD_RECORD(4, budget);
    LCD_RECORD(5, LCD_CONTROL);
    LCD_RECORD(6, LCD_COMMAND);
    LCD_RECORD(7, LCD_DATA);
#else
    (void)started; (void)budget; (void)blocks;
#endif
}

bool ln_q21_lcd_init(void)
{
    if (ready) return true;
    uint32_t started = ln_time_us(), budget = 1000000u;
    LCD_RECORD(0, 1); LCD_RECORD(1, 0);
    /* Apple Disk Mode can leave the PP5020 mono bridge accepting writes while
     * still presenting its inherited scanout.  Acquire the bridge explicitly
     * before programming the HD66753.  This is the same enable/reset ordering
     * used by the established iPod 4G platform initialization. */
    REG(0x6000600cu) |= 0x04000000u;
    REG(0x60006004u) |= 0x04000000u;
    if (!delay_us(1u)) {
        LCD_RECORD(1, 4u); finish(started, budget, 0); return false;
    }
    REG(0x60006004u) &= ~0x04000000u;
    if (!(REG(0x6000600cu) & 0x04000000u) ||
        (REG(0x60006004u) & 0x04000000u)) {
        LCD_RECORD(1, 3u);
        finish(started, budget, 0);
        return false;
    }
    if (!delay_us(1000u)) {
        LCD_RECORD(1, 4u); finish(started, budget, 0); return false;
    }
    LCD_CONTROL = 0x0084u;
    /* A persistent boot can inherit a powered but sleeping HD66753. Its GRAM
     * accepts writes in that state while the glass keeps showing the final
     * Apple frame. Wake the oscillator and analog stages before programming
     * the normal 4G register set. */
    if (!command_data(0x00u, 0x0001u, started, &budget, LCD_TIMEOUT_US) ||
        !delay_us(10000u) ||
        !command_data(0x03u, 0x1200u, started, &budget, LCD_TIMEOUT_US) ||
        !delay_us(1000u) ||
        !command_data(0x03u, 0x120cu, started, &budget, LCD_TIMEOUT_US)) {
        LCD_RECORD_FAILURE();
        finish(started, budget, 0); return false;
    }
    static const uint16_t setup[][2] = {
        {0x02u,0x0048u}, {0x03u,0x120cu}, {0x07u,0x0019u},
        {0x04u,0x0463u}, {0x01u,0x000fu}, {0x0du,0x7f00u}, {0x05u,0}
    };
    for (uint32_t i = 0; i < sizeof setup / sizeof setup[0]; ++i)
        if (!command_data(setup[i][0], setup[i][1], started, &budget, LCD_TIMEOUT_US)) {
            finish(started, budget, 0); return false;
        }
    /* Shared 4G/Color PWM and B3 backlight circuit. */
    REG(0x6000d804u) = 0x0c0cu;
    REG(0x6000d824u) = 0x0808u;
    REG(0x70000084u) &= ~0x02000000u;
    REG(0x6000600cu) |= 0x00020000u;
    REG(0x7000a010u) = 0x80640000u;
    ready = true;
    finish(started, budget, 0);
    return true;
}

bool ln_q21_lcd_reinitialize(void)
{
    uint32_t pwm=REG(0x7000a010u),backlight=REG(0x6000d024u)&8u;
    ready = false;
    bool ok=ln_q21_lcd_init();
    /* Resetting scanout must preserve the user's backlight state. */
    REG(0x7000a010u)=pwm;
    REG(0x6000d824u)=backlight?0x0808u:0x0800u;
    return ok;
}

bool ln_q21_lcd_diagnostic(void)
{
    static const char lines[7][24]={"LINNET DISPLAY TEST","4G MONO 160 X 128","0123456789","ABCDEFGHIJKLMNO","PQRSTUVWXYZ","EDGE TO EDGE"};
    return draw(0xffffu,lines,false,false);
}

bool ln_q21_lcd_begin_row(uint32_t y)
{
    return ln_q21_lcd_begin_span(y, 0);
}

bool ln_q21_lcd_begin_span(uint32_t y, uint32_t block)
{
    if (y >= LINNET_LCD_HEIGHT || block >= LINNET_LCD_WIDTH/8u || !ln_q21_lcd_init()) return false;
    uint32_t started = ln_time_us(), budget = 100000u;
    if (!command_data(0x11u, (y << 5) + 20u - block, started, &budget, 2000u) ||
        !prepare(0x12u, started, &budget, 2000u)) return false;
    return true;
}

bool ln_q21_lcd_flush(void)
{
    /* The common ready case needs no timer read or polling budget setup. */
    if (!(LCD_CONTROL & LCD_BUSY)) return true;
    uint32_t started = ln_time_us(), budget = 100000u;
    return wait_ready(started, &budget, 2000u);
}

bool ln_q21_lcd_write(uint8_t first, uint8_t second)
{
    if (!ln_q21_lcd_flush()) return false;
    LCD_DATA = first;
    if (!ln_q21_lcd_flush()) return false;
    LCD_DATA = second;
    return true;
}

static uint8_t shade(uint16_t color)
{
    uint32_t r=(color>>11)&31u,g=(color>>5)&63u,b=color&31u;
    uint32_t luma=(r*77u*255u/31u+g*150u*255u/63u+b*29u*255u/31u)>>8;
    return (uint8_t)(3u-(luma*3u+127u)/255u);
}

static uint8_t text_pixel(const char (*lines)[24],uint32_t x,uint32_t y)
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
    if(x==0u||x==159u||y==0u||y==127u)return 3u;
    if(x<8u||x>=152u||y<8u||y>=120u)return 0;
    x-=8u;y-=8u;uint32_t line=y/16u,row=(y%16u)/2u,column=x/6u,bit=x%6u;
    if(line>=7u||row>=7u||column>=24u||bit>=5u)return 0;
    uint8_t c=(uint8_t)lines[line][column],mask=0;
    if(c>='0'&&c<='9')mask=glyphs[c-'0'][row];
    else if(c>='A'&&c<='Z')mask=glyphs[c-'A'+10u][row];
    else if(c==':')mask=row==2u||row==5u?4u:0u;
    else if(c=='+')mask=row==3u?14u:(row==2u||row==4u?4u:0u);
    return (mask&(16u>>bit))?3u:0u;
}

static bool draw(uint16_t color,const char (*lines)[24],bool disk_screen,bool symbol_visible)
{
    uint32_t started=ln_time_us(),blocks=0;LCD_RECORD(1, 0);
    if(!ln_q21_lcd_init())return false;
    uint8_t solid=shade(color);
    for(uint32_t y=0;y<LINNET_LCD_HEIGHT;++y){
        if(!ln_q21_lcd_begin_row(y))goto failed;
        for(uint32_t x=0;x<LINNET_LCD_WIDTH;x+=8u){
            uint8_t bytes[2]={0,0};
            for(uint32_t p=0;p<8u;++p){
                uint8_t value;
                if(disk_screen){
                    uint16_t pixel=ln_pp_disk_screen_pixel(LINNET_LCD_WIDTH,LINNET_LCD_HEIGHT,x+p,y,symbol_visible);
                    value=pixel==0xffffu?0u:pixel==0xe71cu?1u:
                        pixel==0x9cd3u||pixel==0x738eu?2u:3u;
                }else value=lines?text_pixel(lines,x+p,y):solid;
                bytes[p>>2]|=(uint8_t)(value<<(6u-2u*(p&3u)));
            }
            if(!ln_q21_lcd_write(bytes[0],bytes[1]))goto failed;
            ++blocks;
        }
    }
    finish(started,1000000u,blocks);return true;
failed:
    finish(started,0,blocks);ready=false;return false;
}

bool ln_lcd_fill(uint16_t color){return draw(color,0,false,false);}
bool ln_target_disk_screen(bool symbol_visible){return draw(0xffffu,0,true,symbol_visible);}
#if defined(LINNET_IMAGE_BOOT)
bool ln_lcd_boot(void)
{
    static const char lines[7][24]={"LINNET","MP102","STARTING"};
    return draw(0xffffu,lines,false,false);
}
bool ln_lcd_boot_apple(void)
{
    static const char lines[7][24]={"APPLE IPOD SOFTWARE","","STARTING"};
    return draw(0xffffu,lines,false,false);
}
bool ln_lcd_boot_menu(uint32_t available,uint32_t selected,bool starting)
{
    char lines[7][24]={"CHOOSE STARTUP","","","","WHEEL: CHOOSE",
        "CENTER: START","AUTO: LINNET 30 SEC"};
    if(starting){
        const char *name=selected==3u?"APPLE SOFTWARE":selected==2u?"ROCKBOX":"LINNET";
        const char *prefix="STARTING ";uint32_t at=0;
        while(prefix[at]){lines[2][at]=prefix[at];++at;}
        for(uint32_t i=0;name[i];++i)lines[2][at+i]=name[i];
        for(uint32_t row=4u;row<7u;++row)lines[row][0]=0;
    }else if(!available){
        const char *message="CHECKING STARTUP FILES";
        for(uint32_t i=0;message[i];++i)lines[2][i]=message[i];
    }else{
        uint32_t row=1u;
        static const char *names[3]={"LINNET","ROCKBOX","APPLE SOFTWARE"};
        for(uint32_t choice=1u;choice<=3u;++choice){
            if(!(available&(1u<<(choice-1u))))continue;
            lines[row][0]=choice==selected?'+':' ';
            lines[row][1]=' ';
            const char *name=names[choice-1u];
            for(uint32_t i=0;name[i];++i)lines[row][i+2u]=name[i];
            ++row;
        }
    }
    return draw(0xffffu,lines,false,false);
}
#endif
bool ln_lcd_fault(uint32_t reason,const uint8_t build_id[32])
{
    static const char hex[]="0123456789ABCDEF";
    char lines[7][24]={"LINNET STOPPED","CODE 00000000","BUILD 0000000000000000",
        "RESTARTING","MENU+CENTER: RESET","CENTER+PLAY: DISK"};
    for(uint32_t i=0;i<8u;++i)lines[1][5u+i]=hex[(reason>>(28u-i*4u))&15u];
    for(uint32_t i=0;i<8u;++i){lines[2][6u+i*2u]=hex[build_id[i]>>4];lines[2][7u+i*2u]=hex[build_id[i]&15u];}
    return draw(0xffffu,lines,false,false);
}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_lcd_diagnostics(uint32_t output[8])
{
    if(output)for(uint32_t i=0;i<8u;++i)output[i]=trace[i];
}
#endif
