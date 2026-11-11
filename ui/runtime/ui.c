#include "services/display/preferences.h"
#include "ui/runtime/ui.h"
#include "linnet_menu_definition.h"
#include "linnet_scene_ids.h"
#define UI_X(value) ((int32_t)((value)*LINNET_UI_WIDTH/220u))
#define UI_Y(value) ((int32_t)((value)*LINNET_UI_HEIGHT/176u))
static uint32_t session_theme=UINT32_MAX,active_theme;
static bool theme_ready;
static struct ln_ui_font apple_font;
static bool apple_font_ready;
bool ln_ui_apple_font_load(const uint8_t *data,uint32_t length){
    const uint32_t alpha_at=888u,alpha_bytes=7678u;
    if(!data){apple_font_ready=false;theme_ready=false;return true;}
    if(length!=LN_UI_APPLE_FONT_BYTES||((uintptr_t)data&3u))return false;
    static const uint8_t magic[8]={'L','N','F','O','N','T','1',0};
    for(uint32_t i=0;i<8u;++i)if(data[i]!=magic[i])return false;
    const uint32_t *header=(const uint32_t *)(const void *)(data+8u);
    if(header[0]!=15u||header[1]!=17u||header[2]!=14u||header[3]!=32u||header[4]!=95u||header[5]!=alpha_bytes||data[127])return false;
    uint32_t crc=~0u;
    for(uint32_t i=0;i<length-4u;++i){crc^=data[i];for(uint32_t bit=0;bit<8u;++bit)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    uint32_t saved=(uint32_t)data[length-4u]|((uint32_t)data[length-3u]<<8)|((uint32_t)data[length-2u]<<16)|((uint32_t)data[length-1u]<<24);
    if(~crc!=saved)return false;
    const struct ln_ui_glyph *glyphs=(const struct ln_ui_glyph *)(const void *)(data+128u);
    for(uint32_t i=0;i<95u;++i){const struct ln_ui_glyph *g=glyphs+i;
        if(!data[32u+i]||data[32u+i]>14u||(uint32_t)g->x+g->w>15u||(uint32_t)g->y+g->h>17u||g->offset>alpha_bytes||(uint32_t)g->w*g->h>alpha_bytes-g->offset)return false;
    }
    apple_font=(struct ln_ui_font){data+alpha_at,15u,17u,14u,32u,95u,data+32u,glyphs};
    apple_font_ready=true;theme_ready=false;return true;
}
static bool session_clock_12;
__attribute__((weak)) bool ln_ui_clock_12(void){return session_clock_12;}
__attribute__((weak)) void ln_ui_clock_toggle(void){session_clock_12=!session_clock_12;}
__attribute__((weak)) uint32_t ln_ui_battery_status(void){return UINT32_MAX;}
__attribute__((weak)) uint32_t ln_ui_clock_seconds(void){return UINT32_MAX;}
__attribute__((weak)) uint32_t ln_ui_playback_status(void){return 0;}
__attribute__((weak)) uint32_t ln_ui_playback_modes(void){return 0;}
__attribute__((weak)) bool ln_ui_clock_begin(void){return false;}
__attribute__((weak)) int ln_ui_clock_input(struct ln_ui_state*s,uint32_t kind,int32_t value){(void)s;(void)kind;(void)value;return 0;}
__attribute__((weak)) void ln_ui_clock_cancel(void){}
__attribute__((weak)) const char *ln_ui_clock_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){(void)n;(void)item;return fallback;}
__attribute__((weak)) bool ln_ui_dynamic_navigation(const struct ln_ui_node*n,uint32_t item){(void)n;(void)item;return false;}
__attribute__((weak)) uint32_t ln_games_count(void){return 0;}
__attribute__((weak)) const char *ln_games_label(uint32_t row){(void)row;return 0;}
__attribute__((weak)) const char *ln_games_description(uint32_t row){(void)row;return "";}
__attribute__((weak)) void ln_games_start(uint32_t row){(void)row;}
__attribute__((weak)) int ln_games_input(uint32_t kind,int32_t value){(void)kind;(void)value;return 0;}
__attribute__((weak)) bool ln_games_tick(uint32_t now){(void)now;return false;}
__attribute__((weak)) uint32_t ln_games_deadline(uint32_t now){(void)now;return UINT32_MAX;}
__attribute__((weak)) uint32_t ln_games_hud_revision(void){return 0;}
__attribute__((weak)) const char *ln_games_text(const char *key){return key;}
__attribute__((weak)) void ln_games_configure(uint32_t mask){(void)mask;}
__attribute__((weak)) void ln_games_best_set(uint32_t index,uint16_t value){(void)index;(void)value;}
__attribute__((weak)) bool ln_games_playing(void){return false;}
__attribute__((weak)) void ln_games_audio_step(void){}
__attribute__((weak)) void ln_games_audio_stop(void){}
__attribute__((weak)) void ln_games_music_toggle(void){}
__attribute__((weak)) void ln_games_step(uint32_t now,bool visible,bool hold,bool may_write){(void)now;(void)visible;(void)hold;(void)may_write;}
__attribute__((weak)) void ln_games_stop(void){}
__attribute__((weak)) bool ln_games_quiesce(bool publish){(void)publish;return true;}
__attribute__((weak)) bool ln_games_pending(void){return false;}
__attribute__((weak)) bool ln_games_take_exit(void){return false;}
static void theme_sync(void);
__attribute__((weak)) uint32_t ln_ui_theme_preference(void){return session_theme;}
__attribute__((weak)) void ln_ui_theme_changed(uint32_t index){session_theme=index;}
__attribute__((weak)) const char *ln_ui_theme_status(void){return "Applied";}
uint32_t ln_ui_theme_index(void){uint32_t index=ln_ui_theme_preference();return index<ln_ui_theme_count?index:ln_ui_default_theme;}
static uint16_t album_colors[LN_UI_COLOR_COUNT];
static bool album_colors_valid;
__attribute__((weak)) const struct ln_ui_asset *ln_ui_now_playing_art(uint32_t *revision){*revision=0;return 0;}
__attribute__((weak)) const struct ln_ui_asset *ln_ui_now_playing_art_sized(uint32_t *revision,uint32_t side){(void)side;return ln_ui_now_playing_art(revision);}
__attribute__((weak)) bool ln_ui_now_playing_art_loading(void){return false;}
uint16_t ln_ui_color(uint32_t value){return value>=LN_UI_COLOR_TOKEN&&value<LN_UI_COLOR_TOKEN+LN_UI_COLOR_COUNT?((ln_ui_themes[active_theme].flags&LN_UI_THEME_ALBUM_ART)&&album_colors_valid?album_colors[value-LN_UI_COLOR_TOKEN]:ln_ui_themes[active_theme].colors[value-LN_UI_COLOR_TOKEN]):(uint16_t)value;}
static void theme_sync(void){
    uint32_t next=ln_ui_theme_index();if(theme_ready&&next==active_theme)return;
    active_theme=next;ln_ui_scenes=ln_ui_themes[active_theme].scenes;
    ln_ui_font=apple_font_ready&&(ln_ui_themes[active_theme].flags&LN_UI_THEME_APPLE_FONT)?apple_font:*ln_ui_themes[active_theme].font;theme_ready=true;
}
static uint32_t layout(void){return ln_ui_themes[active_theme].layout;}
static bool classic_layout(void){return layout()==LN_UI_LAYOUT_IPOD_CLASSIC||layout()==LN_UI_LAYOUT_CLASSIC_SPLIT;}
static bool photo_content_layout(void){return layout()==LN_UI_LAYOUT_IPOD_PHOTO||(layout()==LN_UI_LAYOUT_CLASSIC_SPLIT&&LINNET_UI_WIDTH==220u);}
static uint16_t color(uint32_t role){return ln_ui_color(LN_UI_COLOR_TOKEN+role);}
static bool same(const char*a,const char*b){if(!a||!b)return false;while(*a&&*a==*b){++a;++b;}return *a==*b;}
/* Stable menu IDs are independent of theme labels and disabled features. */
static uint8_t menu_order[LN_MENU_GROUPS][16]=LN_MENU_DEFAULT_ORDER;
static uint8_t menu_counts[LN_MENU_GROUPS]=LN_MENU_DEFAULT_COUNTS;
static uint32_t menu_revision;
/* Reuse the resolved rows while drawing and navigating the same menu. */
static struct {
    const struct ln_ui_node *node;
    const struct ln_ui_scene *scenes;
    uint32_t rows[sizeof menu_order[0]+3u],count,group;
    bool games;
} menu_cache;
uint32_t ln_ui_menu_count(uint32_t group){return group<LN_MENU_GROUPS?menu_counts[group]:0;}
uint32_t ln_ui_menu_item(uint32_t group,uint32_t row){return group<LN_MENU_GROUPS&&row<menu_counts[group]?menu_order[group][row]:UINT32_MAX;}
uint32_t ln_ui_menu_revision(void){return menu_revision;}
bool ln_ui_menu_valid(const uint8_t *data,uint32_t length){
    if(!data)return false;
    if(length!=80||data[0]!='M'||data[1]!='E'||data[2]!='N'||data[3]!='U'||data[4]!=3||data[5]||data[6]||data[7])return false;
    uint32_t crc=~0u;for(uint32_t i=0;i<76;++i){crc^=data[i];for(uint32_t bit=0;bit<8;++bit)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    uint32_t saved=(uint32_t)data[76]|((uint32_t)data[77]<<8)|((uint32_t)data[78]<<16)|((uint32_t)data[79]<<24);
    if(~crc!=saved)return false;
    for(uint32_t g=0;g<LN_MENU_GROUPS;++g){const uint8_t *row=data+8+g*17;uint32_t seen=0;
        if(!row[0]||row[0]>ln_menu_limits[g])return false;
        for(uint32_t i=0;i<row[0];++i){uint32_t id=row[i+1];if(id>=ln_menu_limits[g]||(seen&(1u<<id)))return false;seen|=1u<<id;}
        if((seen&ln_menu_required[g])!=ln_menu_required[g])return false;
    }
    return true;
}
bool ln_ui_menu_load(const uint8_t *data,uint32_t length){
    if(!ln_ui_menu_valid(data,length))return false;
    menu_cache.node=0;++menu_revision;
    for(uint32_t g=0;g<LN_MENU_GROUPS;++g){menu_counts[g]=data[8+g*17];for(uint32_t i=0;i<menu_counts[g];++i)menu_order[g][i]=data[9+g*17+i];}
    return true;
}
uint32_t ln_ui_menu_limit(uint32_t group){return group<LN_MENU_GROUPS?ln_menu_limits[group]:0;}
const char *ln_ui_menu_label(uint32_t group,uint32_t item){
    if(group==LN_MENU_HOME&&item==7u&&ln_games_count())return "Games";
    return group<LN_MENU_GROUPS&&item<ln_menu_limits[group]?ln_menu_labels[group][item]:"";
}
bool ln_ui_menu_required_item(uint32_t group,uint32_t item){return group<LN_MENU_GROUPS&&item<ln_menu_limits[group]&&(ln_menu_required[group]&(1u<<item))!=0;}
bool ln_ui_menu_available(uint32_t group,uint32_t item){
    if(group>=LN_MENU_GROUPS||item>=ln_menu_limits[group])return false;
    if(!(ln_menu_device_editable[group]&(1u<<item)))return false;
    uint32_t scene=ln_menu_scene_ids[group][item];
    return scene==64u||(scene<ln_ui_scene_count&&(ln_ui_enabled_scenes&(UINT64_C(1)<<scene))!=0);
}
void ln_ui_menu_encode(uint8_t data[80]){
    for(uint32_t i=0;i<80u;++i)data[i]=0;
    data[0]='M';data[1]='E';data[2]='N';data[3]='U';data[4]=3;
    for(uint32_t g=0;g<LN_MENU_GROUPS;++g){data[8+g*17]=menu_counts[g];for(uint32_t i=0;i<menu_counts[g];++i)data[9+g*17+i]=menu_order[g][i];}
    uint32_t crc=~0u;for(uint32_t i=0;i<76u;++i){crc^=data[i];for(uint32_t b=0;b<8u;++b)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}crc=~crc;
    for(uint32_t i=0;i<4u;++i)data[76+i]=(uint8_t)(crc>>(8u*i));
}
bool ln_ui_menu_toggle(uint32_t group,uint32_t item){
    if(!ln_ui_menu_available(group,item)||ln_ui_menu_required_item(group,item))return false;
    uint32_t n=menu_counts[group],at=0;while(at<n&&menu_order[group][at]!=item)++at;
    if(at<n){uint32_t available=0;for(uint32_t i=0;i<n;++i)if(ln_ui_menu_available(group,menu_order[group][i]))++available;
        if(n==1u||(available<=1u&&ln_ui_menu_available(group,item)))return false;
        for(uint32_t i=at+1u;i<n;++i)menu_order[group][i-1u]=menu_order[group][i];menu_counts[group]=(uint8_t)(n-1u);}
    else {if(!ln_ui_menu_available(group,item)||n>=16u)return false;menu_order[group][n]=(uint8_t)item;menu_counts[group]=(uint8_t)(n+1u);}
    menu_cache.node=0;++menu_revision;return true;
}
bool ln_ui_menu_move(uint32_t group,uint32_t item,int32_t direction){
    if(!ln_ui_menu_available(group,item)||!direction)return false;uint32_t n=menu_counts[group],at=0;
    while(at<n&&menu_order[group][at]!=item)++at;if(at==n||(direction<0&&!at)||(direction>0&&at+1u==n))return false;
    uint32_t to=at;do{if((direction<0&&!to)||(direction>0&&to+1u==n))return false;to=direction<0?to-1u:to+1u;}while(!ln_ui_menu_available(group,menu_order[group][to]));
    uint8_t other=menu_order[group][to];menu_order[group][to]=menu_order[group][at];menu_order[group][at]=other;menu_cache.node=0;++menu_revision;return true;
}
__attribute__((weak)) bool ln_preferences_input(struct ln_ui_state*s,uint32_t kind,int32_t value){(void)s;(void)kind;(void)value;return false;}
__attribute__((weak)) void ln_preferences_open(struct ln_ui_state*s){(void)s;}
__attribute__((weak)) uint32_t ln_preferences_count(const struct ln_ui_node*n,uint32_t fallback){(void)n;return fallback;}
__attribute__((weak)) const char *ln_preferences_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){(void)n;(void)item;return fallback;}
static const char *const menu_scenes[]={"linnet.system.home","linnet.system.music","linnet.system.settings","linnet.system.extras"};
static uint32_t menu_group(const struct ln_ui_node *n){
    if(!n||n->kind!=LN_UI_MENU||!n->targets)return UINT32_MAX;
    for(uint32_t scene=0;scene<ln_ui_scene_count;++scene){const struct ln_ui_scene *s=&ln_ui_scenes[scene];
        for(uint32_t i=0;i<s->count;++i)if(n==&s->nodes[i]){for(uint32_t g=0;g<LN_MENU_GROUPS;++g)if(same(s->id,menu_scenes[g]))return g;return UINT32_MAX;}}
    return UINT32_MAX;
}
static uint32_t menu_lookup(const struct ln_ui_node *n,uint32_t row,uint32_t *count){
    if(!n||n->kind!=LN_UI_MENU||!n->targets)return row;
    bool games=ln_games_count()!=0;
    if(menu_cache.node!=n||menu_cache.scenes!=ln_ui_scenes||menu_cache.games!=games){
        uint32_t group=menu_group(n),found=0;
        if(group!=UINT32_MAX){
            /* Two fixed playback rows plus at most sixteen saved choices. */
            if(group==LN_MENU_SETTINGS){
                for(uint32_t action=LINNET_UI_ACTION_SHUFFLE;action>=LINNET_UI_ACTION_REPEAT;--action)
                    for(uint32_t j=0;j<n->item_count;++j)if(n->targets[j]==action){menu_cache.rows[found++]=j;break;}
            }
            for(uint32_t j=0;group==LN_MENU_SETTINGS&&j<n->item_count;++j)if(n->targets[j]==LINNET_UI_ACTION_MENUS){menu_cache.rows[found++]=j;break;}
            for(uint32_t i=0;i<menu_counts[group];++i){const char *id=ln_menu_targets[group][menu_order[group][i]];
                if(group==LN_MENU_HOME&&menu_order[group][i]==7u){if(!games)continue;id="linnet.system.games";}
                if(group==LN_MENU_EXTRAS&&same(id,"linnet.system.games")&&!games)continue;
                for(uint32_t j=0;j<n->item_count;++j){uint32_t target=n->targets[j];const char *actual=target==LINNET_UI_ACTION_SYNC?"@sync":target==LINNET_UI_ACTION_SHUFFLE_ALL?"@shuffle-all":target<ln_ui_scene_count?ln_ui_scenes[target].id:"";
                    if(same(id,actual)){menu_cache.rows[found++]=j;break;}}}
        }
        menu_cache.node=n;menu_cache.scenes=ln_ui_scenes;menu_cache.games=games;
        menu_cache.group=group;menu_cache.count=found;
    }
    if(menu_cache.group==UINT32_MAX)return row;
    if(count)*count=menu_cache.count;
    return row<menu_cache.count?menu_cache.rows[row]:UINT32_MAX;
}
static uint32_t menu_source(const struct ln_ui_node *n,uint32_t row){return menu_lookup(n,row,0);}
static uint32_t menu_total(const struct ln_ui_node *n,uint32_t fallback){
    /* Count matching entries once instead of rebuilding each preceding row. */
    uint32_t count=fallback;(void)menu_lookup(n,UINT32_MAX,&count);return count;
}
static bool theme_choices(const struct ln_ui_node*n){return n&&same(n->text,"Theme choices");}
static bool apple_style(void){return (ln_ui_themes[active_theme].flags&(LN_UI_THEME_MONOCHROME|LN_UI_THEME_APPLE_PHOTO|LN_UI_THEME_APPLE_CLASSIC))!=0;}
__attribute__((weak)) const char *ln_display_settings_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){(void)n;(void)item;return fallback;}
__attribute__((weak)) uint32_t ln_ui_dynamic_count(const struct ln_ui_node*n,uint32_t fallback){(void)n;return fallback;}
__attribute__((weak)) uint32_t ln_ui_dynamic_value(const struct ln_ui_node*n,uint32_t fallback){(void)n;return fallback;}
__attribute__((weak)) const struct ln_ui_asset *ln_ui_dynamic_asset(const struct ln_ui_node*n,const struct ln_ui_asset*fallback){(void)n;return fallback;}
__attribute__((weak)) const struct ln_ui_asset *ln_ui_dynamic_item_asset(const struct ln_ui_node*n,uint32_t item,const struct ln_ui_asset*fallback){(void)n;(void)item;return fallback;}
__attribute__((weak)) const char *ln_ui_dynamic_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){(void)n;(void)item;return fallback;}
struct rect {int32_t x,y,w,h;};
struct painter {uint16_t *pixels;struct rect clip;};
static bool image_independent(const struct painter*,const struct ln_ui_node*,const struct ln_ui_asset*);
static int32_t min(int32_t a,int32_t b){return a<b?a:b;}
static int32_t max(int32_t a,int32_t b){return a>b?a:b;}
static struct rect intersect(struct rect a,struct rect b){int32_t x=max(a.x,b.x),y=max(a.y,b.y),r=min(a.x+a.w,b.x+b.w),d=min(a.y+a.h,b.y+b.h);return(struct rect){x,y,max(0,r-x),max(0,d-y)};}
static struct rect bounds(const struct ln_ui_node*n){return(struct rect){n->x,n->y,n->w,n->h};}
static const struct rect screen={0,0,(int32_t)LINNET_UI_WIDTH,(int32_t)LINNET_UI_HEIGHT};
/* Bounded shift/subtract quotient; ARM7 has no hardware divide. Small layout
 * operands avoid walking all 32 bits, with no compiler division runtime. */
