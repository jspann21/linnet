/* Pocket Pinball: bounded fixed-point physics at twenty updates per second. */
static const uint8_t pp_peg_x[10]={82,155,238,111,207,64,159,247,121,198};
static const uint8_t pp_peg_y[10]={115,119,115,135,139,168,158,171,193,193};
static int16_t pp_x,pp_y,pp_vx,pp_vy;
static int8_t pp_tilt;
static int8_t pp_half_x,pp_half_y;
static uint8_t pp_lives,pp_kicks,pp_cooldown,pp_targets,pp_gravity_phase,pp_banks;
static uint8_t pp_flash,pp_flash_target;
static uint32_t pp_at;
static void pp_ball(void){
    pp_x=273*16;pp_y=181*16;pp_vx=-12;pp_vy=-100;
    pp_kicks=3u;pp_tilt=0;pp_cooldown=pp_gravity_phase=0u;pp_half_x=pp_half_y=0;
}
static void pp_start(void){
    score=cw_award=0u;pp_lives=3u;pp_targets=pp_banks=pp_flash=0u;pp_at=0u;
    pp_ball();games_phase=CW_INTRO;++hud_revision;
}
static int pp_wheel(int32_t value){
    if(!value||games_phase!=PLAY)return 3;
    int32_t next=pp_tilt+value;
    if(next< -3)next= -3;if(next>3)next=3;
    if(next==pp_tilt)return 3;
    pp_tilt=(int8_t)next;++hud_revision;return 1;
}
static int pp_button(uint32_t pressed){
    if(games_phase==CW_INTRO){if(pressed&16u){games_phase=PLAY;pp_at=0u;++hud_revision;}return 1;}
    if(games_phase==PP_SERVE){if(pressed&16u){games_phase=PLAY;pp_at=0u;++hud_revision;}return 1;}
    if(games_phase==LOST){if(pressed&16u)pp_start();return 1;}
    if(games_phase==PAUSED){
        if(pressed&2u){games_phase=PLAY;pp_at=0u;++hud_revision;}
        else if(pressed&16u){cw_record(POCKET_PINBALL,score);pp_start();}
        return 1;
    }
    if(pressed&2u){games_phase=PAUSED;++hud_revision;return 1;}
    if(pressed&4u)return pp_wheel(1);
    if(pressed&8u)return pp_wheel(-1);
    if(pressed&16u&&pp_kicks){
        --pp_kicks;pp_vy=(int16_t)(pp_vy-64);
        if(pp_vy> -64)pp_vy= -64;
        if(pp_vy< -96)pp_vy= -96;
        sound_pending=2u;++hud_revision;return 1;
    }
    return 3;
}
static uint32_t pp_deadline(uint32_t now){
    if(games_phase!=PLAY)return UINT32_MAX;
    if(!pp_at)return now+50000u;
    uint32_t due=pp_at+50000u;
    return (int32_t)(due-now)>0?due:now;
}
static void pp_step(void){
    int32_t vx=pp_vx-pp_vx/32+pp_tilt,vy=pp_vy+(pp_gravity_phase?2:1);
    pp_gravity_phase^=1u;
    if(vx< -52)vx= -52;if(vx>52)vx=52;
    if(vy>68)vy=68;
    pp_vx=(int16_t)vx;pp_vy=(int16_t)vy;
    int32_t move_x=pp_vx+pp_half_x,move_y=pp_vy+pp_half_y;
    pp_x=(int16_t)(pp_x+move_x/2);pp_y=(int16_t)(pp_y+move_y/2);
    pp_half_x=(int8_t)(move_x%2);pp_half_y=(int8_t)(move_y%2);
    if(pp_x<22*16){pp_x=22*16;pp_vx=(int16_t)(pp_vx<0?-pp_vx:pp_vx);}
    if(pp_x>298*16){pp_x=298*16;pp_vx=(int16_t)(pp_vx>0?-pp_vx:pp_vx);}
    if(pp_y<52*16){pp_y=52*16;pp_vy=(int16_t)(pp_vy<0?-pp_vy/2:pp_vy);}
    int32_t x=pp_x/16,y=pp_y/16;
    if(pp_flash)--pp_flash;
    if(y>=130&&pp_targets==7u)pp_targets=0u;
    if(y>=65&&y<=99&&pp_vy<0){
        for(uint8_t i=0;i<3u;++i){
            int32_t tx=80+(int32_t)i*80;
            int32_t dx=x-tx,dy=y-82;
            if(dx*dx+dy*dy<=289&&!(pp_targets&(1u<<i))){
                pp_targets|=(uint8_t)(1u<<i);cw_add_score(50u);
                pp_flash=10u;pp_flash_target=i;sound_pending=4u;
                pp_vy=(int16_t)(pp_vy< -36?32:24);
                if(pp_targets==7u){
                    if(pp_banks<9u)++pp_banks;
                    cw_add_score((uint16_t)(200u+pp_banks*50u));
                }
                break;
            }
        }
    }
    if(pp_cooldown)--pp_cooldown;
    else for(uint8_t i=0;i<10u;++i){
        int32_t dx=x-pp_peg_x[i],dy=y-pp_peg_y[i];
        if(dx*dx+dy*dy>121)continue;
        pp_y=(int16_t)((pp_peg_y[i]+(pp_vy>0?-12:12))*16);
        pp_vy=(int16_t)(pp_vy>0?-(pp_vy<22?22:pp_vy):(-pp_vy<22?22:-pp_vy));
        pp_vx=(int16_t)(pp_vx+(dx<0?-9:9));pp_cooldown=6u;
        if(score<65530u)score+=5u;
        if((score/5u)%4u==0u)sound_pending=1u;
        break;
    }
    y=pp_y/16;
    if(pp_vy>0&&y>=182&&y<=207){
        int32_t rail=-1,direction=0;
        if(x>=26&&x<=76){rail=186+(x-26)*18/50;direction=1;}
        else if(x>=244&&x<=294){rail=204-(x-244)*18/50;direction=-1;}
        else if(x>=78&&x<=133){rail=201-(x-78)*11/55;direction=1;}
        else if(x>=187&&x<=242){rail=190+(x-187)*11/55;direction=-1;}
        if(rail>=0&&y>=rail-6&&y<=rail+7){
            pp_y=(int16_t)((rail-7)*16);
            pp_vy=(int16_t)(-(pp_vy<24?24:pp_vy));
            pp_vx=(int16_t)(pp_vx+direction*15);
            sound_pending=1u;
        }
    }
    if(pp_y>205*16){
        if(pp_lives)--pp_lives;
        cw_record(POCKET_PINBALL,score);
        if(!pp_lives){games_phase=LOST;sound_pending=3u;}
        else {pp_ball();games_phase=PP_SERVE;sound_pending=3u;}
    }
}
static bool pp_tick(uint32_t now){
    if(games_phase!=PLAY)return false;
    if(!pp_at){pp_at=now;return false;}
    uint32_t steps=(now-pp_at)/50000u;
    if(!steps)return false;
    if(steps>2u){steps=2u;pp_at=now;}
    else pp_at+=steps*50000u;
    for(uint32_t i=0;i<steps&&games_phase==PLAY;++i)pp_step();
    ++hud_revision;return true;
}
