#include "ui/runtime/ui.h"
#include "linnet_target.h"
#include "services/music/library.h"
#include "services/music/controller.h"
#include "services/music/player.h"
#include "services/music/artwork.h"
#include "services/music/coverflow.h"
#include "services/music/browser.h"
#include "services/music/menu.h"
#include "services/music/search.h"
#include "services/music/queue_view.h"
#include "services/display/settings.h"
#if defined(LINNET_FILES)
#include "services/files/files.h"
#endif
#if defined(LINNET_NOTES)
#include "services/notes/notes.h"
#endif
#if defined(LINNET_PHOTOS)
#include "services/photos/photos.h"
#endif
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#if defined(LINNET_TARGET_VIDEO5G_M25)
#include "kernel/runtime.h"
#endif
#endif
static bool equal(const char*a,const char*b){if(!a||!b)return false;while(*a&&*a==*b){++a;++b;}return *a==*b;}
static char *number_text(char *out,uint32_t value){
    char digits[10];uint32_t count=0;
    do{digits[count++]=(char)('0'+value%10u);value/=10u;}while(value);
    while(count)*out++=digits[--count];return out;
}
/* Text providers only inspect retained player/library memory. */
static uint64_t clock_divide(uint64_t numerator,uint32_t denominator,uint32_t *rest){
    uint64_t quotient=0,remainder=0;
    /* ARM7 freestanding images do not provide __aeabi_uldivmod.
     * Leading zero bits cannot contribute to the quotient or remainder. */
    uint32_t high=(uint32_t)(numerator>>32);
    uint32_t bits=high?64u-(uint32_t)__builtin_clz(high):
        numerator?32u-(uint32_t)__builtin_clz((uint32_t)numerator):0u;
    for(uint32_t bit=bits;bit;--bit){
        remainder=(remainder<<1)|((numerator>>(bit-1u))&1u);
        if(remainder>=denominator){remainder-=denominator;quotient|=(uint64_t)1u<<(bit-1u);}
    }
    if(rest)*rest=(uint32_t)remainder;return quotient;
}
static const char *clock_text(char *out,uint64_t frames,uint32_t rate,bool remaining){
    if(!rate)return "--:--";
    uint32_t seconds,digit;uint64_t minutes=clock_divide(clock_divide(frames,rate,0),60u,&seconds);
    char digits[24];uint32_t count=0,used=0;
    if(remaining)out[used++]='-';
    do{minutes=clock_divide(minutes,10u,&digit);digits[count++]=(char)('0'+digit);}while(minutes);
    while(count)out[used++]=digits[--count];
    out[used++]=':';out[used++]=(char)('0'+seconds/10u);out[used++]=(char)('0'+seconds%10u);out[used]=0;
    return out;
}
uint32_t ln_ui_playback_modes(void){return ln_music_playback_modes();}
uint32_t ln_ui_playback_status(void){
#if defined(LINNET_VIDEO)
    if(ln_video_active())return ln_video_paused()?2u:1u;
#endif
    struct ln_player_status p;ln_player_snapshot(&p);
    return p.phase==LN_PLAYER_PAUSE?2u:(p.phase==LN_PLAYER_PLAY||p.phase==LN_PLAYER_DRAIN)?1u:0u;
}
bool ln_ui_now_playing_art_loading(void){return ln_music_art_loading();}
const struct ln_ui_asset *ln_ui_now_playing_art(uint32_t *revision){
    static struct ln_ui_asset art={64,64,0};
    *revision=ln_music_art_revision();art.pixels=ln_music_art_pixels();
    art.width=art.height=(uint16_t)ln_music_art_side();
    return art.pixels?&art:0;
}
const struct ln_ui_asset *ln_ui_now_playing_art_sized(uint32_t *revision,uint32_t side){
    static struct ln_ui_asset art;
    uint32_t actual;
    *revision=ln_music_art_revision();art.pixels=ln_music_art_pixels_sized(side,&actual);
    art.width=art.height=(uint16_t)actual;
    return art.pixels?&art:0;
}
static bool in_scene(const struct ln_ui_node*n,uint32_t id){
    static const struct ln_ui_node *cached_node;
    static const struct ln_ui_scene *cached_scenes;
    static uint32_t cached_scene;
    if(n==cached_node&&ln_ui_scenes==cached_scenes)
        return id<ln_ui_scene_count&&cached_scene==id;
    uintptr_t address=(uintptr_t)n;
    for(uint32_t i=0;i<ln_ui_scene_count;++i){const struct ln_ui_scene*s=&ln_ui_scenes[i];uintptr_t first=(uintptr_t)s->nodes;
        if(address>=first&&address-first<(uintptr_t)s->count*sizeof(*n)){
            cached_node=n;cached_scenes=ln_ui_scenes;cached_scene=i;
            return i==id;
        }}
    return false;
}
const struct ln_ui_asset *ln_ui_dynamic_asset(const struct ln_ui_node*n,const struct ln_ui_asset*fallback){
#if defined(LINNET_PHOTOS)
    if(n->kind==LN_UI_PHOTO_SURFACE&&in_scene(n,LN_UI_SCENE_PHOTOS_VIEWER))return ln_photos_asset();
#endif
#if defined(LINNET_VIDEO)
    if(n->kind==LN_UI_VIDEO_SURFACE)return ln_video_asset();
#endif
    static struct ln_ui_asset art={64,64,0};
    if(n->kind==LN_UI_ALBUM_ART&&in_scene(n,LN_UI_SCENE_NOW_PLAYING)){
        uint32_t actual;
        art.pixels=ln_music_art_pixels_sized((uint32_t)n->w,&actual);art.width=art.height=(uint16_t)actual;
        if(art.pixels)return &art;}
    return fallback;
}
const struct ln_ui_asset *ln_ui_dynamic_item_asset(const struct ln_ui_node*n,uint32_t item,const struct ln_ui_asset*fallback){
#if defined(LINNET_PHOTOS)
    if(in_scene(n,LN_UI_SCENE_PHOTOS_GRID))return ln_photos_thumbnail(item);
#else
    (void)n;(void)item;
#endif
    return fallback;
}
bool ln_ui_dynamic_navigation(const struct ln_ui_node*n,uint32_t item){
    if(n->kind!=LN_UI_VIRTUAL_LIST)return false;
#if defined(LINNET_FILES)
    if(in_scene(n,LN_UI_SCENE_FILES))return ln_files_navigates(item);
#endif
    if(in_scene(n,LN_UI_SCENE_MUSIC))return ln_music_browser_row_navigates(item);
    if(in_scene(n,LN_UI_SCENE_MUSIC_SOUND))return ln_music_sound_action(item)>=LN_SOUND_CUSTOM_EQ;
    if(in_scene(n,LN_UI_SCENE_DISPLAY))return item==1u;
    if(in_scene(n,LN_UI_SCENE_SHUTDOWN))return item==2u;
    if(in_scene(n,LN_UI_SCENE_MUSIC_OPTIONS)){
        uint32_t action=ln_music_option_action(item);
        return action==7u||action==8u||action==11u||action==12u;
    }
    return false;
}
uint32_t ln_ui_dynamic_count(const struct ln_ui_node*n,uint32_t fallback){
    if(in_scene(n,LN_UI_SCENE_ABOUT)&&n->kind==LN_UI_VIRTUAL_LIST)return ln_display_about_count();
#if defined(LINNET_FILES)
    if(in_scene(n,LN_UI_SCENE_FILES)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_files_count();
#endif
    if(in_scene(n,LN_UI_SCENE_DISPLAY)&&n->kind==LN_UI_VIRTUAL_LIST)return ln_music_menu_scene_enabled(LN_UI_SCENE_THEMES)?2u:1u;
#if defined(LINNET_NOTES)
    if(in_scene(n,LN_UI_SCENE_NOTES)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_notes_count();
#endif
#if defined(LINNET_VIDEO)
    if(in_scene(n,LN_UI_SCENE_VIDEOS)&&n->kind==LN_UI_VIRTUAL_LIST)return ln_video_library_count();
    if(in_scene(n,LN_UI_SCENE_VIDEO_SETTINGS)&&n->kind==LN_UI_VIRTUAL_LIST)return 4u;
    if(in_scene(n,LN_UI_SCENE_VIDEO_OUTPUT)&&n->kind==LN_UI_VIRTUAL_LIST)return 2u;
#endif
#if defined(LINNET_PHOTOS)
    if((in_scene(n,LN_UI_SCENE_PHOTOS)||in_scene(n,LN_UI_SCENE_PHOTOS_GRID))&&
       (n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_GRID||n->kind==LN_UI_SCROLLBAR))return ln_photos_count();
#endif
    if(in_scene(n,LN_UI_SCENE_MUSIC_OPTIONS)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_music_option_count();
    if(in_scene(n,LN_UI_SCENE_MUSIC_SOUND)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_music_sound_count();
    if(in_scene(n,LN_UI_SCENE_MUSIC_SEARCH)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_music_search_count();
    if(in_scene(n,LN_UI_SCENE_MUSIC_QUEUE)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_music_queue_view_count();
    if(in_scene(n,LN_UI_SCENE_MUSIC_QUEUE_ACTIONS)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return 5;
    if(in_scene(n,LN_UI_SCENE_MUSIC)&&(n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_SCROLLBAR))return ln_music_browser_count();
    if(in_scene(n,LN_UI_SCENE_NOW_PLAYING)&&n->kind==LN_UI_PROGRESS_BAR){
        struct ln_player_status p;ln_player_snapshot(&p);const struct ln_music_library_row*r=ln_music_current();uint64_t frames=p.frames?p.frames:r?r->track.frames:0;
        return frames>UINT32_MAX?UINT32_MAX:(uint32_t)frames;}
    return fallback;
}
uint32_t ln_ui_dynamic_value(const struct ln_ui_node*n,uint32_t fallback){
    if(in_scene(n,LN_UI_SCENE_ABOUT)&&n->kind==LN_UI_PROGRESS_BAR)return ln_display_about_percent();
#if defined(LINNET_VIDEO)
    if(in_scene(n,LN_UI_SCENE_VIDEO_CONTROLS)&&n->kind==LN_UI_PROGRESS_BAR)return ln_video_progress();
#endif
    if(in_scene(n,LN_UI_SCENE_NOW_PLAYING)&&n->kind==LN_UI_PROGRESS_BAR){uint64_t position=ln_player_position();return position>UINT32_MAX?UINT32_MAX:(uint32_t)position;}
    return fallback;
}
const char *ln_ui_dynamic_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){
    if(in_scene(n,LN_UI_SCENE_ABOUT)&&n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_display_about_row(item);
#if defined(LINNET_FILES)
    if(in_scene(n,LN_UI_SCENE_FILES)){
        if(n->kind==LN_UI_STATUS_BAR)return ln_files_title();
        if(n->kind==LN_UI_TEXT)return ln_files_hint();
        if(n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_files_label(item);
    }
#endif
    if(n->text&&equal(n->text,"Sleep timer"))return item<4u?ln_music_sleep_label(item):fallback;
    if(n->text&&equal(n->text,"Sleep timer status"))return ln_music_sleep_status();
#if defined(LINNET_NOTES)
    if(in_scene(n,LN_UI_SCENE_NOTES)){
        if(n->kind==LN_UI_STATUS_BAR)return ln_notes_title();
        if(n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_notes_label(item);
    }
#endif
    /* These are the only kinds handled below. Other nodes preserve their
     * fallback without repeating scene ownership scans or calling services. */
    if(n->kind!=LN_UI_TEXT&&n->kind!=LN_UI_STATUS_BAR&&n->kind!=LN_UI_VIRTUAL_LIST&&
       n->kind!=LN_UI_GRID&&n->kind!=LN_UI_VIDEO_SURFACE&&n->kind!=LN_UI_PHOTO_SURFACE&&
       n->kind!=LN_UI_ALBUM_ART)return fallback;
#if defined(LINNET_VIDEO)
    if(in_scene(n,LN_UI_SCENE_VIDEOS)&&n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_video_library_label(item);
    if(in_scene(n,LN_UI_SCENE_VIDEO_SETTINGS)&&n->kind==LN_UI_VIRTUAL_LIST&&item<4u)return ln_video_option_label(item);
    if(in_scene(n,LN_UI_SCENE_VIDEO_OUTPUT)&&n->kind==LN_UI_VIRTUAL_LIST&&item<2u)return item?"TV":"iPod";
    if(in_scene(n,LN_UI_SCENE_VIDEO_PLAYER)||in_scene(n,LN_UI_SCENE_VIDEO_CONTROLS)){
        const char *status=ln_video_status();
#if defined(LINNET_TARGET_VIDEO5G_M25)
        /* Selection can paint before stopped music/storage admits open(). */
        if(ln_runtime_video_requested())status="Starting video...";
#endif
        if(n->kind==LN_UI_VIDEO_SURFACE)return ln_video_asset()?"":status;
        if(n->kind==LN_UI_STATUS_BAR)return status;
    }
#endif
#if defined(LINNET_PHOTOS)
    if(in_scene(n,LN_UI_SCENE_PHOTOS)||in_scene(n,LN_UI_SCENE_PHOTOS_GRID)||in_scene(n,LN_UI_SCENE_PHOTOS_VIEWER)){
        if(n->kind==LN_UI_STATUS_BAR)return ln_photos_title();
        if((n->kind==LN_UI_VIRTUAL_LIST||n->kind==LN_UI_GRID)&&item!=UINT32_MAX)return ln_photos_label(item);
        if(n->kind==LN_UI_PHOTO_SURFACE)return ln_photos_asset()?"":ln_photos_status();
        if(n->kind==LN_UI_TEXT&&equal(fallback,"Photo empty folder")){
            const char *status=ln_photos_status();
            return status[0]?status:ln_photos_count()?"":"No photos in this folder";
        }
        if(n->kind==LN_UI_TEXT&&equal(fallback,"Photo count")){
            if(ln_photos_status()[0])return "";
            static char count[24];uint32_t value=ln_photos_count(),length=0;char digits[10];
            do{digits[length++]=(char)('0'+value%10u);value/=10u;}while(value);
            uint32_t at=0;while(length)count[at++]=digits[--length];
            const char *suffix=" Photos";while(*suffix)count[at++]=*suffix++;count[at]=0;return count;
        }
    }
    if(in_scene(n,LN_UI_SCENE_PHOTOS_OPTIONS)&&n->kind==LN_UI_VIRTUAL_LIST&&item<7u){
        static const char *const labels[]={"Wheel: zoom","Wheel: pan left/right","Wheel: pan up/down","Rotate clockwise","Start slideshow","Fit image","Back to photo"};
        if(item==4u&&ln_photos_slideshow_active())return "Stop slideshow";return labels[item];
    }
#endif
#if defined(LINNET_COVERFLOW)
    if(in_scene(n,LN_UI_SCENE_COVER_FLOW)&&n->kind==LN_UI_TEXT)
        return ln_coverflow_text(equal(fallback,"Cover Flow artist"));
#endif
    if(in_scene(n,LN_UI_SCENE_MUSIC_OPTIONS)){
        if(n->kind==LN_UI_STATUS_BAR)return "Playback Options";
        if(n->kind==LN_UI_VIRTUAL_LIST&&item<13)return ln_music_option_label(ln_music_option_action(item));
    }
    if(in_scene(n,LN_UI_SCENE_MUSIC_SOUND)){
        if(n->kind==LN_UI_STATUS_BAR)return ln_music_sound_title();
        if(n->kind==LN_UI_TEXT)return ln_music_sound_status();
        if(n->kind==LN_UI_VIRTUAL_LIST&&item<ln_music_sound_count())return ln_music_sound_label(item);
    }
    if(in_scene(n,LN_UI_SCENE_MUSIC_QUEUE)){
        if(n->kind==LN_UI_STATUS_BAR&&ln_music_notice())return ln_music_notice();
        if(n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_music_queue_view_label(item);
    }
    if(in_scene(n,LN_UI_SCENE_MUSIC_QUEUE_ACTIONS)&&n->kind==LN_UI_STATUS_BAR&&ln_music_notice())return ln_music_notice();
    if(in_scene(n,LN_UI_SCENE_MUSIC_QUEUE_ACTIONS)&&n->kind==LN_UI_VIRTUAL_LIST&&item<5){
        static const char *const labels[]={"Play","Remove","Move up","Move down","Clear queue"};return labels[ln_music_queue_action(item)];
    }
    if(in_scene(n,LN_UI_SCENE_MUSIC_SEARCH)){
        if(n->kind==LN_UI_STATUS_BAR)return ln_music_search_title();
        if(n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_music_search_label(item);
        if(n->kind==LN_UI_TEXT&&equal(fallback,"Search prefix"))return ln_music_search_prefix();
        if(n->kind==LN_UI_TEXT&&equal(fallback,"Search controls"))return ln_music_search_hint();
    }
    if(n->kind==LN_UI_ALBUM_ART&&in_scene(n,LN_UI_SCENE_NOW_PLAYING)&&ln_music_art_pixels())return "";
    if(in_scene(n,LN_UI_SCENE_MUSIC)){
        if(n->kind==LN_UI_TEXT){
            if(ln_music_library_status()==LN_LIBRARY_ERROR&&
               ln_music_library_error()==LN_MUSIC_LIBRARY_RECOVERY_REQUIRED)
                return "Connect to Media Manager to recover music";
            return ln_music_browser_hint();
        }
        if(n->kind==LN_UI_STATUS_BAR){uint32_t status=ln_music_library_status();return status==LN_LIBRARY_READY?ln_music_browser_title():status==LN_LIBRARY_ERROR?
            ln_music_library_error()==LN_MUSIC_LIBRARY_RECOVERY_REQUIRED?"Music - recovery needed":"Music - no library":"Music - loading";}
        if(n->kind==LN_UI_VIRTUAL_LIST&&item!=UINT32_MAX)return ln_music_browser_label(item);
    }
    if(in_scene(n,LN_UI_SCENE_NOW_PLAYING)&&n->kind==LN_UI_TEXT){
        const struct ln_music_library_row*r=ln_music_current();struct ln_player_status p;ln_player_snapshot(&p);
        if(equal(fallback,"Sample track"))return r?r->title:"Choose a track";
        if(equal(fallback,"Audio pending"))return r?r->artist:"Music library";
        if(equal(fallback,"Track album"))return r&&r->album_ready?r->album:"";
        if(equal(fallback,"Track position")){
            static char text[32];uint32_t position,count;
            if(!ln_music_queue_position(&position,&count))return "-- of --";
            char *out=number_text(text,position);*out++=' ';*out++='o';*out++='f';*out++=' ';
            out=number_text(out,count);*out=0;return text;
        }
        if(equal(fallback,"Track elapsed")||equal(fallback,"Track remaining")||equal(fallback,"Track duration")){
            static char elapsed[32],remaining[32],duration[32];
            uint32_t rate=p.rate?p.rate:r?r->track.rate:0;
            uint64_t frames=p.frames?p.frames:r?r->track.frames:0,position=ln_player_position();
            if(equal(fallback,"Track elapsed"))return clock_text(elapsed,position,rate,false);
            if(!frames)return "--:--";
            if(equal(fallback,"Track duration"))return clock_text(duration,frames,rate,false);
            return clock_text(remaining,position<frames?frames-position:0,rate,true);
        }
        if(equal(fallback,"Native playback shell")){
#if LINNET_ART_MAX_SIDE >= 160u
            return ln_music_notice()?ln_music_notice():p.phase==LN_PLAYER_ERROR?"Playback error":
                p.phase==LN_PLAYER_IDLE&&!ln_music_loading()&&r?"Stopped":"";
#else
            return ln_music_notice()?ln_music_notice():ln_music_loading()?"Loading...":p.phase==LN_PLAYER_PLAY?"":p.phase==LN_PLAYER_PAUSE?"":p.phase==LN_PLAYER_ERROR?"Playback error":ln_player_busy()?"Loading...":r?"Stopped":"";
#endif
        }
    }
    return fallback;
}