static uint32_t divide(uint32_t numerator,uint32_t denominator){
    if(!denominator||numerator<denominator)return 0;if(denominator==1u)return numerator;
    uint32_t quotient=0,bit=1u;
    while(denominator<=(numerator>>1)){denominator<<=1;bit<<=1;}
    do{if(numerator>=denominator){numerator-=denominator;quotient|=bit;}denominator>>=1;bit>>=1;}while(bit);
    return quotient;
}
/* Every mix caller supplies alpha 0..255. RGB565's largest rounded channel
 * numerator is 63*255+127=16192; this shift/add quotient is exact on that range. */
static uint32_t blend_div255(uint32_t value){return (value+1u+(value>>8))>>8;}
static uint16_t mix(uint16_t a,uint16_t b,uint32_t alpha){
    uint32_t ar=((a&0xf800u)<<5)|(a&31u),br=((b&0xf800u)<<5)|(b&31u);
    uint32_t rb=br*255u+(ar-br)*alpha+0x007f007fu;
    rb+=0x00010001u+((rb>>8)&0x00ff00ffu);
    uint32_t ag=(a>>5)&63u,bg=(b>>5)&63u;
    uint32_t g=blend_div255(bg*255u+(ag-bg)*alpha+127u);
    return(uint16_t)(((rb>>13)&0xf800u)|(g<<5)|((rb>>8)&31u));
}
/* Text keeps its ink color for the whole run. Inline the exact blend so each
 * partial-alpha pixel reuses those channels without a generic mix call. */
static inline __attribute__((always_inline)) uint16_t text_mix(uint32_t ar,uint32_t ag,uint16_t b,uint32_t alpha){
    uint32_t br=((b&0xf800u)<<5)|(b&31u);
    uint32_t rb=br*255u+(ar-br)*alpha+0x007f007fu;
    rb+=0x00010001u+((rb>>8)&0x00ff00ffu);
    uint32_t bg=(b>>5)&63u;
    uint32_t g=blend_div255(bg*255u+(ag-bg)*alpha+127u);
    return(uint16_t)(((rb>>13)&0xf800u)|(g<<5)|((rb>>8)&31u));
}
/* Linnet album palette: bounded histogram, artwork-revision cache and
 * 250 ms transitions. No storage access, allocations or floating point
 * on the rendering path. */
static uint16_t album_from[LN_UI_COLOR_COUNT],album_target[LN_UI_COLOR_COUNT];
static uint32_t album_revision,album_theme=UINT32_MAX,album_started;
static bool album_fading,album_waiting;
static uint32_t album_missing_at;
/* Copyright (C) 2026 jspann21 and Linnet contributors. GPL-3.0-or-later.
 * Palette calculations use 8-bit channel expansion, integer rounding and
 * bounded bucket traversal; RGB565 packing happens after channel averaging. */
struct album_rgb {uint32_t r,g,b;};
static struct album_rgb album_unpack(uint16_t c){
    return (struct album_rgb){((c>>8)&248u)|(c>>13),((c>>3)&252u)|((c>>9)&3u),((c<<3)&248u)|((c>>2)&7u)};
}
static uint16_t album_pack(struct album_rgb c){return (uint16_t)(((c.r>>3)<<11)|((c.g>>2)<<5)|(c.b>>3));}
static uint32_t album_luma(struct album_rgb c){return (c.r*77u+c.g*150u+c.b*29u)>>8;}
/* Video fallbacks and controls keep the theme hue without a bright canvas. */
static uint16_t video_background(uint16_t value){
    uint32_t luma=album_luma(album_unpack(value));
    return luma>48u?mix(0u,value,255u-divide(48u*255u,luma)):value;
}
static uint16_t video_ink(uint16_t value){
    uint32_t luma=album_luma(album_unpack(value));
    return luma<180u?mix(0xffffu,value,divide((180u-luma)*255u,255u-luma)):value;
}
static uint32_t album_bucket(uint16_t c){struct album_rgb v=album_unpack(c);return ((v.r>>4)<<8)|((v.g>>4)<<4)|(v.b>>4);}
static struct album_rgb album_average(const struct ln_ui_asset*a,uint32_t bucket){
    struct album_rgb sum={0,0,0};uint32_t count=0,size=a->width*a->height;
    for(uint32_t i=0;i<size;i+=4u){if(album_bucket(a->pixels[i])!=bucket)continue;
        struct album_rgb c=album_unpack(a->pixels[i]);sum.r+=c.r;sum.g+=c.g;sum.b+=c.b;++count;}
    return (struct album_rgb){divide(sum.r,count),divide(sum.g,count),divide(sum.b,count)};
}
static uint32_t album_score(uint32_t bucket,uint32_t count){
    uint32_t r=bucket>>8,g=(bucket>>4)&15u,b=bucket&15u;
    uint32_t hi=(uint32_t)max((int32_t)r,max((int32_t)g,(int32_t)b));
    uint32_t lo=(uint32_t)min((int32_t)r,min((int32_t)g,(int32_t)b));
    /* Four-bit extrema have only 256 pairs. Precompute the exact integer
     * quotient, including black, instead of dividing each occupied bucket. */
    static const uint8_t saturation_by_extrema[256]={
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        15,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        15,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        15,10,5,0,0,0,0,0,0,0,0,0,0,0,0,0,
        15,11,7,3,0,0,0,0,0,0,0,0,0,0,0,0,
        15,12,9,6,3,0,0,0,0,0,0,0,0,0,0,0,
        15,12,10,7,5,2,0,0,0,0,0,0,0,0,0,0,
        15,12,10,8,6,4,2,0,0,0,0,0,0,0,0,0,
        15,13,11,9,7,5,3,1,0,0,0,0,0,0,0,0,
        15,13,11,10,8,6,5,3,1,0,0,0,0,0,0,0,
        15,13,12,10,9,7,6,4,3,1,0,0,0,0,0,0,
        15,13,12,10,9,8,6,5,4,2,1,0,0,0,0,0,
        15,13,12,11,10,8,7,6,5,3,2,1,0,0,0,0,
        15,13,12,11,10,9,8,6,5,4,3,2,1,0,0,0,
        15,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,
        15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
    };
    uint32_t saturation=saturation_by_extrema[(hi<<4)|lo];
    return count*(saturation+8u)/8u;
}
static uint16_t album_lerp(uint16_t from,uint16_t to,uint32_t t){
    struct album_rgb a=album_unpack(from),b=album_unpack(to);
    return album_pack((struct album_rgb){
        (uint32_t)((int32_t)a.r+(((int32_t)b.r-(int32_t)a.r)*(int32_t)t>>8)),
        (uint32_t)((int32_t)a.g+(((int32_t)b.g-(int32_t)a.g)*(int32_t)t>>8)),
        (uint32_t)((int32_t)a.b+(((int32_t)b.b-(int32_t)a.b)*(int32_t)t>>8))});
}
/* Palette extraction runs before painting. Its temporary histogram can share
 * storage with the retained row cache, which is invalidated after extraction. */
#define CONTENT_ROWS 8u
#define CONTENT_NODES 8u
static union {
    uint16_t histogram[4096];
    struct {
    const struct ln_ui_state *owner;uint32_t scene,epoch,first,count,selected;
    bool valid,present[CONTENT_ROWS];char labels[CONTENT_ROWS][256];
    char texts[CONTENT_NODES][256];
} content;
} ui_workspace;
#define content (ui_workspace.content)
static void album_extract(const struct ln_ui_asset*a,uint16_t*out){
    /* Static workspace avoids placing 8 KiB on the device's runtime stack. */
    uint16_t *histogram=ui_workspace.histogram;
    __builtin_memset(ui_workspace.histogram,0,sizeof ui_workspace.histogram);
    for(uint32_t i=0;i<a->width*a->height;i+=4u)++histogram[album_bucket(a->pixels[i])];
    uint32_t dominant=0,best=0,fallback=0;
    for(uint32_t i=0;i<4096u;++i){
        if(!histogram[i])continue;
        if(histogram[i]>histogram[fallback])fallback=i;
        uint32_t r=i>>8,g=(i>>4)&15u,b=i&15u;
        if((r<2u&&g<2u&&b<2u)||(r>13u&&g>13u&&b>13u)||((r*5u+g*9u+b*2u)>>4)<2u)continue;
        uint32_t score=album_score(i,histogram[i]);if(score>best){best=score;dominant=i;}
    }
    if(!best)dominant=fallback;
    struct album_rgb bg=album_average(a,dominant);uint32_t bg_luma=album_luma(bg);
    uint32_t accent=UINT32_MAX;best=0;
    for(uint32_t i=0;i<4096u;++i){
        if(!histogram[i]||i==dominant)continue;
        struct album_rgb approx={(i>>8)*17u,((i>>4)&15u)*17u,(i&15u)*17u};
        uint32_t luma=album_luma(approx),contrast=luma>bg_luma?luma-bg_luma:bg_luma-luma;
        if(luma<30u||contrast<100u)continue;
        uint32_t score=album_score(i,histogram[i]);if(score>best){best=score;accent=i;}
    }
    uint32_t neutral=bg_luma<128u?255u:0u;
    struct album_rgb fg=accent==UINT32_MAX?(struct album_rgb){neutral,neutral,neutral}:album_average(a,accent);
    uint32_t fg_luma=album_luma(fg),contrast=fg_luma>bg_luma?fg_luma-bg_luma:bg_luma-fg_luma;
    if(contrast<100u){
        uint32_t target=bg_luma<128u?(uint32_t)min((int32_t)bg_luma+100,255):(uint32_t)max((int32_t)bg_luma-100,0);
        if(fg_luma){uint32_t scale=divide(target*256u,fg_luma);
            fg.r=(uint32_t)min((int32_t)(fg.r*scale>>8),255);fg.g=(uint32_t)min((int32_t)(fg.g*scale>>8),255);fg.b=(uint32_t)min((int32_t)(fg.b*scale>>8),255);
        }else fg=(struct album_rgb){target,target,target};
    }
    uint16_t base=album_pack(bg),ink=album_pack(fg);
    /* Map canvas to dominant, text/selector to accent, selected text to
     * dominant and separators to a quarter-strength accent. The widget
     * surface is a 36/256 blend toward inverse canvas. */
    uint16_t panel=album_lerp(base,(uint16_t)~base,36u);
    out[LN_UI_BACKGROUND]=base;out[LN_UI_FOREGROUND]=ink;out[LN_UI_ACCENT]=ink;
    out[LN_UI_PANEL]=panel;out[LN_UI_MUTED]=ink;
    out[LN_UI_SELECTION_BG]=ink;out[LN_UI_SELECTION_FG]=base;
    out[LN_UI_STATUS_BG]=base;out[LN_UI_STATUS_FG]=ink;
    out[LN_UI_BORDER]=album_lerp(base,ink,64u);
    out[LN_UI_ARTWORK_BG]=panel;out[LN_UI_ARTWORK_FG]=ink;out[LN_UI_BATTERY]=ink;
    content.valid=false;
}
uint32_t ln_ui_palette_tick(struct ln_ui_state*s,uint32_t now){
    if(!s)return UINT32_MAX;theme_sync();
    if(!(ln_ui_themes[active_theme].flags&LN_UI_THEME_ALBUM_ART)){
        album_theme=UINT32_MAX;album_colors_valid=false;album_fading=false;album_waiting=false;return UINT32_MAX;
    }
    uint32_t revision;const struct ln_ui_asset*a=ln_ui_now_playing_art(&revision);
    bool entered=album_theme!=active_theme;
    if(entered){
        album_theme=active_theme;album_colors_valid=true;album_fading=false;
        for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i)album_colors[i]=ln_ui_themes[active_theme].colors[i];
    }
    bool changed_art=entered||revision!=album_revision;
    if(changed_art){
        album_revision=revision;
        album_waiting=!(a&&a->pixels&&a->width&&a->height&&a->width<=LINNET_UI_WIDTH&&a->height<=LINNET_UI_HEIGHT);
        album_missing_at=now;
    }
    /* Keep the prior palette through disk reads; the fallback is only for
     * absent or failed artwork, never a cover that is still loading. */
    if(album_waiting&&ln_ui_now_playing_art_loading())album_missing_at=now;
    bool missing_expired=album_waiting&&now-album_missing_at>=1000000u;
    if((changed_art&&!album_waiting)||missing_expired){
        album_waiting=false;
        for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i){album_from[i]=album_colors[i];album_target[i]=ln_ui_themes[active_theme].colors[i];}
        if(a&&a->pixels&&a->width&&a->height&&a->width<=LINNET_UI_WIDTH&&a->height<=LINNET_UI_HEIGHT)album_extract(a,album_target);
        album_started=now;album_fading=false;
        for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i)if(album_from[i]!=album_target[i])album_fading=true;
        if(entered)s->dirty|=1u;
    }
    if(!album_fading)return album_waiting?album_missing_at+1000000u:UINT32_MAX;
    uint32_t elapsed=now-album_started,alpha=elapsed>=150000u?256u:divide(elapsed*256u,150000u);
    bool changed=false;
    for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i){uint16_t c=album_lerp(album_from[i],album_target[i],alpha);if(c!=album_colors[i])changed=true;album_colors[i]=c;}
    if(changed)s->dirty|=1u; /* Palette affects every region, including empty space. */
    if(elapsed>=150000u){album_fading=false;return album_waiting?album_missing_at+1000000u:UINT32_MAX;}
    return now+25000u;
}
typedef uint32_t ln_fill_word __attribute__((may_alias));
static void fill(struct painter*p,struct rect r,uint16_t c){
    r=intersect(r,p->clip);if(r.w<=0||r.h<=0)return;
    for(int32_t y=r.y;y<r.y+r.h;++y){
        uint16_t *dest=p->pixels+(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)r.x;
        int32_t left=r.w;uint32_t word=(uint32_t)c|((uint32_t)c<<16);
        if(left>0&&((uintptr_t)dest&3u)){*dest++=c;--left;}
        if(left>=2){
            /* Eight aligned word stores amortize wide-row loop overhead. */
            while(left>=16){
                ln_fill_word *out=(ln_fill_word*)dest;
                out[0]=word;out[1]=word;out[2]=word;out[3]=word;
                out[4]=word;out[5]=word;out[6]=word;out[7]=word;
                dest+=16;left-=16;
            }
            while(left>=2){*(ln_fill_word*)dest=word;dest+=2;left-=2;}
        }
        if(left>0)*dest=c;
    }
}
static void border(struct painter*p,struct rect r,uint16_t c){fill(p,(struct rect){r.x,r.y,r.w,1},c);fill(p,(struct rect){r.x,r.y+r.h-1,r.w,1},c);fill(p,(struct rect){r.x,r.y,1,r.h},c);fill(p,(struct rect){r.x+r.w-1,r.y,1,r.h},c);}
/* Consume one bounded UTF-8 character. Bad sequences consume only their lead
 * byte, so a following ASCII character is never lost. */
static uint32_t text_character(const char*s,uint32_t remaining,uint32_t*bytes){
    uint32_t ch=(uint8_t)s[0],count=1;*bytes=1;
    if(ch<0x80u)return remaining?ch:'?';
    if(ch>=0xc2u&&ch<=0xdfu){count=2;ch&=0x1fu;}
    else if(ch>=0xe0u&&ch<=0xefu){count=3;ch&=0x0fu;}
    else if(ch>=0xf0u&&ch<=0xf4u){count=4;ch&=7u;}
    else if(ch>=0x80u)return '?';
    if(count>remaining)return '?';
    for(uint32_t i=1;i<count;++i){uint32_t b=(uint8_t)s[i];if((b&0xc0u)!=0x80u)return '?';ch=(ch<<6)|(b&0x3fu);}
    if((count==2&&ch<0x80u)||(count==3&&ch<0x800u)||(count==4&&ch<0x10000u)||
       (ch>=0xd800u&&ch<=0xdfffu)||ch>0x10ffffu)return '?';
    *bytes=count;
    if(ch>=ln_ui_font.first&&ch-ln_ui_font.first<ln_ui_font.count)return ch;
    /* Current font packs contain ASCII. Preserve metadata and substitute
     * equivalent punctuation only when the font lacks the original glyph. */
    if((ch>=0x2010u&&ch<=0x2015u)||ch==0x2212u||ch==0xfe63u||ch==0xff0du)return '-';
    if(ch==0x2018u||ch==0x2019u||ch==0x201au||ch==0x201bu)return '\'';
    if(ch>=0x201cu&&ch<=0x201fu)return '"';
    if(ch==0xa0u||ch==0x202fu||(ch>=0x2000u&&ch<=0x200au))return ' ';
    return ch;
}
static uint32_t advance(uint32_t ch){
    if(ch<ln_ui_font.first||ch-ln_ui_font.first>=ln_ui_font.count)ch='?';
    return ln_ui_font.advances&&ch>=ln_ui_font.first&&ch-ln_ui_font.first<ln_ui_font.count?ln_ui_font.advances[ch-ln_ui_font.first]:ln_ui_font.advance;
}
/* Include visible overhang: glyph ink can extend beyond its advance. */
static int32_t text_width(const char*s){
    int32_t pen=0,right=0;
    if(s)for(uint32_t i=0;i<1024u&&s[i]&&s[i]!='\n';){
        uint32_t bytes,ch=text_character(s+i,1024u-i,&bytes);i+=bytes;
        if(ch<ln_ui_font.first||ch-ln_ui_font.first>=ln_ui_font.count)ch='?';
        int32_t ink=(int32_t)advance(ch);
        if(ln_ui_font.glyphs&&ch>=ln_ui_font.first&&ch-ln_ui_font.first<ln_ui_font.count){
            const struct ln_ui_glyph*g=&ln_ui_font.glyphs[ch-ln_ui_font.first];ink=(int32_t)g->x+g->w;
        }
        right=max(right,pen+ink);pen+=(int32_t)advance(ch);
    }
    return max(pen,right);
}
/* Width and ink extent only grow. Clipped labels can stop as soon as either
 * crosses the available span instead of scanning the invisible suffix. */
