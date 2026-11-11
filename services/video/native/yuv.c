#include "yuv.h"
#include "hal/api/platform.h"
static uint32_t width,height,outw,outh;
static uint16_t red[1024],green[1024],blue[1024];
static uint8_t clip[1024];
static uint16_t xmap[LINNET_VIDEO_WIDTH],ymap[LINNET_VIDEO_HEIGHT];
#ifdef LINNET_ARM926_YUV
static struct {uint16_t x,count;bool half;} spans[LINNET_VIDEO_WIDTH/2u];
static uint32_t span_count;
#endif
uint32_t ln_yuv_convert_us;
bool ln_yuv_init(uint32_t w,uint32_t h,uint32_t left,uint32_t top){
    if(!w||!h||w>640||h>480||left>640-w||top>480-h)return false;
    for(unsigned i=0;i<1024;i++){
        unsigned c=i<288?0:i>543?255:i-288;
        clip[i]=(uint8_t)c;red[i]=(c>>3)<<11;green[i]=(c>>2)<<5;blue[i]=c>>3;
    }
    width=outw=w;height=outh=h;
    if(outw>LINNET_VIDEO_WIDTH){outh=outh*LINNET_VIDEO_WIDTH/outw;outw=LINNET_VIDEO_WIDTH;}
    if(outh>LINNET_VIDEO_HEIGHT){outw=outw*LINNET_VIDEO_HEIGHT/outh;outh=LINNET_VIDEO_HEIGHT;}
    if(!outw||!outh)return false;
    for(uint32_t x=0;x<outw;x++)xmap[x]=(uint16_t)(x*w/outw+left);
    for(uint32_t y=0;y<outh;y++)ymap[y]=(uint16_t)(y*h/outh+top);
#ifdef LINNET_ARM926_YUV
    span_count=0;
    if(!(outw&1u))for(uint32_t x=0;x<outw;){
        uint32_t end=x+1;
        while(end<outw&&xmap[end]==xmap[end-1]+2u)++end;
        uint32_t count=(end-x)&~1u;
        bool half=count>=8u;
        if(!half)count=2;
        if(!half&&span_count&&!spans[span_count-1].half)spans[span_count-1].count+=count;
        else {spans[span_count].x=x;spans[span_count].count=count;spans[span_count++].half=half;}
        x+=count;
    }
    if(span_count>32u)span_count=0;
#endif
    ln_yuv_convert_us=0;return true;
}
void ln_yuv_size(uint32_t *w,uint32_t *h){*w=outw;*h=outh;}
static uint16_t pixel(unsigned y,int r,int g,int b){
    int c=298*((int)y-16);
    return red[288+((c+r)>>8)]|green[288+((c+g)>>8)]|blue[288+((c+b)>>8)];
}
static void convert(const uint8_t *data,uint32_t stride,uint32_t chroma_offset,bool nv12,uint16_t *pixels){
    const uint8_t *u=data+chroma_offset,*v=nv12?u+1:u+chroma_offset/4u;
    uint32_t chroma_stride=nv12?stride:stride/2u,chroma_step=nv12?2u:1u;
    uint32_t began=ln_time_us();
#ifdef LINNET_ARM926_YUV
    if(width==outw*2u&&height==outh*2u&&!(outw&1u)){
        extern void ln_arm926_yuv_half_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *);
        extern void ln_arm926_nv12_half_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *);
        void (*row)(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *)=
            nv12?ln_arm926_nv12_half_row:ln_arm926_yuv_half_row;
        for(uint32_t y=0;y<outh;y++){
            uint32_t sy=ymap[y],sx=xmap[0],ci=(sy/2u)*chroma_stride+(sx/2u)*chroma_step;
            row(data+sy*stride+sx,u+ci,v+ci,pixels+y*outw,outw,clip+288);
        }
        ln_yuv_convert_us+=ln_time_us()-began;return;
    }
    extern void ln_arm926_yuv_mapped_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *,const uint16_t *);
    extern void ln_arm926_nv12_mapped_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *,const uint16_t *);
    void (*row)(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *,const uint16_t *)=
        nv12?ln_arm926_nv12_mapped_row:ln_arm926_yuv_mapped_row;
    for(uint32_t y=0;y<outh;y++){
        uint32_t sy=ymap[y],ci=(sy/2u)*chroma_stride;
        if(span_count){
            extern void ln_arm926_yuv_half_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *);
            extern void ln_arm926_nv12_half_row(const uint8_t *,const uint8_t *,const uint8_t *,uint16_t *,uint32_t,const uint8_t *);
            for(uint32_t i=0;i<span_count;i++){
                uint32_t x=spans[i].x,n=spans[i].count,sx=xmap[x],c=ci+(sx/2u)*chroma_step;
                if(spans[i].half){
                    if(nv12)ln_arm926_nv12_half_row(data+sy*stride+sx,u+c,v+c,pixels+y*outw+x,n,clip+288);
                    else ln_arm926_yuv_half_row(data+sy*stride+sx,u+c,v+c,pixels+y*outw+x,n,clip+288);
                }else row(data+sy*stride,u+ci,v+ci,pixels+y*outw+x,n,clip+288,xmap+x);
            }
        }else row(data+sy*stride,u+ci,v+ci,pixels+y*outw,outw,clip+288,xmap);
    }
    ln_yuv_convert_us+=ln_time_us()-began;return;
#endif
    typedef uint32_t pair __attribute__((may_alias));
    for(uint32_t y=0;y<outh;y++){
        uint32_t sy=ymap[y];uint16_t *row=pixels+y*outw;
        for(uint32_t x=0;x<outw;){
#ifdef LINNET_ARM926_YUV
            if(!(x&15u)&&x+16u<outw)__asm__ volatile("pld [%0]"::"r"(row+x+16u));
#endif
            uint32_t sx=xmap[x],ci=(sy/2u)*chroma_stride+(sx/2u)*chroma_step;
            int d=(int)u[ci]-128,e=(int)v[ci]-128;
            uint16_t first=pixel(data[sy*stride+sx],409*e+128,-100*d-208*e+128,516*d+128);
            if(x+1u<outw&&!((uintptr_t)(row+x)&3u)){
                sx=xmap[x+1u];ci=(sy/2u)*chroma_stride+(sx/2u)*chroma_step;d=(int)u[ci]-128;e=(int)v[ci]-128;
                uint16_t second=pixel(data[sy*stride+sx],409*e+128,-100*d-208*e+128,516*d+128);
                *(pair *)(void *)(row+x)=(uint32_t)first|((uint32_t)second<<16);x+=2;
            }else row[x++]=first;
        }
    }
    ln_yuv_convert_us+=ln_time_us()-began;
}

void ln_yuv_convert(const uint8_t *data,uint32_t stride,uint32_t rows,uint16_t *pixels){convert(data,stride,stride*rows,false,pixels);}
void ln_yuv_convert_nv12(const uint8_t *data,uint32_t stride,uint32_t chroma_offset,uint16_t *pixels){convert(data,stride,chroma_offset,true,pixels);}
