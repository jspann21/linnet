/* Desktop-only native ABI preview. Device code always uses the bounded ARM loader. */
#include "services/games/games.h"
#include "sdk/include/linnet/game.h"
#include "game_previews.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static HMODULE image;
#else
#include <dlfcn.h>
static void *image;
#endif
static char directory[4096];
static const struct ln_game_module *module;
static uint32_t current,now,revision,hold_at,held_us;
static bool held,exit_requested,music_on;
static uint32_t scores[128],save_lengths[128],schemas[128];
static uint8_t saves[128][LN_GAME_SAVE_BYTES];
static uint32_t time_us(void){return (held?hold_at:now)-held_us;}
static uint32_t best(void){return scores[current];}
static bool record(uint32_t value){if(value>scores[current])scores[current]=value;return true;}
static int32_t read_save(uint32_t schema,void *out,uint32_t capacity){
    if(schema!=schemas[current]||capacity<save_lengths[current]||(!out&&save_lengths[current]))return -1;
    if(save_lengths[current])memcpy(out,saves[current],save_lengths[current]);return (int32_t)save_lengths[current];
}
static bool write_save(uint32_t schema,const void *data,uint32_t bytes){
    if(schema!=game_previews[current].schema||schema<schemas[current]||bytes>LN_GAME_SAVE_BYTES||(!data&&bytes))return false;
    if(bytes)memcpy(saves[current],data,bytes);schemas[current]=schema;save_lengths[current]=bytes;return true;
}
static const char *titles[]={"Amber Sky","Blue River","Copper Moon","Distant Shore"};
static uint32_t song=UINT32_MAX;
static bool quiz_active,saved_music;
static uint32_t saved_song;
static uint32_t music_count(void){return 4u;}
static bool title(uint32_t index,char *out,uint32_t capacity){
    if(index>=4u||!capacity)return false;
    int count=snprintf(out,capacity,"%s",titles[index]);return count>=0&&(uint32_t)count<capacity;
}
static uint32_t music_current(void){return song;}
static bool loading(void){return false;}
static bool select_song(uint32_t index){if(index>=4u)return false;song=index;music_on=true;return true;}
static bool begin_song(uint32_t index){
    if(index>=4u)return false;
    if(!quiz_active){saved_song=song;saved_music=music_on;quiz_active=true;}
    return select_song(index);
}
static void end_song(void){if(quiz_active){song=saved_song;music_on=saved_music;quiz_active=false;}}
static bool pause_song(bool paused){music_on=!paused;return true;}
static void toggle_song(void){music_on=!music_on;}
static bool music_playing(void){return music_on;}
static struct ln_game_host host={LN_GAME_ABI,sizeof host,0,
    &ln_ui_font,&ln_ui_hint_font,&ln_ui_clock_font,time_us,best,record,read_save,write_save,
    music_count,title,music_current,loading,begin_song,select_song,end_song,pause_song,toggle_song,music_playing};
bool ln_game_preview_init(const char *path){
    for(uint32_t i=0;i<GAME_PREVIEW_COUNT;++i)schemas[i]=game_previews[i].schema;
    int size=snprintf(directory,sizeof directory,"%s",path);return size>=0&&size<(int)sizeof directory;
}
bool ln_game_preview_ready(void){return module!=0;}
uint32_t ln_games_count(void){return GAME_PREVIEW_COUNT;}
const char *ln_games_label(uint32_t row){return row<GAME_PREVIEW_COUNT?game_previews[row].title:"";}
const char *ln_games_description(uint32_t row){return row<GAME_PREVIEW_COUNT?game_previews[row].description:"";}
void ln_games_stop(void){
    if(module){module->stop();module=0;end_song();++revision;}
    if(image){
#ifdef _WIN32
        FreeLibrary(image);
#else
        dlclose(image);
#endif
        image=0;
    }
}
void ln_games_start(uint32_t row){
    ln_games_stop();if(row>=GAME_PREVIEW_COUNT)return;
    current=row;held=false;held_us=0;
    host.capabilities=game_previews[row].capabilities;
    char path[8192];int size=snprintf(path,sizeof path,"%s/%s",directory,game_previews[row].file);
    if(size<0||size>=(int)sizeof path)return;
#ifdef _WIN32
    image=LoadLibraryA(path);
    ln_game_entry entry=image?(ln_game_entry)(uintptr_t)GetProcAddress(image,"ln_game_entry_point"):0;
#else
    image=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    ln_game_entry entry=image?(ln_game_entry)dlsym(image,"ln_game_entry_point"):0;
#endif
    if(!entry){ln_games_stop();return;}
    module=entry(&host);
    if(!module||module->abi!=LN_GAME_ABI||module->bytes!=sizeof *module||!module->start||!module->input||
       !module->tick||!module->deadline||!module->draw||!module->playing||!module->revision||!module->take_cue||!module->stop){
        module=0;ln_games_stop();return;
    }
    if(!module->start())ln_games_stop();
    ++revision;
}
int ln_games_input(uint32_t kind,int32_t value){
    if(!module)return LN_GAME_UNHANDLED;
    if(kind==LN_GAME_INPUT_HOLD){
        if(held!=(value!=0)){if(value)hold_at=now;else held_us+=now-hold_at;held=value!=0;}
    }else if(held)return LN_GAME_CONSUMED;
    int result=module->input(kind,value);
    if(result==LN_GAME_CHANGED)++revision;
    if(result==LN_GAME_EXIT){ln_games_stop();exit_requested=true;}
    return result;
}
bool ln_games_tick(uint32_t value){now=value;return module&&!held?module->tick(time_us()):false;}
uint32_t ln_games_deadline(uint32_t value){
    (void)value;if(!module||held)return UINT32_MAX;
    uint32_t due=module->deadline(time_us());return due==UINT32_MAX?due:due+held_us;
}
bool ln_games_playing(void){return module&&module->playing();}
uint32_t ln_games_hud_revision(void){return revision+(module?module->revision():0);}
void ln_games_draw(const struct ln_ui_canvas_context *context){if(module)module->draw(context);}
const char *ln_games_text(const char *key){return key&&!strcmp(key,"Game music")?(music_on?"Play: Pause music":"Play: Start/resume music"):"";}
void ln_games_music_toggle(void){toggle_song();++revision;}
void ln_games_best_set(uint32_t index,uint16_t value){if(index<128u&&value>scores[index])scores[index]=value;}
void ln_games_audio_step(void){}
void ln_games_audio_stop(void){}
void ln_games_step(uint32_t value,bool visible,bool hold,bool may_write){
    (void)may_write;now=value;
    if(!visible){ln_games_stop();return;}
    if(module&&hold!=held)(void)ln_games_input(LN_GAME_INPUT_HOLD,hold);
}
bool ln_games_pending(void){return module!=0;}
bool ln_games_quiesce(bool publish){(void)publish;ln_games_stop();return true;}
bool ln_games_take_exit(void){bool value=exit_requested;exit_requested=false;return value;}