static bool text_fits(const char*s,int32_t width){
    int32_t pen=0,right=0;
    if(s)for(uint32_t i=0;i<1024u&&s[i]&&s[i]!='\n';){
        uint32_t bytes,ch=text_character(s+i,1024u-i,&bytes);i+=bytes;
        if(ch<ln_ui_font.first||ch-ln_ui_font.first>=ln_ui_font.count)ch='?';
        uint32_t step=advance(ch);int32_t ink=(int32_t)step;
        if(ln_ui_font.glyphs&&ch>=ln_ui_font.first&&ch-ln_ui_font.first<ln_ui_font.count){
            const struct ln_ui_glyph*g=&ln_ui_font.glyphs[ch-ln_ui_font.first];ink=(int32_t)g->x+g->w;
        }
        right=max(right,pen+ink);pen+=(int32_t)step;
        if(max(pen,right)>width)return false;
    }
    return true;
}
static void text_scaled(struct painter*p,int32_t x,int32_t y,const char*s,uint16_t c,uint32_t scale){
    if(!s||!ln_ui_font.alpha||!ln_ui_font.advance||!ln_ui_font.width||!ln_ui_font.height)return;
    uint32_t ink_rb=((c&0xf800u)<<5)|(c&31u),ink_g=(c>>5)&63u;
    bool monochrome=(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)!=0;
    int32_t start=x;uint32_t limit=0;
    while(limit<1024u&&*s){uint32_t bytes,ch=text_character(s,1024u-limit,&bytes);s+=bytes;limit+=bytes;if(ch=='\n'){x=start;y+=((int32_t)ln_ui_font.height+2)*(int32_t)scale;continue;}
        if(ch<ln_ui_font.first||ch-ln_ui_font.first>=ln_ui_font.count)ch='?';
        if(ch>=ln_ui_font.first&&ch-ln_ui_font.first<ln_ui_font.count){
            uint32_t glyph=ch-ln_ui_font.first,w=ln_ui_font.width,h=ln_ui_font.height,offset=glyph*w*h;
            int32_t gx=x,gy=y;
            if(ln_ui_font.glyphs){const struct ln_ui_glyph*g=&ln_ui_font.glyphs[glyph];gx+=(int32_t)(g->x*scale);gy+=(int32_t)(g->y*scale);w=g->w;h=g->h;offset=g->offset;}
            const uint8_t*a=ln_ui_font.alpha+offset;
            struct rect r=intersect((struct rect){gx,gy,(int32_t)(w*scale),(int32_t)(h*scale)},p->clip);
            if(scale==1u&&monochrome)for(int32_t yy=r.y;yy<r.y+r.h;++yy){
                uint16_t *dest=p->pixels+(uint32_t)yy*LINNET_UI_WIDTH+(uint32_t)r.x;
                const uint8_t *source=a+(uint32_t)(yy-gy)*w+(uint32_t)(r.x-gx);
                for(int32_t left=r.w;left>0;--left){if(*source++>=128u)*dest=c;++dest;}
            }else if(scale==1u)for(int32_t yy=r.y;yy<r.y+r.h;++yy){
                uint16_t *dest=p->pixels+(uint32_t)yy*LINNET_UI_WIDTH+(uint32_t)r.x;
                const uint8_t *source=a+(uint32_t)(yy-gy)*w+(uint32_t)(r.x-gx);
                for(int32_t left=r.w;left>0;--left){uint32_t alpha=*source++;if(alpha)*dest=alpha==255u?c:text_mix(ink_rb,ink_g,*dest,alpha);++dest;}
            }else {uint32_t shift=scale-1u;
                for(int32_t yy=r.y;yy<r.y+r.h;++yy){
                    uint16_t *dest=p->pixels+(uint32_t)yy*LINNET_UI_WIDTH+(uint32_t)r.x;
                    const uint8_t *source=a+((uint32_t)(yy-gy)>>shift)*w;
                    for(int32_t xx=r.x;xx<r.x+r.w;++xx){uint32_t alpha=source[(uint32_t)(xx-gx)>>shift];if(monochrome)alpha=alpha>=128u?255u:0u;if(alpha)*dest=alpha==255u?c:mix(c,*dest,alpha);++dest;}
                }
            }
        }
        x+=(int32_t)(advance(ch)*scale);
    }
}
static void text(struct painter*p,int32_t x,int32_t y,const char*s,uint16_t c){text_scaled(p,x,y,s,c,1u);}
static inline __attribute__((always_inline)) void clipped_text(struct painter*p,int32_t x,int32_t y,const char*s,uint16_t fg,int32_t width){
    char clipped[128];uint32_t length=0;int32_t used=0,ellipsis=text_width("...");
    while(length<124u&&s[length]){
        uint32_t bytes,ch=text_character(s+length,1024u-length,&bytes);
        if(ch=='\n'||length+bytes>124u||used+(int32_t)advance(ch)+ellipsis>width)break;
        used+=(int32_t)advance(ch);for(uint32_t i=0;i<bytes;++i){clipped[length]=s[length];++length;}
    }
    if(ellipsis<=width){clipped[length++]='.';clipped[length++]='.';clipped[length++]='.';}clipped[length]=0;text(p,x,y,clipped,fg);
}
static void fitted_text(struct painter*p,int32_t x,int32_t y,const char*s,uint16_t fg,int32_t width){
    if(!s||width<=0)return;
    if(text_fits(s,width)){text(p,x,y,s,fg);return;}
    clipped_text(p,x,y,s,fg,width);
}
static const struct ln_ui_state *scroll_owner;
static uint32_t scroll_key,scroll_started;
static int32_t scroll_pixels;
static bool scrolling_field(const struct ln_ui_node*n){
    return photo_content_layout()&&n->kind==LN_UI_TEXT&&
        (same(n->text,"Sample track")||same(n->text,"Audio pending")||same(n->text,"Track album"));
}
static void scrolling_text(struct painter*p,struct ln_ui_state*s,int32_t x,int32_t y,const char*label,uint16_t ink,int32_t width){
    int32_t overflow=max(0,text_width(label)-width);
    if(!overflow){text(p,x,y,label,ink);return;}
    struct rect saved=p->clip;p->clip=intersect(saved,(struct rect){x,y,width,(int32_t)ln_ui_font.height});
    text(p,x-(scroll_owner==s?min(scroll_pixels,overflow):0),y,label,ink);p->clip=saved;
}
static void text_block(struct painter*p,const struct ln_ui_node*n,const char*s,uint16_t fg){
    if(!s)return;
    if(!(n->flags&32u)){
        int32_t w=text_width(s),x=n->x;
        if(n->flags&8u)x+=max(0,(n->w-w)/2);else if(n->flags&16u)x+=max(0,n->w-w);
        int32_t available=n->x+n->w-x;
        if(available>0){if(w<=available)text(p,x,n->y,s,fg);else clipped_text(p,x,n->y,s,fg,available);}return;
    }
    uint32_t at=0;int32_t y=n->y;
    while(s[at]&&at<512u&&y+(int32_t)ln_ui_font.height<=n->y+n->h){
        if(y+2*(int32_t)ln_ui_font.height>n->y+n->h){fitted_text(p,n->x,y,s+at,fg,n->w);break;}
        char line[128];uint32_t len=0,last_space=0;int32_t width=0;
        while(len<127u&&at+len<512u&&s[at+len]&&s[at+len]!='\n'){
            uint32_t bytes,ch=text_character(s+at+len,512u-at-len,&bytes);
            if(len+bytes>127u||width+(int32_t)advance(ch)>n->w)break;
            width+=(int32_t)advance(ch);
            for(uint32_t i=0;i<bytes;++i){line[len]=s[at+len];++len;}
            if(ch==' ')last_space=len;
        }
        if(s[at+len]&&s[at+len]!='\n'&&last_space)len=last_space;
        if(!len)break;line[len]=0;text(p,n->x,y,line,fg);at+=len;while(s[at]==' '||s[at]=='\n')++at;y+=(int32_t)ln_ui_font.height;
    }
}
static bool photo_setting(const struct ln_ui_node*n){
    return photo_content_layout()&&
        (same(n->text,"Display timeout")||same(n->text,"Battery capacity")||
         same(n->text,"Automatic shutdown")||same(n->text,"Sleep timer")||
         same(n->text,"Clock settings")||same(n->text,"Clock fields")||
         same(n->text,"Option ")||same(n->text,"Sound ")||menu_group(n)==LN_MENU_SETTINGS);
}
static bool setting_text(struct painter*p,const struct ln_ui_node*n,struct rect r,const char*label,uint16_t ink){
    if(!photo_setting(n)||!label)return false;
    char name[128];uint32_t length=0;
    while(label[length]&&label[length]!=':'&&length<127u){name[length]=label[length];++length;}
    if(label[length]!=':'||label[length+1]!=' ')return false;
    name[length]=0;const char*value=label+length+2;
    int32_t padding=(int32_t)ln_ui_themes[active_theme].padding;
    int32_t available=r.w-2*padding,value_width=min(text_width(value),available/2);
    int32_t y=r.y+max(0,(r.h-(int32_t)ln_ui_font.height)/2)+1;
    fitted_text(p,r.x+padding,y,name,ink,available-value_width-8);
    fitted_text(p,r.x+r.w-padding-value_width,y,value,ink,value_width);
    return true;
}
static void footer_text(struct painter*p,const struct ln_ui_node*n,const char*s,uint16_t ink){
    if(!s)return;
    struct ln_ui_font saved=ln_ui_font;
    char parts[4][128];uint32_t count=0,at=0;int32_t widths[4],width=0;
    while(s[at]&&at<512u&&count<4u){
        uint32_t len=0;
        while(s[at]&&at<512u&&len<127u){
            if(s[at]==' '&&s[at+1]==' ')break;
            parts[count][len++]=s[at++];
        }
        parts[count][len]=0;while(s[at]==' '&&at<512u)++at;
        widths[count]=text_width(parts[count]);width+=widths[count];++count;
    }
    if((count&&width+(int32_t)(count-1u)*12>n->w)||(int32_t)ln_ui_font.height>n->h){
        ln_ui_font=ln_ui_hint_font;width=0;
        for(uint32_t i=0;i<count;++i){widths[i]=text_width(parts[i]);width+=widths[i];}
    }
    int32_t fh=(int32_t)ln_ui_font.height;
    if(count&&width+(int32_t)(count-1u)*12<=n->w){
        int32_t x=n->x+(count==1u?max(0,(n->w-width)/2):0);
        int32_t gap=count>1u?(n->w-width)/(int32_t)(count-1u):0;
        int32_t y=n->y+max(0,(n->h-fh)/2);
        for(uint32_t i=0;i<count;++i){text(p,x,y,parts[i],ink);x+=widths[i]+gap;}
    }else{
        char lines[3][128];uint32_t rows=0;
        for(uint32_t i=0;i<count&&rows<3u;++i){
            uint32_t start=0;
            while(parts[i][start]&&rows<3u){
                uint32_t len=0,space=0;int32_t used=0;
                while(parts[i][start+len]&&len<127u){
                    uint32_t ch=(uint8_t)parts[i][start+len];
                    if(used+(int32_t)advance(ch)>n->w)break;
                    used+=(int32_t)advance(ch);if(ch==' ')space=len;
                    ++len;
                }
                if(parts[i][start+len]&&space)len=space;
                if(!len)break;
                for(uint32_t j=0;j<len;++j)lines[rows][j]=parts[i][start+j];
                lines[rows++][len]=0;start+=len;while(parts[i][start]==' ')++start;
            }
        }
        int32_t step=fh+1,y=n->y+max(0,(n->h-(int32_t)rows*step)/2);
        for(uint32_t i=0;i<rows;++i)text(p,n->x,y+(int32_t)i*step,lines[i],ink);
    }
    ln_ui_font=saved;
}
static void gradient(struct painter*p,struct rect r,uint16_t top,uint16_t bottom){
    if((uint32_t)r.x<=LINNET_UI_WIDTH&&(uint32_t)r.y<=LINNET_UI_HEIGHT&&(uint32_t)r.w<=LINNET_UI_WIDTH&&(uint32_t)r.h<=LINNET_UI_HEIGHT){struct rect v=intersect(r,p->clip);if(v.w<=0||v.h<=0)return;}
    if(r.h<=0)return;uint32_t denominator=(uint32_t)max(1,r.h-1),step=divide(255u,denominator),remainder=255u-step*denominator,alpha=0,error=0;
    for(int32_t y=0;y<r.h;++y){fill(p,(struct rect){r.x,r.y+y,r.w,1},mix(bottom,top,alpha));alpha+=step;error+=remainder;if(error>=denominator){error-=denominator;++alpha;}}
}
/* The Classic's tall, empty pane exposes RGB565's few gray levels. Dither
 * between neutral grays instead of rounding each color channel into bands.
 * Screen coordinates keep the pattern stable across partial repaints. */
static void classic_pane_gradient(struct painter*p,struct rect r){
    static const uint8_t thresholds[8][8]={
        {0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},
        {12,44,4,36,14,46,6,38},{60,28,52,20,62,30,54,22},
        {3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},
        {15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}
    };
    struct rect v=intersect(intersect(r,p->clip),screen);
    if(v.w<=0||v.h<=0)return;
    uint32_t denominator=(uint32_t)max(1,r.h-1),range=6u*64u;
    uint32_t step=divide(range,denominator),remainder=range-step*denominator;
    uint32_t shade=30u*64u,error=0;
    for(int32_t y=r.y;y<v.y+v.h;++y){
        if(y>=v.y){
            uint32_t level=shade>>6,fraction=shade&63u;
            uint16_t low=(uint16_t)((level<<11)|(((level<<1)|(level>>4))<<5)|level);
            uint32_t next=level+(fraction!=0u);
            uint16_t high=(uint16_t)((next<<11)|(((next<<1)|(next>>4))<<5)|next);
            uint16_t row[8];
            for(uint32_t x=0;x<8u;++x)row[x]=fraction>thresholds[(uint32_t)y&7u][x]?high:low;
            uint16_t *dest=p->pixels+(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)v.x;
            for(int32_t x=v.x;x<v.x+v.w;++x)*dest++=row[(uint32_t)x&7u];
        }
        shade-=step;error+=remainder;
        if(error>=denominator){error-=denominator;--shade;}
    }
}
static uint32_t fraction(uint32_t value,uint32_t scale,uint32_t denominator){
    if(!denominator)return 0;uint32_t result=0,remainder=0;
    for(uint32_t i=0;i<scale;++i){if(remainder>=denominator-value){remainder-=denominator-value;++result;}else remainder+=value;}return result;
}
static void number(char out[11],uint32_t n){char rev[10];uint32_t len=0;do{rev[len++]=(char)('0'+n%10u);n/=10u;}while(n&&len<10u);for(uint32_t i=0;i<len;++i)out[i]=rev[len-i-1u];out[len]=0;}
static bool games_menu(const struct ln_ui_node*n){
    if(!n||n->kind!=LN_UI_MENU||!same(n->text,"Game choices"))return false;
    return true;
}
static uint32_t total(const struct ln_ui_node*n){return games_menu(n)?ln_games_count():theme_choices(n)?ln_ui_theme_count:ln_preferences_count(n,ln_ui_dynamic_count(n,menu_total(n,n->count?n->count:n->item_count)));}
static bool baked_geometry(const struct ln_ui_node*n){
    uint32_t f=n->flags;
    return n->kind>=LN_UI_VIRTUAL_LIST&&n->kind<=LN_UI_GRID&&
        (f&(LN_UI_GEOMETRY|LN_UI_GEOMETRY_RESERVED))==LN_UI_GEOMETRY&&
        ((f>>LN_UI_GEOMETRY_ROW_SHIFT)&127u)&&((f>>LN_UI_GEOMETRY_COLUMNS_SHIFT)&7u)&&
        ((f>>LN_UI_GEOMETRY_ROWS_SHIFT)&15u)&&((f>>LN_UI_GEOMETRY_WIDTH_SHIFT)&255u);
}
static uint32_t columns(const struct ln_ui_node*n){
    if(baked_geometry(n))return(n->flags>>LN_UI_GEOMETRY_COLUMNS_SHIFT)&7u;
    if(n->kind==LN_UI_GRID)return layout()==LN_UI_LAYOUT_IPOD_PHOTO?5u:layout()==LN_UI_LAYOUT_AURORA?2u:3u;
    return layout()==LN_UI_LAYOUT_BLUEPRINT&&n->kind==LN_UI_MENU&&n->h>=90?2u:1u;
}
static uint32_t row_height(const struct ln_ui_node*n){
    if(baked_geometry(n))return(n->flags>>LN_UI_GEOMETRY_ROW_SHIFT)&127u;
    if(same(n->text,"Clock fields"))return layout()==LN_UI_LAYOUT_GRAPHITE?24u:layout()==LN_UI_LAYOUT_IPOD_PHOTO?22u:(uint32_t)max(18,(int32_t)ln_ui_font.height+3);
    if(n->kind==LN_UI_VIRTUAL_LIST)return (uint32_t)max(18,(int32_t)ln_ui_font.height+5);
    if(n->kind==LN_UI_GRID)return layout()==LN_UI_LAYOUT_IPOD_PHOTO?31u:(uint32_t)max(1,n->h/2);
    if(columns(n)==2u)return 46u;
    return ln_ui_themes[active_theme].row_height;
}
static uint32_t rows(const struct ln_ui_node*n){if(baked_geometry(n))return(n->flags>>LN_UI_GEOMETRY_ROWS_SHIFT)&15u;uint32_t h=row_height(n);return n->h>0?max(1,(int32_t)divide((uint32_t)n->h,h)):1u;}
static uint32_t first_row(uint32_t selected,uint32_t capacity){return divide(selected,capacity)*capacity;}
static bool edge_list(const struct ln_ui_node*n){return (n->kind==LN_UI_MENU||n->kind==LN_UI_VIRTUAL_LIST)&&columns(n)==1u;}
static uint32_t list_first(const struct ln_ui_state*s,const struct ln_ui_node*n,uint32_t selected,bool focused){
    uint32_t capacity=rows(n),cols=columns(n);
    if(!focused||!edge_list(n))return first_row(divide(selected,cols),capacity)*cols;
    uint32_t count=total(n),first=s->first_visible[s->scene],last=count>capacity?count-capacity:0;
    if(first>last)first=last;
    if(selected<first)first=selected;
    else if(selected-first>=capacity)first=selected-capacity+1u;
    return first>last?last:first;
}

