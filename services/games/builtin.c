#if defined(LN_GAME_PACKAGE_ID)
#define LN_GAME_COUNT 18u
#define LN_GAME_SUPPORTED_MASK ((1u<<LN_GAME_COUNT)-1u)
#else
#include "services/games/games.h"
#endif
#if defined(LINNET_AUDIO) && !defined(LN_GAME_PACKAGE_ID)
#include "hal/api/audio.h"
#include "services/music/controller.h"
#include "services/music/player.h"
#endif
#if defined(LINNET_STORAGE) && !defined(LN_GAME_PACKAGE_ID)
#include "services/display/settings.h"
#endif
#if LINNET_GAME_PRESENTATION_PHOTO220
#include "services/games/presentations/photo220/solitaire_courts.h"
#elif LINNET_GAME_PRESENTATION_COLOR320
#include "services/games/presentations/color320/solitaire_courts.h"
#else
#error Games require a supported presentation profile
#endif

enum { COMET, PULSE, SOLITAIRE, SUDOKU, BRICK, MUSIC_QUIZ, VORTEX, MINESWEEPER,
       FALLING, REVERSI, SNAKE, WORD_SEARCH, SCRUB, DIAL_HEIST, MIXTAPE, SIGNAL,
       CLOCKWORK_CITY, POCKET_PINBALL, GAME_COUNT };
typedef char ln_game_count_matches[(GAME_COUNT==LN_GAME_COUNT)?1:-1];
enum { PLAY=1u, PAUSED, LOST, WATCH, MISTAKE, WON, ARCADE_READY, SOL_CONFIRM,
       SUDOKU_LEVEL, SUDOKU_PICK, SUDOKU_MENU, SUDOKU_CONFIRM, SCR_INTRO, SCR_CLEAR,
       CW_INTRO, CW_CLEAR, PP_SERVE };
static uint32_t installed=LN_GAME_SUPPORTED_MASK;
#if defined(LN_GAME_PACKAGE_ID)
static const uint8_t game=LN_GAME_PACKAGE_ID;
#else
static uint8_t game;
#endif
static uint8_t games_phase,paused_phase,hearts,focus,sequence[24],entered,level,flash;
static uint16_t ship=160,energy,score,best[LN_GAME_COUNT];
static uint16_t cw_award;
static uint32_t random_state=0x9e3779b9u,last_tick,spawn_at,flash_until,invulnerable_until,hud_revision,energy_carry;
static uint32_t buttons;
static uint8_t sound_pending;
static bool resume_pending;
static uint32_t hit_until,shield_notice_until;
static bool shield_failed;
static uint16_t previous_best;
static uint32_t burst_until;
static int16_t burst_x,burst_y;
static bool burst_shield;
static struct {int16_t x,y;uint8_t size,live;uint32_t fraction;} rock[12];
static const char *const names[LN_GAME_COUNT]={"Comet Run","Pulse Path","Solitaire","Sudoku","Brick","Music Quiz",
    "Vortex","Minesweeper","Falling Blocks","Reversi","Snake","Word Search","Scrub",
    "Dial Heist","Mixtape","Signal","Clockwork City","Pocket Pinball"};
static const char *const descriptions[LN_GAME_COUNT]={"Thread through the meteor field.","Remember the lights. Find the path.",
    "Build four suits from Ace to King.","Classic 9 by 9 number puzzle.",
    "Break six handcrafted walls.","Name songs from your music library.","Orbit the core and clear each wave.",
    "Clear safe tiles at three difficulties.","Build lines with seven classic pieces.",
    "Outflank a thoughtful opponent.","Turn, boost, and grow.","Find every hidden word.",
    "Pin one echo. Move the rest of time.","Crack three tumblers before patrol arrives.",
    "Place loops on a rotating eight-beat map.","Tune and decode hidden transmissions.",
    "Rotate street rings to open a route.","Tilt the table and kick for a high score."};
static uint32_t random_next(void);

/* Draw-one Klondike. Piles use their last element as the top card. A tableau
 * source index can name any face-up card, moving that card and everything above. */
