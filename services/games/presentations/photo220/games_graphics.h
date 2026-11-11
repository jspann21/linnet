/* Native 220 x 176 presentation for the iPod Photo. Game state stays shared. */
static const uint16_t photo_white=0xefbf,photo_muted=0x9d95,photo_cyan=0x6f5d,photo_gold=0xfed3;
static void photo_label(const struct ln_ui_canvas_context *c,int x,int y,const char *s,uint16_t ink){arcade_text(c,x,y,s,ink,true);}
static void photo_center(const struct ln_ui_canvas_context *c,int y,const char *s,uint16_t ink){arcade_center(c,110,y,s,ink,true,1);}
static void photo_number(const struct ln_ui_canvas_context *c,int x,int y,uint32_t v,uint16_t ink){char s[12],*p=s;number(&p,v);*p=0;photo_label(c,x,y,s,ink);}
static void photo_footer(const struct ln_ui_canvas_context *c,const char *a,const char *b){
    box(c,0,153,220,23,0x0926);box(c,0,153,220,1,0x294d);
    photo_center(c,155,a,photo_white);photo_center(c,166,b,photo_muted);
}
static void photo_lives(const struct ln_ui_canvas_context *c){
    for(int i=0;i<3;++i){int x=181+i*12;uint16_t ink=i<hearts?0xfbce:0x294b;
        box(c,x,6,3,2,ink);box(c,x+4,6,3,2,ink);box(c,x,8,7,2,ink);box(c,x+1,10,5,1,ink);box(c,x+2,11,3,1,ink);}
}
static void photo_stars(const struct ln_ui_canvas_context *c,bool moving){
    uint32_t t=moving?last_tick/50000u:0;
    for(uint32_t i=0;i<28;++i){int x=4+(i*73u)%212u,y=42+(i*47u+t*(i%3+1))%109u;
        box(c,x,y,1,i%5?1:2,i%5?0x296a:0x7c96);}
}
static void photo_ship(const struct ln_ui_canvas_context *c,int x,int y){
    for(int dy=0;dy<20;++dy){int r=dy<13?dy/3:7-(dy-13)/2;box(c,x-r,y+dy,r*2+1,1,0x4cb8);box(c,x-r,y+dy,r+1,1,0xc73d);}
    box(c,x-1,y+7,3,7,0x19ae);box(c,x,y+8,1,4,photo_cyan);
    box(c,x-2,y+20,5,3+(last_tick/70000u)%3u,photo_gold);
}
static void photo_symbol(const struct ln_ui_canvas_context *c,int x,int y,int i,uint16_t ink){
    if(i==0){for(int d=0;d<9;++d)box(c,x-d,y-5+d,2*d+1,1,ink);}
    else if(i==1){arcade_disc(c,x,y,8,ink);arcade_disc(c,x,y,4,0x10a7);}
    else if(i==2){for(int d=-8;d<=8;++d){int r=8-(d<0?-d:d);box(c,x-r,y+d,r*2+1,1,ink);}}
    else{box(c,x-7,y-7,15,15,ink);box(c,x-3,y-3,7,7,0x10a7);}
}
static void photo_arcade(const struct ln_ui_canvas_context *c){
    static const uint16_t colors[]={0xfd4b,0x4edb,0xff30,0xbb7f};
    arcade_shade(c,0,0,220,176,0x112a,0x0023);
    if(games_phase==ARCADE_READY){
        photo_stars(c,false);photo_center(c,7,"LINNET ARCADE",photo_cyan);
        arcade_center(c,110,24,names[game],photo_white,false,2);
        if(game==COMET){arcade_disc(c,110,81,26,0x19ee);arcade_disc(c,110,81,24,0x08c9);photo_ship(c,110,67);
            arcade_disc(c,54,72,10,0xbc0e);arcade_disc(c,51,69,6,0xe553);arcade_disc(c,168,92,13,0xbc0e);arcade_disc(c,165,89,8,0xe553);}
        else{for(int i=0;i<4;++i){int x=77+(i==1||i==2?66:0),y=i>=2?96:66;arcade_disc(c,x,y,14,colors[i]);arcade_disc(c,x,y,12,0x10a7);photo_symbol(c,x,y,i,colors[i]);}}
        photo_center(c,115,game==COMET?"Wheel: steer   Select: shield":"Watch. Remember. Repeat.",photo_white);
        photo_center(c,128,game==COMET?"Dodge meteors. Protect your ship.":"Wheel: choose   Select: tap",photo_muted);
        arcade_panel(c,31,145,158,20,photo_cyan);photo_center(c,149,"Select / Start",0x0926);return;
    }
    photo_label(c,7,4,names[game],photo_white);photo_lives(c);
    if(game==COMET){
        photo_label(c,7,21,"SCORE",photo_muted);photo_number(c,46,21,score,photo_white);
        photo_label(c,88,21,"BEST",photo_muted);photo_number(c,120,21,best[game],photo_cyan);
        box(c,165,22,47,4,0x294b);box(c,165,22,energy*47/100,4,photo_cyan);box(c,183,21,1,6,photo_white);
        photo_stars(c,true);
        /* The shared arena uses logical coordinates; only the playfield maps here. */
        struct ln_ui_canvas_context field=*c;int end=c->clip_y+c->clip_h;if(end>151)end=151;
        if(field.clip_y<39)field.clip_y=39;field.clip_h=end>field.clip_y?end-field.clip_y:0;
        for(int i=0;i<12;++i)if(rock[i].live){int x=rock[i].x*220/320,y=39+(rock[i].y-55)*112/152,r=(rock[i].size*220+160)/320;
            for(int t=1;t<9;++t)box(&field,x-r/3,y-r-t,r/2+1,1,t<4?0x7188:0x30e7);
            arcade_disc(&field,x,y,r+1,0x4928);arcade_disc(&field,x,y,r,0xbc0e);arcade_disc(&field,x-1,y-1,r-2,0xe553);arcade_disc(&field,x+1,y+2,2,0x8aac);}
        int sx=ship*220/320;
        if((int32_t)(last_tick-invulnerable_until)<0){for(int dy=-16;dy<=16;++dy){int dx=17;while(dx*dx+dy*dy>289)--dx;
            box(&field,sx-dx,133+dy,1,1,photo_cyan);box(&field,sx+dx,133+dy,1,1,photo_cyan);}}
        photo_ship(&field,sx,123);
        if(burst_until&&(int32_t)(last_tick-burst_until)<0){int d=3+(350000u-(burst_until-last_tick))/50000u;
            for(int i=-1;i<=1;++i)for(int j=-1;j<=1;++j)if(i||j)box(&field,burst_x*220/320+i*d,39+(burst_y-55)*112/152+j*d,2,2,burst_shield?photo_cyan:photo_gold);}
        if(hit_until&&(int32_t)(last_tick-hit_until)<0){box(c,0,39,2,112,0xfbce);box(c,218,39,2,112,0xfbce);}
        bool notice=shield_notice_until&&(int32_t)(last_tick-shield_notice_until)<0;
        photo_center(c,31,notice?(shield_failed?"Shield needs 40% energy":"Shield active"):score<80?"DRIFT":score<200?"SURGE":"STORM",notice&&shield_failed?photo_gold:photo_cyan);
        photo_footer(c,"Wheel: steer   Select: shield","Play: pause   Menu: exit");
    }else{
        photo_label(c,7,21,"ROUND",photo_muted);photo_number(c,49,21,level,photo_white);
        photo_label(c,78,21,"PTS",photo_muted);photo_number(c,103,21,score,photo_white);
        photo_label(c,151,21,games_phase==WATCH?"WATCH":games_phase==MISTAKE?"RETRY":games_phase==PLAY?"YOUR TURN":"",photo_cyan);
        for(int i=0;i<4;++i){int x=7+(i==1||i==2?107:0),y=38+(i>=2?53:0);bool lit=flash==i+1,selected=focus==i&&games_phase==PLAY;
            arcade_panel(c,x,y+2,99,46,0x0022);arcade_panel(c,x,y,99,46,selected?photo_white:lit?colors[i]:0x294d);
            arcade_panel(c,x+2,y+2,95,42,0x1129);arcade_shade(c,x+4,y+4,91,38,arcade_mix(colors[i],0x1129,lit?195:30),0x1129);
            photo_symbol(c,x+25,y+23,i,lit?photo_white:colors[i]);
            static const char *const labels[]={"NOVA","ORBIT","PRISM","ECHO"};photo_label(c,x+45,y+11,labels[i],lit?photo_white:colors[i]);photo_number(c,x+45,y+25,i+1,photo_white);
            if(selected)box(c,x+85,y+32,5,2,photo_white);}
        uint32_t done=entered==255?0:games_phase==WATCH?entered+(flash?1:0):entered;int w=200/level;
        for(uint32_t i=0;i<level;++i)box(c,10+i*w,144,w-2,3,i<done?photo_cyan:0x294d);
        photo_footer(c,games_phase==WATCH?(entered==255&&level>1?"Round clear. One more light.":"Remember the order of the lights."):games_phase==MISTAKE?"One life lost. Watch again.":"Wheel: choose   Select: tap","Play: pause   Menu: exit");
    }
    if(games_phase==PAUSED||games_phase==LOST||games_phase==WON){
        arcade_panel(c,12,39,196,112,0x6474);arcade_panel(c,13,40,194,110,0x0926);
        if(games_phase==PAUSED){arcade_center(c,110,47,"Paused",photo_white,false,2);
            photo_center(c,82,game==COMET?"Select shields for 40% energy.":"Repeat the lights in order.",photo_muted);photo_center(c,96,game==COMET?"Energy recharges as you fly.":"Wheel or </> chooses a pad.",photo_muted);}
        else{photo_center(c,47,games_phase==WON?"ALL 24 ROUNDS CLEARED":score>previous_best?"NEW PERSONAL BEST":"RUN COMPLETE",photo_gold);
            char s[40],*p=s;number(&p,score);*p=0;arcade_center(c,110,63,s,photo_white,false,2);
            p=s;append(&p,"BEST ");number(&p,best[game]);append(&p," / ");number(&p,game==COMET?score/5:games_phase==WON?24:level-1);append(&p,game==COMET?" METEORS":" ROUNDS");*p=0;photo_center(c,100,s,photo_muted);}
        arcade_panel(c,32,124,156,19,photo_cyan);photo_center(c,128,games_phase==PAUSED?"Play / Resume":"Select / Play again",0x0926);
        photo_footer(c,games_phase==PAUSED?"Your game is paused.":"Ready for another round?","Menu: return to Games");
    }
}
/* Small cards retain native text and suit marks; court interiors use the licensed art. */
static void photo_suit(const struct ln_ui_canvas_context *c,int x,int y,int suit,uint16_t ink){
    static const uint8_t masks[4][6]={{4,14,4,27,31,4},{4,14,31,14,4,0},{27,31,31,14,4,0},{4,14,31,31,4,14}};
    for(int yy=0;yy<6;++yy)for(int xx=0;xx<5;++xx)if(masks[suit][yy]&(16>>xx))box(c,x+xx,y+yy,1,1,ink);
}
static void photo_card(const struct ln_ui_canvas_context *c,int x,int y,uint8_t card,bool back,int h){
    struct ln_ui_canvas_context clip=*c;int end=y+h;if(end>c->clip_y+c->clip_h)end=c->clip_y+c->clip_h;clip.clip_h=end>clip.clip_y?end-clip.clip_y:0;c=&clip;
    box(c,x,y,27,36,0xce79);box(c,x+1,y+1,25,34,back?0x1a0f:0xffff);
    if(back){for(int yy=3;yy<34;yy+=3)for(int xx=3;xx<25;xx+=3)box(c,x+xx,y+yy,1,1,0x6474);
        if(h>=36){for(int dy=-7;dy<=7;++dy){int r=7-(dy<0?-dy:dy);box(c,x+13-r,y+18+dy,r*2+1,1,0x8d37);}box(c,x+11,y+16,5,5,0x1a0f);}return;}
    uint8_t rank=sol_rank(card),suit=sol_suit(card);uint16_t ink=sol_red(card)?0xc8c3:0x18a7;
    char label[3]={0};if(rank==10){label[0]='1';label[1]='0';}else label[0]="A23456789TJQK"[rank-1];
    photo_label(c,x+2,y,label,ink);photo_suit(c,x+(rank==10?17:11),y+3,suit,ink);
    if(h<12)return;
    if(rank>=11){const uint8_t *pixels=sol_court_pixels[suit*3+rank-11];
        for(int yy=0;yy<23;++yy)for(int xx=0;xx<23;++xx){int px=x+2+xx,py=y+11+yy;
            if(px>=c->clip_x&&px<c->clip_x+c->clip_w&&py>=c->clip_y&&py<c->clip_y+c->clip_h&&px>=0&&px<220&&py>=0&&py<176)
                c->pixels[py*220+px]=sol_court_palette[pixels[yy*23+xx]];}}
    else if(rank==1)photo_suit(c,x+11,y+21,suit,ink);
    else{
        static const uint8_t px[]={4,18,4,18,4,18,4,18,11,11};
        static const uint8_t ys[9][10]={
            {13,27}, {13,20,27}, {13,13,27,27}, {13,13,27,27,20},
            {13,13,20,20,27,27}, {13,13,20,20,27,27,16},
            {13,13,20,20,27,27,16,24}, {11,11,17,17,23,23,29,29,20},
            {11,11,17,17,23,23,29,29,14,26}};
        for(int i=0;i<rank;++i){int dx=rank<=3?11:rank==5&&i==4?11:(rank==7||rank==8)&&i>=6?11:px[i];
            photo_suit(c,x+dx,y+ys[rank-2][i],suit,ink);}
    }
}
static void photo_outline(const struct ln_ui_canvas_context *c,int x,int y,int h,uint16_t ink){box(c,x-1,y-1,29,1,ink);box(c,x-1,y+h,29,1,ink);box(c,x-1,y,1,h,ink);box(c,x+27,y,1,h,ink);}
static void photo_slot(const struct ln_ui_canvas_context *c,int x,int y){arcade_panel(c,x,y,27,36,0x43d0);arcade_panel(c,x+1,y+1,25,34,0x1269);}
static void photo_solitaire(const struct ln_ui_canvas_context *c){
    box(c,0,0,220,176,0x0926);arcade_shade(c,0,32,220,121,0x1b4a,0x11e7);
    photo_label(c,5,3,"Solitaire",photo_white);char s[64],*p=s;append(&p,"MOVES ");number(&p,sol_moves);append(&p,"  WINS ");number(&p,best[SOLITAIRE]);*p=0;photo_label(c,90,3,s,photo_muted);
    uint8_t spot=sol_source!=255?sol_source:sol_cursor,depth=sol_source!=255?sol_source_depth:sol_depth,card=sol_visible_card(spot,depth);
    if(card!=255){static const char *const ranks[]={"Ace","2","3","4","5","6","7","8","9","10","Jack","Queen","King"},*const suits[]={"Clubs","Diamonds","Hearts","Spades"};
        p=s;append(&p,ranks[sol_rank(card)-1]);append(&p," of ");append(&p,suits[sol_suit(card)]);*p=0;}
    else if(!spot){p=s;append(&p,sol_stock_count?"Stock / ":"Recycle / ");number(&p,sol_stock_count?sol_stock_count:sol_waste_count);*p=0;}
    else if(spot==1){p=s;append(&p,"Waste is empty");*p=0;}
    else if(spot<6){static const char *const labels[]={"Clubs home","Diamonds home","Hearts home","Spades home"};p=s;append(&p,labels[spot-2]);*p=0;}
    else{p=s;append(&p,"Empty / Kings only");*p=0;}
    photo_label(c,5,19,s,sol_source!=255?photo_cyan:photo_gold);
    if(sol_source!=255){p=s;number(&p,sol_source>=6?sol_count[sol_source-6]-sol_source_depth:1);append(&p," HELD");*p=0;photo_label(c,166,19,s,photo_cyan);}
    else photo_label(c,166,19,"DRAW 1",photo_muted);
    static const uint8_t xs[]={3,34,96,127,158,189};
    for(int i=0;i<6;++i){int x=xs[i];photo_slot(c,x,35);
        if(!i){if(sol_stock_count)photo_card(c,x,35,0,true,36);else{photo_label(c,x+5,46,"<> ",sol_waste_count?photo_gold:photo_muted);}}
        else if(i==1&&sol_waste_count)photo_card(c,x,35,sol_waste[sol_waste_count-1],false,36);
        else if(i>=2){if(sol_foundation[i-2])photo_card(c,x,35,(i-2)*13+sol_foundation[i-2]-1,false,36);else photo_suit(c,x+11,50,i-2,i==3||i==4?0xc391:0x8d75);}
        if(sol_can_place(i))photo_outline(c,x,35,36,0x7f56);if(sol_source==i)photo_outline(c,x,35,36,photo_cyan);if(sol_cursor==i)photo_outline(c,x,35,36,photo_gold);
    }
    bool preview=sol_source!=255;
    for(int col=0;col<7;++col){int x=3+col*31,y=79,len=sol_count[col],hidden=sol_hidden[col],faces=len-hidden,step=11;
        if(faces>1){step=(36-hidden*2)/(faces-1);if(step>11)step=11;if(step<2)step=2;}
        if(!len){photo_slot(c,x,y);photo_label(c,x+10,y+13,"K",photo_muted);}
        for(int i=0;i<len;++i)photo_card(c,x,y+(i<hidden?i*2:hidden*2+(i-hidden)*step),sol_tableau[col][i],i<hidden,i==len-1?36:i<hidden?2:step);
        int last=y+hidden*2+(faces?faces-1:0)*step;
        if(sol_can_place(col+6))photo_outline(c,x,len?last:y,36,0x7f56);
        if(sol_cursor==col+6){int at=len?y+hidden*2+(sol_depth-hidden)*step:y;photo_outline(c,x,at,len&&sol_depth+1<len?step:36,photo_gold);if(step<11&&card!=255)preview=true;}
        if(sol_source==col+6){int at=y+hidden*2+(sol_source_depth-hidden)*step;photo_outline(c,x,at,last+36-at,photo_cyan);}
    }
    if(preview&&card!=255){photo_card(c,65,35,card,false,36);photo_outline(c,65,35,36,photo_cyan);}
    else{int n=0;for(int i=0;i<4;++i)n+=sol_foundation[i];p=s;number(&p,n);append(&p,"/52");*p=0;arcade_center(c,78,44,s,photo_gold,true,1);arcade_center(c,78,57,"HOME",photo_muted,true,1);}
    const char *hint=sol_source!=255?(sol_can_place(sol_cursor)?"Select: place / Green: legal piles":"Wheel: target / Menu: cancel"):!sol_cursor?(sol_stock_count?"Select: draw a card":"Select: recycle waste"):sol_cursor>=6?"Select: pick / </>: choose card":"Select: pick up";
    if(sol_message){if(sol_message[0]=='A')hint="Use opposite color, one rank lower.";else if(sol_message[0]=='B')hint="Build suits from Ace to King.";else if(sol_message[0]=='C'&&sol_message[1]=='h')hint="Choose a column or foundation.";else hint=sol_message;}
    photo_footer(c,hint,sol_source!=255?"Wheel: target  Play: options  Menu: cancel":"Wheel: pile  Play: options  Menu: back");
    if(games_phase==PAUSED||games_phase==SOL_CONFIRM||games_phase==WON){
        arcade_panel(c,13,34,194,117,0xded9);arcade_panel(c,14,35,192,115,0x0926);
        if(games_phase==PAUSED){arcade_center(c,110,39,"Paused",photo_white,false,2);static const char *const labels[]={"Resume game","Undo last move","New deal"};
            for(int i=0;i<3;++i){int y=70+i*23;if(sol_menu==i)arcade_panel(c,25,y,170,21,photo_gold);photo_label(c,38,y+5,labels[i],sol_menu==i?0x0926:i==1&&!sol_history_count?0x6474:photo_white);}
            photo_footer(c,sol_message?sol_message:"Wheel: choose / Select: confirm","Play or Menu: resume");
        }else if(games_phase==SOL_CONFIRM){arcade_center(c,110,41,"New deal?",photo_white,false,2);photo_center(c,78,"Replace this game and clear",photo_muted);photo_center(c,92,"its undo history?",photo_muted);
            arcade_panel(c,28,118,164,21,photo_gold);photo_center(c,123,"Select / Deal again",0x0926);photo_footer(c,"Keep playing?","Menu or Play: cancel");
        }else{photo_center(c,44,"BEAUTIFULLY PLAYED",photo_gold);arcade_center(c,110,65,"You won",photo_white,false,2);p=s;number(&p,sol_moves);append(&p," moves / ");number(&p,best[SOLITAIRE]);append(&p," wins");*p=0;photo_center(c,97,s,photo_muted);
            arcade_panel(c,28,121,164,21,photo_gold);photo_center(c,126,"Select / New deal",0x0926);photo_footer(c,"All four suits are home.","Menu: return to Games");}
    }
}
#include "services/games/presentations/photo220/games_sudoku.h"
#include "services/games/presentations/games_extra_draw.h"
#include "services/games/presentations/photo220/collection.h"
void ln_games_draw(const struct ln_ui_canvas_context *c){
    if(game>=DIAL_HEIST)p_collection_draw(c);else if(game==SCRUB)scr_draw(c);
    else if(game>=BRICK)extra_draw(c);else if(game==SUDOKU)photo_sudoku(c);
    else if(game==SOLITAIRE)photo_solitaire(c);else photo_arcade(c);
}
