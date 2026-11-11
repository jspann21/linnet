/* Native 220 x 176 artwork. All geometry is drawn in display coordinates. */
static const int8_t p_x32[32]={0,12,24,36,45,53,59,63,64,63,59,53,45,36,24,12,
    0,-12,-24,-36,-45,-53,-59,-63,-64,-63,-59,-53,-45,-36,-24,-12};
static const int8_t p_y32[32]={-64,-63,-59,-53,-45,-36,-24,-12,0,12,24,36,45,53,59,63,
    64,63,59,53,45,36,24,12,0,-12,-24,-36,-45,-53,-59,-63};
static const int8_t p_x12[12]={0,32,55,64,55,32,0,-32,-55,-64,-55,-32};
static const int8_t p_y12[12]={-64,-55,-32,0,32,55,64,55,32,0,-32,-55};
static __attribute__((noinline)) void p_line(const struct ln_ui_canvas_context *c,
                       int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint16_t ink){
    int32_t dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y1-y0:y0-y1;
    int32_t sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for(;;){box(c,x0,y0,1,1,ink);if(x0==x1&&y0==y1)break;
        int32_t twice=err*2;if(twice> -dy){err-=dy;x0+=sx;}
        if(twice<dx){err+=dx;y0+=sy;}}
}
static __attribute__((noinline)) void p_number(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                     const char *label,uint32_t value,uint16_t ink){
    char line[24],*p=line;append(&p,label);number(&p,value);*p=0;
    arcade_text(c,x,y,line,ink,true);
}
static __attribute__((noinline)) void p_header(const struct ln_ui_canvas_context *c,const char *title,const char *subtitle,
                     uint8_t stage,uint8_t total,uint16_t accent){
    box(c,0,0,220,176,0x0825u);box(c,0,0,220,34,0x10a9u);
    box(c,0,33,220,1,0x2a2fu);arcade_text(c,8,3,title,0xf7deu,false);
    char line[16],*p=line;
    if(game==POCKET_PINBALL)append(&p,"BALL ");
    number(&p,stage);append(&p,"/");number(&p,total);*p=0;
    arcade_center(c,game==POCKET_PINBALL?186:201,6,line,accent,true,1u);
    if(games_phase==PLAY){char points[32];arcade_score_line(points,false);
        arcade_center(c,110,22,points,accent,true,1u);}
    else arcade_center(c,110,22,subtitle,accent,true,1u);
}
static __attribute__((noinline)) void p_footer(const struct ln_ui_canvas_context *c,const char *hint){
    box(c,0,157,220,1,0x2a2fu);box(c,0,158,220,18,0x10a9u);
    arcade_center(c,110,162,hint,0xf7deu,true,1u);
}
static __attribute__((noinline)) void p_overlay(const struct ln_ui_canvas_context *c,const char *title,
                      const char *a,const char *b,const char *d,const char *action,uint16_t accent){
    arcade_panel(c,11,36,198,116,accent);box(c,14,39,192,110,0x10a9u);
    arcade_center(c,110,45,title,accent,false,1u);
    arcade_center(c,110,69,a,0xf7deu,true,1u);
    arcade_center(c,110,83,b,0xb596u,true,1u);
    arcade_center(c,110,97,d,0xb596u,true,1u);
    arcade_panel(c,30,121,160,22,accent);
    arcade_center(c,110,125,action,0x0825u,false,1u);
}
static void p_meter(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,int32_t w,
                    uint8_t value,uint8_t max,uint16_t accent){
    box(c,x,y,w,5,0x318eu);if(max&&value)box(c,x,y,(int32_t)value*w/max,5,accent);
}
static void p_dial(const struct ln_ui_canvas_context *c){
    const uint16_t gold=0xfed0u,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    p_header(c,"DIAL HEIST","CRACK THREE TUMBLERS",dh_lock+1u,DH_LOCKS,gold);
    arcade_disc(c,72,96,53,0x318eu);arcade_disc(c,72,96,50,0x0825u);
    arcade_disc(c,72,96,38,0x2a2fu);arcade_disc(c,72,96,36,0x0825u);
    for(uint8_t i=0;i<32u;++i){int32_t x=72+(int32_t)p_x32[i]*47/64,
        y=96+(int32_t)p_y32[i]*47/64;box(c,x,y,i%4u?1:2,i%4u?1:2,i%4u?muted:gold);}
    int32_t x=72+(int32_t)p_x32[dh_dial]*47/64,y=96+(int32_t)p_y32[dh_dial]*47/64;
    p_line(c,72,96,x,y,gold);arcade_disc(c,x,y,4,gold);
    arcade_disc(c,72,96,8,gold);arcade_disc(c,72,96,6,0x0825u);
    p_number(c,66,90,"",dh_dial,0xf7deu);
    arcade_text(c,139,43,"TUMBLERS",muted,true);
    for(uint8_t i=0;i<3u;++i){int32_t yy=56+i*25;
        uint16_t ink=i<dh_stage?0x5e7du:i==dh_stage?gold:0x4a4fu;
        arcade_panel(c,137,yy,74,22,ink);box(c,140,yy+3,68,16,0x10a9u);
        p_number(c,147,yy+5,"",i+1u,ink);
        arcade_text(c,166,yy+6,i<dh_stage?"OPEN":i==dh_stage?"LIVE":"SEALED",ink,true);}
    p_number(c,140,135,"TRIES ",3u-dh_misses,dh_misses?0xf969u:gold);
    p_meter(c,140,145,65,(uint8_t)(16u-dh_distance()),16u,gold);
    p_number(c,13,143,"TIME ",dh_seconds,gold);
    p_footer(c,"WHEEL DIAL  </> FINE  SEL SET  PLAY");
    if(games_phase==CW_INTRO)p_overlay(c,"HOW TO PLAY","Wheel scans the dial.",
        "Bright signal means close.","Select sets each tumbler.","SELECT / BEGIN",gold);
    else if(games_phase==PAUSED)p_overlay(c,"PAUSED","The patrol clock is held.",
        "Play resumes.","Select restarts this lock.","PLAY / RESUME",gold);
    else if(games_phase==CW_CLEAR)p_overlay(c,"LOCK OPEN","Three tumblers aligned.",
        "The next lock awaits.",result,"SELECT / NEXT",gold);
    else if(games_phase==WON)p_overlay(c,"HEIST COMPLETE","Every lock is open.",
        "Best run saved.",result,"SELECT / AGAIN",gold);
    else if(games_phase==LOST)p_overlay(c,dh_misses>=3u?"LOCK JAMMED":"PATROL ARRIVED",
        dh_misses>=3u?"Three failed sets.":"Time ran out.",
        "Retry this lock.",result,"SELECT / RETRY",gold);
}
static void p_mix(const struct ln_ui_canvas_context *c){
    static const uint16_t inks[4]={0xfcaeu,0x6f9du,0xfed0u,0xb95fu};
    static const char *const labels[4]={"KICK","SNARE","HAT","LEAD"};
    const uint16_t gold=0xfed0u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    p_header(c,"MIXTAPE","FOUR LOOPS. EIGHT BEATS.",mt_bar+1u,MT_BARS,gold);
    for(uint8_t lane=0;lane<4u;++lane){int32_t r=19+lane*9;
        uint16_t rail=lane==mt_lane?0x6b14u:0x318eu;
        for(uint8_t beat=0;beat<8u;++beat){uint8_t next=(beat+1u)&7u;
            p_line(c,76+(int32_t)p_x32[beat*4u]*r/64,96+(int32_t)p_y32[beat*4u]*r/64,
                   76+(int32_t)p_x32[next*4u]*r/64,96+(int32_t)p_y32[next*4u]*r/64,rail);}
        if(lane==mt_lane){int32_t px=76+(int32_t)p_x32[mt_cursor*4u]*r/64,
            py=96+(int32_t)p_y32[mt_cursor*4u]*r/64;
            arcade_disc(c,px,py,7,gold);arcade_disc(c,px,py,5,0x0825u);}
        uint8_t goal=mt_goal[mt_bar][lane];
        int32_t gx=76+(int32_t)p_x32[goal*4u]*r/64,gy=96+(int32_t)p_y32[goal*4u]*r/64;
        arcade_disc(c,gx,gy,4,inks[lane]);arcade_disc(c,gx,gy,2,0x0825u);
        if(mt_placed[lane]<8u){uint8_t beat=mt_placed[lane];
            arcade_disc(c,76+(int32_t)p_x32[beat*4u]*r/64,
                        96+(int32_t)p_y32[beat*4u]*r/64,3,inks[lane]);}
        }
    arcade_disc(c,76,96,12,0x18ebu);arcade_disc(c,76,96,9,0x0825u);
    p_number(c,72,90,"",mt_cursor+1u,0xf7deu);
    for(uint8_t lane=0;lane<4u;++lane){int32_t y=42+lane*26;
        arcade_panel(c,139,y,71,23,lane==mt_lane?gold:0x318eu);
        box(c,142,y+3,65,17,0x10a9u);box(c,145,y+8,5,5,inks[lane]);
        arcade_text(c,155,y+4,labels[lane],0xf7deu,true);
        arcade_text(c,155,y+14,mt_placed[lane]==mt_goal[mt_bar][lane]?"IN TIME":
            mt_placed[lane]<8u?"SHIFT":"EMPTY",inks[lane],true);}
    p_number(c,12,145,"TIME ",mt_seconds,gold);
    p_footer(c,"WHEEL BEAT  </> LOOP  SEL PLACE  PLAY");
    if(games_phase==CW_INTRO)p_overlay(c,"HOW TO PLAY","Wheel chooses a beat.",
        "</> chooses a loop.","Select places on hollow cue.","SELECT / BEGIN",gold);
    else if(games_phase==PAUSED)p_overlay(c,"PAUSED","The bar is waiting.",
        "Play resumes the clock.","Select restarts the bar.","PLAY / RESUME",gold);
    else if(games_phase==CW_CLEAR)p_overlay(c,"MIX COMPLETE","All four loops align.",
        "A new bar awaits.",result,"SELECT / NEXT",gold);
    else if(games_phase==WON)p_overlay(c,"MASTER MIX","All eight bars complete.",
        "Best run saved.",result,"SELECT / AGAIN",gold);
    else if(games_phase==LOST)p_overlay(c,"TIME IS UP","The bar ended early.",
        "Retry this bar.",result,"SELECT / RETRY",gold);
}
static void p_signal(const struct ln_ui_canvas_context *c){
    const uint16_t cyan=0x6f9du,gold=0xfed0u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    p_header(c,"SIGNAL","TUNE. LOCK. HOLD.",sg_station+1u,SG_STATIONS,cyan);
    box(c,10,43,200,78,0x10a9u);
    for(uint8_t i=0;i<32u;++i){int32_t distance=(int32_t)i-(sg_frequency[sg_station]+2u)/4;
        if(distance<0)distance= -distance;
        int32_t peak=distance<5?23-distance*5:0;
        int32_t height=5+(i*17u+sg_station*13u)%9u+peak;
        box(c,13+i*6,116-height,4,height,distance<3?cyan:0x318eu);}
    int32_t marker=13+(int32_t)sg_tune*189/127;
    p_line(c,marker,42,marker,120,gold);arcade_disc(c,marker,120,3,gold);
    p_number(c,12,129,"FREQ ",sg_tune,0xf7deu);
    arcade_text(c,119,121,sg_locked?"DECODING":sg_rejected?"NO LOCK":
        sg_distance()<=1u?"READY":"TUNING",sg_locked||sg_distance()<=1u?cyan:gold,true);
    uint8_t strength=sg_distance()>=24u?0u:(uint8_t)(24u-sg_distance());
    p_meter(c,119,132,88,strength,24u,cyan);
    for(uint8_t i=0;i<4u;++i)box(c,122+i*22,145,16,3,i<sg_progress?cyan:0x318eu);
    p_footer(c,"WHEEL TUNE  </> FINE  SEL LOCK  PLAY");
    if(games_phase==CW_INTRO)p_overlay(c,"HOW TO PLAY","Find the bright peak.",
        "Tune until strength is full.","Select locks; hold steady.","SELECT / BEGIN",cyan);
    else if(games_phase==PAUSED)p_overlay(c,"PAUSED","The channel is held.",
        "Play resumes decoding.","Select restarts this station.","PLAY / RESUME",cyan);
    else if(games_phase==CW_CLEAR)p_overlay(c,"TRANSMISSION FOUND",sg_messages[sg_station],
        "The next frequency awaits.",result,"SELECT / NEXT",cyan);
    else if(games_phase==WON)p_overlay(c,"ALL SIGNALS FOUND",sg_messages[sg_station],
        "Eight messages decoded.",result,"SELECT / AGAIN",cyan);
}
static void p_city(const struct ln_ui_canvas_context *c){
    const uint16_t cyan=0x6f9du,gold=0xfed0u,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    p_header(c,"CLOCKWORK CITY","ROTATE RINGS. OPEN THE ROUTE.",cc_map+1u,CC_MAPS,cyan);
    uint8_t progress=cc_route();
    for(uint8_t ring=0;ring<2u;++ring){
        uint8_t gate=(uint8_t)((cc_exit[cc_map][ring]+cc_rot[ring])%12u);
        uint8_t next=(uint8_t)((cc_entry[cc_map][ring+1u]+cc_rot[ring+1u])%12u);
        if(gate==next&&progress>=ring+2u){int32_t outer=55-(int32_t)ring*17,inner=outer-17;
            p_line(c,109+(int32_t)p_x12[gate]*(outer-4)/64,
                   96+(int32_t)p_y12[gate]*(outer-4)/64,
                   109+(int32_t)p_x12[gate]*(inner+4)/64,
                   96+(int32_t)p_y12[gate]*(inner+4)/64,cyan);}
    }
    for(uint8_t ring=0;ring<3u;++ring){int32_t r=55-ring*17;
        uint16_t rail=ring==cc_focus?0x6b14u:0x318eu;
        for(uint8_t i=0;i<12u;++i){uint8_t next=(i+1u)%12u;
            p_line(c,109+(int32_t)p_x12[i]*r/64,96+(int32_t)p_y12[i]*r/64,
                   109+(int32_t)p_x12[next]*r/64,96+(int32_t)p_y12[next]*r/64,rail);}
        uint8_t in=(cc_entry[cc_map][ring]+cc_rot[ring])%12u;
        uint8_t out=(cc_exit[cc_map][ring]+cc_rot[ring])%12u;
        int32_t ix=109+(int32_t)p_x12[in]*r/64,iy=96+(int32_t)p_y12[in]*r/64;
        int32_t ox=109+(int32_t)p_x12[out]*r/64,oy=96+(int32_t)p_y12[out]*r/64;
        p_line(c,ix,iy,ox,oy,ring<progress?cyan:0x39edu);
        arcade_disc(c,ix,iy,4,cyan);arcade_disc(c,ix,iy,2,0x0825u);
        arcade_disc(c,ox,oy,4,gold);}
    if(progress==4u)p_line(c,109+(int32_t)p_x12[cc_destination[cc_map]]*19/64,
                           96+(int32_t)p_y12[cc_destination[cc_map]]*19/64,
                           109+(int32_t)p_x12[cc_destination[cc_map]]*13/64,
                           96+(int32_t)p_y12[cc_destination[cc_map]]*13/64,cyan);
    arcade_disc(c,109,41,4,0xf7deu);arcade_disc(c,109,41,2,cyan);
    arcade_disc(c,109,96,8,0x18ebu);arcade_disc(c,109,96,6,0x0825u);
    box(c,106,92,6,8,0xf7deu);
    int32_t dx=109+(int32_t)p_x12[cc_destination[cc_map]]*13/64,
            dy=96+(int32_t)p_y12[cc_destination[cc_map]]*13/64;
    arcade_disc(c,dx,dy,4,0xf7deu);arcade_disc(c,dx,dy,2,gold);
    p_number(c,7,54,"ROUTE ",progress,cyan);
    arcade_text(c,7,66,"/4",muted,true);
    p_number(c,174,61,"RING ",cc_focus+1u,gold);
    arcade_text(c,168,78,"BLUE IN",cyan,true);
    arcade_text(c,158,91,"GOLD OUT",gold,true);
    arcade_text(c,158,104,"START TOP",0xf7deu,true);
    p_number(c,8,133,"TRIES ",cc_attempts,muted);
    p_number(c,8,145,"TIME ",cc_seconds,gold);
    p_footer(c,"WHEEL TURN  </> RING  SEL GO  PLAY");
    if(games_phase==CW_INTRO)p_overlay(c,"HOW TO PLAY","</> chooses a ring.",
        "Wheel rotates its streets.","Join blue to gold. Select.","SELECT / BEGIN",cyan);
    else if(games_phase==PAUSED)p_overlay(c,"PAUSED","The city is held.",
        "Play resumes the clock.","Select restarts this map.","PLAY / RESUME",cyan);
    else if(games_phase==CW_CLEAR)p_overlay(c,"ROUTE OPEN","The courier arrived.",
        "A new map awaits.",result,"SELECT / NEXT",cyan);
    else if(games_phase==WON)p_overlay(c,"CITY MASTERED","Every route is open.",
        "Best run saved.",result,"SELECT / AGAIN",cyan);
    else if(games_phase==LOST)p_overlay(c,"CITY SHIFTED","The route closed early.",
        "Retry this map.",result,"SELECT / RETRY",cyan);
}
static int32_t p_pin_x(int32_t x){return 13+(x-20)*194/280;}
static int32_t p_pin_y(int32_t y){return 37+(y-49)*114/157;}
static void p_pinball(const struct ln_ui_canvas_context *c){
    const uint16_t pink=0xfcaeu,cyan=0x6f9du,gold=0xfed0u;
    char result[32];arcade_score_line(result,false);
    p_header(c,"POCKET PINBALL","TILT GRAVITY. KICK THE BALL.",
             pp_lives?4u-pp_lives:3u,3u,pink);
    arcade_panel(c,10,35,200,120,pink);box(c,13,38,194,114,0x102au);
    box(c,91,152,38,4,0x0825u);
    for(uint8_t i=0;i<3u;++i){int32_t x=p_pin_x(80+i*80),y=p_pin_y(82);
        uint16_t ink=pp_targets&(1u<<i)?0x318eu:cyan;
        if(pp_flash&&pp_flash_target==i)arcade_disc(c,x,y,10,gold);
        arcade_disc(c,x,y,8,ink);arcade_disc(c,x,y,5,0x102au);arcade_disc(c,x,y,2,ink);}
    for(uint8_t i=0;i<10u;++i){int32_t x=p_pin_x(pp_peg_x[i]),y=p_pin_y(pp_peg_y[i]);
        arcade_disc(c,x,y,5,0x39edu);arcade_disc(c,x,y,4,i%2u?pink:gold);}
    p_line(c,p_pin_x(26),p_pin_y(186),p_pin_x(76),p_pin_y(204),pink);
    p_line(c,p_pin_x(294),p_pin_y(186),p_pin_x(244),p_pin_y(204),pink);
    p_line(c,p_pin_x(78),p_pin_y(201),p_pin_x(133),p_pin_y(190),cyan);
    p_line(c,p_pin_x(242),p_pin_y(201),p_pin_x(187),p_pin_y(190),cyan);
    int32_t x=p_pin_x(pp_x/16),y=p_pin_y(pp_y/16);
    arcade_disc(c,x,y,4,0xf7deu);arcade_disc(c,x-1,y-1,1,gold);
    p_number(c,18,41,"BANKS ",pp_banks,0xf7deu);
    p_number(c,158,41,"KICKS ",pp_kicks,gold);
    arcade_text(c,164,125,"TILT",0x8cb4u,true);
    for(int8_t i=-3;i<=3;++i)box(c,164+(int32_t)(i+3)*6,140,3,i==pp_tilt?7:3,
                                  i==pp_tilt?gold:0x39edu);
    p_footer(c,"WHEEL TILT  SEL KICK  PLAY PAUSE");
    if(games_phase==CW_INTRO)p_overlay(c,"HOW TO PLAY","Wheel tilts the table.",
        "Select kicks the ball up.","Clear targets for a bonus.","SELECT / LAUNCH",pink);
    else if(games_phase==PP_SERVE)p_overlay(c,"NEXT BALL READY","Tilt is centered.",
        "Three kicks reloaded.",result,"SELECT / LAUNCH",pink);
    else if(games_phase==PAUSED)p_overlay(c,"PAUSED","The ball is held.",
        "Play resumes the table.","Select starts a new run.","PLAY / RESUME",pink);
    else if(games_phase==LOST)p_overlay(c,"GAME OVER","All three balls spent.",
        "Best score saved.",result,"SELECT / AGAIN",pink);
}
static void p_collection_draw(const struct ln_ui_canvas_context *c){
    if(game==DIAL_HEIST)p_dial(c);else if(game==MIXTAPE)p_mix(c);
    else if(game==SIGNAL)p_signal(c);else if(game==CLOCKWORK_CITY)p_city(c);
    else p_pinball(c);
}
