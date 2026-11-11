#ifndef LINNET_PHOTO_JPEG_H
#define LINNET_PHOTO_JPEG_H
#include <stdbool.h>
#include <stdint.h>
#define LN_PHOTO_JPEG_MAX_BYTES 262144u
#define LN_PHOTO_JPEG_MAX_DIMENSION 2048u
enum ln_photo_jpeg_result { LN_PHOTO_JPEG_PENDING=0,LN_PHOTO_JPEG_READY=1,
    LN_PHOTO_JPEG_CORRUPT=-1,LN_PHOTO_JPEG_UNSUPPORTED=-2,LN_PHOTO_JPEG_ARGUMENT=-3 };
struct ln_photo_jpeg_huffman {
    uint32_t first[17],end[17],offset[17];
    uint8_t symbols[256];bool present;
};
struct ln_photo_jpeg_component {uint8_t id,h,v,quant,dc,ac;int32_t predictor;};
struct ln_photo_jpeg {
    const uint8_t *source;uint16_t *surface;
    uint32_t bytes,cursor,markers,phase,width,height,output_width,output_height,stride;
    uint32_t image_width,image_height,left,top,clear_row,orientation;
    uint32_t components,max_h,max_v,mcu_x,mcu_y,mcu_columns,mcu_rows,component,block;
    uint32_t restart_interval,restart_count,restart_marker,bits,bit_count;
    uint32_t render_x0,render_x1,render_y,render_y1;
    uint16_t quant[4][64];uint8_t quant_present;
    struct ln_photo_jpeg_huffman huffman[2][4];
    struct ln_photo_jpeg_component channels[3];
    uint8_t planes[3][256],block_pixels[64];int32_t coefficients[64];
    bool frame_seen,scan_seen,adobe_seen;uint8_t adobe_transform;int error;
};
/* The caller owns immutable compressed bytes and a separate RGB565 surface
 * until READY/error. No allocation, source copy, I/O, globals or hidden yield.
 * Engine must finish/cancel its load before initialization or buffer reuse.
 * Each step handles one bounded marker, one entropy/IDCT block (<=64 coeffs),
 * one MCU setup, or one target-width output span. Call only in the photo scheduler's
 * admitted budget; simply stop stepping to suspend/cancel. Output is centered
 * aspect-fit with black margins, no upscale, and explicit EXIF orientation1..8.
 * Baseline 8-bit grayscale/YCbCr one interleaved scan, 1x1/2x1/1x2/2x2 luma;
 * progressive, CMYK, RGB Adobe, repeated tables and other coding modes return
 * UNSUPPORTED. Chroma reconstruction uses nearest-neighbor samples; the LPIC
 * compiler remains the high-quality scaling and normal interaction path.
 * Partial surface contents must not be presented before READY. */
int ln_photo_jpeg_init(struct ln_photo_jpeg *,const uint8_t *source,uint32_t bytes,
    uint16_t *surface,uint32_t width,uint32_t height,uint32_t stride,uint32_t orientation);
int ln_photo_jpeg_step(struct ln_photo_jpeg *);
#endif
