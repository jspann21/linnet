#include "services/display/preferences.h"
#include "kernel/runtime.h"
#include "kernel/executive.h"
#include "kernel/shell.h"
#include "kernel/wheel_motion.h"
#include "hal/api/power_policy.h"
#if defined(LINNET_TARGET_VIDEO5G_M25)
#include "targets/video5g-m25/coprocessor.h"
#endif
#if defined(LINNET_FILES)
#include "services/files/files.h"
#endif
#if defined(LINNET_NOTES)
#include "services/notes/notes.h"
#endif
#include "hal/api/platform.h"
#if !defined(LINNET_PROFILE_RELEASE)
#include "kernel/runtime_render_timing.h"
#endif
#include "hal/api/core.h"
#include "hal/api/input.h"
#include "hal/api/power.h"
#include "hal/api/display.h"
#include "hal/api/recovery.h"
#if defined(LINNET_BATTERY_GUARD)
#include "services/power/battery.h"
static struct ln_battery_guard battery_guard;
#endif
#if defined(LINNET_PHOTOS)
#include "services/photos/photos.h"
static uint32_t photo_revision,photo_navigation,photo_wheel_mode;
static bool photo_resume_loaded;
#endif
#if defined(LINNET_NATIVE_UI)
#include "services/display/settings.h"
#include "services/display/clock.h"
#include "linnet_build.h"
#include "linnet_scene_ids.h"
#endif
#if defined(LINNET_AUDIO)
#include "hal/api/audio.h"
#include "services/music/player.h"
#include "services/music/controller.h"
#include "services/music/menu.h"
#include "services/music/artwork.h"
#include "services/music/coverflow.h"
#include "services/music/browser.h"
#include "services/music/search.h"
#include "services/music/queue_view.h"
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#include "hal/api/usb.h"
#endif
#if defined(LINNET_STORAGE)
#include "services/storage/storage.h"
#endif
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
static uint32_t video_revision,video_library_revision;
static const char *video_selected_path;
static char video_path[256];
static bool video_requested,video_resume_loaded;
static bool video_from_files;
static struct ln_fat32_file video_file;
#endif
static ln_executive executive;
#if !defined(LINNET_PROFILE_RELEASE)
void ln_runtime_exec_diagnostics(uint32_t words[24]){ln_exec_diagnostics(&executive,words);}
#endif
static ln_arena scene_arena;
static uint8_t scene_memory[2048] __attribute__((aligned(8)));
static ln_shell *shell;
static struct ln_input_state input;
static struct ln_power_state power;
static uint32_t last_injection, synthetic_flags, override_mask;
static uint32_t sleep_count,wake_count,wake_at,animation_left,animation_at;
#if defined(LINNET_AUDIO)
static uint32_t sound_parent_first;
#endif
#if defined(LINNET_NATIVE_UI)
/* Accumulate across the 32-bit microsecond clock wrap for long timeouts. */
static uint64_t shutdown_idle_us;
static uint32_t shutdown_sample_at,auto_shutdown_timeout;
static bool auto_shutdown_headphones;
static void shutdown_activity(void){shutdown_idle_us=0;shutdown_sample_at=ln_time_us();}
static uint32_t text_scroll_at=UINT32_MAX,palette_at=UINT32_MAX;
#endif
static uint32_t brightness=100;
#if defined(LINNET_HARDWARE_CLICKER)
static void click_feedback_stop(void){
    ln_platform_click_stop();ln_audio_click_stop();
}
static void click_feedback(void){
    uint8_t tone=ln_display_click_tone(),volume=ln_display_click_volume();
    if(tone==1u||!volume){click_feedback_stop();return;}
    uint8_t output=ln_display_click_output();
    bool speaker=output==LN_CLICK_BOTH||output==LN_CLICK_SPEAKER||
        (output==LN_CLICK_AUTO&&!shell->headphones);
    bool headphones=shell->headphones&&(output==LN_CLICK_BOTH||output==LN_CLICK_HEADPHONES||output==LN_CLICK_AUTO);
    /* A cold headphone sink may need codec setup; start it before the piezo. */
    if(headphones)ln_audio_click(tone,volume,ln_music_output_volume());else ln_audio_click_stop();
    if(speaker)ln_platform_click(tone,volume);else ln_platform_click_stop();
}
#endif
static uint32_t backlight_deadline,backlight_timeout=30000000u;
static bool backlight_idle,backlight_off;
static bool first_frame_ready;
#if defined(LINNET_STORAGE)
static bool restart_requested;
static uint32_t restart_started;
#endif
void ln_runtime_restart(void){
#if defined(LINNET_STORAGE)
    if(!restart_requested){restart_requested=true;restart_started=ln_time_us();}
#endif
}
#if defined(LINNET_NATIVE_UI) && defined(LINNET_STORAGE)
static bool boot_theme_waiting;
#endif
bool ln_runtime_first_frame_ready(void){return first_frame_ready;}
static struct ln_wheel_motion wheel_motion;
static uint32_t wheel_scene, wheel_epoch;
__attribute__((weak)) bool ln_display_diagnostic(uint32_t pattern){(void)pattern;return false;}
static uint32_t shutdown_deadline;
static bool shutdown_tracking;
#define LINNET_SHUTDOWN_HOLD_US 2000000u
static bool sleeping, dirty, ready;
#if LINNET_RUNTIME_POWER_POLICY
void ln_runtime_power_begin(void){
    bool busy=!ready||!first_frame_ready||dirty||executive.used||ln_display_busy()||animation_left||shutdown_tracking;
#if defined(LINNET_STORAGE)
    busy=busy||restart_requested||ln_storage_busy();
#endif
#if defined(LINNET_AUDIO)
    enum ln_music_library_status library=ln_music_library_status();
    busy=busy||ln_music_loading()||(library!=LN_LIBRARY_READY&&library!=LN_LIBRARY_ERROR);
#endif
#if defined(LINNET_NATIVE_UI)
    busy=busy||(shell&&shell->scene==LN_UI_SCENE_GAME_PLAY);
#endif
#if defined(LINNET_VIDEO)
    busy=busy||video_requested||ln_video_active()||ln_video_pending();
#endif
#if defined(LINNET_PHOTOS)
    busy=busy||ln_photos_active()||ln_photos_pending();
#endif
#if defined(LINNET_FILES)
    busy=busy||ln_files_pending();
#endif
#if defined(LINNET_NOTES)
    busy=busy||ln_notes_pending();
#endif
    struct ln_power_workload work={.flags=busy?LN_POWER_WORK_BUSY:0u};
#if defined(LINNET_AUDIO)
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    struct ln_player_status player;ln_player_snapshot(&player);
    work.queued_frames=audio.queued;work.rate=audio.rate;work.underruns=audio.underruns;
    work.playback_generation=player.generation;
    if(player.phase==LN_PLAYER_PLAY&&audio.playing&&!audio.paused)work.flags|=LN_POWER_WORK_PLAYING;
    if((player.phase==LN_PLAYER_IDLE||player.phase==LN_PLAYER_ERROR||player.phase==LN_PLAYER_PAUSE)&&!audio.playing)
        work.flags|=LN_POWER_WORK_SETTLED;
    /* A prepared next track is buffered state, not foreground work. Keeping
     * READY busy would hold the CPU at its normal clock for most playlists. */
    if(player.next_phase==LN_PLAYER_NEXT_OPENING||player.next_phase==LN_PLAYER_NEXT_COMMITTED||
       player.next_phase==LN_PLAYER_NEXT_CANCELING)work.flags|=LN_POWER_WORK_NEXT_PENDING;
    if(audio.fault)work.flags|=LN_POWER_WORK_AUDIO_FAULT;
#else
    work.flags|=LN_POWER_WORK_SETTLED;
#endif
    if(!ln_power_policy_begin(&work))ln_panic(30,14,ln_core_clock_failure());
}
void ln_runtime_power_end(void){ln_power_policy_end();}
#endif
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
void ln_runtime_power_checkpoint(void){
    if(!ready)return;
    uint32_t now=ln_time_us();
    if((int32_t)(now-ln_power_next_deadline())<0)return;
    bool changed=ln_power_poll(now,&power);
#if defined(LINNET_NATIVE_UI)
    if(changed&&ln_ui_status_refresh(shell))dirty=true;
#else
    (void)changed;
#endif
}
#endif
#if LINNET_IDLE_CLOCK_HZ
static uint32_t idle_clock_failures;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t idle_clock_cycles,idle_clock_us;
void ln_runtime_idle_clock_diagnostics(uint32_t output[4]){
    output[0]=LINNET_IDLE_CLOCK_HZ;output[1]=idle_clock_cycles;
    output[2]=idle_clock_us;output[3]=idle_clock_failures;
}
#endif
#endif
static bool wake(void);
static void backlight_restore(void){
    backlight_idle=backlight_off;
    backlight_deadline=ln_time_us()+backlight_timeout;
    ln_power_backlight(!sleeping&&!backlight_off&&brightness!=0,
        backlight_off?0u:(uint8_t)((brightness*255u+50u)/100u));
}
static void backlight_activity(void){
    if(sleeping&&!wake())return;
    backlight_deadline=ln_time_us()+backlight_timeout;
    if(backlight_idle!=backlight_off)backlight_restore();
}
#if defined(LINNET_AUDIO)
static uint32_t sleep_timer_revision;
static uint32_t music_revision,music_phase,music_generation,music_progress_at,music_navigation;
static bool music_sound_applying;
static uint32_t music_queue_drawn_revision;
static const char *music_queue_drawn_notice;
static uint32_t music_window_epoch=UINT32_MAX,music_window_selection=UINT32_MAX,music_window_navigation=UINT32_MAX;
static uint32_t search_navigation,search_revision,search_library_status;
static bool playback_from_search,music_window_pending;
#endif
#if defined(LINNET_NATIVE_UI)
static uint32_t display_settings_revision;
static uint32_t update_status_at;
/* Theme presentation tracking: publication needs a completed accepted frame,
 * not merely a render into the CPU framebuffer. No storage calls here. */
