/* Clockwork City: every route and opening is authored before installation. */
#define CC_MAPS 8u
static const uint8_t cc_entry[CC_MAPS][3]={
    {2,7,4},{9,3,8},{5,1,10},{0,6,3},
    {8,11,2},{4,9,1},{10,5,7},{6,2,11}
};
static const uint8_t cc_exit[CC_MAPS][3]={
    {5,11,6},{2,5,11},{7,7,11},{4,9,7},
    {2,0,4},{5,2,4},{1,8,0},{11,6,0}
};
static const uint8_t cc_destination[CC_MAPS]={9,10,9,11,9,9,11,10};
static const uint8_t cc_scramble[CC_MAPS][3]={
    {3,0,8},{1,9,2},{7,2,5},{5,1,9},
    {2,8,3},{9,4,6},{4,10,1},{8,3,0}
};
static uint8_t cc_map,cc_focus,cc_rot[3],cc_seconds,cc_attempts;
static uint16_t cc_travel;
static uint32_t cc_at;
static uint8_t cc_route(void){
    uint8_t gate=(uint8_t)((cc_entry[cc_map][0]+cc_rot[0])%12u);
    if(gate)return 0u;
    gate=(uint8_t)((cc_exit[cc_map][0]+cc_rot[0])%12u);
    if(gate!=(cc_entry[cc_map][1]+cc_rot[1])%12u)return 1u;
    gate=(uint8_t)((cc_exit[cc_map][1]+cc_rot[1])%12u);
    if(gate!=(cc_entry[cc_map][2]+cc_rot[2])%12u)return 2u;
    gate=(uint8_t)((cc_exit[cc_map][2]+cc_rot[2])%12u);
    return gate==cc_destination[cc_map]?4u:3u;
}
static void cc_load(uint8_t map){
    cc_map=map;cc_focus=cc_attempts=0u;cc_travel=0u;
    cc_seconds=(uint8_t)(75u-map*5u);cc_at=0u;
    for(uint8_t i=0;i<3u;++i)cc_rot[i]=cc_scramble[map][i];
    games_phase=PLAY;++hud_revision;
}
static void cc_start(void){score=cw_award=0u;cc_load(0u);games_phase=CW_INTRO;}
static int cc_wheel(int32_t value){
    if(!value||games_phase!=PLAY)return 3;
    uint8_t before=cc_route();
    cc_rot[cc_focus]=(uint8_t)(((int32_t)cc_rot[cc_focus]+value%12+12)%12);
    uint32_t travel=(uint32_t)(value<0?-(int64_t)value:value)%12u;
    cc_travel=(uint16_t)(travel>UINT16_MAX-cc_travel?UINT16_MAX:cc_travel+travel);
    if(cc_route()>before)sound_pending=1u;
    ++hud_revision;return 1;
}
static int cc_button(uint32_t pressed){
    if(games_phase==CW_INTRO){if(pressed&16u)cc_load(0u);return 1;}
    if(games_phase==CW_CLEAR){if(pressed&16u)cc_load((uint8_t)(cc_map+1u));return 1;}
    if(games_phase==WON){if(pressed&16u)cc_start();return 1;}
    if(games_phase==LOST){if(pressed&16u)cc_load(cc_map);return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;cc_at=0u;++hud_revision;}
        else if(pressed&16u)cc_load(cc_map);
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)cc_focus=(uint8_t)((cc_focus+1u)%3u);
    if(pressed&8u)cc_focus=(uint8_t)((cc_focus+2u)%3u);
    if(pressed&16u){
        if(cc_route()==4u){
            uint16_t bonus=(uint16_t)(cc_seconds*5u);
            uint16_t cost=(uint16_t)((cc_travel>40u?40u:cc_travel)*7u+
                                     (cc_attempts>4u?4u:cc_attempts)*80u);
            cw_add_score((uint16_t)(700u+(cost>=bonus?0u:bonus-cost)));
            cw_record(CLOCKWORK_CITY,score);
            games_phase=cc_map+1u==CC_MAPS?WON:CW_CLEAR;sound_pending=4u;
        }else{if(cc_attempts<255u)++cc_attempts;sound_pending=3u;}
    }
    ++hud_revision;return 1;
}
static uint32_t cc_deadline(uint32_t now){
    if(games_phase!=PLAY)return UINT32_MAX;
    if(!cc_at)return now+1000000u;
    uint32_t due=cc_at+1000000u;
    return (int32_t)(due-now)>0?due:now;
}
static bool cc_tick(uint32_t now){
    if(games_phase!=PLAY)return false;
    if(!cc_at){cc_at=now;return false;}
    uint32_t elapsed=(now-cc_at)/1000000u;
    if(!elapsed)return false;
    cc_at+=elapsed*1000000u;
    cc_seconds=(uint8_t)(elapsed>=cc_seconds?0u:cc_seconds-elapsed);
    if(!cc_seconds){games_phase=LOST;sound_pending=3u;}
    ++hud_revision;return true;
}
