/* Dial Heist: ten authored locks; the device only checks table entries. */
#define DH_LOCKS 10u
static const uint8_t dh_targets[DH_LOCKS][3]={
    {7,22,13},{26,9,18},{11,30,4},{18,5,27},{3,20,11},
    {24,14,1},{9,28,17},{16,2,25},{29,12,6},{5,19,31}
};
static uint8_t dh_lock,dh_stage,dh_dial,dh_misses,dh_seconds;
static uint32_t dh_at;
static uint8_t dh_distance(void){
    if(dh_stage>=3u)return 0u;
    uint8_t target=dh_targets[dh_lock][dh_stage];
    uint8_t delta=dh_dial>target?dh_dial-target:target-dh_dial;
    return delta<16u?delta:(uint8_t)(32u-delta);
}
static void dh_load(uint8_t lock){
    dh_lock=lock;dh_stage=dh_dial=dh_misses=0u;
    dh_seconds=(uint8_t)(45u-lock*2u);dh_at=0u;games_phase=PLAY;++hud_revision;
}
static void dh_start(void){score=cw_award=0u;dh_load(0u);games_phase=CW_INTRO;}
static int dh_wheel(int32_t value){
    if(!value||games_phase!=PLAY)return 3;
    uint8_t before=dh_distance();
    dh_dial=(uint8_t)(((int32_t)dh_dial+value%32+32)%32);
    if(before>2u&&dh_distance()<=2u)sound_pending=1u;
    ++hud_revision;return 1;
}
static int dh_button(uint32_t pressed){
    if(games_phase==CW_INTRO){if(pressed&16u)dh_load(0u);return 1;}
    if(games_phase==CW_CLEAR){if(pressed&16u)dh_load((uint8_t)(dh_lock+1u));return 1;}
    if(games_phase==WON){if(pressed&16u)dh_start();return 1;}
    if(games_phase==LOST){if(pressed&16u)dh_load(dh_lock);return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;dh_at=0u;++hud_revision;}
        else if(pressed&16u)dh_load(dh_lock);
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)return dh_wheel(1);
    if(pressed&8u)return dh_wheel(-1);
    if(pressed&16u){
        if(dh_distance()<=1u){
            sound_pending=2u;++dh_stage;
            if(dh_stage==3u){
                cw_add_score((uint16_t)(600u+dh_seconds*8u-dh_misses*80u));
                cw_record(DIAL_HEIST,score);
                games_phase=dh_lock+1u==DH_LOCKS?WON:CW_CLEAR;sound_pending=4u;
            }
        }else{
            ++dh_misses;sound_pending=3u;
            if(dh_misses>=3u)games_phase=LOST;
        }
        ++hud_revision;return 1;
    }
    return 3;
}
static uint32_t dh_deadline(uint32_t now){
    if(games_phase!=PLAY)return UINT32_MAX;
    if(!dh_at)return now+1000000u;
    uint32_t due=dh_at+1000000u;
    return (int32_t)(due-now)>0?due:now;
}
static bool dh_tick(uint32_t now){
    if(games_phase!=PLAY)return false;
    if(!dh_at){dh_at=now;return false;}
    uint32_t elapsed=(now-dh_at)/1000000u;
    if(!elapsed)return false;
    dh_at+=elapsed*1000000u;
    dh_seconds=(uint8_t)(elapsed>=dh_seconds?0u:dh_seconds-elapsed);
    if(!dh_seconds){games_phase=LOST;sound_pending=3u;}
    ++hud_revision;return true;
}