static struct {
    uint32_t revision,theme,generation,submitted,completed,failures,resets;
    bool armed,complete;
} theme_frame;
static void theme_frame_observe(void){
    if(!theme_frame.armed||theme_frame.complete||ln_display_busy())return;
    struct ln_display_presentation m;ln_display_presentation_snapshot(&m);
    theme_frame.complete=m.generation==theme_frame.generation&&m.resets==theme_frame.resets&&
        m.failures==theme_frame.failures&&m.frames_submitted==theme_frame.submitted&&
        m.frames_completed==theme_frame.completed+1u;
    if(!theme_frame.complete)theme_frame.armed=false;
}
static bool theme_frame_ready(void){
    theme_frame_observe();
    if(!theme_frame.armed||!theme_frame.complete||theme_frame.revision!=ln_display_theme_revision()||
       theme_frame.theme!=ln_ui_theme_index())return false;
    struct ln_display_presentation m;ln_display_presentation_snapshot(&m);
    return m.generation==theme_frame.generation&&m.resets==theme_frame.resets&&m.failures==theme_frame.failures;
}
bool ln_display_theme_presented(uint32_t revision){
    return theme_frame_ready()&&theme_frame.revision==revision;
}
static void theme_frame_submitted(uint32_t revision){
    if(revision!=ln_display_theme_revision()||!shell->rendered||shell->drawn_theme!=ln_ui_theme_index())return;
    if(theme_frame_ready())return; /* Later animation must not starve a completed theme. */
    struct ln_display_presentation m;ln_display_presentation_snapshot(&m);
    theme_frame.revision=revision;theme_frame.theme=shell->drawn_theme;
    theme_frame.generation=m.generation;theme_frame.submitted=m.frames_submitted;
    theme_frame.completed=m.frames_completed;theme_frame.failures=m.failures;theme_frame.resets=m.resets;
    theme_frame.armed=true;theme_frame.complete=false;
}
static void theme_frame_request(void){
    if(theme_frame_ready())return;
    if(theme_frame.armed&&!theme_frame.complete&&ln_display_busy()&&
       theme_frame.revision==ln_display_theme_revision()&&theme_frame.theme==ln_ui_theme_index())return;
    /* API changes may keep the same effective colors outside the Themes scene.
     * Every new revision still needs its own accepted, completed frame. */
    if(!(shell->dirty&1u))ln_ui_refresh(shell);
    dirty=true;
}
/* End theme presentation tracking. */
static bool scene_is(uint32_t id){
    return shell&&shell->scene<ln_ui_scene_count&&shell->scene==id;
}
#endif
static uint32_t generation(void){
#if defined(LINNET_PROFILE_RELEASE)
    return 0;
#else
    return ln_recovery_generation();
#endif
}
static uint32_t buttons(uint32_t value){
    return ((value&LINNET_BUTTON_MENU)?LN_BUTTON_MENU:0u)|
      ((value&LINNET_BUTTON_PLAY)?LN_BUTTON_PLAY:0u)|
      ((value&LINNET_BUTTON_RIGHT)?LN_BUTTON_NEXT:0u)|
      ((value&LINNET_BUTTON_LEFT)?LN_BUTTON_PREV:0u)|
      ((value&LINNET_BUTTON_SELECT)?LN_BUTTON_SELECT:0u);
}
static bool reset_shell(void){
#if defined(LINNET_NATIVE_UI)
    theme_frame.armed=theme_frame.complete=false;
#endif
    ln_arena_ref ref;ln_arena_reset(&scene_arena);
    if(!ln_arena_alloc(&scene_arena,sizeof(ln_shell),8,&ref))return false;
    shell=ln_arena_get(&scene_arena,ref);if(!shell)return false;
    ln_shell_init(shell);shell->hold=input.hold;shell->headphones=input.headphones;dirty=true;return true;
}
#if defined(LINNET_NATIVE_UI)
static void retain_scene(void){
    /* Preserve the bounded shell value and invalidate all prior arena refs. */
    ln_shell next=*shell;ln_arena_ref ref;ln_arena_reset(&scene_arena);
    if(!ln_arena_alloc(&scene_arena,sizeof next,8,&ref))ln_panic(30,9,0);
    shell=ln_arena_get(&scene_arena,ref);if(!shell)ln_panic(30,10,0);*shell=next;
}
#endif
#if defined(LINNET_AUDIO)
static void browser_navigation(void){
    if(!scene_is(LN_UI_SCENE_MUSIC))return;
    uint32_t navigation=ln_music_browser_navigation();
    if(navigation!=music_navigation){
        shell->selected=0;shell->selections[shell->scene]=0;
        music_navigation=navigation;uint32_t restored_row;if(ln_music_browser_take_selection(&restored_row,&shell->first_visible[shell->scene])){shell->selected=restored_row;shell->selections[shell->scene]=restored_row;}
        ln_ui_refresh(shell);dirty=true;
    }
    if(music_window_epoch==shell->scene_epoch&&music_window_selection==shell->selected&&
       music_window_navigation==navigation)return;
    uint32_t rows=ln_ui_visible_capacity(shell);if(!rows)rows=1;
    ln_music_browser_visible_rows(rows);
    ln_music_browser_window(ln_ui_first_visible(shell));
    music_window_epoch=shell->scene_epoch;music_window_selection=shell->selected;
    music_window_navigation=navigation;
}
static void search_reconcile(bool fallback_selection){
    if(!scene_is(LN_UI_SCENE_MUSIC_SEARCH))return;
    if(!ln_music_search_active())ln_music_search_resume();
    if(search_navigation!=ln_music_search_navigation()){
        search_navigation=ln_music_search_navigation();shell->selected=0;shell->selections[shell->scene]=0;
        ln_ui_refresh(shell);dirty=true;
    }
    uint32_t row,first;
    if(ln_music_search_take_viewport(&row,&first)){
        shell->selected=row;shell->selections[shell->scene]=row;shell->first_visible[shell->scene]=first;
        ln_ui_refresh(shell);dirty=true;
    }
    ln_music_search_window(ln_ui_first_visible(shell));
    if(fallback_selection&&ln_music_search_take_selection(&row)){
        shell->selected=row;ln_ui_refresh(shell);dirty=true;
    }
}
#if LINNET_TARGET_NUMBER == 25u
static bool search_frame_deferred(uint32_t now,uint32_t *deadline){
    if(!scene_is(LN_UI_SCENE_MUSIC_SEARCH)||sleeping||animation_left||
       (shell->dirty&6u)||shell->drawn_theme!=ln_ui_theme_index()){
        ln_music_search_cancel_frame_defer();return false;
    }
    return ln_music_search_frame_deferred(now,deadline);
}
#endif
#endif
#if defined(LINNET_PHOTOS)
static bool photo_scene(void){
    return scene_is(LN_UI_SCENE_PHOTOS)||scene_is(LN_UI_SCENE_PHOTOS_GRID)||
        scene_is(LN_UI_SCENE_PHOTOS_VIEWER)||scene_is(LN_UI_SCENE_PHOTOS_OPTIONS);
}
static void photo_save(void){
    if(ln_photos_file_mode())return;
    uint8_t raw[16];if(!photo_resume_loaded)return;
    ln_photos_resume_get(raw);ln_display_settings_photo_set(raw);
}
static void photo_navigation_update(void){
    if(!photo_scene()||(ln_photos_file_mode()&&ln_photos_active()))return;
    if(!ln_photos_active())ln_photos_open();
    uint32_t navigation=ln_photos_navigation();
    if(navigation!=photo_navigation||(!scene_is(LN_UI_SCENE_PHOTOS_OPTIONS)&&
       ((ln_photos_view()==LN_PHOTOS_VIEWER&&!scene_is(LN_UI_SCENE_PHOTOS_VIEWER))||
        (ln_photos_view()==LN_PHOTOS_LIST&&!scene_is(LN_UI_SCENE_PHOTOS_GRID))||
        (ln_photos_view()==LN_PHOTOS_ALBUMS&&!scene_is(LN_UI_SCENE_PHOTOS))))){
        const char *id=ln_photos_view()==LN_PHOTOS_VIEWER?"linnet.system.photos-viewer":
            ln_photos_view()==LN_PHOTOS_LIST?"linnet.system.photos-grid":"linnet.system.photos";
        if(ln_ui_show(shell,id))retain_scene();
        shell->selected=ln_photos_selection();shell->selections[shell->scene]=shell->selected;
        photo_navigation=navigation;photo_wheel_mode=0;ln_ui_refresh(shell);dirty=true;
    }
    if(ln_photos_view()!=LN_PHOTOS_VIEWER){
        if(ln_photos_focus(shell->selected))photo_save();
        uint32_t count=ln_ui_visible_capacity(shell);if(!count)count=1;
        ln_photos_window(ln_ui_first_visible(shell),count);
    }
}
#endif
#if defined(LINNET_VIDEO)
static bool video_scene(void){return scene_is(LN_UI_SCENE_VIDEO_PLAYER)||scene_is(LN_UI_SCENE_VIDEO_CONTROLS);}
static bool video_path_set(const char *path){
    if(!path||path[0]!='/')return false;uint32_t n=0;while(path[n]&&n<255u)++n;if(!n||n==255u)return false;
    for(uint32_t i=0;i<=n;++i)video_path[i]=path[i];video_selected_path=video_path;return true;
}
static bool video_start(const char *path){
    if(!shell||!path||path[0]!='/'||ln_video_active()||ln_video_playback_pending())return false;
    if(!video_path_set(path))return false;
    ln_music_stop();ln_photos_close();video_requested=true;
    (void)ln_ui_show(shell,"linnet.system.video-player");dirty=true;return true;
}
bool ln_runtime_video_open(const char *path){video_from_files=false;return video_start(path);}
bool ln_runtime_video_requested(void){return video_requested;}
bool ln_runtime_video_open_file(const struct ln_fat32_file*file,const char*title){
    if(!shell||!file||!title||ln_video_active()||ln_video_playback_pending())return false;
    char path[192];uint32_t n=0;path[n++]='/';
    static const char hex[]="0123456789ABCDEF";
    for(uint32_t i=8;i;--i)path[n++]=hex[(file->cluster>>((i-1u)*4u))&15u];path[n++]='/';
    for(uint32_t i=0;title[i]&&n+1u<sizeof path;++i)path[n++]=title[i];path[n]=0;
    if(!video_path_set(path))return false;video_file=*file;video_from_files=true;
    if(ln_video_output_prompt()){(void)ln_ui_show(shell,"linnet.system.video-output");dirty=true;return true;}
    return video_start(video_path);
}
static void video_save(void){uint32_t raw[2];if(video_resume_loaded){ln_video_resume_get(raw);ln_video_resume_set(raw);ln_display_settings_video_set(raw);}}
static bool stop_video(void){
    video_requested=false;video_save();ln_video_stop();uint32_t started=ln_time_us();
    while(ln_video_active()||ln_video_pending()){
        ln_video_step();ln_storage_step();if(ln_time_us()-started>=30000000u)return false;
    }return true;
}
#endif
#if defined(LINNET_NATIVE_UI)
static bool clock_save_presented;
static const char *clock_admission(void){
    if(ln_power_calendar_busy())return "Saving date and time...";
    if(!ready||sleeping||shutdown_tracking)return "Try again when the player is awake.";
    if(shell->hold)return "Unlock Hold before saving.";
    /* The RTC uses the power chip I2C bus, independently of storage activity. */
    return 0;
}
bool ln_ui_clock_begin(void){
    clock_save_presented=false;ln_clock_editor_cancel();
    const char *blocked=clock_admission();if(blocked){ln_clock_editor_block(blocked);return false;}
    struct ln_calendar calendar;
    enum ln_calendar_read_result result=ln_power_calendar_read_status(&calendar);
    ln_clock_editor_begin(&calendar,result==LN_CALENDAR_READ_VALID);
    if(result==LN_CALENDAR_READ_UNAVAILABLE)ln_clock_editor_block("Clock unavailable. Edit, retry Save.");
    return true;
}
static void clock_save_step(void){
    if(ln_power_calendar_busy()){
        enum ln_calendar_result result=ln_power_calendar_poll();
        if(result==LN_CALENDAR_PENDING)return;
        clock_save_presented=false;
        if(scene_is(LN_UI_SCENE_CLOCK_EDIT)){
            ln_clock_editor_result(result);
            if(result==LN_CALENDAR_SAVED&&ln_ui_show(shell,"linnet.system.clock"))retain_scene();
            ln_ui_refresh(shell);dirty=true;
        }
        return;
    }
    if(!clock_save_presented)return;
    if(!scene_is(LN_UI_SCENE_CLOCK_EDIT)){clock_save_presented=false;ln_clock_editor_cancel();return;}
    if(ln_display_busy())return;
    clock_save_presented=false;
    struct ln_calendar requested;if(!ln_clock_editor_request(&requested))return;
    const char *blocked=clock_admission();
    if(blocked)ln_clock_editor_block(blocked);
    else{
        enum ln_calendar_result result=ln_power_calendar_set(&requested);
        if(result==LN_CALENDAR_PENDING)return;
        ln_clock_editor_result(result);
        if(result==LN_CALENDAR_SAVED){if(ln_ui_show(shell,"linnet.system.clock"))retain_scene();}
    }
    ln_ui_refresh(shell);dirty=true;
}
#endif
static void apply(uint32_t kind,int32_t value){
#if defined(LINNET_AUDIO) && LINNET_TARGET_NUMBER == 25u
    /* A new interaction cancels the hold. Release edges leave Find armed. */
    if((kind==LN_INPUT_WHEEL&&value)||
       (kind==LN_INPUT_BUTTON&&((uint32_t)value&~shell->buttons))||
       (kind==LN_INPUT_HOLD&&((value!=0)!=(shell->hold!=0)))||
       (kind==LN_INPUT_HEADPHONE&&((value!=0)!=(shell->headphones!=0))))
        ln_music_search_cancel_frame_defer();
#endif
#if defined(LINNET_HARDWARE_CLICKER)
    uint32_t feedback_pressed=kind==LN_INPUT_BUTTON?((uint32_t)value&~shell->buttons):0;
    /* Transport commands must not enqueue a tone that Pause can cut short
     * or a track change happens to cancel. Navigation keeps its feedback. */
    bool quiet_transport=scene_is(LN_UI_SCENE_NOW_PLAYING)&&feedback_pressed&&
        !(feedback_pressed&~(LN_BUTTON_PLAY|LN_BUTTON_NEXT|LN_BUTTON_PREV));
    bool changing_click=kind==LN_INPUT_BUTTON&&!shell->hold&&
        scene_is(LN_UI_SCENE_MUSIC_SOUND)&&ln_sound_menu_click(ln_music_sound_action(shell->selected))&&
        ((uint32_t)value&~shell->buttons&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV));
    if((kind==LN_INPUT_HOLD&&value)||
       (kind==LN_INPUT_HEADPHONE&&((value!=0)!=(shell->headphones!=0))))click_feedback_stop();
    else if(!quiet_transport&&!changing_click&&!scene_is(LN_UI_SCENE_GAME_PLAY)&&!shell->hold&&
            kind==LN_INPUT_BUTTON&&((uint32_t)value&~shell->buttons))
        click_feedback();
