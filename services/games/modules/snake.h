#ifndef LINNET_GAME_SNAKE_H
#define LINNET_GAME_SNAKE_H

/* Snake */
static uint8_t sn_x[240],sn_y[240],sn_len,sn_dir,sn_turn,sn_food_x,sn_food_y,sn_boost;
static uint32_t sn_move_at;
static bool sn_on_count(uint8_t x,uint8_t y,uint8_t count){for(uint8_t i=0;i<count;++i)if(sn_x[i]==x&&sn_y[i]==y)return true;return false;}
static bool sn_on(uint8_t x,uint8_t y){return sn_on_count(x,y,sn_len);}
static void sn_food(void){do{sn_food_x=(uint8_t)(random_next()%20u);sn_food_y=(uint8_t)(random_next()%12u);}while(sn_on(sn_food_x,sn_food_y));}
static void sn_start(void){sn_len=5;sn_dir=0;sn_turn=255;sn_boost=0;for(uint8_t i=0;i<sn_len;++i){sn_x[i]=(uint8_t)(10u-i);sn_y[i]=6u;}sn_food();ex_phase=EX_READY;}
static void sn_step(void){if(sn_turn!=255u){sn_dir=sn_turn;sn_turn=255u;}int8_t dx[4]={1,0,-1,0},dy[4]={0,1,0,-1};int16_t x=sn_x[0]+dx[sn_dir],y=sn_y[0]+dy[sn_dir];bool eat=x==sn_food_x&&y==sn_food_y;if(x<0||x>=20||y<0||y>=12||sn_on_count((uint8_t)x,(uint8_t)y,(uint8_t)(sn_len-(eat?0u:1u)))){ex_phase=EX_LOST;extra_record();sound_pending=3u;return;}if(eat&&sn_len<239u)++sn_len;for(uint8_t i=sn_len-1u;i;--i){sn_x[i]=sn_x[i-1u];sn_y[i]=sn_y[i-1u];}sn_x[0]=(uint8_t)x;sn_y[0]=(uint8_t)y;if(eat){score=(uint16_t)(score>65435u?65535u:score+100u);sn_food();sound_pending=2u;}if(sn_boost)--sn_boost;}

#endif
