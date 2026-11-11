#ifndef LINNET_MUSIC_MENU_H
#define LINNET_MUSIC_MENU_H
#include "ui/runtime/ui.h"
#include "linnet_scene_ids.h"

/* Presentation IDs do not change MENUS.BIN or the underlying service actions. */
enum { LN_MUSIC_COVER_FLOW=16, LN_MUSIC_MORE=17 };
static inline bool ln_music_menu_more(uint32_t id){
    return id==5u||id==6u||id==10u||id==11u||id==12u||id==13u||id==14u;
}
static inline bool ln_music_menu_scene_enabled(uint32_t scene){
    return scene<LINNET_UI_MAX_SCENES&&(ln_ui_enabled_scenes&(UINT64_C(1)<<scene))!=0;
}
static inline uint32_t ln_music_menu_category(bool more,uint32_t row){
    bool covers=!more&&ln_music_menu_scene_enabled(LN_UI_SCENE_COVER_FLOW),has_more=false;
    for(uint32_t i=0,n=ln_ui_menu_count(LN_MENU_MUSIC);i<n;++i){
        uint32_t id=ln_ui_menu_item(LN_MENU_MUSIC,i);bool secondary=ln_music_menu_more(id);
        if(secondary)has_more=true;
        if(secondary!=more)continue;
        /* Cover Flow follows the conventional browsing categories, before Search. */
        if(id==8u&&covers){if(!row)return LN_MUSIC_COVER_FLOW;--row;covers=false;}
        if(!row)return id;--row;
    }
    if(covers){if(!row)return LN_MUSIC_COVER_FLOW;--row;}
    return !more&&has_more&&!row?LN_MUSIC_MORE:UINT32_MAX;
}
static inline uint32_t ln_music_menu_count(bool more){
    uint32_t count=0;bool has_more=false;
    for(uint32_t i=0,n=ln_ui_menu_count(LN_MENU_MUSIC);i<n;++i){
        bool secondary=ln_music_menu_more(ln_ui_menu_item(LN_MENU_MUSIC,i));
        if(secondary)has_more=true;
        if(secondary==more)++count;
    }
    if(!more){
        if(ln_music_menu_scene_enabled(LN_UI_SCENE_COVER_FLOW))++count;
        if(has_more)++count;
    }
    return count;
}
enum { LN_SOUND_ROOT, LN_SOUND_EQ, LN_SOUND_ADVANCED };
/* Playback row ordering maps to stable service actions in each mode. */
static inline uint32_t ln_music_menu_option_action(bool file_mode,uint32_t row){
    static const uint8_t file_actions[]={8,12,5,9,10,6};
    static const uint8_t actions[]={0,1,8,12,7,11,2,3,4,5,9,10,6};
    return file_mode?(row<sizeof(file_actions)?file_actions[row]:UINT32_MAX):
        (row<sizeof(actions)?actions[row]:UINT32_MAX);
}
static inline uint32_t ln_music_menu_option_count(bool file_mode){return file_mode?6u:13u;}
enum { LN_SOUND_CLICKER=13, LN_SOUND_CLICK_VOLUME, LN_SOUND_CLICK_ENABLE, LN_SOUND_CLICK_OUTPUT, LN_SOUND_CUSTOM_EQ, LN_SOUND_MORE };
static inline bool ln_sound_menu_click(uint32_t action){
    return action>=LN_SOUND_CLICKER&&action<=LN_SOUND_CLICK_OUTPUT;
}
static inline uint32_t ln_sound_menu_action(uint32_t view,uint32_t row,bool extended,bool clicker){
    static const uint8_t root[]={1,6,0,LN_SOUND_CUSTOM_EQ,LN_SOUND_CLICK_ENABLE,LN_SOUND_CLICK_OUTPUT,LN_SOUND_CLICKER,LN_SOUND_CLICK_VOLUME,LN_SOUND_MORE};
    static const uint8_t advanced[]={3,4,2,5,7};
    if(view==LN_SOUND_ROOT){
        if(row>=sizeof(root)-(clicker?0u:4u))return UINT32_MAX;
        if(!clicker&&row>=4u)row+=4u;
        return root[row];
    }
    if(view==LN_SOUND_ADVANCED)return row<(extended?5u:4u)?advanced[row]:UINT32_MAX;
    if(view==LN_SOUND_EQ)return row<(extended?5u:2u)?row+(extended?8u:7u):UINT32_MAX;
    return UINT32_MAX;
}
static inline uint32_t ln_sound_menu_count(uint32_t view,bool extended,bool clicker){
    return view==LN_SOUND_ROOT?(clicker?9u:5u):view==LN_SOUND_EQ?(extended?5u:2u):(extended?5u:4u);
}
#endif
