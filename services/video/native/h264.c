#include "h264.h"
#include "yuv.h"
#include "compat.h"
#include "hal/api/platform.h"
#include "third_party/h264bsd/h264bsd_decoder.h"
/* Native playback owns STAGE. Keep its first two MiB for compressed packets and
 * audio; decoder allocations are bounded by the remaining arena. */
#define POOL ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x200000u))
#define POOL_BYTES (LINNET_STAGE_CAPACITY-0x200000u)
struct block {uint32_t bytes,used;};
static storage_t decoder;
static uint32_t picture,width,height,nal_bytes;
static bool initialized;
static uint16_t red565[1024],green565[1024],blue565[1024];
typedef uint32_t pixel_pair __attribute__((may_alias));
static inline uint32_t pair(uint16_t first,uint16_t second){
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint32_t)first<<16|second;
#else
    return first|(uint32_t)second<<16;
#endif
}
uint32_t ln_h264_decode_us;
void ln_h264_diagnostics(uint32_t out[3]){out[0]=out[1]=out[2]=0;}
void ln_h264_output_size(uint32_t *w,uint32_t *h){*w=width;*h=height;}
extern void ln_memory_copy(void *,const void *,uint32_t);
extern void ln_memory_clear(void *,uint32_t);
void *ln_h264_copy(void *to,const void *from,size_t bytes){ln_memory_copy(to,from,(uint32_t)bytes);return to;}
void *ln_h264_set(void *to,int value,size_t bytes){
    if(!value)ln_memory_clear(to,(uint32_t)bytes);
    else {uint8_t *p=to;while(bytes--)*p++=(uint8_t)value;}return to;
}
void *ln_h264_alloc(size_t bytes){
    if(!bytes||bytes>POOL_BYTES-16)return 0;bytes=(bytes+7u)&~7u;
    for(uint32_t at=0;at<POOL_BYTES;){
        struct block *b=(struct block *)(POOL+at);
        if(!b->used&&b->bytes>=bytes){
            if(b->bytes>=bytes+16){struct block *next=(struct block *)(POOL+at+8+bytes);next->used=0;next->bytes=b->bytes-bytes-8;b->bytes=(uint32_t)bytes;}
            b->used=1;return b+1;
        }
        at+=8+b->bytes;
    }return 0;
}
void ln_h264_free(void *pointer){
    if(!pointer)return;uintptr_t p=(uintptr_t)pointer;
    if(p<(uintptr_t)POOL+8||p>=(uintptr_t)POOL+POOL_BYTES)return;
    ((struct block *)pointer-1)->used=0;
    for(uint32_t at=0;at<POOL_BYTES;){
        struct block *b=(struct block *)(POOL+at);uint32_t next=at+8+b->bytes;
        if(next>=POOL_BYTES)break;struct block *n=(struct block *)(POOL+next);
        if(!b->used&&!n->used)b->bytes+=8+n->bytes;else at=next;
    }
}
void ln_h264_close(void){if(initialized)h264bsdShutdown(&decoder);initialized=false;}
static inline uint16_t rgb565(int y,int red,int green,int blue){
    /* Full-range input bytes produce components in [-277,534]. The
     * biased tables exactly combine clipping and RGB565 packing. */
    int c=298*(y-16);
    return red565[288+((c+red)>>8)]|green565[288+((c+green)>>8)]|blue565[288+((c+blue)>>8)];
}
static int nal(uint8_t *data,uint32_t bytes,uint16_t *pixels){
    uint32_t at=0,stalls=0;int output=0;
    while(at<bytes){
        uint32_t used=0,began=ln_time_us();uint32_t r=h264bsdDecode(&decoder,data+at,bytes-at,picture,&used);
        ln_h264_decode_us+=ln_time_us()-began;
        if(used>bytes-at||r>=H264BSD_ERROR)return -1;
        if(!used){if(++stalls>4)return -1;}else stalls=0;
        at+=used;
        if(r==H264BSD_HDRS_RDY){
            if(h264bsdPicWidth(&decoder)*16u>LINNET_VIDEO_WIDTH||h264bsdPicHeight(&decoder)*16u>LINNET_VIDEO_HEIGHT||h264bsdProfile(&decoder)!=66)return -1;
        }
        if(r==H264BSD_PIC_RDY){
            uint32_t id,idr,errors;uint8_t *yuv=h264bsdNextOutputPicture(&decoder,&id,&idr,&errors);
            if(!yuv||errors)return -1;
            uint32_t stride=h264bsdPicWidth(&decoder)*16u,rows=h264bsdPicHeight(&decoder)*16u;
            uint32_t crop,left,w,top,h;h264bsdCroppingParams(&decoder,&crop,&left,&w,&top,&h);
            if(!crop){left=top=0;w=stride;h=rows;}
            if(w!=width||h!=height||left+w>stride||top+h>rows||((left|top|w|h)&1u))return -1;
            if(!pixels){output=1;continue;}
            const uint8_t *u=yuv+stride*rows,*v=u+stride*rows/4u;
            began=ln_time_us();
            for(uint32_t y=0;y<height;y+=2){
                const uint8_t *luma=yuv+(y+top)*stride+left;
                const uint8_t *cb=u+((y+top)/2)*(stride/2)+left/2,*cr=v+((y+top)/2)*(stride/2)+left/2;
                pixel_pair *out=(pixel_pair *)(void *)(pixels+y*width),*below=out+width/2;
                for(uint32_t x=0;x<width;x+=2){
                    if(!(x&15u)&&x+16u<width){__builtin_prefetch(out+x/2+8,1,3);__builtin_prefetch(below+x/2+8,1,3);}
                    int d=(int)*cb++-128,e=(int)*cr++-128;
                    int red=409*e+128,green=-100*d-208*e+128,blue=516*d+128;
                    out[x/2]=pair(rgb565(luma[x],red,green,blue),rgb565(luma[x+1],red,green,blue));
                    below[x/2]=pair(rgb565(luma[x+stride],red,green,blue),rgb565(luma[x+stride+1],red,green,blue));
                }
            }
            ln_yuv_convert_us+=ln_time_us()-began;
            output=1;
        }
    }return output;
}
bool ln_h264_init(const uint8_t *avcc,uint32_t bytes,uint32_t w,uint32_t h){
    ln_h264_close();
    if(bytes<7||avcc[0]!=1||avcc[1]!=66||!w||!h||w>LINNET_VIDEO_WIDTH||h>LINNET_VIDEO_HEIGHT)return false;
    for(unsigned i=0;i<1024;i++){
        unsigned component=i<288u?0:i>543u?255u:i-288u;
        red565[i]=(uint16_t)((component>>3)<<11);green565[i]=(uint16_t)((component>>2)<<5);blue565[i]=(uint16_t)(component>>3);
    }
    struct block *first=(struct block *)POOL;first->bytes=POOL_BYTES-8;first->used=0;
    if(h264bsdInit(&decoder,1))return false;initialized=true;picture=0;width=w;height=h;nal_bytes=(avcc[4]&3)+1;
    ln_h264_decode_us=ln_yuv_convert_us=0;
    uint32_t at=6,count=avcc[5]&31;uint8_t packet[516];
    for(unsigned group=0;group<2;group++){
        if(!count)return false;
        for(uint32_t i=0;i<count;i++){
            if(at+2>bytes)return false;uint32_t n=(uint32_t)avcc[at]<<8|avcc[at+1];at+=2;
            if(!n||n>512||n>bytes-at)return false;
            packet[0]=packet[1]=packet[2]=0;packet[3]=1;ln_h264_copy(packet+4,avcc+at,n);
            if(nal(packet,n+4,0)<0)return false;at+=n;
        }
        if(!group){if(at>=bytes)return false;count=avcc[at++];}
    }return true;
}
int ln_h264_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels){
    if(!initialized||nal_bytes!=4||(pixels&&((uintptr_t)pixels&3u)))return -1;
    uint32_t at=0;int result=0;++picture;
    while(at<bytes){
        if(bytes-at<4)return -1;uint32_t n=(uint32_t)packet[at]<<24|(uint32_t)packet[at+1]<<16|(uint32_t)packet[at+2]<<8|packet[at+3];
        if(!n||n>bytes-at-4)return -1;
        packet[at]=packet[at+1]=packet[at+2]=0;packet[at+3]=1;
        int r=nal(packet+at,n+4,pixels);if(r<0)return r;result|=r;at+=n+4;
    }return result;
}

void ln_h264_render(void){}
bool ln_h264_async(void){return false;}
