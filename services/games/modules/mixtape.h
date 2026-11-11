/* Mixtape: each authored bar has four loop entrances on an eight-beat ring. */
#define MT_BARS 8u
static const uint8_t mt_goal[MT_BARS][4]={
    {0,2,4,6},{1,3,6,0},{7,2,5,1},{3,0,4,7},
    {6,1,3,5},{2,7,0,4},{5,3,1,6},{4,0,7,2}
};
static uint8_t mt_bar,mt_lane,mt_cursor,mt_placed[4],mt_seconds,mt_changes;
static uint32_t mt_at;
static void mt_load(uint8_t bar){
    mt_bar=bar;mt_lane=mt_cursor=mt_changes=0u;mt_seconds=(uint8_t)(60u-bar*3u);mt_at=0u;
    for(uint8_t i=0;i<4u;++i)mt_placed[i]=255u;
    games_phase=PLAY;++hud_revision;
}
static void mt_start(void){score=cw_award=0u;mt_load(0u);games_phase=CW_INTRO;}
static int mt_wheel(int32_t value){
    if(!value||games_phase!=PLAY)return 3;
    mt_cursor=(uint8_t)(((int32_t)mt_cursor+value%8+8)%8);
    ++hud_revision;return 1;
}
static int mt_button(uint32_t pressed){
    if(games_phase==CW_INTRO){if(pressed&16u)mt_load(0u);return 1;}
    if(games_phase==CW_CLEAR){if(pressed&16u)mt_load((uint8_t)(mt_bar+1u));return 1;}
    if(games_phase==WON){if(pressed&16u)mt_start();return 1;}
    if(games_phase==LOST){if(pressed&16u)mt_load(mt_bar);return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;mt_at=0u;++hud_revision;}
        else if(pressed&16u)mt_load(mt_bar);
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)mt_lane=(uint8_t)((mt_lane+1u)&3u);
    if(pressed&8u)mt_lane=(uint8_t)((mt_lane+3u)&3u);
    if(pressed&16u){
        if(mt_changes<255u)++mt_changes;
        mt_placed[mt_lane]=mt_placed[mt_lane]==mt_cursor?255u:mt_cursor;
        sound_pending=(uint8_t)(5u+mt_lane);
        bool aligned=true;
        for(uint8_t i=0;i<4u;++i)if(mt_placed[i]!=mt_goal[mt_bar][i])aligned=false;
        if(aligned){
            uint16_t bonus=(uint16_t)(mt_seconds*6u);
            uint16_t edits=mt_changes>4u?(uint16_t)(mt_changes-4u)*18u:0u;
            cw_add_score((uint16_t)(550u+(edits>=bonus?0u:bonus-edits)));
            cw_record(MIXTAPE,score);
            games_phase=mt_bar+1u==MT_BARS?WON:CW_CLEAR;sound_pending=9u;
        }
    }
    ++hud_revision;return 1;
}
static uint32_t mt_deadline(uint32_t now){
    if(games_phase!=PLAY)return UINT32_MAX;
    if(!mt_at)return now+1000000u;
    uint32_t due=mt_at+1000000u;
    return (int32_t)(due-now)>0?due:now;
}
static bool mt_tick(uint32_t now){
    if(games_phase!=PLAY)return false;
    if(!mt_at){mt_at=now;return false;}
    uint32_t elapsed=(now-mt_at)/1000000u;
    if(!elapsed)return false;
    mt_at+=elapsed*1000000u;
    mt_seconds=(uint8_t)(elapsed>=mt_seconds?0u:mt_seconds-elapsed);
    if(!mt_seconds){games_phase=LOST;sound_pending=3u;}
    ++hud_revision;return true;
}