static uint8_t sol_stock[52],sol_waste[52],sol_tableau[7][52];
static uint8_t sol_stock_count,sol_waste_count,sol_count[7],sol_hidden[7],sol_foundation[4];
static uint8_t sol_cursor,sol_depth,sol_source=255u,sol_source_depth;
static uint16_t sol_moves;
static uint8_t sol_menu,sol_history_head,sol_history_count;
static const char *sol_message;
static struct sol_snapshot {
    uint8_t cards[52],stock_count,waste_count,count[7],hidden[7],foundation[4],cursor,depth;
    uint16_t moves;
} sol_history[16];
static void sol_remember(void){
    struct sol_snapshot *s=&sol_history[sol_history_head];
    uint8_t at=0u;
    for(uint8_t i=0;i<sol_stock_count;++i)s->cards[at++]=sol_stock[i];
    for(uint8_t i=0;i<sol_waste_count;++i)s->cards[at++]=sol_waste[i];
    for(uint8_t col=0;col<7u;++col)
        for(uint8_t i=0;i<sol_count[col];++i)s->cards[at++]=sol_tableau[col][i];
    for(uint8_t i=0;i<7u;++i){s->count[i]=sol_count[i];s->hidden[i]=sol_hidden[i];}
    for(uint8_t i=0;i<4u;++i)s->foundation[i]=sol_foundation[i];
    s->stock_count=sol_stock_count;s->waste_count=sol_waste_count;s->cursor=sol_cursor;s->depth=sol_depth;s->moves=sol_moves;
    sol_history_head=(sol_history_head+1u)&15u;if(sol_history_count<16u)++sol_history_count;
}
static void sol_undo(void){
    if(!sol_history_count){sol_message="No moves to undo yet.";sound_pending=3u;return;}
    sol_history_head=(sol_history_head+15u)&15u;--sol_history_count;
    const struct sol_snapshot *s=&sol_history[sol_history_head];
    uint8_t at=0u;
    for(uint8_t i=0;i<s->stock_count;++i)sol_stock[i]=s->cards[at++];
    for(uint8_t i=0;i<s->waste_count;++i)sol_waste[i]=s->cards[at++];
    for(uint8_t col=0;col<7u;++col)
        for(uint8_t i=0;i<s->count[col];++i)sol_tableau[col][i]=s->cards[at++];
    for(uint8_t i=0;i<7u;++i){sol_count[i]=s->count[i];sol_hidden[i]=s->hidden[i];}
    for(uint8_t i=0;i<4u;++i)sol_foundation[i]=s->foundation[i];
    sol_stock_count=s->stock_count;sol_waste_count=s->waste_count;sol_cursor=s->cursor;sol_depth=s->depth;sol_moves=s->moves;
    sol_source=255u;sol_message="Move undone.";sound_pending=2u;games_phase=PLAY;++hud_revision;
}
static uint8_t sol_rank(uint8_t card){return (uint8_t)(card%13u+1u);}
static uint8_t sol_suit(uint8_t card){return card/13u;}
static bool sol_red(uint8_t card){uint8_t suit=sol_suit(card);return suit==1u||suit==2u;}
static void sol_focus(uint8_t spot){
    sol_cursor=spot;
    if(spot>=6u){uint8_t col=(uint8_t)(spot-6u);sol_depth=sol_count[col]?sol_count[col]-1u:0u;}
}
static void sol_new(void){
    uint8_t deck[52];for(uint8_t i=0;i<52u;++i)deck[i]=i;
    for(uint32_t i=51u;i;i--){uint32_t j=random_next()%(i+1u);uint8_t swap=deck[i];deck[i]=deck[j];deck[j]=swap;}
    uint32_t next=0;for(uint8_t col=0;col<7u;++col){
        sol_count[col]=col+1u;sol_hidden[col]=col;
        for(uint8_t at=0;at<=col;++at)sol_tableau[col][at]=deck[next++];
    }
    sol_stock_count=24u;sol_waste_count=0u;
    for(uint8_t i=0;i<24u;++i)sol_stock[i]=deck[next++];
    for(uint8_t i=0;i<4u;++i)sol_foundation[i]=0u;
    sol_source=255u;sol_source_depth=0u;sol_moves=0u;sol_focus(0u);
    sol_menu=sol_history_head=sol_history_count=0u;sol_message=0;
}
static void sol_win(void){
    for(uint8_t i=0;i<4u;++i)if(sol_foundation[i]!=13u)return;
    games_phase=WON;sound_pending=4u;
    if(best[SOLITAIRE]!=UINT16_MAX){++best[SOLITAIRE];
#if defined(LINNET_STORAGE)
        ln_display_game_best_set(SOLITAIRE,best[SOLITAIRE]);
#endif
    }
}
static bool sol_draw(void){
    if(sol_stock_count){sol_remember();sol_waste[sol_waste_count++]=sol_stock[--sol_stock_count];if(sol_moves<UINT16_MAX)++sol_moves;sound_pending=2u;return true;}
    if(!sol_waste_count)return false;
    sol_remember();
    while(sol_waste_count)sol_stock[sol_stock_count++]=sol_waste[--sol_waste_count];
    if(sol_moves<UINT16_MAX)++sol_moves;sol_message="Waste recycled. Draw again.";sound_pending=2u;return true;
}
static bool sol_select_source(void){
    if(sol_cursor==1u&&!sol_waste_count)return false;
    if(sol_cursor>=2u&&sol_cursor<6u&&!sol_foundation[sol_cursor-2u])return false;
    if(sol_cursor>=6u){uint8_t col=(uint8_t)(sol_cursor-6u);
        if(!sol_count[col]||sol_depth<sol_hidden[col]||sol_depth>=sol_count[col])return false;
    }
    sol_source=sol_cursor;sol_source_depth=sol_depth;sound_pending=1u;return true;
}
static uint8_t sol_visible_card(uint8_t spot,uint8_t depth){
    if(spot==1u&&sol_waste_count)return sol_waste[sol_waste_count-1u];
    if(spot>=2u&&spot<6u&&sol_foundation[spot-2u])return (uint8_t)((spot-2u)*13u+sol_foundation[spot-2u]-1u);
    if(spot>=6u&&spot<13u&&depth<sol_count[spot-6u]&&depth>=sol_hidden[spot-6u])return sol_tableau[spot-6u][depth];
    return 255u;
}
static bool sol_can_place(uint8_t dest){
    if(sol_source==255u||dest==sol_source||dest<2u||dest>=13u)return false;
    uint8_t card=sol_visible_card(sol_source,sol_source_depth);if(card==255u)return false;
    uint8_t count=sol_source>=6u?sol_count[sol_source-6u]-sol_source_depth:1u;
    if(dest<6u)return count==1u&&sol_suit(card)==dest-2u&&sol_rank(card)==sol_foundation[dest-2u]+1u;
    uint8_t col=dest-6u,length=sol_count[col];if(length+count>52u)return false;
    if(!length)return sol_rank(card)==13u;
    uint8_t top=sol_tableau[col][length-1u];return sol_red(top)!=sol_red(card)&&sol_rank(top)==sol_rank(card)+1u;
}
static bool sol_move(void){
    uint8_t source=sol_source,dest=sol_cursor,first,count,card;
    if(!sol_can_place(dest)){
        sol_message=dest<2u?"Choose a column or a foundation.":dest<6u?"Build each suit from Ace to King.":
            !sol_count[dest-6u]?"Only Kings fit an empty column.":"Alternate colors, one rank lower.";return false;}
    if(source==1u){first=(uint8_t)(sol_waste_count-1u);count=1u;card=sol_waste[first];}
    else if(source<6u){first=0u;count=1u;card=(uint8_t)((source-2u)*13u+sol_foundation[source-2u]-1u);}
    else {uint8_t col=(uint8_t)(source-6u);first=sol_source_depth;
        count=(uint8_t)(sol_count[col]-first);card=sol_tableau[col][first];}
    sol_remember();
    if(dest<6u)++sol_foundation[dest-2u];
    else {uint8_t col=(uint8_t)(dest-6u);
        for(uint8_t i=0;i<count;++i)sol_tableau[col][sol_count[col]++]=source==1u?card:
            source<6u?card:sol_tableau[source-6u][first+i];
    }
    if(source==1u)--sol_waste_count;
    else if(source<6u)--sol_foundation[source-2u];
    else {uint8_t col=(uint8_t)(source-6u);sol_count[col]=first;
        if(sol_count[col]&&sol_hidden[col]==sol_count[col]){--sol_hidden[col];sound_pending=5u;}
        else sound_pending=2u;
    }
    if(sol_moves<UINT16_MAX)++sol_moves;sol_source=255u;sol_focus(dest);sol_message="Card placed.";sol_win();return true;
}