static bool decorative_asset(const struct ln_ui_node*n,const struct ln_ui_asset*a){
    bool decoration=n->kind==LN_UI_ICON||n->kind==LN_UI_IMAGE||n->kind==LN_UI_ALBUM_ART||n->kind==LN_UI_GRID||n->kind==LN_UI_PHOTO_SURFACE||n->kind==LN_UI_VIDEO_SURFACE;
    return decoration&&n->resource<ln_ui_asset_count&&a==&ln_ui_assets[n->resource];
}
static uint16_t asset_pixel(uint16_t value){
    uint32_t light=(((value>>11)&31u)*77u*255u/31u+((value>>5)&63u)*150u*255u/63u+(value&31u)*29u*255u/31u)>>8;
    if(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)light=light>=128u?255u:0u;
    return mix(color(LN_UI_ARTWORK_BG),color(LN_UI_ARTWORK_FG),light);
}
struct sample_axis {uint32_t index,error,step,remainder,denominator;};
static bool sample_axis_begin(struct sample_axis*axis,uint32_t source,uint32_t destination,uint32_t start,uint32_t count){
    /* Bound products and remainders; unsupported metadata keeps basic arithmetic. */
    if(!source||source>65535u||!destination||destination>65535u||start>=destination||count>destination-start)return false;
    axis->denominator=destination;axis->step=divide(source,destination);
    axis->remainder=source-axis->step*destination;
    uint32_t product=start*source;axis->index=divide(product,destination);
    axis->error=product-axis->index*destination;return true;
}
static uint32_t sample_axis_next(struct sample_axis*axis){
    uint32_t index=axis->index;axis->index+=axis->step;axis->error+=axis->remainder;
    if(axis->error>=axis->denominator){axis->error-=axis->denominator;++axis->index;}return index;
}
typedef uint32_t ln_image_word __attribute__((may_alias));
static void image_asset(struct painter*p,const struct ln_ui_node*n,const struct ln_ui_asset*a){
    if(!a)return;
    if(!a->pixels||!a->width||!a->height||n->w<=0||n->h<=0)return;
    struct ln_ui_node fitted;
    if(n->kind==LN_UI_ALBUM_ART||n->kind==LN_UI_PHOTO_SURFACE||n->kind==LN_UI_VIDEO_SURFACE){
        fitted=*n;
        if(a->width*(uint32_t)n->h>a->height*(uint32_t)n->w){fitted.h=(int32_t)divide(a->height*(uint32_t)n->w,a->width);fitted.y+=(n->h-fitted.h)/2;}
        else{fitted.w=(int32_t)divide(a->width*(uint32_t)n->h,a->height);fitted.x+=(n->w-fitted.w)/2;}
        if(fitted.w<=0||fitted.h<=0)return;n=&fitted;
    }
    struct rect r=intersect(bounds(n),p->clip);uint32_t source_x[LINNET_UI_WIDTH];bool decoration=decorative_asset(n,a);
    if(!r.w||!r.h)return;
    if(a->width==(uint32_t)n->w&&a->height==(uint32_t)n->h){
        bool paired=!decoration&&a->width<=LINNET_UI_WIDTH&&a->height<=LINNET_UI_HEIGHT;
        if(paired){uintptr_t source=(uintptr_t)a->pixels,target=(uintptr_t)p->pixels;
            uint32_t bytes=a->width*a->height*2u;
            /* Overlapping storage retains the original forward pixel order. */
            paired=source<target?target-source>=bytes:source-target>=LINNET_UI_WIDTH*LINNET_UI_HEIGHT*2u;
        }
#if defined(__arm__)
        if(paired&&r.x==0&&r.w==(int32_t)LINNET_UI_WIDTH&&a->width==LINNET_UI_WIDTH&&
           !(((uintptr_t)a->pixels^(uintptr_t)p->pixels)&3u)){
            /* Whole visible rows are contiguous in both bounded buffers. */
            extern void ln_memory_copy(void *,const void *,uint32_t);
            ln_memory_copy(p->pixels+(uint32_t)r.y*LINNET_UI_WIDTH,
                a->pixels+(uint32_t)(r.y-n->y)*LINNET_UI_WIDTH,(uint32_t)r.h*LINNET_UI_WIDTH*2u);
            return;
        }
#endif
        for(int32_t y=r.y;y<r.y+r.h;++y){
            const uint16_t *source=a->pixels+(uint32_t)(y-n->y)*a->width+(uint32_t)(r.x-n->x);
            uint16_t *dest=p->pixels+(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)r.x;
            if(paired&&!(((uintptr_t)source^(uintptr_t)dest)&3u)){
#if defined(__arm__)
                extern void ln_memory_copy(void *,const void *,uint32_t);
                ln_memory_copy(dest,source,(uint32_t)r.w*2u);
#else
                int32_t left=r.w;
                if((uintptr_t)dest&3u){*dest++=*source++;--left;}
                while(left>=2){*(ln_image_word*)dest=*(const ln_image_word*)source;dest+=2;source+=2;left-=2;}
                if(left>0)*dest=*source;
#endif
                continue;
            }
            if(decoration){for(int32_t x=0;x<r.w;++x)dest[x]=asset_pixel(source[x]);}
            else{for(int32_t x=0;x<r.w;++x)dest[x]=source[x];}
        }return;
    }
    struct sample_axis axis;bool fast=sample_axis_begin(&axis,a->width,(uint32_t)n->w,(uint32_t)r.x-(uint32_t)n->x,(uint32_t)r.w);
    for(int32_t x=r.x;x<r.x+r.w;++x)source_x[x-r.x]=fast?sample_axis_next(&axis):divide((uint32_t)(x-n->x)*a->width,(uint32_t)n->w);
    fast=sample_axis_begin(&axis,a->height,(uint32_t)n->h,(uint32_t)r.y-(uint32_t)n->y,(uint32_t)r.h);
    bool reuse_rows=r.w>=16&&a->height<(uint32_t)n->h&&image_independent(p,n,a);
    uint32_t previous_sy=UINT32_MAX;
    for(int32_t y=r.y;y<r.y+r.h;++y){uint32_t sy=fast?sample_axis_next(&axis):divide((uint32_t)(y-n->y)*a->height,(uint32_t)n->h);
        if(reuse_rows&&sy==previous_sy){
            uint16_t *dest=p->pixels+(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)r.x;
#if defined(__arm__)
            extern void ln_memory_copy(void *,const void *,uint32_t);
            ln_memory_copy(dest,dest-LINNET_UI_WIDTH,(uint32_t)r.w*2u);
#else
            __builtin_memcpy(dest,dest-LINNET_UI_WIDTH,(uint32_t)r.w*2u);
#endif
            continue;
        }
        previous_sy=sy;
        for(int32_t x=r.x;x<r.x+r.w;++x){uint16_t value=a->pixels[sy*a->width+source_x[x-r.x]];p->pixels[(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)x]=decoration?asset_pixel(value):value;}
    }
}
static void image(struct painter*p,const struct ln_ui_node*n){image_asset(p,n,ln_ui_dynamic_asset(n,n->resource<ln_ui_asset_count?&ln_ui_assets[n->resource]:0));}
static bool image_independent(const struct painter*p,const struct ln_ui_node*n,const struct ln_ui_asset*a){
    if(!a||!a->pixels||n->w<=0||n->w>(int32_t)LINNET_UI_WIDTH||n->h<=0||n->h>(int32_t)LINNET_UI_HEIGHT||
       !a->width||a->width>LINNET_UI_WIDTH||!a->height||a->height>LINNET_UI_HEIGHT)return false;
    /* Preserve write order if source storage aliases the framebuffer. */
    uintptr_t source=(uintptr_t)a->pixels,target=(uintptr_t)p->pixels;
    uint32_t bytes=a->width*a->height*2u;
    return source<target?target-source>=bytes:source-target>=LINNET_UI_WIDTH*LINNET_UI_HEIGHT*2u;
}
static void image_letterbox(struct painter*p,const struct ln_ui_node*n,const struct ln_ui_asset*a,uint16_t bg){
    if(a->width*(uint32_t)n->h>a->height*(uint32_t)n->w){
        int32_t h=(int32_t)divide(a->height*(uint32_t)n->w,a->width),top=(n->h-h)/2;
        fill(p,(struct rect){n->x,n->y,n->w,top},bg);
        fill(p,(struct rect){n->x,n->y+top+h,n->w,n->h-top-h},bg);
    }else{
        int32_t w=(int32_t)divide(a->width*(uint32_t)n->h,a->height),left=(n->w-w)/2;
        fill(p,(struct rect){n->x,n->y,left,n->h},bg);
        fill(p,(struct rect){n->x+left+w,n->y,n->w-left-w,n->h},bg);
    }
}
#include "ui/runtime/symbols.h"
#include "themes/ipod-photo/chrome.h"
static void photo_stripes(struct painter*p,struct rect r,const uint16_t*stripes,uint32_t count){
    for(int32_t y=0;y<r.h;++y)fill(p,(struct rect){r.x,r.y+y,r.w,1},stripes[min(y,(int32_t)count-1)]);
}
/* Blend coverage into the actual surface (including gradients and sidebars).
 * Fixed masks share optical centers and never allocate or scale on device. */
static void symbol_mask(struct painter*p,int32_t x,int32_t y,int32_t w,int32_t h,const uint8_t*mask,uint16_t ink){
    struct rect r=intersect((struct rect){x,y,w,h},p->clip);
    bool monochrome=(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)!=0;
    for(int32_t yy=r.y;yy<r.y+r.h;++yy)for(int32_t xx=r.x;xx<r.x+r.w;++xx){
        uint32_t i=(uint32_t)((yy-y)*w+xx-x),a=(mask[i>>1]>>((i&1u)?0:4))&15u;
        if(monochrome)a=a>=8u?15u:0u;
        if(a){uint16_t*dest=&p->pixels[(uint32_t)yy*LINNET_UI_WIDTH+(uint32_t)xx];*dest=a==15u?ink:mix(ink,*dest,a*17u);}
    }
}
static void symbol(struct painter*p,int32_t x,int32_t y,uint32_t kind,uint16_t ink){
    const uint8_t*mask=kind==1u?symbol_play:kind==2u?symbol_pause:kind==3u?symbol_headphones:kind==4u?symbol_lock:kind==5u?symbol_clock:0;
    if(mask)symbol_mask(p,x,y,11,11,mask,ink);
}
static void mode_icon(struct painter*p,int32_t x,int32_t y,bool shuffle,bool one,uint16_t ink){
    symbol_mask(p,x,y,15,11,shuffle?symbol_shuffle:symbol_repeat,ink);
    if(one){fill(p,(struct rect){x+7,y+3,1,5},ink);fill(p,(struct rect){x+6,y+4,1,1},ink);fill(p,(struct rect){x+6,y+7,3,1},ink);}
}
static int32_t playback_indicators(struct painter*p,int32_t right,int32_t y,uint16_t ink){
    uint32_t modes=ln_ui_playback_modes();
    if(modes&1u){right-=15;mode_icon(p,right,y,true,false,ink);right-=5;}
    if(modes>>1){right-=15;mode_icon(p,right,y,false,(modes>>1)==2u,ink);right-=5;}
    return right;
}
static void time_label(char out[6]){
    uint32_t value=ln_ui_clock_seconds();if(value>=86400u){out[0]='-';out[1]='-';out[2]=':';out[3]='-';out[4]='-';out[5]=0;return;}
    uint32_t hour=value/3600u,minute=(value-hour*3600u)/60u;
    if(ln_ui_clock_12()){hour%=12u;if(!hour)hour=12u;}
    out[0]=(char)('0'+hour/10u);out[1]=(char)('0'+hour%10u);out[2]=':';out[3]=(char)('0'+minute/10u);out[4]=(char)('0'+minute%10u);out[5]=0;
}
static void battery(struct painter*p,int32_t x,int32_t y,uint16_t ink){
    uint32_t status=ln_ui_battery_status();
    if(layout()==LN_UI_LAYOUT_IPOD_PHOTO){
        uint32_t level=status==UINT32_MAX?0u:min(status&255u,100u),width=level*21u/100u;
        if(level&&level<20u)width=max(6,(int32_t)width);
        const uint8_t*external=(status!=UINT32_MAX&&(status&256u))?photo_battery_charging:
            (status!=UINT32_MAX&&(status&512u)&&level==100u)?photo_battery_charged:0;
        struct rect r=intersect((struct rect){x,y,25,12},p->clip);
        for(int32_t yy=r.y;yy<r.y+r.h;++yy)for(int32_t xx=r.x;xx<r.x+r.w;++xx){
            uint32_t dx=(uint32_t)(xx-x),dy=(uint32_t)(yy-y),i=dy*25u+dx;
            uint16_t pixel=photo_battery_palette[external?external[i]:dx>=1u&&dx<=width?photo_battery_full[i]:photo_battery_empty[i]];
            if(!external&&dx>=1u&&dx<=width&&level<20u)pixel=photo_battery_low[dy];
            p->pixels[(uint32_t)yy*LINNET_UI_WIDTH+(uint32_t)xx]=pixel;
        }
        return;
    }
    symbol_mask(p,x,y,25,11,symbol_battery,ink);
    fill(p,(struct rect){x+1,y+1,21,9},color(LN_UI_STATUS_BG));
    if(status!=UINT32_MAX&&(status&255u)<=100u){
        bool external=(status&(256u|512u))!=0;
        bool monochrome=LINNET_BATTERY_THEME_MONOCHROME&&
            (ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)!=0;
        uint32_t width=monochrome&&external?19u:(status&255u)*19u/100u;
        struct rect charge={x+2,y+2,(int32_t)width,7};
        uint16_t charge_color=classic_layout()&&!monochrome&&(status&255u)<20u?0xe986u:color(LN_UI_BATTERY);
        if(monochrome)fill(p,charge,charge_color);
        else gradient(p,charge,mix(charge_color,color(LN_UI_BACKGROUND),130),charge_color);
        if(classic_layout()&&(status&512u)&&(status&255u)==100u){
            fill(p,(struct rect){x+10,y+3,3,6},ink);
            fill(p,(struct rect){x+8,y+2,2,4},ink);
            fill(p,(struct rect){x+13,y+2,2,4},ink);
        }else if(external)symbol_mask(p,x+6,y,11,11,symbol_bolt,monochrome?color(LN_UI_STATUS_BG):ink);
    }else {fill(p,(struct rect){x+7,y+5,9,1},ink);}
}
/* Both orientations use the same compact bolt for charging or external power,
 * including USB power when the battery is already full. */