#endif
    if((kind==LN_INPUT_WHEEL&&value&&!shell->hold)||
       (kind==LN_INPUT_BUTTON&&!shell->hold&&(uint32_t)value!=shell->buttons)||
       (kind==LN_INPUT_HOLD&&!value&&shell->hold))backlight_activity();
#if defined(LINNET_NATIVE_UI)
    if((kind==LN_INPUT_WHEEL&&value&&!shell->hold)||
       (kind==LN_INPUT_BUTTON&&!shell->hold&&((uint32_t)value&~shell->buttons)))shutdown_activity();
    uint32_t before=shell->scene;
#endif
    uint32_t wheel_selection=shell->selected,wheel_scene=shell->scene;
    bool handled=false,wheel_changed=false;
#if defined(LINNET_NOTES)
    handled=ln_notes_input(shell,kind,value);
#endif
#if defined(LINNET_FILES)
    if(!handled)handled=ln_files_input(shell,kind,value);
#endif
#if defined(LINNET_VIDEO)
    if(scene_is(LN_UI_SCENE_VIDEOS)&&!ln_video_library_empty()&&!shell->hold&&
       ((kind==LN_INPUT_WHEEL&&value)||
        (kind==LN_INPUT_BUTTON&&((uint32_t)value&~shell->buttons))))
        ln_storage_video_browse(true,true);
    if(kind==LN_INPUT_HEADPHONE)ln_video_headphones(value!=0);
    if(!shell->hold&&scene_is(LN_UI_SCENE_VIDEO_SETTINGS)&&kind==LN_INPUT_BUTTON){
        uint32_t pressed=(uint32_t)value&~shell->buttons;
        if(pressed&LN_BUTTON_MENU)(void)ln_ui_show(shell,"linnet.system.settings");
        else if(pressed&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV)){
            (void)ln_video_option_adjust(shell->selected,(pressed&LN_BUTTON_PREV)?-1:1);
            ln_display_settings_video_options_set(ln_video_options());
        }
        if(pressed){shell->buttons=(uint32_t)value;handled=true;dirty=true;ln_ui_refresh(shell);}
    }else if(!shell->hold&&scene_is(LN_UI_SCENE_VIDEO_OUTPUT)&&kind==LN_INPUT_BUTTON){
        uint32_t pressed=(uint32_t)value&~shell->buttons;
        if(pressed&LN_BUTTON_MENU)(void)ln_ui_show(shell,video_from_files?"linnet.system.files":"linnet.system.videos");
        else if(pressed&LN_BUTTON_SELECT){ln_video_output_once(shell->selected==1u);(void)video_start(video_path);}
        if(pressed){shell->buttons=(uint32_t)value;handled=true;dirty=true;ln_ui_refresh(shell);}
    }else if(!shell->hold&&(video_scene()||scene_is(LN_UI_SCENE_VIDEOS))){
        if(kind==LN_INPUT_BUTTON){uint32_t pressed=(uint32_t)value&~shell->buttons;
            if(pressed){
                if(scene_is(LN_UI_SCENE_VIDEOS)){
                    if(pressed&LN_BUTTON_SELECT){const char *path=ln_video_library_path(shell->selected);if(path){video_from_files=false;if(ln_video_output_prompt()&&video_path_set(path))(void)ln_ui_show(shell,"linnet.system.video-output");else (void)ln_runtime_video_open(path);}}
                    else if(pressed&LN_BUTTON_MENU)(void)ln_ui_show(shell,"linnet.system.home");
                }else if(pressed&LN_BUTTON_MENU){video_save();video_requested=false;ln_video_stop();(void)ln_ui_show(shell,video_from_files?"linnet.system.files":"linnet.system.videos");}
                else if(pressed&LN_BUTTON_PLAY)(void)ln_video_pause(!ln_video_paused());
                else if(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV))(void)ln_video_seek((pressed&LN_BUTTON_PREV)?-10:10);
                else if(pressed&LN_BUTTON_SELECT){bool show=scene_is(LN_UI_SCENE_VIDEO_PLAYER);if(ln_video_controls(show))(void)ln_ui_show(shell,show?"linnet.system.video-controls":"linnet.system.video-player");}
                shell->buttons=(uint32_t)value;handled=true;dirty=true;ln_ui_refresh(shell);
            }
        }else if(kind==LN_INPUT_WHEEL&&video_scene()){(void)ln_music_volume(value);handled=true;}
    }
    if((video_requested||ln_video_active()||ln_video_playback_pending())&&!video_scene()&&!scene_is(LN_UI_SCENE_VIDEOS)){
        video_requested=false;video_save();ln_video_stop();if(kind==LN_INPUT_BUTTON){shell->buttons=(uint32_t)value;handled=true;}
    }
#endif
#if defined(LINNET_PHOTOS)
    if(photo_scene()&&!shell->hold){
        if(kind==LN_INPUT_BUTTON){
            uint32_t pressed=(uint32_t)value&~shell->buttons;
            if(pressed){
                if(scene_is(LN_UI_SCENE_PHOTOS_OPTIONS)){
                    if(pressed&LN_BUTTON_MENU)(void)ln_ui_show(shell,"linnet.system.photos-viewer");
                    else if(pressed&LN_BUTTON_SELECT){
                        uint32_t choice=shell->selected;
                        if(choice<3u)photo_wheel_mode=choice;
                        else if(choice==3u)ln_photos_rotate();
                        else if(choice==4u)ln_photos_slideshow(!ln_photos_slideshow_active());
                        else if(choice==5u)ln_photos_zoom(-32);
                        (void)ln_ui_show(shell,"linnet.system.photos-viewer");
                    }
                }else if(pressed&LN_BUTTON_MENU){
                    if(ln_photos_file_mode()){ln_photos_close();(void)ln_ui_show(shell,"linnet.system.files");}
                    else if(!ln_photos_back()){ln_photos_close();(void)ln_ui_show(shell,"linnet.system.home");}
                }else if(ln_photos_view()==LN_PHOTOS_VIEWER){
                    if((pressed&LN_BUTTON_SELECT)&&!ln_photos_file_mode()){ln_photos_slideshow(false);(void)ln_ui_show(shell,"linnet.system.photos-options");}
                    else if((pressed&LN_BUTTON_PLAY)&&!ln_photos_file_mode())ln_photos_slideshow(!ln_photos_slideshow_active());
                    else if(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV))ln_photos_next((pressed&LN_BUTTON_PREV)?-1:1);
                }else if(pressed&LN_BUTTON_SELECT)(void)ln_photos_select(shell->selected);
                else if(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV))
                    (void)ln_shell_input(shell,LN_INPUT_WHEEL,(pressed&LN_BUTTON_PREV)?-3:3);
                else if(pressed&LN_BUTTON_PLAY)ln_music_toggle();
                shell->buttons=(uint32_t)value;handled=true;ln_ui_refresh(shell);dirty=true;
                photo_navigation_update();photo_save();
            }
        }else if(kind==LN_INPUT_WHEEL&&scene_is(LN_UI_SCENE_PHOTOS_VIEWER)){
            if(photo_wheel_mode==1u)(void)ln_photos_pan((int64_t)value*16,0);
            else if(photo_wheel_mode==2u)(void)ln_photos_pan(0,(int64_t)value*16);
            else (void)ln_photos_zoom(value);
            photo_save();handled=true;
        }
    }
#endif
#if defined(LINNET_NATIVE_UI)
    if(kind==LN_INPUT_BUTTON&&!shell->hold&&(scene_is(LN_UI_SCENE_BATTERY)||scene_is(LN_UI_SCENE_SHUTDOWN))){
        uint32_t pressed=(uint32_t)value&~shell->buttons;
        if(pressed&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV)){
            if(scene_is(LN_UI_SCENE_BATTERY))(void)ln_battery_capacity_adjust((pressed&LN_BUTTON_PREV)?-1:1);
            else if(scene_is(LN_UI_SCENE_SHUTDOWN)){
                if(shell->selected<2u)(void)ln_auto_shutdown_adjust(shell->selected==1u,(pressed&LN_BUTTON_PREV)?-1:1);
                else if(pressed&LN_BUTTON_PREV)(void)ln_ui_back(shell);
                else (void)ln_ui_show(shell,"linnet.system.battery");
            }
            shell->buttons=(uint32_t)value;ln_ui_refresh(shell);dirty=true;handled=true;
        }else if(pressed&LN_BUTTON_MENU){
            (void)ln_ui_back(shell);
            shell->buttons=(uint32_t)value;dirty=true;handled=true;
        }
    }
#endif
#if defined(LINNET_AUDIO)
    if(kind==LN_INPUT_BUTTON&&!shell->hold&&scene_is(LN_UI_SCENE_SLEEP_TIMER)){
        uint32_t pressed=(uint32_t)value&~shell->buttons;
        if(pressed&LN_BUTTON_MENU){
            (void)ln_ui_back(shell);handled=true;
        }else if((pressed&LN_BUTTON_SELECT)||(shell->selected<2u&&(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV)))){
            (void)ln_music_sleep_adjust(shell->selected,(pressed&LN_BUTTON_PREV)?-1:1);handled=true;
        }
        if(handled){shell->buttons=(uint32_t)value;ln_ui_refresh(shell);dirty=true;}
    }
    if(kind==LN_INPUT_HEADPHONE)ln_music_headphones(value!=0);
    if(!handled&&kind==LN_INPUT_BUTTON&&!shell->hold){uint32_t pressed=(uint32_t)value&~shell->buttons;
#if defined(LINNET_COVERFLOW)
        if(scene_is(LN_UI_SCENE_COVER_FLOW)){
            if(pressed&LN_BUTTON_MENU)(void)ln_ui_back(shell);
            else if(pressed&LN_BUTTON_SELECT){if(ln_coverflow_select()){(void)ln_ui_show(shell,"linnet.system.music");browser_navigation();}}
            else if(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV))ln_coverflow_move((pressed&LN_BUTTON_PREV)?-1:1);
            else if(pressed&LN_BUTTON_PLAY)ln_music_toggle();
            dirty=true;handled=true;
        }else
