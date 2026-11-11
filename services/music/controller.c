#include "services/music/controller.h"
#include "services/music/menu.h"
#include "services/music/player.h"
#include "services/music/artwork.h"
#include "services/music/coverflow.h"
#include "services/music/queue.h"
#include "services/music/history.h"
#include "services/music/browser.h"
#include "services/music/profile.h"
#include "services/music/settings.h"
#include "services/music/collections.h"
#include "services/display/settings.h"
#include "services/music/search.h"
#include "services/music/queue_view.h"
#if defined(LINNET_FILES)
#include "services/files/files.h"
#endif
#include "services/storage/storage.h"
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
#include "services/music/history_pages.h"
#endif
#if defined(LINNET_PHOTOS)
#include "services/photos/photos.h"
#endif
#include "services/state/store.h"
#include "hal/api/audio.h"
#include "hal/api/platform.h"
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
#include "kernel/runtime.h"
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#endif
#endif
static struct ln_queue queue;
/* Selection and resume decoding finish synchronously before yielding; neither
 * retains its candidate after publishing the active queue. */
static struct ln_queue queue_candidate;
static struct ln_music_profile profile;
static bool sound_pending;
static uint32_t sound_edits;
static struct ln_music_library_row current,waiting;
static struct ln_fat32_file direct_file;
static bool file_mode,file_waiting;
bool ln_music_file_mode(void){return file_mode;}
static bool have_current,have_waiting,active,restored,selecting,persist_failed;
static bool restore_remap;
static uint32_t revision,saved_revision,skipped;
static uint32_t restore_attempts,restore_retry_at,restore_revision;
static uint8_t resume[LN_COLLECTION_RESUME];
static uint64_t requested_position;
static bool headphone_known,headphones,recent_recorded;
static uint64_t recent_id;
static struct ln_music_library_row upcoming;
static bool prefetching,next_ready,next_armed,next_failed;
static bool manual_next_pending;
static uint32_t next_index,next_token;
static uint32_t resolver_kind,resolver_token,resolver_ordinal,next_ordinal,current_cursor,waiting_cursor;
static bool show_now_playing;
static bool shutting_down,stepping,pause_captured;
static bool queue_opened,queue_clear_confirm;
static uint32_t queue_choice,queue_choice_revision;
static const char *notice;
static struct ln_music_browser_queue additions;
static uint32_t additions_revision;static bool adding;
static struct ln_queue expanded_queue;
static struct ln_queue quiz_saved_queue;
static bool quiz_active,quiz_saved_active,quiz_saved_paused,quiz_restore_pause;
static uint64_t quiz_saved_track_id,quiz_restore_track_id;
static uint64_t quiz_saved_position;
#define LN_MUSIC_VOLUME_RETRY_US 100000u
static uint32_t volume_retry_at;
static bool volume_retry_pending;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t budget_steps,budget_max_step_us,budget_photo_windows,budget_urgent_windows;
static uint32_t budget_metadata_hits,budget_metadata_iterations,budget_next_arms,budget_transitions;
static uint32_t budget_low_pcm_windows,budget_min_queued;
static void budget_count(uint32_t *value){if(*value!=UINT32_MAX)++*value;}
void ln_music_budget_diagnostics(uint32_t output[12]){
    output[0]=0x4744424du;output[1]=budget_steps;output[2]=budget_max_step_us;
    output[3]=budget_photo_windows;output[4]=budget_urgent_windows;
    output[5]=budget_metadata_hits;output[6]=budget_metadata_iterations;
    output[7]=budget_next_arms;output[8]=budget_transitions;
    output[9]=budget_low_pcm_windows;output[10]=budget_min_queued;output[11]=0;
}
#endif
static const uint32_t sleep_minutes[]={1,5,10,15,30,45,60,90,120};
static const char *const sleep_modes[]={"After: 1 minute","After: 5 minutes","After: 10 minutes","After: 15 minutes","After: 30 minutes","After: 45 minutes","After: 1 hour","After: 90 minutes","After: 2 hours","End of track","End of album"};
static const uint32_t sleep_fades[]={0,5,10,30};
static uint32_t sleep_choice=3,sleep_fade_choice,sleep_mode,sleep_fade;
static uint32_t sleep_left,sleep_fraction,sleep_sample,sleep_revision,sleep_attenuation;
static bool sleep_active,sleep_due,sleep_tail_ended;
static const char *sleep_notice;
static bool sleep_holds_next(void);
static void sleep_cancel_boundary(void);
static bool next_queue_cursor(uint32_t *cursor){
    if(!queue.count)return false;
    if(queue.repeat==LINNET_QUEUE_REPEAT_ONE)*cursor=queue.cursor;
    else if(queue.cursor+1u<queue.count)*cursor=queue.cursor+1u;
    else if(queue.repeat==LINNET_QUEUE_REPEAT_ALL)*cursor=0;
    else return false;
    return true;
}
static bool sleep_holds_next(void){
    if(!sleep_active||sleep_mode<9u)return false;
    if(sleep_mode==9u)return true;
    uint32_t cursor;
    return !have_current||!current.album[0]||!next_queue_cursor(&cursor)||cursor<=queue.cursor||
        next_failed||(next_ready&&upcoming.track.album!=current.track.album);
}
static void sleep_cancel(void){
    sleep_active=sleep_due=sleep_tail_ended=false;sleep_attenuation=0;sleep_notice=0;++sleep_revision;
}
static void sleep_cancel_boundary(void){if(sleep_active&&sleep_mode>=9u)sleep_cancel();}
uint32_t ln_music_sleep_revision(void){return sleep_revision;}
const char *ln_music_sleep_label(uint32_t row){
    static const char *const fades[]={"Fade out: Off","Fade out: 5 seconds","Fade out: 10 seconds","Fade out: 30 seconds"};
    if(row==0)return sleep_modes[sleep_choice];
    if(row==1)return fades[sleep_fade_choice];
    if(row==2)return sleep_active?"Restart timer":"Start timer";
    return "Cancel timer";
}
const char *ln_music_sleep_status(void){
    static char label[32];
    if(sleep_notice)return sleep_notice;
    if(!sleep_active)return "Timer off";
    if(sleep_due)return "Shutting down...";
    if(sleep_mode>=9u)return sleep_mode==9u?"Sleeping after this track":"Sleeping after this album";
    uint32_t minutes=sleep_left/60u,seconds=sleep_left%60u,n=0,count=0;char digits[10];
    const char *prefix="Remaining: ";while(*prefix)label[n++]=*prefix++;
    do{digits[count++]=(char)('0'+minutes%10u);minutes/=10u;}while(minutes);
    while(count)label[n++]=digits[--count];
    label[n++]=':';label[n++]=(char)('0'+seconds/10u);label[n++]=(char)('0'+seconds%10u);label[n]=0;return label;
}
bool ln_music_sleep_adjust(uint32_t row,int32_t direction){
    if(shutting_down||!direction||row>3u)return false;
    sleep_notice=0;
    if(row<2u){
        uint32_t *choice=row?&sleep_fade_choice:&sleep_choice,max=row?3u:10u;
        *choice=direction<0?(*choice?*choice-1u:max):(*choice==max?0u:*choice+1u);
        ++sleep_revision;return true;
    }
    if(row==3u){sleep_cancel();ln_music_volume_sync();return true;}
    if(sleep_choice>=9u){
        struct ln_player_status p;ln_player_snapshot(&p);
        if((!file_mode&&(!active||!have_current))||ln_music_loading()||
           (p.phase!=LN_PLAYER_PLAY&&p.phase!=LN_PLAYER_PRIME&&p.phase!=LN_PLAYER_PAUSE&&p.phase!=LN_PLAYER_DRAIN)){
            sleep_notice="Play a track first";++sleep_revision;return false;
        }
        /* Never claim an end boundary after next-track PCM is committed. */
        if(!ln_player_cancel_next()){sleep_notice="Track changing; try again";++sleep_revision;return false;}
        next_armed=false;
    }
    sleep_active=true;sleep_due=sleep_tail_ended=false;sleep_attenuation=0;
    sleep_mode=sleep_choice;sleep_fade=sleep_fades[sleep_fade_choice];
    sleep_left=sleep_mode<9u?sleep_minutes[sleep_mode]*60u:0;
    sleep_fraction=0;sleep_sample=ln_time_us();++sleep_revision;ln_music_volume_sync();return true;
}
bool ln_music_sleep_step(uint32_t now){
    if(!sleep_active)return false;
    uint32_t delta=now-sleep_sample;sleep_sample=now;
    if(sleep_mode<9u&&!sleep_due){
        uint32_t elapsed=delta/1000000u;
        sleep_fraction+=delta%1000000u;
        if(sleep_fraction>=1000000u){sleep_fraction-=1000000u;++elapsed;}
        if(elapsed){sleep_left=elapsed>=sleep_left?0:sleep_left-elapsed;++sleep_revision;}
        if(!sleep_left){sleep_due=true;++sleep_revision;}
    }
    uint32_t attenuation=0;
    if(sleep_fade){
        if(sleep_mode<9u&&sleep_left<=sleep_fade){
            uint32_t remaining=sleep_left*1000u;
            uint32_t fraction=sleep_fraction/1000u;
            remaining=remaining>fraction?remaining-fraction:0;
            attenuation=730u-(remaining*730u)/(sleep_fade*1000u);
        }else if(sleep_mode>=9u&&sleep_holds_next()){
            struct ln_player_status p;ln_player_snapshot(&p);
            uint64_t position=ln_player_position(),frames=p.frames?p.frames:current.track.frames;
            uint32_t window=p.rate*sleep_fade;
            uint64_t remaining=frames>position?frames-position:0;
            if(frames&&window&&remaining<=window)attenuation=730u-((uint32_t)remaining*730u)/window;
        }
    }
    if(sleep_due){attenuation=sleep_fade?730u:0;(void)ln_player_pause(true);}
    if(attenuation!=sleep_attenuation){sleep_attenuation=attenuation;ln_music_volume_sync();}
    return sleep_due;
}
static int32_t output_volume(void){
    int32_t value=queue.volume_db_tenths-(int32_t)sleep_attenuation+(int32_t)ln_player_headroom();
    if(value>profile.max_volume_db_tenths)value=profile.max_volume_db_tenths;
    return value< -730?-730:value;
}
int32_t ln_music_output_volume(void){
    int32_t value=output_volume();return value>profile.max_volume_db_tenths?profile.max_volume_db_tenths:value;
}
const char *ln_music_notice(void){return notice;}
__attribute__((noinline)) bool ln_music_loading(void){return file_waiting||selecting||have_waiting||manual_next_pending||resolver_kind==1
#if defined(LINNET_FILES)
    ||(file_mode&&ln_files_audio_pending())
#endif
    ;}
