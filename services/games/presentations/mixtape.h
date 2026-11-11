static void mt_draw(const struct ln_ui_canvas_context *c){
    static const uint16_t ink[4]={0xfcaeu,0x6f9du,0xfed0u,0xb95fu};
    static const char *const track[4]={"KICK","SNARE","HAT","LEAD"};
    const uint16_t gold=0xfed0u,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    cw_header(c,"MIXTAPE","PLACE FOUR LOOPS ON THE EIGHT-BEAT MAP",(uint8_t)(mt_bar+1u),MT_BARS,gold);
    for(uint8_t lane=0;lane<4u;++lane){
        int32_t radius=27+(int32_t)lane*13;
        uint16_t rail=lane==mt_lane?0x6b14u:0x318eu;
        for(uint8_t beat=0;beat<8u;++beat){
            uint8_t next=(uint8_t)((beat+1u)&7u);
            int32_t x0=143+(int32_t)cw_x32[beat*4u]*radius/64,
                    y0=126+(int32_t)cw_y32[beat*4u]*radius/64;
            int32_t x1=143+(int32_t)cw_x32[next*4u]*radius/64,
                    y1=126+(int32_t)cw_y32[next*4u]*radius/64;
            cw_line(c,x0,y0,x1,y1,rail);
            arcade_disc(c,x0,y0,2,0x5a50u);
        }
        if(lane==mt_lane){
            int32_t px=143+(int32_t)cw_x32[mt_cursor*4u]*radius/64,
                    py=126+(int32_t)cw_y32[mt_cursor*4u]*radius/64;
            arcade_disc(c,px,py,8,gold);arcade_disc(c,px,py,6,0x0825u);
        }
        uint8_t goal=mt_goal[mt_bar][lane];
        int32_t gx=143+(int32_t)cw_x32[goal*4u]*radius/64,
                gy=126+(int32_t)cw_y32[goal*4u]*radius/64;
        arcade_disc(c,gx,gy,5,ink[lane]);arcade_disc(c,gx,gy,3,0x0825u);
        if(mt_placed[lane]<8u){
            uint8_t beat=mt_placed[lane];
            int32_t px=143+(int32_t)cw_x32[beat*4u]*radius/64,
                    py=126+(int32_t)cw_y32[beat*4u]*radius/64;
            arcade_disc(c,px,py,4,ink[lane]);arcade_disc(c,px,py,2,0xf7deu);
        }
    }
    arcade_disc(c,143,126,18,0x18ebu);arcade_disc(c,143,126,15,0x0825u);
    cw_text_number(c,130,119,"",mt_cursor+1u,0xf7deu);
    arcade_text(c,235,53,"LOOPS",muted,true);
    for(uint8_t lane=0;lane<4u;++lane){
        int32_t y=72+(int32_t)lane*32;
        box(c,228,y,80,28,lane==mt_lane?gold:0x2a2fu);
        box(c,230,y+2,76,24,0x10a9u);
        box(c,234,y+9,7,7,ink[lane]);
        arcade_text(c,246,y+7,track[lane],lane==mt_lane?0xf7deu:muted,true);
        arcade_text(c,246,y+18,mt_placed[lane]==mt_goal[mt_bar][lane]?"IN TIME":
                    mt_placed[lane]<8u?"SHIFT":"EMPTY",ink[lane],true);
    }
    cw_text_number(c,14,194,"TIME ",mt_seconds,gold);
    cw_footer(c,"WHEEL BEAT   </> TRACK   SELECT PLACE   PLAY PAUSE");
    if(games_phase==CW_INTRO)
        cw_overlay(c,"HOW TO PLAY","Turn the wheel to choose a beat.",
                   "Use </> to choose a loop.","Select places it on the hollow cue.","SELECT / BEGIN",gold);
    else if(games_phase==PAUSED)
        cw_overlay(c,"PAUSED","The bar is waiting.","Play resumes the clock.",
                   "Select restarts this bar.","PLAY / RESUME",gold);
    else if(games_phase==CW_CLEAR)
        cw_overlay(c,"MIX COMPLETE","Four loops landed in time.","A new bar is ready.",
                   result, "SELECT / NEXT",gold);
    else if(games_phase==WON)
        cw_overlay(c,"MASTER MIX","All eight bars are complete.","Best run saved.",
                   result, "SELECT / PLAY AGAIN",gold);
    else if(games_phase==LOST)
        cw_overlay(c,"TIME IS UP","The bar ended before the mix.","Retry this bar.",
                   result, "SELECT / RETRY",gold);
}
