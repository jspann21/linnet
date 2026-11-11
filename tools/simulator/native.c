/* Desktop oracle executes the same generated scenes and native renderer. */
#include "ui/runtime/ui.h"
#include "linnet_target.h"
#include "services/music/menu.h"
#include "services/display/clock.h"
#include "services/display/preferences.h"
#include "services/games/games.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef LN_GAME_PREVIEWS
extern bool ln_game_preview_init(const char *path);
extern bool ln_game_preview_ready(void);
#endif
static uint32_t sample_battery=80u,sample_clock=9u*3600u+41u*60u,sample_playback,sample_modes;
static bool sample_art,sample_more;
static bool sample_video_frame;
static uint16_t sample_video_pixels[LINNET_UI_WIDTH*LINNET_UI_HEIGHT];
static const struct ln_ui_asset sample_video_asset={LINNET_UI_WIDTH,LINNET_UI_HEIGHT,sample_video_pixels};
static uint32_t sample_sound_view,sample_sound_parent,sample_sound_first,sample_music_parent;
#if defined(LINNET_HARDWARE_CLICKER)
#define SAMPLE_CLICKER true
#else
#define SAMPLE_CLICKER false
#endif
static uint32_t sound_action(uint32_t row){return ln_sound_menu_action(sample_sound_view,row,LINNET_EXTENDED_SOUND,SAMPLE_CLICKER);}
static bool node_scene(const struct ln_ui_node *n,const char *id){
    for(uint32_t i=0;i<ln_ui_scene_count;++i)if(!strcmp(ln_ui_scenes[i].id,id))
        for(uint32_t j=0;j<ln_ui_scenes[i].count;++j)if(n==&ln_ui_scenes[i].nodes[j])return true;
    return false;
}
uint32_t ln_ui_battery_status(void){return sample_battery;}
uint32_t ln_ui_clock_seconds(void){return sample_clock;}
uint32_t ln_ui_playback_status(void){return sample_playback;}
uint32_t ln_ui_playback_modes(void){return sample_modes;}
const struct ln_ui_asset *ln_ui_now_playing_art(uint32_t *revision){
    *revision=sample_art?1u:0u;return sample_art&&ln_ui_asset_count?&ln_ui_assets[0]:0;
}
const struct ln_ui_asset *ln_ui_dynamic_asset(const struct ln_ui_node*n,const struct ln_ui_asset*fallback){
    return n->kind==LN_UI_VIDEO_SURFACE&&sample_video_frame?&sample_video_asset:fallback;
}
/* Explicit sample metadata; device builds bind these keys to the music service. */
static bool music_category_node(const struct ln_ui_node *node){
    for(uint32_t i=0;i<ln_ui_scene_count;++i){
        const struct ln_ui_scene *scene=&ln_ui_scenes[i];
        if(strcmp(scene->id,"linnet.system.music"))continue;
        for(uint32_t j=0;j<scene->count;++j)if(&scene->nodes[j]==node)return true;
        break;
    }
    return false;
}
bool ln_ui_dynamic_navigation(const struct ln_ui_node*n,uint32_t item){
    if(n->kind!=LN_UI_VIRTUAL_LIST)return false;
    if(node_scene(n,"linnet.system.files"))return item<5u;
    if(music_category_node(n))return ln_music_menu_category(sample_more,item)!=15u;
    if(n->text&&!strcmp(n->text,"Sound "))return sound_action(item)>=LN_SOUND_CUSTOM_EQ;
    if(node_scene(n,"linnet.system.display"))return item==1u;
    if(node_scene(n,"linnet.system.shutdown"))return item==2u;
    if(n->text&&!strcmp(n->text,"Option ")){
        uint32_t action=ln_music_menu_option_action(false,item);
        return action==7u||action==8u||action==11u||action==12u;
    }
    return false;
}
uint32_t ln_ui_dynamic_count(const struct ln_ui_node *node,uint32_t fallback){
    if(node_scene(node,"linnet.system.about")&&node->text&&!strcmp(node->text,"About detail"))return 33u;
    if(node_scene(node,"linnet.system.files")&&(node->kind==LN_UI_VIRTUAL_LIST||node->kind==LN_UI_SCROLLBAR))return 6u;
    if(music_category_node(node)&&(node->kind==LN_UI_VIRTUAL_LIST||node->kind==LN_UI_SCROLLBAR))return ln_music_menu_count(sample_more);
    if(node_scene(node,"linnet.system.music-sound")&&(node->kind==LN_UI_VIRTUAL_LIST||node->kind==LN_UI_SCROLLBAR))return ln_sound_menu_count(sample_sound_view,LINNET_EXTENDED_SOUND,SAMPLE_CLICKER);
    if(node_scene(node,"linnet.system.music-options")&&(node->kind==LN_UI_VIRTUAL_LIST||node->kind==LN_UI_SCROLLBAR))return ln_music_menu_option_count(false);
    if(node_scene(node,"linnet.system.display")&&node->kind==LN_UI_VIRTUAL_LIST)return ln_music_menu_scene_enabled(LN_UI_SCENE_THEMES)?2u:1u;
    return fallback;
}
uint32_t ln_ui_dynamic_value(const struct ln_ui_node *node,uint32_t fallback){
    if(node_scene(node,"linnet.system.about")&&node->text&&!strcmp(node->text,"About storage"))return 27u;
    return fallback;
}
const char *ln_ui_dynamic_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){
    if(n->kind==LN_UI_VIDEO_SURFACE||
       (n->kind==LN_UI_STATUS_BAR&&node_scene(n,"linnet.system.video-controls"))){
        if(n->kind==LN_UI_VIDEO_SURFACE&&sample_video_frame)return "";
        return sample_playback==2u?"Paused":sample_playback?"Playing":"Stopped";
    }
    if(node_scene(n,"linnet.system.about")&&n->text&&!strcmp(n->text,"About detail")&&item!=UINT32_MAX){
        static const char*const details[]={
            "Linnet OS 1.0.0","Build 4e37dca803b2","Name  My iPod","Device  iPod Classic",
            "Model no.  MB147","Board  n25","Apple OS  1.1.2","Family  classic",
            "Region  LL","Serial  8K12345TTXK","USB serial  000000001234TTXK",
            "Disk ID  000000001234TTXK","USB model  Apple iPod USB Device",
            "USB  05AC:1261","Volume ID  ABCD-1234","Disk size  30.0 GB",
            "Partition  29.9 GB","Sector  512 B","Capacity  29.8 GB",
            "Used  8.2 GB","Free  21.6 GB","Audio  6.1 GB","Video  1.0 GB",
            "Data  64.0 MB","Other  1.0 GB","Drive model  TOSHIBA MK3008GAL",
            "Drive serial  12ABC3456","Drive FW  JC102A","Drive type  Hard disk",
            "Rotation  4200","Physical sector  512","Addressing  LBA28","Write cache  On"};
        return item<sizeof details/sizeof details[0]?details[item]:fallback;
    }
    if(node_scene(n,"linnet.system.files")){
        static const char*const names[]={"Music","Notes","Photos","Videos",".LINNET","README.txt"};
        if(n->kind==LN_UI_VIRTUAL_LIST&&item<6u)return names[item];
        if(n->kind==LN_UI_TEXT)return "Left: Info  Right: Live";
    }
    if(n->kind==LN_UI_STATUS_BAR&&music_category_node(n))return sample_more?"More Music":"Music";
    if(n->kind==LN_UI_STATUS_BAR&&node_scene(n,"linnet.system.music-sound"))return sample_sound_view==LN_SOUND_EQ?"Custom EQ":sample_sound_view==LN_SOUND_ADVANCED?"Advanced Sound":"Sound";
    if(!n->text)return fallback;
    if(item==UINT32_MAX&&node_scene(n,"linnet.system.music-search")){
        if(!strcmp(n->text,"Search prefix"))return "Enter name prefix";
        if(!strcmp(n->text,"Search controls"))return "Select: Add  Left: Erase  Right: Find";
    }
    if(item==UINT32_MAX&&strcmp(n->text,"Right: Actions  Menu: Back")==0&&music_category_node(n))
        return "Select: Choose  Menu: Back";
    if(item!=UINT32_MAX&&strcmp(n->text,"Track ")==0&&music_category_node(n)){
        static const char *const categories[]={"Songs","Artists","Albums","Genres","Composers","Years","Folders","Playlists","Search","Favorites","Bookmarks","Recently Played","Most Played","Recently Added","Album Artists","Shuffle Songs"};
        uint32_t category=ln_music_menu_category(sample_more,item);
        if(category==LN_MUSIC_COVER_FLOW)return "Cover Flow";
        if(category==LN_MUSIC_MORE)return "More";
        if(category<16u)return categories[category];
    }
    if(item!=UINT32_MAX&&strcmp(n->text,"Option ")==0){
        static const char *const labels[]={"Shuffle: Off","Repeat: Off","Favorite: No","Save bookmark","No bookmark saved","Restart track","Stop","Play Queue","Sound","Back 10 seconds","Forward 10 seconds","Save queue as playlist...","Sleep Timer"};
        uint32_t action=ln_music_menu_option_action(false,item);
        if(action==0)return sample_modes&1u?"Shuffle: Songs":"Shuffle: Off";
        if(action==1)return (sample_modes>>1)==2u?"Repeat: One":(sample_modes>>1)==1u?"Repeat: All":"Repeat: Off";
        return action<13u?labels[action]:fallback;
    }
    if(item!=UINT32_MAX){
        static const char *const sound[]={"EQ: Off","ReplayGain: Off","Crossfeed: Off","Balance: Center","Channels: Stereo","Preamp: 0 dB","Volume limit: 0 dB","Bass 100 Hz: 0 dB","Treble 8 kHz: 0 dB"};
        static const char *const extended_sound[]={"EQ: Off","Sound Check: Off","Crossfeed: Off","Balance: Center","Channels: Stereo","Preamp: 0 dB","Volume limit: 0 dB","Dock line out: Off","Bass 100 Hz: 0 dB","Low 250 Hz: 0 dB","Mid 1 kHz: 0 dB","High 4 kHz: 0 dB","Treble 8 kHz: 0 dB"};
        static const char *const photos[]={"Wheel: zoom","Wheel: pan left/right","Wheel: pan up/down","Rotate clockwise","Start slideshow","Fit image","Back to photo"};
        static const char *const actions[]={"Play","Move up","Move down","Remove","Clear queue"};
        static const char *const video[]={"TV Out: Off","TV Signal: NTSC","Widescreen: Off","TV Connection: Composite"};
        if(!strcmp(n->text,"Sound ")){
            uint32_t action=sound_action(item);
            if(action==LN_SOUND_CLICKER)return "Click pitch: Classic";
            if(action==LN_SOUND_CLICK_VOLUME)return "Click volume: 100%";
            if(action==LN_SOUND_CLICK_ENABLE)return "Clicker: On";
            if(action==LN_SOUND_CLICK_OUTPUT)return "Click output: Auto";
            if(action==LN_SOUND_CUSTOM_EQ)return "Custom EQ";
            if(action==LN_SOUND_MORE)return "Advanced";
            if(LINNET_EXTENDED_SOUND&&action<13u)return extended_sound[action];
            if(action<9u)return sound[action];
        }
        if(!strcmp(n->text,"Photo controls")&&item<7u)return photos[item];
        if(!strcmp(n->text,"Action ")&&item<5u)return actions[item];
        if(!strcmp(n->text,"Video setting")&&item<4u)return video[item];
        if(!strcmp(n->text,"Video output")&&item<2u)return item?"TV":"iPod";
        return fallback;
    }
    if(!strcmp(n->text,"Native playback shell"))return sample_playback?"":"Stopped";
    if(!strcmp(n->text,"Track position"))return "1 of 12";
    if(!strcmp(n->text,"Track album"))return "Sample album";
    if(!strcmp(n->text,"Track elapsed"))return "1:24";
    if(!strcmp(n->text,"Track remaining"))return "-2:36";
    return fallback;
}
static struct ln_calendar sample_calendar={2026,9,7,9,41,0,1};
static uint32_t sample_read=LN_CALENDAR_READ_VALID,sample_result=LN_CALENDAR_SAVED,save_calls;
static bool save_presented;
bool ln_ui_clock_begin(void){
    ln_clock_editor_cancel();save_presented=false;
    sample_calendar.hour=(uint8_t)(sample_clock<86400u?sample_clock/3600u:0);
    sample_calendar.minute=(uint8_t)(sample_clock<86400u?(sample_clock/60u)%60u:0);
    ln_clock_editor_begin(&sample_calendar,sample_read==LN_CALENDAR_READ_VALID);
    if(sample_read==LN_CALENDAR_READ_UNAVAILABLE)ln_clock_editor_block("Clock unavailable. Edit, retry Save.");
    return true;
}
int ln_ui_clock_input(struct ln_ui_state*s,uint32_t kind,int32_t value){return ln_clock_editor_input(s,kind,value);}
void ln_ui_clock_cancel(void){ln_clock_editor_cancel();}
const char *ln_ui_clock_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){return ln_clock_editor_text(n,item,fallback);}
static void clock_step(struct ln_ui_state*s){
    if(!save_presented)return;save_presented=false;
    struct ln_calendar requested;if(!ln_clock_editor_request(&requested))return;
    if(s->hold)ln_clock_editor_block("Unlock Hold before saving.");
    else{
        ++save_calls;ln_clock_editor_result((enum ln_calendar_result)sample_result);
        if(sample_result==LN_CALENDAR_SAVED){sample_calendar=requested;sample_clock=(uint32_t)requested.hour*3600u+(uint32_t)requested.minute*60u;
            (void)ln_ui_show(s,"linnet.system.clock");}
    }
    ln_ui_refresh(s);
}
static uint16_t pixels[LINNET_UI_WIDTH*LINNET_UI_HEIGHT];
static uint32_t damage_count;
static void damage(int32_t x,int32_t y,int32_t w,int32_t h){(void)x;(void)y;(void)w;(void)h;++damage_count;}
static uint32_t crc(void){
    uint32_t c=0xffffffffu;
    for(uint32_t i=0;i<LINNET_UI_WIDTH*LINNET_UI_HEIGHT;++i)for(unsigned j=0;j<2;++j){
        c^=(pixels[i]>>(j*8))&255u;for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);
    }
    return c^0xffffffffu;
}
static int snapshot(const struct ln_ui_state *state,const char *directory,uint32_t index){
    char name[4096];if(snprintf(name,sizeof name,"%s/frame-%05u.rgb565",directory,index)>=(int)sizeof name)return 0;
    FILE *file=fopen(name,"wb");if(!file)return 0;
    for(uint32_t i=0;i<LINNET_UI_WIDTH*LINNET_UI_HEIGHT;++i){unsigned char p[2]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8)};if(fwrite(p,1,2,file)!=2){fclose(file);return 0;}}
    if(fclose(file))return 0;
    uint8_t menu_data[80];ln_ui_menu_encode(menu_data);
    if(snprintf(name,sizeof name,"%s/frame-%05u.menus.bin",directory,index)>=(int)sizeof name)return 0;
    file=fopen(name,"wb");if(!file)return 0;
    if(fwrite(menu_data,1,sizeof menu_data,file)!=sizeof menu_data){fclose(file);return 0;}
    if(fclose(file))return 0;
    printf("{\"frame\":%u,\"theme\":%u,\"battery\":%u,\"clock\":%u,\"playback\":%u,\"scene\":%u,\"selected\":%u,\"crc32\":%u,\"paint_count\":%u,\"painted_pixels\":%u,\"visible_items\":%u,\"scene_epoch\":%u,\"transitions\":%u,\"damage_count\":%u,\"action\":%u,\"clock_save_calls\":%u,\"brightness\":%u}\n",
        index,ln_ui_theme_index(),sample_battery,sample_clock,sample_playback,state->scene,state->selected,crc(),state->paint_count,state->painted_pixels,state->visible_items,state->scene_epoch,state->transitions,damage_count,state->action,save_calls,ln_preferences_brightness());
    return 1;
}
static void menu_input(struct ln_ui_state *s,uint32_t kind,int32_t value){
    uint32_t pressed=kind==2u?(uint32_t)value&~s->buttons:0;bool handled=false;
    const char *id=ln_ui_scenes[s->scene].id;
    if(!s->hold&&pressed){
        if(!strcmp(id,"linnet.system.music")){
            uint32_t category=ln_music_menu_category(sample_more,s->selected);
            if((pressed&1u)&&sample_more){sample_more=false;s->selected=sample_music_parent;s->first_visible[s->scene]=0;handled=true;}
            else if(pressed&16u){
                if(category==LN_MUSIC_MORE){sample_music_parent=s->selected;sample_more=true;s->selected=0;s->first_visible[s->scene]=0;handled=true;}
                else if(category==LN_MUSIC_COVER_FLOW){(void)ln_ui_show(s,"linnet.system.cover-flow");handled=true;}
            }
        }else if(!strcmp(id,"linnet.system.now-playing")&&(pressed&16u)){
            (void)ln_ui_show(s,"linnet.system.music-options");handled=true;
        }else if(!strcmp(id,"linnet.system.music-options")){
            uint32_t action=ln_music_menu_option_action(false,s->selected);
            if(pressed&1u){(void)ln_ui_show(s,"linnet.system.now-playing");handled=true;}
            else if(pressed&16u){
                if(action==8u)handled=ln_ui_show(s,"linnet.system.music-sound");
                else if(action==12u)handled=ln_ui_show(s,"linnet.system.sleep-timer");
                else if(action==7u)handled=ln_ui_show(s,"linnet.system.music-queue");
            }
        }else if(!strcmp(id,"linnet.system.music-sound")){
            uint32_t action=sound_action(s->selected);
            if((pressed&1u)&&sample_sound_view){sample_sound_view=LN_SOUND_ROOT;s->selected=sample_sound_parent;s->first_visible[s->scene]=sample_sound_first;handled=true;}
            else if((pressed&(16u|4u|8u))&&(action==LN_SOUND_CUSTOM_EQ||action==LN_SOUND_MORE)){
                if(pressed&8u)(void)ln_ui_back(s);
                else {sample_sound_first=ln_ui_first_visible(s);sample_sound_parent=s->selected;sample_sound_view=action==LN_SOUND_CUSTOM_EQ?LN_SOUND_EQ:LN_SOUND_ADVANCED;s->selected=0;s->first_visible[s->scene]=0;}
                handled=true;
            }
        }
        else if((pressed&16u)&&!strcmp(id,"linnet.system.shutdown")&&s->selected==2u){(void)ln_ui_show(s,"linnet.system.battery");handled=true;}
    }
    if(handled){s->buttons=(uint32_t)value;ln_ui_refresh(s);}else ln_ui_input(s,kind,value);
}
int main(int argc,char **argv){
    if(argc<2||argc>6)return 2;
    bool all_scenes=false,optional_font=false,optional_menus=false,game_preview=false;
    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--all-scenes")&&!all_scenes)all_scenes=true;
        else if(!strcmp(argv[i],"--apple-font")&&!optional_font)optional_font=true;
        else if(!strcmp(argv[i],"--menus")&&!optional_menus)optional_menus=true;
        else if(!strcmp(argv[i],"--game")&&!game_preview)game_preview=true;
        else return 2;
    }
    if(optional_menus){
        uint8_t menus[80];char path[4096];
        if(snprintf(path,sizeof path,"%s/MENUS.BIN",argv[1])>=(int)sizeof path)return 8;
        FILE *input=fopen(path,"rb");if(!input)return 8;
        size_t bytes=fread(menus,1,sizeof menus,input);int extra=fgetc(input),failed=ferror(input);fclose(input);
        if(failed||bytes!=sizeof menus||extra!=EOF||!ln_ui_menu_load(menus,sizeof menus))return 8;
    }
    if(optional_font){
        static uint8_t font[LN_UI_APPLE_FONT_BYTES] __attribute__((aligned(4)));
        char path[4096];if(snprintf(path,sizeof path,"%s/PODIUM.FNT",argv[1])>=(int)sizeof path)return 8;
        FILE *input=fopen(path,"rb");if(!input)return 8;
        size_t bytes=fread(font,1,sizeof font,input);int extra=fgetc(input),failed=ferror(input);fclose(input);
        if(failed||bytes!=sizeof font||extra!=EOF||!ln_ui_apple_font_load(font,sizeof font))return 8;
    }
    for(uint32_t y=0;y<LINNET_UI_HEIGHT;++y)for(uint32_t x=0;x<LINNET_UI_WIDTH;++x)
        sample_video_pixels[y*LINNET_UI_WIDTH+x]=(uint16_t)(((x*31u/LINNET_UI_WIDTH)<<11)|((y*63u/LINNET_UI_HEIGHT)<<5)|10u);
