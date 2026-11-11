/* Shared native primitives for the five click-wheel games. */
static const int8_t cw_x32[32]={0,12,24,36,45,53,59,63,64,63,59,53,45,36,24,12,
    0,-12,-24,-36,-45,-53,-59,-63,-64,-63,-59,-53,-45,-36,-24,-12};
static const int8_t cw_y32[32]={-64,-63,-59,-53,-45,-36,-24,-12,0,12,24,36,45,53,59,63,
    64,63,59,53,45,36,24,12,0,-12,-24,-36,-45,-53,-59,-63};
static const int8_t cw_x12[12]={0,32,55,64,55,32,0,-32,-55,-64,-55,-32};
static const int8_t cw_y12[12]={-64,-55,-32,0,32,55,64,55,32,0,-32,-55};
static __attribute__((noinline)) void cw_line(const struct ln_ui_canvas_context *c,int32_t x0,int32_t y0,
                    int32_t x1,int32_t y1,uint16_t ink){
    int32_t dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y1-y0:y0-y1;
    int32_t sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for(;;){
        box(c,x0,y0,1,1,ink);
        if(x0==x1&&y0==y1)break;
        int32_t twice=err*2;
        if(twice> -dy){err-=dy;x0+=sx;}
        if(twice<dx){err+=dx;y0+=sy;}
    }
}
static void cw_text_number(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                           const char *label,uint32_t value,uint16_t ink){
    char line[24],*p=line;append(&p,label);number(&p,value);*p=0;
    arcade_text(c,x,y,line,ink,true);
}
static void cw_header(const struct ln_ui_canvas_context *c,const char *title,
                      const char *subtitle,uint8_t stage,uint8_t total,uint16_t accent){
    box(c,0,0,320,240,0x0825u);box(c,0,0,320,40,0x10a9u);
    box(c,0,39,320,1,0x2a2fu);
    arcade_text(c,13,5,title,0xf7deu,false);
    char line[20],*p=line;append(&p,game==POCKET_PINBALL?"BALL ":"STAGE ");
    number(&p,stage);append(&p,"/");number(&p,total);*p=0;
    arcade_text(c,game==POCKET_PINBALL?252:250,7,line,accent,true);
    if(games_phase==PLAY){char line[32];arcade_score_line(line,false);
        arcade_center(c,160,27,line,accent,true,1u);}
    else arcade_center(c,160,27,subtitle,accent,true,1u);
}
static void cw_footer(const struct ln_ui_canvas_context *c,const char *hint){
    box(c,0,211,320,1,0x2a2fu);box(c,0,212,320,28,0x10a9u);
    arcade_center(c,160,220,hint,0xf7deu,true,1u);
}
static void cw_overlay(const struct ln_ui_canvas_context *c,const char *title,
                       const char *a,const char *b,const char *d,const char *action,
                       uint16_t accent){
    arcade_panel(c,24,49,272,147,accent);
    arcade_panel(c,26,51,268,143,0x18ebu);
    box(c,29,54,262,137,0x10a9u);
    arcade_center(c,160,63,title,accent,false,1u);
    arcade_center(c,160,92,a,0xf7deu,true,1u);
    arcade_center(c,160,109,b,0xc638u,true,1u);
    arcade_center(c,160,126,d,0xc638u,true,1u);
    arcade_panel(c,52,156,216,27,accent);
    arcade_center(c,160,162,action,0x0825u,false,1u);
}
static void cw_meter(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                     int32_t width,uint8_t value,uint8_t maximum,uint16_t accent){
    box(c,x,y,width,7,0x294bu);
    if(maximum&&value)box(c,x,y,(int32_t)value*width/maximum,7,accent);
}
#include "services/games/presentations/dial_heist.h"
#include "services/games/presentations/mixtape.h"
#include "services/games/presentations/signal.h"
#include "services/games/presentations/clockwork_city.h"
#include "services/games/presentations/pocket_pinball.h"
static void cw_draw(const struct ln_ui_canvas_context *c){
    if(game==DIAL_HEIST)dh_draw(c);
    else if(game==MIXTAPE)mt_draw(c);
    else if(game==SIGNAL)sg_draw(c);
    else if(game==CLOCKWORK_CITY)cc_draw(c);
    else pp_draw(c);
}
