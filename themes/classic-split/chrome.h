/* Integer shapes remain crisp on RGB565 and the binary monochrome theme. */
static void split_triangle(struct painter*p,int32_t x,int32_t y,uint16_t ink){
    static const uint8_t heights[5]={7,7,5,3,1};
    for(int32_t column=0;column<5;++column)
        fill(p,(struct rect){x+column,y+(7-heights[column])/2,1,heights[column]},ink);
}
/* Host-authored 16-pixel masks, ordered Shuffle, Photos, Videos, Cover Flow,
 * Settings, Extras, Files, Notes, Music. Runtime only scales integer runs. */
#include "themes/classic-split/music.h"
static const uint16_t split_icons[8][16]={
    {0x0000,0x0000,0x3c00,0x300c,0x381c,0x3c30,0x0660,0x03c0,0x0180,0x03c0,0x3660,0x3c30,0x381c,0x3c0c,0x0000,0x0000},
    {0x0000,0x0000,0x7ffe,0x4002,0x4c02,0x4c02,0x4002,0x4002,0x4182,0x47c2,0x5fe2,0x5ff2,0x5ffa,0x7ffe,0x0000,0x0000},
    {0x0000,0x0000,0x7ffe,0x4002,0x700e,0x4042,0x40c2,0x71ce,0x43c2,0x41c2,0x70ce,0x4042,0x4002,0x7ffe,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x0ff0,0x0810,0xfbdf,0x9bd9,0x9bd9,0x9bd9,0x9bd9,0x9819,0xf81f,0x0810,0x0ff0,0x0000,0x0000},
    {0x0000,0x03c0,0x03c0,0x1bd8,0x1ff8,0x0810,0x7bde,0x7a5e,0x7a5e,0x7bde,0x0810,0x1ff8,0x1bd8,0x03c0,0x03c0,0x0000},
    {0x0000,0x0000,0x07e0,0x0420,0x0420,0x3ffc,0x2004,0x2184,0x3ffc,0x2184,0x2004,0x2004,0x2004,0x3ffc,0x0000,0x0000},
    {0x0000,0x0000,0x0000,0x007c,0x01fc,0x7ffe,0x7ffe,0x4002,0x4002,0x4002,0x4002,0x4002,0x4002,0x7ffe,0x0000,0x0000},
    {0x0000,0x1ff8,0x1008,0x1008,0x17e8,0x1008,0x1008,0x17e8,0x1008,0x1008,0x17e8,0x1008,0x1008,0x1008,0x1ff8,0x0000},
};
static void split_section(struct painter*p,struct rect pane,uint32_t target){
    int32_t scale=LINNET_UI_WIDTH==320u?3:2;
    int32_t left=pane.x+(pane.w-16*scale)/2,top=pane.y+(pane.h-16*scale)/2;
    uint32_t icon=target==LINNET_UI_ACTION_SHUFFLE_ALL?0u:
        target==LN_UI_SCENE_PHOTOS?1u:target==LN_UI_SCENE_VIDEOS?2u:
        target==LN_UI_SCENE_COVER_FLOW?3u:target==LN_UI_SCENE_SETTINGS?4u:
        target==LN_UI_SCENE_EXTRAS||target==LN_UI_SCENE_GAMES?5u:
        target==LN_UI_SCENE_FILES?6u:target==LN_UI_SCENE_NOTES?7u:8u;
    uint16_t ink=color(LN_UI_ARTWORK_FG);
    if(icon==8u){
        struct rect r=intersect(p->clip,(struct rect){left,top,SPLIT_MUSIC_SIDE,SPLIT_MUSIC_SIDE});
        bool mono=(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)!=0;
        for(int32_t y=r.y;y<r.y+r.h;++y)for(int32_t x=r.x;x<r.x+r.w;++x){
            uint32_t index=(uint32_t)(y-top)*SPLIT_MUSIC_SIDE+(uint32_t)(x-left);
            uint32_t alpha=((split_music_alpha[index/2]>>((index&1u)*4))&15u)*17u;
            if(mono)alpha=alpha>=128u?255u:0u;
            uint16_t*pixel=&p->pixels[(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)x];
            if(alpha)*pixel=alpha==255u?ink:mix(ink,*pixel,alpha);
        }
        return;
    }
    for(int32_t y=0;y<16;++y){uint32_t bits=split_icons[icon][y];
        for(int32_t x=0;x<16;){
            if(!(bits&(1u<<x))){++x;continue;}
            int32_t first=x;do{++x;}while(x<16&&(bits&(1u<<x)));
            fill(p,(struct rect){left+first*scale,top+y*scale,(x-first)*scale,scale},ink);
        }
    }
}
