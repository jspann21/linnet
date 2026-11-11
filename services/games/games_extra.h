#ifndef LINNET_GAMES_EXTRA_H
#define LINNET_GAMES_EXTRA_H

/* Allocation-free state for the eight expanded games. Coordinates are logical
 * and presentations map them independently to each display. */
enum { EX_READY,EX_LEVEL,EX_PLAY,EX_PAUSED,EX_REVEAL,EX_WON,EX_LOST,EX_LOADING };
static uint8_t ex_phase,ex_choice,ex_menu;
static uint32_t ex_tick_at,ex_effect_until;
static void extra_record(void);

#include "services/games/modules/brick.h"
#include "services/games/modules/vortex.h"
#include "services/games/modules/falling.h"
#include "services/games/modules/minesweeper.h"
#include "services/games/modules/reversi.h"
#include "services/games/modules/snake.h"
#include "services/games/modules/word_search.h"
#include "services/games/modules/music_quiz.h"

static void extra_record(void){if(score>best[game]){best[game]=score;
#if defined(LINNET_STORAGE)
    ln_display_game_best_set(game,score);
#endif
}}
static bool extra_arcade(void){return game==BRICK||game==VORTEX||game==FALLING||game==SNAKE;}
static void extra_leave(void){
#if defined(LINNET_AUDIO)
    if(game==MUSIC_QUIZ)ln_music_quiz_end();
#endif
}
static void extra_start(void){score=0;ex_choice=1u;ex_menu=0;ex_tick_at=ex_effect_until=0;previous_best=best[game];if(game==BRICK)br_start();else if(game==VORTEX)vx_start();else if(game==FALLING)fb_start();else if(game==MINESWEEPER){ex_choice=0;ex_phase=EX_LEVEL;}else if(game==REVERSI)rv_start();else if(game==SNAKE)sn_start();else if(game==WORD_SEARCH){ex_choice=0;ex_phase=EX_LEVEL;}
#if defined(LINNET_AUDIO)
    else if(game==MUSIC_QUIZ)mq_start();
#else
    else if(game==MUSIC_QUIZ)ex_phase=EX_LOADING;
#endif
    ++hud_revision;
}
static int extra_wheel(int32_t value){
    if(!value)return 3;bool changed=false;
    if(ex_phase==EX_LEVEL){uint8_t next=(uint8_t)((ex_choice+value%3+3)%3);changed=next!=ex_choice;ex_choice=next;}
    else if(game==BRICK&&(ex_phase==EX_READY||ex_phase==EX_PLAY)){
        /* About three quarters of a wheel turn covers the full paddle range. */
        int64_t target=(int64_t)br_target+(int64_t)value*12;
        if(target<27)target=27;if(target>229)target=229;
        changed=target!=br_target;br_target=(int16_t)target;
        if(changed&&ex_phase==EX_READY){br_paddle=br_target;br_ball_x=(int32_t)br_paddle<<8;}
    }else if(game==VORTEX&&(ex_phase==EX_READY||ex_phase==EX_PLAY)){
        uint8_t next=(uint8_t)(vx_angle+(uint32_t)value*3u);changed=next!=vx_angle;vx_angle=next;
    }else if(game==FALLING&&ex_phase==EX_PLAY){
        int8_t x=(int8_t)(fb_x+(value>0?1:-1));
        if(!fb_hit(x,fb_y,fb_rot)){fb_x=x;changed=true;if(fb_lock_at&&fb_lock_resets++<8u)fb_lock_at=ex_tick_at+350000u;}
    }else if(game==MINESWEEPER&&ex_phase==EX_PLAY){
        uint8_t next=(uint8_t)((ms_cursor+(value>0?1:ms_w*ms_h-1u))%(ms_w*ms_h));changed=next!=ms_cursor;ms_cursor=next;
    }else if(game==REVERSI&&ex_phase==EX_PLAY&&rv_turn==1u){
        uint8_t next=rv_next(rv_cursor,value>0?1:-1,1u);changed=next!=rv_cursor;rv_cursor=next;
    }else if(game==SNAKE&&(ex_phase==EX_READY||ex_phase==EX_PLAY)){
        uint8_t next=(uint8_t)((sn_dir+(value>0?1u:3u))&3u);changed=next!=sn_turn;sn_turn=next;
    }else if(game==WORD_SEARCH&&ex_phase==EX_PLAY){
        uint8_t next=(uint8_t)((ws_cursor+(value>0?1:ws_w*ws_h-1u))%(ws_w*ws_h));changed=next!=ws_cursor;ws_cursor=next;
    }
#if defined(LINNET_AUDIO)
    else if(game==MUSIC_QUIZ&&ex_phase==EX_PLAY&&!mq_answered){uint8_t next=(uint8_t)((mq_focus+(value>0?1u:3u))&3u);changed=next!=mq_focus;mq_focus=next;}
#endif
    if(changed)++hud_revision;return changed?1:3;
}
static int extra_button(uint32_t pressed){if(pressed&1u){extra_leave();return 2;}if((ex_phase==EX_WON||ex_phase==EX_LOST||ex_phase==EX_REVEAL)&&(pressed&16u)){
#if defined(LINNET_AUDIO)
        if(game==MUSIC_QUIZ){mq_restart();++hud_revision;return 1;}
#endif
        extra_start();return 1;}if(ex_phase==EX_LEVEL){if(pressed&4u)ex_choice=(ex_choice+1u)%3u;if(pressed&8u)ex_choice=(ex_choice+2u)%3u;if(pressed&16u){if(game==MINESWEEPER)ms_start_level(ex_choice);else ws_start_level(ex_choice);}++hud_revision;return 1;}if((extra_arcade()||game==REVERSI)&&(pressed&2u)){if(ex_phase==EX_PLAY){ex_phase=EX_PAUSED;if(game==FALLING)fb_lock_at=0;}else if(ex_phase==EX_PAUSED){ex_phase=EX_PLAY;ex_tick_at=0;}++hud_revision;return 1;}if(ex_phase==EX_PAUSED)return 1;if(game==BRICK){if((ex_phase==EX_READY||ex_phase==EX_PLAY)&&(pressed&16u)){br_launched=1;ex_phase=EX_PLAY;ex_tick_at=0;}if(pressed&4u){br_target+=16;if(br_target>229)br_target=229;}if(pressed&8u){br_target-=16;if(br_target<27)br_target=27;}if(ex_phase==EX_READY){br_paddle=br_target;br_ball_x=(int32_t)br_paddle<<8;}}
    else if(game==VORTEX){if(ex_phase==EX_READY&&(pressed&16u)){ex_phase=EX_PLAY;ex_tick_at=0;}else if(ex_phase==EX_PLAY&&(pressed&16u))vx_fire();if(pressed&4u)vx_angle+=8u;if(pressed&8u)vx_angle-=8u;}
    else if(game==FALLING){if(ex_phase==EX_READY&&(pressed&16u)){ex_phase=EX_PLAY;ex_tick_at=0;}else if(ex_phase==EX_PLAY){if(pressed&4u)fb_drop(false);if(pressed&8u)fb_drop(true);if(pressed&16u){uint8_t rot=(fb_rot+1u)&3u,changed=0;if(!fb_hit(fb_x,fb_y,rot)){fb_rot=rot;changed=1;}else if(fb_x&&!fb_hit((int8_t)(fb_x-1u),fb_y,rot)){--fb_x;fb_rot=rot;changed=1;}else if(!fb_hit((int8_t)(fb_x+1u),fb_y,rot)){++fb_x;fb_rot=rot;changed=1;}if(changed&&fb_lock_at&&fb_lock_resets++<8u)fb_lock_at=ex_tick_at+350000u;}}}
    else if(game==MINESWEEPER&&ex_phase==EX_PLAY){if(pressed&4u)ms_cursor=(ms_cursor+1u)%(ms_w*ms_h);if(pressed&8u)ms_cursor=(ms_cursor+ms_w*ms_h-1u)%(ms_w*ms_h);if(pressed&2u&&!ms_open[ms_cursor]){ms_flag[ms_cursor]=!ms_flag[ms_cursor];sound_pending=1u;}if(pressed&16u)ms_reveal(ms_cursor);}
    else if(game==REVERSI&&ex_phase==EX_PLAY&&rv_turn==1u){if(pressed&4u)rv_cursor=rv_next(rv_cursor,1,1u);if(pressed&8u)rv_cursor=rv_next(rv_cursor,-1,1u);if(pressed&16u&&rv_flips(rv_cursor,1u,true)){rv_last=rv_cursor;rv_turn=2u;rv_pass=0;sound_pending=1u;if(!rv_has(2u)){if(!rv_has(1u))rv_finish();else{rv_turn=1u;rv_pass=2u;rv_cursor=rv_next(rv_last,1,1u);}}else rv_ai();}}
    else if(game==SNAKE){if(ex_phase==EX_READY&&(pressed&16u)){ex_phase=EX_PLAY;sn_move_at=0;}else if(ex_phase==EX_PLAY){if(pressed&4u)sn_turn=(sn_dir+1u)&3u;if(pressed&8u)sn_turn=(sn_dir+3u)&3u;if(pressed&16u)sn_boost=8u;}}
    else if(game==WORD_SEARCH&&ex_phase==EX_PLAY){if(pressed&4u)ws_cursor=(ws_cursor+1u)%(ws_w*ws_h);if(pressed&8u)ws_cursor=(ws_cursor+ws_w*ws_h-1u)%(ws_w*ws_h);if(pressed&16u)ws_select();if(pressed&2u&&ws_anchor!=255u)ws_anchor=255u;}
#if defined(LINNET_AUDIO)
    else if(game==MUSIC_QUIZ){if(pressed&2u){mq_clip_sample=0;if(mq_clip_paused&&!mq_answered){mq_clip_active_us=mq_clip_elapsed=0;mq_clip_paused=0;(void)ln_player_pause(false);}else ln_music_toggle();return 1;}if((ex_phase==EX_LOADING||mq_unavailable)&&(pressed&16u))mq_retry();else if(ex_phase==EX_PLAY){if(mq_answered&&(pressed&16u)){if(++mq_round>=10u){ex_phase=EX_WON;extra_record();}else{mq_choice_state(mq_choose(false));mq_answered=0;ex_tick_at=0;}}else if(pressed&16u){mq_answered=1;(void)ln_player_pause(true);mq_clip_paused=1;if(mq_focus==mq_correct){++mq_streak;score=(uint16_t)(score+100u+mq_streak*25u);sound_pending=2u;}else{mq_streak=0;sound_pending=3u;}}if(!mq_answered){if(pressed&4u)mq_focus=(mq_focus+1u)&3u;if(pressed&8u)mq_focus=(mq_focus+3u)&3u;}}}
#endif
    ++hud_revision;return 1;}