#endif
        if((pressed&LN_BUTTON_NEXT)&&scene_is(LN_UI_SCENE_MUSIC)){
            (void)ln_music_browser_actions(shell->selected);browser_navigation();handled=true;dirty=true;
        }else if((pressed&LN_BUTTON_PREV)&&scene_is(LN_UI_SCENE_MUSIC)){
            (void)ln_music_browser_options(shell->selected);browser_navigation();handled=true;dirty=true;
        }else if((pressed&LN_BUTTON_SELECT)&&scene_is(LN_UI_SCENE_MUSIC)){
            (void)ln_music_browser_select(shell->selected);browser_navigation();handled=true;
#if defined(LINNET_COVERFLOW)
            if(ln_music_browser_take_coverflow())(void)ln_ui_show(shell,"linnet.system.cover-flow");
#endif
            if(ln_music_browser_take_search()){
                ln_music_search_open();if(ln_ui_show(shell,"linnet.system.music-search"))dirty=true;
            }
        }else if((pressed&LN_BUTTON_SELECT)&&scene_is(LN_UI_SCENE_NOW_PLAYING)){
            if(ln_ui_show(shell,"linnet.system.music-options"))dirty=true;handled=true;
        }else if((pressed&LN_BUTTON_MENU)&&scene_is(LN_UI_SCENE_NOW_PLAYING)){
            if(playback_from_search)ln_music_search_resume();
            if(ln_ui_show(shell,ln_music_file_mode()?"linnet.system.files":playback_from_search?"linnet.system.music-search":"linnet.system.music"))dirty=true;
            handled=true;
        }else if((pressed&LN_BUTTON_SELECT)&&scene_is(LN_UI_SCENE_MUSIC_OPTIONS)){
            uint32_t action=ln_music_option_action(shell->selected);
            if(action==7){ln_music_queue_open();(void)ln_ui_show(shell,"linnet.system.music-queue");}
            else if(action==12){(void)ln_ui_show(shell,"linnet.system.sleep-timer");}
            else if(action==11){ln_music_queue_close();(void)ln_ui_show(shell,"linnet.system.music");(void)ln_music_save_queue();browser_navigation();}
            else if(action==8){(void)ln_ui_show(shell,"linnet.system.music-sound");}
            else (void)ln_music_option_select(action);
            ln_ui_refresh(shell);dirty=true;handled=true;
        }else if(scene_is(LN_UI_SCENE_MUSIC_QUEUE)&&(pressed&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV|LN_BUTTON_MENU))){
            if(pressed&LN_BUTTON_MENU){ln_music_queue_close();(void)ln_ui_show(shell,"linnet.system.music-options");}
            else {ln_music_queue_choose(shell->selected);
                if(pressed&LN_BUTTON_PREV){ln_music_queue_close();(void)ln_ui_show(shell,"linnet.system.music-options");}
                else (void)ln_ui_show(shell,"linnet.system.music-queue-actions");}
            ln_ui_refresh(shell);dirty=true;handled=true;
        }else if(scene_is(LN_UI_SCENE_MUSIC_QUEUE_ACTIONS)&&(pressed&(LN_BUTTON_SELECT|LN_BUTTON_MENU))){
            if((pressed&LN_BUTTON_MENU)||ln_music_queue_edit(ln_music_queue_action(shell->selected)))
                (void)ln_ui_show(shell,"linnet.system.music-queue");
            ln_ui_refresh(shell);dirty=true;handled=true;
        }else if(scene_is(LN_UI_SCENE_MUSIC_SOUND)&&(pressed&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV|LN_BUTTON_MENU))){
            uint32_t row=shell->selected;
            if(pressed&LN_BUTTON_MENU){
                if(ln_music_sound_back(&row)){shell->selected=row;shell->first_visible[shell->scene]=sound_parent_first;}
                else (void)ln_ui_back(shell);
            }else if(ln_music_sound_action(row)>=LN_SOUND_CUSTOM_EQ){
                if(pressed&LN_BUTTON_PREV){if(!ln_music_sound_back(&row))(void)ln_ui_back(shell);else {shell->selected=row;shell->first_visible[shell->scene]=sound_parent_first;}}
                else {uint32_t first=ln_ui_first_visible(shell);
                    if(ln_music_sound_enter(row)){sound_parent_first=first;shell->selected=0;shell->first_visible[shell->scene]=0;}}
            }else{
                (void)ln_music_sound_adjust(row,(pressed&LN_BUTTON_PREV)?-1:1);
#if defined(LINNET_HARDWARE_CLICKER)
                if(ln_sound_menu_click(ln_music_sound_action(row))){
                    click_feedback_stop();click_feedback();
                }
#endif
            }
            ln_ui_refresh(shell);dirty=true;handled=true;
        }else if(scene_is(LN_UI_SCENE_MUSIC_SEARCH)&&(pressed&(LN_BUTTON_SELECT|LN_BUTTON_NEXT|LN_BUTTON_PREV|LN_BUTTON_MENU))){
            if(pressed&LN_BUTTON_MENU){if(!ln_music_search_back())(void)ln_ui_show(shell,"linnet.system.music");}
            else if(pressed&LN_BUTTON_NEXT){if(ln_music_search_actions(shell->selected)){(void)ln_ui_show(shell,"linnet.system.music");browser_navigation();}else ln_music_search_right();}
            else if(pressed&LN_BUTTON_PREV)ln_music_search_left();
            else {ln_music_search_window(ln_ui_first_visible(shell));(void)ln_music_search_select(shell->selected);}
            search_reconcile(false);
            ln_ui_refresh(shell);dirty=true;handled=true;
        }else if((pressed&LN_BUTTON_MENU)&&scene_is(LN_UI_SCENE_MUSIC_OPTIONS)){
            if(ln_ui_show(shell,"linnet.system.now-playing"))dirty=true;handled=true;
        }else if((pressed&LN_BUTTON_MENU)&&scene_is(LN_UI_SCENE_MUSIC)){
            handled=ln_music_browser_back();
            if(handled)browser_navigation();
#if defined(LINNET_COVERFLOW)
            if(ln_music_browser_take_coverflow()){(void)ln_ui_show(shell,"linnet.system.cover-flow");dirty=true;handled=true;}
            else
#endif
            if(ln_music_browser_take_search_return()){ln_music_search_resume();(void)ln_ui_show(shell,"linnet.system.music-search");dirty=true;handled=true;}
            else if(!handled){(void)ln_ui_show(shell,"linnet.system.home");dirty=true;handled=true;}
        }else if((pressed&LN_BUTTON_PLAY)&&!scene_is(LN_UI_SCENE_GAME_PLAY)){
            if(scene_is(LN_UI_SCENE_MUSIC)&&ln_music_browser_queue_status()==LN_BROWSER_QUEUE_CAPACITY)
                (void)ln_music_browser_select_single(shell->selected);
            else ln_music_toggle();handled=true;
        }
        else if(scene_is(LN_UI_SCENE_NOW_PLAYING)&&(pressed&(LN_BUTTON_NEXT|LN_BUTTON_PREV))){
            (void)ln_music_next((pressed&LN_BUTTON_PREV)!=0);handled=true;}
        if(handled)shell->buttons=(uint32_t)value;
    }
#if defined(LINNET_COVERFLOW)
    if(kind==LN_INPUT_WHEEL&&!shell->hold&&scene_is(LN_UI_SCENE_COVER_FLOW)){wheel_changed=ln_coverflow_move(value);handled=true;}
#endif
    if(kind==LN_INPUT_WHEEL&&!shell->hold&&scene_is(LN_UI_SCENE_NOW_PLAYING)){(void)ln_music_volume(value);handled=true;}
#endif
    if(!handled&&ln_shell_input(shell,kind,value)){
        dirty=true;
#if defined(LINNET_AUDIO)
        /* Bind a newly selected music page before this input's frame renders.
         * Cached pages then need no follow-up browser revision to replace
         * provisional labels. */
        if(kind==LN_INPUT_WHEEL&&scene_is(LN_UI_SCENE_MUSIC))music_window_pending=true;
#endif
    }
#if defined(LINNET_HARDWARE_CLICKER)
    /* Feedback follows a changed selection, including service-owned lists.
     * Contact, clamped movement and handled wheel events without navigation
     * (clock editing, music volume, photo zoom) do not count as scrolled items. */
    wheel_changed=wheel_changed||(kind==LN_INPUT_WHEEL&&!shell->hold&&
        shell->scene==wheel_scene&&shell->selected!=wheel_selection);
    if(wheel_changed&&!scene_is(LN_UI_SCENE_GAME_PLAY))click_feedback();
#else
    (void)wheel_selection;(void)wheel_scene;
    (void)wheel_changed;
#endif
#if defined(LINNET_PHOTOS)
    if(photo_scene())photo_navigation_update();
    else if(ln_photos_active()){photo_save();ln_photos_close();}
#endif
#if defined(LINNET_NATIVE_UI)
    if(before!=shell->scene){
#if defined(LINNET_HARDWARE_CLICKER)
        /* Do not carry a menu click into gameplay. */
        if(scene_is(LN_UI_SCENE_GAME_PLAY))click_feedback_stop();
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_COVERFLOW)
        /* A ready Cover Flow revision belongs to this transition's frame.
         * Preserve the animation deadline when revisiting a moving view. */
        ln_coverflow_visible(scene_is(LN_UI_SCENE_COVER_FLOW));
        if(scene_is(LN_UI_SCENE_COVER_FLOW))shell->dirty|=ln_coverflow_tick(ln_time_us());
#endif
#if defined(LINNET_FILES)
        /* Bind an already prepared Files page before this transition paints.
         * The common storage gate keeps this safe in validation callbacks. */
        if(scene_is(LN_UI_SCENE_FILES))ln_files_step(shell);
#endif
        if(before<ln_ui_scene_count&&ln_ui_scenes[before].id&&
           scene_is(LN_UI_SCENE_GAME_PLAY)==false){ln_games_audio_stop();}
#if defined(LINNET_AUDIO)
        if(scene_is(LN_UI_SCENE_MUSIC_SOUND)){uint32_t row;
            if(ln_music_sound_back(&row)){shell->selected=row;shell->first_visible[shell->scene]=sound_parent_first;ln_ui_refresh(shell);}}
#endif
        retain_scene();
    }
