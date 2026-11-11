/* Shared 320 x 240 Boot chooser drawing for Classic and Video iPods.
 * Each row is independent, so framebuffer and scanline panels show the same UI. */
#ifndef LINNET_COLOR320_BOOT_MENU_H
#define LINNET_COLOR320_BOOT_MENU_H

#include <stdint.h>
#include "ui/profiles/color320/boot_menu_font.h"

#define LN_MENU_WIDTH 320u
#define LN_MENU_HEIGHT 240u
#define LN_MENU_BG 0x0841u
#define LN_MENU_BAR 0x1083u
#define LN_MENU_PANEL 0x10a4u
#define LN_MENU_SELECTED 0x118au
#define LN_MENU_CYAN 0x5e7eu
#define LN_MENU_RULE 0x328cu
#define LN_MENU_INK 0xefbfu
#define LN_MENU_MUTED 0xa5d8u

static inline uint32_t ln_menu_320_width(const char *message) {
    uint32_t width=0;
    for(;*message;++message)
        if(*message>=32&&*message<=126)
            width+=ln_boot_menu_advances[(uint8_t)*message-32u];
    return width;
}

static inline uint16_t ln_menu_320_mix(uint16_t background,uint16_t ink,uint32_t level) {
    if(level==3u)return ink;
    uint32_t weight=level==2u?3u:1u;
    uint32_t back=4u-weight;
    return (uint16_t)(((((background>>11&31u)*back+(ink>>11&31u)*weight+2u)>>2)<<11)|
        (((((background>>5&63u)*back+(ink>>5&63u)*weight+2u)>>2)<<5))|
        ((((background&31u)*back+(ink&31u)*weight+2u)>>2)));
}

static inline void ln_menu_320_text(uint16_t row[LN_MENU_WIDTH],uint32_t y,uint32_t top,
    uint32_t left,const char *message,uint16_t ink,uint16_t background) {
    if(y<top||y>=top+LINNET_BOOT_MENU_FONT_HEIGHT)return;
    uint16_t shades[4]={background,ln_menu_320_mix(background,ink,1u),
        ln_menu_320_mix(background,ink,2u),ink};
    for(;*message&&left<LN_MENU_WIDTH;++message) {
        uint8_t c=(uint8_t)*message;
        if(c<32u||c>126u)continue;
        uint32_t glyph=c-32u,index=ln_boot_menu_index[glyph];
        if(index==255u)continue;
        uint32_t base=(y-top)*LINNET_BOOT_MENU_FONT_WIDTH;
        for(uint32_t x=0;x<LINNET_BOOT_MENU_FONT_WIDTH&&left+x<LN_MENU_WIDTH;++x) {
            uint32_t pixel=base+x;
            uint8_t packed=ln_boot_menu_font[index][pixel>>2];
            uint32_t level=(packed>>(6u-2u*(pixel&3u)))&3u;
            if(level)row[left+x]=shades[level];
        }
        left+=ln_boot_menu_advances[glyph];
    }
}

static inline void ln_menu_320_row(uint16_t row[LN_MENU_WIDTH],uint32_t y,
    uint32_t available,uint32_t selected,uint32_t starting) {
    uint16_t background=y<47u?LN_MENU_BAR:y>=196u?LN_MENU_BAR:LN_MENU_BG;
    for(uint32_t x=0;x<LN_MENU_WIDTH;++x)row[x]=background;
    if(y==47u||y==195u)
        for(uint32_t x=20u;x<300u;++x)row[x]=LN_MENU_RULE;
    ln_menu_320_text(row,y,15u,22u,starting?"Starting":"Choose startup",LN_MENU_INK,LN_MENU_BAR);
    if(starting) {
        const char *name=selected==3u?"Apple iPod software":selected==2u?"Rockbox":"Linnet";
        uint32_t width=ln_menu_320_width(name);
        ln_menu_320_text(row,y,105u,(LN_MENU_WIDTH-width)/2u,name,LN_MENU_INK,LN_MENU_BG);
        if(y>=138u&&y<141u)
            for(uint32_t x=105u;x<215u;++x)row[x]=LN_MENU_CYAN;
        return;
    }
    if(!available) {
        const char *message="Checking startup files";
        uint32_t width=ln_menu_320_width(message);
        ln_menu_320_text(row,y,108u,(LN_MENU_WIDTH-width)/2u,message,LN_MENU_MUTED,LN_MENU_BG);
    }else{
        static const char *names[3]={"Linnet","Rockbox","Apple iPod software"};
        uint32_t visible=0u;
        for(uint32_t choice=1u;choice<=3u;++choice){
            if(!(available&(1u<<(choice-1u))))continue;
            uint32_t top=62u+visible*41u;
            uint16_t panel=choice==selected?LN_MENU_SELECTED:LN_MENU_PANEL;
            if(y>=top&&y<top+35u){
                for(uint32_t x=20u;x<300u;++x)row[x]=panel;
            }
            ln_menu_320_text(row,y,top+9u,38u,names[choice-1u],
                choice==selected?LN_MENU_INK:LN_MENU_MUTED,panel);
            ++visible;
        }
    }
    ln_menu_320_text(row,y,209u,22u,"Wheel: choose",LN_MENU_MUTED,LN_MENU_BAR);
    const char *action="Center: start";
    ln_menu_320_text(row,y,209u,LN_MENU_WIDTH-22u-ln_menu_320_width(action),
        action,LN_MENU_MUTED,LN_MENU_BAR);
}

#endif
