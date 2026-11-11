#ifndef LINNET_GAME_MUSIC_QUIZ_H
#define LINNET_GAME_MUSIC_QUIZ_H

#if defined(LINNET_AUDIO)
static char mq_answer[4][40];
static uint8_t mq_focus,mq_correct,mq_round,mq_streak,mq_answered,mq_clip_paused,mq_unavailable,mq_pending;
static uint32_t mq_target,mq_clip_sample,mq_clip_active_us,mq_clip_elapsed;
static uint32_t mq_scan_count,mq_scan_start,mq_scan_at,mq_ids[4];
static uint8_t mq_found;
static bool mq_first;
static void mq_copy(char out[40],const char *in){uint8_t i=0;if(!in)in="";while(i<30u&&in[i]){out[i]=in[i];++i;}out[i]=0;}
static char mq_fold(char c){return c>='A'&&c<='Z'?(char)(c+'a'-'A'):c;}
static bool mq_same(const char *a,const char *b){while(*a&&mq_fold(*a)==mq_fold(*b)){++a;++b;}return *a==*b;}
enum {MQ_BUSY,MQ_STARTED,MQ_NEEDS_TITLES,MQ_SCANNING};
/* Titles are cached by the host. Bound even a 100k-track duplicate-title scan;
 * reuse the four prepared choices after playback starts instead of scanning twice. */
static uint8_t mq_scan(void){
    for(uint32_t budget=0;budget<32u&&mq_scan_at<mq_scan_count&&mq_found<4u;++budget){
        uint32_t at=(mq_scan_start+mq_scan_at++)%mq_scan_count;char candidate[40];mq_copy(candidate,ln_music_library_title(at));
        bool unique=candidate[0]!=0;for(uint8_t j=0;j<mq_found&&unique;++j)if(mq_same(candidate,mq_answer[j]))unique=false;
        if(unique){mq_copy(mq_answer[mq_found],candidate);mq_ids[mq_found++]=at;}
    }
    if(mq_found<4u)return mq_scan_at<mq_scan_count?MQ_SCANNING:MQ_NEEDS_TITLES;
    uint8_t pick=(uint8_t)(random_next()&3u);
    if(!mq_first&&mq_ids[pick]==mq_target)pick=(pick+1u)&3u;mq_target=mq_ids[pick];mq_correct=pick;
    bool started=mq_first?ln_music_quiz_begin(mq_target):
        (ln_music_quiz_select(mq_target)||ln_music_quiz_begin(mq_target));
    return started?MQ_STARTED:MQ_BUSY;
}
static uint8_t mq_choose(bool first){
    mq_scan_count=ln_music_library_count();mq_scan_start=mq_scan_count?random_next()%mq_scan_count:0;
    mq_scan_at=0;mq_found=0;mq_first=first;return mq_scan();
}
static void mq_choice_state(uint8_t result){mq_unavailable=result==MQ_NEEDS_TITLES;mq_pending=result==MQ_SCANNING?2u:result==MQ_BUSY?1u:0u;ex_phase=mq_unavailable?EX_LOST:EX_LOADING;}
static bool mq_prepare(void){
    const struct ln_music_library_row *row=ln_music_current();uint32_t count=ln_music_library_count();
    if(!row||row->index!=mq_target||count<4u||!row->title[0])return false;
    mq_copy(mq_answer[mq_correct],row->title);
    mq_focus=0;mq_answered=0;mq_clip_sample=mq_clip_active_us=mq_clip_elapsed=0;mq_clip_paused=0;(void)ln_player_pause(false);ex_phase=EX_PLAY;return true;
}
static void mq_start(void){mq_round=0;mq_streak=0;mq_focus=0;mq_answered=0;mq_unavailable=mq_pending=0;mq_target=UINT32_MAX;mq_choice_state(mq_choose(true));}
static void mq_restart(void){mq_round=0;mq_streak=0;mq_focus=0;mq_answered=0;mq_unavailable=mq_pending=0;score=0;previous_best=best[MUSIC_QUIZ];mq_choice_state(mq_choose(false));}
static void mq_retry(void){if(ln_music_loading())return;if(mq_pending==2u)return;if(mq_pending){if(ln_music_quiz_select(mq_target)||ln_music_quiz_begin(mq_target))mq_pending=0;return;}mq_choice_state(mq_choose(mq_target==UINT32_MAX));}
static bool mq_tick(uint32_t now){
    if(ex_phase==EX_LOADING){
        if(mq_pending==2u){mq_choice_state(mq_scan());if(mq_unavailable||mq_pending!=2u){++hud_revision;return true;}return false;}
        if(!ln_music_loading()){if(mq_pending){if(ln_music_quiz_select(mq_target)||ln_music_quiz_begin(mq_target)){mq_pending=0;return true;}return false;}if(mq_prepare()){++hud_revision;return true;}mq_pending=1;}return false;}
    if(ex_phase!=EX_PLAY||mq_answered)return false;const struct ln_music_library_row *row=ln_music_current();
    if(ln_music_loading()||!row||row->index!=mq_target){ex_phase=EX_LOADING;return true;}
    struct ln_player_status player;ln_player_snapshot(&player);
    if(!mq_clip_sample){mq_clip_sample=now;return false;}
    uint32_t delta=now-mq_clip_sample;mq_clip_sample=now;
    if(player.phase!=LN_PLAYER_PLAY||mq_clip_paused)return false;
    mq_clip_active_us+=delta;uint32_t seconds=mq_clip_active_us/1000000u;
    if(seconds!=mq_clip_elapsed){mq_clip_elapsed=seconds;if(seconds>=8u){(void)ln_player_pause(true);mq_clip_paused=1;}return true;}return false;
}
static uint32_t mq_deadline(uint32_t now){
    if(ex_phase==EX_LOADING)return now+(mq_pending==2u?1000u:50000u);
    if(ex_phase!=EX_PLAY||mq_answered||mq_clip_paused)return UINT32_MAX;
    struct ln_player_status player;ln_player_snapshot(&player);
    if(player.phase!=LN_PLAYER_PLAY||!mq_clip_sample)return now+33333u;
    uint32_t boundary=(mq_clip_elapsed+1u)*1000000u;
    return mq_clip_active_us>=boundary?now:now+(boundary-mq_clip_active_us);
}
#endif

#endif
