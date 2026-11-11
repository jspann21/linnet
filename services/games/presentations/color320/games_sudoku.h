/* Shoji-inspired 320 x 240 Sudoku presentation, following the click-wheel
 * game's board -> number picker -> in-game Menu flow. */
static void sud_tiny(const struct ln_ui_canvas_context *c,int x,int y,uint8_t n,uint16_t ink){
    static const uint8_t bits[9][5]={{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},
        {7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    for(uint8_t yy=0;yy<5u;++yy)for(uint8_t xx=0;xx<3u;++xx)if(bits[n-1u][yy]&(4u>>xx))box(c,x+xx,y+yy,1,1,ink);
}
static void sud_320_button(const struct ln_ui_canvas_context *c,int x,int y,int w,const char *label,bool selected){
    arcade_panel(c,x,y,w,22,selected?0xf3acu:0xd6d1u);arcade_center(c,x+w/2,y+3,label,selected?0x41e8u:0x2104u,false,1);
}
static void sud_320_backdrop(const struct ln_ui_canvas_context *c){
    arcade_shade(c,0,0,320,240,0xf79du,0xded5u);box(c,0,0,320,28,0x5928u);box(c,0,27,320,2,0xbac8u);
    /* A quiet mountain, sun and bamboo echo the original game's Japanese scenery. */
    arcade_disc(c,267,73,25,0xe9e8u);
    for(int y=78;y<166;++y){int left=218-(y-78)/2,right=282+(y-78)/3;box(c,left,y,right-left,1,0xbdd2u);}
    box(c,292,39,5,179,0x7aebu);box(c,294,39,2,179,0xa42eu);
    for(int y=57;y<204;y+=27){box(c,286,y,16,3,0x6948u);for(int k=0;k<14;++k){box(c,295-k,y-9-k/2,9+k/2,1,0x8b6bu);box(c,297,y+5+k,10+k/2,1,0x8b6bu);}}
}
static void sud_320_level(const struct ln_ui_canvas_context *c){
    sud_320_backdrop(c);arcade_center(c,160,4,"Sudoku",0xffffu,false,1);
    arcade_center(c,117,43,"Choose a difficulty",0x5928u,false,1);
    arcade_center(c,117,62,"Each puzzle has one solution.",0x8a6au,true,1);
    for(uint8_t i=0;i<4u;++i)sud_320_button(c,34,87+i*29,166,sud_levels[i],sud_choice==i);
    arcade_text(c,29,219,"Wheel: choose",0x5928u,true);arcade_text(c,124,219,"Select: begin",0x5928u,true);arcade_text(c,237,219,"Menu: Games",0x5928u,true);
}
static void sud_320_grid(const struct ln_ui_canvas_context *c){
    const int ox=7,oy=30,cell=21;uint8_t selected=sud_board[sud_cursor];
    for(uint8_t i=0;i<81u;++i){int row=i/9,col=i%9,x=ox+col*cell,y=oy+row*cell;bool peer=row==sud_cursor/9||col==sud_cursor%9||(row/3==sud_cursor/27&&col/3==(sud_cursor%9)/3);
        uint16_t paper=((row/3+col/3)&1)?0xef18u:0xffbbu;if(peer)paper=0xff55u;if(selected&&sud_board[i]==selected)paper=0xf68cu;if(i==sud_cursor)paper=0xf3acu;
        box(c,x+1,y+1,cell-1,cell-1,paper);
        if(sud_board[i]){char n[2]={(char)('0'+sud_board[i]),0};arcade_center(c,x+11,y+2,n,sud_given[i]?0x2925u:0x1352u,false,1);}
        else if(sud_notes[i])for(uint8_t n=1;n<=9u;++n)if(sud_notes[i]&(1u<<(n-1u)))sud_tiny(c,x+3+(n-1u)%3u*6,y+2+(n-1u)/3u*6,n,0x5a6du);
    }
    for(int i=0;i<=9;++i){int p=i*cell,w=i%3?1:2;box(c,ox+p,oy,w,189,0x4a49u);box(c,ox,oy+p,189,w,0x4a49u);}
    box(c,ox+sud_cursor%9*cell,oy+sud_cursor/9*cell,cell,2,0xb980u);box(c,ox+sud_cursor%9*cell,oy+(sud_cursor/9+1)*cell-2,cell,2,0xb980u);
    box(c,ox+sud_cursor%9*cell,oy+sud_cursor/9*cell,2,cell,0xb980u);box(c,ox+(sud_cursor%9+1)*cell-2,oy+sud_cursor/9*cell,2,cell,0xb980u);
}
static void sud_320_sidebar(const struct ln_ui_canvas_context *c){
    char text[24],time[8],*p=text;sud_time(time);arcade_panel(c,204,31,109,187,0xf79du);box(c,206,33,105,183,0xeed8u);
    arcade_center(c,258,39,sud_levels[sud_level],0x5928u,false,1);arcade_center(c,258,56,time,0x2925u,false,1);
    if(games_phase==SUDOKU_PICK){
        arcade_center(c,258,77,sud_pencil?"PENCIL MARK":"CHOOSE NUMBER",0x9a49u,true,1);
        for(uint8_t n=0;n<10u;++n){int x=211+(n&1u)*49,y=91+(n/2u)*23;char label[2]={(char)(n?'0'+n:'X'),0};sud_320_button(c,x,y,45,label,sud_picker==n);}
        arcade_center(c,258,207,sud_message?"Try another.":"Menu: cancel",sud_message?0x9a49u:0x8a6au,true,1);return;
    }
    p=text;number(&p,sud_blanks());append(&p," empty");*p=0;arcade_center(c,258,80,text,0x8a6au,true,1);
    p=text;number(&p,sud_errors);append(&p," mistakes");*p=0;arcade_center(c,258,94,text,0x8a6au,true,1);
    p=text;number(&p,sud_hints);append(&p," hints");*p=0;arcade_center(c,258,108,text,0x8a6au,true,1);
    arcade_panel(c,215,133,86,31,sud_pencil?0x79b0u:0xd6d1u);arcade_center(c,258,138,sud_pencil?"PENCIL":"NUMBER",sud_pencil?0xffffu:0x5928u,false,1);
    arcade_center(c,258,174,"Play changes mode",0x8a6au,true,1);arcade_center(c,258,188,"Menu: options",0x8a6au,true,1);
    if(sud_message)arcade_center(c,258,203,sud_message,0x9a49u,true,1);
}
static void sud_320_modal(const struct ln_ui_canvas_context *c){
    if(games_phase!=SUDOKU_MENU&&games_phase!=SUDOKU_CONFIRM&&games_phase!=WON)return;
    arcade_panel(c,42,30,236,190,0x8a6au);arcade_panel(c,44,32,232,186,0xf7dau);
    if(games_phase==SUDOKU_MENU){static const char *const items[]={"Resume","Hint","Check puzzle","Restart","Difficulty","Return to Games"};
        arcade_center(c,160,42,"Menu",0x5928u,false,2);
        for(uint8_t i=0;i<6u;++i)sud_320_button(c,65,73+i*23,190,items[i],sud_menu==i);
    }else if(games_phase==SUDOKU_CONFIRM){const char *title=sud_confirm==1u?"Restart puzzle?":sud_confirm==2u?"Choose difficulty?":"Return to Games?";
        arcade_center(c,160,56,title,0x5928u,false,2);arcade_center(c,160,96,"Your current progress will be lost.",0x8a6au,true,1);
        sud_320_button(c,66,128,188,"Select / Confirm",true);arcade_center(c,160,170,"Menu or Play: cancel",0x8a6au,true,1);
    }else{char time[8];sud_time(time);arcade_center(c,160,51,"Puzzle complete",0x9a49u,false,2);arcade_center(c,160,91,time,0x5928u,false,2);
        char s[48],*p=s;number(&p,sud_errors);append(&p," mistakes  /  ");number(&p,sud_hints);append(&p," hints");*p=0;arcade_center(c,160,126,s,0x8a6au,true,1);
        sud_320_button(c,66,153,188,"Select / New puzzle",true);arcade_center(c,160,190,"Menu: return to Games",0x8a6au,true,1);}
}
static void sud_draw_320(const struct ln_ui_canvas_context *c){
    if(games_phase==SUDOKU_LEVEL){sud_320_level(c);return;}
    box(c,0,0,320,240,0xeed8u);box(c,0,0,320,28,0x5928u);arcade_text(c,9,4,"Sudoku",0xffffu,false);
    char time[8];sud_time(time);arcade_text(c,224,7,sud_levels[sud_level],0xf3acu,true);arcade_text(c,280,7,time,0xffffu,true);
    sud_320_grid(c);sud_320_sidebar(c);box(c,0,219,320,21,0x5928u);
    arcade_text(c,8,222,"Wheel: cell",0xffffu,true);arcade_text(c,86,222,"Select: number",0xffffu,true);arcade_text(c,181,222,"Play: pencil",0xf3acu,true);arcade_text(c,263,222,"Menu",0xffffu,true);
    sud_320_modal(c);
}
