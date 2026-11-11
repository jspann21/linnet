#ifndef LINNET_GAME_REVERSI_H
#define LINNET_GAME_REVERSI_H

/* Reversi */
static uint8_t rv_board[64],rv_cursor,rv_turn,rv_last,rv_pass;
static int8_t rv_dx[8]={-1,0,1,-1,1,-1,0,1},rv_dy[8]={-1,-1,-1,0,0,1,1,1};
static uint8_t rv_flips(uint8_t at,uint8_t who,bool place){if(rv_board[at])return 0;uint8_t total=0,x=at&7u,y=at>>3;for(uint8_t d=0;d<8u;++d){int8_t xx=x+rv_dx[d],yy=y+rv_dy[d];uint8_t count=0;while(xx>=0&&xx<8&&yy>=0&&yy<8&&rv_board[yy*8+xx]==3u-who){++count;xx+=rv_dx[d];yy+=rv_dy[d];}if(count&&xx>=0&&xx<8&&yy>=0&&yy<8&&rv_board[yy*8+xx]==who){total+=count;if(place)for(uint8_t n=1;n<=count;++n)rv_board[(y+rv_dy[d]*n)*8+x+rv_dx[d]*n]=who;}}if(place&&total)rv_board[at]=who;return total;}
static uint8_t rv_next(uint8_t from,int8_t dir,uint8_t who){for(uint8_t n=0;n<64u;++n){from=(uint8_t)(from+dir)&63u;if(rv_flips(from,who,false))return from;}return from;}
static bool rv_has(uint8_t who){for(uint8_t i=0;i<64u;++i)if(rv_flips(i,who,false))return true;return false;}
static void rv_finish(void){uint8_t a=0,b=0;for(uint8_t i=0;i<64u;++i){a+=rv_board[i]==1u;b+=rv_board[i]==2u;}score=a;ex_phase=a>b?EX_WON:a==b?EX_REVEAL:EX_LOST;if(a>b&&best[REVERSI]!=UINT16_MAX){++best[REVERSI];
#if defined(LINNET_STORAGE)
        ln_display_game_best_set(REVERSI,best[REVERSI]);
#endif
    }sound_pending=4u;}
static void rv_ai(void){static const int8_t weight[64]={30,-8,4,2,2,4,-8,30,-8,-12,-2,-2,-2,-2,-12,-8,4,-2,3,1,1,3,-2,4,2,-2,1,1,1,1,-2,2,2,-2,1,1,1,1,-2,2,4,-2,3,1,1,3,-2,4,-8,-12,-2,-2,-2,-2,-12,-8,30,-8,4,2,2,4,-8,30};
    for(uint8_t turn=0;turn<64u;++turn){int16_t best_value=-30000;uint8_t move=255;
        for(uint8_t i=0;i<64u;++i){uint8_t f=rv_flips(i,2u,false);if(f){int16_t v=weight[i]+f*3+(int16_t)(random_next()&3u);if(v>best_value){best_value=v;move=i;}}}
        if(move==255u){if(!rv_has(1u))rv_finish();else{rv_turn=1u;rv_pass=2u;rv_cursor=rv_next(rv_last,1,1u);}return;}
        rv_flips(move,2u,true);rv_last=move;sound_pending=2u;
        if(rv_has(1u)){rv_turn=1u;rv_cursor=rv_next(rv_last,1,1u);return;}
        if(!rv_has(2u)){rv_finish();return;}rv_pass=1u;
    }
    rv_finish();
}
static void rv_start(void){for(uint8_t i=0;i<64u;++i)rv_board[i]=0;rv_board[27]=rv_board[36]=2;rv_board[28]=rv_board[35]=1;rv_turn=1;rv_last=255;rv_pass=0;rv_cursor=rv_next(18,1,1u);ex_phase=EX_PLAY;}

#endif
