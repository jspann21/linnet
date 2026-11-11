#ifndef LINNET_UI_H
#define LINNET_UI_H
#include <stdint.h>
#include <stdbool.h>
#include "linnet_target.h"
#define LINNET_UI_MAX_SCENES 64u
#define LINNET_UI_ACTION_SYNC 0xfffffffeu
#define LINNET_UI_ACTION_SHUFFLE 0xfffffffdu
#define LINNET_UI_ACTION_REPEAT 0xfffffffcu
#define LINNET_UI_ACTION_SHUFFLE_ALL 0xfffffffbu
#define LINNET_UI_ACTION_MENUS 0xfffffffau
enum ln_ui_kind { LN_UI_SCREEN=1,LN_UI_VIEW,LN_UI_TEXT,LN_UI_ICON,LN_UI_IMAGE,
    LN_UI_VIRTUAL_LIST,LN_UI_MENU,LN_UI_GRID,LN_UI_SCROLLBAR,LN_UI_PROGRESS_BAR,
    LN_UI_STATUS_BAR,LN_UI_DIALOG,LN_UI_TOAST,LN_UI_ALBUM_ART,LN_UI_PHOTO_SURFACE,
    LN_UI_VIDEO_SURFACE,LN_UI_CANVAS_SURFACE };
/* Semantic color references remain native constants; literal RGB565 is unchanged. */
#define LN_UI_COLOR_TOKEN 0x10000u
#define LN_UI_THEME_MONOCHROME 1u
#define LN_UI_THEME_APPLE_PHOTO 2u
#define LN_UI_THEME_ALBUM_ART 4u
#define LN_UI_THEME_APPLE_CLASSIC 8u
#define LN_UI_THEME_APPLE_FONT 16u
#define LN_UI_APPLE_FONT_BYTES 8570u
#define LN_UI_CANVAS_OPAQUE 64u
/* Host-baked list geometry. Low six flags retain their existing meanings.
 * Unmarked, reserved-bit, or incomplete encodings use runtime geometry. */
#define LN_UI_GEOMETRY 0x80000000u
#define LN_UI_GEOMETRY_RESERVED 0x70000000u
#define LN_UI_GEOMETRY_ROW_SHIFT 6u
#define LN_UI_GEOMETRY_COLUMNS_SHIFT 13u
#define LN_UI_GEOMETRY_ROWS_SHIFT 16u
#define LN_UI_GEOMETRY_WIDTH_SHIFT 20u
enum ln_ui_color_role {LN_UI_FOREGROUND,LN_UI_BACKGROUND,LN_UI_ACCENT,LN_UI_MUTED,
    LN_UI_PANEL,LN_UI_SELECTION_FG,LN_UI_SELECTION_BG,LN_UI_STATUS_FG,LN_UI_STATUS_BG,
    LN_UI_BORDER,LN_UI_ARTWORK_BG,LN_UI_ARTWORK_FG,LN_UI_BATTERY,LN_UI_COLOR_COUNT};
enum ln_ui_layout {LN_UI_LAYOUT_DEFAULT,LN_UI_LAYOUT_IPOD_PHOTO,LN_UI_LAYOUT_GRAPHITE,
    LN_UI_LAYOUT_AURORA,LN_UI_LAYOUT_SUNSET,LN_UI_LAYOUT_BLUEPRINT,LN_UI_LAYOUT_MINIMAL,LN_UI_LAYOUT_NATIVE,
    LN_UI_LAYOUT_IPOD_CLASSIC,LN_UI_LAYOUT_CLASSIC_SPLIT};