static void remember_position(void);
static void discard_next(bool restarting){
    ln_music_library_cancel_prefetch();
    prefetching=next_ready=next_armed=next_failed=false;
    manual_next_pending=false;
    if(resolver_kind==2)resolver_kind=0;
    if(!ln_player_cancel_next()){
        if(restarting)ln_player_stop_for_restart();else ln_player_stop();}
}
static bool same(const uint8_t*a,const uint8_t*b){for(uint32_t i=0;i<32;++i)if(a[i]!=b[i])return false;return true;}
static bool select_index(uint32_t index){
    file_mode=file_waiting=false;
    restore_remap=false;
    have_waiting=false;resolver_kind=0;notice=0;discard_next(true);waiting_cursor=queue.cursor;
    selecting=ln_music_library_select(index);return selecting;
}
static bool resolve_queue(uint32_t ordinal,uint32_t kind){
    if(queue.source!=LN_QUEUE_GROUP){
        if(kind==1)return select_index(ordinal);
        next_index=ordinal;return prefetching=ln_music_library_prefetch(ordinal);
    }
    uint32_t token=resolver_token+1u;if(!token)token=1;
    if(!ln_music_library_queue_member(queue.source_group,ordinal,token))return false;
    resolver_kind=kind;resolver_ordinal=ordinal;resolver_token=token;return true;
}
static bool select_current(void){
    file_mode=file_waiting=false;
    restore_remap=false;queue.last_track_id=0;
    uint32_t ordinal;if(!ln_queue_current(&queue,&ordinal))return false;
    have_waiting=selecting=false;resolver_kind=0;discard_next(true);
    return resolve_queue(ordinal,1);
}
void ln_music_queue_open(void){
    queue_opened=true;ln_music_search_close();ln_music_browser_suspend(true);
    ln_music_queue_view_open(&queue,revision);
}
void ln_music_queue_close(void){queue_opened=false;ln_music_queue_view_close();}
void ln_music_queue_choose(uint32_t row){queue_clear_confirm=false;notice=0;queue_choice=row;queue_choice_revision=revision;}
bool ln_music_queue_edit(uint32_t action){
    if(shutting_down||ln_music_loading()||queue_choice_revision!=revision||queue_choice>=queue.count||action>4){notice="Queue changed; choose again";++revision;return false;}
    if(action==4&&!queue_clear_confirm){queue_clear_confirm=true;notice="Select again to clear";++revision;queue_choice_revision=revision;return false;}
    if(action==4){notice="Queue cleared";queue_clear_confirm=false;int32_t volume=queue.volume_db_tenths;uint32_t repeat=queue.repeat;
        bool shuffled=queue.shuffled;ln_music_stop();ln_queue_init(&queue,ln_time_us());queue.shuffled=shuffled;(void)ln_music_library_identity(queue.library_identity);queue.volume_db_tenths=volume;queue.repeat=repeat;++revision;return true;}
    remember_position();uint32_t occurrence;
    if(queue.source){if(!ln_queue_track_at(&queue,queue_choice,&occurrence))return false;}
    else occurrence=queue.order[queue_choice];
    bool was_current=queue_choice==queue.cursor,accepted=false;
    if(action>1&&queue.source){notice="Large queue: reorder unavailable";++revision;return false;}
    if(action&& !ln_player_cancel_next()){notice="Track change in progress";++revision;return false;}
    if(action==0){
        sleep_cancel_boundary();accepted=ln_queue_select_at(&queue,queue_choice);
        if(accepted){active=true;requested_position=0;accepted=select_current();show_now_playing=accepted;}
    }else if(action==1){
        accepted=ln_queue_remove_occurrence(&queue,occurrence);
        if(accepted){
            discard_next(true);resolver_kind=0;
            if(was_current){sleep_cancel_boundary();ln_player_stop_for_restart();requested_position=0;active=queue.count!=0;
                if(active)(void)select_current();else ln_player_stop();}
            else current_cursor=queue.cursor;
        }
    }else {
        if(!queue.source&&queue.shuffled){
            uint32_t target=action==2?(queue_choice?queue_choice-1:queue_choice):(queue_choice+1<queue.count?queue_choice+1:queue_choice);
            uint32_t other=queue.order[target];queue.order[target]=queue.order[queue_choice];queue.order[queue_choice]=other;
            if(queue.cursor==queue_choice)queue.cursor=target;else if(queue.cursor==target)queue.cursor=queue_choice;
            accepted=true;
        }else{
            uint32_t target=action==2?(occurrence?occurrence-1:occurrence):(occurrence+1<queue.count?occurrence+1:occurrence);
            accepted=ln_queue_move_occurrence(&queue,occurrence,target);
        }
        if(accepted){discard_next(true);resolver_kind=0;current_cursor=queue.cursor;}
    }
    if(accepted){notice=0;++revision;}else {notice="Queue edit unavailable";++revision;}
    return accepted;
}
void ln_music_init(void){restore_attempts=restore_retry_at=0;restore_revision=revision;volume_retry_pending=false;volume_retry_at=0;
    sound_pending=false;sound_edits=0;
    ln_player_set_gain_sync(ln_music_volume_sync);
#if !defined(LINNET_PROFILE_RELEASE)
    budget_steps=budget_max_step_us=budget_photo_windows=budget_urgent_windows=0;
    budget_metadata_hits=budget_metadata_iterations=budget_next_arms=budget_transitions=budget_low_pcm_windows=0;
    budget_min_queued=UINT32_MAX;
#endif
    ln_queue_init(&queue,ln_time_us());ln_music_profile_default(&profile);(void)ln_audio_lineout(false);ln_music_library_init();ln_music_browser_init();ln_music_history_init();}
