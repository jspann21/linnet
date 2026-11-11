#if defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE)
#include "boot/include/sha256.h"
#endif
#include "hal/api/platform.h"
#include "boot/include/linnet_boot.h"
#include "linnet_build.h"
#if defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_IMAGE_BOOT) && defined(LINNET_PROFILE_RELEASE)
#include "hal/s5l8702/boot_state.h"
#endif
#if defined(LINNET_IMAGE_BOOT) && (LINNET_BOOT_APPLE_FALLBACK || LINNET_BOOT_SELECTOR_ROCKBOX)
#include "hal/api/input.h"
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_BOOT)
#include "hal/api/core.h"
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
#include "kernel/runtime.h"
#include "hal/api/core.h"
#include "hal/api/display.h"
#include "hal/api/usb.h"
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#include "kernel/development.h"
#endif
#if defined(LINNET_STORAGE)
#include "services/storage/storage.h"
#include "hal/api/core.h"
#endif
static const uint8_t ln_build_id[32] = LINNET_BUILD_ID_BYTES;
#if defined(LINNET_AUDIO) && !defined(LINNET_PROFILE_RELEASE)
#include "services/music/player.h"
#define LN_TIMING_BEGIN(name) uint32_t name=ln_time_us()
#define LN_TIMING_END(name,stage) ln_player_timing_stage(stage,ln_time_us()-name)
#else
#define LN_TIMING_BEGIN(name) ((void)0)
#define LN_TIMING_END(name,stage) ((void)0)
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
#define LN_CACHE_FIRST_MARK(phase) do { if(cache_first_cycle) \
    ln_platform_cache_marker(phase); } while(0)
#else
#define LN_CACHE_FIRST_MARK(phase) do { } while(0)
#endif

#if !defined(LINNET_PROFILE_RELEASE)
static volatile uint8_t ln_crash_mailbox[LINNET_CRASH_MAILBOX_SIZE]
    __attribute__((section(".noinit.crash"), used));
#endif

_Noreturn void ln_panic(uint32_t reason, uint32_t pc, uint32_t status)
{
    uint32_t masked_status;
    __asm__ volatile("mrs %0, cpsr\n\torr %0, %0, #0xc0\n\tmsr cpsr_c, %0"
                     : "=&r"(masked_status) :: "memory");
#if !defined(LINNET_PROFILE_RELEASE)
    struct ln_crash_record record;
    uint8_t encoded[LINNET_CRASH_MAILBOX_SIZE];
    record.sequence = 0; /* Persistent boot sequence is not integrated yet. */
    record.reason = reason;
    for (unsigned i = 0; i < 16; ++i) record.registers[i] = 0;
    record.registers[15] = pc;
    record.cpsr = status;
    record.flags = 1; /* Bit 0: partial capture; only fault PC and CPSR valid. */
    for (unsigned i = 0; i < sizeof record.build_id; ++i)
        record.build_id[i] = ln_build_id[i];
    record.boot_generation = ln_recovery_generation();
    record.active_service = 0;
    record.last_event = 0;
    if (ln_crash_encode(&record, encoded, sizeof encoded)) {
        volatile uint8_t *mailbox=ln_crash_mailbox;
#if defined(LINNET_AUDIO)
        mailbox=(volatile uint8_t *)ln_platform_uncached_address((uintptr_t)mailbox);
#endif
        for (unsigned i = 0; i < 4; ++i) mailbox[i] = 0;
        for (unsigned i = 4; i < sizeof encoded; ++i) mailbox[i] = encoded[i];
        __asm__ volatile("" ::: "memory");
        for (unsigned i = 0; i < 4; ++i) mailbox[i] = encoded[i];
    }
#else
    (void)pc;
    (void)status;
#endif
#if defined(LINNET_PROFILE_RELEASE)
#if defined(LINNET_PLATFORM_CORE) && (defined(LINNET_IMAGE_OS) || defined(LINNET_STORAGE))
    ln_core_watchdog_arm();
#endif
    bool fault_visible=false;
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
    if(ln_display_quiesce())fault_visible=ln_lcd_fault(reason,ln_build_id);
#else
    fault_visible=ln_lcd_fault(reason,ln_build_id);
#endif
#if defined(LINNET_PLATFORM_CORE) && (defined(LINNET_IMAGE_OS) || defined(LINNET_STORAGE))
    /* Keep a successful field display readable for eight seconds. Only finite
     * elapsed-time progress renews the watchdog; a stalled LCD/CPU still resets.
     * The instruction budget also ends the dwell if the timer stops. */
    if(fault_visible){uint32_t started=ln_time_us(),renewed=started;
        for(uint32_t budget=80000000u;budget;--budget){uint32_t now=ln_time_us();
            if(now-started>=8000000u)break;
            if(now-renewed>=100000u){ln_core_watchdog_feed();renewed=now;}
        }
    }
#else
    (void)fault_visible;
#endif
#elif defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
    if (ln_display_quiesce()) (void)ln_lcd_fill(0xf800u);
#else
    (void)ln_lcd_fill(0xf800u);
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    ln_recovery_return(reason);
#elif defined(LINNET_PLATFORM_CORE) && (defined(LINNET_IMAGE_OS) || defined(LINNET_STORAGE))
    /* Terminal faults, including a rejected PMU shutdown, must still reset
     * when no COP monitor is available. Rearm and unmask the reset FIQ. */
    ln_core_watchdog_arm();
#endif
    for (;;) { __asm__ volatile("nop"); }
}