#ifdef LN_GAME_PREVIEWS
    if(!ln_game_preview_init(argv[1]))return 8;
#endif
    struct ln_ui_state state;ln_ui_init(&state);
    if(game_preview){if(!ln_games_count())return 8;(void)ln_ui_show(&state,"linnet.system.game-play");ln_games_start(0);
#ifdef LN_GAME_PREVIEWS
        if(!ln_game_preview_ready()){fprintf(stderr,"Game preview could not load or start the native module\n");return 8;}
#endif
    }
    ln_ui_render(&state,pixels,damage);
    if(!snapshot(&state,argv[1],0))return 3;
    uint32_t kind,repeat,capture,index=0,total=0,simulated_us=0;int32_t value;
#ifdef LN_GAME_PREVIEWS
    uint32_t game_revision=ln_games_hud_revision();
#endif
    while(scanf("%u %d %u %u",&kind,&value,&repeat,&capture)==4){
        if(kind>12u||!repeat||repeat>100000u||total>100000u-repeat||capture>1u)return 4;
        for(uint32_t i=0;i<repeat;++i){if(kind<=4u&&kind)menu_input(&state,kind,value);
            else if(kind==0u&&value==1)ln_ui_refresh(&state);
            else if(kind==5u){if(value!=-1&&((value&255)>100||value<0||value>1023))return 4;sample_battery=(uint32_t)value;}
            else if(kind==6u){if(value< -1||value>=86400)return 4;sample_clock=(uint32_t)value;}
            else if(kind==7u){if(value<0||value>2)return 4;sample_playback=(uint32_t)value;}
            else if(kind==8u){if(value<0||value>4)return 4;sample_result=(uint32_t)value;}
            else if(kind==9u){if(value<0||value>2)return 4;sample_read=(uint32_t)value;}
            else if(kind==10u){if(value<0||value>2)return 4;}
            else if(kind==11u){if(value<0||value>5)return 4;sample_modes=(uint32_t)value;}
            else if(kind==12u){if(value<0||value>1)return 4;if(sample_art!=(value!=0)){sample_art=value!=0;ln_ui_refresh(&state);}}
            if(state.action==LINNET_UI_ACTION_SHUFFLE||state.action==LINNET_UI_ACTION_REPEAT){
                uint32_t action=ln_ui_take_action(&state);
                sample_modes=action==LINNET_UI_ACTION_SHUFFLE?sample_modes^1u:(sample_modes&1u)|((((sample_modes>>1)+1u)%3u)<<1);
                ln_ui_refresh(&state);
            }
            simulated_us+=33333u;
#ifdef LN_GAME_PREVIEWS
            ln_games_step(simulated_us,state.scene==LN_UI_SCENE_GAME_PLAY,state.hold,true);
            if(ln_games_take_exit())(void)ln_ui_show(&state,"linnet.system.games");
            uint32_t next_game_revision=ln_games_hud_revision();
            if(next_game_revision!=game_revision){game_revision=next_game_revision;ln_ui_refresh(&state);}
#endif
            if(ln_games_tick(simulated_us))ln_ui_refresh(&state);
            clock_step(&state);(void)ln_ui_scroll_tick(&state,simulated_us);
            (void)ln_ui_palette_tick(&state,simulated_us);
            ln_ui_render(&state,pixels,damage);save_presented=ln_clock_editor_pending();}
        total+=repeat;
        if(capture){if(++index>512u||!snapshot(&state,argv[1],index))return 5;}
    }
    if(!feof(stdin))return 6;
    if(all_scenes){
        for(uint32_t theme=0;theme<ln_ui_theme_count;++theme){
            ln_ui_theme_changed(theme);ln_ui_init(&state);
            for(uint32_t scene=0;scene<ln_ui_scene_count&&scene<LINNET_UI_MAX_SCENES;++scene){
                if(!(ln_ui_enabled_scenes&(UINT64_C(1)<<scene)))continue;
                sample_video_frame=false;sample_playback=0;
                (void)ln_ui_show(&state,ln_ui_scenes[scene].id);
                memset(pixels,0,sizeof pixels);
                if(!strcmp(ln_ui_scenes[scene].id,"linnet.system.game-play"))ln_games_start(0);
                state.selected=0;ln_ui_refresh(&state);ln_ui_render(&state,pixels,damage);
                if(++index>2048u||!snapshot(&state,argv[1],index))return 7;
                if(!strcmp(ln_ui_scenes[scene].id,"linnet.system.video-player")||
                   !strcmp(ln_ui_scenes[scene].id,"linnet.system.video-controls")){
                    for(uint32_t status=1;status<=4u;++status){
                        sample_playback=status==2u?2u:1u;sample_video_frame=status>=3u;
                        ln_ui_refresh(&state);ln_ui_render(&state,pixels,damage);
                        if(++index>2048u||!snapshot(&state,argv[1],index))return 7;
                    }
                    if(ln_ui_themes[theme].flags&LN_UI_THEME_ALBUM_ART){
                        sample_video_frame=false;sample_art=true;
                        for(uint32_t step=0;step<=6u;++step){
                            (void)ln_ui_palette_tick(&state,200000u+step*25000u);
                            ln_ui_refresh(&state);ln_ui_render(&state,pixels,damage);
                            if(++index>2048u||!snapshot(&state,argv[1],index))return 7;
                        }
                        sample_art=false;
                        (void)ln_ui_palette_tick(&state,400000u);
                        (void)ln_ui_palette_tick(&state,1400000u);
                        (void)ln_ui_palette_tick(&state,1550000u);
                    }
                    sample_video_frame=false;sample_playback=0;
                }
                uint32_t count=ln_ui_focused_count(&state);
                if(count>ln_ui_visible_capacity(&state)){
                    state.selected=count-1u;ln_ui_refresh(&state);ln_ui_render(&state,pixels,damage);
                    if(++index>2048u||!snapshot(&state,argv[1],index))return 7;
                }
            }
        }
        return 0;
    }
    if(++index>513u||!snapshot(&state,argv[1],index))return 7;
    return 0;
}
