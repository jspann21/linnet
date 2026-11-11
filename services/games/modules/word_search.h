#ifndef LINNET_GAME_WORD_SEARCH_H
#define LINNET_GAME_WORD_SEARCH_H

/* Word Search */
static char ws_grid[120];
static uint8_t ws_w,ws_h,ws_count,ws_found,ws_cursor,ws_anchor,ws_level;
static uint8_t ws_begin[9],ws_end[9];
static uint16_t ws_found_mask;
static uint32_t ws_started_at,ws_elapsed;
static const char *const ws_words[3][9]={
    {"MUSIC","CLICK","PLAY","MENU","SONG",0,0,0,0},
    {"PODCAST","ALBUM","ARTIST","SHUFFLE","TRACK","PHOTO","VIDEO",0,0},
    {"PLAYLIST","HEADPHONE","CLASSIC","LIBRARY","COVERFLOW","BATTERY","SCREEN","WHEEL","LINNET"}};
static uint8_t ws_len(const char *s){uint8_t n=0;while(s[n])++n;return n;}
#if LN_GAME_PACKAGE_ID==11
#include "game_assets.h"
typedef char ws_prepared_size[(sizeof word_boards==3u*8u*138u)?1:-1];
#endif
static void ws_start_level(uint8_t level){
    ws_level=level;ws_w=level==0?8u:level==1?10u:12u;ws_h=level==0?8u:10u;ws_count=level==0?5u:level==1?7u:9u;
#if LN_GAME_PACKAGE_ID==11
    const uint8_t *board=word_boards+((uint32_t)level*8u+(random_next()&7u))*138u;
    for(uint8_t i=0;i<120u;++i)ws_grid[i]=(char)board[i];
    for(uint8_t i=0;i<9u;++i){ws_begin[i]=board[120u+i];ws_end[i]=board[129u+i];}
#endif
    ws_found_mask=0;ws_found=0;ws_cursor=0;ws_anchor=255;ws_elapsed=0;ws_started_at=0;ex_phase=EX_PLAY;
}
static bool ws_cell_found(uint8_t at){
    int8_t x=at%ws_w,y=at/ws_w;
    for(uint8_t w=0;w<ws_count;++w){
        if(!(ws_found_mask&(1u<<w)))continue;
        int8_t ax=ws_begin[w]%ws_w,ay=ws_begin[w]/ws_w,bx=ws_end[w]%ws_w,by=ws_end[w]/ws_w;
        int8_t dx=bx==ax?0:bx>ax?1:-1,dy=by==ay?0:by>ay?1:-1;uint8_t len=ws_len(ws_words[ws_level][w]);
        for(uint8_t i=0;i<len;++i)if(ax+dx*i==x&&ay+dy*i==y)return true;
    }
    return false;
}
static bool ws_cell_preview(uint8_t at){
    if(ws_anchor==255u)return false;
    int8_t ax=ws_anchor%ws_w,ay=ws_anchor/ws_w,bx=ws_cursor%ws_w,by=ws_cursor/ws_w;
    int8_t px=at%ws_w,py=at/ws_w,dx=bx==ax?0:bx>ax?1:-1,dy=by==ay?0:by>ay?1:-1;
    int16_t sx=bx-ax,sy=by-ay;if(ax!=bx&&ay!=by&&sx*sx!=sy*sy)return at==ws_anchor||at==ws_cursor;
    uint8_t distance=(uint8_t)(sx*sx>sy*sy?(sx>0?sx:-sx):(sy>0?sy:-sy));
    for(uint8_t i=0;i<=distance;++i)if(ax+dx*i==px&&ay+dy*i==py)return true;return false;
}
static void ws_select(void){
    if(ws_anchor==255u){ws_anchor=ws_cursor;sound_pending=1u;return;}
    bool matched=false;for(uint8_t w=0;w<ws_count;++w)if(!(ws_found_mask&(1u<<w))&&
        ((ws_anchor==ws_begin[w]&&ws_cursor==ws_end[w])||(ws_anchor==ws_end[w]&&ws_cursor==ws_begin[w]))){
        ws_found_mask|=(uint16_t)(1u<<w);++ws_found;matched=true;score=(uint16_t)(score+100u);break;}
    ws_anchor=255;if(matched){sound_pending=2u;if(ws_found==ws_count){ex_phase=EX_WON;sound_pending=4u;}}else sound_pending=3u;
}

#endif
