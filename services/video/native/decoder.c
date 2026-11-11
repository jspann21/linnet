#include "decoder.h"
#include "h264.h"
#include "yuv.h"
#ifdef LINNET_MPEG4
#include "mpeg4.h"
#endif
static uint32_t codec;
void ln_video_decoder_close(void){
    ln_h264_close();
#ifdef LINNET_MPEG4
    ln_mpeg4_close();
#endif
}
bool ln_video_decoder_init(uint32_t kind,const uint8_t *config,uint32_t bytes,uint32_t w,uint32_t h){
    ln_video_decoder_close();codec=kind;
    if(codec==0x61766331u)return ln_h264_init(config,bytes,w,h);
#ifdef LINNET_MPEG4
    if(codec==0x6d703476u)return ln_mpeg4_init(config,bytes,w,h);
#endif
    return false;
}
int ln_video_decoder_decode(uint8_t *packet,uint32_t bytes,uint16_t *pixels){
    if(codec==0x61766331u)return ln_h264_decode(packet,bytes,pixels);
#ifdef LINNET_MPEG4
    if(codec==0x6d703476u)return ln_mpeg4_decode(packet,bytes,pixels);
#endif
    return -1;
}
bool ln_video_decoder_async(void){
#ifdef LINNET_MPEG4
    if(codec==0x6d703476u)return ln_mpeg4_async();
#endif
    return codec==0x61766331u&&ln_h264_async();
}
void ln_video_decoder_render(void){
    if(codec==0x61766331u)ln_h264_render();
#ifdef LINNET_MPEG4
    else if(codec==0x6d703476u)ln_mpeg4_render();
#endif
}
void ln_video_decoder_output_size(uint32_t *w,uint32_t *h){
    if(codec==0x61766331u)ln_h264_output_size(w,h);else ln_yuv_size(w,h);
}
void ln_video_decoder_diagnostics(uint32_t out[3]){
    if(codec==0x61766331u)ln_h264_diagnostics(out);
#ifdef LINNET_MPEG4
    else if(codec==0x6d703476u)ln_mpeg4_diagnostics(out);
#endif
    else {out[0]=codec;out[1]=out[2]=0;}
}
uint32_t ln_video_decoder_time(void){
#ifdef LINNET_MPEG4
    if(codec==0x6d703476u)return ln_mpeg4_decode_us;
#endif
    return ln_h264_decode_us;
}