enum ln_ui_menu_group { LN_MENU_HOME, LN_MENU_MUSIC, LN_MENU_SETTINGS, LN_MENU_EXTRAS, LN_MENU_GROUPS };
uint32_t ln_ui_menu_count(uint32_t group);
uint32_t ln_ui_menu_item(uint32_t group,uint32_t row);
uint32_t ln_ui_menu_revision(void);
bool ln_ui_menu_valid(const uint8_t *data,uint32_t length);
bool ln_ui_menu_load(const uint8_t *data,uint32_t length);
void ln_ui_menu_encode(uint8_t data[80]);
uint32_t ln_ui_menu_limit(uint32_t group);
const char *ln_ui_menu_label(uint32_t group,uint32_t item);
bool ln_ui_menu_available(uint32_t group,uint32_t item);
bool ln_ui_menu_required_item(uint32_t group,uint32_t item);
bool ln_ui_menu_toggle(uint32_t group,uint32_t item);
bool ln_ui_menu_move(uint32_t group,uint32_t item,int32_t direction);
bool ln_games_tick(uint32_t now);
uint32_t ln_games_deadline(uint32_t now);
uint32_t ln_games_hud_revision(void);
bool ln_games_playing(void);
void ln_games_audio_step(void);
void ln_games_audio_stop(void);
void ln_games_music_toggle(void);
void ln_games_step(uint32_t now,bool visible,bool hold,bool may_write);
void ln_games_stop(void);
bool ln_games_quiesce(bool publish);
bool ln_games_pending(void);
bool ln_games_take_exit(void);
struct ln_ui_scene;
struct ln_ui_font;
struct ln_ui_theme {
    const char *id,*name;uint16_t colors[LN_UI_COLOR_COUNT];uint32_t flags;
    const struct ln_ui_scene *scenes;const struct ln_ui_font *font;
    uint32_t row_height,padding,layout;
};
extern const struct ln_ui_theme ln_ui_themes[];
extern const uint32_t ln_ui_theme_count,ln_ui_default_theme;
uint16_t ln_ui_color(uint32_t value);
uint32_t ln_ui_theme_index(void);
uint32_t ln_ui_battery_status(void); /* percent0..100,bit8 charging,bit9 external; UINT32_MAX unknown */
uint32_t ln_ui_clock_seconds(void); /* local RTC seconds since midnight, UINT32_MAX unknown */
bool ln_ui_clock_12(void);
void ln_ui_clock_toggle(void);
uint32_t ln_ui_playback_modes(void); /* bit0 shuffle; bits1..2 repeat: off,all,one */
uint32_t ln_ui_playback_status(void); /*0 stopped,1 playing,2 paused*/
/* Memory-only preference hooks; persistence belongs to display settings. */
uint32_t ln_ui_theme_preference(void);
void ln_ui_theme_changed(uint32_t index);
const char *ln_ui_theme_status(void);
#include "sdk/include/linnet/graphics.h"
typedef void (*ln_ui_canvas_fn)(const struct ln_ui_canvas_context *context);
struct ln_ui_node {
    uint32_t kind;int32_t x,y,w,h;uint32_t fg,bg,flags,count,resource,index;
    const char *text;const char *const *items;const uint32_t *targets;
    uint32_t item_count,deps;
    ln_ui_canvas_fn canvas;
};
struct ln_ui_scene {const char *id,*title;const struct ln_ui_node *nodes;uint32_t count,focus_node;};
struct ln_ui_asset {uint32_t width,height;const uint16_t *pixels;};
/* Memory-only per-row assets for virtualized image grids. */
const struct ln_ui_asset *ln_ui_dynamic_item_asset(const struct ln_ui_node *,uint32_t,const struct ln_ui_asset *);

