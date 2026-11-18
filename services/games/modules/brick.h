#ifndef LINNET_GAME_BRICK_H
#define LINNET_GAME_BRICK_H

enum {
    BR_COLS = 8, BR_ROWS = 6, BR_CELL_W = 32, BR_CELL_H = 9, BR_WALL_TOP = 22,
    BR_PADDLE_Y = 151, BR_PADDLE_HALF = 25, BR_PADDLE_CONTACT = (BR_PADDLE_Y - 5) << 8
};
static uint8_t br_bricks[BR_ROWS*BR_COLS],br_level,br_lives,br_combo,br_launched;
static int16_t br_paddle,br_vx,br_vy;
static int32_t br_ball_x,br_ball_y;
static void br_layout(void){
    for(uint8_t r=0;r<BR_ROWS;++r)for(uint8_t c=0;c<BR_COLS;++c){
        bool gap=(br_level==1u&&(c==3u||c==4u))||(br_level==2u&&((r+c)&1u))||
            (br_level==3u&&(c<r-1u||c>8u-r))||(br_level==4u&&(r==2u||c==1u||c==6u))||
            (br_level==5u&&((r+c)%3u==0u));
        br_bricks[r*BR_COLS+c]=gap?0u:(uint8_t)(1u+(r+br_level)%3u);
    }
}
static void br_serve(void){br_ball_x=(int32_t)br_paddle<<8;br_ball_y=142<<8;br_vx=(random_next()&1u)?520:-520;br_vy=-650;br_launched=0;}
static void br_start(void){br_level=0;br_lives=3;br_combo=0;br_paddle=128;br_layout();br_serve();ex_phase=EX_READY;}
static bool br_move_paddle(int64_t delta){
    int64_t position = br_paddle + delta;
    if(position < BR_PADDLE_HALF + 2)position = BR_PADDLE_HALF + 2;
    if(position > 256 - BR_PADDLE_HALF - 2)position = 256 - BR_PADDLE_HALF - 2;
    if(position == br_paddle)return false;
    br_paddle = (int16_t)position;
    if(!br_launched)br_ball_x = (int32_t)br_paddle << 8;
    return true;
}
static bool br_left(void){for(uint8_t i=0;i<BR_ROWS*BR_COLS;++i)if(br_bricks[i])return true;return false;}
static uint8_t *br_solid(int32_t x,int32_t y){
    int32_t row = (y >> 8) - BR_WALL_TOP;
    if(row < 0 || row >= BR_ROWS * BR_CELL_H)return 0;
    /* The side walls constrain x to the brick grid. */
    uint8_t *brick = &br_bricks[row / BR_CELL_H * BR_COLS + (x >> 8) / BR_CELL_W];
    return *brick ? brick : 0;
}
static void br_step(void){
    if(!br_launched)return;
    int32_t nx = br_ball_x + br_vx, ny = br_ball_y + br_vy;
    if(nx < (7<<8)){nx = 7<<8;br_vx = -br_vx;}
    if(nx > (249<<8)){nx = 249<<8;br_vx = -br_vx;}
    if(ny < (12<<8)){ny = 12<<8;br_vy = -br_vy;}

    /* Resolve each axis before committing it so a surviving brick can never
     * contain the ball on the next tick. A corner can block both axes. */
    uint8_t *brick = br_solid(nx,br_ball_y);
    if(brick){
        nx = br_ball_x;
        br_vx = -br_vx;
    }
    uint8_t *vertical = br_solid(nx,ny);
    if(vertical){
        ny = br_ball_y;
        br_vy = -br_vy;
        if(!brick)brick = vertical;
    }
    if(brick){
        --*brick;
        if(br_combo < 9u)++br_combo;
        uint32_t add = 10u + br_combo*2u;
        score = (uint16_t)(score > 65535u-add ? 65535u : score+add);
        sound_pending = 1u;
        if(!*brick && !br_left()){
            if(++br_level >= 6u){ex_phase = EX_WON;extra_record();return;}
            br_layout();br_serve();ex_phase = EX_READY;return;
        }
    }

    if(br_vy > 0 && br_ball_y <= BR_PADDLE_CONTACT && ny >= BR_PADDLE_CONTACT){
        int32_t contact_x = br_ball_x +
            (nx - br_ball_x) * (BR_PADDLE_CONTACT - br_ball_y) / (ny - br_ball_y);
        int32_t offset = contact_x - ((int32_t)br_paddle << 8);
        if(offset >= -(BR_PADDLE_HALF << 8) && offset <= (BR_PADDLE_HALF << 8)){
            nx = contact_x;
            ny = BR_PADDLE_CONTACT;
            br_vy = (int16_t)(-(650 + br_level*45));
            br_vx = (int16_t)(offset * 38 / 256);
            if(br_vx > -180 && br_vx < 180)br_vx = br_vx < 0 ? -180 : 180;
            br_combo = 0;
            sound_pending = 2u;
        }
    }
    if((ny >> 8) > 168){
        if(br_lives)--br_lives;
        if(!br_lives){ex_phase = EX_LOST;extra_record();}
        else{br_combo = 0;br_serve();ex_phase = EX_READY;}
        sound_pending = 3u;
        return;
    }
    br_ball_x = nx;
    br_ball_y = ny;
}

#endif