#endif
}
static _Noreturn void shutdown_failed(uint32_t phase){
    /* Restore independent recovery even if a terminal PMU request failed. */
    ln_core_watchdog_arm();ln_panic(33,phase,0);
}
#if defined(LINNET_NATIVE_UI)
#if defined(LINNET_PHOTOS)
static bool stop_photos(void){
    photo_save();ln_photos_close();uint32_t started=ln_time_us();
    do {
        ln_photos_step();
        if(!ln_photos_pending())return true;
        if(ln_time_us()-started>=30000000u)return false;
        ln_storage_step();
    }while(true);
}
#endif
static bool save_display_settings(void){
#if defined(LINNET_PHOTOS)
    photo_save();
#endif
    uint32_t started=ln_time_us();
    while(ln_display_settings_pending()){
        ln_display_settings_step(true);
        if(ln_time_us()-started>=30000000u)return false;
        ln_storage_step();
    }
    return true;
}
#endif
#if defined(LINNET_NOTES) || defined(LINNET_FILES)
static bool stop_documents(void){
#if defined(LINNET_NOTES)
    ln_notes_close();
#endif
#if defined(LINNET_FILES)
    ln_files_close();
#endif
    uint32_t started=ln_time_us();
#if defined(LINNET_FILES)
    while(ln_files_pending()){ln_files_step(0);if(ln_time_us()-started>=30000000u)return false;ln_storage_step();}
#endif
#if defined(LINNET_NOTES)
    while(ln_notes_pending()){ln_notes_step(0);if(ln_time_us()-started>=30000000u)return false;ln_storage_step();}
#endif
    return true;
}
#endif
#if defined(LINNET_STORAGE)
static _Noreturn void restart(bool disk_mode){
    ready=false;shutdown_tracking=false;animation_left=0;
#if defined(LINNET_NATIVE_UI)
    if(!ln_games_quiesce(true))ln_panic(34,7,0);
#endif
#if defined(LINNET_VIDEO)
    if(!stop_video())ln_panic(34,5,0);
#endif
#if defined(LINNET_PHOTOS)
    if(!stop_photos())ln_panic(34,4,0);
#endif
#if defined(LINNET_NOTES) || defined(LINNET_FILES)
    if(!stop_documents())ln_panic(34,5,0);
#endif
#if defined(LINNET_AUDIO)
    if(!ln_music_prepare_shutdown())ln_panic(34,2,0);
#endif
#if defined(LINNET_NATIVE_UI)
    if(!save_display_settings())ln_panic(34,3,0);
#endif
    if(disk_mode)(void)ln_storage_disk_mode();else (void)ln_storage_restart();
    ln_panic(34,1,0);
}
#endif
void ln_runtime_restart_step(void){
#if defined(LINNET_STORAGE)
    if(ready&&restart_requested){
        if(!ln_storage_busy())restart(false);
        if(ln_time_us()-restart_started>=30000000u)ln_panic(34,6,0);
    }
#endif
}
static _Noreturn void shutdown(bool save_settings){
    (void)save_settings;
    ready=false;shutdown_tracking=false;animation_left=0;
#if defined(LINNET_NATIVE_UI)
    if(!ln_games_quiesce(save_settings))ln_panic(34,7,0);
#endif
#if defined(LINNET_NOTES) || defined(LINNET_FILES)
    if(!stop_documents())shutdown_failed(14);
#endif
#if defined(LINNET_VIDEO)
    if(!stop_video())shutdown_failed(13);
#endif
#if defined(LINNET_PHOTOS)
    if(!stop_photos())shutdown_failed(12);
#endif
#if defined(LINNET_AUDIO)
    if(!ln_music_quiesce(save_settings))shutdown_failed(10);
    if(!ln_audio_stop())shutdown_failed(9);
#endif
#if defined(LINNET_STORAGE)
#if defined(LINNET_NATIVE_UI)
    if(save_settings&&!save_display_settings())shutdown_failed(11);
#endif
    if(!ln_storage_quiesce())shutdown_failed(8);
#endif
    if(!ln_display_quiesce())shutdown_failed(1);
    if(!ln_platform_audio_idle())shutdown_failed(2);
#if !defined(LINNET_PROFILE_RELEASE)
    if(!ln_usb_stop())shutdown_failed(3);
#endif
    ln_core_watchdog_disarm();
    ln_platform_quiesce();
    if(!ln_platform_dma_idle())shutdown_failed(4);
    ln_power_backlight(false,0);
    /* CHGWAK immediately restarts an externally powered PCF50605. Keep
     * EXTONWAK enabled in both cases, and allow charger wake only unplugged. */
    uint8_t wake_sources=LINNET_POWER_FIREWIRE;
    if(!ln_power_sources())wake_sources|=LINNET_POWER_USB;
    if(!ln_power_shutdown(wake_sources))shutdown_failed(5);
    /* GOSTDBY removes CPU power. A successful bus write without power-down
     * must not leave an apparently off device running indefinitely. */
    for(uint32_t i=0;i<20u;++i)if(!ln_core_delay_us(10000u))shutdown_failed(6);
    shutdown_failed(7);
}
static bool wake(void){
    if(!sleeping)return true;
#if defined(LINNET_STORAGE)
    if(!ln_storage_resume())return false;
#endif
    if(!ln_power_wake())return false;
    sleeping=false;++wake_count;wake_at=0;
    backlight_restore();
    return true;
}
bool ln_runtime_init(void){
    first_frame_ready=false;
#if !defined(LINNET_PROFILE_RELEASE)
    rnd_init();
#endif
    LN_BOOT_STAMP(LN_BT_OS_RUNTIME);
#if defined(LINNET_NATIVE_UI)
    ln_display_settings_init();
    display_settings_revision=ln_display_settings_revision();
    shutdown_activity();auto_shutdown_timeout=0;auto_shutdown_headphones=false;
    update_status_at=0;
    brightness=ln_preferences_brightness();
        backlight_timeout=ln_display_backlight_timeout_us();
    backlight_off=ln_display_backlight_off();
#endif
    ln_exec_init(&executive);ln_arena_init(&scene_arena,scene_memory,sizeof scene_memory);
    if(!ln_core_init())ln_panic(30,1,0);
    if(!ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ)){
        uint32_t cop[8];ln_display_cop_diagnostics(cop);
        ln_panic(30,2,(ln_core_clock_failure()<<16)|cop[0]);
    }
    LN_BOOT_STAMP(LN_BT_OS_CLOCK);
    /* Initialize large media buffers after the verified clock setup,
     * before input, display providers or the first service step can use them. */
#if defined(LINNET_VIDEO)
    ln_video_init();video_revision=0;video_requested=video_resume_loaded=false;
#endif
#if defined(LINNET_FILES)
    ln_files_init();
#endif
#if defined(LINNET_PHOTOS)
    ln_photos_init();photo_revision=photo_navigation=photo_wheel_mode=0;photo_resume_loaded=false;
#endif
    LN_BOOT_STAMP(LN_BT_OS_MEDIA);
    if(!ln_input_init())ln_panic(30,3,0);
    if(!ln_power_init())ln_panic(30,4,0);
    if(!ln_display_init())ln_panic(30,5,ln_display_init_error());
#if defined(LINNET_AUDIO)
    /* Correct the resident monitor before the first COP presentation. */
    if(LINNET_RESIDENT_MONITOR && !ln_display_upgrade_monitor())ln_panic(30,12,ln_display_cop_error());
#if defined(LINNET_VIDEO)
    /* Select the buffer layout before the first presentation. */
    ln_display_column_mode(ln_display_upgrade_blit());
#endif
#endif
    ln_input_snapshot(&input);ln_power_snapshot(&power);
    if(!reset_shell())ln_panic(30,6,0);
    backlight_restore();
#if defined(LINNET_NATIVE_UI) && defined(LINNET_STORAGE)
    /* Preserve the Boot panel until the saved theme has been resolved by the
     * normal asynchronous storage/settings path. No extra disk read or delay. */
    boot_theme_waiting=true;
#else
#if defined(LINNET_NATIVE_UI)
    uint32_t initial_theme_revision=ln_display_theme_revision();
#endif
    ln_shell_render(shell,ln_display_framebuffer());dirty=false;
    if(!ln_display_present())ln_panic(30,7,0);
#if defined(LINNET_NATIVE_UI)
    theme_frame_submitted(initial_theme_revision);
#endif
    uint32_t start=ln_time_us();
    while(ln_display_busy()){
        if(!ln_display_service(LINNET_DISPLAY_SERVICE_PAIRS)||ln_time_us()-start>400000u)ln_panic(30,8,0);
    }
#if defined(LINNET_NATIVE_UI)
    theme_frame_observe();
#endif
    LN_BOOT_STAMP(LN_BT_OS_FRAME);
    first_frame_ready=true;
#endif
#if defined(LINNET_AUDIO)
    /* The decoder executes from SDRAM. Its instruction/data working set needs
     * the CPU cache; scanout/audio DMA publish through uncached aliases. */
    if(!ln_core_cache_enable())ln_panic(30,11,0);
