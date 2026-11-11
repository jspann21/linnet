#ifndef LINNET_GAME_FALLING_H
#define LINNET_GAME_FALLING_H

/* Falling Blocks */
static const uint16_t fb_shape[7][4]={
    {0x0f00,0x2222,0x00f0,0x4444},{0x0660,0x0660,0x0660,0x0660},{0x0720,0x2620,0x2700,0x2320},
    {0x0710,0x2260,0x4700,0x3220},{0x0740,0x6220,0x1700,0x2230},{0x0360,0x4620,0x3600,0x2310},{0x0630,0x2640,0x6300,0x1320}};
static uint8_t fb_board[180],fb_piece,fb_next,fb_rot,fb_y,fb_bag[7],fb_bag_at;
static int8_t fb_x;
static uint16_t fb_level,fb_lines,fb_combo;
static uint32_t fb_lock_at;static uint8_t fb_lock_resets;
static void fb_shuffle(void){for(uint8_t i=0;i<7u;++i)fb_bag[i]=i;for(uint8_t i=6u;i;i--){uint8_t j=(uint8_t)(random_next()%(i+1u)),t=fb_bag[i];fb_bag[i]=fb_bag[j];fb_bag[j]=t;}fb_bag_at=0;}
static uint8_t fb_take(void){if(fb_bag_at>=7u)fb_shuffle();return fb_bag[fb_bag_at++];}
static bool fb_hit(int8_t x,int8_t y,uint8_t rot){uint16_t bits=fb_shape[fb_piece][rot];for(uint8_t r=0;r<4u;++r)for(uint8_t c=0;c<4u;++c)if(bits&(1u<<(15u-r*4u-c))){int8_t xx=x+c,yy=y+r;if(xx<0||xx>=10||yy>=18||(yy>=0&&fb_board[yy*10+xx]))return true;}return false;}
static void fb_spawn(void){fb_piece=fb_next;fb_next=fb_take();fb_rot=0;fb_x=3;fb_y=0;fb_lock_at=0;fb_lock_resets=0;if(fb_hit(fb_x,fb_y,fb_rot)){ex_phase=EX_LOST;extra_record();}}
static void fb_start(void){for(uint16_t i=0;i<180u;++i)fb_board[i]=0;fb_shuffle();fb_next=fb_take();fb_level=1;fb_lines=fb_combo=0;fb_spawn();ex_phase=EX_READY;}
static void fb_lock(void){uint16_t bits=fb_shape[fb_piece][fb_rot];for(uint8_t r=0;r<4u;++r)for(uint8_t c=0;c<4u;++c)if(bits&(1u<<(15u-r*4u-c)))fb_board[(fb_y+r)*10+fb_x+c]=(uint8_t)(fb_piece+1u);
    uint8_t cleared=0;for(int8_t r=17;r>=0;--r){bool full=true;for(uint8_t c=0;c<10u;++c)if(!fb_board[r*10+c])full=false;if(full){for(int8_t y=r;y>0;--y)for(uint8_t c=0;c<10u;++c)fb_board[y*10+c]=fb_board[(y-1)*10+c];for(uint8_t c=0;c<10u;++c)fb_board[c]=0;++cleared;++r;}}
    if(cleared){fb_lines=fb_lines>65535u-cleared?65535u:(uint16_t)(fb_lines+cleared);fb_level=(uint16_t)(1u+fb_lines/10u);if(fb_combo<65535u)++fb_combo;uint32_t add=(uint32_t)cleared*cleared*100u*fb_level+(uint32_t)fb_combo*25u;score=(uint16_t)(add>=65535u-score?65535u:score+add);ex_effect_until=ex_tick_at+180000u;sound_pending=2u;}else fb_combo=0;fb_spawn();}
static void fb_drop(bool hard){if(hard){uint8_t cells=0;while(!fb_hit(fb_x,(int8_t)(fb_y+1u),fb_rot)){++fb_y;++cells;}score=(uint16_t)(score>65535u-cells*2u?65535u:score+cells*2u);fb_lock();return;}if(!fb_hit(fb_x,(int8_t)(fb_y+1u),fb_rot)){++fb_y;fb_lock_at=0;}else if(!fb_lock_at)fb_lock_at=ex_tick_at+350000u;else if((int32_t)(ex_tick_at-fb_lock_at)>=0)fb_lock();}

#endif
