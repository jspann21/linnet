#ifndef LINNET_COLOR320_DISK_SCREEN_H
#define LINNET_COLOR320_DISK_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/profiles/color320/disk_screen_runs.h"

/* Decode a fixed 320x240 image directly into a row or the existing display
 * framebuffer. Disk Mode needs no extra framebuffer or filesystem work. */
struct ln_color_disk_decoder {
    uint32_t run;
    uint32_t remaining;
    uint16_t color;
};

/* The generated symbol is the only warm-colored part of this artwork. */
static bool ln_color_disk_symbol_tint(uint16_t color)
{
    return (((color >> 11) & 31u) * 2u) > (((color >> 5) & 63u) + 2u);
}

static bool ln_color_disk_expand(struct ln_color_disk_decoder *state,
                                 uint16_t *pixels, uint32_t count,
                                 bool symbol_visible)
{
    if (!state || !pixels) return false;
    while (count) {
        if (!state->remaining) {
            if (state->run >= sizeof ln_color_disk_runs / sizeof ln_color_disk_runs[0])
                return false;
            uint32_t packed = ln_color_disk_runs[state->run++];
            uint32_t index = packed & 255u;
            state->remaining = packed >> 8;
            if (!state->remaining || index >= sizeof ln_color_disk_palette / sizeof ln_color_disk_palette[0])
                return false;
            state->color = ln_color_disk_palette[index];
            if (!symbol_visible && ln_color_disk_symbol_tint(state->color))
                state->color = 0xffdfu; /* Disk Mode paper, RGB565. */
        }
        uint32_t take = count < state->remaining ? count : state->remaining;
        for (uint32_t i = 0; i < take; ++i) pixels[i] = state->color;
        pixels += take;
        count -= take;
        state->remaining -= take;
    }
    return true;
}

static bool ln_color_disk_finished(const struct ln_color_disk_decoder *state)
{
    return state && !state->remaining &&
        state->run == sizeof ln_color_disk_runs / sizeof ln_color_disk_runs[0];
}

#endif