static void vertical_battery(struct painter*p,int32_t x,int32_t y,uint16_t ink){
    uint32_t status=ln_ui_battery_status();
    symbol_mask(p,x,y,11,25,symbol_battery_vertical,ink);
    fill(p,(struct rect){x+1,y+3,9,21},color(LN_UI_PANEL));
    if(status!=UINT32_MAX&&(status&255u)<=100u){
        int32_t level=(int32_t)((status&255u)*19u/100u);
        gradient(p,(struct rect){x+2,y+23-level,7,level},mix(color(LN_UI_BATTERY),color(LN_UI_BACKGROUND),130),color(LN_UI_BATTERY));
        if(status&(256u|512u))symbol_mask(p,x,y+8,11,11,symbol_bolt,ink);
    }else fill(p,(struct rect){x+3,y+13,5,1},ink);
}
/* All preview paths use the host-authored Home menu edge. */
static struct rect classic_pane(const struct ln_ui_scene*c){
    if(!c||c->focus_node>=c->count)return(struct rect){0,0,0,0};
    const struct ln_ui_node*n=&c->nodes[c->focus_node];
    int32_t left=n->x+n->w;
    return(struct rect){left,n->y,max(0,(int32_t)LINNET_UI_WIDTH-left),max(0,(int32_t)LINNET_UI_HEIGHT-n->y)};
}
static uint32_t classic_target(const struct ln_ui_scene*c,const struct ln_ui_state*s){
    if(!c||c->focus_node>=c->count)return UINT32_MAX;
    const struct ln_ui_node*n=&c->nodes[c->focus_node];uint32_t source=menu_source(n,s->selected);
    return n->targets&&source<n->item_count?n->targets[source]:UINT32_MAX;
}
static const struct ln_ui_asset*classic_preview_art(const struct ln_ui_scene*c,const struct ln_ui_state*s){
    uint32_t target=classic_target(c,s);
    if((ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)||target>=ln_ui_scene_count)return 0;
    const char*id=ln_ui_scenes[target].id;
    if(!same(id,"linnet.system.music")&&!same(id,"linnet.system.now-playing"))return 0;
    uint32_t revision;const struct ln_ui_asset*art=ln_ui_now_playing_art(&revision);
    return art&&art->pixels&&art->width&&art->height?art:0;
}
#include "themes/classic-split/chrome.h"
static void classic_preview(struct painter*p,struct ln_ui_state*s){
    if(!classic_layout()||s->scene!=LN_UI_SCENE_HOME)return;
    const struct ln_ui_scene*c=&ln_ui_scenes[s->scene];struct rect pane=classic_pane(c);
    struct rect clip=p->clip;p->clip=intersect(clip,pane);
    const struct ln_ui_asset*art=classic_preview_art(c,s);
    if(art){
        /* Fill a tall pane with a square cover, cropping equally at both sides. */
        struct ln_ui_node album={.x=pane.x+(pane.w-pane.h)/2,.y=pane.y,.w=pane.h,.h=pane.h};
        image_asset(p,&album,art);
    }else if(layout()==LN_UI_LAYOUT_CLASSIC_SPLIT){
        if(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)fill(p,pane,color(LN_UI_BACKGROUND));
        else gradient(p,pane,color(LN_UI_BACKGROUND),color(LN_UI_ARTWORK_BG));
        split_section(p,pane,classic_target(c,s));
    }else classic_pane_gradient(p,pane);
    p->clip=clip;fill(p,(struct rect){pane.x,pane.y,1,pane.h},color(LN_UI_BORDER));
}
static void chrome(struct painter*p,struct ln_ui_state*s){
    classic_preview(p,s);
    uint16_t ink=color(LN_UI_MUTED);char clock[6];time_label(clock);
    if(layout()==LN_UI_LAYOUT_AURORA){
        fill(p,(struct rect){0,0,UI_X(26),LINNET_UI_HEIGHT},color(LN_UI_PANEL));fill(p,(struct rect){UI_X(25),0,1,LINNET_UI_HEIGHT},color(LN_UI_BORDER));
        vertical_battery(p,UI_X(7),UI_Y(10),ink);symbol(p,UI_X(7),UI_Y(47),ln_ui_playback_status(),ink);
        int32_t next=UI_Y(47);if(ln_ui_playback_status())next+=30;
        if(s->headphones){symbol(p,UI_X(7),next,3,ink);next+=30;}if(s->hold)symbol(p,UI_X(7),next,4,ink);
        clock[2]=0;text(p,max(1,(UI_X(25)-text_width(clock))/2),UI_Y(142),clock,ink);
        text(p,max(1,(UI_X(25)-text_width(clock+3))/2),UI_Y(157),clock+3,ink);
    }else if(layout()==LN_UI_LAYOUT_GRAPHITE||layout()==LN_UI_LAYOUT_SUNSET){
        fill(p,(struct rect){0,UI_Y(154),LINNET_UI_WIDTH,LINNET_UI_HEIGHT-UI_Y(154)},color(LN_UI_PANEL));
        int32_t next=UI_X(10);uint32_t play=ln_ui_playback_status();
        if(play){symbol(p,next,UI_Y(159),play,ink);next+=19;}if(s->headphones){symbol(p,next,UI_Y(159),3,ink);next+=19;}if(s->hold)symbol(p,next,UI_Y(159),4,ink);
        symbol(p,UI_X(155),UI_Y(159),5,ink);fitted_text(p,UI_X(170),UI_Y(154)+max(0,(UI_Y(22)-(int32_t)ln_ui_font.height)/2),clock,ink,UI_X(48));
    }
}
static uint32_t battery_signature(void){
    uint32_t status=ln_ui_battery_status(),level=status&255u,style=layout();
    /* Track visible pixels rather than percentage noise within one icon
     * column. The full-width Minimal bar still needs every percentage. */
    if(style==LN_UI_LAYOUT_MINIMAL)return status&1023u;
    if(style==LN_UI_LAYOUT_IPOD_PHOTO){
        if(status==UINT32_MAX)return 0;
        level=min(level,100u);
        if(status&256u)return 256u;
        if((status&512u)&&level==100u)return 512u;
        uint32_t width=level*21u/100u;
        if(level&&level<20u)width=max(6,(int32_t)width);
        return width|((level&&level<20u)?64u:0u);
    }
    if(status==UINT32_MAX||level>100u)return 0;
    bool external=(status&(256u|512u))!=0;
    bool monochrome=LINNET_BATTERY_THEME_MONOCHROME&&
        (ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)!=0;
    uint32_t width=monochrome&&external&&style!=LN_UI_LAYOUT_AURORA?19u:level*19u/100u;
    return 32u|width|(external?128u:0u)|
        ((classic_layout()&&level<20u)?64u:0u)|
        ((classic_layout()&&(status&512u)&&level==100u)?256u:0u);
}
static uint32_t status_signature(void){
    uint32_t seconds=ln_ui_clock_seconds(),minutes=seconds<86400u?seconds/60u:2047u;
    return battery_signature()|(minutes<<10)|((ln_ui_playback_status()&3u)<<21)|((ln_ui_playback_modes()&7u)<<23);
}
static const struct ln_ui_state*preview_owner;
static const struct ln_ui_asset*preview_art;
static uint32_t preview_revision;
bool ln_ui_status_refresh(struct ln_ui_state*s){
    if(!s)return false;theme_sync();bool preview_changed=false;
    if(classic_layout()&&s->scene==LN_UI_SCENE_HOME){
        uint32_t revision;const struct ln_ui_asset*art=ln_ui_now_playing_art(&revision);
        preview_changed=preview_owner!=s||preview_art!=art||preview_revision!=revision;
        if(preview_changed)s->dirty|=64u;
        preview_owner=s;preview_art=art;preview_revision=revision;
    }else preview_owner=0;
    uint32_t status=status_signature();if(s->drawn_status==status)return preview_changed;
    /* Restoring preferences can finish while Settings is already visible. */
    if(((s->drawn_status^status)&(7u<<23))&&s->scene==LN_UI_SCENE_SETTINGS)s->dirty|=1u;
    else s->dirty|=4u;return true;
}
static void draw_node(struct painter*p,const struct ln_ui_node*n,struct ln_ui_state*s,bool focused){
    if(n->flags&2u)return;
    struct rect previous=p->clip;p->clip=intersect(previous,bounds(n));if(!p->clip.w||!p->clip.h){p->clip=previous;return;}
    uint16_t fg=ln_ui_color(n->fg),bg=ln_ui_color(n->bg);int32_t row_h=(int32_t)row_height(n);
    if(s->scene==LN_UI_SCENE_VIDEO_CONTROLS&&n->kind==LN_UI_TEXT)fg=video_ink(fg);
    const char *node_text=s->scene==LN_UI_SCENE_GAMES&&same(n->text,"Game description")?
        ln_games_description(s->selected):s->scene==LN_UI_SCENE_GAMES&&same(n->text,"Game music")?
        ln_games_text("Game music"):s->scene==LN_UI_SCENE_GAME_PLAY&&n->text&&
        (same(n->text,"Game title")||same(n->text,"Game score")||same(n->text,"Game hint")||
         same(n->text,"Game overlay")||same(n->text,"Game detail"))?
        ln_games_text(n->text):ln_ui_clock_text(n,UINT32_MAX,ln_preferences_text(n,UINT32_MAX,ln_display_settings_text(n,UINT32_MAX,ln_ui_dynamic_text(n,UINT32_MAX,n->text))));
    struct ln_ui_node metadata;
    if(scrolling_field(n)){
        uint32_t revision;const struct ln_ui_asset*art=ln_ui_now_playing_art(&revision);
        metadata=*n;
        if(art&&art->pixels){metadata.x=88;metadata.w=122;}
        else {int32_t width=min(metadata.w,text_width(node_text));metadata.x+=(metadata.w-width)/2;metadata.w=width;}
        n=&metadata;
    }
    switch(n->kind){
    case LN_UI_SCREEN:{
        bool preview=false;
        if(classic_layout()&&s->scene==LN_UI_SCENE_HOME){
            const struct ln_ui_scene*c=&ln_ui_scenes[s->scene];
            if(c->nodes&&c->focus_node<c->count){
                const struct ln_ui_asset*art=classic_preview_art(c,s);
                struct rect pane=classic_pane(c);
                struct ln_ui_node album={.x=pane.x+(pane.w-pane.h)/2,.y=pane.y,.w=pane.h,.h=pane.h};
                preview=!art||image_independent(p,&album,art);
            }
        }
        struct rect area=p->clip,pane=intersect(area,classic_pane(&ln_ui_scenes[s->scene]));
        if(preview&&pane.w&&pane.h){
            fill(p,(struct rect){area.x,area.y,area.w,pane.y-area.y},bg);
            fill(p,(struct rect){area.x,pane.y+pane.h,area.w,area.y+area.h-pane.y-pane.h},bg);
            fill(p,(struct rect){area.x,pane.y,pane.x-area.x,pane.h},bg);
            fill(p,(struct rect){pane.x+pane.w,pane.y,area.x+area.w-pane.x-pane.w,pane.h},bg);
        }else fill(p,bounds(n),bg);
        chrome(p,s);break;}
    case LN_UI_VIEW:fill(p,bounds(n),bg);break;
    case LN_UI_TEXT:
        if((n->flags&LN_UI_FOOTER)&&!same(n->text,"Theme status")){
            if(same(n->text,"Clock source"))node_text=ln_ui_clock_text(n,UINT32_MAX,ln_ui_clock_seconds()<86400u?"Device clock":"Clock unavailable");
            footer_text(p,n,node_text,fg);
        }else if(same(n->text,"System clock")){char clock[9];time_label(clock);if(ln_ui_clock_12()&&ln_ui_clock_seconds()<86400u){clock[5]=' ';clock[6]=ln_ui_clock_seconds()<43200u?'A':'P';clock[7]='M';clock[8]=0;}
            struct ln_ui_font saved=ln_ui_font;ln_ui_font=ln_ui_clock_font;
            int32_t width=text_width(clock),y=n->y+max(0,(n->h-(int32_t)ln_ui_font.height)/2);
            fitted_text(p,n->x+max(0,(n->w-width)/2),y,clock,fg,n->w);ln_ui_font=saved;}
        else if(same(n->text,"Clock source")){text_block(p,n,ln_ui_clock_text(n,UINT32_MAX,ln_ui_clock_seconds()<86400u?"Device clock":"Clock unavailable"),fg);}
        else if(same(n->text,"Theme status")){
            char status[32];uint32_t at=0;const char *part=ln_ui_theme_status();
            while(*part&&at<29u)status[at++]=*part++;status[at++]=':';status[at++]=' ';part=ln_ui_themes[active_theme].name;
            while(*part&&at<31u)status[at++]=*part++;status[at]=0;footer_text(p,n,status,fg);
        }else if(photo_content_layout()&&same(n->text,"Track position")){
            int32_t right=playback_indicators(p,n->x+n->w,n->y+1,fg);
            p->clip=intersect(p->clip,(struct rect){n->x,n->y,max(0,right-n->x),n->h});
            int32_t x=n->x;
            for(uint32_t i=0;node_text&&node_text[i]&&i<32u;++i){
                char c=node_text[i];int32_t glyph=c>='0'&&c<='9'?c-'0':c=='o'?10:c=='f'?11:-1;
                if(glyph>=0){symbol_mask(p,x,n->y,8,13,photo_position_glyphs+glyph*52,fg);x+=photo_position_advance[glyph];}
                else if(c=='-'){fill(p,(struct rect){x+1,n->y+6,4,1},fg);x+=5;}
                else x+=3;
            }
        }else if(same(n->text,"Native playback shell")&&!photo_content_layout()){
            struct ln_ui_node notice=*n;
            int32_t right=playback_indicators(p,n->x+n->w,n->y+max(0,(n->h-11)/2),color(LN_UI_FOREGROUND));
            notice.w=max(0,right-n->x);text_block(p,&notice,node_text,fg);
        }else if(scrolling_field(n))scrolling_text(p,s,n->x,n->y,node_text,fg,n->w);
        else text_block(p,n,node_text,fg);break;
    case LN_UI_ICON:case LN_UI_IMAGE:image(p,n);break;
    case LN_UI_VIRTUAL_LIST:case LN_UI_MENU:case LN_UI_GRID:{
        bool photo_grid=n->kind==LN_UI_GRID&&s->scene==LN_UI_SCENE_PHOTOS_GRID;
        bool photo_list=layout()==LN_UI_LAYOUT_IPOD_PHOTO&&n->kind!=LN_UI_GRID;
        bool classic_list=classic_layout()&&n->kind!=LN_UI_GRID;
        if(!photo_list&&!classic_list)fill(p,bounds(n),bg);uint32_t count=total(n),cols=columns(n);
        uint32_t capacity=rows(n),selected=focused?s->selected:n->index;
        uint32_t first=list_first(s,n,selected,focused);
        int32_t w=(int32_t)(baked_geometry(n)?(n->flags>>LN_UI_GEOMETRY_WIDTH_SHIFT)&255u:divide((uint32_t)n->w,cols));
        if(photo_list||classic_list){
            w=n->w-(count>capacity&&n->w==220?10:0);
            int32_t used=(int32_t)min(capacity,count>first?count-first:0)*row_h;
            fill(p,(struct rect){n->x,n->y+used,n->w,max(0,n->h-used)},bg);
        }
        uint32_t column=0,row=0;
        for(uint32_t i=first;i<count&&i-first<capacity*cols;++i){
            struct rect r={n->x+(int32_t)column*w,n->y+(int32_t)row*row_h,w,row_h};
            if(++column==cols){column=0;++row;}
            struct rect visible=intersect(p->clip,r);if(!visible.w||!visible.h)continue;
            bool chosen=focused&&i==s->selected;uint16_t ink=chosen?color(LN_UI_SELECTION_FG):fg;
            if(photo_grid){
                const struct ln_ui_asset*a=ln_ui_dynamic_item_asset(n,i,0);
                struct ln_ui_node thumbnail=*n;
                int32_t tw=a?a->width:LINNET_TARGET_NUMBER==1238u?56:LINNET_TARGET_NUMBER==25u?50:40;
                int32_t th=a?a->height:LINNET_TARGET_NUMBER==1238u?56:LINNET_TARGET_NUMBER==25u?38:30;
                thumbnail.x=r.x+(r.w-tw)/2;thumbnail.y=r.y+(r.h-th)/2;thumbnail.w=tw;thumbnail.h=th;
                struct rect saved=p->clip;p->clip=intersect(saved,r);
                if(a)image_asset(p,&thumbnail,a);
                if(chosen)border(p,bounds(&thumbnail),0xffe0u);
                p->clip=saved;continue;
            }
            struct rect card=r;
            if(layout()==LN_UI_LAYOUT_GRAPHITE&&n->kind!=LN_UI_VIRTUAL_LIST){card.y+=2;card.h-=4;}
            if(layout()==LN_UI_LAYOUT_BLUEPRINT&&cols==2u){card.x+=2;card.y+=2;card.w-=4;card.h-=4;}
            if(card.x!=r.x||card.y!=r.y||card.w!=r.w||card.h!=r.h)fill(p,r,bg);
            if(layout()!=LN_UI_LAYOUT_IPOD_PHOTO||(photo_list&&!chosen))fill(p,card,chosen?color(LN_UI_SELECTION_BG):layout()==LN_UI_LAYOUT_GRAPHITE&&n->kind!=LN_UI_VIRTUAL_LIST?color(LN_UI_PANEL):bg);
            if(chosen&&classic_list&&layout()==LN_UI_LAYOUT_CLASSIC_SPLIT&&!(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)){
                fill(p,(struct rect){card.x,card.y,card.w,1},mix(color(LN_UI_SELECTION_BG),0xffffu,210));
                fill(p,(struct rect){card.x,card.y+card.h-1,card.w,1},mix(color(LN_UI_SELECTION_BG),0x0000u,230));
            }
            if(chosen&&classic_list&&layout()!=LN_UI_LAYOUT_CLASSIC_SPLIT){gradient(p,card,mix(color(LN_UI_SELECTION_BG),0xffffu,205),mix(color(LN_UI_SELECTION_BG),0x0000u,220));fill(p,(struct rect){card.x,card.y,card.w,1},mix(color(LN_UI_SELECTION_BG),0xffffu,150));}
            if(n->kind!=LN_UI_VIRTUAL_LIST&&(layout()==LN_UI_LAYOUT_GRAPHITE||layout()==LN_UI_LAYOUT_BLUEPRINT))border(p,card,chosen?color(LN_UI_ACCENT):color(LN_UI_BORDER));
            if(layout()==LN_UI_LAYOUT_AURORA)fill(p,(struct rect){r.x,r.y,2,r.h},chosen?color(LN_UI_ACCENT):color(LN_UI_PANEL));

            if(chosen&&(ln_ui_themes[active_theme].flags&LN_UI_THEME_APPLE_PHOTO))photo_stripes(p,r,photo_selection,22);
            struct rect saved=p->clip;p->clip=intersect(saved,r);char num[11],fallback_label[144];const char*label=0;
            uint32_t source=menu_source(n,i);if(n->items&&source<n->item_count)label=n->items[source];label=games_menu(n)?ln_games_label(i):theme_choices(n)?ln_ui_themes[i].name:ln_ui_clock_text(n,i,ln_preferences_text(n,i,ln_display_settings_text(n,i,ln_ui_dynamic_text(n,i,label))));if(same(n->text,"Clock settings")&&i==1u)label=ln_ui_clock_12()?"Time format: 12-hour":"Time format: 24-hour";if(!label){number(num,i+1u);label=num;
                if(node_text&&n->kind!=LN_UI_GRID){uint32_t len=0;while(len<128u&&node_text[len]){fallback_label[len]=node_text[len];++len;}
                    fallback_label[len++]=' ';for(uint32_t digit=0;num[digit]&&len<143u;++digit)fallback_label[len++]=num[digit];fallback_label[len]=0;label=fallback_label;}
            }
            if(n->targets&&source<n->item_count){
                uint32_t action=n->targets[source],modes=ln_ui_playback_modes();
                if(action==LINNET_UI_ACTION_SHUFFLE&&!(classic_layout()&&menu_group(n)==LN_MENU_HOME))label=(modes&1u)?"Shuffle: Songs":"Shuffle: Off";
                else if(action==LINNET_UI_ACTION_REPEAT)label=(modes>>1)==2u?"Repeat: One":(modes>>1)==1u?"Repeat: All":"Repeat: Off";
            }
            if(n->kind==LN_UI_GRID){
                const struct ln_ui_asset*a=ln_ui_dynamic_item_asset(n,i,n->resource<ln_ui_asset_count?&ln_ui_assets[n->resource]:0);
                struct ln_ui_node thumbnail=*n;int32_t size=layout()==LN_UI_LAYOUT_IPOD_PHOTO?27:min(56,min(w-8,row_h-(int32_t)ln_ui_font.height-8));
                thumbnail.x=r.x+(w-size)/2;thumbnail.y=r.y+2;thumbnail.w=size;thumbnail.h=size;
                image_asset(p,&thumbnail,a);if(layout()!=LN_UI_LAYOUT_IPOD_PHOTO)fitted_text(p,r.x+3,r.y+size+5,label,ink,r.w-6);
            }else {
                bool mark=theme_choices(n)&&i==active_theme;
                bool arrow=apple_style()&&((n->targets&&source<n->item_count&&n->targets[source]<ln_ui_scene_count)||
                    (n->kind==LN_UI_VIRTUAL_LIST&&ln_ui_dynamic_navigation(n,i)));
                int32_t padding=(int32_t)ln_ui_themes[active_theme].padding;
                if(layout()==LN_UI_LAYOUT_SUNSET&&n->kind!=LN_UI_VIRTUAL_LIST){char ordinal[11];number(ordinal,i+1u);text(p,r.x+2,r.y+max(0,(row_h-(int32_t)ln_ui_font.height)/2),ordinal,ink);padding=max(28,text_width(ordinal)+10);}
                if(cols==2u){struct ln_ui_node tile=*n;tile.x=r.x+7;tile.y=r.y+7;tile.w=r.w-14;tile.h=r.h-12;tile.flags=32u;text_block(p,&tile,label,ink);}
                else if(setting_text(p,n,r,label,ink)){}
                else if(chosen&&(layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout()||s->scene==LN_UI_SCENE_ABOUT))scrolling_text(p,s,r.x+padding,r.y+max(0,(row_h-(int32_t)ln_ui_font.height)/2)+(layout()==LN_UI_LAYOUT_IPOD_PHOTO?1:0),label,ink,r.w-padding-6-(mark||arrow?12:0));
                else fitted_text(p,r.x+padding,r.y+max(0,(row_h-(int32_t)ln_ui_font.height)/2)+(layout()==LN_UI_LAYOUT_IPOD_PHOTO?1:0),label,ink,r.w-padding-6-(mark||arrow?12:0));
                if(mark)symbol_mask(p,r.x+r.w-16,r.y+(row_h-11)/2,11,11,symbol_check,ink);
                else if(arrow){
                    if(layout()==LN_UI_LAYOUT_IPOD_PHOTO)symbol_mask(p,r.x+r.w-24,r.y+6,7,11,photo_chevron,ink);
                    else if(layout()==LN_UI_LAYOUT_CLASSIC_SPLIT)split_triangle(p,r.x+r.w-11,r.y+(row_h-7)/2,ink);
                    else symbol_mask(p,r.x+r.w-16,r.y+(row_h-11)/2,11,11,symbol_chevron,ink);
                }
            }
            p->clip=saved;++s->visible_items;
        }break;}
    case LN_UI_SCROLLBAR:{
        uint32_t count=theme_choices(n)?ln_ui_theme_count:ln_ui_dynamic_count(n,n->count?n->count:1u),capacity=1u;
        const struct ln_ui_scene*c=&ln_ui_scenes[s->scene];
        if(c->focus_node<c->count){const struct ln_ui_node*f=&c->nodes[c->focus_node];
            if(f->kind==LN_UI_VIRTUAL_LIST||f->kind==LN_UI_MENU||f->kind==LN_UI_GRID){count=total(f);capacity=rows(f)*columns(f);}}
        if(count<=capacity){if(!photo_content_layout())fill(p,bounds(n),bg);break;}
        fill(p,bounds(n),bg);
        const struct ln_ui_node*f=c->focus_node<c->count?&c->nodes[c->focus_node]:0;
        uint32_t first=f?list_first(s,f,s->selected,true):0;
        uint32_t last=f&&edge_list(f)?count-capacity:first_row(count-1u,capacity);
        int32_t height=min(n->h,max(4,(int32_t)fraction(capacity,(uint32_t)n->h,count)));
        int32_t top=(int32_t)fraction(first,(uint32_t)(n->h-height),last);
        if(layout()==LN_UI_LAYOUT_IPOD_PHOTO){
            for(int32_t x=0;x<n->w;++x){
                fill(p,(struct rect){n->x+x,n->y,1,n->h},photo_scroll_empty[min(x,9)]);
                fill(p,(struct rect){n->x+x,n->y+top,1,height},photo_scroll_full[min(x,9)]);
            }
        }else if(classic_layout()&&!(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)){
            gradient(p,bounds(n),0xe71cu,0xbdf7u);border(p,bounds(n),color(LN_UI_BORDER));
            fill(p,(struct rect){n->x+2,n->y+top+1,max(1,n->w-4),max(2,height-2)},color(LN_UI_MUTED));
        }else fill(p,(struct rect){n->x,n->y+top,n->w,height},fg);break;}

    case LN_UI_PROGRESS_BAR:{uint32_t count=ln_ui_dynamic_count(n,n->count?n->count:100u),value=ln_ui_dynamic_value(n,n->index);int32_t width=(int32_t)fraction(value>count?count:value,(uint32_t)n->w,count);
        if(s->scene==LN_UI_SCENE_VIDEO_CONTROLS){
            fg=video_ink(color(LN_UI_ACCENT));bg=mix(fg,video_background(color(LN_UI_PANEL)),56u);
            fill(p,bounds(n),bg);fill(p,(struct rect){n->x,n->y,width,n->h},fg);
        }else if(layout()==LN_UI_LAYOUT_IPOD_PHOTO){
            photo_stripes(p,bounds(n),photo_progress_empty,11);
            struct rect r=intersect((struct rect){n->x,n->y,width,n->h},p->clip);
            for(int32_t y=r.y;y<r.y+r.h;++y)for(int32_t x=r.x;x<r.x+r.w;++x)
                p->pixels[(uint32_t)y*LINNET_UI_WIDTH+(uint32_t)x]=photo_progress_full[min(y-n->y,10)*5+(x-n->x)%5];
        }else if(classic_layout()&&!(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME)){
            gradient(p,bounds(n),0xffffu,0xbdf7u);border(p,bounds(n),color(LN_UI_BORDER));
            if(width>2)gradient(p,(struct rect){n->x+1,n->y+1,width-2,max(1,n->h-2)},0x6ddfu,0x2457u);
        }else {fill(p,bounds(n),bg);fill(p,(struct rect){n->x,n->y,width,n->h},fg);}break;}
    case LN_UI_STATUS_BAR:{
        fg=color(LN_UI_STATUS_FG);bg=color(LN_UI_STATUS_BG);
        if(s->scene==LN_UI_SCENE_VIDEO_CONTROLS){fg=video_ink(fg);bg=video_background(bg);fill(p,bounds(n),bg);}
        else if(layout()==LN_UI_LAYOUT_IPOD_PHOTO)photo_stripes(p,bounds(n),photo_header,21);
        else if(classic_layout()&&!(ln_ui_themes[active_theme].flags&LN_UI_THEME_MONOCHROME))gradient(p,bounds(n),0xffffu,0xbdf7u);
        else fill(p,bounds(n),bg);
        fill(p,(struct rect){n->x,n->y+n->h-1,n->w,1},color(LN_UI_BORDER));
        int32_t left=n->x+5,right=n->x+n->w-5,icon_y=n->y+max(0,(n->h-11)/2);
        if(layout()!=LN_UI_LAYOUT_AURORA&&layout()!=LN_UI_LAYOUT_MINIMAL){
            battery(p,right-25,icon_y,fg);right-=31;
            if(layout()==LN_UI_LAYOUT_IPOD_PHOTO){
                uint32_t play=ln_ui_playback_status();if(play){if(play==1u)symbol_mask(p,left+3,n->y+3,15,15,photo_play,fg);else symbol(p,left+3,icon_y,play,fg);left+=20;}
                if(s->hold){right-=14;symbol(p,right,icon_y,4,fg);}
            }else if(classic_layout()){
                if(s->hold){right-=14;symbol(p,right,icon_y,4,fg);}
                uint32_t play=ln_ui_playback_status();
                if(play){right-=14;symbol(p,right,icon_y,play,fg);}
            }else if(layout()==LN_UI_LAYOUT_BLUEPRINT){char clock[6];time_label(clock);int32_t clock_width=max(36,text_width(clock));fitted_text(p,left,n->y+max(0,(n->h-(int32_t)ln_ui_font.height)/2),clock,fg,clock_width);left+=clock_width+6;
                if(s->headphones){symbol(p,left,icon_y,3,fg);left+=14;}if(s->hold){symbol(p,left,icon_y,4,fg);left+=14;}}
            else if(layout()==LN_UI_LAYOUT_DEFAULT){if(s->headphones){symbol(p,left,icon_y,3,fg);left+=14;}if(s->hold){symbol(p,left,icon_y,4,fg);left+=14;}}
            if(layout()==LN_UI_LAYOUT_DEFAULT||layout()==LN_UI_LAYOUT_BLUEPRINT){uint32_t play=ln_ui_playback_status();if(play){symbol(p,left,icon_y,play,fg);left+=14;}}
        }
        if(layout()==LN_UI_LAYOUT_MINIMAL){uint32_t play=ln_ui_playback_status();if(play){symbol(p,left,icon_y,play,fg);left+=14;}if(s->hold){symbol(p,left,icon_y,4,fg);left+=14;}}
        const char *title=node_text?node_text:ln_ui_scenes[s->scene].title;
        int32_t x=left;
        if(classic_layout()&&s->scene==LN_UI_SCENE_HOME){
            x=n->x+7;
        }else if(layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout()){
            x=max(left,min(n->x+(n->w-text_width(title)+1)/2,right-text_width(title)));
        }else if(layout()==LN_UI_LAYOUT_BLUEPRINT){
            x=max(left,min(n->x+(n->w-text_width(title))/2,right-text_width(title)));
        }
        int32_t title_y=n->y+max(0,(n->h-(int32_t)ln_ui_font.height)/2);
        if(layout()==LN_UI_LAYOUT_DEFAULT||layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout())++title_y;
        fitted_text(p,x,title_y,title,fg,right-x);
        break;}
    case LN_UI_DIALOG:case LN_UI_TOAST:fill(p,bounds(n),bg);border(p,bounds(n),fg);fitted_text(p,n->x+7,n->y+max(0,(n->h-(int32_t)ln_ui_font.height)/2),node_text,fg,n->w-14);break;
    case LN_UI_ALBUM_ART:case LN_UI_PHOTO_SURFACE:case LN_UI_VIDEO_SURFACE:{
        if(n->kind==LN_UI_ALBUM_ART&&(layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout())){
            uint32_t revision;const struct ln_ui_asset*art=ln_ui_now_playing_art_sized(&revision,(uint32_t)n->w);
            if(image_independent(p,n,art))image_letterbox(p,n,art,bg);
            else fill(p,bounds(n),bg);
            if(art&&art->pixels)image_asset(p,n,art);break;
        }
        /* Asset providers are side-effect-free retained-memory lookups. Moving
         * these writes is not a contract for framebuffer-inspecting overrides. */
        bool video=n->kind==LN_UI_VIDEO_SURFACE;
        if(video){bg=video_background(bg);fg=video_ink(fg);}
        const struct ln_ui_asset*a=video?ln_ui_dynamic_asset(n,n->resource<ln_ui_asset_count?&ln_ui_assets[n->resource]:0):0;
        if(video&&image_independent(p,n,a)){
            /* Paint only the letterbox bars: the image overwrites the interior. */
            image_letterbox(p,n,a,bg);
        }else {fill(p,bounds(n),bg);if(!video)border(p,bounds(n),color(LN_UI_BORDER));}
        if(video)image_asset(p,n,a);else image(p,n);
        if(video){struct ln_ui_node label=*n;label.x+=8;label.w=max(1,label.w-16);label.y+=max(0,(label.h-(int32_t)ln_ui_font.height)/2);label.h=(int32_t)ln_ui_font.height;label.flags=8u;text_block(p,&label,node_text?node_text:"",fg);}
        else text(p,n->x+7,n->y+7,node_text?node_text:"Preview",fg);break;}
    case LN_UI_CANVAS_SURFACE:{
        if(!n->canvas||!(n->flags&LN_UI_CANVAS_OPAQUE))fill(p,bounds(n),bg);
        if(n->canvas){const struct ln_ui_canvas_context context={p->pixels,n->x,n->y,n->w,n->h,p->clip.x,p->clip.y,p->clip.w,p->clip.h,s->animation,fg,bg};n->canvas(&context);}
        else {border(p,bounds(n),fg);text(p,n->x+7,n->y+7,node_text,fg);}
        break;}
    default:break;
    }if(n->flags&4u)border(p,bounds(n),fg);p->clip=previous;
}
static const struct ln_ui_scene*scene(const struct ln_ui_state*s){return s->scene<ln_ui_scene_count&&s->scene<LINNET_UI_MAX_SCENES?&ln_ui_scenes[s->scene]:0;}
static const struct ln_ui_node*focus(const struct ln_ui_state*s){const struct ln_ui_scene*c=scene(s);return c&&c->focus_node<c->count?&c->nodes[c->focus_node]:0;}
uint32_t ln_ui_visible_capacity(const struct ln_ui_state*s){
    theme_sync();const struct ln_ui_node*n=focus(s);if(!n||(n->kind!=LN_UI_MENU&&n->kind!=LN_UI_VIRTUAL_LIST&&n->kind!=LN_UI_GRID))return 0;
    return rows(n)*columns(n);
}
uint32_t ln_ui_first_visible(struct ln_ui_state*s){
    if(!s||s->scene>=LINNET_UI_MAX_SCENES)return 0;
    theme_sync();const struct ln_ui_node*n=focus(s);
    if(!n||(n->kind!=LN_UI_MENU&&n->kind!=LN_UI_VIRTUAL_LIST&&n->kind!=LN_UI_GRID))return 0;
    return s->first_visible[s->scene]=list_first(s,n,s->selected,true);
}
uint32_t ln_ui_focused_count(const struct ln_ui_state*s){
    theme_sync();const struct ln_ui_node*n=focus(s);return n&&(n->kind==LN_UI_MENU||n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_GRID)?total(n):0;
}
static bool change_scene_to(struct ln_ui_state*s,uint32_t target,bool remember){if(target>=ln_ui_scene_count||target>=LINNET_UI_MAX_SCENES||!(ln_ui_enabled_scenes&(UINT64_C(1)<<target))||target==s->scene)return false;bool editor=target==LN_UI_SCENE_CLOCK_EDIT;if(editor&&!ln_ui_clock_begin()){s->dirty|=1u;return false;}
    if(s->scene==LN_UI_SCENE_GAME_PLAY)ln_games_stop();
    if(!target)s->history_count=0;
    else if(remember){
        /* Explicit Menu handlers return to the parent with ln_ui_show().
         * Consume that history entry so Menu there keeps going toward Home. */
        if(s->history_count&&s->history[s->history_count-1u]==target)--s->history_count;
        else {if(s->history_count==sizeof s->history){for(uint32_t i=1;i<sizeof s->history;++i)s->history[i-1]=s->history[i];--s->history_count;}s->history[s->history_count++]=(uint8_t)s->scene;}
    }
    s->selections[s->scene]=s->selected;s->scene=target;s->selected=editor?0u:s->selections[target];const struct ln_ui_node*n=focus(s);uint32_t count=n?total(n):0;if(theme_choices(n))s->selected=ln_ui_theme_index();if(!count)s->selected=0;else if(s->selected>=count)s->selected=count-1u;s->dirty|=1u;++s->transitions;++s->scene_epoch;return true;}