#include "services/games/sudoku.h"
static void cw_add_score(uint16_t points){
    cw_award=points;
    score=(uint16_t)(points>UINT16_MAX-score?UINT16_MAX:score+points);
}
#include "services/games/modules/scrub.h"

static uint32_t random_next(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static __attribute__((noinline)) void number(char **out,uint32_t value){char reverse[10];uint32_t n=0;do{reverse[n++]=(char)('0'+value%10u);value/=10u;}while(value);while(n)*(*out)++=reverse[--n];}
static __attribute__((noinline)) void append(char **out,const char *text){while(*text)*(*out)++=*text++;}
#include "services/games/games_extra.h"
static void cw_record(uint8_t index,uint16_t value){
    if(value<=best[index])return;
    best[index]=value;
#if defined(LINNET_STORAGE)
    ln_display_game_best_set(index,value);
#endif
}
#include "services/games/modules/dial_heist.h"
#include "services/games/modules/mixtape.h"
#include "services/games/modules/signal.h"
#include "services/games/modules/clockwork_city.h"
#include "services/games/modules/pocket_pinball.h"
static void cw_start(void){
    if(game==DIAL_HEIST)dh_start();else if(game==MIXTAPE)mt_start();
    else if(game==SIGNAL)sg_start();else if(game==CLOCKWORK_CITY)cc_start();
    else pp_start();
}
static int cw_input(uint32_t kind,int32_t value){
    if(kind==1u){
        if(game==DIAL_HEIST)return dh_wheel(value);
        if(game==MIXTAPE)return mt_wheel(value);
        if(game==SIGNAL)return sg_wheel(value);
        if(game==CLOCKWORK_CITY)return cc_wheel(value);
        return pp_wheel(value);
    }
    if(kind!=2u)return 0;
    uint32_t pressed=(uint32_t)value&~buttons;buttons=(uint32_t)value;
    if(!pressed)return 3;
    if(pressed&1u){if(game==POCKET_PINBALL)cw_record(POCKET_PINBALL,score);return 2;}
    if(game==DIAL_HEIST)return dh_button(pressed);
    if(game==MIXTAPE)return mt_button(pressed);
    if(game==SIGNAL)return sg_button(pressed);
    if(game==CLOCKWORK_CITY)return cc_button(pressed);
    return pp_button(pressed);
}
static uint32_t cw_deadline(uint32_t now){
    if(game==DIAL_HEIST)return dh_deadline(now);
    if(game==MIXTAPE)return mt_deadline(now);
    if(game==SIGNAL)return sg_deadline(now);
    if(game==CLOCKWORK_CITY)return cc_deadline(now);
    return pp_deadline(now);
}
static bool cw_tick(uint32_t now){
    if(game==DIAL_HEIST)return dh_tick(now);
    if(game==MIXTAPE)return mt_tick(now);
    if(game==SIGNAL)return sg_tick(now);
    if(game==CLOCKWORK_CITY)return cc_tick(now);
    return pp_tick(now);
}
uint32_t ln_games_count(void){uint32_t count=0;for(uint32_t i=0;i<LN_GAME_COUNT;++i)if(installed&(1u<<i))++count;return count;}
const char *ln_games_label(uint32_t row){for(uint32_t i=0;i<LN_GAME_COUNT;++i)if(installed&(1u<<i)){if(!row)return names[i];--row;}return 0;}
const char *ln_games_description(uint32_t row){
    for(uint32_t i=0;i<LN_GAME_COUNT;++i)if(installed&(1u<<i)){
        if(!row)return descriptions[i];
        --row;
    }
    return "Choose games in Linnet Media.";
}
void ln_games_configure(uint32_t mask){installed=mask&LN_GAME_SUPPORTED_MASK;}
void ln_games_best_set(uint32_t index,uint16_t value){if(index<LN_GAME_COUNT)best[index]=value;}
bool ln_games_playing(void){
    if(game>=DIAL_HEIST)return games_phase==PLAY;
    if(game==SCRUB)return games_phase==PLAY;
    if(game>=BRICK)return ex_phase==EX_PLAY;
    return game!=SOLITAIRE&&(games_phase==PLAY||games_phase==WATCH||games_phase==MISTAKE);
}
uint32_t ln_games_hud_revision(void){return hud_revision;}
uint32_t ln_games_deadline(uint32_t now){
    if(game>=DIAL_HEIST)return cw_deadline(now);
    if(game==SCRUB)return UINT32_MAX;
    if(game>=BRICK)return extra_deadline(now);
    if(game==SUDOKU&&(games_phase==PLAY||games_phase==SUDOKU_PICK))return now+(1000000u-sud_clock_remainder);
    if(!ln_games_playing())return UINT32_MAX;
    if(!last_tick)return now;
    if(game==COMET)return (int32_t)(last_tick+33333u-now)>0?last_tick+33333u:now;
    if(games_phase==WATCH||games_phase==MISTAKE||flash)
        return (int32_t)(flash_until-now)>0?flash_until:now;
    return UINT32_MAX;
}
static void finish(bool won);
static void new_pulse(void){
    if(level>=24u){finish(true);return;}
    sequence[level]=(uint8_t)(random_next()&3u);++level;entered=0;
    flash=0;entered=255u;flash_until=last_tick+700000u;games_phase=WATCH;
}
void ln_games_start(uint32_t row){
#if !defined(LN_GAME_PACKAGE_ID)
    for(uint32_t i=0;i<LN_GAME_COUNT;++i)if(installed&(1u<<i)){if(!row){game=(uint8_t)i;break;}--row;}
#else
    (void)row;
#endif
    random_state^=last_tick+0x6d2b79f5u;games_phase=paused_phase=PLAY;score=0;hearts=3u;focus=0u;sound_pending=1u;
    resume_pending=false;
    hit_until=shield_notice_until=burst_until=0;shield_failed=false;previous_best=best[game];
    if(game>=DIAL_HEIST){cw_start();last_tick=0;++hud_revision;return;}
    if(game==SCRUB){scr_start();last_tick=0;return;}
    if(game>=BRICK){extra_start();last_tick=0;return;}
    if(game==COMET){ship=160u;energy=100u;energy_carry=0;spawn_at=last_tick+600000u;invulnerable_until=last_tick+1200000u;for(uint32_t i=0;i<12u;++i)rock[i].live=0;}
    else if(game==PULSE){level=0u;new_pulse();}
    else if(game==SOLITAIRE)sol_new();
    else{games_phase=SUDOKU_LEVEL;sud_choice=1u;sud_message=0;sud_clock_at=0;}
    if(game==COMET||game==PULSE)games_phase=ARCADE_READY;
    last_tick=0;++hud_revision;
}
static void finish(bool won){games_phase=won?WON:LOST;sound_pending=4u;if(score>best[game]){
        best[game]=score;
#if defined(LINNET_STORAGE)
        ln_display_game_best_set(game,score);
#endif
    }}
int ln_games_input(uint32_t kind,int32_t value){
    if(game>=DIAL_HEIST)return cw_input(kind,value);
    if(game==SCRUB)return scr_input(kind,value);
    if(game>=BRICK){if(kind==1u)return extra_wheel(value);if(kind!=2u)return 0;uint32_t pressed=(uint32_t)value&~buttons;buttons=(uint32_t)value;return extra_button(pressed);}
    if(kind==1u){if(!value)return 3;
        if(game==SUDOKU){
            uint8_t choice=sud_choice,picker=sud_picker,menu=sud_menu,cursor=sud_cursor;
            if(games_phase==SUDOKU_LEVEL){sud_choice=(uint8_t)((sud_choice+value%4+4)%4);}
            else if(games_phase==SUDOKU_PICK){sud_picker=(uint8_t)((sud_picker+value%10+10)%10);}
            else if(games_phase==SUDOKU_MENU){sud_menu=(uint8_t)((sud_menu+value%6+6)%6);}
            else if(games_phase==PLAY){int32_t next=(int32_t)sud_cursor+value%81;while(next<0)next+=81;while(next>=81)next-=81;sud_cursor=(uint8_t)next;}
            bool changed=choice!=sud_choice||picker!=sud_picker||menu!=sud_menu||cursor!=sud_cursor;
            if(changed){if(games_phase!=SUDOKU_MENU)sud_message=0;++hud_revision;}return changed?1:3;
        }
        if(game==SOLITAIRE&&games_phase==PAUSED){uint8_t next=(uint8_t)((sol_menu+value%3+3)%3);
            if(next==sol_menu)return 3;sol_menu=next;sol_message=0;return 1;}
        if(games_phase==PLAY&&game==SOLITAIRE){
            int32_t next=(int32_t)sol_cursor+value%13;
            while(next<0)next+=13;while(next>=13)next-=13;
            if((uint8_t)next==sol_cursor)return 3;
            sol_focus((uint8_t)next);sol_message=0;++hud_revision;return 1;
        }if(games_phase==PLAY&&game==COMET){
            int32_t steps=value<-24?-24:value>24?24:value;
            int32_t next=(int32_t)ship+steps*6;if(next<18)next=18;if(next>302)next=302;
            if(next==ship)return 3;ship=(uint16_t)next;return 1;}
        else if(games_phase==PLAY&&game==PULSE){uint8_t next=(uint8_t)((focus+value%4+4)&3u);
            if(next==focus)return 3;focus=next;return 1;}return 3;}
    if(kind!=2u)return 0;
    uint32_t pressed=(uint32_t)value&~buttons;buttons=(uint32_t)value;
    if(game==SUDOKU){
        if(games_phase==SUDOKU_LEVEL){
            if(pressed&1u)return 2;
            if(pressed&4u)sud_choice=(sud_choice+1u)&3u;if(pressed&8u)sud_choice=(sud_choice+3u)&3u;
            if(pressed&16u)sud_begin(sud_choice);if(pressed)++hud_revision;return 1;
        }
        if(games_phase==SUDOKU_PICK){
            if(pressed&1u){games_phase=PLAY;sud_message=0;}
            if(pressed&4u){sud_picker=(sud_picker+1u)%10u;sud_message=0;}if(pressed&8u){sud_picker=(sud_picker+9u)%10u;sud_message=0;}
            if(pressed&2u){sud_pencil=!sud_pencil;sud_message=sud_pencil?"Pencil marks on.":"Pencil marks off.";}
            if(pressed&16u)sud_place();if(pressed)++hud_revision;return 1;
        }
        if(games_phase==SUDOKU_MENU){
            if(pressed&(1u|2u)){games_phase=PLAY;sud_message=0;sud_clock_at=0;}
            if(pressed&4u)sud_menu=(sud_menu+1u)%6u;if(pressed&8u)sud_menu=(sud_menu+5u)%6u;
            if(pressed&16u){
                if(!sud_menu){games_phase=PLAY;sud_message=0;sud_clock_at=0;}
                else if(sud_menu==1u){games_phase=PLAY;sud_clock_at=0;sud_hint();}
                else if(sud_menu==2u){games_phase=PLAY;sud_clock_at=0;sud_message="Everything entered is correct.";sound_pending=2u;}
                else{sud_confirm=(uint8_t)(sud_menu-2u);games_phase=SUDOKU_CONFIRM;}
            }
            if(pressed)++hud_revision;return 1;
        }
        if(games_phase==SUDOKU_CONFIRM){
            if(pressed&(1u|2u)){games_phase=SUDOKU_MENU;sud_confirm=0;}
            if(pressed&16u){uint8_t action=sud_confirm;sud_confirm=0;
                if(action==1u)sud_restart();else if(action==2u){games_phase=SUDOKU_LEVEL;sud_choice=sud_level;}
                else return 2;}
            if(pressed)++hud_revision;return 1;
        }
        if(games_phase==WON){if(pressed&1u)return 2;if(pressed&16u)sud_begin(sud_level);return 1;}
        if(games_phase==PLAY){
            if(pressed&1u){games_phase=SUDOKU_MENU;sud_menu=0;sud_message=0;sud_clock_at=0;}
            else if(pressed&2u){sud_pencil=!sud_pencil;sud_message=sud_pencil?"Pencil marks on.":"Pencil marks off.";}
            else if(pressed&4u){sud_cursor=(uint8_t)((sud_cursor+1u)%81u);sud_message=0;}
            else if(pressed&8u){sud_cursor=(uint8_t)((sud_cursor+80u)%81u);sud_message=0;}
            else if(pressed&16u){if(sud_given[sud_cursor]){sud_message="This clue is fixed.";sound_pending=3u;}
                else{sud_picker=sud_board[sud_cursor];if(!sud_picker)sud_picker=1u;games_phase=SUDOKU_PICK;sud_message=0;}}
            if(pressed)++hud_revision;return 1;
        }
        return 1;
    }
    if(game==SOLITAIRE&&games_phase==SOL_CONFIRM){
        if(pressed&(1u|2u)){games_phase=PAUSED;return 1;}
        if(pressed&16u){sol_new();games_phase=PLAY;++hud_revision;}return 1;}
    if(game==SOLITAIRE&&games_phase==PAUSED){
        if(pressed&(1u|2u)){games_phase=PLAY;return 1;}
        if(pressed&4u)sol_menu=(sol_menu+1u)%3u;
        if(pressed&8u)sol_menu=(sol_menu+2u)%3u;
        if(pressed&16u){if(!sol_menu)games_phase=PLAY;else if(sol_menu==1u)sol_undo();else games_phase=SOL_CONFIRM;}
        return 1;}
    if(game==SOLITAIRE&&games_phase==PLAY&&sol_source!=255u&&(pressed&1u)){
        sol_source=255u;sol_message="Pickup cancelled.";return 1;}
    if(pressed&1u)return 2;
    if(games_phase==ARCADE_READY){if(pressed&16u){games_phase=game==COMET?PLAY:WATCH;last_tick=0;++hud_revision;}return 1;}
    if(pressed&2u){if(games_phase==PLAY||games_phase==WATCH||games_phase==MISTAKE){paused_phase=games_phase;games_phase=PAUSED;sol_menu=0;sol_message=0;}
        else if(games_phase==PAUSED){games_phase=paused_phase;resume_pending=true;}return 1;}
    if((games_phase==LOST||games_phase==WON)&&(pressed&16u)){
        uint32_t row=0;for(uint32_t i=0;i<game;++i)if(installed&(1u<<i))++row;
        ln_games_start(row);if(game!=SOLITAIRE)games_phase=game==COMET?PLAY:WATCH;return 1;}
    if(games_phase!=PLAY)return 1;
    if(game==SOLITAIRE){
        if(pressed)sol_message=0;
        if(sol_cursor>=6u&&sol_source==255u){uint8_t col=(uint8_t)(sol_cursor-6u);
            if((pressed&8u)&&sol_depth>sol_hidden[col]){--sol_depth;++hud_revision;}
            if((pressed&4u)&&sol_depth+1u<sol_count[col]){++sol_depth;++hud_revision;}
        }
        if(pressed&16u){bool changed=false;
            if(sol_source==255u)changed=sol_cursor==0u?sol_draw():sol_select_source();
            else if(sol_source==sol_cursor){sol_source=255u;changed=true;}
            else changed=sol_move();
            if(changed)++hud_revision;else{sound_pending=3u;if(!sol_message)sol_message="No face-up card here.";}
        }
    }else if(game==COMET){
        if(pressed&4u)ship=ship<286u?ship+16u:302u;
        if(pressed&8u)ship=ship>34u?ship-16u:18u;
        if((pressed&16u)&&energy>=40u){energy-=40u;
            shield_failed=false;shield_notice_until=last_tick+800000u;
            uint32_t shield_until=last_tick+800000u;
            if((int32_t)(shield_until-invulnerable_until)>0)invulnerable_until=shield_until;
            sound_pending=2u;}
        else if(pressed&16u){shield_failed=true;shield_notice_until=last_tick+600000u;sound_pending=3u;}
    }else{
        if(pressed&4u)focus=(focus+1u)&3u;
        if(pressed&8u)focus=(focus+3u)&3u;
        if((pressed&16u)&&entered<level){
            flash=focus+1u;flash_until=last_tick+230000u;
            if(focus!=sequence[entered]){
                sound_pending=3u;if(hearts)--hearts;++hud_revision;
                if(!hearts)finish(false);
                else{entered=0;games_phase=MISTAKE;flash_until=last_tick+450000u;}
            }else {sound_pending=(uint8_t)(5u+focus);++entered;++hud_revision;}
        }
    }
    return 1;
}
bool ln_games_tick(uint32_t now){
    if(!games_phase){last_tick=now;return false;}
    if(game>=DIAL_HEIST)return cw_tick(now);
    if(game==SCRUB)return false;
    if(game>=BRICK)return extra_tick(now);
    if(game==SUDOKU)return sud_tick(now);
    if(game==SOLITAIRE)return false;
    uint16_t old_score=score;uint8_t old_hearts=hearts,old_phase=games_phase;
    if(games_phase==PAUSED||games_phase==ARCADE_READY)return false;
    if(resume_pending){resume_pending=false;
        if(last_tick){uint32_t paused_for=now-last_tick;
            if(game==COMET){spawn_at+=paused_for;invulnerable_until+=paused_for;
                if(hit_until)hit_until+=paused_for;if(shield_notice_until)shield_notice_until+=paused_for;
                if(burst_until)burst_until+=paused_for;}
            else flash_until+=paused_for;
            last_tick=now;return false;
        }
    }
    if(!last_tick){last_tick=now;
        if(game==COMET){spawn_at=now+600000u;invulnerable_until=now+1200000u;}
        else if(flash||games_phase==WATCH)flash_until=now+(games_phase==MISTAKE?450000u:games_phase==WATCH?700000u:230000u);
        return false;}
    if(game==PULSE){
        last_tick=now;
        if(games_phase==MISTAKE&&(int32_t)(now-flash_until)>=0){
            flash=sequence[0]+1u;entered=0;games_phase=WATCH;flash_until=now+500000u;
            sound_pending=(uint8_t)(5u+sequence[0]);++hud_revision;return true;
        }
        if(games_phase==WATCH&&(int32_t)(now-flash_until)>=0){
            if(flash){flash=0;flash_until=now+170000u;}
            else if(entered==255u||entered+1u<level){entered=entered==255u?0u:(uint8_t)(entered+1u);flash=sequence[entered]+1u;flash_until=now+500000u;
                sound_pending=(uint8_t)(5u+sequence[entered]);}
            else{entered=0;games_phase=PLAY;++hud_revision;}
            return true;
        }
        if(games_phase==PLAY&&flash&&(int32_t)(now-flash_until)>=0){flash=0;
            if(entered==level){score+=level*10u;new_pulse();++hud_revision;}return true;}
        return false;
    }
    uint32_t elapsed=now-last_tick;if(elapsed<33333u)return false;if(elapsed>100000u)elapsed=100000u;last_tick=now;
    if(games_phase!=PLAY)return false;
    if(energy<100u){uint32_t charge=energy_carry+elapsed*25u;
        energy+=(uint16_t)(charge/1000000u);energy_carry=charge%1000000u;
        if(energy>100u)energy=100u;}
    if((int32_t)(now-spawn_at)>=0){
        for(uint32_t i=0;i<12u;++i)if(!rock[i].live){rock[i].live=1u;rock[i].x=(int16_t)(20u+random_next()%280u);
            rock[i].y=55;rock[i].fraction=0;rock[i].size=(uint8_t)(5u+random_next()%9u);break;}
        spawn_at=now+((score<80u?600u:score<200u?430u:300u)*1000u);
    }
    for(uint32_t i=0;i<12u;++i)if(rock[i].live){
        uint32_t speed=120u+(score<280u?score/2u:140u);
        uint32_t movement=rock[i].fraction+elapsed*speed;
        rock[i].y+=(int16_t)(movement/1000000u);rock[i].fraction=movement%1000000u;
        int32_t x=rock[i].x,y=rock[i].y,r=rock[i].size;
        if(y-r-2>203){rock[i].live=0u;if(score<65530u)score+=5u;}
        else if(x+r+2>(int32_t)ship-11&&x-r-2<(int32_t)ship+11&&
                y+r+2>167&&y-r-2<201){
            rock[i].live=0u;burst_x=(int16_t)x;burst_y=(int16_t)y;burst_until=now+350000u;
            burst_shield=(int32_t)(now-invulnerable_until)<0;
            if(burst_shield){if(score<65530u)score+=5u;sound_pending=2u;continue;}
            if(hearts)--hearts;sound_pending=3u;
            hit_until=now+350000u;invulnerable_until=now+1200000u;if(!hearts){finish(false);break;}}
    }
    if(score!=old_score||hearts!=old_hearts||games_phase!=old_phase)++hud_revision;
    return true;
}
const char *ln_games_text(const char *key){(void)key;return "";}
static __attribute__((noinline)) void box(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t w,int32_t h,uint16_t ink){
    int32_t left=x>c->clip_x?x:c->clip_x,top=y>c->clip_y?y:c->clip_y;
    int32_t right=x+w<c->clip_x+c->clip_w?x+w:c->clip_x+c->clip_w;
    int32_t bottom=y+h<c->clip_y+c->clip_h?y+h:c->clip_y+c->clip_h;
    if(left<0)left=0;if(top<0)top=0;if(right>(int32_t)LINNET_UI_WIDTH)right=LINNET_UI_WIDTH;if(bottom>(int32_t)LINNET_UI_HEIGHT)bottom=LINNET_UI_HEIGHT;
    for(int32_t yy=top;yy<bottom;++yy)for(int32_t xx=left;xx<right;++xx)c->pixels[yy*LINNET_UI_WIDTH+xx]=ink;
}
#include "services/games/drawing.h"
#include "services/games/presentations/games_scrub.h"
#if LINNET_GAME_PRESENTATION_COLOR320
#include "services/games/presentations/collection_ui.h"
#endif
#if LINNET_GAME_PRESENTATION_PHOTO220
#include "services/games/presentations/photo220/games_graphics.h"
#elif LINNET_GAME_PRESENTATION_COLOR320
#include "services/games/presentations/color320/games_arcade.h"
static void sol_glyph(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,char ch,uint16_t ink){
    char text[2]={ch,0};arcade_text(c,x,y-2,text,ink,true);
}
static void sol_outline(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t h,uint16_t ink){
    box(c,x-2,y-2,42,2,ink);box(c,x-2,y+h,42,2,ink);
    box(c,x-2,y,2,h,ink);box(c,x+38,y,2,h,ink);
}
static void sol_suit_mark(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint8_t suit,uint8_t scale,uint16_t ink){
    static const uint8_t bits[4][7]={
        {8,28,8,119,127,28,8},
        {8,28,62,127,62,28,8},
        {54,127,127,127,62,28,8},
        {8,28,62,127,119,8,28}
    };
    for(uint8_t row=0;row<7u;++row)for(uint8_t col=0;col<7u;++col)
        if(bits[suit][row]&(64u>>col))box(c,x+col*scale,y+row*scale,scale,scale,ink);
}
static void sol_small_suit(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint8_t suit,uint16_t ink){
    static const uint8_t bits[4][5]={
        {4,14,21,14,4},{4,14,31,14,4},
        {10,31,31,14,4},{4,14,31,21,4}
    };
    for(uint8_t row=0;row<5u;++row)for(uint8_t col=0;col<5u;++col)
        if(bits[suit][row]&(16u>>col))box(c,x+col,y+row,1,1,ink);
}
static void sol_court_card(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint8_t rank,uint8_t suit){
    const uint8_t *pixels=sol_court_pixels[suit*3u+rank-11u];
    for(int32_t row=0;row<50;++row){
        int32_t yy=y+row;
        if(yy<0||yy>=240||yy<c->clip_y||yy>=c->clip_y+c->clip_h)continue;
        for(int32_t col=0;col<38;++col){
            int32_t xx=x+col;
            if(xx<0||xx>=320||xx<c->clip_x||xx>=c->clip_x+c->clip_w)continue;
            c->pixels[yy*320+xx]=sol_court_palette[pixels[row*38+col]];
        }
    }
}
static void sol_card(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint8_t card,bool back,int32_t height){
    struct ln_ui_canvas_context clipped=*c;
    int32_t bottom=y+height;if(bottom>c->clip_y+c->clip_h)bottom=c->clip_y+c->clip_h;
    clipped.clip_h=bottom>clipped.clip_y?bottom-clipped.clip_y:0;c=&clipped;
    box(c,x+1,y+1,38,height,0x0985u);
    if(!back&&height>=50&&sol_rank(card)>=11u){sol_court_card(c,x,y,sol_rank(card),sol_suit(card));return;}
    arcade_panel(c,x,y,38,height,back?0xe73cu:0xc618u);
    box(c,x+1,y+1,36,height>2?height-2:1,back?0x2254u:0xffffu);
    if(back){
        box(c,x+3,y+3,32,height>6?height-6:1,0x8d37u);box(c,x+4,y+4,30,height>8?height-8:1,0x1a0fu);
        for(int32_t dy=6;dy<height-5;dy+=3)for(int32_t dx=6+(dy%2);dx<33;dx+=4)box(c,x+dx,y+dy,1,1,0x6474u);
        if(height>=50){for(int32_t dy=-11;dy<=11;++dy){int32_t half=11-(dy<0?-dy:dy);
            box(c,x+19-half,y+25+dy,half*2+1,1,0x8d37u);
            if(half>2)box(c,x+21-half,y+25+dy,half*2-3,1,0x1a0fu);}
            box(c,x+17,y+23,5,5,0xded9u);}
        return;
    }
    uint16_t ink=sol_red(card)?0xd0c3u:0x18a7u;
    uint8_t rank=sol_rank(card),suit=sol_suit(card);
    if(rank==10u){sol_glyph(c,x+3,y+3,'1',ink);sol_glyph(c,x+9,y+3,'0',ink);}
    else sol_glyph(c,x+3,y+3,"A23456789TJQK"[rank-1u],ink);
    if(height>=9)sol_small_suit(c,x+(rank==10u?17:11),y+4,suit,ink);
    if(height<30)return;
    if(rank==1u)sol_suit_mark(c,x+12,y+24,suit,2u,ink);
    else if(rank>=11u)sol_suit_mark(c,x+12,y+24,suit,2u,ink);
    else {static const uint8_t slot_x[]={15,15,15,6,25,6,25,6,25,15,15};
        static const uint8_t slot_y[]={16,26,37,15,15,26,26,37,37,22,32};
        static const uint8_t layout[9][10]={
            {0,2,255},{0,1,2,255},{3,4,7,8,255},{3,4,1,7,8,255},
            {3,4,5,6,7,8,255},{3,4,5,6,7,8,0,255},
            {3,4,5,6,7,8,0,2,255},{3,4,5,6,7,8,0,1,2,255},
            {3,4,5,6,7,8,0,9,10,2}
        };
        for(uint8_t i=0;i<10u&&layout[rank-2u][i]!=255u;++i){uint8_t slot=layout[rank-2u][i];
            sol_small_suit(c,x+slot_x[slot],y+slot_y[slot],suit,ink);}
    }
}
#include "services/games/presentations/color320/games_solitaire.h"
#include "services/games/presentations/color320/games_sudoku.h"
#include "services/games/presentations/games_extra_draw.h"
void ln_games_draw(const struct ln_ui_canvas_context *c){
#if LINNET_GAME_PRESENTATION_COLOR320
    if(game>=DIAL_HEIST){cw_draw(c);return;}
    if(game==SCRUB){scr_draw(c);return;}
#endif
    if(game>=BRICK){extra_draw(c);return;}
    if(game==SUDOKU){sud_draw_320(c);return;}
    if(game==SOLITAIRE){sol_draw_scene(c);return;}
    arcade_draw(c);
}

#endif /* native display presentation */

#if defined(LINNET_AUDIO) && !defined(LN_GAME_PACKAGE_ID)
static bool games_audio_owned;
void ln_games_audio_stop(void){if(games_audio_owned){(void)ln_audio_stop();games_audio_owned=false;}sound_pending=0;}
void ln_games_audio_step(void){
    /* A busy player owns audio even if it is still priming. A selection in
     * progress reserves it across the otherwise-idle gap between tracks. */
    if(ln_player_busy()){games_audio_owned=false;sound_pending=0;return;}
    if(ln_music_loading()){ln_games_audio_stop();return;}
    struct ln_audio_status state;ln_audio_snapshot(&state);
    if(games_audio_owned&&!state.playing&&!sound_pending){ln_games_audio_stop();return;}
    /* Music retains the audio device during games, matching stock iPod
     * playback. Drop the short cue instead of interrupting the current track. */
    if(!sound_pending)return;
    if(games_audio_owned&&state.playing)return;
    uint8_t cue=sound_pending;sound_pending=0;
    if(!ln_audio_init(22050u))return;
    games_audio_owned=true;
    int16_t pcm[512];uint32_t phase_value=0;
    uint32_t step=cue>=5u&&cue<=8u?0x04000000u+(cue-5u)*0x01000000u:
        cue==3u?0x03000000u:cue==4u?0x02000000u:cue==2u?0x08000000u:0x06000000u;
    for(uint32_t chunk=0;chunk<(cue==9u?16u:4u);++chunk){
        if(cue==9u)step=0x04000000u+(uint32_t)mt_goal[mt_bar][chunk/4u]*0x01000000u;
        for(uint32_t i=0;i<256u;++i){
            uint32_t at=chunk*256u+i,wave=(phase_value>>23)&511u;
            int32_t triangle=(int32_t)(wave<256u?wave:511u-wave)-128;
            uint32_t envelope=cue==9u?1024u-(at&1023u):1024u-at;
            int16_t sample=(int16_t)((triangle*(int32_t)envelope*38)>>10);
            pcm[i*2u]=pcm[i*2u+1u]=sample;phase_value+=step;
        }
        if(ln_audio_submit(pcm,256u)!=256u){ln_games_audio_stop();return;}
    }
    ln_audio_finish();if(!ln_audio_start())ln_games_audio_stop();
}
#else
void ln_games_audio_step(void){(void)sound_pending;}
void ln_games_audio_stop(void){sound_pending=0;}
#endif
