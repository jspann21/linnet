static void dh_draw(const struct ln_ui_canvas_context *c){
    const uint16_t gold=0xfed0u,white=0xf7deu,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    cw_header(c,"DIAL HEIST","CRACK THE LOCK BEFORE THE PATROL",(uint8_t)(dh_lock+1u),DH_LOCKS,gold);
    arcade_disc(c,128,125,77,0x2a2fu);arcade_disc(c,128,125,73,0x10a9u);
    arcade_disc(c,128,125,57,0x318eu);arcade_disc(c,128,125,53,0x0825u);
    for(uint8_t i=0;i<32u;++i){
        int32_t x=128+(int32_t)cw_x32[i]*69/64,y=125+(int32_t)cw_y32[i]*69/64;
        box(c,x-1,y-1,i%4u?2:3,i%4u?2:3,i%4u?0x5a50u:gold);
    }
    int32_t px=128+(int32_t)cw_x32[dh_dial]*69/64,py=125+(int32_t)cw_y32[dh_dial]*69/64;
    cw_line(c,128,125,px,py,gold);arcade_disc(c,px,py,5,gold);
    arcade_disc(c,128,125,10,gold);arcade_disc(c,128,125,6,0x0825u);
    cw_text_number(c,113,112,"",dh_dial,white);
    arcade_text(c,221,53,"TUMBLERS",muted,true);
    for(uint8_t i=0;i<3u;++i){
        int32_t y=72+(int32_t)i*37;
        uint16_t ink=i<dh_stage?0x5e7du:i==dh_stage?gold:0x4a4fu;
        arcade_panel(c,218,y,91,29,ink);box(c,221,y+3,85,23,0x10a9u);
        char id[2]={(char)('1'+i),0};arcade_text(c,230,y+7,id,ink,false);
        arcade_text(c,253,y+9,i<dh_stage?"OPEN":i==dh_stage?"ACTIVE":"SEALED",ink,true);
    }
    arcade_text(c,222,185,"SIGNAL",muted,true);
    cw_meter(c,222,199,86,(uint8_t)(16u-dh_distance()),16u,gold);
    cw_text_number(c,13,194,"PATROL ",dh_seconds,gold);
    arcade_text(c,135,185,"TRIES",muted,true);
    for(uint8_t i=0;i<3u;++i)box(c,135+i*17,199,12,5,i<3u-dh_misses?gold:0xf969u);
    cw_footer(c,"WHEEL DIAL   </> FINE   SELECT SET   PLAY PAUSE");
    if(games_phase==CW_INTRO)
        cw_overlay(c,"HOW TO PLAY","Turn the wheel to scan the dial.",
                   "A bright signal means close.","Select sets each of three tumblers.","SELECT / BEGIN",gold);
    else if(games_phase==PAUSED)
        cw_overlay(c,"PAUSED","The patrol clock is stopped.","Play resumes the lock.",
                   "Select restarts this lock.","PLAY / RESUME",gold);
    else if(games_phase==CW_CLEAR)
        cw_overlay(c,"LOCK OPEN","All three tumblers aligned.","A new lock is waiting.",
                   result, "SELECT / NEXT",gold);
    else if(games_phase==WON)
        cw_overlay(c,"HEIST COMPLETE","Every lock is open.","Best run saved.",
                   result, "SELECT / PLAY AGAIN",gold);
    else if(games_phase==LOST)
        cw_overlay(c,dh_misses>=3u?"LOCK JAMMED":"PATROL ARRIVED",
                   dh_misses>=3u?"Three failed sets.":"Time ran out.",
                   "Retry this lock.",result,"SELECT / RETRY",gold);
}
