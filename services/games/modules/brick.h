#ifndef LINNET_GAME_BRICK_H
#define LINNET_GAME_BRICK_H

/* Brick */
static uint8_t br_bricks[48],br_level,br_lives,br_combo,br_launched;
static int16_t br_paddle,br_target,br_vx,br_vy;
static int32_t br_ball_x,br_ball_y;
static void br_layout(void){
    for(uint8_t r=0;r<6u;++r)for(uint8_t c=0;c<8u;++c){
        bool gap=(br_level==1u&&(c==3u||c==4u))||(br_level==2u&&((r+c)&1u))||
            (br_level==3u&&(c<r-1u||c>8u-r))||(br_level==4u&&(r==2u||c==1u||c==6u))||
            (br_level==5u&&((r+c)%3u==0u));
        br_bricks[r*8u+c]=gap?0u:(uint8_t)(1u+(r+br_level)%3u);
    }
}
static void br_serve(void){br_ball_x=(int32_t)br_paddle<<8;br_ball_y=142<<8;br_vx=(random_next()&1u)?520:-520;br_vy=-650;br_launched=0;}
static void br_start(void){br_level=0;br_lives=3;br_combo=0;br_paddle=br_target=128;br_layout();br_serve();ex_phase=EX_READY;}
static bool br_left(void){for(uint8_t i=0;i<48u;++i)if(br_bricks[i])return true;return false;}
static void br_step(void){
    br_paddle+=(br_target-br_paddle)/3;if(!br_launched){br_ball_x=(int32_t)br_paddle<<8;return;}
    int32_t nx=br_ball_x+br_vx,ny=br_ball_y+br_vy;
    if(nx<(7<<8)){nx=7<<8;br_vx=-br_vx;}if(nx>(249<<8)){nx=249<<8;br_vx=-br_vx;}
    if(ny<(12<<8)){ny=12<<8;br_vy=-br_vy;}
    int16_t px=(int16_t)(nx>>8),py=(int16_t)(ny>>8);
    if(br_vy>0&&py>=146&&py<=153&&px>=br_paddle-25&&px<=br_paddle+25){
        br_vy=(int16_t)(-(650+br_level*45));br_vx=(int16_t)((px-br_paddle)*38);
        if(br_vx>-180&&br_vx<180)br_vx=br_vx<0?-180:180;br_combo=0;sound_pending=2u;
    }
    if(py>=22&&py<76){uint8_t col=(uint8_t)(px/32),row=(uint8_t)((py-22)/9);
        if(col<8u&&row<6u&&br_bricks[row*8u+col]){uint8_t *brick=&br_bricks[row*8u+col];if(*brick)--*brick;
            br_vy=-br_vy;if(br_combo<9u)++br_combo;uint32_t add=10u+br_combo*2u;
            score=(uint16_t)(score>65535u-add?65535u:score+add);sound_pending=1u;ex_effect_until=ex_tick_at+140000u;
            if(!br_left()){if(++br_level>=6u){ex_phase=EX_WON;extra_record();return;}br_layout();br_serve();ex_phase=EX_READY;return;}
        }
    }
    if(py>168){if(br_lives)--br_lives;if(!br_lives){ex_phase=EX_LOST;extra_record();}else{br_combo=0;br_serve();ex_phase=EX_READY;}sound_pending=3u;return;}
    br_ball_x=nx;br_ball_y=ny;
}

#endif