static bool change_scene(struct ln_ui_state*s,uint32_t target){return change_scene_to(s,target,true);}
static bool previous_scene(struct ln_ui_state*s){while(s->history_count){uint32_t target=s->history[--s->history_count];if(target<ln_ui_scene_count&&(ln_ui_enabled_scenes&(UINT64_C(1)<<target)))return change_scene_to(s,target,false);}return change_scene_to(s,0,false);}
bool ln_ui_back(struct ln_ui_state*s){return s&&previous_scene(s);}
void ln_ui_refresh(struct ln_ui_state*s){if(!s)return;theme_sync();const struct ln_ui_node*n=focus(s);uint32_t count=n?total(n):0;if(!count)s->selected=0;else if(s->selected>=count)s->selected=count-1u;if(s->scene<LINNET_UI_MAX_SCENES)s->selections[s->scene]=s->selected;s->dirty|=1u;}
void ln_ui_refresh_content(struct ln_ui_state*s){
    if(!s)return;const struct ln_ui_node*n=focus(s);uint32_t count=n?total(n):0;
    if(!count)s->selected=0;else if(s->selected>=count)s->selected=count-1u;
    if(s->scene<LINNET_UI_MAX_SCENES)s->selections[s->scene]=s->selected;s->dirty|=16u;
}
bool ln_ui_show(struct ln_ui_state*s,const char*id){if(!s||!id)return false;for(uint32_t i=0;i<ln_ui_scene_count&&i<LINNET_UI_MAX_SCENES;++i){const char*other=ln_ui_scenes[i].id;uint32_t j=0;if(!other)continue;while(j<128u&&id[j]&&id[j]==other[j])++j;if(j<128u&&!id[j]&&!other[j])return change_scene(s,i);}return false;}
void ln_ui_init(struct ln_ui_state*s){if(!s)return;theme_sync();*s=(struct ln_ui_state){.dirty=1u,.action=UINT32_MAX,.scene_epoch=1u};
    for(uint32_t i=0;i<ln_ui_scene_count&&i<LINNET_UI_MAX_SCENES;++i){const struct ln_ui_scene*c=&ln_ui_scenes[i];if(c->focus_node<c->count){const struct ln_ui_node*n=&c->nodes[c->focus_node];uint32_t count=total(n);s->selections[i]=count?(n->index<count?n->index:count-1u):0;}}s->selected=s->selections[0];
}
bool ln_ui_input(struct ln_ui_state*s,uint32_t kind,int32_t value){if(!s)return false;theme_sync();
    if(kind==3u||kind==4u){bool v=value!=0;bool *old=kind==3u?&s->hold:&s->headphones;if(*old==v)return false;*old=v;s->dirty|=4u;return true;}
    if(ln_preferences_input(s,kind,value))return true;
    bool clock_edit=s->scene==LN_UI_SCENE_CLOCK_EDIT;
    if(!s->hold&&s->scene==LN_UI_SCENE_GAME_PLAY&&(kind==1u||kind==2u)){
        int result=ln_games_input(kind,value);if(kind==2u)s->buttons=(uint32_t)value;
        if(result==3)return false;
        if(result==2){(void)ln_ui_show(s,"linnet.system.games");return true;}
        if(result){s->dirty|=8u;if(kind==2u)ln_ui_refresh(s);return true;}
    }
    if(!s->hold&&kind==2u&&s->scene==LN_UI_SCENE_GAMES&&((uint32_t)value&~s->buttons&2u)){
        ln_games_music_toggle();s->buttons=(uint32_t)value;ln_ui_refresh(s);return true;
    }
    if(!s->hold&&kind==2u&&s->scene==LN_UI_SCENE_GAMES&&((uint32_t)value&~s->buttons&16u)&&ln_games_count()){
        ln_games_start(s->selected);s->buttons=(uint32_t)value;return ln_ui_show(s,"linnet.system.game-play");
    }
    if(clock_edit&&(kind==1u||kind==2u)){
        uint32_t pressed=kind==2u?(uint32_t)value&~s->buttons:0;
        int result=ln_ui_clock_input(s,kind,value);
        if(result==2)return false;
        if(result){s->dirty|=1u;return true;}
        if(!s->hold&&(pressed&1u)){ln_ui_clock_cancel();s->buttons=(uint32_t)value;return previous_scene(s);}
        if(kind==2u){s->buttons=(uint32_t)value;return false;}
    }
    if(kind==2u&&!s->hold&&s->scene==LN_UI_SCENE_CLOCK&&((uint32_t)value&~s->buttons&16u)){
        s->buttons=(uint32_t)value;if(s->selected==1u)ln_ui_clock_toggle();else (void)ln_ui_show(s,"linnet.system.clock-edit");s->dirty|=1u;return true;
    }
    if(kind==2u){uint32_t pressed=(uint32_t)value&~s->buttons;s->buttons=(uint32_t)value;if(s->hold)return false;if(pressed&1u){if(clock_edit)ln_ui_clock_cancel();return previous_scene(s);}if(pressed&16u){const struct ln_ui_node*n=focus(s);if(theme_choices(n)&&s->selected<ln_ui_theme_count){ln_ui_theme_changed(s->selected);theme_sync();s->dirty|=1u;return true;}uint32_t source=n?menu_source(n,s->selected):UINT32_MAX;if(n&&n->targets&&source<n->item_count){uint32_t target=n->targets[source];if(target==LINNET_UI_ACTION_MENUS){ln_preferences_open(s);return true;}if(target==LINNET_UI_ACTION_SYNC||target==LINNET_UI_ACTION_SHUFFLE||target==LINNET_UI_ACTION_REPEAT||target==LINNET_UI_ACTION_SHUFFLE_ALL){s->action=target;return false;}return change_scene(s,target);}}return false;}
    if(kind!=1u||s->hold||!value)return false;const struct ln_ui_node*n=focus(s);uint32_t count=n?total(n):0;if(!count)return false;
    (void)ln_ui_first_visible(s);int64_t next=(int64_t)s->selected+value;if(next<0)next=0;if((uint64_t)next>=count)next=count-1u;if((uint32_t)next==s->selected)return false;s->selected=(uint32_t)next;s->selections[s->scene]=s->selected;(void)ln_ui_first_visible(s);s->dirty|=2u;return true;
}
static void pixel_copy(uint16_t *to,const uint16_t *from,uint32_t pixels){
#if defined(__arm__)
    extern void ln_memory_copy(void *,const void *,uint32_t);
    ln_memory_copy(to,from,pixels*2u);
#else
    for(uint32_t i=0;i<pixels;++i)to[i]=from[i];
#endif
}
#if defined(LINNET_VIDEO)
/* Retain opaque video controls between data changes. Each band is bounded;
 * unfamiliar layouts use normal painting. Never cache the moving video pixels. */
