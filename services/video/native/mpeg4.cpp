#include "mpeg4.h"
#include "mpeg4_memory.h"
#include "yuv.h"
#include "linnet_target.h"
#include "third_party/pvmp4/mp4dec_api.h"
extern "C" {
uint32_t ln_time_us(void);
void ln_memory_copy(void *,const void *,uint32_t);
void ln_memory_clear(void *,uint32_t);
}
static VideoDecControls decoder;
static uint32_t width,height,stride,rows,index;
static uint8_t *frames[2],*input;
static uint8_t configuration[576];
static bool initialized;
uint32_t ln_mpeg4_decode_us;
void ln_mpeg4_close(void){
    if(initialized)(void)PVCleanUpVideoDecoder(&decoder);
    initialized=false;ln_mpeg4_memory_reset();decoder={};
}
bool ln_mpeg4_init(const uint8_t *config,uint32_t bytes,uint32_t w,uint32_t h){
    ln_mpeg4_close();
    if(!config||!bytes||bytes>512||!w||!h||w>640||h>480||(w|h)&1u)return false;
    ln_memory_clear(configuration,sizeof configuration);ln_memory_copy(configuration,config,bytes);
    uint8_t *buffers[]={configuration};int32 sizes[]={(int32)bytes};
    if(!PVInitVideoDecoder(&decoder,buffers,sizes,1,(int)w,(int)h,MPEG4_MODE))return false;
    initialized=true;
    int32 display_w=0,display_h=0;PVGetVideoDimensions(&decoder,&display_w,&display_h);
    width=w;height=h;stride=(w+15u)&~15u;rows=(h+15u)&~15u;
    if(display_w!=(int32)w||display_h!=(int32)h||decoder.size!=(int32)(stride*rows)||
       PVGetDecBitstreamMode(&decoder)!=MPEG4_MODE)return false;
    for(unsigned i=0;i<2;i++)frames[i]=(uint8_t *)ln_mpeg4_alloc(stride*rows*3u/2u+64u);
    input=(uint8_t *)ln_mpeg4_alloc(65536u+64u);
    if(!frames[0]||!frames[1]||!input||!ln_yuv_init(w,h,0,0))return false;
    PVSetReferenceYUV(&decoder,frames[1]);index=0;ln_mpeg4_decode_us=0;return true;
}
int ln_mpeg4_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels){
    if(!initialized||!packet||!bytes||bytes>65536)return -1;
    uint32_t began=ln_time_us();
    ln_memory_copy(input,packet,bytes);ln_memory_clear(input+bytes,64);
    uint8_t *buffers[]={input};int32 sizes[]={(int32)bytes};uint32 stamp[]={0};uint external[]={1};
    VopHeaderInfo header={};
    if(!PVDecodeVopHeader(&decoder,buffers,stamp,sizes,&header,external,frames[index])||
       (header.frameType!=MP4_I_FRAME&&header.frameType!=MP4_P_FRAME))return -1;
    int32 w=0,h=0;PVGetVideoDimensions(&decoder,&w,&h);
    if(w!=(int32)width||h!=(int32)height||decoder.size!=(int32)(stride*rows)||
       !PVDecodeVopBody(&decoder,sizes)||!decoder.outputFrame)return -1;
    ln_mpeg4_decode_us+=ln_time_us()-began;
    if(pixels)ln_yuv_convert(decoder.outputFrame,stride,rows,pixels);
    index^=1u;return 1;
}

bool ln_mpeg4_async(void){return false;}
void ln_mpeg4_render(void){}
void ln_mpeg4_diagnostics(uint32_t out[3]){out[0]=0x6d703476u;out[1]=out[2]=0;}
