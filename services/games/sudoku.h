/* Shared Sudoku state and rules. Presentation-specific drawing lives beside the
 * other game renderers; this file stays allocation-free for both PP502x targets. */
static const char *const sud_puzzles[4]={
    "530070000600195000098000060800060003400803001700020006060000280000419005000080079",
    "000260701680070090190004500820100040004602900050003028009300074040050036703018000",
    "000000907000420180000705026100904000050000040000507009920108000034059000507000000",
    "005300000800000020070010500400005300010070006003200080060500009004000030000009700"
};
static const char *const sud_solutions[4]={
    "534678912672195348198342567859761423426853791713924856961537284287419635345286179",
    "435269781682571493197834562826195347374682915951743628519326874248957136763418259",
    "462831957795426183381795426173984265659312748248567319926178534834259671517643892",
    "145327698839654127672918543496185372218473956753296481367542819984761235521839764"
};
static const char *const sud_levels[]={"Easy","Normal","Hard","Very Hard"};
static uint8_t sud_board[81],sud_solution[81],sud_initial[81],sud_given[81];
static uint16_t sud_notes[81];
static uint8_t sud_cursor,sud_level,sud_choice,sud_picker,sud_menu,sud_confirm,sud_errors,sud_hints;
static bool sud_pencil;
static uint32_t sud_elapsed,sud_clock_at,sud_clock_remainder;
static const char *sud_message;

static uint8_t sud_blanks(void){uint8_t n=0;for(uint8_t i=0;i<81u;++i)if(!sud_board[i])++n;return n;}
static bool sud_complete(void){for(uint8_t i=0;i<81u;++i)if(sud_board[i]!=sud_solution[i])return false;return true;}
static void sud_time(char out[8]){
    uint32_t seconds=sud_elapsed>5999u?5999u:sud_elapsed,minutes=seconds/60u;seconds%=60u;
    out[0]=(char)('0'+minutes/10u);out[1]=(char)('0'+minutes%10u);out[2]=':';
    out[3]=(char)('0'+seconds/10u);out[4]=(char)('0'+seconds%10u);out[5]=0;
}
static void sud_restart(void){
    for(uint8_t i=0;i<81u;++i){sud_board[i]=sud_initial[i];sud_notes[i]=0;}
    sud_cursor=0;while(sud_cursor<80u&&sud_given[sud_cursor])++sud_cursor;
    sud_elapsed=sud_clock_at=sud_clock_remainder=0;sud_errors=sud_hints=0;sud_pencil=false;sud_message=0;games_phase=PLAY;
}
static void sud_begin(uint8_t difficulty){
    uint8_t digits[9];for(uint8_t i=0;i<9u;++i)digits[i]=i+1u;
    for(uint8_t i=8u;i;i--){uint8_t j=(uint8_t)(random_next()%(i+1u)),swap=digits[i];digits[i]=digits[j];digits[j]=swap;}
    const char *p=sud_puzzles[difficulty],*solution=sud_solutions[difficulty];
    for(uint8_t i=0;i<81u;++i){uint8_t clue=(uint8_t)(p[i]-'0'),answer=(uint8_t)(solution[i]-'0');
        sud_solution[i]=digits[answer-1u];sud_board[i]=clue?digits[clue-1u]:0u;sud_initial[i]=sud_board[i];sud_given[i]=clue!=0u;sud_notes[i]=0;}
    sud_level=difficulty;sud_choice=difficulty;sud_menu=sud_confirm=sud_errors=sud_hints=0;sud_pencil=false;sud_message=0;
    sud_cursor=0;while(sud_cursor<80u&&sud_given[sud_cursor])++sud_cursor;
    sud_elapsed=sud_clock_at=sud_clock_remainder=0;games_phase=PLAY;sound_pending=1u;++hud_revision;
}
static void sud_clear_peer_notes(uint8_t cell,uint8_t digit){
    uint16_t mask=(uint16_t)~(1u<<(digit-1u));uint8_t row=cell/9u,col=cell%9u;
    for(uint8_t i=0;i<9u;++i){sud_notes[row*9u+i]&=mask;sud_notes[i*9u+col]&=mask;}
    uint8_t top=(row/3u)*3u,left=(col/3u)*3u;
    for(uint8_t y=0;y<3u;++y)for(uint8_t x=0;x<3u;++x)sud_notes[(top+y)*9u+left+x]&=mask;
}
static void sud_hint(void){
    uint8_t cell=sud_cursor;if(sud_board[cell]==sud_solution[cell]){cell=0;while(cell<81u&&sud_board[cell]==sud_solution[cell])++cell;}
    if(cell>=81u)return;sud_cursor=cell;sud_board[cell]=sud_solution[cell];sud_notes[cell]=0;sud_clear_peer_notes(cell,sud_board[cell]);
    if(sud_hints<255u)++sud_hints;sud_message="Hint placed.";sound_pending=2u;
    if(sud_complete()){games_phase=WON;sound_pending=4u;sud_clock_at=0;}
}
static void sud_place(void){
    uint8_t cell=sud_cursor;
    if(sud_given[cell]){games_phase=PLAY;sud_message="This clue is fixed.";sound_pending=3u;return;}
    if(sud_pencil){
        if(!sud_picker)sud_notes[cell]=0;
        else{sud_board[cell]=0;sud_notes[cell]^=(uint16_t)(1u<<(sud_picker-1u));}
        games_phase=PLAY;sud_message=sud_picker?"Candidate updated.":"Cell cleared.";sound_pending=2u;return;
    }
    if(!sud_picker){sud_board[cell]=0;sud_notes[cell]=0;games_phase=PLAY;sud_message="Cell cleared.";sound_pending=2u;return;}
    if(sud_picker!=sud_solution[cell]){if(sud_errors<255u)++sud_errors;sud_message="That number does not fit.";sound_pending=3u;return;}
    sud_board[cell]=sud_picker;sud_notes[cell]=0;sud_clear_peer_notes(cell,sud_picker);games_phase=PLAY;sud_message="Number placed.";sound_pending=2u;
    if(sud_complete()){games_phase=WON;sound_pending=4u;sud_clock_at=0;}
}
static bool sud_tick(uint32_t now){
    if(games_phase!=PLAY&&games_phase!=SUDOKU_PICK){sud_clock_at=0;return false;}
    if(!sud_clock_at){sud_clock_at=now;return false;}
    uint32_t elapsed=now-sud_clock_at;sud_clock_at=now;sud_clock_remainder+=elapsed;bool changed=false;
    while(sud_clock_remainder>=1000000u){sud_clock_remainder-=1000000u;if(sud_elapsed<5999u)++sud_elapsed;changed=true;}
    if(changed)++hud_revision;return changed;
}