extern const struct ln_ui_scene *ln_ui_scenes;
extern const uint32_t ln_ui_scene_count;
extern const uint64_t ln_ui_enabled_scenes;
extern const struct ln_ui_asset ln_ui_assets[];
extern const uint32_t ln_ui_asset_count;
extern struct ln_ui_font ln_ui_font;
extern const struct ln_ui_font ln_ui_hint_font;
extern const struct ln_ui_font ln_ui_clock_font;
/* Caller retains the validated, aligned optional font buffer for the session. */
bool ln_ui_apple_font_load(const uint8_t *data,uint32_t length);
/* Text-only flag; list nodes use these bits for baked geometry. */
#define LN_UI_FOOTER 64u
struct ln_ui_state {
    uint32_t scene,selected,buttons,animation;
    uint32_t selections[LINNET_UI_MAX_SCENES],first_visible[LINNET_UI_MAX_SCENES];
    uint32_t drawn_first;
    uint32_t drawn_scene,drawn_selected,drawn_flags,dirty,action,transitions;
    uint32_t paint_count,painted_pixels,visible_items,scene_epoch,drawn_theme,drawn_status;
    uint8_t history[16],history_count;
    bool hold,headphones,rendered;
};
typedef void (*ln_ui_damage_fn)(int32_t x,int32_t y,int32_t w,int32_t h);
void ln_ui_init(struct ln_ui_state *state);
/* Clock editor bridge: labels/input are memory-only; begin is explicit foreground input. */
bool ln_ui_clock_begin(void);
/* 0 unhandled, 1 changed, 2 consumed unchanged. */
int ln_ui_clock_input(struct ln_ui_state *,uint32_t kind,int32_t value);
void ln_ui_clock_cancel(void);
const char *ln_ui_clock_text(const struct ln_ui_node *,uint32_t item,const char *fallback);

/* Providers must be bounded memory-only reads: no storage, allocation or I/O.
 * item UINT32_MAX requests node text; other values request a list/grid row.
 * Default weak implementations return fallback unchanged. */
bool ln_ui_dynamic_navigation(const struct ln_ui_node *node,uint32_t item);
uint32_t ln_ui_dynamic_count(const struct ln_ui_node *node,uint32_t fallback);
uint32_t ln_ui_dynamic_value(const struct ln_ui_node *node,uint32_t fallback);
const struct ln_ui_asset *ln_ui_dynamic_asset(const struct ln_ui_node *node,const struct ln_ui_asset *fallback);
const char *ln_ui_dynamic_text(const struct ln_ui_node *node,uint32_t item,const char *fallback);
void ln_ui_refresh(struct ln_ui_state *state);
/* Returns the next text-scroll deadline, or UINT32_MAX when idle. */
uint32_t ln_ui_scroll_tick(struct ln_ui_state *state,uint32_t now_us);
/* Retained artwork only; revision changes when published pixels or availability change. */
const struct ln_ui_asset *ln_ui_now_playing_art(uint32_t *revision);
const struct ln_ui_asset *ln_ui_now_playing_art_sized(uint32_t *revision,uint32_t side);
bool ln_ui_now_playing_art_loading(void);
uint32_t ln_ui_palette_tick(struct ln_ui_state *state,uint32_t now_us);
bool ln_ui_status_refresh(struct ln_ui_state *state);
uint32_t ln_ui_visible_capacity(const struct ln_ui_state *state);
uint32_t ln_ui_first_visible(struct ln_ui_state *state);
uint32_t ln_ui_focused_count(const struct ln_ui_state *state);
/* Compare bounded visible list/text content with the last painted frame.
 * Scene changes and unsupported layouts retain full refresh semantics. */
void ln_ui_refresh_content(struct ln_ui_state *state);
bool ln_ui_back(struct ln_ui_state *state);
bool ln_ui_show(struct ln_ui_state *state,const char *scene_id);
/* Input kinds match the native input interface: wheel1,buttons2,hold3,headphones4. */
bool ln_ui_input(struct ln_ui_state *state,uint32_t kind,int32_t value);
void ln_ui_render(struct ln_ui_state *state,uint16_t *pixels,ln_ui_damage_fn damage);
void ln_ui_animate(struct ln_ui_state *state,uint16_t *pixels,uint32_t frame,ln_ui_damage_fn damage);
uint32_t ln_ui_take_action(struct ln_ui_state *state);
#endif
