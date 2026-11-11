#ifndef LINNET_GAME_MINESWEEPER_H
#define LINNET_GAME_MINESWEEPER_H

/* Minesweeper */
static uint8_t ms_mine[192],ms_open[192],ms_flag[192],ms_queue[192],ms_queued[192],ms_w,ms_h,ms_total,ms_cursor,ms_generated,ms_level;
static uint16_t ms_opened;
static uint32_t ms_started_at,ms_elapsed;
static uint8_t ms_adj(uint8_t at){uint8_t x=at%ms_w,y=at/ms_w,n=0;for(int8_t dy=-1;dy<=1;++dy)for(int8_t dx=-1;dx<=1;++dx){int16_t xx=x+dx,yy=y+dy;if((dx||dy)&&xx>=0&&yy>=0&&xx<ms_w&&yy<ms_h)n+=ms_mine[yy*ms_w+xx];}return n;}
static void ms_start_level(uint8_t level){ms_level=level;ms_w=level==0?9u:level==1?12u:16u;ms_h=level==0?9u:level==1?10u:12u;ms_total=level==0?10u:level==1?20u:40u;for(uint16_t i=0;i<192u;++i)ms_mine[i]=ms_open[i]=ms_flag[i]=0;ms_cursor=ms_generated=0;ms_opened=0;ms_elapsed=0;ms_started_at=0;ex_phase=EX_PLAY;}
static void ms_generate(uint8_t safe){uint16_t cells=ms_w*ms_h,placed=0;while(placed<ms_total){uint8_t p=(uint8_t)(random_next()%cells);int16_t dx=(p%ms_w)-(safe%ms_w),dy=(p/ms_w)-(safe/ms_w);if(ms_mine[p]||(dx>=-1&&dx<=1&&dy>=-1&&dy<=1))continue;ms_mine[p]=1;++placed;}ms_generated=1;}
static void ms_reveal(uint8_t at){if(ms_flag[at])return;if(ms_open[at]){uint8_t needed=ms_adj(at),flags=0,x=at%ms_w,y=at/ms_w;for(int8_t dy=-1;dy<=1;++dy)for(int8_t dx=-1;dx<=1;++dx){int16_t xx=x+dx,yy=y+dy;if((dx||dy)&&xx>=0&&yy>=0&&xx<ms_w&&yy<ms_h)flags+=ms_flag[yy*ms_w+xx];}if(flags==needed)for(int8_t dy=-1;dy<=1&&ex_phase==EX_PLAY;++dy)for(int8_t dx=-1;dx<=1&&ex_phase==EX_PLAY;++dx){int16_t xx=x+dx,yy=y+dy;if((dx||dy)&&xx>=0&&yy>=0&&xx<ms_w&&yy<ms_h&&!ms_open[yy*ms_w+xx])ms_reveal((uint8_t)(yy*ms_w+xx));}return;}if(!ms_generated)ms_generate(at);uint16_t cells=ms_w*ms_h,head=0,tail=0;for(uint16_t i=0;i<cells;++i)ms_queued[i]=0;ms_queue[tail++]=at;ms_queued[at]=1;while(head<tail){uint8_t p=ms_queue[head++];if(ms_open[p]||ms_flag[p])continue;ms_open[p]=1;++ms_opened;if(ms_mine[p]){ex_phase=EX_LOST;for(uint16_t i=0;i<cells;++i)if(ms_mine[i])ms_open[i]=1;sound_pending=3u;return;}if(!ms_adj(p)){uint8_t x=p%ms_w,y=p/ms_w;for(int8_t dy=-1;dy<=1;++dy)for(int8_t dx=-1;dx<=1;++dx){int16_t xx=x+dx,yy=y+dy;if(xx>=0&&yy>=0&&xx<ms_w&&yy<ms_h){uint8_t q=(uint8_t)(yy*ms_w+xx);if(!ms_open[q]&&!ms_queued[q]&&tail<cells){ms_queued[q]=1;ms_queue[tail++]=q;}}}}}
    if(ms_opened+ms_total==ms_w*ms_h){ex_phase=EX_WON;score=(uint16_t)(65535u-(ms_elapsed>65534u?65534u:ms_elapsed));sound_pending=4u;}else sound_pending=1u;}

#endif
