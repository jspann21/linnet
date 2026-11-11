#include "hal/api/platform.h"
#include "hal/api/display.h"
#include "hal/api/core.h"
#include "hal/s5l8702/registers.h"
#include "hal/s5l8702/board.h"
#include "ui/profiles/color320/boot_font.h"
#include "ui/profiles/color320/boot_menu.h"
#include "ui/profiles/color320/disk_screen.h"
#if defined(LINNET_IMAGE_OS) || defined(LINNET_IMAGE_BOOT)
#include "boot/include/linnet_boot_wordmark_320.h"
#endif
bool ln_lcd_fill(uint16_t color) {
    uint16_t *frame=ln_display_framebuffer();
    if(!frame){if(!ln_display_init())return false;frame=ln_display_framebuffer();}
    if(!ln_display_quiesce())return false;
    for(unsigned i=0;i<LINNET_DISPLAY_PIXELS;i++)frame[i]=color;
    if(!ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT) || !ln_display_present())return false;
    return ln_display_quiesce();
}
static void text(unsigned top,const char *message,uint16_t color) {
    uint16_t *frame=ln_display_framebuffer();unsigned width=0;
    for(const char *p=message;*p;p++)if(*p>=32&&*p<=126)width+=advances[(unsigned)*p-32]*2u;
    unsigned left=width<LINNET_UI_WIDTH?(LINNET_UI_WIDTH-width)/2:0;
    for(;*message&&left<LINNET_UI_WIDTH;message++) {
        if(*message<32||*message>126)continue;unsigned glyph=(unsigned)*message-32;
        for(unsigned y=0;y<22&&top+y<LINNET_UI_HEIGHT;y++)
            for(unsigned x=0;x<22&&left+x<LINNET_UI_WIDTH;x++)
                if(glyphs[glyph][y/2]&(1u<<(x/2)))frame[(top+y)*LINNET_UI_WIDTH+left+x]=color;
        left+=advances[glyph]*2u;
    }
}
static bool present(void){return ln_display_damage(0,0,LINNET_UI_WIDTH,LINNET_UI_HEIGHT)&&ln_display_present()&&ln_display_quiesce();}
bool ln_lcd_boot(void){
#if defined(LINNET_IMAGE_OS) || defined(LINNET_IMAGE_BOOT)
    uint16_t *frame=ln_display_framebuffer();
    if(!frame){if(!ln_display_init())return false;frame=ln_display_framebuffer();}
    if(!frame || !ln_display_quiesce())return false;
    unsigned pixel=0;
    for(unsigned run=0;run<sizeof ln_boot_runs/sizeof ln_boot_runs[0];run++){
        uint32_t packed=ln_boot_runs[run],index=packed&511u,count=packed>>9;
        if(!count || index>=sizeof ln_boot_palette/sizeof ln_boot_palette[0] ||
           count>LINNET_DISPLAY_PIXELS-pixel)return false;
        while(count--)frame[pixel++]=ln_boot_palette[index];
    }
    return pixel==LINNET_DISPLAY_PIXELS && present();
#else
    if(!ln_lcd_fill(0x0841))return false;
    text(94,"Linnet",0xffff);
    text(150,"Starting",0xad75);return present();
#endif
}
bool ln_lcd_boot_apple(void){
    if(!ln_lcd_fill(0xffff))return false;
    text(94,"Apple iPod Software",0x0000);text(150,"Starting",0x4208);return present();
}
bool ln_s5l_board_disk_screen(bool symbol_visible){
    uint16_t *frame=ln_display_framebuffer();
    if(!frame){if(!ln_display_init())return false;frame=ln_display_framebuffer();}
    if(!frame||!ln_display_quiesce())return false;
    struct ln_color_disk_decoder decoder={0};
    if(!ln_color_disk_expand(&decoder,frame,LINNET_DISPLAY_PIXELS,symbol_visible)||
       !ln_color_disk_finished(&decoder))return false;
    return present();
}
bool ln_s5l_board_recovery_screen(bool connected){
    if(!ln_lcd_fill(0x0841))return false;
    text(44,"Linnet",0xffff);
    if(connected){
        text(120,"Setup on computer",0xad75);
    }else{
        text(94,"Select: Start",0xad75);
        text(134,"Play: Disk Mode",0xad75);
        text(186,"Connect USB",0xad75);
    }
    return present();
}
bool ln_lcd_boot_menu(uint32_t available,uint32_t selected,bool starting){
    uint16_t *frame=ln_display_framebuffer();
    if(!frame){if(!ln_display_init())return false;frame=ln_display_framebuffer();}
    if(!frame||!ln_display_quiesce())return false;
    for(uint32_t y=0;y<LN_MENU_HEIGHT;++y)
        ln_menu_320_row(frame+y*LN_MENU_WIDTH,y,available,selected,starting);
    return present();
}
bool ln_lcd_fault(uint32_t reason,const uint8_t build[32]) {
    (void)build;
    if(!ln_lcd_fill(0x7800))return false;
    char code[]="Error 00000000";const char digits[]="0123456789ABCDEF";
    for(unsigned i=0;i<8;i++)code[6+i]=digits[(reason>>(28-i*4))&15];
    text(94,"Startup needs recovery",0xffff);text(150,code,0xffff);return present();
}
void ln_lcd_diagnostics(uint32_t out[8]) {
    if(!out)return;
    out[0]=LN_REG(0x38300000u);out[1]=LN_REG(0x3830001cu);out[2]=ln_display_init_error();
    out[3]=ln_display_busy();out[4]=ln_display_snapshot_crc32();
    out[5]=LN_REG(0x3cf000c4u);out[6]=LN_REG(0x38300020u);out[7]=ln_time_us();
}
