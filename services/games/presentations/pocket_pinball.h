static void pp_draw(const struct ln_ui_canvas_context *c){
    const uint16_t pink=0xfcaeu,cyan=0x6f9du,gold=0xfed0u,white=0xf7deu,muted=0x8cb4u;
    char result[32];arcade_score_line(result,false);
    cw_header(c,"POCKET PINBALL","TILT GRAVITY. KICK THE BALL.",
              (uint8_t)(pp_lives?4u-pp_lives:3u),3u,pink);
    arcade_panel(c,17,46,286,163,pink);
    box(c,20,49,280,157,0x102au);
    box(c,134,205,52,5,0x0825u);
    box(c,23,74,274,1,0x318eu);
    cw_text_number(c,29,52,"BANKS ",pp_banks,white);
    cw_text_number(c,239,52,"KICKS ",pp_kicks,gold);
    for(uint8_t i=0;i<3u;++i){
        int32_t x=80+(int32_t)i*80;
        uint16_t target=pp_targets&(1u<<i)?0x318eu:cyan;
        if(pp_flash&&pp_flash_target==i)arcade_disc(c,x,82,15,gold);
        arcade_disc(c,x,82,12,target);arcade_disc(c,x,82,8,0x102au);
        arcade_disc(c,x,82,4,target);
    }
    for(uint8_t i=0;i<10u;++i){
        int32_t x=pp_peg_x[i],y=pp_peg_y[i];
        arcade_disc(c,x,y,9,0x39edu);arcade_disc(c,x,y,7,i%2u?pink:gold);
        arcade_disc(c,x-2,y-2,2,white);
    }
    cw_line(c,26,186,76,204,pink);cw_line(c,294,186,244,204,pink);
    cw_line(c,78,201,133,190,cyan);cw_line(c,242,201,187,190,cyan);
    int32_t x=pp_x/16,y=pp_y/16;
    arcade_disc(c,x,y,7,0x10a9u);arcade_disc(c,x,y,5,white);
    arcade_disc(c,x-1,y-1,2,gold);
    for(uint8_t i=0;i<pp_lives;++i)arcade_disc(c,37+i*16,195,4,pink);
    arcade_text(c,231,192,"TILT",muted,true);
    for(int8_t i=-3;i<=3;++i)box(c,257+(int32_t)i*7,196,4,i==pp_tilt?8:4,
                                 i==pp_tilt?gold:0x39edu);
    cw_footer(c,"WHEEL TILT   SELECT KICK   PLAY PAUSE");
    if(games_phase==CW_INTRO)
        cw_overlay(c,"HOW TO PLAY","Turn the wheel to tilt gravity.",
                   "Select kicks the ball upward.","Clear three targets for a bonus.","SELECT / LAUNCH",pink);
    else if(games_phase==PP_SERVE)
        cw_overlay(c,"NEXT BALL READY","Tilt returns to center.",
                   "Three kicks are reloaded.",result,"SELECT / LAUNCH",pink);
    else if(games_phase==PAUSED)
        cw_overlay(c,"PAUSED","The ball is held in place.","Play resumes the table.",
                   "Select starts a fresh run.","PLAY / RESUME",pink);
    else if(games_phase==LOST)
        cw_overlay(c,"GAME OVER","All three balls are spent.","Your best score was saved.",
                   result, "SELECT / PLAY AGAIN",pink);
}