static bool extra_tick(uint32_t now){
#if defined(LINNET_AUDIO)
    if(game==MUSIC_QUIZ)return mq_tick(now);
#endif
    if(ex_phase==EX_LOADING)return false;
    if(ex_phase!=EX_PLAY)return false;
    if(game==MINESWEEPER||game==WORD_SEARCH){
        uint32_t *started=game==MINESWEEPER?&ms_started_at:&ws_started_at;
        uint32_t *elapsed=game==MINESWEEPER?&ms_elapsed:&ws_elapsed;
        if(!*started)*started=now;uint32_t next=(now-*started)/1000000u;
        if(next!=*elapsed){*elapsed=next;return true;}return false;
    }
    if(game==FALLING&&fb_lock_at&&(int32_t)(now-fb_lock_at)>=0){ex_tick_at=now;fb_lock();return true;}
    if(!ex_tick_at){ex_tick_at=now;return false;}
    uint32_t step=game==SNAKE?(sn_boost?65000u:(170000u-(sn_len<50u?sn_len*1200u:60000u))):33333u;
    if(game==FALLING)step=(uint32_t)(650000u-(fb_level<12u?fb_level*42000u:504000u));
    if((int32_t)(now-ex_tick_at)<(int32_t)step)return false;
    uint8_t loops=0;while((int32_t)(now-ex_tick_at)>=(int32_t)step&&loops<3u){++loops;ex_tick_at+=step;
        if(game==BRICK)br_step();else if(game==VORTEX)vx_step();else if(game==FALLING)fb_drop(false);else if(game==SNAKE)sn_step();if(ex_phase!=EX_PLAY)break;}
    if(loops==3u&&(int32_t)(now-ex_tick_at)>=(int32_t)step)ex_tick_at=now;
    return true;
}
static uint32_t extra_deadline(uint32_t now){
#if defined(LINNET_AUDIO)
    if(game==MUSIC_QUIZ)return mq_deadline(now);
#endif
    if(ex_phase!=EX_PLAY)return UINT32_MAX;if(game==MINESWEEPER||game==WORD_SEARCH)return now+1000000u;if(!ex_tick_at)return now;
    uint32_t step=game==SNAKE?(sn_boost?65000u:(170000u-(sn_len<50u?sn_len*1200u:60000u))):game==FALLING?(uint32_t)(650000u-(fb_level<12u?fb_level*42000u:504000u)):33333u;
    uint32_t deadline=ex_tick_at+step;if(game==FALLING&&fb_lock_at&&(int32_t)(fb_lock_at-deadline)<0)deadline=fb_lock_at;
    return (int32_t)(deadline-now)>0?deadline:now;
}

#endif
