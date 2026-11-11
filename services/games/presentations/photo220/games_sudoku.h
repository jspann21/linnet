/* Compact 220 x 176 Sudoku presentation for the iPod Photo. */
static void photo_sud_tiny(const struct ln_ui_canvas_context *c,int x,int y,uint8_t n,uint16_t ink){
    static const uint8_t bits[9][5]={{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},
        {7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    for(uint8_t yy=0;yy<5u;++yy)for(uint8_t xx=0;xx<3u;++xx)if(bits[n-1u][yy]&(4u>>xx))box(c,x+xx,y+yy,1,1,ink);
}
static void photo_sud_button(const struct ln_ui_canvas_context *c,int x,int y,int w,const char *label,bool selected){
    arcade_panel(c,x,y,w,19,selected?0xf3acu:0xd6d1u);arcade_center(c,x+w/2,y+3,label,selected?0x41e8u:0x2104u,true,1);
}
static const char *photo_sud_message(void){
    if(!sud_message)return 0;
    return sud_message[0]=='T'?(sud_message[5]=='n'?"Try again":"Fixed clue"):
        sud_message[0]=='H'?"Hint placed":sud_message[0]=='C'?(sud_message[1]=='a'?"Updated":"Cleared"):
        sud_message[0]=='N'?"Placed":sud_message[0]=='E'?"Looks good":"Mode changed";
}
static void photo_sud_backdrop(const struct ln_ui_canvas_context *c){
    arcade_shade(c,0,0,220,176,0xf79du,0xded5u);box(c,0,0,220,21,0x5928u);arcade_disc(c,184,50,18,0xe9e8u);
    for(int y=59;y<149;++y){int left=151-(y-59)/2;box(c,left,y,220-left,1,0xbdd2u);}
    box(c,205,29,4,143,0x7aebu);for(int y=45;y<154;y+=24){box(c,200,y,13,2,0x6948u);for(int k=0;k<10;++k)box(c,207-k,y-6-k/2,7+k/2,1,0x8b6bu);}
}
static void photo_sud_level(const struct ln_ui_canvas_context *c){
    photo_sud_backdrop(c);arcade_center(c,110,2,"Sudoku",0xffffu,false,1);photo_center(c,29,"Choose a difficulty",0x5928u);
    for(uint8_t i=0;i<4u;++i)photo_sud_button(c,28,52+i*25,154,sud_levels[i],sud_choice==i);
    photo_label(c,8,163,"Wheel: choose",0x5928u);photo_label(c,145,163,"Select: begin",0x5928u);
}
static void photo_sud_grid(const struct ln_ui_canvas_context *c){
    const int ox=2,oy=20,cell=17;uint8_t selected=sud_board[sud_cursor];
    for(uint8_t i=0;i<81u;++i){int row=i/9,col=i%9,x=ox+col*cell,y=oy+row*cell;bool peer=row==sud_cursor/9||col==sud_cursor%9||(row/3==sud_cursor/27&&col/3==(sud_cursor%9)/3);
        uint16_t paper=((row/3+col/3)&1)?0xef18u:0xffbbu;if(peer)paper=0xff55u;if(selected&&sud_board[i]==selected)paper=0xf68cu;if(i==sud_cursor)paper=0xf3acu;box(c,x+1,y+1,cell-1,cell-1,paper);
        if(sud_board[i]){char n[2]={(char)('0'+sud_board[i]),0};arcade_center(c,x+9,y+1,n,sud_given[i]?0x2925u:0x1352u,false,1);}
        else if(sud_notes[i])for(uint8_t n=1;n<=9u;++n)if(sud_notes[i]&(1u<<(n-1u)))photo_sud_tiny(c,x+2+(n-1u)%3u*5,y+1+(n-1u)/3u*5,n,0x5a6du);
    }
    for(int i=0;i<=9;++i){int p=i*cell,w=i%3?1:2;box(c,ox+p,oy,w,153,0x4a49u);box(c,ox,oy+p,153,w,0x4a49u);}
    int x=ox+sud_cursor%9*cell,y=oy+sud_cursor/9*cell;box(c,x,y,cell,2,0xb980u);box(c,x,y+cell-2,cell,2,0xb980u);box(c,x,y,2,cell,0xb980u);box(c,x+cell-2,y,2,cell,0xb980u);
}
static void photo_sud_side(const struct ln_ui_canvas_context *c){
    char time[8],text[18],*p=text;sud_time(time);box(c,157,20,63,156,0xeed8u);
    arcade_center(c,188,23,sud_levels[sud_level],0x5928u,true,1);arcade_center(c,188,35,time,0x2925u,true,1);
    if(games_phase==SUDOKU_PICK){photo_center(c,49,sud_pencil?"PENCIL":"NUMBER",0x9a49u);
        for(uint8_t n=0;n<10u;++n){int x=160+(n&1u)*29,y=62+(n/2u)*21;char s[2]={(char)(n?'0'+n:'X'),0};photo_sud_button(c,x,y,27,s,sud_picker==n);}
        if(sud_message)arcade_center(c,188,166,photo_sud_message(),0x9a49u,true,1);return;}
    p=text;number(&p,sud_blanks());append(&p," empty");*p=0;arcade_center(c,188,52,text,0x8a6au,true,1);
    p=text;number(&p,sud_errors);append(&p," errors");*p=0;arcade_center(c,188,65,text,0x8a6au,true,1);
    arcade_panel(c,163,83,51,25,sud_pencil?0x79b0u:0xd6d1u);arcade_center(c,188,88,sud_pencil?"PENCIL":"NUMBER",sud_pencil?0xffffu:0x5928u,true,1);
    arcade_center(c,188,118,"Play: mode",0x8a6au,true,1);arcade_center(c,188,131,"Menu: options",0x8a6au,true,1);
    if(sud_message)arcade_center(c,188,148,photo_sud_message(),0x9a49u,true,1);
}
static void photo_sud_modal(const struct ln_ui_canvas_context *c){
    if(games_phase!=SUDOKU_MENU&&games_phase!=SUDOKU_CONFIRM&&games_phase!=WON)return;
    arcade_panel(c,10,14,200,154,0x8a6au);arcade_panel(c,12,16,196,150,0xf7dau);
    if(games_phase==SUDOKU_MENU){static const char *const items[]={"Resume","Hint","Check puzzle","Restart","Difficulty","Return to Games"};
        arcade_center(c,110,20,"Menu",0x5928u,false,1);for(uint8_t i=0;i<6u;++i)photo_sud_button(c,30,42+i*20,160,items[i],sud_menu==i);
    }else if(games_phase==SUDOKU_CONFIRM){const char *title=sud_confirm==1u?"Restart puzzle?":sud_confirm==2u?"Choose difficulty?":"Return to Games?";
        arcade_center(c,110,31,title,0x5928u,false,2);photo_center(c,74,"Current progress will be lost.",0x8a6au);photo_sud_button(c,30,101,160,"Select / Confirm",true);photo_center(c,137,"Menu or Play: cancel",0x8a6au);
    }else{char time[8],s[40],*p=s;sud_time(time);arcade_center(c,110,27,"Puzzle complete",0x9a49u,false,1);arcade_center(c,110,52,time,0x5928u,false,2);
        p=s;number(&p,sud_errors);append(&p," errors / ");number(&p,sud_hints);append(&p," hints");*p=0;photo_center(c,88,s,0x8a6au);photo_sud_button(c,30,111,160,"Select / New puzzle",true);photo_center(c,145,"Menu: return to Games",0x8a6au);}
}
static void photo_sudoku(const struct ln_ui_canvas_context *c){
    if(games_phase==SUDOKU_LEVEL){photo_sud_level(c);return;}
    box(c,0,0,220,176,0xeed8u);box(c,0,0,220,19,0x5928u);photo_label(c,5,3,"Sudoku",0xffffu);
    char time[8];sud_time(time);photo_label(c,158,3,sud_levels[sud_level],0xf3acu);photo_label(c,193,3,time,0xffffu);
    photo_sud_grid(c);photo_sud_side(c);photo_sud_modal(c);
}
