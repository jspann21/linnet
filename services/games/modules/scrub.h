/* Scrub: precomputed 32-frame timelines. The device only indexes these paths. */
#define SCR_LEVELS 12u
static const uint8_t scr_paths[SCR_LEVELS][3][32]={
    { /* 1 */
        {13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12},
        {14,12,10,8,6,4,2,0,2,4,6,8,10,12,14,14,12,10,8,6,4,2,0,2,4,6,8,10,12,14,14,12},
        {11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9}
    },
    { /* 2 */
        {7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9},
        {2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1},
        {5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7}
    },
    { /* 3 */
        {5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3},
        {14,13,10,7,4,1,2,5,8,11,14,13,10,7,4,1,2,5,8,11,14,13,10,7,4,1,2,5,8,11,14,13},
        {11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13}
    },
    { /* 4 */
        {7,10,13,14,11,8,5,2,1,4,7,10,13,14,11,8,5,2,1,4,7,10,13,14,11,8,5,2,1,4,7,10},
        {12,15,12,9,6,3,0,3,6,9,12,15,12,9,6,3,0,3,6,9,12,15,12,9,6,3,0,3,6,9,12,15},
        {7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5}
    },
    { /* 5 */
        {11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12},
        {4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3},
        {9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10}
    },
    { /* 6 */
        {1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
        {2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3},
        {3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1}
    },
    { /* 7 */
        {11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12},
        {0,3,6,9,12,15,12,9,6,3,0,3,6,9,12,15,12,9,6,3,0,3,6,9,12,15,12,9,6,3,0,3},
        {3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1}
    },
    { /* 8 */
        {7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6},
        {14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13},
        {1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1}
    },
    { /* 9 */
        {1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
        {6,4,2,0,2,4,6,8,10,12,14,14,12,10,8,6,4,2,0,2,4,6,8,10,12,14,14,12,10,8,6,4},
        {1,2,5,8,11,14,13,10,7,4,1,2,5,8,11,14,13,10,7,4,1,2,5,8,11,14,13,10,7,4,1,2}
    },
    { /* 10 */
        {7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6},
        {2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3},
        {1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1}
    },
    { /* 11 */
        {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2},
        {10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8,9,10,11},
        {1,4,7,10,13,14,11,8,5,2,1,4,7,10,13,14,11,8,5,2,1,4,7,10,13,14,11,8,5,2,1,4}
    },
    { /* 12 */
        {1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1,3,5,7,9,11,13,15,13,11,9,7,5,3,1,1},
        {12,14,14,12,10,8,6,4,2,0,2,4,6,8,10,12,14,14,12,10,8,6,4,2,0,2,4,6,8,10,12,14},
        {3,4,5,6,7,8,9,10,11,12,13,14,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4}
    }
};
static const uint8_t scr_goals[SCR_LEVELS][3]={{13,2,5},{5,2,3},{13,13,11},{11,3,13},{8,2,10},{12,9,13},{3,3,11},{10,4,7},{4,12,7},{5,9,7},{10,10,2},{7,6,13}};
static uint8_t scr_level,scr_time,scr_focus,scr_anchor,scr_anchor_at;
static uint8_t scr_pins;
static uint16_t scr_travel;
static void scr_load(uint8_t level){
    scr_level=level;scr_time=0;scr_focus=(uint8_t)(level%3u);scr_anchor=3u;
    scr_anchor_at=scr_pins=0;scr_travel=0;games_phase=PLAY;++hud_revision;
}
static void scr_start(void){score=cw_award=0;scr_load(0u);games_phase=SCR_INTRO;}
static uint8_t scr_position(uint8_t lane){
    return scr_paths[scr_level][lane][lane==scr_anchor?scr_anchor_at:scr_time];
}
static void scr_check(void){
    for(uint8_t lane=0;lane<3u;++lane)
        if(scr_position(lane)!=scr_goals[scr_level][lane])return;
    uint16_t effort=(uint16_t)(scr_travel>40u?200u:scr_travel*5u);
    effort=(uint16_t)(effort+scr_pins*20u);
    cw_add_score((uint16_t)(300u+(effort>=200u?0u:200u-effort)));
    sound_pending=4u;
    if(score>best[SCRUB]){
        best[SCRUB]=score;
#if defined(LINNET_STORAGE)
        ln_display_game_best_set(SCRUB,score);
#endif
    }
    games_phase=scr_level+1u==SCR_LEVELS?WON:SCR_CLEAR;++hud_revision;
}
static int scr_input(uint32_t kind,int32_t value){
    if(kind==1u){
        if(!value||games_phase!=PLAY)return 3;
        int32_t next=((int32_t)scr_time+value%32+32)%32;
        if(next==scr_time)return 3;
        scr_time=(uint8_t)next;
        uint32_t travel=(uint32_t)(value<0?-(int64_t)value:value)%32u;
        scr_travel=(uint16_t)(travel>UINT16_MAX-scr_travel?UINT16_MAX:scr_travel+travel);
        ++hud_revision;scr_check();return 1;
    }
    if(kind!=2u)return 0;
    uint32_t pressed=(uint32_t)value&~buttons;buttons=(uint32_t)value;
    if(!pressed)return 3;
    if(pressed&1u)return 2;
    if(games_phase==SCR_INTRO){if(pressed&16u){games_phase=PLAY;++hud_revision;}return 1;}
    if(games_phase==SCR_CLEAR){if(pressed&16u)scr_load((uint8_t)(scr_level+1u));return 1;}
    if(games_phase==WON){if(pressed&16u)scr_start();return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;++hud_revision;}
        else if(pressed&16u)scr_load(scr_level);
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)scr_focus=(uint8_t)((scr_focus+1u)%3u);
    if(pressed&8u)scr_focus=(uint8_t)((scr_focus+2u)%3u);
    if(pressed&16u){
        if(scr_pins<9u)++scr_pins;
        if(scr_anchor==scr_focus)scr_anchor=3u;
        else{scr_anchor=scr_focus;scr_anchor_at=scr_time;}
        sound_pending=1u;
    }
    ++hud_revision;scr_check();return 1;
}
