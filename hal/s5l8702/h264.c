#include "services/video/native/h264.h"
#include "services/video/native/yuv.h"
#include "services/video/native/compat.h"
#include "hal/s5l8702/vpu.h"
#include "hal/arm926/cache.h"
#include "hal/api/platform.h"
#include "third_party/h264bsd/h264bsd_dpb.h"
#include "third_party/h264bsd/h264bsd_pic_order_cnt.h"
#define ARENA ((uint8_t *)(uintptr_t)(LINNET_STAGE_ADDRESS+0x200000u))
#define ARENA_BYTES (LINNET_STAGE_CAPACITY-0x200000u)
#define STREAM_BYTES 262144u
static uint32_t allocated,length_bytes,picture;
static bool initialized,dpb_ready,pending;
static uint8_t *pending_output;
static const uint8_t *ready_output;
static uint16_t *ready_pixels;
static uint32_t pending_started;
static seqParamSet_t sps;
static picParamSet_t pps;
static dpbStorage_t dpb;
static pocStorage_t poc;
static uint8_t *rbsp,*stream,*control;
static uint32_t (*descriptors)[8];
static sliceHeader_t headers[10];
static nalUnit_t units[10];
static uint32_t phase;
uint32_t ln_h264_decode_us;
void ln_h264_diagnostics(uint32_t out[3]){out[0]=phase;out[1]=ln_vpu_status;out[2]=ln_vpu_fault|(ln_vpu_clock()<<16);}
void ln_h264_output_size(uint32_t *w,uint32_t *h){ln_yuv_size(w,h);}
extern void ln_memory_copy(void *,const void *,uint32_t);
extern void ln_memory_clear(void *,uint32_t);
/* All allocations belong to one movie. Page alignment also satisfies the
 * VPU's output-plane requirements. Closing resets the entire arena. */