#if defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    ln_platform_cache_marker(0xca0bu);
#endif
#endif
    LN_BOOT_STAMP(LN_BT_OS_RUNTIME_END);
    ready=true;return true;
}
int ln_runtime_can_inject(uint32_t first_sequence,uint32_t count){
    return ready&&first_sequence>last_injection&&count&&count<=LINNET_EVENT_CAPACITY-executive.used;
}
int ln_runtime_inject(uint32_t kind,int32_t value,uint32_t sequence){
    if(!ln_runtime_can_inject(sequence,1)||kind<1||kind>4||
       (kind==1&&(value < -64||value>64))||(kind==2&&(value<0||value>31))||
       (kind>=3&&(value<0||value>1)))return 0;
    ln_event event={kind,ln_time_us(),generation(),value};
    if(!ln_exec_post(&executive,LN_SERVICE_INPUT,event))return 0;
    ln_wheel_motion_reset(&wheel_motion);
    last_injection=sequence;return 1;
}
void ln_runtime_step(void){
    /* Storage drain callbacks may reenter here during shutdown or Sync.
     * Once terminal cleanup starts, UI work must not reopen media. */
    if(!ready)return;
    uint32_t now=ln_time_us();uint32_t wakes=ln_core_take_wakes();
#if defined(LINNET_HARDWARE_CLICKER)
    ln_platform_click_poll(now);
#endif
#if defined(LINNET_NATIVE_UI)
    if(scene_is(LN_UI_SCENE_GAME_PLAY)){
        static uint32_t drawn_game_hud;
        if(!shell->hold&&ln_games_tick(now)){shell->dirty|=8u;dirty=true;}
        uint32_t game_hud=ln_games_hud_revision();
        if(game_hud!=drawn_game_hud){drawn_game_hud=game_hud;ln_ui_refresh(shell);dirty=true;}
        if(ln_games_playing())backlight_activity();
    }
#endif
#if defined(LINNET_AUDIO)
    ln_music_browser_set_visible(scene_is(LN_UI_SCENE_MUSIC));
#if defined(LINNET_COVERFLOW)
    ln_coverflow_visible(scene_is(LN_UI_SCENE_COVER_FLOW));
    uint32_t cover_dirty=ln_coverflow_tick(now);if(cover_dirty){shell->dirty|=cover_dirty;dirty=true;}
#endif
#endif
    /* Check fresh power before optional publications or new viewer work.
     * A locked storage callback must return to its owner to finish draining. */
    if((int32_t)(now-ln_power_next_deadline())>=0)(void)ln_power_poll(now,&power);
#if defined(LINNET_BATTERY_GUARD)
    bool battery_emergency=ln_battery_shutdown_due(&battery_guard,&power,ln_time_us());
    if(battery_emergency&&!ln_storage_busy())shutdown(false);
#endif
#if defined(LINNET_AUDIO) && !defined(LINNET_PROFILE_RELEASE)
    uint32_t timing_started=now;
#endif
#if defined(LINNET_NATIVE_UI)
    bool settings_io=true;
#if defined(LINNET_VIDEO)
    settings_io=!ln_video_active()&&!ln_video_pending();
    if(!video_resume_loaded){uint32_t raw[2];uint8_t options;if(ln_display_settings_video_get(raw)&&ln_display_settings_video_options_get(&options)){ln_video_resume_set(raw);ln_video_options_set(options);video_resume_loaded=true;}}
#endif
#if defined(LINNET_AUDIO)
    if(settings_io){
        struct ln_audio_status settings_audio;ln_audio_snapshot(&settings_audio);
        struct ln_player_status settings_player;ln_player_snapshot(&settings_player);
        settings_io=!settings_audio.playing&&(settings_player.phase==LN_PLAYER_IDLE||settings_player.phase==LN_PLAYER_PAUSE||settings_player.phase==LN_PLAYER_ERROR);
    }
#endif
#if defined(LINNET_BATTERY_GUARD)
    settings_io=settings_io&&!battery_emergency;
#endif
#if defined(LINNET_PHOTOS)
    /* Photo focus updates the resume envelope in RAM. Publish it on leaving
     * Photos, avoiding disk spin-up and settings writes during cached scrolling.
     * Shutdown still drains Photos and explicitly saves the settings record. */
    settings_io=settings_io&&!ln_photos_active();
#endif
    bool theme_presented=settings_io&&!sleeping&&theme_frame_ready();
    ln_display_settings_step_ready(settings_io&&!sleeping,theme_presented,theme_frame.revision);
    ln_games_step(now,scene_is(LN_UI_SCENE_GAME_PLAY),shell->hold!=0,settings_io&&!sleeping);
    if(ln_games_take_exit()&&scene_is(LN_UI_SCENE_GAME_PLAY)){(void)ln_ui_show(shell,"linnet.system.games");dirty=true;}
    static uint32_t game_revision;
    uint32_t next_game_revision=ln_games_hud_revision();
    if(game_revision!=next_game_revision){game_revision=next_game_revision;
        if(scene_is(LN_UI_SCENE_GAMES)||scene_is(LN_UI_SCENE_GAME_PLAY)||scene_is(LN_UI_SCENE_HOME)||scene_is(LN_UI_SCENE_EXTRAS)){
            ln_ui_refresh(shell);dirty=true;}}

#if defined(LINNET_STORAGE)
    if(boot_theme_waiting){
        if(!ln_display_settings_boot_ready()&&
           !(ln_storage_startup_complete()&&!ln_storage_prepared()))return;
#if defined(LINNET_TARGET_VIDEO5G_M25)
        if(ln_storage_prepared()&&!ln_m25_coprocessor_prepare_display())return;
#endif
        boot_theme_waiting=false;
        ln_ui_refresh(shell);dirty=true;
    }
#endif
#if defined(LINNET_PHOTOS)
    if(!photo_resume_loaded){uint8_t raw[16];if(ln_display_settings_photo_get(raw)){
        (void)ln_photos_resume_set(raw);photo_resume_loaded=true;
    }}
    ln_photos_prepare(photo_resume_loaded&&!sleeping&&scene_is(LN_UI_SCENE_HOME)&&ln_storage_prepared()&&ln_display_settings_boot_ready());
    if(!ln_photos_active()&&ln_photos_pending()&&!scene_is(LN_UI_SCENE_HOME))ln_photos_step();
#endif
    if(scene_is(LN_UI_SCENE_UPDATE)&&(int32_t)(now-update_status_at)>=0){
        uint32_t state[16];ln_storage_update_snapshot(state);
        if(ln_display_update_status(LINNET_BUILD_ID,state,ln_storage_update_history())){ln_ui_refresh(shell);dirty=true;}
        update_status_at=now+500000u;
    }
    uint32_t settings_revision=ln_display_settings_revision();
    if(display_settings_revision!=settings_revision){
        display_settings_revision=settings_revision;
        brightness=ln_preferences_brightness();
        backlight_timeout=ln_display_backlight_timeout_us();
        backlight_off=ln_display_backlight_off();
        if(!sleeping){backlight_restore();backlight_activity();}
        if(scene_is(LN_UI_SCENE_DISPLAY)||scene_is(LN_UI_SCENE_BATTERY)||scene_is(LN_UI_SCENE_SHUTDOWN)||scene_is(LN_UI_SCENE_THEMES)||scene_is(LN_UI_SCENE_HOME)||scene_is(LN_UI_SCENE_GAMES)||scene_is(LN_UI_SCENE_ABOUT)||scene_is(LN_UI_SCENE_MUSIC_SOUND)||shell->drawn_theme!=ln_ui_theme_index()){ln_ui_refresh(shell);dirty=true;}
    }
    theme_frame_request();
#endif
#if defined(LINNET_AUDIO)
    ln_audio_service(now);
#if defined(LINNET_NATIVE_UI)
    if(scene_is(LN_UI_SCENE_GAME_PLAY))ln_games_audio_step();else ln_games_audio_stop();
#endif
    if(ln_music_take_now_playing()&&(scene_is(LN_UI_SCENE_HOME)||scene_is(LN_UI_SCENE_MUSIC)||scene_is(LN_UI_SCENE_MUSIC_SEARCH)||scene_is(LN_UI_SCENE_MUSIC_QUEUE)||scene_is(LN_UI_SCENE_MUSIC_QUEUE_ACTIONS))){
        if(scene_is(LN_UI_SCENE_HOME)||scene_is(LN_UI_SCENE_MUSIC)||scene_is(LN_UI_SCENE_MUSIC_SEARCH))
            playback_from_search=scene_is(LN_UI_SCENE_MUSIC_SEARCH);
        ln_music_queue_close();
        if(ln_ui_show(shell,"linnet.system.now-playing")){retain_scene();dirty=true;}
    }
    browser_navigation();
    if(scene_is(LN_UI_SCENE_MUSIC_QUEUE)){
        uint32_t rows=ln_ui_visible_capacity(shell);if(!rows)rows=1;
        ln_music_queue_view_visible_rows(rows);
        ln_music_queue_view_window(ln_ui_first_visible(shell));
    }
    search_reconcile(true);
#if defined(LINNET_VIDEO)
    /* Video has its own presentation revision. Keep music caches untouched so
     * the first callback after leaving video reconciles every deferred change. */
    if(!video_scene())
#endif
    {
    struct ln_player_status music;ln_player_snapshot(&music);
    uint32_t current_search_revision=ln_music_search_revision(),current_library_status=ln_music_library_status();
    if(scene_is(LN_UI_SCENE_MUSIC_SEARCH)&&
       (search_revision!=current_search_revision||search_library_status!=current_library_status)){
        ln_ui_refresh_content(shell);dirty=true;
    }
    search_revision=current_search_revision;search_library_status=current_library_status;
    /* Queue rows and their notice have their own presentation dependencies.
     * History saves and playback position changes do not change this screen. */
    if(scene_is(LN_UI_SCENE_MUSIC_QUEUE)&&
       (music_queue_drawn_revision!=ln_music_queue_view_revision()||
        music_queue_drawn_notice!=ln_music_notice())){
        ln_ui_refresh(shell);dirty=true;
    }
    uint32_t revision=ln_music_library_revision()+ln_music_art_revision()+ln_music_browser_revision()+ln_music_revision()+ln_music_search_revision()+ln_music_queue_view_revision();
    bool applying=ln_music_sound_pending();
    if(revision!=music_revision||music.phase!=music_phase||music.generation!=music_generation||
       (scene_is(LN_UI_SCENE_MUSIC_SOUND)&&applying!=music_sound_applying)){
        if(scene_is(LN_UI_SCENE_MUSIC)){ln_ui_refresh_content(shell);dirty=true;}
        else if(scene_is(LN_UI_SCENE_NOW_PLAYING)||scene_is(LN_UI_SCENE_MUSIC_OPTIONS)||scene_is(LN_UI_SCENE_MUSIC_SOUND)||scene_is(LN_UI_SCENE_MUSIC_QUEUE_ACTIONS)){ln_ui_refresh(shell);dirty=true;}
        music_revision=revision;music_phase=music.phase;music_generation=music.generation;
    }
    music_sound_applying=applying;
    /* Elapsed time stays valid after long periods away from playback. */
    if(scene_is(LN_UI_SCENE_NOW_PLAYING)&&(music.phase==LN_PLAYER_PLAY||music.phase==LN_PLAYER_DRAIN)&&
       (uint32_t)(now-music_progress_at)>=250000u){
        shell->dirty|=8u;dirty=true;music_progress_at=now;
    }
    }
#endif
#if defined(LINNET_NOTES)
    ln_notes_step(shell);if(shell->dirty)dirty=true;
#endif
#if defined(LINNET_FILES)
    ln_files_step(shell);if(shell->dirty)dirty=true;
#endif
#if defined(LINNET_PHOTOS)
    /* Scene changes can also originate from development controls. Close
     * background media even when no further physical input arrives. */
    if(!photo_scene()&&ln_photos_active()){photo_save();ln_photos_close();}
    ln_photos_step();
#if defined(LINNET_VIDEO)
    if(!video_scene()){
        video_requested=false;
        if(ln_video_active()){video_save();ln_video_stop();}
    }
    if(scene_is(LN_UI_SCENE_VIDEOS))ln_video_library_load();
    ln_storage_video_browse(scene_is(LN_UI_SCENE_VIDEOS)&&!ln_video_library_empty(),false);
    uint32_t library_revision=ln_video_library_revision();
    if(library_revision!=video_library_revision){video_library_revision=library_revision;if(scene_is(LN_UI_SCENE_VIDEOS)){ln_ui_refresh(shell);dirty=true;}}
    if(video_requested&&!ln_player_busy()&&!ln_photos_pending()&&!ln_storage_busy()){
        struct ln_audio_status a;ln_audio_snapshot(&a);
        if(!a.initialized&&video_resume_loaded&&(video_from_files?ln_video_open_file(&video_file,video_selected_path):ln_video_open(video_selected_path)))video_requested=false;
    }
    ln_video_step();
    if(video_scene()&&!video_requested&&ln_video_completed()){
        video_save();
        (void)ln_ui_show(shell,"linnet.system.videos");dirty=true;
    }
    if(video_scene()){
        uint32_t revision=ln_video_revision();
        if(revision!=video_revision){video_revision=revision;ln_ui_refresh(shell);dirty=true;}
        if(ln_video_active()&&!ln_video_paused())backlight_activity();
    }
#endif
    if(photo_scene()){
        photo_navigation_update();uint32_t revision=ln_photos_revision();
        if(revision!=photo_revision){photo_revision=revision;ln_ui_refresh(shell);dirty=true;photo_save();}
    }
#endif
#if defined(LINNET_STORAGE) && !defined(LINNET_NATIVE_UI)
    uint32_t previous_buttons=input.buttons;
#endif
    bool previous_touch=input.touched;
    bool poll_input=(wakes&(LINNET_WAKE_INPUT|LINNET_WAKE_GPIO|LINNET_WAKE_TIMER))!=0;
#if LINNET_RUNTIME_POWER_POLICY
    /* A due sample must advance even before the first timer/wheel IRQ. An
     * expired unsampled deadline would otherwise prevent the first WFI. */
    poll_input=poll_input||(int32_t)(ln_time_us()-ln_input_deadline(now))>=0;
#endif
    if(poll_input&&ln_input_poll(&input)){
#if LINNET_RUNTIME_POWER_POLICY
        if(!ln_power_policy_fast(true))ln_panic(30,14,ln_core_clock_failure());
#endif
        /* Count contact and sub-menu-step motion, but not idle sensor packets. */
        if(!input.hold&&((input.touched&&!previous_touch)||input.wheel_delta))backlight_activity();
#if defined(LINNET_STORAGE) && !defined(LINNET_NATIVE_UI)
        if(!ln_storage_busy()&&!input.hold&&shell->scene==4&&input.buttons==LINNET_BUTTON_SELECT&&!(previous_buttons&LINNET_BUTTON_SELECT)){
            if(!ln_storage_disk_mode())ln_panic(34,1,0);
        }
#endif
#if defined(LINNET_NATIVE_UI)
        if(!input.hold&&input.wheel_delta)shutdown_activity();
#endif
        if(sleeping)(void)wake();
        if(!(override_mask&1u))apply(LN_INPUT_HOLD,input.hold);
        if(!(override_mask&2u))apply(LN_INPUT_HEADPHONE,input.headphones);
        apply(LN_INPUT_BUTTON,(int32_t)buttons(input.buttons));
#if defined(LINNET_INPUT_REMOTE) && defined(LINNET_AUDIO)
        /* Headset media keys bypass menu navigation and the physical Play
         * shutdown gesture. They remain usable with the Hold switch locked. */
        if(input.remote_actions||input.remote_volume){
#if defined(LINNET_NATIVE_UI)
            shutdown_activity();
#endif
            if(input.remote_volume)(void)ln_music_volume(input.remote_volume);
#if defined(LINNET_VIDEO)
            if(ln_video_active()){
                if(input.remote_actions&LINNET_REMOTE_PLAY_PAUSE)ln_video_pause(!ln_video_paused());
                if(input.remote_actions&LINNET_REMOTE_NEXT)(void)ln_video_seek(10);
                if(input.remote_actions&LINNET_REMOTE_PREVIOUS)(void)ln_video_seek(-10);
            }else
#endif
            {
                if(input.remote_actions&LINNET_REMOTE_PLAY_PAUSE)ln_music_toggle();
                if(input.remote_actions&(LINNET_REMOTE_NEXT|LINNET_REMOTE_PREVIOUS))
                    (void)ln_music_next((input.remote_actions&LINNET_REMOTE_PREVIOUS)!=0);
            }
            ln_ui_refresh(shell);dirty=true;
        }
#endif
        uint32_t epoch=0,count=0;
#if defined(LINNET_NATIVE_UI)
        epoch=shell->scene_epoch;count=ln_ui_focused_count(shell);
#if defined(LINNET_COVERFLOW)
        if(scene_is(LN_UI_SCENE_COVER_FLOW))count=ln_coverflow_count();
#endif
#endif
        if(wheel_scene!=shell->scene||wheel_epoch!=epoch||input.buttons){
            ln_wheel_motion_reset(&wheel_motion);
            wheel_scene=shell->scene;wheel_epoch=epoch;
        }
        int32_t steps=ln_wheel_motion_step(&wheel_motion,input.wheel_delta,
            input.touched,input.hold,count,
#if defined(LINNET_COVERFLOW)
            scene_is(LN_UI_SCENE_COVER_FLOW),
#else
            false,
#endif
            ln_time_us());
        if(steps)apply(LN_INPUT_WHEEL,steps);
    }
    if(!input.hold&&input.buttons==LINNET_BUTTON_PLAY){
        if(!shutdown_tracking){shutdown_tracking=true;shutdown_deadline=ln_time_us()+LINNET_SHUTDOWN_HOLD_US;}
        else if((int32_t)(ln_time_us()-shutdown_deadline)>=0){
#if defined(LINNET_STORAGE)
            if(!ln_storage_busy())shutdown(true);
#else
            shutdown(true);
#endif
        }
    }else shutdown_tracking=false;
#if defined(LINNET_NATIVE_UI)
    if(ln_ui_status_refresh(shell))dirty=true;
#endif
#if defined(LINNET_BATTERY_GUARD)
    if(ln_battery_shutdown_due(&battery_guard,&power,ln_time_us())&&!ln_storage_busy())shutdown(false);
#endif
    ln_exec_due(&executive,now);
    uint32_t owner;ln_event event;
    for(uint32_t budget=0;budget<32&&ln_exec_take(&executive,ln_time_us(),&owner,&event);++budget){
        if(event.generation!=generation())continue;
        if(owner==LN_SERVICE_INPUT){
            if(event.kind>=3){uint32_t bit=1u<<(event.kind-3u);override_mask|=bit;
                if(event.value)synthetic_flags|=bit;else synthetic_flags&=~bit;}
            apply(event.kind,event.value);
        }else if(owner==LN_SERVICE_POWER)(void)wake();
    }
#if defined(LINNET_AUDIO)
    /* Accelerated and synthetic wheel bursts can update selection many times
     * per callback. Bind only the final page that this frame will display. */
    if(music_window_pending){music_window_pending=false;browser_navigation();}
    if(scene_is(LN_UI_SCENE_MUSIC_QUEUE)){
        /* Input bursts settle the viewport before its first frame. Preparing
         * resident labels never takes the asynchronous query owner's slot. */
        ln_music_queue_view_visible_rows(ln_ui_visible_capacity(shell));
        ln_music_queue_view_window(ln_ui_first_visible(shell));
        if(ln_music_queue_view_prepare()){ln_ui_refresh_content(shell);dirty=true;}
    }
    if(scene_is(LN_UI_SCENE_MUSIC_SEARCH)){
        search_reconcile(false);
        if(ln_music_search_prepare(ln_ui_visible_capacity(shell))){ln_ui_refresh_content(shell);dirty=true;}
    }
#endif
#if defined(LINNET_NATIVE_UI)
#if defined(LINNET_AUDIO)
    bool sleep_timer_due=ln_music_sleep_step(ln_time_us());
    uint32_t sleep_revision=ln_music_sleep_revision();
    if(sleep_revision!=sleep_timer_revision){
        sleep_timer_revision=sleep_revision;
        if(scene_is(LN_UI_SCENE_SLEEP_TIMER)){ln_ui_refresh(shell);dirty=true;}
    }
    if(sleep_timer_due&&!ln_storage_busy())shutdown(true);
#endif
    uint32_t shutdown_now=ln_time_us(),timeout=ln_auto_shutdown_seconds(shell->headphones!=0);
    if(timeout!=auto_shutdown_timeout||auto_shutdown_headphones!=(shell->headphones!=0)){
        shutdown_idle_us=0;auto_shutdown_timeout=timeout;auto_shutdown_headphones=shell->headphones!=0;
    }else if(timeout)shutdown_idle_us+=(uint32_t)(shutdown_now-shutdown_sample_at);
    shutdown_sample_at=shutdown_now;
    if(timeout&&shutdown_idle_us>=(uint64_t)timeout*1000000u&&!ln_storage_busy())shutdown(true);
#endif
    if(sleeping&&wake_at&&(int32_t)(now-wake_at)>=0)(void)wake();
    /* Identical idle policy on battery, USB and AC; background work is not input.
     * Latch expiry so the wrapping microsecond clock cannot relight the panel. */
    if(!sleeping&&!backlight_idle&&backlight_timeout&&
       (int32_t)(ln_time_us()-backlight_deadline)>=0){
        backlight_idle=true;
        ln_power_backlight(false,(uint8_t)((brightness*255u+50u)/100u));
    }
#if defined(LINNET_NATIVE_UI)
    text_scroll_at=(!sleeping&&!backlight_idle)?ln_ui_scroll_tick(shell,now):UINT32_MAX;
    palette_at=(!sleeping&&!backlight_idle)?ln_ui_palette_tick(shell,now):UINT32_MAX;
    if(shell->dirty)dirty=true;
#if defined(LINNET_AUDIO)
    if(shell->action==LINNET_UI_ACTION_SHUFFLE_ALL){
        (void)ln_shell_take_action(shell);
        if(!ln_music_browser_shuffle_all())(void)ln_ui_show(shell,"linnet.system.music");
        ln_ui_refresh(shell);dirty=true;
    }
    if(shell->action==LINNET_UI_ACTION_SHUFFLE||shell->action==LINNET_UI_ACTION_REPEAT){
        uint32_t action=ln_shell_take_action(shell);
        (void)ln_music_option_select(action==LINNET_UI_ACTION_SHUFFLE?0u:1u);
        ln_ui_refresh(shell);dirty=true;
    }
#endif
    if(shell->action==LINNET_UI_ACTION_SYNC&&!ln_storage_busy()){
        (void)ln_shell_take_action(shell);
        restart(true);
    }
#endif
    if(!ln_display_service(LINNET_DISPLAY_SERVICE_PAIRS))ln_panic(32,0,0);
#if defined(LINNET_NATIVE_UI)
    theme_frame_observe();
    if(!first_frame_ready&&theme_frame_ready()){
        first_frame_ready=true;
        LN_BOOT_STAMP(LN_BT_OS_FRAME);
    }
    clock_save_step();
#endif
#if LINNET_DISPLAY_PIPELINE
    bool render_ready=!sleeping&&ln_display_can_render();
#if defined(LINNET_NATIVE_UI)
    /* Confirm the first frame of a theme before overlapping later frames. */
    if(render_ready&&ln_display_busy()&&!theme_frame_ready())render_ready=false;
#endif
#else
    bool render_ready=!sleeping&&!ln_display_busy();
#endif
#if defined(LINNET_AUDIO) && LINNET_TARGET_NUMBER == 25u
    uint32_t search_frame_due;
    if(dirty&&search_frame_deferred(ln_time_us(),&search_frame_due))render_ready=false;
#endif
#if LINNET_AUDIO_RENDER_RESERVE && defined(LINNET_AUDIO)
    if(render_ready&&ln_player_busy()){
        struct ln_audio_status audio;ln_audio_snapshot(&audio);
        /* Drawing and PIO share the music decoder's CPU. Keep a PCM reserve
         * before starting another frame, including during sustained wheel input.
         * Video owns the audio HAL separately and must be allowed to render
         * its offered frame while its PCM queue drains. */
        if(audio.playing&&!audio.paused&&audio.queued<LINNET_AUDIO_RING_FRAMES/2u){
            struct ln_player_status player;ln_player_snapshot(&player);
            if(player.phase!=LN_PLAYER_DRAIN)render_ready=false;
        }
    }
#endif
    if(render_ready){
#if defined(LINNET_NATIVE_UI)
        uint32_t rendered_theme_revision=ln_display_theme_revision();
#endif
        if(dirty){
#if LINNET_RUNTIME_POWER_POLICY
            if(!ln_power_policy_fast(false))ln_panic(30,14,ln_core_clock_failure());
#endif
#if !defined(LINNET_PROFILE_RELEASE)
            struct rnd_span render_span=rnd_enter();
#endif
            uint16_t *render_pixels=NULL;
#if defined(LINNET_SOC_S5L8702) && defined(LINNET_NATIVE_UI)
            /* A full repaint overwrites every pixel; keep the stale draw buffer
             * out of the path instead of copying the previous frame first. */
            if(!shell->rendered||(shell->dirty&1u)||
               shell->drawn_theme!=ln_ui_theme_index())
                render_pixels=ln_display_direct_framebuffer();
#endif
#if defined(LINNET_VIDEO) && defined(LINNET_NATIVE_UI)
            if(!render_pixels&&video_scene()){
                const struct ln_ui_asset *asset=ln_video_asset();
                const struct ln_ui_scene *c=&ln_ui_scenes[shell->scene];
                if(asset&&asset->pixels&&asset->width&&asset->width<=LINNET_VIDEO_WIDTH&&
                   asset->height&&asset->height<=LINNET_VIDEO_HEIGHT&&c->nodes){
                    for(uint32_t i=0;i<c->count&&i<1024u;++i){
                        const struct ln_ui_node *n=&c->nodes[i];
                        if(n->kind==LN_UI_VIDEO_SURFACE&&!(n->flags&2u)&&
                           n->x==0&&n->y==0&&n->w==(int32_t)LINNET_UI_WIDTH&&n->h==(int32_t)LINNET_UI_HEIGHT){
                            render_pixels=ln_display_direct_framebuffer();
                            if(render_pixels)shell->dirty|=1u;
                            break;
                        }
                    }
                }
            }
#endif
            if(!render_pixels)render_pixels=ln_display_framebuffer();
            ln_shell_render(shell,render_pixels);dirty=false;
#if defined(LINNET_AUDIO)
            if(scene_is(LN_UI_SCENE_MUSIC_QUEUE)){
                music_queue_drawn_revision=ln_music_queue_view_revision();
                music_queue_drawn_notice=ln_music_notice();
            }
            if(scene_is(LN_UI_SCENE_MUSIC_SEARCH)){
                search_revision=ln_music_search_revision();search_library_status=ln_music_library_status();
#if LINNET_TARGET_NUMBER == 25u
                ln_music_search_cancel_frame_defer();
#endif
            }
#endif
#if !defined(LINNET_PROFILE_RELEASE)
            rnd_rendered(&render_span);
#endif
            bool presented=ln_display_present();
#if !defined(LINNET_PROFILE_RELEASE)
            rnd_leave(&render_span,presented);
#endif
#if defined(LINNET_NATIVE_UI)
            if(presented)theme_frame_submitted(rendered_theme_revision);
            if(presented&&scene_is(LN_UI_SCENE_CLOCK_EDIT)&&ln_clock_editor_pending())clock_save_presented=true;
#endif
#if defined(LINNET_VIDEO)
            if(presented&&video_scene())ln_video_presented();
#else
            (void)presented;
#endif
        }
        else if(animation_left&&(int32_t)(now-animation_at)>=0){
#if defined(LINNET_NATIVE_UI)
            uint32_t paints=shell->paint_count;
#endif
            ln_shell_animate(shell,ln_display_framebuffer(),shell->animation+1u);
            bool presented=ln_display_present();
#if defined(LINNET_NATIVE_UI)
            if(presented)theme_frame_submitted(rendered_theme_revision);
            /* A scene without animation effects still consumes its deadline. */
            if(shell->paint_count==paints)presented=true;
#endif
            if(presented){--animation_left;animation_at=now+33333u;}
        }
    }
#if LINNET_TARGET_NUMBER==25u
    /* A long media/render span must not defer every ADC stage to another
     * complete frame. Offer one due-gated transfer after the frame as well. */
    now=ln_time_us();
    if(ready&&(int32_t)(now-ln_power_next_deadline())>=0){
        bool changed=ln_power_poll(now,&power);
#if defined(LINNET_BATTERY_GUARD)
        if(ln_battery_shutdown_due(&battery_guard,&power,ln_time_us())&&!ln_storage_busy())shutdown(false);
#endif
        if(changed&&ln_ui_status_refresh(shell))dirty=true;
    }
#endif
#if defined(LINNET_AUDIO) && !defined(LINNET_PROFILE_RELEASE)
    ln_player_timing_runtime(ln_time_us()-timing_started);
#endif
}
void ln_runtime_idle(void){
#if !defined(LINNET_PROFILE_RELEASE)
    if(ln_usb_rx_pending())return;
#endif
    uint32_t now=ln_time_us();
    if(executive.used)return;
    bool display_wait=false;uint32_t display_due=now;
#if LINNET_DISPLAY_PIPELINE
    display_wait=ln_display_wait_deadline(&display_due);
#endif
    bool search_wait=false;uint32_t search_due=now;
#if defined(LINNET_AUDIO) && LINNET_TARGET_NUMBER == 25u
    search_wait=dirty&&search_frame_deferred(now,&search_due);
#endif
    if(dirty&&!display_wait&&!search_wait)return;
    struct ln_display_metrics display;ln_display_get_metrics(&display);
    /* CPU fallback must service its bounded worker; a live COP owns the LCD. */
    if(ln_display_busy()&&!display.cop_active&&!display_wait)return;
    uint32_t next=now+200000u;
#if LINNET_RUNTIME_POWER_POLICY
    uint32_t input_due=ln_input_deadline(now);if((int32_t)(input_due-next)<0)next=input_due;
#endif
    if(search_wait&&(int32_t)(search_due-next)<0)next=search_due;
#if defined(LINNET_HARDWARE_CLICKER)
    uint32_t click_due;
    if(ln_platform_click_deadline(&click_due)&&(int32_t)(click_due-next)<0)next=click_due;
#endif
#if defined(LINNET_NATIVE_UI)
    if(true){
        uint32_t game_due=ln_games_deadline(now);
        if(game_due!=UINT32_MAX&&(int32_t)(game_due-next)<0)next=game_due;
    }
#endif
    if(display_wait&&(int32_t)(display_due-next)<0)next=display_due;
#if defined(LINNET_COVERFLOW)
    uint32_t cover_due=ln_coverflow_deadline();if(cover_due!=UINT32_MAX&&(int32_t)(cover_due-next)<0)next=cover_due;
#endif
#if defined(LINNET_NATIVE_UI)
    if(text_scroll_at!=UINT32_MAX&&(int32_t)(text_scroll_at-next)<0)next=text_scroll_at;
    if(palette_at!=UINT32_MAX&&(int32_t)(palette_at-next)<0)next=palette_at;
#endif
#if defined(LINNET_VIDEO)
    uint32_t video_due=ln_video_deadline(now);if((int32_t)(video_due-next)<0)next=video_due;
#endif /* independent watchdog progress lease */
#if defined(LINNET_PHOTOS)
    uint32_t photo_due=ln_photos_deadline(now);if((int32_t)(photo_due-next)<0)next=photo_due;
#endif
#if defined(LINNET_FILES)
    uint32_t files_due=ln_files_deadline(now);if((int32_t)(files_due-next)<0)next=files_due;
#endif
    if(!sleeping&&!backlight_idle&&backlight_timeout&&(int32_t)(backlight_deadline-next)<0)next=backlight_deadline;
#if defined(LINNET_AUDIO)
    uint32_t music_due=ln_music_deadline(now);if((int32_t)(music_due-next)<0)next=music_due;
    uint32_t audio_due=ln_audio_deadline(now);if(audio_due!=UINT32_MAX&&(int32_t)(audio_due-next)<0)next=audio_due;
#endif
    if(shutdown_tracking&&(int32_t)(shutdown_deadline-next)<0)next=shutdown_deadline;
    uint32_t power_due=ln_power_next_deadline();if((int32_t)(power_due-next)<0)next=power_due;
    if(animation_left&&(int32_t)(animation_at-next)<0)next=animation_at;
    if(sleeping&&wake_at&&(int32_t)(wake_at-next)<0)next=wake_at;
    if(ln_display_busy()&&(int32_t)((now+1000u)-next)<0)next=now+1000u;
#if defined(LINNET_STORAGE)
    uint32_t storage_due=ln_storage_next_deadline(now);
    if((int32_t)(storage_due-next)<0)next=storage_due;
#if !defined(LINNET_PROFILE_RELEASE)
    ln_storage_schedule_observe(now);
#endif
    if(ln_storage_busy()){
        uint32_t wait_due;
        if(!ln_storage_idle_wait(now,&wait_due))return;
        if((int32_t)(wait_due-next)<0)next=wait_due;
    }
#endif
    next=ln_exec_next(&executive,now,next);
#if LINNET_IDLE_CLOCK_HZ
    /* Short sampling waits do not repay PLL switching. Never change the
     * audio clock or storage timing during an active transfer. */
    bool clock_idle=(int32_t)(next-ln_time_us())>=20000;
#if defined(LINNET_AUDIO)
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    clock_idle=clock_idle&&!audio.initialized&&!ln_player_busy();
#endif
#if defined(LINNET_STORAGE)
    clock_idle=clock_idle&&!ln_storage_busy();
#endif
    if(clock_idle&&!idle_clock_failures){
        bool lowered=ln_core_clock_set(LINNET_IDLE_CLOCK_HZ);
#if !defined(LINNET_PROFILE_RELEASE)
        uint32_t began=ln_time_us();
#endif
        if(lowered){ln_core_idle_until(next);
#if !defined(LINNET_PROFILE_RELEASE)
            ++idle_clock_cycles;idle_clock_us+=ln_time_us()-began;
#endif
        }
        else ++idle_clock_failures;
        /* A partial transition must also restore the known working clock. */
        if(!ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ))ln_panic(30,14,ln_core_clock_failure());
        if(lowered)return;
    }
