#include "linnet_target.h"
#include "services/video/native/mpeg4.h"
#include "services/video/native/mpeg4_memory.h"
#include "services/video/native/yuv.h"
#include "third_party/pvmp4/mp4dec_lib.h"
#include "mpeg4_vpu.h"
extern "C" {
#include "hal/arm926/cache.h"
uint32_t ln_time_us(void);
void ln_memory_copy(void *,const void *,uint32_t);
void ln_memory_clear(void *,uint32_t);
}
static VideoDecControls decoder;
static uint32_t width,height,stride,rows,chroma,frame_bytes,index,started;
static uint8_t *frames[2],*input,*work,*ready_frame;
static uint16_t *ready_pixels,*pending_pixels;
static uint8_t configuration[576];
static bool initialized,pending,reference;
uint32_t ln_mpeg4_decode_us;
bool ln_mpeg4_async(void){return true;}
void ln_mpeg4_render(void){
    if(ready_pixels)ln_yuv_convert_nv12(ready_frame,stride,chroma,ready_pixels);
    ready_frame=0;ready_pixels=0;
}
void ln_mpeg4_diagnostics(uint32_t out[3]){
    out[0]=ln_mpeg4_vpu_status;out[1]=ln_mpeg4_vpu_fault;out[2]=(ln_mpeg4_vpu_vld<<1)|pending;
}
void ln_mpeg4_close(void){
    ln_mpeg4_vpu_close();
    if(initialized)(void)PVCleanUpVideoDecoder(&decoder);
    initialized=pending=reference=false;ready_frame=0;ready_pixels=pending_pixels=0;
    decoder={};ln_mpeg4_memory_reset();
}
bool ln_mpeg4_init(const uint8_t *config,uint32_t bytes,uint32_t w,uint32_t h){
    ln_mpeg4_close();
    if(!config||!bytes||bytes>512||!w||!h||w>640||h>480||((w|h)&1u))return false;
    ln_memory_clear(configuration,sizeof configuration);ln_memory_copy(configuration,config,bytes);
    uint8_t *buffers[]={configuration};int32 sizes[]={(int32)bytes};
    if(!PVInitVideoDecoder(&decoder,buffers,sizes,1,(int)w,(int)h,MPEG4_MODE))return false;
    initialized=true;width=w;height=h;stride=(w+15u)&~15u;rows=(h+15u)&~15u;
    VideoDecData *v=(VideoDecData *)decoder.videoDecoderData;Vol *vol=v->vol[0];
    if(v->displayWidth!=(int32)w||v->displayHeight!=(int32)h||v->shortVideoHeader||
       vol->quantType||vol->dataPartitioning||vol->scalability)return false;
    /* VPU-A plane addresses omit their low address bits. Keep each plane
     * and every DMA work area aligned independently, including odd MB sizes. */
    chroma=(stride*rows+1023u)&~1023u;
    frame_bytes=(chroma+stride*rows/2u+31u)&~31u;
    for(unsigned i=0;i<2;i++)frames[i]=(uint8_t *)ln_mpeg4_alloc_aligned(frame_bytes,1024);
    input=(uint8_t *)ln_mpeg4_alloc(LINNET_VIDEO_PACKET_BYTES+320u);
    work=(uint8_t *)ln_mpeg4_alloc_aligned(1024u+0x40000u,1024);
    if(!frames[0]||!frames[1]||!input||!work||!ln_yuv_init(w,h,0,0))return false;
    ln_memory_clear(frames[0],frame_bytes);ln_memory_clear(frames[1],frame_bytes);
    ln_memory_clear(work,1024u+0x40000u);ln_arm926_data_clean();
    index=0;ln_mpeg4_decode_us=0;return ln_mpeg4_vpu_open();
}
static int complete(void){
    VideoDecData *v=(VideoDecData *)decoder.videoDecoderData;
    Vop *old=v->prevVop;v->prevVop=v->currVop;v->currVop=old;
    ready_frame=frames[index];ready_pixels=pending_pixels;index^=1u;
    pending=false;reference=true;ln_mpeg4_decode_us+=ln_time_us()-started;return 1;
}
int ln_mpeg4_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels){
    if(!initialized||!packet||!bytes||bytes>LINNET_VIDEO_PACKET_BYTES)return -1;
    if(pending){
        int result=ln_mpeg4_vpu_poll();if(!result)return 2;if(result<0){pending=false;return -1;}
        if(!ln_arm926_data_invalidate_range(frames[index],frame_bytes))return -1;
        return complete();
    }
    if(ready_frame==frames[index])ln_mpeg4_render();
    started=ln_time_us();ln_memory_copy(input,packet,bytes);ln_memory_clear(input+bytes,320);
    uint8_t *buffers[]={input};int32 sizes[]={(int32)bytes};uint32 stamp[]={0};uint external[]={1};
    VopHeaderInfo header={};
    if(!PVDecodeVopHeader(&decoder,buffers,stamp,sizes,&header,external,frames[index]))return -1;
    VideoDecData *v=(VideoDecData *)decoder.videoDecoderData;Vol *vol=v->vol[0];Vop *pic=v->currVop;
    if(v->displayWidth!=(int32)width||v->displayHeight!=(int32)height||
       (pic->predictionType!=I_VOP&&pic->predictionType!=P_VOP)||
       (!reference&&pic->predictionType!=I_VOP)||v->shortVideoHeader||vol->quantType||
       vol->dataPartitioning||vol->scalability)return -1;
    pending_pixels=pixels;
    if(!pic->vopCoded){
        if(!reference)return -1;
        ln_mpeg4_render();ln_memory_copy(frames[index],frames[index^1u],frame_bytes);
        ln_arm926_data_clean_range(frames[index],frame_bytes);return complete();
    }
    struct ln_mpeg4_picture job={};
    job.input=(uint32_t)(uintptr_t)input;job.bytes=bytes;job.bits=vol->bitstream->bitcnt;
    job.output=(uint32_t)(uintptr_t)frames[index];job.reference=(uint32_t)(uintptr_t)frames[index^1u];
    job.work=(uint32_t)(uintptr_t)work;job.width=stride;job.height=rows;job.chroma_offset=chroma;
    job.type=pic->predictionType;job.rounding=pic->roundingType;job.quantizer=pic->quantizer;
    job.fcode=pic->predictionType?pic->fcodeForward:1;job.dc_threshold=pic->intraDCVlcThr;
    job.time_increment=pic->timeInc;job.time_bits=vol->nbitsTimeIncRes;
    ln_arm926_data_clean_range(input,bytes+320);
    if(!ln_mpeg4_vpu_start(&job))return -1;
    pending=true;ln_mpeg4_render();return 2;
}
