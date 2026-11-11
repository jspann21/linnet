#ifndef LINNET_GAME_DRAW_H
#define LINNET_GAME_DRAW_H
#include "graphics.h"
/* Prepared opaque RGB565 or packed visible spans. All offsets are host-built. */
struct ln_game_span {uint16_t x,y,length,padding;uint32_t offset;};
struct ln_game_sprite {
    uint16_t width,height;
    uint32_t span_count;
    const struct ln_game_span *spans;
    const uint16_t *pixels;
};
static inline void ln_game_fill(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                                int32_t width,int32_t height,uint16_t color){
    if(width<=0||height<=0)return;
    int64_t left=(int64_t)x+c->x,top=(int64_t)y+c->y,right=left+width,bottom=top+height;
    if(left<c->clip_x)left=c->clip_x;if(top<c->clip_y)top=c->clip_y;
    if(right>(int64_t)c->clip_x+c->clip_w)right=(int64_t)c->clip_x+c->clip_w;
    if(bottom>(int64_t)c->clip_y+c->clip_h)bottom=(int64_t)c->clip_y+c->clip_h;
    if(left>=right||top>=bottom)return;
    for(int32_t row=(int32_t)top;row<(int32_t)bottom;++row)
        for(int32_t at=(int32_t)left;at<(int32_t)right;++at)c->pixels[row*LINNET_UI_WIDTH+at]=color;
}
static inline void ln_game_sprite_run(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                                      const uint16_t *pixels,uint32_t count){
    if(y<c->clip_y||y>=c->clip_y+c->clip_h||x>=c->clip_x+c->clip_w)return;
    if(x<c->clip_x){uint64_t skip=(uint64_t)((int64_t)c->clip_x-x);if(skip>=count)return;pixels+=(uint32_t)skip;count-=(uint32_t)skip;x=c->clip_x;}
    uint32_t remaining=(uint32_t)(c->clip_x+c->clip_w-x);if(count>remaining)count=remaining;
    uint16_t *out=c->pixels+y*LINNET_UI_WIDTH+x;
    for(uint32_t i=0;i<count;++i)out[i]=pixels[i];
}
static inline void ln_game_blit(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                                const struct ln_game_sprite *sprite){
    if(!sprite||!sprite->pixels)return;
    int64_t left=(int64_t)x+c->x,top=(int64_t)y+c->y;
    if(left>=(int64_t)c->clip_x+c->clip_w||top>=(int64_t)c->clip_y+c->clip_h||
       left+sprite->width<=c->clip_x||top+sprite->height<=c->clip_y)return;
    x=(int32_t)left;y=(int32_t)top;
    if(sprite->spans){for(uint32_t i=0;i<sprite->span_count;++i){const struct ln_game_span *span=sprite->spans+i;
        ln_game_sprite_run(c,x+span->x,y+span->y,sprite->pixels+span->offset,span->length);}}
    else for(uint32_t row=0;row<sprite->height;++row)
        ln_game_sprite_run(c,x,y+(int32_t)row,sprite->pixels+row*sprite->width,sprite->width);
}
#endif
