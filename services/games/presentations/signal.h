static void sg_draw(const struct ln_ui_canvas_context *c){
    const uint16_t cyan=0x6f9du,white=0xf7deu,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    cw_header(c,"SIGNAL","TUNE THE DIAL. HOLD THE CHANNEL.",(uint8_t)(sg_station+1u),SG_STATIONS,cyan);
    arcade_text(c,15,51,"BAND  00                         127",muted,true);
    box(c,13,71,294,92,0x10a9u);
    for(uint8_t i=0;i<32u;++i){
        int32_t distance=(int32_t)i-(int32_t)(sg_frequency[sg_station]+2u)/4;
        if(distance<0)distance= -distance;
        int32_t peak=distance<6?36-distance*6:0;
        int32_t height=7+(i*17u+sg_station*13u)%14u+peak;
        int32_t x=17+(int32_t)i*9;
        box(c,x,153-height,6,height,distance<3?cyan:0x318eu);
        box(c,x,153-height,6,2,distance<3?white:0x5a50u);
    }
    int32_t marker=17+(int32_t)sg_tune*279/127;
    cw_line(c,marker,69,marker,158,0xfed0u);
    arcade_disc(c,marker,158,5,0xfed0u);
    arcade_panel(c,13,168,294,36,0x318eu);
    box(c,16,171,288,30,0x0825u);
    cw_text_number(c,24,177,"FREQ ",sg_tune,white);
    arcade_text(c,111,178,sg_locked?"DECODING":sg_rejected?"NO LOCK":
                sg_distance()<=1u?"READY":"TUNING",sg_locked||sg_distance()<=1u?cyan:muted,true);
    uint8_t strength=sg_distance()>=24u?0u:(uint8_t)(24u-sg_distance());
    cw_meter(c,202,180,89,strength,24u,cyan);
    for(uint8_t i=0;i<4u;++i)box(c,202+i*22,194,16,3,i<sg_progress?cyan:0x318eu);
    cw_footer(c,"WHEEL TUNE   </> FINE   SELECT LOCK   PLAY PAUSE");
    if(games_phase==CW_INTRO)
        cw_overlay(c,"HOW TO PLAY","Find the bright peak on the band.",
                   "Tune until strength is full.","Select locks; hold steady to decode.","SELECT / BEGIN",cyan);
    else if(games_phase==PAUSED)
        cw_overlay(c,"PAUSED","The channel is held.","Play resumes decoding.",
                   "Select restarts this station.","PLAY / RESUME",cyan);
    else if(games_phase==CW_CLEAR)
        cw_overlay(c,"TRANSMISSION FOUND",sg_messages[sg_station],
                   "The next frequency is waiting.",result, "SELECT / NEXT",cyan);
    else if(games_phase==WON)
        cw_overlay(c,"ALL SIGNALS FOUND",sg_messages[sg_station],
                   "Eight transmissions decoded.",result, "SELECT / PLAY AGAIN",cyan);
}