#endif
    ln_core_idle_until(next);
}
int ln_runtime_control(uint32_t command,uint32_t argument){
    if(!ready)return 0;
    switch(command){
    case LN_CONTROL_SLEEP:
#if defined(LINNET_NATIVE_UI)
        if(ln_games_pending())return 0;
#endif
#if defined(LINNET_NOTES)
        if(ln_notes_pending())return 0;
#endif
#if defined(LINNET_FILES)
        if(ln_files_pending())return 0;
#endif
#if defined(LINNET_NATIVE_UI)
        if(ln_display_settings_pending())return 0;
#endif
#if defined(LINNET_VIDEO)
        if(video_requested||ln_video_active()||ln_video_pending())return 0;
#endif
#if defined(LINNET_PHOTOS)
        if(ln_photos_active()||ln_photos_pending())return 0;
#endif
#if defined(LINNET_AUDIO)
        {struct ln_audio_status audio;ln_audio_snapshot(&audio);if(audio.initialized||ln_player_busy())return 0;}
#endif
        if(argument<50u||argument>1000u||sleeping||ln_display_busy())return 0;
        #if defined(LINNET_STORAGE)
        if(!ln_storage_quiesce())return 0;
        #endif
        if(!ln_power_sleep()){
#if defined(LINNET_STORAGE)
            (void)ln_storage_resume();
#endif
            return 0;
        }
        sleeping=true;++sleep_count;wake_at=ln_time_us()+argument*1000u;
        if(!ln_exec_schedule(&executive,LN_SERVICE_POWER,1,wake_at,(ln_event){0,wake_at,generation(),0})){
            (void)ln_power_wake();
#if defined(LINNET_STORAGE)
            (void)ln_storage_resume();
#endif
            sleeping=false;wake_at=0;--sleep_count;return 0;
        }
        return 1;
    case LN_CONTROL_WAKE:ln_exec_cancel(&executive,LN_SERVICE_POWER,1);return wake();
    case LN_CONTROL_BACKLIGHT:
        if(argument>100u)return 0;brightness=argument;
        backlight_restore();return 1;
    case LN_CONTROL_COP_RESET:
        if(!ln_display_reset())return 0;dirty=true;return 1;
    case LN_CONTROL_ANIMATION:
        if(argument<1u||argument>120u||animation_left)return 0;
        animation_left=argument;animation_at=ln_time_us();return 1;
    case LN_CONTROL_CLEAR_SYNTHETIC:
        override_mask=synthetic_flags=0;apply(LN_INPUT_HOLD,input.hold);apply(LN_INPUT_HEADPHONE,input.headphones);return 1;
    case LN_CONTROL_RESET_SHELL:
        if(executive.used||animation_left||sleeping)return 0;return reset_shell();
    case LN_CONTROL_DISPLAY_DIAGNOSTIC:
        return ln_display_diagnostic(argument);
    default:return 0;
    }
}
uint32_t ln_runtime_state(uint32_t*out,uint32_t capacity){
    if(!out||capacity<32u)return 0;
    struct ln_display_metrics d;struct ln_core_status c;
    ln_display_get_metrics(&d);ln_core_snapshot(&c);ln_input_snapshot(&input);ln_power_snapshot(&power);
    uint32_t words[32]={0x54414c50u,generation(),(ready?1u:0u)|(sleeping?2u:0u)|(d.cop_active?4u:0u)|(input.packets?8u:0u)|(shutdown_tracking?16u:0u)|((power.backlight_on?brightness:0u)<<8),
        shell?shell->selected:0,shell?shell->scene:0,executive.accepted,executive.processed,executive.dropped,executive.max_latency,
        executive.high,executive.deadline_high,scene_arena.high,d.frames_submitted,d.frames_completed,d.max_present_us,d.head,d.tail,d.resets,
        sleep_count,wake_count,c.idle_us,c.idle_count,buttons(input.buttons),input.hold,input.headphones,synthetic_flags,last_injection,
        shell?shell->animation:0,c.clock_hz,power.battery_mv,power.flags,c.irq_max_us};
    for(uint32_t i=0;i<32;++i)out[i]=words[i];return 32;
}
const uint16_t*ln_runtime_framebuffer(void){return ln_display_snapshot();}
uint32_t ln_runtime_ui_state(uint32_t *out,uint32_t capacity){
#if defined(LINNET_NATIVE_UI)
    if(!ready||!out||capacity<16u)return 0;
    uint32_t words[16]={0x54534955u,0,shell->scene,shell->selected,shell->scene_epoch,
        shell->transitions,shell->paint_count,shell->painted_pixels,shell->visible_items,
        shell->dirty,shell->action,scene_arena.used,scene_arena.high,scene_arena.generation,
        generation(),ln_ui_scene_count};
    for(uint32_t i=0;i<16u;++i)out[i]=words[i];return 16;
#else
    (void)out;(void)capacity;return 0;
#endif
}
