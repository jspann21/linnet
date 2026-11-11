static void arcade_value(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint32_t value,uint16_t ink){
    char text[12],*p=text;number(&p,value);*p=0;arcade_text(c,x,y,text,ink,false);
}
static void arcade_ship(const struct ln_ui_canvas_context *c,int32_t x,int32_t top,int32_t scale){
    uint32_t flame=5u+(last_tick/70000u)%5u;
    for(int32_t dy=0;dy<(int32_t)flame;++dy)box(c,x+(-3+dy/3)*scale,top+(24+dy)*scale,(6-2*(dy/3))*scale,scale,dy<4?0xffd6u:0xfc88u);
    for(int32_t dy=0;dy<24*scale;++dy){int32_t half=dy<15*scale?dy/4:10*scale-(dy-15*scale)/2;
        box(c,x-half,top+dy,half*2+1,1,0x4cb8u);
        box(c,x-half,top+dy,half+1,1,0xc73du);
        box(c,x,top+dy,1,1,0xf7ffu);}
    box(c,x-2*scale,top+9*scale,5*scale,8*scale,0x19aeu);
    box(c,x-scale,top+10*scale,2*scale,5*scale,0x6f5du);
    box(c,x-10*scale,top+20*scale,4*scale,4*scale,0xfeb2u);box(c,x+7*scale,top+20*scale,4*scale,4*scale,0xfeb2u);
}
static void arcade_sky(const struct ln_ui_canvas_context *c,bool moving){
    uint32_t phase=moving?last_tick/35000u:0;
    for(uint32_t i=0;i<38u;++i){int32_t x=(int32_t)((i*113u+29u)%310u)+5,y=59+(int32_t)((i*71u+phase*(i%3u+1u))%148u);
        box(c,x,y,1,i%4u?1:3,i%4u?0x296au:0x6bd4u);}
}
static void arcade_lives(const struct ln_ui_canvas_context *c){
    for(uint32_t i=0;i<3u;++i){int32_t x=268+(int32_t)i*16;uint16_t ink=i<hearts?0xfbceu:0x294bu;
        box(c,x,12,4,3,ink);box(c,x+6,12,4,3,ink);box(c,x,15,10,3,ink);
        box(c,x+2,18,6,2,ink);box(c,x+4,20,2,2,ink);}
}
static void arcade_symbol(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,uint32_t i,uint16_t ink){
    if(i==0u){for(int32_t r=0;r<11;++r)box(c,x-r,y-10+r,2*r+1,1,ink);}
    else if(i==1u){arcade_disc(c,x,y-2,9,ink);arcade_disc(c,x,y-2,5,0x10a7u);}
    else if(i==2u){for(int32_t r=-10;r<=10;++r){int32_t w=10-(r<0?-r:r);box(c,x-w,y+r-2,w*2+1,1,ink);}}
    else{box(c,x-9,y-11,18,18,ink);box(c,x-5,y-7,10,10,0x10a7u);}
}
static void arcade_launch(const struct ln_ui_canvas_context *c){
    const uint16_t white=0xefbfu,muted=0x94d6u,cyan=0x6f5du;
    arcade_shade(c,0,0,320,240,0x112au,0x0023u);arcade_sky(c,false);
    arcade_center(c,160,13,game==COMET?"LINNET ARCADE  /  01":"LINNET ARCADE  /  02",muted,true,1);
    arcade_center(c,160,32,names[game],white,false,2);
    if(game==COMET){
        arcade_disc(c,160,108,34,0x19eeu);arcade_disc(c,160,108,32,0x08c9u);
        arcade_disc(c,83,97,13,0x9b0eu);arcade_disc(c,80,93,8,0xdd73u);arcade_disc(c,86,100,4,0x61eau);
        arcade_disc(c,241,85,17,0x9b0eu);arcade_disc(c,237,81,12,0xdd73u);arcade_disc(c,247,89,5,0x61eau);
        arcade_ship(c,160,78,2);
    }else{static const uint16_t colors[]={0xfd4bu,0x4edbu,0xff30u,0xbb7fu};
        box(c,127,90,65,1,0x294du);box(c,127,124,65,1,0x294du);box(c,127,90,1,35,0x294du);box(c,192,90,1,35,0x294du);
        for(uint32_t i=0;i<4u;++i){int32_t x=i==1u||i==2u?192:127,y=i>=2u?124:86;
            arcade_disc(c,x,y,18,colors[i]);arcade_disc(c,x,y,16,0x10a7u);arcade_symbol(c,x,y+2,i,colors[i]);}}
    arcade_center(c,160,151,game==COMET?"Find your way through the meteor field.":"A little focus. A longer pattern.",white,true,1);
    arcade_center(c,160,169,game==COMET?"Turn the wheel to steer. Select shields.":"Watch the lights. Turn to choose. Select taps.",muted,true,1);
    arcade_center(c,160,183,game==COMET?"Three lives. Shields recharge as you fly.":"Three chances to clear all 24 rounds.",muted,true,1);
    arcade_panel(c,62,202,196,24,cyan);arcade_center(c,160,207,game==COMET?"Select  /  Launch":"Select  /  Begin",0x0865u,false,1);
    arcade_text(c,12,229,"Menu: back",muted,true);
    char label[24],*p=label;append(&p,"BEST  ");number(&p,best[game]);*p=0;arcade_text(c,236,229,label,muted,true);
}
static void arcade_draw(const struct ln_ui_canvas_context *c){
    const uint16_t navy=0x0865u,muted=0x94d6u,white=0xefbfu,cyan=0x6f5du;
    if(games_phase==ARCADE_READY){arcade_launch(c);return;}
    box(c,0,0,320,240,navy);box(c,0,0,320,31,0x10e8u);
    arcade_text(c,12,6,names[game],white,false);arcade_lives(c);
    box(c,0,30,320,1,0x294bu);
    if(game==COMET){
        arcade_text(c,12,36,"SCORE",muted,true);arcade_value(c,53,33,score,white);
        arcade_text(c,114,36,"BEST",muted,true);arcade_value(c,148,33,best[game],cyan);
        bool notice=shield_notice_until&&(int32_t)(last_tick-shield_notice_until)<0;
        arcade_text(c,219,36,notice?(shield_failed?"NEED 40%":"SHIELD ON"):energy>=40u?"SHIELD READY":"RECHARGING",notice&&shield_failed?0xfd4bu:energy>=40u||notice?cyan:muted,true);
        box(c,220,51,88,3,0x294bu);box(c,220,51,(int32_t)(energy*88u/100u),3,cyan);
        box(c,255,50,1,5,white);
        const struct ln_ui_canvas_context *full=c;
        struct ln_ui_canvas_context field=*c;
        int32_t bottom=c->clip_y+c->clip_h;if(bottom>207)bottom=207;
        if(field.clip_y<58)field.clip_y=58;
        field.clip_h=bottom>field.clip_y?bottom-field.clip_y:0;c=&field;
        arcade_sky(c,true);
        for(uint32_t i=0;i<12u;++i)if(rock[i].live){int32_t x=rock[i].x,y=rock[i].y,r=rock[i].size;
            /* A tapered tail and shaded stone keep the solid collision body clear. */
            for(int32_t t=1;t<14;++t)box(c,x-r/3,y-r-t,r*2/3+1,1,t<6?0x7188u:0x30e7u);
            arcade_disc(c,x,y,r+1,0x4928u);arcade_disc(c,x,y,r,0xbc0eu);
            arcade_disc(c,x-2,y-2,r>7?r-3:r-2,0xe553u);
            arcade_disc(c,x+r/3,y+r/3,r/3,0x8aacu);box(c,x-r/2,y-r/2,r/2+1,2,0xfedbu);
        }
        bool shield=(int32_t)(last_tick-invulnerable_until)<0;
        if(shield){for(int32_t dy=-21;dy<=21;++dy){int32_t dx=22;while(dx*dx+dy*dy>22*22)--dx;
            box(c,(int32_t)ship-dx,183+dy,2,1,cyan);box(c,(int32_t)ship+dx-1,183+dy,2,1,cyan);}}
        arcade_ship(c,ship,168,1);
        if(burst_until&&(int32_t)(last_tick-burst_until)<0){
            static const int8_t vx[]={-3,-2,0,2,3,2,0,-2},vy[]={0,-2,-3,-2,0,2,3,2};
            uint32_t age=350000u-(burst_until-last_tick);int32_t step=3+(int32_t)(age/45000u);
            for(uint32_t i=0;i<8u;++i)box(c,burst_x+vx[i]*step/2,burst_y+vy[i]*step/2,2,2,burst_shield?cyan:0xfd4bu);
        }
        c=full;
        box(c,0,207,320,1,0x294bu);
        if(hit_until&&(int32_t)(last_tick-hit_until)<0){box(c,0,58,3,147,0xfbceu);box(c,317,58,3,147,0xfbceu);}
        if(games_phase==PLAY){arcade_text(c,12,212,"Wheel: steer    Select: shield",white,true);
            arcade_text(c,252,212,score<80u?"DRIFT":score<200u?"SURGE":"STORM",cyan,true);}
    }else{
        static const uint16_t colors[]={0xfd4bu,0x4edbu,0xff30u,0xbb7fu};
        arcade_text(c,12,36,"ROUND",muted,true);arcade_value(c,57,33,level,white);
        arcade_text(c,93,36,"SCORE",muted,true);arcade_value(c,134,33,score,white);
        const char *status=games_phase==WATCH?(entered==255u?"GET READY":"WATCH"):games_phase==MISTAKE?"TRY AGAIN":games_phase==PLAY?"YOUR TURN":"";
        arcade_text(c,222,36,status,games_phase==MISTAKE?0xfd4bu:cyan,true);
        /* Clockwise numbering follows the click wheel through the four corners. */
        for(uint32_t i=0;i<4u;++i){int32_t x=16+(i==1u||i==2u?150:0),y=59+(i>=2u?67:0);
            bool selected=focus==i&&games_phase==PLAY,lit=flash==i+1u;
            uint16_t color=colors[i];
            arcade_panel(c,x,y+3,138,59,0x0022u);
            arcade_panel(c,x,y,138,59,selected?white:lit?color:0x294du);
            arcade_panel(c,x+2,y+2,134,55,0x1129u);
            arcade_shade(c,x+4,y+4,130,51,arcade_mix(color,0x1129u,lit?180:35),0x1129u);
            box(c,x+9,y+6,120,1,lit?white:arcade_mix(color,0x1129u,80));
            arcade_disc(c,x+36,y+29,19,arcade_mix(color,0x10a7u,lit?200:30));
            arcade_disc(c,x+36,y+29,16,0x10a7u);arcade_symbol(c,x+36,y+31,i,lit?white:color);
            static const char *const labels[]={"NOVA","ORBIT","PRISM","ECHO"};
            arcade_text(c,x+66,y+13,labels[i],lit?white:color,true);
            arcade_value(c,x+66,y+29,i+1u,white);
            if(selected){box(c,x+112,y+34,9,2,white);box(c,x+117,y+31,2,8,white);}
        }
        /* One segment per beat: filled segments are accepted input or shown beats. */
        uint32_t done=entered==255u?0u:games_phase==WATCH?entered+(flash?1u:0u):entered;
        int32_t width=level?280/(int32_t)level:280;
        for(uint32_t i=0;i<level;++i)box(c,20+(int32_t)i*width,194,width-2,4,i<done?cyan:0x294du);
        if(games_phase==WATCH||games_phase==MISTAKE||games_phase==PLAY)
            arcade_text(c,16,207,games_phase==WATCH?(entered==255u&&level>1u?"Round clear. One more light joins the path.":"Watch the lights. Remember the order."):games_phase==MISTAKE?"One life lost. Take another look.":"Wheel: choose    Select: tap",white,true);
    }
    arcade_text(c,12,227,games_phase==PAUSED?"Play: resume":games_phase==LOST||games_phase==WON?"Select: retry":"Play: pause",muted,true);arcade_text(c,236,227,"Menu: exit",muted,true);
    if(games_phase==PAUSED||games_phase==LOST||games_phase==WON){
        box(c,0,55,320,160,navy);
        arcade_panel(c,20,61,280,146,0x294du);arcade_panel(c,21,62,278,144,0x10e8u);
        arcade_shade(c,24,65,272,136,0x196bu,0x10c8u);
        if(games_phase==PAUSED){
            arcade_center(c,160,76,"Paused",white,false,2);
            arcade_center(c,160,118,game==COMET?"Wheel or </> steers. Select shields.":"Wheel or </> chooses. Select taps.",white,true,1);
            arcade_center(c,160,137,game==COMET?"Shields use 40% energy and recharge.":"Repeat the lights in the same order.",muted,true,1);
        }else{
            bool record=score>previous_best;
            arcade_center(c,160,74,games_phase==WON?"ALL 24 ROUNDS CLEARED":record?"NEW PERSONAL BEST":game==COMET?"RUN COMPLETE":"OUT OF LIVES",record?0xff30u:cyan,true,1);
            char label[40],*p=label;number(&p,score);*p=0;
            arcade_center(c,160,93,label,white,false,2);
            arcade_center(c,160,128,"POINTS",muted,true,1);
            p=label;append(&p,"BEST  ");number(&p,best[game]);append(&p,"    ");
            if(game==COMET){number(&p,score/5u);append(&p," METEORS");}
            else{number(&p,games_phase==WON?24u:level-1u);append(&p," ROUNDS CLEARED");}
            *p=0;arcade_center(c,160,148,label,muted,true,1);
        }
        arcade_panel(c,55,175,210,24,cyan);
        arcade_center(c,160,180,games_phase==PAUSED?"Play  /  Resume":"Select  /  Play again",navy,false,1);
    }
}
