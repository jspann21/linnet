#ifndef LINNET_PP502X_DISK_SCREEN_H
#define LINNET_PP502X_DISK_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

/* A single, fixed image for the exclusive USB storage owner. Pixel generation
 * uses no UI state, framebuffer, cache, interrupts, or filesystem access. */
static uint8_t ln_pp_disk_glyph(char c, uint32_t row)
{
    static const uint8_t letters[11][7] = {
        {0,0,0,0,0,0,0},          /* space */
        {14,17,16,16,16,17,14}, /* C */
        {30,17,17,17,17,17,30}, /* D */
        {31,16,16,30,16,16,31}, /* E */
        {14,4,4,4,4,4,14},      /* I */
        {17,18,20,24,20,18,17},/* K */
        {16,16,16,16,16,16,31},/* L */
        {17,27,21,21,17,17,17},/* M */
        {17,25,21,19,17,17,17},/* N */
        {14,17,17,17,17,17,14},/* O */
        {15,16,16,14,1,1,30}   /* S */
    };
    static const uint8_t t[7] = {31,4,4,4,4,4,4};
    static const char names[] = " CDEIKLMNOS";
    if (c == 'T') return t[row];
    for (uint32_t i = 0; i < sizeof names - 1u; ++i)
        if (c == names[i]) return letters[i][row];
    return 0;
}

static bool ln_pp_disk_text(const char *s, uint32_t count, uint32_t left,
                            uint32_t top, uint32_t scale, uint32_t x, uint32_t y)
{
    if (x < left || y < top) return false;
    x -= left; y -= top;
    if (x >= count * 6u * scale || y >= 7u * scale) return false;
    uint32_t column = scale == 2u ? x >> 1 : x;
    if (column % 6u == 5u) return false;
    uint32_t row = scale == 2u ? y >> 1 : y;
    return (ln_pp_disk_glyph(s[column / 6u], row) & (16u >> (column % 6u))) != 0u;
}

static uint16_t ln_pp_disk_screen_pixel(uint32_t width, uint32_t height,
                                        uint32_t x, uint32_t y, bool symbol_visible)
{
    uint32_t sx = x * 160u / width, sy = y * 128u / height;
    uint16_t ink = 0x2104u;
    uint16_t color = 0xffffu;
    if (sy < 28u) color = 0xe71cu;
    if (sy == 27u || sy == 127u) color = 0x9cd3u;
    if (ln_pp_disk_text("LINNET DISK MODE", 16u, 32u, 11u, 1u, sx, sy)) return ink;

    int32_t dx = (int32_t)sx - 80, dy = (int32_t)sy - 68;
    int32_t distance = dx * dx + dy * dy;
    if (symbol_visible) {
        if (distance >= 27 * 27 && distance <= 30 * 30) color = ink;
        if (distance < 29 * 29 && dx + dy >= -3 && dx + dy <= 3) color = ink;
    }
    if (ln_pp_disk_text("DO NOT DISCONNECT", 17u, 29u, 107u, 1u, sx, sy))
        color = ink;
    return color;
}

#endif