bool ln_music_take_now_playing(void){bool show=show_now_playing;show_now_playing=false;return show;}
const struct ln_music_library_row *ln_music_current(void){return have_current||file_mode?&current:0;}
bool ln_music_queue_position(uint32_t *position,uint32_t *count){
    if(!position||!count||!have_current||selecting||have_waiting||resolver_kind==1||
       !queue.count||queue.cursor>=queue.count||current_cursor!=queue.cursor)return false;
    *position=queue.cursor+1u;*count=queue.count;return true;
}
uint32_t ln_music_revision(void){return revision+ln_music_history_revision()+ln_collections_revision();}
static uint32_t sound_view,sound_parent;
#if defined(LINNET_HARDWARE_CLICKER)
#define SOUND_CLICKER true
#else
#define SOUND_CLICKER false
#endif
uint32_t ln_music_sound_count(void){return ln_sound_menu_count(sound_view,LINNET_EXTENDED_SOUND,SOUND_CLICKER);}
uint32_t ln_music_sound_action(uint32_t row){return ln_sound_menu_action(sound_view,row,LINNET_EXTENDED_SOUND,SOUND_CLICKER);}
const char *ln_music_sound_title(void){return sound_view==LN_SOUND_EQ?"Custom EQ":sound_view==LN_SOUND_ADVANCED?"Advanced Sound":"Sound";}
bool ln_music_sound_pending(void){return sound_pending||volume_retry_pending||ln_player_sound_pending();}
const char *ln_music_sound_status(void){return ln_music_sound_pending()?"Applying sound...":"Select: Open / Change";}
bool ln_music_sound_enter(uint32_t row){
    uint32_t action=ln_music_sound_action(row);
    if(action!=LN_SOUND_CUSTOM_EQ&&action!=LN_SOUND_MORE)return false;
    sound_parent=row;sound_view=action==LN_SOUND_CUSTOM_EQ?LN_SOUND_EQ:LN_SOUND_ADVANCED;return true;
}
bool ln_music_sound_back(uint32_t *row){
    if(sound_view==LN_SOUND_ROOT)return false;
    sound_view=LN_SOUND_ROOT;*row=sound_parent;return true;
}
const char *ln_music_sound_label(uint32_t row){
    uint32_t index=ln_music_sound_action(row);
    if(index==UINT32_MAX)return "";
    if(index==LN_SOUND_CUSTOM_EQ)return "Custom EQ";
    if(index==LN_SOUND_MORE)return "Advanced";
    if(index==LN_SOUND_CLICKER)return ln_display_click_label();
    if(index==LN_SOUND_CLICK_VOLUME)return ln_display_click_volume_label();
    if(index==LN_SOUND_CLICK_ENABLE)return ln_display_click_enabled_label();
    if(index==LN_SOUND_CLICK_OUTPUT)return ln_display_click_output_label();
    static char balance[24],preamp[24],levels[7][24];
    static const char *const modes[]={"Channels: Stereo","Channels: Mono","Channels: Left","Channels: Right","Channels: Karaoke"};
    if(index==0&&profile.preset==LN_MUSIC_EQ_CUSTOM)return "EQ: Custom";
    if(index==0){
        static const char *const presets[]={"EQ: Off","EQ: Acoustic","EQ: Bass Booster","EQ: Classical","EQ: Electronic","EQ: Hip Hop","EQ: Loudness","EQ: Lounge","EQ: Small Speakers","EQ: Spoken Word","EQ: Treble Booster","EQ: Vocal Booster"};
        return presets[profile.preset];
    }
#if LINNET_EXTENDED_SOUND
    if(index==1)return profile.replaygain==2?"Sound Check: Album":profile.replaygain?"Sound Check: Track":"Sound Check: Off";
#else
    if(index==1)return profile.replaygain==2?"ReplayGain: Album":profile.replaygain?"ReplayGain: Track":"ReplayGain: Off";
#endif
    if(index==2)return profile.crossfeed?"Crossfeed: On":"Crossfeed: Off";
    if(index==3){
        if(!profile.balance)return "Balance: Center";
        const char *prefix=profile.balance<0?"Balance: L ":"Balance: R ";uint32_t n=0;
        while(prefix[n]){balance[n]=prefix[n];++n;}uint32_t value=(uint32_t)(profile.balance<0?-profile.balance:profile.balance)/10u;
        if(value>=100)balance[n++]='1';if(value>=10)balance[n++]=(char)('0'+value/10u%10u);
        balance[n++]=(char)('0'+value%10u);balance[n++]='%';balance[n]=0;return balance;
    }
    if(index==4)return modes[profile.mode];
    if(index==5){uint32_t value=(uint32_t)(-profile.preamp_db_q16)/65536u,n=0;const char *prefix="Preamp: -";
        while(prefix[n]){preamp[n]=prefix[n];++n;}if(value>=10)preamp[n++]=(char)('0'+value/10u);
        preamp[n++]=(char)('0'+value%10u);preamp[n++]=' ';preamp[n++]='d';preamp[n++]='B';preamp[n]=0;return preamp;}
#if LINNET_EXTENDED_SOUND
    if(index==6||index>=8){
        static const char *const names[]={"Volume limit: ","Bass 100 Hz: ","Low 250 Hz: ","Mid 1 kHz: ","High 4 kHz: ","Treble 8 kHz: "};
        uint32_t slot=index==6?0u:index-7u,band=index>=8?index-8u:0u;
#else
    if(index>=6){
        static const char *const names[]={"Volume limit: ","Bass 100 Hz: ","Treble 8 kHz: "};
        uint32_t slot=index-6u,band=index==8u?4u:0u;
#endif
        uint32_t n=0;char *out=levels[slot];const char *name=names[slot];
        while(*name)out[n++]=*name++;
        int32_t db=slot?profile.eq_db[band]:profile.max_volume_db_tenths/10;
        out[n++]=db<0?'-':'+';uint32_t value=(uint32_t)(db<0?-db:db);
        if(value>=10)out[n++]=(char)('0'+value/10);
        out[n++]=(char)('0'+value%10);out[n++]=' ';out[n++]='d';out[n++]='B';out[n]=0;return out;
    }
#if LINNET_EXTENDED_SOUND
    if(index==7)return profile.lineout?"Dock line out: On":"Dock line out: Off";
#endif
    return "";
}
static int32_t profile_gain(const struct ln_music_profile *p,const struct ln_music_track *t){
    return p->replaygain==2&&t->album_replaygain!=INT32_MIN?t->album_replaygain:t->replaygain;
}
static uint32_t sound_reserve(void){
#if LINNET_TARGET_NUMBER==25u
    /* Video's fixed dock path does not share headphone gain compensation. */
    if(profile.lineout)return 0;
#endif
    int32_t available=profile.max_volume_db_tenths-queue.volume_db_tenths;
    return available<=0?0u:available>=90?90u:(uint32_t)available;
}
static bool configure_sound(uint32_t rate,int32_t replaygain,struct ln_dsp_config *config){
    if(!ln_music_profile_configure(&profile,rate,replaygain,config))return false;
    /* Reserve spare output gain before any EQ edit. At normal volume every
     * curve shares the same digital/codec gain, so changing a band cannot
     * briefly rebalance the whole song. Give flat playback its full range
     * near the volume ceiling, where that spare output gain is unavailable. */
    uint32_t reserve=sound_reserve();
    if(reserve>config->headroom_db_tenths){
        uint32_t gain=ln_dsp_gain_from_db(profile.preamp_db_q16-(int32_t)(reserve*65536u/10u));
        uint32_t rg=profile.replaygain?ln_dsp_gain_from_db(replaygain):LINNET_DSP_UNITY;
        config->gain_q16=(uint32_t)(((uint64_t)gain*rg+32768u)>>16);
        if(!config->gain_q16)config->gain_q16=1;
        config->headroom_db_tenths=reserve;
    }
    return true;
}
/* Settings are user intent. A committed join or a draining stop can temporarily
 * refuse DSP changes; retry after the transition rather than losing the edit.
 * Use the decoder's rate, including playback without a library selection. */
static void apply_sound(void){
    if(!sound_pending||manual_next_pending)return;
    struct ln_player_status player;ln_player_snapshot(&player);
    if(player.phase!=LN_PLAYER_IDLE&&player.phase!=LN_PLAYER_ERROR){
        struct ln_dsp_config config;
        int32_t gain=have_current&&!file_mode?profile_gain(&profile,&current.track):INT32_MIN;
        if(!player.rate||!configure_sound(player.rate,gain,&config)||
           !ln_player_configure(&config,player.rate))return;
    }
    if(!ln_player_cancel_next())return;
    prefetching=next_ready=next_armed=next_failed=false;
    if(resolver_kind==2)resolver_kind=0;
    ln_music_library_cancel_prefetch();
    sound_pending=false;++revision;
}
bool ln_music_sound_adjust(uint32_t row,int32_t direction){
    uint32_t index=ln_music_sound_action(row);
    if(shutting_down||index>=LN_SOUND_CUSTOM_EQ||!direction)return false;
    if(index==LN_SOUND_CLICKER)return ln_display_click_adjust(direction);
    if(index==LN_SOUND_CLICK_VOLUME)return ln_display_click_volume_adjust(direction);
    if(index==LN_SOUND_CLICK_ENABLE)return ln_display_click_enabled_adjust(direction);
    if(index==LN_SOUND_CLICK_OUTPUT)return ln_display_click_output_adjust(direction);
    struct ln_music_profile next=profile;bool up=direction>0;
    if(index==0){
#if LINNET_EXTENDED_SOUND
        next.preset=(next.preset+(up?1u:LN_MUSIC_EQ_CUSTOM))%(LN_MUSIC_EQ_CUSTOM+1u);
#else
        static const uint8_t choices[]={LN_MUSIC_EQ_FLAT,LN_MUSIC_EQ_BASS_BOOSTER,LN_MUSIC_EQ_VOCAL_BOOSTER,LN_MUSIC_EQ_CUSTOM};
        uint32_t at=0;while(at<4u&&choices[at]!=next.preset)++at;if(at==4u)at=0;
        next.preset=choices[(at+(up?1u:3u))%4u];
#endif
    }
    else if(index==1)next.replaygain=(next.replaygain+(up?1u:2u))%3u;
    else if(index==2)next.crossfeed=!next.crossfeed;
    else if(index==3){next.balance+=up?100:-100;if(next.balance>1000)next.balance=1000;if(next.balance< -1000)next.balance=-1000;}
    else if(index==4)next.mode=(next.mode+(up?1u:4u))%5u;
    else if(index==5){next.preamp_db_q16+=up?65536:-65536;if(next.preamp_db_q16>0)next.preamp_db_q16=0;if(next.preamp_db_q16< -24*65536)next.preamp_db_q16=-24*65536;}
    else if(index==6){next.max_volume_db_tenths+=up?10:-10;
        if(next.max_volume_db_tenths>0)next.max_volume_db_tenths=0;
        if(next.max_volume_db_tenths< -730)next.max_volume_db_tenths=-730;
    }
#if LINNET_EXTENDED_SOUND
    else if(index==7){next.lineout=!next.lineout;}
    else {uint32_t band=index-8u;int32_t db=next.eq_db[band]+(up?1:-1);if(db>6)db=6;if(db< -6)db=-6;
        next.eq_db[band]=(int8_t)db;next.preset=LN_MUSIC_EQ_CUSTOM;}
#else
    else {uint32_t band=index==8u?4u:0u;int32_t db=next.eq_db[band]+(up?1:-1);if(db>6)db=6;if(db< -6)db=-6;
        next.eq_db[band]=(int8_t)db;next.preset=LN_MUSIC_EQ_CUSTOM;}
#endif
    if(!ln_music_profile_valid(&next))return false;
    uint32_t field=index;
#if !LINNET_EXTENDED_SOUND
    if(index>=7u)field=index==7u?8u:12u;
#endif
    uint32_t edits=(1u<<field)|(field>=8u?1u:0u);
    if(index==6){profile=next;sound_edits|=edits;sound_pending=true;ln_music_volume(0);return true;}
#if LINNET_EXTENDED_SOUND
    if(index==7){
        if(!ln_audio_lineout(next.lineout))return false;
        profile=next;sound_edits|=edits;++revision;
#if LINNET_TARGET_NUMBER==25u
        sound_pending=true;apply_sound();
#endif
        return true;
    }
#endif
    profile=next;sound_edits|=edits;sound_pending=true;++revision;apply_sound();return true;
}
uint32_t ln_music_deadline(uint32_t now){
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && defined(LINNET_VIDEO)
    /* Match the executive's suspended music service during video playback.
     * Its old metadata deadlines cannot advance; volume sync still runs. */
    if(ln_video_active()||ln_video_playback_pending()){
        uint32_t next=now+200000u;
        if(volume_retry_pending&&(int32_t)(volume_retry_at-next)<0)next=volume_retry_at;
        return next;
    }
#endif
    /* Library and history deadlines own admitted metadata reads. A browser
     * waiting for one must not keep the executive on its immediate path. */
    bool browser_query_idle=ln_music_browser_query_idle();
    if((ln_music_browser_busy()&&!ln_music_browser_waiting())||
       (ln_music_search_busy()&&!ln_music_search_query_pending()&&browser_query_idle)||
       (ln_music_queue_view_busy()&&!ln_music_queue_view_query_pending()&&
        queue_opened&&!ln_music_search_active()&&browser_query_idle))return now;
    struct ln_player_status player;ln_player_snapshot(&player);
    if(have_waiting&&player.phase==LN_PLAYER_IDLE&&!ln_storage_busy()&&
       (ln_music_history_error()||(ln_music_history_ready()&&
        (ln_music_history_record_ready(waiting.track.id)||
         ln_music_history_record_error(waiting.track.id)))))return now;
    uint32_t next=ln_player_deadline(now),library=ln_music_library_deadline(now);
    if((int32_t)(library-next)<0)next=library;
    uint32_t art=ln_music_art_deadline(now);
    if((int32_t)(art-next)<0)next=art;
#if defined(LINNET_COVERFLOW)
    uint32_t covers=ln_coverflow_io_deadline();if(covers!=UINT32_MAX&&(int32_t)(covers-next)<0)next=covers;
#endif
    uint32_t history=ln_music_history_deadline(now);
    if((int32_t)(history-next)<0)next=history;
    if(volume_retry_pending&&(int32_t)(volume_retry_at-next)<0)next=volume_retry_at;
    return next;
}
/* Menu rows map to stable service actions, shared by labels and selection. */
uint32_t ln_music_option_action(uint32_t row){
    return ln_music_menu_option_action(file_mode,row);
}
uint32_t ln_music_option_count(void){return ln_music_menu_option_count(file_mode);}
uint32_t ln_music_queue_action(uint32_t row){
    /* Play, move up/down, remove, clear. */
    static const uint8_t actions[]={0,2,3,1,4};
    return row<sizeof(actions)?actions[row]:UINT32_MAX;
}
uint32_t ln_music_playback_modes(void){return (queue.shuffled?1u:0u)|(queue.repeat<<1);}
const char *ln_music_option_label(uint32_t index){
    const struct ln_music_journal_record *record=have_current&&index>=2u&&index<=4u?ln_music_history_get(current.track.id):0;
    if(index==2||index==3){
        if(ln_music_history_error())return "History unavailable";
        if(!ln_music_history_ready())return "History loading";
        if(have_current&&!ln_music_history_record_ready(current.track.id))return "History loading";
        if(have_current&&ln_music_history_record_error(current.track.id))
            return "History full";
    }
    switch(index){
    case 0:return queue.shuffled?"Shuffle: Songs":"Shuffle: Off";
    case 1:return queue.repeat==LINNET_QUEUE_REPEAT_ONE?"Repeat: One":queue.repeat==LINNET_QUEUE_REPEAT_ALL?"Repeat: All":"Repeat: Off";
    case 2:return !ln_music_history_ready()?"Favorite: Unavailable":record&&(record->flags&LN_MUSIC_JOURNAL_FAVORITE)?"Favorite: Yes":"Favorite: No";
    case 3:return record&&(record->flags&LN_MUSIC_JOURNAL_BOOKMARK)?"Update bookmark":"Save bookmark";
    case 4:return record&&(record->flags&LN_MUSIC_JOURNAL_BOOKMARK)?"Resume bookmark":"No bookmark saved";
    case 5:return "Restart track";
    case 6:return "Stop";
    case 12:return "Sleep Timer";
    case 11:return "Save queue as playlist...";
    case 7:return "Play Queue";
    case 8:return "Sound";
    case 9:return "Back 10 seconds";
    case 10:return "Forward 10 seconds";
    default:return "";
    }
}
bool ln_music_option_select(uint32_t index){
    if(shutting_down||index>10)return false;
    if(file_mode&&index==5){struct ln_player_status p;ln_player_snapshot(&p);
        if(p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR){ln_player_stop();file_waiting=true;return true;}
        return ln_music_seek(0);}
    if(index==7||index==8)return false; /* Runtime opens their native scenes. */
    if(index==9||index==10){
        struct ln_player_status p;ln_player_snapshot(&p);uint64_t position=ln_player_position(),delta=(uint64_t)p.rate*10u;
        uint64_t target=index==9?(position>delta?position-delta:0):position+delta;
        uint64_t total=p.frames?p.frames:have_current?current.track.frames:0;
        if(total&&target>=total)target=total?total-1:0;
        return p.rate&&ln_music_seek(target);
    }
    if(index==6){ln_music_stop();return true;}
    if(index<2){
        if(ln_music_loading())return false;
        if(!ln_player_cancel_next()&&!ln_player_seek(ln_player_position()))return false;
        prefetching=next_ready=next_armed=next_failed=false;
        if(resolver_kind==2)resolver_kind=0;
        ln_music_library_cancel_prefetch();
        if(index==0)ln_queue_shuffle(&queue,!queue.shuffled);
        else (void)ln_queue_set_repeat(&queue,(queue.repeat+1u)%3u);
        current_cursor=queue.cursor;
        ++revision;return true;
    }
    if(!have_current||selecting||have_waiting)return false;
    const struct ln_music_journal_record *record=ln_music_history_get(current.track.id);
    bool accepted=false;
    if(index==2)accepted=ln_music_history_set_favorite(current.track.id,!(record&&(record->flags&LN_MUSIC_JOURNAL_FAVORITE)));
    else if(index==3)accepted=ln_music_history_set_bookmark(current.track.id,true,ln_player_position());
    else{
        if(index==4&&(!record||!(record->flags&LN_MUSIC_JOURNAL_BOOKMARK)))return false;
        uint64_t position=index==4?record->bookmark_frame:0;
        struct ln_player_status p;ln_player_snapshot(&p);
        if(p.phase==LN_PLAYER_IDLE){requested_position=position;active=true;accepted=select_index(current.index);}
        else accepted=ln_player_seek(position);
        if(accepted){prefetching=next_ready=next_armed=next_failed=false;queue.position_samples=position;}
    }
    if(accepted)++revision;return accepted;
}
static bool select_queue(const struct ln_music_browser_queue *selection){
    uint8_t identity[32];uint32_t count=ln_music_library_count();
    if(shutting_down||!selection->count||selection->count>LINNET_QUEUE_SOURCE_MAX||
       selection->cursor>=selection->count||!ln_music_library_identity(identity)||
       !same(identity,selection->library_hash))return false;
    struct ln_queue *replacement=&queue_candidate;ln_queue_init(replacement,ln_time_us());
    replacement->volume_db_tenths=queue.volume_db_tenths;replacement->repeat=queue.repeat;
    for(uint32_t i=0;i<32;++i)replacement->library_identity[i]=identity[i];
    if(selection->source){
        if((selection->source==LN_QUEUE_ALL&&selection->count!=count)||
           (selection->source==LN_QUEUE_GROUP&&(selection->source_group&~LN_QUEUE_REVERSE)>=ln_music_library_group_count())||
           !ln_queue_set_source(replacement,selection->source,selection->source_group,selection->count,selection->cursor))return false;
    }else{
        if(selection->count>LINNET_QUEUE_CAPACITY)return false;
        for(uint32_t i=0;i<selection->count;++i)
            if(selection->tracks[i]>=count||!ln_queue_add(replacement,selection->tracks[i]))return false;
        if(!ln_queue_select_occurrence(replacement,selection->cursor))return false;
    }
    if(selection->shuffle_all){
        uint32_t random=replacement->seed;random^=random<<13;random^=random>>17;random^=random<<5;
        replacement->seed=random;
        (void)ln_queue_select_at(replacement,(uint32_t)(((uint64_t)random*replacement->count)>>32));
        if(replacement->repeat==LINNET_QUEUE_REPEAT_ONE)replacement->repeat=LINNET_QUEUE_REPEAT_NONE;
    }
    ln_queue_shuffle(replacement,selection->shuffle_all||queue.shuffled);
    sleep_cancel_boundary();remember_position();queue=*replacement;++revision;skipped=0;active=true;requested_position=selection->frame;
    return select_current();
}
bool ln_music_select(uint32_t index){
    static struct ln_music_browser_queue selection;selection=(struct ln_music_browser_queue){0};selection.count=1;selection.tracks[0]=index;
    if(!ln_music_library_identity(selection.library_hash))return false;
    return select_queue(&selection);
}
bool ln_music_quiz_begin(uint32_t index){
    if(quiz_active||shutting_down||ln_music_loading()||index>=ln_music_library_count())return false;
    struct ln_player_status player;ln_player_snapshot(&player);remember_position();
    quiz_saved_queue=queue;quiz_saved_position=have_current?ln_player_position():queue.position_samples;
    quiz_saved_track_id=have_current?current.track.id:0;
    quiz_saved_active=active;quiz_saved_paused=player.phase==LN_PLAYER_PAUSE;
    quiz_active=true;if(ln_music_select(index)){queue.repeat=LINNET_QUEUE_REPEAT_NONE;queue.shuffled=false;return true;}
    quiz_active=false;queue=quiz_saved_queue;active=quiz_saved_active;requested_position=quiz_saved_position;return false;
}
bool ln_music_quiz_select(uint32_t index){
    if(!quiz_active||ln_music_loading()||index>=ln_music_library_count()||!ln_music_select(index))return false;
    queue.repeat=LINNET_QUEUE_REPEAT_NONE;queue.shuffled=false;return true;
}
void ln_music_quiz_end(void){
    if(!quiz_active)return;ln_music_stop();quiz_active=false;queue=quiz_saved_queue;
    queue.position_samples=quiz_saved_position;active=quiz_saved_active;requested_position=quiz_saved_position;
    have_current=false;quiz_restore_track_id=quiz_saved_track_id;quiz_restore_pause=quiz_saved_active&&quiz_saved_paused;
    if(active){if(!select_current()){active=false;quiz_restore_pause=false;}}
    else quiz_restore_pause=false;++revision;
}
bool ln_music_next(bool previous){
    if(shutting_down)return false;
    if(file_mode){
        struct ln_player_status player;ln_player_snapshot(&player);
        if(previous&&player.rate&&ln_player_position()>=(uint64_t)player.rate*3u)return ln_music_seek(0);
#if defined(LINNET_FILES)
        if(ln_files_audio_next(previous)){notice=0;++revision;return true;}
#endif
        return false;
    }
    if(previous&&have_current){
        uint32_t rate=current.track.rate;
        if(rate&&ln_player_position()>=(uint64_t)rate*3u)return ln_music_seek(0);
    }
    remember_position();
    bool moved=previous?ln_queue_previous(&queue):ln_queue_next(&queue,true);
    uint32_t index;if(!moved||!ln_queue_current(&queue,&index))return false;
    sleep_cancel_boundary();++revision;active=true;skipped=0;requested_position=0;
    if(!previous&&next_armed&&next_ordinal==index&&ln_player_skip_prepared_next(next_token)){
        manual_next_pending=true;return true;}
    bool accepted=select_current();
    if(accepted){
        /* An explicit skip no longer needs the old song's PCM runway. Stop
         * its producer promptly; the player still drains its admitted read
         * before replacement metadata/audio can take storage ownership. */
        struct ln_player_status player;ln_player_snapshot(&player);
        if(player.phase!=LN_PLAYER_IDLE&&player.phase!=LN_PLAYER_STOPPING)
            ln_player_stop_for_restart();
    }
    return accepted;
}
void ln_music_file_transport_finished(bool found){
    if(!file_mode)return;
    notice=found?0:"No more audio in folder";++revision;
}
void ln_music_toggle(void){
    if(shutting_down)return;
    struct ln_player_status p;ln_player_snapshot(&p);
    if(ln_player_toggle_pause())return;
    if(file_mode){if(p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR){ln_player_stop();file_waiting=true;}return;}
    if(p.phase==LN_PLAYER_IDLE&&!selecting&&!have_waiting&&!resolver_kind){
        if(queue.count){requested_position=queue.position_samples;active=true;(void)select_current();}}
}
static void remember_position(void){
    uint32_t index;struct ln_player_status p;ln_player_snapshot(&p);
    if(have_current&&!ln_music_loading()&&ln_queue_current(&queue,&index)&&current_cursor==queue.cursor&&p.generation&&
       p.phase!=LN_PLAYER_IDLE&&p.phase!=LN_PLAYER_ERROR&&p.phase!=LN_PLAYER_STOPPING){
        uint64_t position=ln_player_position();
        if(!quiz_active)(void)ln_music_history_set_resume(current.track.id,true,position);
        if(position!=queue.position_samples){queue.position_samples=position;++revision;}}
}
void ln_music_stop(void){if(shutting_down)return;
#if defined(LINNET_FILES)
    ln_files_audio_cancel();
#endif
    file_waiting=false;sleep_cancel_boundary();remember_position();active=false;have_waiting=false;selecting=false;resolver_kind=0;volume_retry_pending=false;volume_retry_at=0;discard_next(false);ln_player_stop();++revision;}
bool ln_music_open_file(const struct ln_fat32_file*file,const char*title){
    if(shutting_down||quiz_active||!file||!file->size||(file->attributes&24u)||!title)return false;
    ln_music_stop();direct_file=*file;have_current=false;file_mode=file_waiting=true;
    current=(struct ln_music_library_row){0};current.index=UINT32_MAX;
    uint32_t n=0;while(title[n]&&n+1u<sizeof current.title){current.title[n]=title[n];++n;}
    ln_music_art_request("",0);notice=0;++revision;return true;
}
static bool configure_file(uint32_t rate,struct ln_dsp_config*dsp){return configure_sound(rate,0,dsp);}
bool ln_music_seek(uint64_t frame){
    if(shutting_down||!ln_player_seek(frame))return false;
#if defined(LINNET_FILES)
    ln_files_audio_cancel();
#endif
    if(file_mode){++revision;return true;}
    prefetching=next_ready=next_armed=next_failed=false;
    if(resolver_kind==2)resolver_kind=0;
    queue.position_samples=frame;++revision;return true;
}
void ln_music_headphones(bool connected){
    if(shutting_down)return;
    if(headphone_known&&headphones&&!connected)(void)ln_player_pause(true);
    headphones=connected;headphone_known=true;
}
static bool persist(void){
    if(!restored||revision==saved_revision)return true;
    if(persist_failed||ln_storage_busy())return false;
    if(!ln_storage_prepared()){
        if(ln_storage_prepare_writes()<0)persist_failed=true;
        return false;
    }
    if(!queue.count)queue.last_track_id=0;
    uint32_t snapshot_revision=revision,length=ln_music_settings_encode(&queue,&profile,resume,sizeof resume);
    if(!length)return false;
    if(!ln_collections_resume_set(resume,length)){
        if(length>444u||!ln_display_settings_music(true,resume,&length)){persist_failed=true;return false;}
    }
    if(revision==snapshot_revision)saved_revision=snapshot_revision;return true;
}
bool ln_music_prepare_shutdown(void){return ln_music_quiesce(true);}
bool ln_music_quiesce(bool save_settings){
    if(shutting_down||stepping||ln_storage_busy())return false;
    shutting_down=true;
#if defined(LINNET_COVERFLOW)
    ln_coverflow_visible(false);
#endif
    remember_position();ln_music_history_freeze();
    active=have_waiting=selecting=false;resolver_kind=0;
    ln_player_stop();ln_music_library_cancel();ln_music_art_request(0,0);
    uint32_t started=ln_time_us();bool drained=false;
    for(uint32_t budget=0;budget<1000000u;++budget){
        ln_player_step();ln_music_library_step();ln_music_art_step(false);ln_collections_step(false,true);
#if defined(LINNET_COVERFLOW)
        ln_coverflow_step(false);
#endif
        struct ln_player_status p;struct ln_audio_status a;
        ln_player_snapshot(&p);ln_audio_snapshot(&a);
        if(p.phase==LN_PLAYER_ERROR||a.fault)return false;
        if(p.phase==LN_PLAYER_IDLE&&!a.playing&&!ln_storage_busy()&&
           ln_music_library_status()==LN_LIBRARY_CLOSED){drained=true;break;}
        if(ln_time_us()-started>=30000000u)return false;
    }
    if(!drained)return false;
    if(!save_settings)return true;
    if((restored&&revision!=saved_revision)||ln_music_history_dirty()){
        int writable;
        while((writable=ln_storage_prepare_writes())==0){
            ln_storage_step();if(ln_time_us()-started>=30000000u)return false;
        }
        if(writable<0)return false;
    }
    if(!persist())return false;
    while(ln_collections_dirty()||ln_collections_busy()){
        ln_collections_step(true,true);ln_storage_step();if(ln_time_us()-started>=30000000u)return false;
    }
    /* No accepted history events exist before ready; shutdown must not start
     * a new load after closing the library. Crash recovery never calls here. */
    if(!ln_music_history_ready()){
        /* Missing or unusable banks cannot accept events before ready.
         * Neither has pending history to flush, and both must permit Sync
         * so the host can prepare the optional history storage.
         * Errors after a successful load still take the flush path below. */
        int error=ln_music_history_error();
        return error==LN_HISTORY_OK||error==LN_HISTORY_MISSING||error==LN_HISTORY_CAPACITY;
    }
    for(;;){
        int result=ln_music_history_flush();if(result)return result==1;
        if(ln_time_us()-started>=30000000u)return false;
        ln_music_history_step(true);
        ln_storage_step();
    }
}
bool ln_music_volume(int32_t steps){
    if(shutting_down)return false;
    uint32_t old_reserve=sound_reserve();
    int64_t next=(int64_t)queue.volume_db_tenths+(int64_t)steps*10;
    if(next<-730)next=-730;if(next>profile.max_volume_db_tenths)next=profile.max_volume_db_tenths;
    bool changed=queue.volume_db_tenths!=(int32_t)next;
    /* Zero-step calls also publish a changed sound profile and sync gain. */
    queue.volume_db_tenths=(int32_t)next;if(changed||!steps)++revision;
    if(sound_reserve()!=old_reserve)sound_pending=true;
    apply_sound();
    (void)ln_music_volume_sync();
    return changed;
}
/* Shared output gain also runs while video owns the audio sink. */
bool ln_music_volume_sync(void){
    if(shutting_down)return false;
    if(queue.volume_db_tenths>profile.max_volume_db_tenths){queue.volume_db_tenths=profile.max_volume_db_tenths;++revision;}
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    int32_t target=output_volume();
    if(!audio.initialized){volume_retry_pending=false;volume_retry_at=0;return true;}
    else if(audio.volume_db_tenths!=target){
        uint32_t retry_now=ln_time_us();
        if(!volume_retry_pending||(int32_t)(retry_now-volume_retry_at)>=0){
            if(ln_audio_volume(target)){
                volume_retry_pending=false;volume_retry_at=0;++revision;return true;
            }else{
                volume_retry_pending=true;volume_retry_at=ln_time_us()+LN_MUSIC_VOLUME_RETRY_US;
            }
        }
        return false;
    }else{volume_retry_pending=false;volume_retry_at=0;return true;}
}
bool ln_music_save_queue(void){return ln_music_browser_save_queue(&queue);}
static void append_queue(struct ln_queue *target,const struct ln_music_browser_queue *selection){
    uint32_t at=selection->action==1&&target->count?target->cursor+1:target->count;
    for(uint32_t i=0;i<selection->count;i++){
        uint32_t occurrence=target->count;ln_queue_add(target,selection->tracks[i]);
        for(uint32_t j=target->count-1;j>at+i;j--)target->order[j]=target->order[j-1];target->order[at+i]=occurrence;
    }
    /* Preserve the requested playback order when shuffle is off too. */
    static uint32_t order[LINNET_QUEUE_CAPACITY];for(uint32_t i=0;i<target->count;i++)order[i]=target->tracks[target->order[i]];
    for(uint32_t i=0;i<target->count;i++){target->tracks[i]=order[i];target->order[i]=i;}
    queue=*target;discard_next(true);current_cursor=queue.cursor;++revision;notice=0;
    ln_music_browser_notice(selection->action==1?"Will play next":"Added to queue");
}
static void accept_browser_queue(const struct ln_music_browser_queue *selection){
    if(!selection->action){if(select_queue(selection))show_now_playing=true;return;}
    if(selection->action==3){
        ln_queue_init(&expanded_queue,ln_time_us());expanded_queue.count=selection->count;
        expanded_queue.source=selection->source;expanded_queue.source_group=selection->source_group;
        expanded_queue.source_count=selection->count;expanded_queue.stride=expanded_queue.inverse=1;
        for(uint32_t i=0;!selection->source&&i<selection->count;i++){expanded_queue.tracks[i]=selection->tracks[i];expanded_queue.order[i]=i;}
        (void)ln_music_browser_save_queue(&expanded_queue);return;
    }
    if(!ln_collections_writable()&&queue.count+selection->count>64){ln_music_browser_notice("Sync to enable larger queues");return;}
    if(adding||selecting||have_waiting||manual_next_pending||queue.count+selection->count>LINNET_QUEUE_CAPACITY){ln_music_browser_notice("Queue busy or over 1024 tracks");return;}
    if(!ln_player_cancel_next()){ln_music_browser_notice("Track changing; try again");return;}
    discard_next(true);additions=*selection;additions_revision=revision;
    if(!queue.count)(void)ln_music_library_identity(queue.library_identity);
    if(queue.source){adding=ln_music_browser_expand(&queue);if(!adding)ln_music_browser_notice("Queue edit unavailable");}
    else{expanded_queue=queue;append_queue(&expanded_queue,selection);}
}
static void music_step(void){
    struct ln_player_status before,p;struct ln_audio_status audio;
    bool expected=false;
    if(active&&have_current&&!selecting&&!have_waiting&&!next_failed){
        if(next_armed||next_ready||prefetching||resolver_kind==2)expected=true;
        else {uint32_t cursor;expected=next_queue_cursor(&cursor);}
    }
    bool allow_next=expected&&!sleep_holds_next()&&!sleep_due;
    ln_player_expect_next(allow_next);
    ln_player_snapshot(&before);ln_player_step();ln_player_snapshot(&p);ln_audio_snapshot(&audio);
    /* A muted seek can discard thousands of MPEG frames without producing
     * PCM. Advance cached decoder work in this turn, bounded so input and
     * display service still run promptly. A pending disk request resumes in
     * the normal storage turn instead of spinning here. */
    if(p.phase==LN_PLAYER_PRIME&&!audio.playing){
        uint32_t started=ln_time_us();
        for(uint32_t n=0;n<32u&&!ln_storage_busy()&&
            (uint32_t)(ln_time_us()-started)<800u;++n){
            ln_player_step();ln_player_snapshot(&p);
            if(p.phase!=LN_PLAYER_PRIME)break;
        }
        ln_audio_snapshot(&audio);
    }
    if(file_mode){
        current.track.rate=p.rate;current.track.frames=p.frames;
        if(sleep_active&&sleep_mode>=9u&&before.phase!=LN_PLAYER_IDLE&&before.phase!=LN_PLAYER_ERROR&&
           (p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR)){sleep_due=true;++sleep_revision;}
        if(file_waiting&&p.phase==LN_PLAYER_IDLE&&!ln_storage_busy()&&ln_player_open_file(&direct_file,configure_file)){file_waiting=false;++revision;}
    }
    /* Restoring a paused pre-quiz song must wait until its decoder has reached
     * PLAY. The player deliberately rejects pause while the file is OPEN. */
    if(quiz_restore_pause&&!quiz_active){
        if(p.phase==LN_PLAYER_PLAY){
            if(!have_current||current.track.id!=quiz_restore_track_id)quiz_restore_pause=false;
            else if(ln_player_pause(true))quiz_restore_pause=false;
        }else if(p.phase==LN_PLAYER_ERROR||(!active&&!selecting&&!have_waiting&&resolver_kind!=1))quiz_restore_pause=false;
    }
#if defined(LINNET_COVERFLOW)
    for(uint32_t budget=0;budget<3&&ln_coverflow_active()&&audio.playing&&!audio.paused&&
        audio.queued<LINNET_AUDIO_RING_FRAMES/2u;++budget){
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
        ln_runtime_power_checkpoint();
#endif
        ln_player_step();ln_player_snapshot(&p);ln_audio_snapshot(&audio);
    }
#endif
    struct ln_player_transition transition;
    if(ln_player_take_transition(&transition)&&next_armed){
#if !defined(LINNET_PROFILE_RELEASE)
        budget_count(&budget_transitions);
#endif
        uint32_t index;
        if(transition.token!=next_token||(!transition.manual&&!ln_queue_next(&queue,false))||
           !ln_queue_current(&queue,&index)||index!=next_ordinal){ln_music_stop();return;}
        if(!quiz_active&&have_current&&!transition.manual){(void)ln_music_history_played(current.track.id);
            (void)ln_music_history_set_resume(current.track.id,false,0);}
        current=upcoming;current_cursor=queue.cursor;have_current=true;next_armed=next_ready=prefetching=next_failed=false;
        queue.last_track_id=current.track.id;
        ln_music_history_pin_current(true,current.track.id);
        if(!quiz_active)ln_collections_played(current.track.id);
        manual_next_pending=false;
        queue.position_samples=0;++revision;skipped=0;
        ln_music_art_request(current.art_path,current.art_crc);
    }
    /* A failed prepared promotion has no transition event. Retry the requested
     * queue item through ordinary selection, without advancing it again. */
    if(manual_next_pending&&(p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR)){
        manual_next_pending=next_armed=next_ready=prefetching=next_failed=false;
        requested_position=0;active=select_current();
        if(!active)ln_player_stop();
        ++revision;
    }
    apply_sound();
    bool urgent=ln_player_prefetch_urgent();
    uint32_t read_runway=ln_player_background_frames(audio.rate);
#if defined(LINNET_PHOTOS) || defined(LINNET_SOC_PP5022)
    uint64_t track_frames=p.frames?p.frames:current.track.frames;
    uint64_t position=ln_player_position();
#endif
#if defined(LINNET_SOC_PP5022)
    uint64_t wake_window=track_frames>>2,maximum=(uint64_t)audio.rate*30u;
    if(wake_window>maximum)wake_window=maximum;
    ln_storage_music_boundary((active&&have_current&&allow_next&&p.phase==LN_PLAYER_PLAY&&audio.playing&&
        track_frames&&(position>=track_frames||track_frames-position<=wake_window)?LN_STORAGE_MUSIC_BOUNDARY:0u)
#if LINNET_TARGET_NUMBER==25u
        |(ln_player_startup_disk()?LN_STORAGE_MUSIC_PREPARE:0u)
#endif
        );
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    if(urgent)budget_count(&budget_urgent_windows);
    if(audio.playing&&audio.queued<read_runway)budget_count(&budget_low_pcm_windows);
    if(audio.playing&&audio.queued<budget_min_queued)budget_min_queued=audio.queued;
#endif
    bool photo_priority=false;
#if defined(LINNET_PHOTOS)
    /* Release speculative-read priority before the audible tail, not only
     * after decode reaches EOF. Otherwise a photo/zoom can leave too little
     * runway to resolve the following short track's metadata. */
    bool transition_soon=track_frames&&(position>=track_frames||
        track_frames-position<=(uint64_t)audio.rate*3u);
    photo_priority=active&&have_current&&!selecting&&!have_waiting&&!manual_next_pending&&
        p.phase==LN_PLAYER_PLAY&&!urgent&&!transition_soon&&ln_photos_busy();
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    if(photo_priority)budget_count(&budget_photo_windows);
#endif
    /* Visible artwork gets the first background window after audio decoding.
     * Keep startup and urgent gapless reads ahead of new cover I/O. */
    bool art_starting=selecting||have_waiting||resolver_kind==1||
        p.phase==LN_PLAYER_OPEN||p.phase==LN_PLAYER_HEADER||p.phase==LN_PLAYER_PRIME;
    ln_music_art_step(!art_starting&&!urgent&&
        (!audio.initialized||!audio.playing||audio.paused||audio.queued>=read_runway));
    /* Metadata is enough to preload the next cover; waiting for its audio
     * decoder also waits for the player's larger speculative-read reserve. */
    if(!art_starting&&!urgent&&!photo_priority&&(next_ready||next_armed)&&p.phase==LN_PLAYER_PLAY&&
       audio.playing&&!audio.paused&&audio.queued>=read_runway)
        ln_music_art_prefetch(upcoming.art_path,upcoming.art_crc);
    /* Both services use the same PCM reserve. Let unfinished visible photos
     * claim that window before speculative metadata consumes it repeatedly.
     * Library/history service calls still drain their admitted operations. */
    ln_music_library_pause(audio.initialized&&audio.playing&&!urgent&&
        (audio.queued<read_runway||photo_priority));
    ln_music_browser_suspend(!adding&&(queue_opened||ln_music_queue_view_busy()||ln_music_search_active()));
    ln_music_library_window_enable(ln_music_browser_window_needed());
    ln_music_library_step();
    uint32_t resolved,token;
    if(ln_music_library_take_queue_member(&resolved,&token)&&resolver_kind&&token==resolver_token){
        uint32_t kind=resolver_kind;resolver_kind=0;
        if(resolved>=ln_music_library_count()){
            if(kind==1){active=false;ln_player_stop();}else next_failed=true;
        }else if(kind==1){
            uint32_t ordinal;if(ln_queue_current(&queue,&ordinal)&&ordinal==resolver_ordinal)(void)select_index(resolved);
        }else {next_index=resolved;prefetching=ln_music_library_prefetch(resolved);}
    }
    bool collections_write=!audio.playing||audio.queued>=LINNET_AUDIO_RING_FRAMES-2048u;
#if defined(LINNET_TARGET_VIDEO5G_M25)
    /* Admitted tickets still drain while foreground preparation defers new work. */
    struct ln_player_status collections_player;
    ln_player_snapshot(&collections_player);
    collections_write=collections_write&&!ln_music_loading()&&
        collections_player.phase!=LN_PLAYER_OPEN&&collections_player.phase!=LN_PLAYER_HEADER&&
        collections_player.phase!=LN_PLAYER_PRIME;
#endif
    ln_collections_step(!audio.playing||audio.queued>=read_runway,collections_write);
    if(!quiz_active&&have_current&&!selecting&&!have_waiting&&ln_collections_writable()&&(!recent_recorded||recent_id!=current.track.id)){ln_collections_played(current.track.id);recent_id=current.track.id;recent_recorded=true;}
    ln_music_browser_step();
    ln_music_search_step();
    if(queue_opened)ln_music_queue_view_update(&queue,revision);
    ln_music_queue_view_step(queue_opened&&!ln_music_search_active()&&ln_music_browser_query_idle());
    /* Fill cached labels before rendering, with a strict time/iteration budget.
     * Hardware waits and PCM pressure always return control to the executive. */
    uint32_t metadata_started=ln_time_us();
    for(uint32_t n=0;n<48u&&ln_music_library_can_continue();n++){
#if LINNET_TARGET_NUMBER==25u
        /* A history wait cannot advance inside this library-only pump. Keep
         * every runnable library intent, including playback prefetch, eligible. */
        if(ln_music_browser_query_idle()&&ln_music_browser_waiting()&&
           !ln_music_search_busy()&&!ln_music_queue_view_busy()){
            uint32_t now=ln_time_us();
            if(ln_music_library_deadline(now)!=now)break;
        }
#endif
        if((uint32_t)(ln_time_us()-metadata_started)>=1000u){
#if !defined(LINNET_PROFILE_RELEASE)
            budget_count(&budget_metadata_hits);
#endif
            break;
        }
        if(!ln_music_browser_busy()&&!ln_music_search_busy()&&!ln_music_queue_view_busy())break;
#if !defined(LINNET_PROFILE_RELEASE)
        budget_count(&budget_metadata_iterations);
#endif
        ln_music_library_step();
        ln_music_browser_step();
        ln_music_search_step();
        ln_music_queue_view_step(queue_opened&&!ln_music_search_active()&&ln_music_browser_query_idle());
    }
    ln_music_browser_publish();
    static struct ln_music_browser_queue browser_queue;
    if(ln_music_search_take_queue(&browser_queue)&&select_queue(&browser_queue)){
        ln_music_search_close();show_now_playing=true;
    }
    if(ln_music_browser_take_queue(&browser_queue))accept_browser_queue(&browser_queue);
    if(adding&&ln_music_browser_take_expanded(&expanded_queue)){
        adding=false;
        if(revision!=additions_revision||!ln_player_cancel_next())ln_music_browser_notice("Queue changed; try again");
        else append_queue(&expanded_queue,&additions);
    }
#if defined(LINNET_COVERFLOW)
    bool starting=selecting||have_waiting||resolver_kind==1||p.phase==LN_PLAYER_OPEN||
        p.phase==LN_PLAYER_HEADER||p.phase==LN_PLAYER_PRIME;
    bool cover_read=!starting&&!urgent&&(!audio.initialized||!audio.playing||audio.paused||
        audio.queued>=LINNET_AUDIO_RING_FRAMES/2u);
#if defined(LINNET_PHOTOS)
    cover_read=cover_read&&!ln_photos_busy();
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && defined(LINNET_VIDEO)
    /* Drain admitted artwork I/O, then let the queued foreground video
     * acquire storage before admitting another optional artwork chunk. */
    cover_read=cover_read&&!ln_runtime_video_requested();
#endif
    ln_coverflow_step(cover_read);
#endif
    if(!restored&&ln_collections_ready()&&
       (ln_music_library_status()==LN_LIBRARY_READY||ln_music_library_status()==LN_LIBRARY_ERROR)&&!ln_storage_busy()&&
       (!restore_attempts||(int32_t)(ln_time_us()-restore_retry_at)>=0)){
        /* Queue edits and early playback supersede queue restoration, but do
         * not discard independently saved sound settings. */
        bool restore_queue=revision==restore_revision&&!active&&!selecting&&!have_waiting&&resolver_kind!=1&&!file_mode;
        uint8_t identity[32];uint32_t length=sizeof resume,snapshot_revision=revision;
        bool have_identity=ln_music_library_identity(identity);
        uint32_t stored=ln_collections_resume_get(resume,sizeof resume);
        int result=stored?1:ln_display_settings_music_restore(resume,&length);
        if(stored)length=stored;
        if(result==0&&revision==snapshot_revision){
            if(++restore_attempts<3u)restore_retry_at=ln_time_us()+1000000u;
            else {
                /* Do not publish fallback defaults over an unread queue.
                 * Subsequent user changes can still be saved normally. */
                restored=true;saved_revision=revision;
            }
        }else {
            if(result==1){
                struct ln_queue *saved=&queue_candidate;struct ln_music_profile saved_profile;
                if(ln_music_settings_decode(saved,&saved_profile,resume,length)&&revision==snapshot_revision){
                    /* Keep each explicit edit, while restoring untouched
                     * fields even if playback or another setting changed. */
                    if(sound_edits&1u)saved_profile.preset=profile.preset;
                    if(sound_edits&2u)saved_profile.replaygain=profile.replaygain;
                    if(sound_edits&4u)saved_profile.crossfeed=profile.crossfeed;
                    if(sound_edits&8u)saved_profile.balance=profile.balance;
                    if(sound_edits&16u)saved_profile.mode=profile.mode;
                    if(sound_edits&32u)saved_profile.preamp_db_q16=profile.preamp_db_q16;
                    if(sound_edits&64u)saved_profile.max_volume_db_tenths=profile.max_volume_db_tenths;
                    if(sound_edits&128u)saved_profile.lineout=profile.lineout;
                    for(uint32_t i=0;i<5u;++i)if(sound_edits&(1u<<(8u+i)))saved_profile.eq_db[i]=profile.eq_db[i];
                    profile=saved_profile;(void)ln_audio_lineout(profile.lineout);
                    sound_pending=true;++revision;
                    if(restore_queue&&have_identity){
                        bool valid=true;
                        if(!same(saved->library_identity,identity))valid=false;
                        if(saved->source==LN_QUEUE_ALL)valid=valid&&saved->source_count==ln_music_library_count();
                        else if(saved->source==LN_QUEUE_GROUP)valid=valid&&(saved->source_group&~LN_QUEUE_REVERSE)<ln_music_library_group_count();
                        else for(uint32_t i=0;i<saved->count;++i)if(saved->tracks[i]>=ln_music_library_count())valid=false;
                        if(valid){queue=*saved;(void)select_current();}
                        else {
                            queue.volume_db_tenths=saved->volume_db_tenths;queue.repeat=saved->repeat;queue.shuffled=saved->shuffled;
                            (void)ln_music_library_identity(queue.library_identity);
                            if(saved->last_track_id&&ln_music_library_select_id(saved->last_track_id)){
                                queue.position_samples=saved->position_samples;queue.shuffled=saved->shuffled;
                                selecting=restore_remap=true;
                            }
                        }
                    }
                }
            }
            restored=true;
        }
    }
    if(ln_music_library_take_selection(&waiting)){
        have_waiting=selecting;selecting=false;
        if(have_waiting&&!active){
            have_waiting=false;
            if(restore_remap){
                uint64_t position=queue.position_samples;bool shuffled=queue.shuffled;
                queue.position_samples=0;queue.shuffled=false;
                if(waiting.index!=UINT32_MAX){
                    (void)ln_queue_set_source(&queue,LN_QUEUE_ALL,0,ln_music_library_count(),waiting.index);
                    ln_queue_shuffle(&queue,shuffled);queue.position_samples=position;
                }
                restore_remap=false;
            }
            if(waiting.index!=UINT32_MAX){
                current=waiting;current_cursor=queue.cursor;have_current=true;queue.last_track_id=current.track.id;
                if(queue.position_samples>=current.track.frames)queue.position_samples=0;
                ln_music_history_pin_current(true,current.track.id);
                ln_music_art_request(current.art_path,current.art_crc);
            }
            ++revision;
        }else if(have_waiting)ln_player_stop_for_restart();
    }
    if(next_armed&&p.next_phase==LN_PLAYER_NEXT_FAILED){
        (void)ln_player_cancel_next();next_armed=next_ready=false;next_failed=true;
    }
    if(have_waiting&&ln_music_history_ready()&&!ln_music_history_record_ready(waiting.track.id))
        (void)ln_music_history_prefetch(waiting.track.id);
    if(active&&have_current&&!selecting&&!have_waiting&&!next_failed&&!sleep_due&&
       !prefetching&&!next_ready&&!next_armed&&!resolver_kind){
        uint32_t cursor;
        if(next_queue_cursor(&cursor)&&ln_queue_track_at(&queue,cursor,&next_ordinal))
            (void)resolve_queue(next_ordinal,2);
    }
    struct ln_music_library_row prefetched;
    if(ln_music_library_take_prefetch(&prefetched)&&prefetching&&prefetched.index==next_index){
        upcoming=prefetched;prefetching=false;next_ready=true;
    }
    if(have_waiting&&p.phase==LN_PLAYER_IDLE&&!ln_storage_busy()&&
       (ln_music_history_error()||(ln_music_history_ready()&&
        (ln_music_history_record_ready(waiting.track.id)||ln_music_history_record_error(waiting.track.id))))){
        struct ln_dsp_config dsp;
        if(!configure_sound(waiting.track.rate,profile_gain(&profile,&waiting.track),&dsp)){
            have_waiting=false;active=false;notice="Unsupported sample rate";ln_player_stop();++revision;
        }else if(ln_player_open_configured(waiting.path,waiting.track.file_bytes,requested_position,&dsp,waiting.track.rate)){
            current=waiting;current_cursor=waiting_cursor;have_current=true;have_waiting=false;
            queue.last_track_id=current.track.id;
            ln_music_history_pin_current(true,current.track.id);
            if(!quiz_active)ln_collections_played(current.track.id);
            ln_music_art_request(current.art_path,current.art_crc);
            active=true;queue.position_samples=requested_position;requested_position=0;
            ++revision;
        }
    }else if(active&&!have_waiting&&!selecting&&resolver_kind!=1&&!next_armed&&
              (sleep_tail_ended||(before.phase!=LN_PLAYER_IDLE&&before.phase!=LN_PLAYER_ERROR&&
              (p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR)))){
        if(sleep_active&&sleep_mode>=9u){
            if(sleep_holds_next()||p.phase==LN_PLAYER_ERROR){
                if(!quiz_active&&p.phase!=LN_PLAYER_ERROR&&have_current){
                    (void)ln_music_history_played(current.track.id);
                    (void)ln_music_history_set_resume(current.track.id,false,0);
                }
                sleep_due=true;sleep_tail_ended=false;active=false;queue.position_samples=requested_position=0;
                ++sleep_revision;++revision;
                return;
            }
            if(!next_ready){sleep_tail_ended=true;return;}
        }
        sleep_tail_ended=false;
        if(p.phase==LN_PLAYER_ERROR){++skipped;ln_player_stop();}
        else {
            if(!quiz_active&&have_current){(void)ln_music_history_played(current.track.id);
                (void)ln_music_history_set_resume(current.track.id,false,0);}
            /* A completed final item must restart at its beginning, including
             * after a previously persisted pause or seek within that item. */
            if(queue.position_samples||requested_position){
                queue.position_samples=requested_position=0;++revision;
            }
        }
        uint32_t index;
        if(skipped<LINNET_QUEUE_CAPACITY&&ln_queue_next(&queue,false)&&ln_queue_current(&queue,&index)){
            /* A pending next-member lookup must not swallow this terminal
             * edge. select_current replaces it with a fresh selection token;
             * the library drains any superseded reader before reusing it. */
            requested_position=0;(void)select_current();++revision;
        }else active=false;
    }
    if(active&&have_current&&!selecting&&!have_waiting&&!next_failed&&!sleep_due){
        if(next_ready&&!next_armed&&!sound_pending&&!sleep_holds_next()&&!photo_priority&&(p.phase==LN_PLAYER_PRIME||p.phase==LN_PLAYER_PLAY||p.phase==LN_PLAYER_DRAIN)){
            uint32_t candidate=next_token+1u;if(!candidate)candidate=1;
            struct ln_dsp_config dsp;
            if(!ln_music_history_error()&&ln_music_history_ready()&&!ln_music_history_record_ready(upcoming.track.id)&&!ln_music_history_record_error(upcoming.track.id)){
                (void)ln_music_history_prefetch(upcoming.track.id);
            }else if(!configure_sound(upcoming.track.rate,profile_gain(&profile,&upcoming.track),&dsp)){
                next_ready=false;next_failed=true;
            }else if(ln_player_prepare_next_configured(upcoming.path,upcoming.track.file_bytes,candidate,&dsp,upcoming.track.rate)){
                next_token=candidate;next_armed=true;
#if !defined(LINNET_PROFILE_RELEASE)
                budget_count(&budget_next_arms);
#endif
            }
        }
    }
    if(p.phase!=LN_PLAYER_PAUSE)pause_captured=false;
    if(p.phase==LN_PLAYER_PAUSE&&!audio.playing&&!pause_captured){
        remember_position();pause_captured=ln_music_history_ready()||ln_music_history_error()!=0;
    }
    bool changing=ln_music_loading();
    bool stopped=!audio.playing&&(p.phase==LN_PLAYER_PAUSE||p.phase==LN_PLAYER_IDLE||p.phase==LN_PLAYER_ERROR);
    bool may_write=stopped&&!changing;
    /* Explicit selection can pass briefly through IDLE. Preserve history
     * loading and an already-started publication, but do not start optional
     * writes ahead of the replacement track's metadata and audio reads. */
    /* Finish an admitted page publication at the normal background reserve.
     * Waiting for a full ring again after its write can strand FLUSH and block
     * the following track's history lookup. Only FLUSH/CHECK/ACK apply;
     * returning to IDLE restores the stricter threshold for each new write. */
    bool history_headroom=(stopped||(audio.playing&&
        (audio.queued>=LINNET_AUDIO_RING_FRAMES-2048u||
         (ln_music_history_finishing()&&audio.queued>=read_runway))))&&
        (!changing||!ln_music_history_ready()||ln_music_history_busy());
    bool history_read=stopped||(!audio.playing&&p.phase==LN_PLAYER_PRIME)||urgent||
        (audio.playing&&audio.queued>=read_runway&&!photo_priority);
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
    bool history_scan=ln_music_browser_history_scan_visible();
    ln_paged_history_scan_policy(history_scan,stopped&&!changing&&!photo_priority);
#endif
    ln_music_history_service(history_read,history_headroom,audio.playing);
    ln_music_volume_sync();
    /* Durability work is deferred until playback is stopped/paused and the
     * asynchronous media owner has drained. New UI input may supersede this
     * snapshot while storage progress runs; retain its dirty revision then. */
    if(!quiz_active&&restored&&!persist_failed&&revision!=saved_revision&&!ln_storage_busy()&&
       may_write&&!ln_music_loading()){
        (void)persist();
    }
}

void ln_music_step(void){
    if(shutting_down||stepping)return;
    stepping=true;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t started=ln_time_us();
#endif
    music_step();
#if !defined(LINNET_PROFILE_RELEASE)
    budget_count(&budget_steps);uint32_t elapsed=ln_time_us()-started;
    if(elapsed>budget_max_step_us)budget_max_step_us=elapsed;
#endif
    stepping=false;
}