_Noreturn void ln_main(void)
{
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t c_entry=ln_time_us();
#endif
#if defined(LINNET_OS_CRC_IRAM)
    /* Startup copied the linked IRAM image. Verify it before recovery's first
     * snapshot checksum and before interrupts or other CRC users are active. */
    (void)ln_crc32_os_iram_init();
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#if defined(LINNET_IMAGE_BOOT)
    if (!ln_recovery_init(1)) ln_panic(15,0,0);
    ln_recovery_timeline_begin(1,c_entry);
#else
    if (!ln_recovery_init(0)) ln_panic(15,0,0);
    ln_recovery_timeline_begin(0,c_entry);
#endif
#endif
    ln_platform_quiesce();
    if (!ln_platform_dma_idle()) ln_panic(16,0,0);
    bool audio_idle = ln_platform_audio_idle();
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_BOOT)
    /* Keep the resident anchor available after a failed boost: recovery
     * increments generation before restoring Boot, which skips this attempt.
     * Release retains its existing fault display and watchdog reset path. */
#if !defined(LINNET_PROFILE_RELEASE)
    ln_recovery_timeline_clock(5u);
    if(ln_recovery_generation()==0u)
#endif
    {
        uint32_t clock_skip=audio_idle?ln_platform_boot_clock_ready():6u;
#if !defined(LINNET_PROFILE_RELEASE)
        if(clock_skip)ln_recovery_timeline_clock(clock_skip);
#endif
        if(!clock_skip){
#if !defined(LINNET_PROFILE_RELEASE)
            ln_recovery_timeline_clock(2u);
#endif
            bool clock_ready=ln_core_clock_set(LINNET_ACTIVE_CLOCK_HZ);
#if !defined(LINNET_PROFILE_RELEASE)
            ln_recovery_timeline_clock(clock_ready?3u:4u);
#endif
            if(!clock_ready)ln_panic(30,13,ln_core_clock_failure());
#if defined(LINNET_STORAGE)
            int hash_iram=ln_sha256_boot_iram_init();
#if !defined(LINNET_PROFILE_RELEASE)
            if(hash_iram)ln_recovery_timeline_hash_iram();
#else
            (void)hash_iram;
#endif
#endif
        }
    }
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
    bool platform_ready = ln_runtime_init();
    if (!platform_ready) ln_panic(30,0,0);
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    ln_platform_cache_marker(0xca0cu);
#endif
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    if (!ln_development_init()) ln_panic(17,0,0);
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    ln_platform_cache_marker(0xca0du);
#endif
    ln_recovery_arm();
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    ln_platform_cache_marker(0xca0eu);
#endif
#if LINNET_PROBE_CRASH
    __asm__ volatile(".word 0xe7f000f0");
#endif
#if LINNET_PROBE_HANG
    for (;;) { __asm__ volatile("nop"); }
#endif
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
    bool first_frame = ln_runtime_first_frame_ready();
