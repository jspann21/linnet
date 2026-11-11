#include <linnet/game.h>
#include "game_assets.h"
static const struct ln_game_host *host;
static uint32_t value,revision,buttons;
static bool start(void){value=host->best();if(value>100u)value=100u;revision=1;buttons=0;return true;}
static int32_t input(uint32_t kind,int32_t amount){
    if(kind==2u){uint32_t pressed=(uint32_t)amount&~buttons;buttons=(uint32_t)amount;
        if(pressed&LN_GAME_BUTTON_MENU)return 2;
        return 3;}
    if(kind!=1u||!amount)return 3;
    int64_t next=(int64_t)value+amount;if(next<0)next=0;if(next>100)next=100;
    if(value==(uint32_t)next)return 3;
    value=(uint32_t)next;++revision;(void)host->record_best(value);return 1;
}
static bool tick(uint32_t now){(void)now;return false;}
static uint32_t deadline(uint32_t now){(void)now;return UINT32_MAX;}
static bool playing(void){return false;}
static uint32_t changed(void){return revision;}
static bool cue(struct ln_game_cue *out){(void)out;return false;}
static void stop(void){}
static void draw(const struct ln_ui_canvas_context *c){
    /* Direct pixel loop: no per-pixel service call or source asset decoding. */
    for(int32_t y=c->clip_y;y<c->clip_y+c->clip_h;++y)
        for(int32_t x=c->clip_x;x<c->clip_x+c->clip_w;++x)
            c->pixels[y*LINNET_UI_WIDTH+x]=(x<(int32_t)(value*LINNET_UI_WIDTH/100u))?0x07e0u:0x0841u;
    ln_game_blit(c,8,8,&counter_marker);
}
static const struct ln_game_module module={LN_GAME_ABI,sizeof module,start,input,tick,deadline,draw,playing,changed,cue,stop};
const struct ln_game_module *ln_game_entry_point(const struct ln_game_host *api){
    if(!api||api->abi!=LN_GAME_ABI||api->bytes<sizeof *api)return 0;
    host=api;return &module;
}
