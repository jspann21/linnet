/* Signal: fixed frequencies and text are prepared in ROM; tuning is a lookup. */
#define SG_STATIONS 8u
static const uint8_t sg_frequency[SG_STATIONS]={19,83,41,108,62,27,96,53};
static const char *const sg_messages[SG_STATIONS]={
    "THE LIGHTHOUSE IS AWAKE","MEET AT THE NORTH PLATFORM",
    "COUNT THE STARS AFTER RAIN","A KEY IS UNDER THE CLOCK",
    "THE GARDEN GATE IS OPEN","FOLLOW THE BLUE TRAM",
    "THE LAST TRAIN LEAVES AT DAWN","YOU FOUND THE QUIET CHANNEL"
};
static uint8_t sg_station,sg_tune,sg_progress;
static uint8_t sg_false_locks;
static uint16_t sg_travel;
static bool sg_locked,sg_rejected;
static uint32_t sg_at;
static uint8_t sg_distance(void){
    uint8_t target=sg_frequency[sg_station];
    return sg_tune>target?sg_tune-target:target-sg_tune;
}
static void sg_load(uint8_t station){
    sg_station=station;sg_tune=64u;sg_progress=sg_false_locks=0u;
    sg_travel=0u;sg_locked=sg_rejected=false;sg_at=0u;
    games_phase=PLAY;++hud_revision;
}
static void sg_start(void){score=cw_award=0u;sg_load(0u);games_phase=CW_INTRO;}
static int sg_wheel(int32_t value){
    if(!value||games_phase!=PLAY)return 3;
    int32_t next=(int32_t)sg_tune+value;
    if(next<0)next=0;if(next>127)next=127;
    if(next==sg_tune)return 3;
    uint8_t before=sg_distance();sg_tune=(uint8_t)next;
    sg_rejected=false;
    uint32_t travel=(uint32_t)(value<0?-(int64_t)value:value);
    sg_travel=(uint16_t)(travel>UINT16_MAX-sg_travel?UINT16_MAX:sg_travel+travel);
    if(sg_locked&&sg_distance()>1u){sg_locked=false;sg_progress=0u;sg_at=0u;
        if(sg_false_locks<9u)++sg_false_locks;sound_pending=3u;}
    else if(before>2u&&sg_distance()<=2u)sound_pending=1u;
    ++hud_revision;return 1;
}
static int sg_button(uint32_t pressed){
    if(games_phase==CW_INTRO){if(pressed&16u)sg_load(0u);return 1;}
    if(games_phase==CW_CLEAR){if(pressed&16u)sg_load((uint8_t)(sg_station+1u));return 1;}
    if(games_phase==WON){if(pressed&16u)sg_start();return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;sg_at=0u;++hud_revision;}
        else if(pressed&16u)sg_load(sg_station);
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)return sg_wheel(1);
    if(pressed&8u)return sg_wheel(-1);
    if(pressed&16u){
        if(sg_distance()>1u){if(sg_false_locks<9u)++sg_false_locks;
            sg_rejected=true;sound_pending=3u;}
        else {sg_locked=!sg_locked;sg_rejected=false;sg_at=0u;sound_pending=sg_locked?2u:1u;}
        ++hud_revision;return 1;
    }
    return 3;
}
static uint32_t sg_deadline(uint32_t now){
    if(games_phase!=PLAY||!sg_locked)return UINT32_MAX;
    if(!sg_at)return now+1000000u;
    uint32_t due=sg_at+1000000u;
    return (int32_t)(due-now)>0?due:now;
}
static bool sg_tick(uint32_t now){
    if(games_phase!=PLAY||!sg_locked)return false;
    if(!sg_at){sg_at=now;return false;}
    uint32_t elapsed=(now-sg_at)/1000000u;
    if(!elapsed)return false;
    sg_at+=elapsed*1000000u;
    sg_progress=(uint8_t)(elapsed>=4u-sg_progress?4u:sg_progress+elapsed);
    if(sg_progress==4u){
        sg_locked=false;
        uint16_t cost=(uint16_t)(sg_travel>100u?300u:sg_travel*3u);
        cost=(uint16_t)(cost+sg_false_locks*60u);
        cw_add_score((uint16_t)(500u+(cost>=300u?0u:300u-cost)));
        cw_record(SIGNAL,score);
        games_phase=sg_station+1u==SG_STATIONS?WON:CW_CLEAR;sound_pending=4u;
    }
    ++hud_revision;return true;
}