#elif defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK
#if LINNET_BOOT_MENU_QUERY_BEFORE_INPUT
    bool initial_menu=ln_input_boot_menu_held();
#endif
    if(!ln_input_init())ln_panic(30,3,0);
#if LINNET_BOOT_MENU_QUERY_AFTER_INPUT
    bool initial_menu=ln_input_boot_menu_held();
#elif !LINNET_BOOT_MENU_QUERY_BEFORE_INPUT
    bool initial_menu=false;
#endif
    bool boot_decided=false,menu_open=false,menu_holding=false;
#if defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_PROFILE_RELEASE)
    bool setup_disk=ln_s5l_boot_mode()==LN_S5L_BOOT_ATTEMPT_DISK;
    if(setup_disk){boot_decided=true;ln_s5l_boot_request(LN_S5L_BOOT_ATTEMPT);}
#endif
    uint32_t menu_started=0,menu_hold_started=0,menu_activity=0;
    uint32_t menu_options=0,menu_selected=1u,menu_buttons=0,menu_last_step=0;
    int32_t menu_wheel_accum=0;
#if LINNET_BOOT_MENU_REDRAW_US
    uint32_t menu_redraw_due=0;
#endif
    bool first_frame = ln_lcd_boot();
#elif defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_SELECTOR_ROCKBOX
    if(!ln_input_init())ln_panic(30,3,0);
    bool boot_decided=false,menu_open=false,menu_holding=false;
    uint32_t menu_started=0,menu_hold_started=0,menu_activity=0;
    uint32_t menu_options=0,menu_selected=1u,menu_buttons=0,menu_last_step=0;
    int32_t menu_wheel_accum=0;
    bool first_frame = ln_lcd_boot();
#elif defined(LINNET_IMAGE_BOOT)
    bool first_frame = ln_lcd_boot();
#else
    bool first_frame = ln_lcd_fill(0x07e0u);
    if (!first_frame) ln_panic(8,0,0);
#endif
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t readiness = LINNET_BOOT_HEALTH_MEMORY | LINNET_BOOT_HEALTH_SERVICES |
        LINNET_BOOT_HEALTH_STORAGE_RESOLVED;
#if defined(LINNET_STORAGE)
    readiness &= ~LINNET_BOOT_HEALTH_STORAGE_RESOLVED;
#endif
    if (first_frame) readiness |= LINNET_BOOT_HEALTH_FIRST_FRAME;
    /* Storage-disabled configurations cannot publish readiness. Codec power-down must
     * succeed before the audio-idle condition can be published. */
    if (audio_idle) readiness |= LINNET_BOOT_HEALTH_AUDIO_IDLE;
    ln_development_health(readiness);
#if !defined(LINNET_IMAGE_BOOT)
    if (readiness == LINNET_BOOT_HEALTH_ALL) ln_recovery_mark_healthy();
#endif
#else
    (void)audio_idle;
    (void)first_frame;
#if defined(LINNET_PLATFORM_CORE) && (defined(LINNET_IMAGE_OS) || defined(LINNET_STORAGE))
    ln_core_watchdog_arm();
#endif
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    bool cache_first_cycle=true;
#endif
    #if defined(LINNET_STORAGE)
    ln_storage_init();
    LN_CACHE_FIRST_MARK(0xca10u);
    #endif
#if defined(LINNET_AUDIO)
    extern void ln_music_init(void);ln_music_init();
    LN_CACHE_FIRST_MARK(0xca11u);