#if LINNET_TARGET_NUMBER == 98
/* The accepted layout bounds below need only the header and footer bands. */
#define VIDEO_OVERLAY_TOP (LINNET_UI_HEIGHT*48u/176u)
#define VIDEO_OVERLAY_ROWS (VIDEO_OVERLAY_TOP+LINNET_UI_HEIGHT-LINNET_UI_HEIGHT*112u/176u+7u)
static uint16_t video_overlay_pixels[LINNET_UI_WIDTH*VIDEO_OVERLAY_ROWS]
    __attribute__((section(".video_overlay"),aligned(32)));
#define VIDEO_OVERLAY_FOOTER (VIDEO_OVERLAY_TOP*LINNET_UI_WIDTH)
#else
#define video_overlay_pixels video_overlay.pixels
#define VIDEO_OVERLAY_FOOTER (bottom*LINNET_UI_WIDTH)
#endif
static struct {
#if LINNET_TARGET_NUMBER != 98
    uint16_t pixels[LINNET_UI_WIDTH*LINNET_UI_HEIGHT];
#endif
    uint16_t colors[LN_UI_COLOR_COUNT];
    uint32_t scene,epoch,theme,battery,playback,minute,progress,top,bottom;
    bool valid,hold,headphones,clock12;
    char status[128];
} video_overlay;
static bool paint_video_overlay(struct painter*p,struct ln_ui_state*s,const struct ln_ui_scene*c){
    if(!same(c->id,"linnet.system.video-controls")||c->count!=5u||
       c->nodes[2].kind!=LN_UI_STATUS_BAR||c->nodes[3].kind!=LN_UI_PROGRESS_BAR||c->nodes[4].kind!=LN_UI_TEXT||
       p->clip.x||p->clip.y||p->clip.w!=(int32_t)LINNET_UI_WIDTH||p->clip.h!=(int32_t)LINNET_UI_HEIGHT)return false;
    uint32_t top=0,bottom=LINNET_UI_HEIGHT;
    for(uint32_t i=2;i<c->count;++i){const struct ln_ui_node*n=&c->nodes[i];
        if(n->flags&2u||n->y<0||n->h<=0||n->y+n->h>(int32_t)LINNET_UI_HEIGHT)return false;
        if((uint32_t)(n->y+n->h)<=LINNET_UI_HEIGHT*48u/176u){if((uint32_t)(n->y+n->h)>top)top=(uint32_t)(n->y+n->h);}
        else if((uint32_t)n->y>=LINNET_UI_HEIGHT*112u/176u){if((uint32_t)n->y<bottom)bottom=(uint32_t)n->y;}
        else return false;
    }
    /* Leave breathing room above the seek track without covering any text. */
    if(bottom<7u||bottom-7u<=top)return false;
    bottom-=7u;
    uint32_t battery=ln_ui_battery_status(),playback=ln_ui_playback_status(),minute=ln_ui_clock_seconds()/60u;
    const struct ln_ui_node*bar=&c->nodes[3];
    uint32_t count=ln_ui_dynamic_count(bar,bar->count?bar->count:100u),value=ln_ui_dynamic_value(bar,bar->index);
    uint32_t progress=(uint32_t)fraction(value>count?count:value,(uint32_t)bar->w,count);
    const char*status=ln_ui_dynamic_text(&c->nodes[2],UINT32_MAX,c->nodes[2].text);if(!status)status="";
    uint32_t length=0;while(status[length]&&length<127u)++length;if(status[length])return false;
    bool changed=!video_overlay.valid||video_overlay.scene!=s->scene||video_overlay.epoch!=s->scene_epoch||
        video_overlay.theme!=active_theme||video_overlay.battery!=battery||video_overlay.playback!=playback||
        video_overlay.minute!=minute||video_overlay.progress!=progress||video_overlay.top!=top||video_overlay.bottom!=bottom||
        video_overlay.hold!=s->hold||video_overlay.headphones!=s->headphones||video_overlay.clock12!=ln_ui_clock_12()||
        !same(video_overlay.status,status);
    for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i)if(video_overlay.colors[i]!=color(i))changed=true;
    /* The opaque bands replace these rows completely. Copy only the visible
     * video rows instead of writing the covered pixels twice to scanout. */
    struct rect clip=p->clip;p->clip=(struct rect){0,(int32_t)top,LINNET_UI_WIDTH,(int32_t)(bottom-top)};
    draw_node(p,&c->nodes[1],s,false);p->clip=clip;
    if(changed){
        const uint16_t header_bg=video_background(color(LN_UI_STATUS_BG)),footer_bg=video_background(color(LN_UI_PANEL));
        const uint16_t ink=video_ink(color(LN_UI_STATUS_FG)),accent=video_ink(color(LN_UI_ACCENT));
        const uint16_t header_hint=mix(ink,header_bg,210u);
        const uint16_t footer_hint=mix(video_ink(color(LN_UI_FOREGROUND)),footer_bg,210u);
        const uint16_t track=mix(accent,footer_bg,56u);
        uint32_t style=layout();
        fill(p,(struct rect){0,0,LINNET_UI_WIDTH,(int32_t)top},header_bg);
        fill(p,(struct rect){0,(int32_t)bottom,LINNET_UI_WIDTH,LINNET_UI_HEIGHT-(int32_t)bottom},footer_bg);
        if(style!=LN_UI_LAYOUT_MINIMAL){
            uint16_t edge=style==LN_UI_LAYOUT_BLUEPRINT?accent:color(LN_UI_BORDER);
            fill(p,(struct rect){0,(int32_t)top-1,LINNET_UI_WIDTH,1},edge);
            fill(p,(struct rect){0,(int32_t)bottom,LINNET_UI_WIDTH,1},edge);
        }
        if(style==LN_UI_LAYOUT_AURORA){
            fill(p,(struct rect){0,0,2,(int32_t)top},accent);
            fill(p,(struct rect){0,(int32_t)bottom,2,LINNET_UI_HEIGHT-(int32_t)bottom},accent);
        }else if(style==LN_UI_LAYOUT_IPOD_PHOTO||style==LN_UI_LAYOUT_IPOD_CLASSIC){
            fill(p,(struct rect){0,0,LINNET_UI_WIDTH,1},mix(0xffffu,header_bg,64u));
        }
        struct ln_ui_font main_font=ln_ui_font;
        const char*hide=LINNET_UI_WIDTH<=220u?"SELECT":"SELECT Hide";
        ln_ui_font=ln_ui_hint_font;
        int32_t title_width=text_width("VIDEO"),hide_width=text_width(hide);
        int32_t side_y=max(0,((int32_t)top-(int32_t)ln_ui_font.height)/2);
        text(p,bar->x,side_y,"VIDEO",header_hint);
        text(p,LINNET_UI_WIDTH-bar->x-hide_width,side_y,hide,header_hint);
        ln_ui_font=main_font;
        int32_t side=max(title_width,hide_width)+2*bar->x,slot=LINNET_UI_WIDTH-2*side;
        if(text_width(status)>slot)ln_ui_font=ln_ui_hint_font;
        int32_t status_width=text_width(status);
        fitted_text(p,side+max(0,(slot-status_width)/2),max(0,((int32_t)top-(int32_t)ln_ui_font.height)/2),status,ink,slot);
        ln_ui_font=main_font;
        fill(p,bounds(bar),track);
        if(progress)fill(p,(struct rect){bar->x,bar->y,(int32_t)progress,bar->h},accent);
        const struct ln_ui_node*hint=&c->nodes[4];
        if(LINNET_UI_WIDTH<=220u)ln_ui_font=ln_ui_hint_font;
        static const char*const labels[]={"MENU Exit","PLAY Pause","< > Seek"};
        int32_t column=hint->w/3,y=hint->y+max(0,(hint->h-(int32_t)ln_ui_font.height)/2);
        for(uint32_t i=0;i<3u;++i){int32_t x=hint->x+(int32_t)i*column;
            fitted_text(p,x+max(0,(column-text_width(labels[i]))/2),y,labels[i],footer_hint,column);
        }
        ln_ui_font=main_font;
        pixel_copy(video_overlay_pixels,p->pixels,top*LINNET_UI_WIDTH);
        pixel_copy(video_overlay_pixels+VIDEO_OVERLAY_FOOTER,p->pixels+bottom*LINNET_UI_WIDTH,(LINNET_UI_HEIGHT-bottom)*LINNET_UI_WIDTH);
        video_overlay.scene=s->scene;video_overlay.epoch=s->scene_epoch;video_overlay.theme=active_theme;
        video_overlay.battery=battery;video_overlay.playback=playback;video_overlay.minute=minute;video_overlay.progress=progress;
        video_overlay.top=top;video_overlay.bottom=bottom;video_overlay.hold=s->hold;video_overlay.headphones=s->headphones;
        video_overlay.clock12=ln_ui_clock_12();for(uint32_t i=0;i<=length;++i)video_overlay.status[i]=status[i];
        for(uint32_t i=0;i<LN_UI_COLOR_COUNT;++i)video_overlay.colors[i]=color(i);
        video_overlay.valid=true;
    }else{
        pixel_copy(p->pixels,video_overlay_pixels,top*LINNET_UI_WIDTH);
        pixel_copy(p->pixels+bottom*LINNET_UI_WIDTH,video_overlay_pixels+VIDEO_OVERLAY_FOOTER,(LINNET_UI_HEIGHT-bottom)*LINNET_UI_WIDTH);
    }
    return true;
}
#endif
static void paint(struct ln_ui_state*s,uint16_t*pixels,struct rect r,ln_ui_damage_fn damage){
    r=intersect(r,screen);if(!r.w||!r.h)return;struct painter p={pixels,r};const struct ln_ui_scene*c=scene(s);
    bool covered=false;uint32_t first=0;
    if(c&&c->nodes&&c->count&&c->count<=1024u){const struct ln_ui_node*n=&c->nodes[0];
        if(n->kind==LN_UI_SCREEN&&!(n->flags&2u)){struct rect cover=intersect(bounds(n),r);covered=cover.x==r.x&&cover.y==r.y&&cover.w==r.w&&cover.h==r.h;}
        if(n->kind==LN_UI_SCREEN&&c->count>1u){const struct ln_ui_node*video=&c->nodes[1];
            if(!(n->flags&2u)&&video->kind==LN_UI_CANVAS_SURFACE&&video->canvas&&
               (video->flags&LN_UI_CANVAS_OPAQUE)&&!(video->flags&2u)){
                struct rect area=intersect(bounds(n),r),cover=intersect(bounds(video),area);
                if(covered&&cover.w&&cover.h){
                    uint16_t bg=ln_ui_color(n->bg);
                    fill(&p,(struct rect){area.x,area.y,area.w,cover.y-area.y},bg);
                    fill(&p,(struct rect){area.x,cover.y+cover.h,area.w,area.y+area.h-cover.y-cover.h},bg);
                    fill(&p,(struct rect){area.x,cover.y,cover.x-area.x,cover.h},bg);
                    fill(&p,(struct rect){cover.x+cover.w,cover.y,area.x+area.w-cover.x-cover.w,cover.h},bg);
                    first=1u;
                }
            }
            if(video->kind==LN_UI_VIDEO_SURFACE&&!(video->flags&2u)){
                const struct ln_ui_asset*a=ln_ui_dynamic_asset(video,video->resource<ln_ui_asset_count?&ln_ui_assets[video->resource]:0);
                struct rect cover=intersect(bounds(video),r);
                /* This opaque video will overwrite the screen and chrome in
                 * the entire damaged clip. Overlays still draw afterwards. */
                if(cover.x==r.x&&cover.y==r.y&&cover.w==r.w&&cover.h==r.h&&image_independent(&p,video,a)){covered=true;first=1u;}
            }
        }
    }
    if(!covered)fill(&p,r,color(LN_UI_BACKGROUND));
    if(c)for(uint32_t i=first;i<c->count&&i<1024u;++i){
#if defined(LINNET_VIDEO)
        if(i==1u&&first==1u&&paint_video_overlay(&p,s,c))break;
#endif
        draw_node(&p,&c->nodes[i],s,i==c->focus_node);
    }
    if(layout()==LN_UI_LAYOUT_MINIMAL){
        uint32_t status=ln_ui_battery_status();
        uint16_t bg=color(LN_UI_BACKGROUND);
        if(s->scene==LN_UI_SCENE_VIDEO_PLAYER||s->scene==LN_UI_SCENE_VIDEO_CONTROLS)bg=video_background(bg);
        fill(&p,(struct rect){0,0,LINNET_UI_WIDTH,2},bg);
        if(status!=UINT32_MAX&&(status&255u)<=100u)
            fill(&p,(struct rect){0,0,(int32_t)((status&255u)*LINNET_UI_WIDTH/100u),2},color(LN_UI_BATTERY));
    }
    s->painted_pixels+=(uint32_t)(r.w*r.h);if(damage)damage(r.x,r.y,r.w,r.h);
}
/* Count the same clipped cells as a full paint without rasterizing their text
 * or media when only the status chrome needs an update. */
static uint32_t visible_cells(const struct ln_ui_state*s,const struct ln_ui_scene*c){
    uint32_t visible=0;
    for(uint32_t j=0;c&&j<c->count&&j<1024u;++j){const struct ln_ui_node*n=&c->nodes[j];
        if((n->flags&2u)||n->kind<LN_UI_VIRTUAL_LIST||n->kind>LN_UI_GRID)continue;
        struct rect clip=intersect(bounds(n),screen);if(!clip.w||!clip.h)continue;
        uint32_t cols=columns(n),capacity=rows(n),count=total(n);
        uint32_t selected=j==c->focus_node?s->selected:n->index;
        uint32_t first=list_first(s,n,selected,j==c->focus_node);
        int32_t w=(int32_t)(baked_geometry(n)?(n->flags>>LN_UI_GEOMETRY_WIDTH_SHIFT)&255u:divide((uint32_t)n->w,cols)),h=(int32_t)row_height(n);
        if((layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout())&&n->kind!=LN_UI_GRID)w=n->w-(count>capacity&&n->w==220?10:0);
        uint32_t column=0,row=0;
        for(uint32_t i=first;i<count&&i-first<capacity*cols;++i){
            struct rect cell=intersect((struct rect){n->x+(int32_t)column*w,n->y+(int32_t)row*h,w,h},clip);
            if(cell.w&&cell.h)++visible;if(++column==cols){column=0;++row;}
        }
    }return visible;
}
/* Photo lists are opaque. Within one page, repaint only their changed rows;
 * the scrollbar and chrome have not changed. Preserve overlapping content. */
