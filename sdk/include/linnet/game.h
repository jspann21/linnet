#ifndef LINNET_GAME_API_H
#define LINNET_GAME_API_H
#include <stdbool.h>
#include <stdint.h>
#include "graphics.h"
#define LN_GAME_ABI 1u
#define LN_GAME_LAYOUT 1u
#define LN_GAME_SLOT_OFFSET 0x7c0000u
#define LN_GAME_SLOT_BYTES 0x40000u
#define LN_GAME_HEADER_BYTES 256u
#define LN_GAME_ID_BYTES 64u
#define LN_GAME_TITLE_BYTES 40u
#define LN_GAME_DESCRIPTION_BYTES 80u
#define LN_GAME_CATALOGUE_MAX 128u
#define LN_GAME_SAVE_BYTES 256u
#define LN_GAME_CAP_MUSIC 1u
#define LN_GAME_CAP_ALL LN_GAME_CAP_MUSIC
#define LN_GAME_BUTTON_MENU 1u
#define LN_GAME_BUTTON_PLAY 2u
#define LN_GAME_BUTTON_NEXT 4u
#define LN_GAME_BUTTON_PREVIOUS 8u
#define LN_GAME_BUTTON_SELECT 16u
#define LN_GAME_INPUT_WHEEL 1u
#define LN_GAME_INPUT_BUTTONS 2u
#define LN_GAME_INPUT_HOLD 3u
#define LN_GAME_UNHANDLED 0
#define LN_GAME_CHANGED 1
#define LN_GAME_EXIT 2
#define LN_GAME_CONSUMED 3
#define LN_GAME_SAVE_UNAVAILABLE (-1)
#define LN_GAME_SAVE_EMPTY 0
#define LN_GAME_NO_DEADLINE UINT32_MAX
/* All pointers borrowed from the host remain host-owned. Title access copies. */
struct ln_game_host {
    uint32_t abi,bytes,capabilities;
    const struct ln_ui_font *font,*hint_font,*clock_font;
    uint32_t (*time_us)(void);
    uint32_t (*best)(void);
    bool (*record_best)(uint32_t score);
    /* read:bytes copied,0 empty,-1 unavailable/schema mismatch. No I/O here.
     * write:true means copied/queued, not durable. Host serializes publication;
     * caller retains no buffer. Exact schema and <=256 bytes are required. */
    int32_t (*save_read)(uint32_t schema,void *data,uint32_t capacity);
    bool (*save_write)(uint32_t schema,const void *data,uint32_t bytes);
    uint32_t (*music_count)(void);
    bool (*music_title)(uint32_t index,char *out,uint32_t capacity);
    uint32_t (*music_current)(void);
    bool (*music_loading)(void);
    bool (*quiz_begin)(uint32_t index);
    bool (*quiz_select)(uint32_t index);
    void (*quiz_end)(void);
    bool (*music_pause)(bool paused);
    void (*music_toggle)(void);
    bool (*music_playing)(void);
};
/* kind0 none,1 tap,2 success,3 error,4 end,5..8 pitches,9 four-note sequence.
 * Returned by copy before unload; no queued audio keeps module pointers. */
struct ln_game_cue {uint32_t kind;uint8_t notes[4];};
struct ln_game_module {
    uint32_t abi,bytes;
    /* Every callback is mandatory, ARM-only, and borrowed until stop completes.
     * Input kind2 supplies the entire button mask (including release0).
     * kind3 reports hold0/1; host suspends wheel/buttons/tick while held.
     * input returns LN_GAME_{UNHANDLED,CHANGED,EXIT,CONSUMED}.
     * Deadlines are wrapping uint32 microseconds, <2^31 ahead; UINT32_MAX
     * means no work. Host schedules deadline even when playing()==false.
     * tick:true means redraw. draw receives a clipped canvas borrowed for
     * this call; it must not keep its pixels pointer. playing controls the
     * backlight hint only, never ownership or loading progress. No I/O waits. */
    bool (*start)(void);
    int32_t (*input)(uint32_t kind,int32_t value);
    bool (*tick)(uint32_t now);
    uint32_t (*deadline)(uint32_t now);
    void (*draw)(const struct ln_ui_canvas_context *context);
    bool (*playing)(void);
    uint32_t (*revision)(void);
    bool (*take_cue)(struct ln_game_cue *out);
    void (*stop)(void);
};
typedef const struct ln_game_module *(*ln_game_entry)(const struct ln_game_host *);
/* The package entry is linked at a fixed, versioned target slot by the SDK. */
#if defined(_WIN32)
#define LN_GAME_EXPORT __declspec(dllexport)
#else
#define LN_GAME_EXPORT
#endif
LN_GAME_EXPORT const struct ln_game_module *ln_game_entry_point(const struct ln_game_host *host);
#endif
