/* Built-in game adapter: each native package specializes one game.
 * Compiler constant propagation and section GC remove all other game data/code. */
#include "sdk/include/linnet/game.h"
static const struct ln_game_host *package_host;
#define ln_display_game_best_set(index,value) ((void)(index),(void)package_host->record_best(value))
#define ln_music_library_count() package_host->music_count()
#define ln_music_loading() package_host->music_loading()
#define ln_music_quiz_begin(index) package_host->quiz_begin(index)
#define ln_music_quiz_select(index) package_host->quiz_select(index)
#define ln_music_quiz_end() package_host->quiz_end()
#define ln_music_toggle() package_host->music_toggle()
#define ln_player_pause(paused) package_host->music_pause(paused)
/* Private helper types stay inside this adapter, never in the public ABI. */
struct ln_music_library_row {uint32_t index;char title[256];};
struct ln_player_status {uint32_t phase;};
#define LN_PLAYER_PLAY 4u
static const char *ln_music_library_title(uint32_t index){
    static char title[256];
    if(!package_host->music_title(index,title,sizeof title))title[0]=0;
    return title;
}
static const struct ln_music_library_row *ln_music_current(void){
    static struct ln_music_library_row row;row.index=package_host->music_current();
    if(row.index==UINT32_MAX)return 0;
    if(!package_host->music_title(row.index,row.title,sizeof row.title))return 0;
    return &row;
}
static void ln_player_snapshot(struct ln_player_status *out){out->phase=package_host->music_playing()?LN_PLAYER_PLAY:0;}
#define ln_games_count builtin_count
#define ln_games_label builtin_label
#define ln_games_description builtin_description
#define ln_games_configure builtin_configure
#define ln_games_best_set builtin_best_set
#define ln_games_start builtin_start
#define ln_games_input builtin_input
#define ln_games_playing builtin_playing
#define ln_games_hud_revision builtin_revision
#define ln_games_deadline builtin_deadline
#define ln_games_tick builtin_tick
#define ln_games_text builtin_text
#define ln_games_draw builtin_draw
#define ln_games_audio_stop builtin_audio_stop
#define ln_games_audio_step builtin_audio_step
#include "services/games/builtin.c"
static bool package_start(void){
    uint32_t seed=package_host->time_us();random_state^=seed;if(!random_state)random_state=0x9e3779b9u;
    last_tick=seed;
    best[LN_GAME_PACKAGE_ID]=(uint16_t)package_host->best();builtin_start(0);return true;
}
static bool package_cue(struct ln_game_cue *out){
    if(!sound_pending)return false;out->kind=sound_pending;sound_pending=0;
    for(uint32_t i=0;i<4u;++i)out->notes[i]=game==MIXTAPE?mt_goal[mt_bar][i]:0;
    return true;
}
static void package_stop(void){
    sound_pending=0;
    if(game==MUSIC_QUIZ)package_host->quiz_end();
}
static const struct ln_game_module module={LN_GAME_ABI,sizeof module,package_start,builtin_input,
    builtin_tick,builtin_deadline,builtin_draw,builtin_playing,builtin_revision,package_cue,package_stop};
#if defined(_WIN32)
__declspec(dllexport)
#endif
const struct ln_game_module *ln_game_entry_point(const struct ln_game_host *host){
    if(!host||host->abi!=LN_GAME_ABI||host->bytes<sizeof *host||
       (game==MUSIC_QUIZ&&!(host->capabilities&LN_GAME_CAP_MUSIC)))return 0;
    package_host=host;return &module;
}