static void paint_selection(struct ln_ui_state*s,uint16_t*pixels,const struct ln_ui_node*n,struct rect r,ln_ui_damage_fn damage){
    if((layout()==LN_UI_LAYOUT_IPOD_PHOTO||classic_layout())&&n->kind!=LN_UI_GRID){
        const struct ln_ui_scene*c=scene(s);bool opaque=true;
        for(uint32_t i=0;i<c->count;++i){const struct ln_ui_node*other=&c->nodes[i];
            if(other==n||(other->flags&2u)||other->kind==LN_UI_SCREEN||other->kind==LN_UI_SCROLLBAR)continue;
            struct rect overlap=intersect(r,bounds(other));if(overlap.w&&overlap.h){opaque=false;break;}
        }
        if(opaque){
            struct painter p={pixels,r};draw_node(&p,n,s,true);
            s->painted_pixels+=(uint32_t)(r.w*r.h);if(damage)damage(r.x,r.y,r.w,r.h);return;
        }
    }
    paint(s,pixels,r,damage);
}
static bool selection_damage(struct ln_ui_state*s,uint16_t*pixels,const struct ln_ui_node*n,ln_ui_damage_fn damage){
    if(n->kind!=LN_UI_MENU&&n->kind!=LN_UI_VIRTUAL_LIST&&n->kind!=LN_UI_GRID)return false;
    uint32_t cols=columns(n);
    uint32_t old_first=s->drawn_first;
    uint32_t new_first=list_first(s,n,s->selected,true);
    if(old_first!=new_first)return false;
    uint32_t indexes[2]={s->drawn_selected,s->selected};
    int32_t width=(int32_t)divide((uint32_t)n->w,cols),height=(int32_t)row_height(n);
    struct rect cells[2];
    for(uint32_t i=0;i<2u;++i){uint32_t cell=indexes[i]-new_first,row=divide(cell,cols),column=cell-row*cols;
        cells[i]=(struct rect){n->x+(int32_t)column*width,n->y+(int32_t)row*height,width,height};
    }
    /* A normal wheel tick moves to an edge-adjacent cell. Paint its exact
     * two-cell union once, avoiding a second scene traversal and damage span. */
    bool horizontal=cells[0].y==cells[1].y&&(cells[0].x+cells[0].w==cells[1].x||cells[1].x+cells[1].w==cells[0].x);
    bool vertical=cells[0].x==cells[1].x&&(cells[0].y+cells[0].h==cells[1].y||cells[1].y+cells[1].h==cells[0].y);
    if(horizontal||vertical){
        int32_t x=min(cells[0].x,cells[1].x),y=min(cells[0].y,cells[1].y);
        int32_t right=max(cells[0].x+cells[0].w,cells[1].x+cells[1].w);
        int32_t bottom=max(cells[0].y+cells[0].h,cells[1].y+cells[1].h);
        paint_selection(s,pixels,n,intersect((struct rect){x,y,right-x,bottom-y},bounds(n)),damage);
    }else for(uint32_t i=0;i<2u;++i){
        paint_selection(s,pixels,n,intersect(cells[i],bounds(n)),damage);
    }
    return true;
}
/* A single painter owns the framebuffer. Bind snapshots to state and scene
 * epoch; changing owner/layout safely falls back to a complete repaint. Fixed
 * limits cover the search scene without enlarging the arena/stack shell state. */
static bool text_same(const char *a,const char *b){
    if(!a||!b)return a==b;
    for(uint32_t i=0;i<256u;++i){if(a[i]!=b[i])return false;if(!a[i])return true;}
    return false;
}
static bool text_save(char out[256],const char *in){
    if(!in)in="";
    for(uint32_t i=0;i<256u;++i){out[i]=in[i];if(!in[i])return true;}
    out[255]=0;return false;
}
static const char *content_text(const struct ln_ui_node *n,uint32_t item){
    uint32_t source=item==UINT32_MAX?item:menu_source(n,item);
    const char *fallback=item==UINT32_MAX?n->text:n->items&&source<n->item_count?n->items[source]:0;
    return ln_preferences_text(n,item,ln_display_settings_text(n,item,ln_ui_dynamic_text(n,item,fallback)));
}
static bool content_layout(const struct ln_ui_state*s){
    const struct ln_ui_scene*c=scene(s);const struct ln_ui_node*n=focus(s);
    if(!c||c->count>CONTENT_NODES||!n||n->kind!=LN_UI_VIRTUAL_LIST||rows(n)>CONTENT_ROWS)return false;
    for(uint32_t i=0;i<c->count;++i){uint32_t kind=c->nodes[i].kind;
        if(kind==LN_UI_VIRTUAL_LIST){if(i!=c->focus_node)return false;}
        else if(kind!=LN_UI_SCREEN&&kind!=LN_UI_VIEW&&kind!=LN_UI_TEXT&&kind!=LN_UI_STATUS_BAR&&kind!=LN_UI_SCROLLBAR)return false;
    }return true;
}
static void content_save(const struct ln_ui_state*s){
    content.valid=false;if(!content_layout(s))return;
    const struct ln_ui_scene*c=scene(s);const struct ln_ui_node*n=focus(s);
    uint32_t capacity=rows(n),first=list_first(s,n,s->selected,true),count=total(n);
    for(uint32_t i=0;i<capacity;++i){
        content.present[i]=first+i<count;
        const char *label=content.present[i]?content_text(n,first+i):"";
        if((content.present[i]&&!label)||!text_save(content.labels[i],label))return;
    }
    for(uint32_t i=0;i<c->count;++i)
        if(!text_save(content.texts[i],content_text(&c->nodes[i],UINT32_MAX)))return;
    content.owner=s;content.scene=s->scene;content.epoch=s->scene_epoch;
    content.first=first;content.count=count;content.selected=s->selected;content.valid=true;
}
/* Reuse complete rows only when their labels still match the painted frame. */
static bool scroll_selection(struct ln_ui_state*s,uint16_t*pixels,const struct ln_ui_node*n,ln_ui_damage_fn damage){
    if(s->dirty!=2u||!content.valid||content.owner!=s||content.scene!=s->scene||
       content.epoch!=s->scene_epoch||!content_layout(s)||!edge_list(n)||(n->flags&4u))return false;
    uint32_t capacity=rows(n),count=total(n),first=list_first(s,n,s->selected,true),old=content.first;
    if(count!=content.count||first==old||(first>old?first-old:old-first)!=1u||
       first+capacity>count||old+capacity>count||capacity<2u)return false;
    struct rect area={n->x,n->y,n->w,(int32_t)(capacity*row_height(n))};
    if(area.x<0||area.y<0||area.x+area.w>(int32_t)LINNET_UI_WIDTH||area.y+area.h>(int32_t)LINNET_UI_HEIGHT||area.h>n->h)return false;
    const struct ln_ui_scene*c=scene(s);
    for(uint32_t i=0;i<c->count;i++){
        const struct ln_ui_node*other=&c->nodes[i];
        if(other==n||(other->flags&2u)||other->kind==LN_UI_SCREEN||other->kind==LN_UI_SCROLLBAR)continue;
        struct rect overlap=intersect(area,bounds(other));if(overlap.w&&overlap.h)return false;
    }
    for(uint32_t i=0;i<capacity;i++){
        uint32_t item=first+i;
        if(item>=old&&item-old<capacity&&!text_same(content_text(n,item),content.labels[item-old]))return false;
    }
    int32_t height=(int32_t)row_height(n),copy_height=area.h-height;
    for(int32_t line=0;line<copy_height;line++){
        int32_t y=first>old?area.y+line:area.y+area.h-1-line;
        int32_t source=first>old?y+height:y-height;
        pixel_copy(pixels+(uint32_t)y*LINNET_UI_WIDTH+area.x,
                     pixels+(uint32_t)source*LINNET_UI_WIDTH+area.x,(uint32_t)area.w);
    }
    s->painted_pixels+=(uint32_t)(area.w*copy_height);
    if(damage)damage(area.x,area.y,area.w,area.h);
    for(uint32_t i=0;i<capacity;i++){
        uint32_t item=first+i;
        if(item<old||item-old>=capacity||item==s->selected||item==s->drawn_selected)
            paint_selection(s,pixels,n,(struct rect){area.x,area.y+(int32_t)i*height,area.w,height},damage);
    }
    return true;
}
static bool content_damage(struct ln_ui_state*s,uint16_t*pixels,ln_ui_damage_fn damage){
    if(!content.valid||content.owner!=s||content.scene!=s->scene||content.epoch!=s->scene_epoch||!content_layout(s))return false;
    const struct ln_ui_scene*c=scene(s);const struct ln_ui_node*n=focus(s);
    uint32_t capacity=rows(n),first=list_first(s,n,s->selected,true),count=total(n);
    if(first!=content.first)paint(s,pixels,bounds(n),damage);
    else for(uint32_t i=0;i<capacity;++i){
        bool present=first+i<count;const char *label=present?content_text(n,first+i):"";
        bool chosen=present&&first+i==s->selected,was_chosen=content.present[i]&&first+i==content.selected;
        if(present!=content.present[i]||chosen!=was_chosen||!text_same(label,content.labels[i])){
            struct rect r={n->x,n->y+(int32_t)i*((int32_t)row_height(n)),n->w,(int32_t)row_height(n)};
            paint(s,pixels,intersect(r,bounds(n)),damage);
        }
    }
    for(uint32_t i=0;i<c->count;++i){const struct ln_ui_node*node=&c->nodes[i];
        if((node->kind==LN_UI_SCROLLBAR&&(count!=content.count||s->selected!=content.selected))||
           ((node->kind==LN_UI_TEXT||node->kind==LN_UI_STATUS_BAR)&&
            !text_same(content_text(node,UINT32_MAX),content.texts[i])))paint(s,pixels,bounds(node),damage);
    }
    return true;
}
/* Original-style horizontal text scrolling. Only memory-backed visible labels
 * participate; sleeping/backlight-off callers suspend the deadline. */
uint32_t ln_ui_scroll_tick(struct ln_ui_state*s,uint32_t now){
    if(!s)return UINT32_MAX;theme_sync();
    if(layout()!=LN_UI_LAYOUT_IPOD_PHOTO&&!classic_layout()&&s->scene!=LN_UI_SCENE_ABOUT){scroll_owner=0;scroll_pixels=0;return UINT32_MAX;}
    const struct ln_ui_scene*c=scene(s);if(!c)return UINT32_MAX;
    uint32_t key=(s->scene_epoch*16777619u)^s->scene^(s->selected*257u)^active_theme;
    int32_t overflow=0;
    for(uint32_t i=0;i<c->count;++i){const struct ln_ui_node*n=&c->nodes[i];
        if(n->flags&2u)continue;uint32_t item=UINT32_MAX;int32_t width=n->w;
        if(i==c->focus_node&&(n->kind==LN_UI_MENU||n->kind==LN_UI_VIRTUAL_LIST)){
            if(s->selected>=total(n)||photo_setting(n))continue;item=s->selected;
            uint32_t source=menu_source(n,item);bool arrow=(n->targets&&source<n->item_count&&n->targets[source]<ln_ui_scene_count)||ln_ui_dynamic_navigation(n,item);
            if((n->w==220||n->w==320)&&total(n)>rows(n))width-=10;
            width-=(int32_t)ln_ui_themes[active_theme].padding+6+(theme_choices(n)||arrow?12:0);
        }else if(!scrolling_field(n))continue;
        else {uint32_t revision;const struct ln_ui_asset*art=ln_ui_now_playing_art(&revision);if(art&&art->pixels)width=122;}
        const char*label=theme_choices(n)&&item<ln_ui_theme_count?ln_ui_themes[item].name:
            ln_ui_clock_text(n,item,content_text(n,item));
        if(!label)continue;
        for(uint32_t j=0;label[j]&&j<256u;++j)key=(key^(uint8_t)label[j])*16777619u;
        overflow=max(overflow,text_width(label)-width);
    }
    if(scroll_owner!=s||scroll_key!=key){scroll_owner=s;scroll_key=key;scroll_started=now;scroll_pixels=0;}
    if(overflow<=0){if(scroll_pixels){scroll_pixels=0;s->dirty|=32u;}return UINT32_MAX;}
    uint32_t phase=((uint32_t)(now-scroll_started)/50000u)%((uint32_t)overflow+50u);
    int32_t offset=min(overflow,max(0,(int32_t)phase-20));
    if(offset!=scroll_pixels){scroll_pixels=offset;s->dirty|=32u;}
    uint32_t ticks=phase<=20u?21u-phase:phase>=(uint32_t)overflow+20u?(uint32_t)overflow+50u-phase:1u;
    return now+ticks*50000u-(uint32_t)(now-scroll_started)%50000u;
}
static void paint_scrolling(struct ln_ui_state*s,uint16_t*pixels,const struct ln_ui_scene*c,ln_ui_damage_fn damage){
    for(uint32_t i=0;c&&i<c->count;++i){const struct ln_ui_node*n=&c->nodes[i];
        if(scrolling_field(n))paint(s,pixels,bounds(n),damage);
        else if(i==c->focus_node&&(n->kind==LN_UI_MENU||n->kind==LN_UI_VIRTUAL_LIST)){
            uint32_t row=s->selected-list_first(s,n,s->selected,true);
            paint(s,pixels,(struct rect){n->x,n->y+(int32_t)(row*row_height(n)),n->w,(int32_t)row_height(n)},damage);
        }
    }
}
void ln_ui_render(struct ln_ui_state*s,uint16_t*pixels,ln_ui_damage_fn damage){if(!s||!pixels)return;theme_sync();if(s->drawn_theme!=active_theme){s->dirty|=1u;++s->scene_epoch;}
    (void)ln_ui_first_visible(s);(void)ln_ui_status_refresh(s);if(!s->dirty)return;
    if(s->rendered&&!(s->dirty&1u)){if(s->selected==s->drawn_selected)s->dirty&=~2u;if(((s->hold?1u:0u)|(s->headphones?2u:0u))==s->drawn_flags&&status_signature()==s->drawn_status)s->dirty&=~4u;if(!s->dirty)return;}
    uint32_t painted_before=s->painted_pixels,render_dirty=s->dirty,visible_before=s->visible_items;bool cached_selection=false;const struct ln_ui_scene*c=scene(s);s->visible_items=0;
    if(!s->rendered||(s->dirty&1u))paint(s,pixels,screen,damage);
    else if(s->dirty==4u&&s->scene!=LN_UI_SCENE_CLOCK&&
            (layout()==LN_UI_LAYOUT_AURORA||layout()==LN_UI_LAYOUT_GRAPHITE||layout()==LN_UI_LAYOUT_SUNSET)){
        paint(s,pixels,layout()==LN_UI_LAYOUT_AURORA?(struct rect){0,0,UI_X(26),LINNET_UI_HEIGHT}:(struct rect){0,UI_Y(154),LINNET_UI_WIDTH,LINNET_UI_HEIGHT-UI_Y(154)},damage);
        for(uint32_t i=0;c&&i<c->count&&i<1024u;++i){const struct ln_ui_node*n=&c->nodes[i];
            if(n->kind==LN_UI_STATUS_BAR||(n->deps&16u))paint(s,pixels,bounds(n),damage);
        }
        s->visible_items=visible_cells(s,c);
    }
    else if((s->dirty&4u)&&(layout()==LN_UI_LAYOUT_AURORA||layout()==LN_UI_LAYOUT_GRAPHITE||layout()==LN_UI_LAYOUT_SUNSET||s->scene==LN_UI_SCENE_CLOCK))paint(s,pixels,screen,damage);
    else {
        bool content_handled=false;
        if(s->dirty&16u){content_handled=content_damage(s,pixels,damage);if(!content_handled)paint(s,pixels,screen,damage);}
        if(c)for(uint32_t i=0;i<c->count&&i<1024u;++i){const struct ln_ui_node*n=&c->nodes[i];
            bool scrollbar_changed=n->kind!=LN_UI_SCROLLBAR;
            if(!scrollbar_changed&&c->focus_node<c->count){const struct ln_ui_node*f=&c->nodes[c->focus_node];
                if(f->kind==LN_UI_MENU||f->kind==LN_UI_VIRTUAL_LIST||f->kind==LN_UI_GRID)
                    scrollbar_changed=s->drawn_first!=list_first(s,f,s->selected,true);
            }
            bool selected=(s->dirty&2u)&&(i==c->focus_node||(n->kind==LN_UI_SCROLLBAR?scrollbar_changed:(n->deps&3u)));bool status=(s->dirty&4u)&&(n->kind==LN_UI_STATUS_BAR||(n->deps&16u));bool animation=(s->dirty&8u)&&(n->deps&4u);
            /* Content damage already composites the updated list and scrollbar. */
            if(content_handled&&(i==c->focus_node||n->kind==LN_UI_SCROLLBAR))selected=false;
            if(selected&&!status&&!animation&&i==c->focus_node){if(scroll_selection(s,pixels,n,damage))continue;if(selection_damage(s,pixels,n,damage)){cached_selection=render_dirty==2u;continue;}}if(selected||status||animation)paint(s,pixels,bounds(n),damage);}
    }
    if(classic_layout()&&s->scene==LN_UI_SCENE_HOME&&!(s->dirty&1u)&&
       (s->selected!=s->drawn_selected||(s->dirty&64u)))paint(s,pixels,classic_pane(c),damage);
    if(layout()==LN_UI_LAYOUT_MINIMAL&&(s->dirty&4u)&&!(s->dirty&1u))paint(s,pixels,(struct rect){0,0,LINNET_UI_WIDTH,2},damage);
    if((s->dirty&32u)&&!(s->dirty&1u))paint_scrolling(s,pixels,c,damage);
    /* Report the retained viewport, not only the rows touched by an
     * incremental selection or content repaint. */
    if(c&&c->focus_node<c->count){const struct ln_ui_node*n=&c->nodes[c->focus_node];
        if(n->kind==LN_UI_MENU||n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_GRID)
            s->visible_items=cached_selection?visible_before:visible_cells(s,c);
    }
    /* Selection damage cannot alter provider text or the visible window. Keep
     * the retained labels and advance only its selection marker. */
    if(cached_selection&&content.valid&&content.owner==s&&content.scene==s->scene&&content.epoch==s->scene_epoch)content.selected=s->selected;
    else content_save(s);
    s->drawn_first=ln_ui_first_visible(s);s->drawn_status=status_signature();s->drawn_theme=active_theme;s->drawn_scene=s->scene;s->drawn_selected=s->selected;s->drawn_flags=(s->hold?1u:0u)|(s->headphones?2u:0u);s->dirty=0;s->rendered=true;if(s->painted_pixels!=painted_before)++s->paint_count;
}
void ln_ui_animate(struct ln_ui_state*s,uint16_t*pixels,uint32_t frame,ln_ui_damage_fn damage){if(!s)return;if(s->animation!=frame){s->animation=frame;const struct ln_ui_scene*c=scene(s);if(c)for(uint32_t i=0;i<c->count&&i<1024u;++i)if(c->nodes[i].deps&4u){s->dirty|=8u;break;}}ln_ui_render(s,pixels,damage);}
uint32_t ln_ui_take_action(struct ln_ui_state*s){if(!s)return UINT32_MAX;uint32_t action=s->action;s->action=UINT32_MAX;return action;}
