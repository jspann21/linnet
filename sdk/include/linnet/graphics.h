#ifndef LINNET_GRAPHICS_H
#define LINNET_GRAPHICS_H
#include <stdint.h>
/* Shared binary layout. Append incompatible changes only with an ABI bump. */
struct ln_ui_glyph {uint32_t offset;uint8_t x,y,w,h;};
_Static_assert(sizeof(struct ln_ui_glyph)==8u,"glyph layout");
struct ln_ui_font {
    const uint8_t *alpha;
    uint32_t width,height,advance,first,count;
    const uint8_t *advances;
    const struct ln_ui_glyph *glyphs;
};
struct ln_ui_canvas_context {
    uint16_t *pixels;
    int32_t x,y,w,h,clip_x,clip_y,clip_w,clip_h;
    uint32_t frame;
    uint16_t fg,bg;
};
#endif
