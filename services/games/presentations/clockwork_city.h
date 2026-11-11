static void cc_draw(const struct ln_ui_canvas_context *c){
    const uint16_t gold=0xfed0u,cyan=0x6f9du,white=0xf7deu,muted=0x8cb4u;
    char result[32];arcade_score_line(result,games_phase==CW_CLEAR);
    cw_header(c,"CLOCKWORK CITY","ROTATE THE STREETS. OPEN THE ROUTE.",
              (uint8_t)(cc_map+1u),CC_MAPS,cyan);
    uint8_t progress=cc_route();
    for(uint8_t ring=0;ring<2u;++ring){
        uint8_t gate=(uint8_t)((cc_exit[cc_map][ring]+cc_rot[ring])%12u);
        uint8_t next=(uint8_t)((cc_entry[cc_map][ring+1u]+cc_rot[ring+1u])%12u);
        if(gate==next&&progress>=ring+2u){
            int32_t outer=77-(int32_t)ring*22,inner=outer-22;
            cw_line(c,158+(int32_t)cw_x12[gate]*(outer-5)/64,
                    125+(int32_t)cw_y12[gate]*(outer-5)/64,
                    158+(int32_t)cw_x12[gate]*(inner+5)/64,
                    125+(int32_t)cw_y12[gate]*(inner+5)/64,cyan);
        }
    }
    for(uint8_t ring=0;ring<3u;++ring){
        int32_t radius=77-(int32_t)ring*22;
        uint16_t rail=ring==cc_focus?0x6b14u:0x318eu;
        for(uint8_t i=0;i<12u;++i){
            uint8_t next=(uint8_t)((i+1u)%12u);
            int32_t x0=158+(int32_t)cw_x12[i]*radius/64,
                    y0=125+(int32_t)cw_y12[i]*radius/64;
            int32_t x1=158+(int32_t)cw_x12[next]*radius/64,
                    y1=125+(int32_t)cw_y12[next]*radius/64;
            cw_line(c,x0,y0,x1,y1,rail);
            arcade_disc(c,x0,y0,2,rail);
        }
        uint8_t entry=(uint8_t)((cc_entry[cc_map][ring]+cc_rot[ring])%12u);
        uint8_t leave=(uint8_t)((cc_exit[cc_map][ring]+cc_rot[ring])%12u);
        int32_t ix=158+(int32_t)cw_x12[entry]*radius/64,
                iy=125+(int32_t)cw_y12[entry]*radius/64;
        int32_t ox=158+(int32_t)cw_x12[leave]*radius/64,
                oy=125+(int32_t)cw_y12[leave]*radius/64;
        cw_line(c,ix,iy,ox,oy,ring<progress?cyan:0x39edu);
        arcade_disc(c,ix,iy,6,cyan);arcade_disc(c,ix,iy,3,0x0825u);
        arcade_disc(c,ox,oy,5,gold);arcade_disc(c,ox,oy,2,white);
    }
    if(progress==4u)cw_line(c,158+(int32_t)cw_x12[cc_destination[cc_map]]*29/64,
                            125+(int32_t)cw_y12[cc_destination[cc_map]]*29/64,
                            158+(int32_t)cw_x12[cc_destination[cc_map]]*17/64,
                            125+(int32_t)cw_y12[cc_destination[cc_map]]*17/64,cyan);
    arcade_disc(c,158,45,5,white);arcade_disc(c,158,45,2,cyan);
    arcade_disc(c,158,125,12,0x18ebu);arcade_disc(c,158,125,9,0x0825u);
    box(c,154,120,8,10,white);box(c,156,116,4,4,gold);
    int32_t dx=158+(int32_t)cw_x12[cc_destination[cc_map]]*17/64,
            dy=125+(int32_t)cw_y12[cc_destination[cc_map]]*17/64;
    arcade_disc(c,dx,dy,5,white);arcade_disc(c,dx,dy,2,gold);
    arcade_text(c,12,57,"ROUTE",muted,true);
    cw_text_number(c,19,74,"",progress,cyan);
    arcade_text(c,31,78,"/4",muted,true);
    arcade_text(c,12,108,"RING",muted,true);
    cw_text_number(c,22,125,"",cc_focus+1u,gold);
    cw_text_number(c,11,151,"TRIES ",cc_attempts,muted);
    cw_text_number(c,11,183,"TIME ",cc_seconds,gold);
    arcade_text(c,244,74,"BLUE IN",cyan,true);
    arcade_text(c,244,95,"GOLD OUT",gold,true);
    arcade_text(c,244,135,"START",white,true);
    arcade_text(c,244,151,"AT TOP",muted,true);
    cw_footer(c,"WHEEL ROTATE   </> RING   SELECT GO   PLAY PAUSE");
    if(games_phase==CW_INTRO)
        cw_overlay(c,"HOW TO PLAY","Use </> to choose a street ring.",
                   "Turn the wheel to rotate it.","Join blue to gold; Select dispatches.","SELECT / BEGIN",cyan);
    else if(games_phase==PAUSED)
        cw_overlay(c,"PAUSED","The city holds its position.","Play resumes the clock.",
                   "Select restarts this map.","PLAY / RESUME",cyan);
    else if(games_phase==CW_CLEAR)
        cw_overlay(c,"ROUTE OPEN","The courier reached the center.","A new city map awaits.",
                   result, "SELECT / NEXT",cyan);
    else if(games_phase==WON)
        cw_overlay(c,"CITY MASTERED","Every route is open.","Best run saved.",
                   result, "SELECT / PLAY AGAIN",cyan);
    else if(games_phase==LOST)
        cw_overlay(c,"CITY SHIFTED","The route closed before dispatch.","Retry this map.",
                   result, "SELECT / RETRY",cyan);
}
