/* Two native layouts share the same precomputed timeline and draw primitives. */
static void scr_label_number(const struct ln_ui_canvas_context *c,int32_t x,int32_t y,
                             const char *label,uint32_t value,uint16_t ink){
    char line[28],*p=line;append(&p,label);number(&p,value);*p=0;
    arcade_text(c,x,y,line,ink,true);
}
static void scr_draw(const struct ln_ui_canvas_context *c){
    const bool small=LINNET_UI_WIDTH==220;
    const int32_t w=LINNET_UI_WIDTH,h=LINNET_UI_HEIGHT;
    const int32_t lane_y[3]={small?40:57,small?76:106,small?112:155};
    const int32_t lane_h=small?30:42,rail_l=small?48:62,rail_r=w-(small?18:24);
    const int32_t rail_w=rail_r-rail_l,timeline_y=small?148:208;
    const uint16_t bg=0x0825u,panel=0x10cau,edge=0x2a2fu,white=0xf7deu,
                   muted=0x8cb4u,gold=0xfed0u,cyan=0x4f3bu;
    static const uint16_t lane_ink[3]={0xfcadu,0x6f9du,0xd31au};
    box(c,0,0,w,h,bg);
    box(c,0,0,w,small?31:42,0x10a9u);
    box(c,0,small?30:41,w,1,edge);
    if(small){
        arcade_text(c,10,4,"SCRUB",white,false);
        scr_label_number(c,76,5,"P ",score,muted);
        scr_label_number(c,157,5,"",scr_level+1u,gold);
        arcade_text(c,170,5,"/12",muted,true);
        arcade_center(c,w/2,20,"PIN ONE ECHO. MOVE TIME.",cyan,true,1u);
    }else{
        arcade_text(c,15,5,"SCRUB",white,false);
        scr_label_number(c,88,7,"RUN ",score,muted);
        scr_label_number(c,151,7,"BEST ",best[SCRUB],muted);
        scr_label_number(c,w-93,7,"CHAPTER ",scr_level+1u,gold);
        arcade_center(c,w/2,28,"PIN ONE ECHO. MOVE THE REST OF TIME.",cyan,true,1u);
    }
    for(uint8_t lane=0;lane<3u;++lane){
        int32_t y=lane_y[lane],cy=y+(small?17:25);
        uint16_t tint=lane_ink[lane],outline=lane==scr_focus?gold:edge;
        arcade_panel(c,8,y,w-16,lane_h,outline);
        arcade_panel(c,10,y+2,w-20,lane_h-4,panel);
        box(c,rail_l,cy-1,rail_w,2,0x39edu);
        for(uint8_t mark=0;mark<=15u;mark+=5u){
            int32_t mx=rail_l+(int32_t)mark*rail_w/15;
            box(c,mx,cy-4,1,8,0x52b0u);
        }
        char id[2]={(char)('A'+lane),0};
        arcade_text(c,19,y+(small?4:7),id,lane==scr_focus?white:tint,false);
        if(scr_anchor==lane)arcade_text(c,15,y+(small?17:25),"PIN",gold,true);
        int32_t gx=rail_l+(int32_t)scr_goals[scr_level][lane]*rail_w/15;
        int32_t px=rail_l+(int32_t)scr_position(lane)*rail_w/15;
        if(scr_anchor!=lane)for(int32_t d=-2;d<=2;++d)if(d){
            uint8_t at=(uint8_t)((scr_time+d+32)&31);
            int32_t ghost=rail_l+(int32_t)scr_paths[scr_level][lane][at]*rail_w/15;
            arcade_disc(c,ghost,cy,small?1:2,0x5a50u);
        }
        arcade_disc(c,gx,cy,small?7:9,gold);
        arcade_disc(c,gx,cy,small?5:7,panel);
        arcade_disc(c,px,cy,small?5:7,tint);
        arcade_disc(c,px,cy,small?2:3,white);
        if(px==gx){box(c,gx-1,cy-(small?10:13),3,3,gold);box(c,gx-1,cy+(small?8:10),3,3,gold);}
    }
    box(c,8,timeline_y-5,w-16,small?15:18,0x10a9u);
    for(int32_t t=0;t<32;++t){
        int32_t x=12+t*(w-24)/32;
        box(c,x,timeline_y,small?4:7,t==scr_time?small?7:10:small?3:4,
            t==scr_time?gold:t%4==0?0x5b13u:0x318eu);
    }
    if(!small){
        arcade_text(c,12,220,"PAST",muted,true);
        scr_label_number(c,134,220,"TIME ",scr_time,white);
        arcade_text(c,w-62,220,"FUTURE",muted,true);
        arcade_text(c,12,229,scr_anchor==scr_focus?"WHEEL TIME   </> ECHO   SELECT RELEASE":
                    scr_anchor==3u?"WHEEL TIME   </> ECHO   SELECT PIN":
                    "WHEEL TIME   </> ECHO   SELECT MOVE PIN",white,true);
    }else{
        arcade_text(c,7,162,scr_anchor==3u?"WHEEL TIME  </> LANE  SEL PIN":
                    scr_anchor==scr_focus?"WHEEL TIME  </> LANE  SEL FREE":
                    "WHEEL TIME  </> LANE  SEL MOVE",white,true);
        scr_label_number(c,192,162,"T",scr_time,gold);
    }
    if(games_phase==PLAY)return;
    int32_t ox=small?13:32,oy=small?36:55,ow=w-ox*2,oh=small?105:146;
    arcade_panel(c,ox,oy,ow,oh,gold);
    arcade_panel(c,ox+2,oy+2,ow-4,oh-4,0x18ebu);
    box(c,ox+7,oy+7,ow-14,oh-14,0x10a9u);
    const char *title=games_phase==SCR_INTRO?"HOW TO PLAY":
        games_phase==PAUSED?"PAUSED":games_phase==WON?"TIME MASTERED":"TIMELINE SOLVED";
    arcade_center(c,w/2,oy+(small?10:15),title,gold,false,1u);
    if(games_phase==SCR_INTRO){
        arcade_center(c,w/2,oy+(small?23:44),small?"WHEEL moves time":"Turn the wheel to move time.",white,true,1u);
        arcade_center(c,w/2,oy+(small?35:61),small?"</> chooses echo":"Use </> to choose an echo.",white,true,1u);
        arcade_center(c,w/2,oy+(small?47:78),small?"SELECT pins one":"Select pins one echo in place.",white,true,1u);
        arcade_center(c,w/2,oy+(small?59:95),small?"Match all three gates":"Match all three glowing gates.",cyan,true,1u);
    }else if(games_phase==PAUSED){
        arcade_center(c,w/2,oy+(small?40:62),"Play: resume",white,true,1u);
        arcade_center(c,w/2,oy+(small?56:82),"Select: restart chapter",muted,true,1u);
    }else{
        char progress[24],*p=progress;
        char result[32];arcade_score_line(result,games_phase==SCR_CLEAR);
        number(&p,scr_level+1u);append(&p," / ");number(&p,SCR_LEVELS);*p=0;
        arcade_center(c,w/2,oy+(small?41:63),progress,white,false,1u);
        arcade_center(c,w/2,oy+(small?59:87),result,cyan,true,1u);
    }
    arcade_panel(c,ox+17,oy+oh-(small?27:36),ow-34,small?19:25,gold);
    arcade_center(c,w/2,oy+oh-(small?24:31),
                  games_phase==SCR_INTRO?"SELECT / BEGIN":games_phase==PAUSED?"PLAY / RESUME":
                  games_phase==WON?"SELECT / PLAY AGAIN":"SELECT / NEXT",bg,false,1u);
}
