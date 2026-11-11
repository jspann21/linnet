/* Fixed-size, allocation-free arcade artwork. Uses the native antialiased fonts. */
static uint16_t arcade_mix(uint16_t a,uint16_t b,uint32_t t){
    uint32_t z=255u-t;
    return (uint16_t)(((((a>>11)*t+(b>>11)*z)/255u)<<11)|
        (((((a>>5)&63u)*t+((b>>5)&63u)*z)/255u)<<5)|(((a&31u)*t+(b&31u)*z)/255u));
}
static void arcade_type(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,const char *s,uint16_t ink,bool small,uint32_t size){
#if defined(LN_GAME_PACKAGE_ID)
    const struct ln_ui_font *f=size>1u?package_host->clock_font:small?package_host->hint_font:package_host->font;
#else
    const struct ln_ui_font *f=size>1u?&ln_ui_clock_font:small?&ln_ui_hint_font:&ln_ui_font;
#endif
    for(;*s;++s){uint32_t ch=(uint8_t)*s;if(ch<f->first||ch-f->first>=f->count)continue;
        uint32_t n=ch-f->first,w=f->width,h=f->height,offset=n*w*h;int32_t gx=x,gy=y;
        if(f->glyphs){const struct ln_ui_glyph *g=&f->glyphs[n];w=g->w;h=g->h;offset=g->offset;gx+=g->x;gy+=g->y;}
        for(uint32_t yy=0;yy<h;++yy)for(uint32_t xx=0;xx<w;++xx){int32_t px=gx+(int32_t)xx,py=gy+(int32_t)yy;
            if(px<0||px>=(int32_t)LINNET_UI_WIDTH||py<0||py>=(int32_t)LINNET_UI_HEIGHT||px<c->clip_x||px>=c->clip_x+c->clip_w||py<c->clip_y||py>=c->clip_y+c->clip_h||px<c->x||px>=c->x+c->w||py<c->y||py>=c->y+c->h)continue;
            uint32_t a=f->alpha[offset+yy*w+xx];if(a)c->pixels[py*LINNET_UI_WIDTH+px]=arcade_mix(ink,c->pixels[py*LINNET_UI_WIDTH+px],a);
        }
        x+=(int32_t)(f->advances?f->advances[n]:f->advance);
    }
}
static void arcade_text(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,const char *s,uint16_t ink,bool small){arcade_type(c,x,y,s,ink,small,1u);}
static void arcade_score_line(char out[32],bool round){
    char *p=out;
    if(round){append(&p,"ROUND +");number(&p,cw_award);append(&p,"  ");}
    append(&p,"SCORE ");number(&p,score);
    if(!round){append(&p,"  BEST ");number(&p,best[game]);}
    *p=0;
}
static __attribute__((noinline)) void arcade_center(const struct ln_ui_canvas_context *c,int32_t center,int32_t y,const char *s,uint16_t ink,bool small,uint32_t size){
#if defined(LN_GAME_PACKAGE_ID)
    const struct ln_ui_font *f=size>1u?package_host->clock_font:small?package_host->hint_font:package_host->font;
#else
    const struct ln_ui_font *f=size>1u?&ln_ui_clock_font:small?&ln_ui_hint_font:&ln_ui_font;
#endif
    int32_t width=0;
    for(const char *p=s;*p;++p){uint32_t ch=(uint8_t)*p;if(ch>=f->first&&ch-f->first<f->count)width+=(int32_t)(f->advances?f->advances[ch-f->first]:f->advance);}
    arcade_type(c,center-width/2,y,s,ink,small,size);
}
static __attribute__((noinline)) void arcade_panel(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t w,int32_t h,uint16_t ink){
    box(c,x+3,y,w-6,h,ink);box(c,x,y+3,w,h-6,ink);box(c,x+1,y+1,w-2,h-2,ink);
}
static __attribute__((noinline)) void arcade_disc(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t r,uint16_t ink){
    for(int32_t dy=-r;dy<=r;++dy){int32_t dx=r;while(dx*dx+dy*dy>r*r)--dx;box(c,x-dx,y+dy,dx*2+1,1,ink);}
}
static void arcade_shade(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t w,int32_t h,uint16_t top,uint16_t bottom){
    /* Ordered RGB565 rounding keeps the dark gradients free of broad bands. */
    static const uint8_t dither[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    int32_t left=x>c->clip_x?x:c->clip_x,right=x+w<c->clip_x+c->clip_w?x+w:c->clip_x+c->clip_w;
    if(left<c->x)left=c->x;if(right>c->x+c->w)right=c->x+c->w;
    if(left<0)left=0;if(right>(int32_t)LINNET_UI_WIDTH)right=LINNET_UI_WIDTH;
    for(int32_t row=0;row<h;++row){int32_t yy=y+row;
        if(yy<0||yy>=(int32_t)LINNET_UI_HEIGHT||yy<c->clip_y||yy>=c->clip_y+c->clip_h||yy<c->y||yy>=c->y+c->h)continue;
        int32_t r=(top>>11)*256+((int32_t)(bottom>>11)-(top>>11))*256*row/(h-1);
        int32_t g=((top>>5)&63)*256+((int32_t)((bottom>>5)&63)-((top>>5)&63))*256*row/(h-1);
        int32_t b=(top&31)*256+((int32_t)(bottom&31)-(top&31))*256*row/(h-1);
        uint16_t inks[4];for(uint32_t k=0;k<4u;++k){int32_t d=dither[yy&3][k]*16;
            inks[k]=(uint16_t)((((r+d)>>8)<<11)|(((g+d)>>8)<<5)|((b+d)>>8));}
        for(int32_t xx=left;xx<right;++xx)c->pixels[yy*LINNET_UI_WIDTH+xx]=inks[xx&3];
    }
}
