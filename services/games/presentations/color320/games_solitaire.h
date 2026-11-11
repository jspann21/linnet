/* Card-table presentation shares the arcade's native antialiased typography. */
static void sol_card_name(char *out,uint8_t card){
    static const char *const ranks[]={"Ace","2","3","4","5","6","7","8","9","10","Jack","Queen","King"};
    static const char *const suits[]={"Clubs","Diamonds","Hearts","Spades"};
    char *p=out;append(&p,ranks[sol_rank(card)-1u]);append(&p," of ");append(&p,suits[sol_suit(card)]);*p=0;
}
static void sol_slot(const struct ln_ui_canvas_context *c,int32_t x,int32_t y){
    arcade_panel(c,x,y,38,50,0x43d0u);arcade_panel(c,x+1,y+1,36,48,0x1269u);
}
static void sol_recycle(const struct ln_ui_canvas_context *c,int32_t x,int32_t y){
    uint16_t ink=sol_waste_count?0xded9u:0x4bceu;
    box(c,x+9,y+18,20,2,ink);box(c,x+9,y+18,2,10,ink);
    box(c,x+9,y+31,20,2,ink);box(c,x+27,y+23,2,10,ink);
    for(int32_t i=0;i<5;++i){box(c,x+24+i,y+15+i,1,9-2*i,ink);box(c,x+13-i,y+28+i,1,9-2*i,ink);}
}
static void sol_draw_scene(const struct ln_ui_canvas_context *c){
    const uint16_t white=0xefbfu,muted=0x9d95u,gold=0xfed3u,green=0x7f56u,held=0x77ffu;
    box(c,0,0,320,240,0x0926u);arcade_shade(c,0,43,320,171,0x1b4au,0x11e7u);
    arcade_text(c,10,4,"Solitaire",white,false);
    char text[64],*p=text;append(&p,"MOVES  ");number(&p,sol_moves);append(&p,"   WINS  ");number(&p,best[SOLITAIRE]);*p=0;
    arcade_text(c,172,8,text,muted,true);
    uint8_t spot=sol_source!=255u?sol_source:sol_cursor;
    uint8_t depth=sol_source!=255u?sol_source_depth:sol_depth;
    uint8_t card=sol_visible_card(spot,depth);
    if(card!=255u)sol_card_name(text,card);
    else if(spot==0u){p=text;append(&p,sol_stock_count?"Stock  /  ":"Recycle  /  ");number(&p,sol_stock_count?sol_stock_count:sol_waste_count);append(&p," cards");*p=0;}
    else if(spot==1u){p=text;append(&p,"Waste is empty");*p=0;}
    else if(spot<6u){static const char *const labels[]={"Clubs foundation","Diamonds foundation","Hearts foundation","Spades foundation"};p=text;append(&p,labels[spot-2u]);*p=0;}
    else{p=text;append(&p,"Empty column  /  Kings only");*p=0;}
    arcade_text(c,10,27,text,sol_source!=255u?held:gold,true);
    if(sol_source!=255u){p=text;number(&p,sol_source>=6u?sol_count[sol_source-6u]-sol_source_depth:1u);append(&p," PICKED UP");*p=0;arcade_text(c,234,27,text,held,true);}
    else arcade_text(c,244,27,"DRAW ONE",muted,true);
    box(c,0,42,320,1,0x43d0u);
    static const int16_t top_x[6]={7,51,139,183,227,271};
    for(uint8_t at=0;at<6u;++at){int32_t x=top_x[at],y=49;sol_slot(c,x,y);
        if(at==0u){if(sol_stock_count)sol_card(c,x,y,0,true,50);else sol_recycle(c,x,y);}
        else if(at==1u&&sol_waste_count)sol_card(c,x,y,sol_waste[sol_waste_count-1u],false,50);
        else if(at>=2u){uint8_t suit=at-2u,rank=sol_foundation[suit];
            if(rank)sol_card(c,x,y,(uint8_t)(suit*13u+rank-1u),false,50);
            else sol_suit_mark(c,x+12,y+18,suit,2u,sol_red(suit*13u)?0xb391u:0x7cf2u);}
        if(sol_can_place(at))sol_outline(c,x,y,50,green);
        if(sol_source==at)sol_outline(c,x,y,50,held);
        if(sol_cursor==at)sol_outline(c,x,y,50,gold);
    }
    bool preview=sol_source!=255u;
    for(uint8_t col=0;col<7u;++col){
        int32_t x=7+(int32_t)col*44,y=110,hidden_step=3,step=13;
        uint8_t length=sol_count[col],hidden=sol_hidden[col],faces=length-hidden;
        if(faces>1u&&(50-hidden*hidden_step)/(faces-1u)<7)hidden_step=2;
        if(faces>1u){step=(50-hidden*hidden_step)/(faces-1u);if(step>13)step=13;if(step<3)step=3;}
        int32_t last_y=y+hidden*hidden_step+(faces?faces-1u:0u)*step;
        if(!length){sol_slot(c,x,y);sol_glyph(c,x+16,y+21,'K',0x7cf2u);last_y=y;}
        for(uint8_t i=0;i<length;++i){int32_t at=y+(i<hidden?i*hidden_step:hidden*hidden_step+(i-hidden)*step);
            sol_card(c,x,at,sol_tableau[col][i],i<hidden,i+1u==length?50:i<hidden?hidden_step:step);}
        if(sol_can_place(col+6u))sol_outline(c,x,last_y,50,green);
        if(sol_cursor==col+6u){int32_t at=length?y+hidden*hidden_step+(sol_depth-hidden)*step:y;
            sol_outline(c,x,at,length&&sol_depth+1u<length?step:50,gold);
            if(step<11&&card!=255u)preview=true;}
        if(sol_source==col+6u){int32_t at=y+hidden*hidden_step+(sol_source_depth-hidden)*step;
            sol_outline(c,x,at,last_y+50-at,held);}
    }
    if(preview&&card!=255u){sol_card(c,95,49,card,false,50);sol_outline(c,95,49,50,held);
        arcade_center(c,114,100,sol_source!=255u?"HELD":"VIEW",held,true,1);}
    else{uint32_t home=0;for(uint8_t i=0;i<4u;++i)home+=sol_foundation[i];
        p=text;number(&p,home);append(&p,"/52");*p=0;arcade_center(c,114,66,text,gold,true,1);arcade_center(c,114,80,"HOME",muted,true,1);}
    box(c,0,214,320,26,0x0926u);box(c,0,214,320,1,0x43d0u);
    const char *hint=sol_message?sol_message:sol_source!=255u?
        (sol_can_place(sol_cursor)?"Select: place here  /  Green marks legal piles":"Wheel: target  /  Menu: cancel pickup"):
        sol_cursor==0u?(sol_stock_count?"Select: draw a card":"Select: recycle waste"):
        sol_cursor>=6u?"Select: pick up  /  </>: choose card":"Select: pick up";
    arcade_text(c,9,217,hint,white,true);
    arcade_text(c,9,229,"Wheel: pile",muted,true);arcade_text(c,113,229,"Play: options",muted,true);arcade_text(c,245,229,sol_source!=255u?"Menu: cancel":"Menu: back",muted,true);
    if(games_phase==PAUSED||games_phase==SOL_CONFIRM||games_phase==WON){
        box(c,0,215,320,25,0x0926u);
        arcade_center(c,160,221,games_phase==PAUSED?"Play or Menu: resume":games_phase==SOL_CONFIRM?"Menu or Play: cancel":"Menu: return to Games",muted,true,1);
        arcade_panel(c,28,52,264,156,0xded9u);arcade_panel(c,29,53,262,154,0x0926u);
        if(games_phase==PAUSED){
            arcade_center(c,160,63,"Paused",white,false,2);
            static const char *const options[]={"Resume game","Undo last move","New deal"};
            for(uint8_t i=0;i<3u;++i){int32_t y=99+i*29;
                if(sol_menu==i)arcade_panel(c,44,y,232,25,gold);
                arcade_text(c,59,y+4,options[i],sol_menu==i?0x0926u:i==1u&&!sol_history_count?0x63afu:white,false);}
            arcade_center(c,160,190,sol_message?sol_message:"Wheel: choose  /  Select: confirm",muted,true,1);
        }else if(games_phase==SOL_CONFIRM){
            arcade_center(c,160,69,"New deal?",white,false,2);
            arcade_center(c,160,110,"This replaces the current game",muted,true,1);
            arcade_center(c,160,127,"and clears its undo history.",muted,true,1);
            arcade_panel(c,48,153,224,25,gold);arcade_center(c,160,158,"Select  /  Deal again",0x0926u,false,1);
            arcade_center(c,160,188,"Menu or Play: keep this game",muted,true,1);
        }else{
            arcade_center(c,160,68,"Beautifully played",gold,false,1);
            arcade_center(c,160,94,"All four suits home",white,false,2);
            p=text;number(&p,sol_moves);append(&p," moves  /  ");number(&p,best[SOLITAIRE]);append(&p," wins");*p=0;
            arcade_center(c,160,135,text,muted,true,1);
            arcade_panel(c,48,166,224,25,gold);arcade_center(c,160,171,"Select  /  New deal",0x0926u,false,1);
        }
    }
}