void *ln_h264_alloc(size_t bytes){
    uint32_t at=(allocated+4095u)&~4095u;
    if(!bytes||at>ARENA_BYTES||bytes>ARENA_BYTES-at)return 0;
    allocated=at+(uint32_t)bytes;return ARENA+at;
}
void ln_h264_free(void *p){(void)p;}
void *ln_h264_copy(void *to,const void *from,size_t bytes){ln_memory_copy(to,from,(uint32_t)bytes);return to;}
void *ln_h264_set(void *to,int value,size_t bytes){
    if(!value)ln_memory_clear(to,(uint32_t)bytes);
    else {uint8_t *p=to;while(bytes--)*p++=(uint8_t)value;}return to;
}
static bool unpack(const uint8_t *p,uint32_t bytes,strmData_t *s,nalUnit_t *unit){
    if(!bytes||bytes>STREAM_BYTES)return false;
    uint32_t count=0,zeros=0;
    for(uint32_t i=0;i<bytes;i++){
        uint8_t c=p[i];
        if(zeros==2&&c==3){if(i+1==bytes||p[i+1]>3)return false;zeros=0;continue;}
        rbsp[count++]=c;zeros=c==0?zeros+1:0;
    }
    *s=(strmData_t){rbsp,rbsp,0,count,0};
    return !h264bsdDecodeNalUnit(s,unit);
}
static uint32_t ebsp_offset(const uint8_t *p,uint32_t bytes,uint32_t rbsp_bytes){
    uint32_t zeros=0,count=0;
    for(uint32_t i=0;i<bytes;i++){
        if(zeros==2&&p[i]==3){zeros=0;continue;}
        if(count++==rbsp_bytes)return i;
        zeros=p[i]==0?zeros+1:0;
    }return bytes;
}
void ln_h264_close(void){ln_vpu_close();initialized=dpb_ready=pending=false;allocated=0;ready_output=0;ready_pixels=0;}
bool ln_h264_init(const uint8_t *avcc,uint32_t bytes,uint32_t w,uint32_t h){
    ln_h264_close();
    phase=1;
    if(bytes<7||avcc[0]!=1||avcc[1]!=66||!w||!h||w>640||h>480||(w|h)&1u)return false;
    sps=(seqParamSet_t){0};pps=(picParamSet_t){0};dpb=(dpbStorage_t){0};poc=(pocStorage_t){0};
    rbsp=ln_h264_alloc(STREAM_BYTES);stream=ln_h264_alloc(STREAM_BYTES);
    descriptors=ln_h264_alloc(320);if(!rbsp||!stream||!descriptors)return false;
    uint32_t at=6,count=avcc[5]&31;length_bytes=(avcc[4]&3)+1;
    phase=11;
    /* A new avcC configuration selects one active SPS/PPS pair. */
    for(unsigned group=0;group<2;group++){
        if(count!=1)return false;
        if(at+2>bytes)return false;uint32_t n=(uint32_t)avcc[at]<<8|avcc[at+1];at+=2;
        if(!n||n>bytes-at)return false;
        strmData_t bits;nalUnit_t unit;
        phase=12u+group;
        if(!unpack(avcc+at,n,&bits,&unit))return false;
        phase=14u+group;
        if(group==0){if(unit.nalUnitType!=7||h264bsdDecodeSeqParamSet(&bits,&sps))return false;}
        else if(unit.nalUnitType!=8||h264bsdDecodePicParamSet(&bits,&pps))return false;
        at+=n;if(!group){if(at>=bytes)return false;count=avcc[at++];}
    }
    uint32_t cw=sps.picWidthInMbs*16u,ch=sps.picHeightInMbs*16u;
    phase=2;
    if(sps.profileIdc!=66||sps.levelIdc>30||!cw||!ch||cw>640||ch>480||
       pps.seqParameterSetId!=sps.seqParameterSetId||pps.numSliceGroups!=1||pps.constrainedIntraPredFlag||
       cw<2u*(sps.frameCropLeftOffset+sps.frameCropRightOffset)||ch<2u*(sps.frameCropTopOffset+sps.frameCropBottomOffset)||
       cw-2u*(sps.frameCropLeftOffset+sps.frameCropRightOffset)!=w||ch-2u*(sps.frameCropTopOffset+sps.frameCropBottomOffset)!=h)return false;
    control=ln_h264_alloc(cw*ch*3u/2u);if(!control)return false;
    phase=3;
    ln_memory_clear(control,cw*ch*3u/2u);
    if(h264bsdInitDpb(&dpb,sps.picWidthInMbs*sps.picHeightInMbs,sps.maxDpbSize,sps.numRefFrames,sps.maxFrameNum,1))return false;
    dpb_ready=true;
    ln_arm926_data_clean_range(ARENA,allocated);
    picture=0;ln_h264_decode_us=ln_yuv_convert_us=0;
    if(!ln_yuv_init(w,h,2u*sps.frameCropLeftOffset,2u*sps.frameCropTopOffset))return false;
    phase=4;initialized=ln_vpu_open();return initialized;
}
void ln_h264_render(void){
    if(ready_output&&ready_pixels)ln_yuv_convert(ready_output,sps.picWidthInMbs*16u,sps.picHeightInMbs*16u,ready_pixels);
    ready_output=0;ready_pixels=0;
}
int ln_h264_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels){
    if(pending){
        int status=ln_vpu_poll();if(!status)return LN_H264_PENDING;
        pending=false;ln_h264_decode_us+=ln_time_us()-pending_started;if(status<0)return -1;
        phase=9;uint32_t luma=sps.picWidthInMbs*sps.picHeightInMbs*256u;
        if(!ln_arm926_data_invalidate_range(pending_output,luma*3u/2u))return -1;
        image_t image={.data=pending_output,.width=sps.picWidthInMbs,.height=sps.picHeightInMbs};
        int order=h264bsdDecodePicOrderCnt(&poc,&sps,&headers[0],&units[0]);
        if(h264bsdMarkDecRefPic(&dpb,units[0].nalRefIdc?&headers[0].decRefPicMarking:0,&image,headers[0].frameNum,order,units[0].nalUnitType==5,++picture,0))return -1;
        dpbOutPicture_t *shown=h264bsdDpbOutputPicture(&dpb);if(!shown)return -1;
        phase=10;ready_output=pixels?shown->data:0;ready_pixels=pixels;return 1;
    }
    phase=5;
    if(!initialized||!dpb_ready||bytes>STREAM_BYTES)return -1;
    uint32_t began=ln_time_us(),at=0,count=0,used=0,total=sps.picWidthInMbs*sps.picHeightInMbs;
    while(at<bytes){
        if(bytes-at<length_bytes)return -1;uint32_t n=0;
        for(unsigned j=0;j<length_bytes;j++)n=(n<<8)|packet[at++];
        if(!n||n>bytes-at)return -1;
        const uint8_t *p=packet+at;uint32_t type=p[0]&31;at+=n;
        if(type==6||type==9||type==12)continue;
        if((type!=1&&type!=5)||count==10)return -1;
        strmData_t bits;nalUnit_t *unit=&units[count];sliceHeader_t *h=&headers[count];
        *h=(sliceHeader_t){0};
        if(!unpack(p,n,&bits,unit)||h264bsdDecodeSliceHeader(&bits,h,&sps,&pps,unit)||h->picParameterSetId!=pps.picParameterSetId||h->firstMbInSlice>=total||h->redundantPicCnt)return -1;
        if((!count&&h->firstMbInSlice)||(count&&(h->firstMbInSlice<=headers[count-1].firstMbInSlice||h->frameNum!=headers[0].frameNum||unit->nalUnitType!=units[0].nalUnitType||unit->nalRefIdc!=units[0].nalRefIdc)))return -1;
        int qp=(int)pps.picInitQp+h->sliceQpDelta;if(qp<0||qp>51)return -1;
        uint32_t offset=ebsp_offset(p,n,bits.strmBuffReadBits/8u),start=(used+31u)&~31u;
        if(offset>=n||start>STREAM_BYTES||n-offset>STREAM_BYTES-start)return -1;
        ln_memory_copy(stream+start,p+offset,n-offset);used=start+n-offset;
        uint32_t *d=descriptors[count];
        d[0]=(h->firstMbInSlice<<21)|((h->sliceType%5u)<<6)|(uint32_t)qp;
        d[1]=((uint32_t)pps.chromaQpIndexOffset&255u)<<15|((h->numRefIdxL0Active-1u)&15u)<<10|h->disableDeblockingFilterIdc<<8|((uint32_t)(h->sliceAlphaC0Offset/2)&15u)<<4|((uint32_t)(h->sliceBetaOffset/2)&15u);
        d[2]=0x00110c85u;d[3]=d[7]=0;d[4]=bits.strmBuffReadBits&7u;d[5]=(uint32_t)(uintptr_t)(stream+start);d[6]=n-offset;
        if(count)descriptors[count-1][0]|=(h->firstMbInSlice-headers[count-1].firstMbInSlice)<<10;
        ++count;
    }
    if(!count)return 0;
    phase=6;
    descriptors[count-1][0]|=(total-headers[count-1].firstMbInSlice)<<10;
    bool idr=units[0].nalUnitType==5;
    if(!idr&&h264bsdCheckGapsInFrameNum(&dpb,headers[0].frameNum,units[0].nalRefIdc!=0,sps.gapsInFrameNumValueAllowedFlag))return -1;
    uint8_t *output=h264bsdAllocateDpbImage(&dpb);if(!output)return -1;
    /* A non-reference output can be reused immediately by the DPB. Finish
     * reading it before the VPU writes; reference pictures can overlap. */
    if(output==ready_output)ln_h264_render();
    phase=7;
    struct ln_vpu_picture job={.control=(uint32_t)(uintptr_t)control,.descriptors=(uint32_t)(uintptr_t)descriptors,.slices=count,.width_mbs=sps.picWidthInMbs,.height_mbs=sps.picHeightInMbs};
    uint32_t luma=total*256u;
    for(unsigned j=0;j<3;j++)job.output[j]=(uint32_t)(uintptr_t)output+(j?luma+(j-1)*luma/4u:0);
    /* The VPU has one reference table for the complete picture. Reject
     * differing slice lists instead of silently supplying wrong references. */
    for(unsigned slice=0;slice<count;slice++){
        h264bsdInitRefPicList(&dpb);
        if(IS_P_SLICE(headers[slice].sliceType)&&h264bsdReorderRefPicList(&dpb,&headers[slice].refPicListReordering,headers[slice].frameNum,headers[slice].numRefIdxL0Active))return -1;
        for(unsigned i=0;i<17;i++){
            uint8_t *ref=i<dpb.numRefFrames?h264bsdGetRefPicData(&dpb,i):0;
            if(!ref)ref=output;
            for(unsigned j=0;j<3;j++){
                uint32_t address=(uint32_t)(uintptr_t)ref+(j?luma+(j-1)*luma/4u:0);
                if(slice&&job.references[i][j]!=address)return -1;
                job.references[i][j]=address;
            }
        }
    }
    ln_arm926_data_clean_range(stream,used);ln_arm926_data_clean_range(descriptors,320);
    phase=8;
    if(!ln_vpu_start(&job))return -1;
    pending_output=output;pending_started=began;pending=true;ln_h264_render();return LN_H264_PENDING;
}

bool ln_h264_async(void){return true;}