#endif
    for (;;) {
#if LINNET_RUNTIME_POWER_POLICY && defined(LINNET_IMAGE_OS)
        ln_runtime_power_begin();
#endif
#if defined(LINNET_IMAGE_BOOT) && (LINNET_BOOT_APPLE_FALLBACK || LINNET_BOOT_SELECTOR_ROCKBOX)
        if(!boot_decided){
            struct ln_input_state input;
            bool changed=ln_input_poll(&input);
            ln_input_snapshot(&input);
            uint32_t now=ln_time_us();
            if(!menu_started)menu_started=now;
            if(!menu_open){
                /* On 4G optical wheels, a button held through reset first
                 * appears in the boot-only keypad query. */
#if LINNET_BOOT_APPLE_FALLBACK
#if LINNET_BOOT_HOLD_SELECTOR
                bool held=input.hold||initial_menu||(input.buttons&LINNET_BUTTON_MENU)!=0u;
#else
                bool held=!input.hold&&(initial_menu||(input.buttons&LINNET_BUTTON_MENU)!=0u);
#endif
#else
                bool held=!input.hold&&(input.buttons&LINNET_BUTTON_MENU)!=0u;
#endif
                if(held){
                    if(!menu_holding){menu_holding=true;menu_hold_started=now;}
                    if(now-menu_hold_started>=750000u){
                        menu_open=true;menu_activity=now;menu_buttons=input.buttons;
                        (void)ln_lcd_boot_menu(0u,1u,false);
                    }
                }else{
                    menu_holding=false;
                    if(now-menu_started>=1500000u
#if LINNET_BOOT_MENU_FIRST_PACKET_WAIT_US
                       && (input.packets||now-menu_started>=LINNET_BOOT_MENU_FIRST_PACKET_WAIT_US)
#endif
                    ){
                        boot_decided=true;ln_storage_boot_choice(4u);
                    }
                }
            }else{
                if(!menu_options){
                    menu_options=ln_storage_boot_options();
                    if(menu_options){
                        menu_selected=ln_storage_boot_default(menu_options);
                        menu_activity=now;
                        (void)ln_lcd_boot_menu(menu_options,menu_selected,false);
#if LINNET_BOOT_MENU_REDRAW_US
                        menu_redraw_due=ln_time_us()+LINNET_BOOT_MENU_REDRAW_US;
#endif
                    }
                }
#if LINNET_BOOT_MENU_REDRAW_US
                /* Refresh panels that need another frame after settling. */
                if(menu_options&&menu_redraw_due&&
                   (int32_t)(ln_time_us()-menu_redraw_due)>=0){
                    menu_redraw_due=0;
                    (void)ln_lcd_boot_menu(menu_options,menu_selected,false);
                }
#endif
                uint32_t pressed=input.buttons&~menu_buttons;
                menu_buttons=input.buttons;
                if(menu_options&&changed&&!input.hold){
                    int direction=0;
                    if(input.wheel_delta){
                        if((menu_wheel_accum>0&&input.wheel_delta<0)||
                           (menu_wheel_accum<0&&input.wheel_delta>0))menu_wheel_accum=0;
                        menu_wheel_accum+=input.wheel_delta;
                        if(menu_wheel_accum>24)menu_wheel_accum=24;
                        if(menu_wheel_accum< -24)menu_wheel_accum= -24;
                        menu_activity=now;
                    }
                    if(pressed&(LINNET_BUTTON_RIGHT|LINNET_BUTTON_MENU))direction=1;
                    else if(pressed&(LINNET_BUTTON_LEFT|LINNET_BUTTON_PLAY))direction=-1;
                    else if(now-menu_last_step>=160000u&&
                            (menu_wheel_accum>=12||menu_wheel_accum<= -12)){
                        direction=menu_wheel_accum>0?1:-1;
                        menu_wheel_accum=0;
                        menu_last_step=now;
                    }
                    if(direction){
                        if(pressed&(LINNET_BUTTON_RIGHT|LINNET_BUTTON_MENU|
                                    LINNET_BUTTON_LEFT|LINNET_BUTTON_PLAY)){
                            menu_wheel_accum=0;menu_last_step=now;
                        }
                        uint32_t next=menu_selected;
                        if(direction>0){
                            for(uint32_t choice=menu_selected+1u;choice<=3u;++choice)
                                if(menu_options&(1u<<(choice-1u))){next=choice;break;}
                        }else{
                            for(uint32_t choice=menu_selected;choice>1u;){
                                --choice;
                                if(menu_options&(1u<<(choice-1u))){next=choice;break;}
                            }
                        }
                        menu_activity=now;
                        if(next!=menu_selected){
                            menu_selected=next;
                            (void)ln_lcd_boot_menu(menu_options,menu_selected,false);
#if LINNET_BOOT_MENU_REDRAW_US
                            menu_redraw_due=ln_time_us()+LINNET_BOOT_MENU_REDRAW_US;
#endif
                        }
                    }
                    if(pressed&LINNET_BUTTON_SELECT){
                        boot_decided=true;
                        (void)ln_lcd_boot_menu(menu_options,menu_selected,true);
                        ln_storage_boot_choice(menu_selected);
                    }
                }
                if(!boot_decided&&menu_options&&now-menu_activity>=30000000u){
                    boot_decided=true;
                    uint32_t default_choice=ln_storage_boot_default(menu_options);
                    (void)ln_lcd_boot_menu(menu_options,default_choice,true);
                    ln_storage_boot_choice(default_choice);
                }
            }
        }
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        ln_runtime_restart_step();
#endif
#if defined(LINNET_AUDIO)
        LN_CACHE_FIRST_MARK(0xca12u);
        LN_TIMING_BEGIN(music_started);
        extern void ln_music_step(void);
#if defined(LINNET_VIDEO)
        extern bool ln_video_active(void);extern bool ln_video_playback_pending(void);
        if(!ln_video_active()&&!ln_video_playback_pending())
#endif
        ln_music_step();
#if defined(LINNET_VIDEO)
        else {extern bool ln_music_volume_sync(void);ln_music_volume_sync();}
#endif
        LN_TIMING_END(music_started,0);
        LN_CACHE_FIRST_MARK(0xca13u);
#endif
#if defined(LINNET_STORAGE)
        LN_CACHE_FIRST_MARK(0xca14u);
        LN_TIMING_BEGIN(storage_started);
        ln_storage_step_deferred();
#if defined(LINNET_TARGET_CLASSIC6G_N25) && defined(LINNET_IMAGE_BOOT) && defined(LINNET_PROFILE_RELEASE)
        if(setup_disk && ln_storage_mounted() && !ln_storage_busy()){
            if(!ln_storage_quiesce() || !ln_platform_enter_disk_mode(ln_storage_disk_sector_bytes()))
                ln_panic(30,14,0);
        }
#endif
        ln_storage_boot();
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        first_frame=ln_runtime_first_frame_ready();
#endif
#if !defined(LINNET_PROFILE_RELEASE)
        if(first_frame)readiness|=LINNET_BOOT_HEALTH_FIRST_FRAME;
        if(!ln_storage_busy()){
            readiness|=LINNET_BOOT_HEALTH_STORAGE_RESOLVED;ln_development_health(readiness);
            if(readiness==LINNET_BOOT_HEALTH_ALL)ln_recovery_mark_healthy();
        }
#endif
        ln_storage_healthy(LINNET_BOOT_HEALTH_MEMORY|LINNET_BOOT_HEALTH_SERVICES|LINNET_BOOT_HEALTH_STORAGE_RESOLVED|
            (first_frame?LINNET_BOOT_HEALTH_FIRST_FRAME:0u)|(audio_idle?LINNET_BOOT_HEALTH_AUDIO_IDLE:0u));
        LN_CACHE_FIRST_MARK(0xca15u);
        LN_TIMING_END(storage_started,1);
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        LN_CACHE_FIRST_MARK(0xca16u);
        ln_runtime_step();
        LN_CACHE_FIRST_MARK(0xca17u);
#endif
#if !defined(LINNET_PROFILE_RELEASE)
        LN_CACHE_FIRST_MARK(0xca18u);
        LN_TIMING_BEGIN(link_started);
        ln_development_poll();
        LN_CACHE_FIRST_MARK(0xca19u);
        ln_recovery_feed();
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        ln_usb_irq_enable();
        ln_core_usb_rearm();
#endif
        LN_TIMING_END(link_started,2);
#else
#if defined(LINNET_PLATFORM_CORE) && (defined(LINNET_IMAGE_OS) || defined(LINNET_STORAGE))
        ln_core_watchdog_feed();
#else
        __asm__ volatile("nop");
#endif
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        LN_CACHE_FIRST_MARK(0xca1au);
#if LINNET_RUNTIME_POWER_POLICY
        ln_runtime_power_end();
#endif
        ln_runtime_idle();
        LN_CACHE_FIRST_MARK(0xca1bu);
#endif
#if defined(LINNET_AUDIO) && defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
        cache_first_cycle=false;
#endif
    }
}
