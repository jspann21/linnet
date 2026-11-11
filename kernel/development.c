#include "kernel/development.h"
#include "boot/include/loader.h"
#include "hal/api/platform.h"
#include "hal/api/usb.h"
#include "hal/api/recovery.h"
#include "linnet_build.h"
#if defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK
#include "hal/api/input.h"
#include "boot/include/linnet_bytes.h"
#endif
#if defined(LINNET_AUDIO)
#include "kernel/audio_link.h"
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
#include "kernel/platform_link.h"
#endif

extern _Noreturn void ln_panic(uint32_t reason, uint32_t pc, uint32_t status);
#if defined(LINNET_STORAGE)
#include "kernel/storage_link.h"
#include "services/storage/storage.h"
#endif
static struct ln_loader loader;
static ln_link_backend backend;
static ln_link_dispatcher dispatcher;
/* One command owns reply until encoding and transmission finish. Runtime
 * progress polls USB DMA without recursively dispatching another command. */
_Static_assert(sizeof dispatcher.reply >= LINNET_LINK_MAX_PAYLOAD, "Link reply capacity");
int ln_development_stage_busy(void){return dispatcher.staging;}
static ln_link_parser parser;
static uint32_t usb_generation;
static uint8_t receive_bytes[512];
static uint8_t transmit_bytes[LINNET_LINK_MAX_FRAME];
#if defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK
static uint8_t boot_input_reply[80];
#endif
static int (*loader_read)(void *, uint16_t, uint8_t *, uint32_t *);
#if defined(LINNET_VIDEO)
#include "services/video/video.h"
#endif
#if defined(LINNET_AUDIO)
#include "services/music/player.h"
#endif
#if defined(LINNET_COVERFLOW_PACKED)
#include "services/music/coverflow.h"
#endif
#if defined(LINNET_VIDEO) || defined(LINNET_AUDIO)
static int (*loader_begin)(void *, uint32_t);
static int (*loader_write)(void *, uint32_t, const uint8_t *, uint32_t);
/* Playback owns STAGE until its outstanding work has drained. */
static int stage_begin(void *context,uint32_t size) {
#if defined(LINNET_VIDEO)
    if(ln_video_active()||ln_video_playback_pending())return 0;
#endif
#if defined(LINNET_AUDIO)
    if(ln_player_busy())return 0;
#endif
#if defined(LINNET_COVERFLOW_PACKED)
    if(size<LINNET_IMAGE_HEADER_SIZE+4u||size>LINNET_STAGE_CAPACITY)return 0;
    if(!ln_coverflow_stage_prepare())return 0;
#endif
    return loader_begin(context,size);
}
static int stage_write(void *context,uint32_t offset,const uint8_t *data,uint32_t size) {
#if defined(LINNET_VIDEO)
    if(ln_video_active()||ln_video_playback_pending())return 0;
#endif
#if defined(LINNET_AUDIO)
    if(ln_player_busy())return 0;
#endif
    return loader_write(context,offset,data,size);
}
#endif
static int diagnostic_read(void *context, uint16_t command, uint8_t *output, uint32_t *length) {
    if(command==54){
        if(!output||!length||*length<192u)return 0;
        uint32_t words[48];ln_recovery_timeline_diagnostics(words);
        for(unsigned i=0;i<48;++i)for(unsigned byte=0;byte<4;++byte)
            output[i*4+byte]=(uint8_t)(words[i]>>(byte*8));
        *length=192u;return 1;
    }
    if(command==53){
        if(!output||!length||*length<64u)return 0;
        uint32_t words[16];ln_recovery_startup_diagnostics(words);
        for(unsigned i=0;i<16;++i)for(unsigned byte=0;byte<4;++byte)
            output[i*4+byte]=(uint8_t)(words[i]>>(byte*8));
        *length=64u;return 1;
    }
    if (command != 48) return loader_read(context, command, output, length);
    if (!output || !length || *length < 96u) return 0;
    uint32_t words[24];
    ln_lcd_diagnostics(words);
    ln_usb_diagnostics(words + 8);
    ln_recovery_diagnostics(words + 16);
    for (unsigned i=0; i<24; ++i)
        for (unsigned byte=0; byte<4; ++byte)
            output[i*4+byte]=(uint8_t)(words[i]>>(byte*8));
    *length=96u;
    return 1;
}
int ln_recovery_transport_stop(void) { return ln_usb_stop(); }
uint32_t ln_recovery_transport_diagnostics(void) { return ln_usb_stop_diagnostics(); }

static void progress(void *context) { (void)context; ln_recovery_feed(); }
static int can_reboot(void *context, uint16_t command) {
    (void)context;
    return ln_recovery_can_reboot(command);
}
static int crash(void *context) {
    (void)context;
    __asm__ volatile(".word 0xe7f000f0");
    return 0;
}

int ln_development_init(void) {
    struct ln_loader_config config = {0};
    const uint8_t build_id[32] = LINNET_BUILD_ID_BYTES;
    config.stage = (uint8_t *)(uintptr_t)LINNET_STAGE_ADDRESS;
    config.capacity = LINNET_STAGE_CAPACITY;
    config.policy.target = LINNET_TARGET_NUMBER;
    config.policy.allowed_profiles = 6u;
    config.policy.ram_start = LINNET_LOAD_ADDRESS;
    config.policy.ram_size = LINNET_OS_CAPACITY;
    config.crash_mailbox = (volatile uint8_t *)(uintptr_t)LINNET_MAILBOX_ADDRESS;
#if defined(LINNET_AUDIO)
    config.crash_mailbox = (volatile uint8_t *)ln_platform_uncached_address(LINNET_MAILBOX_ADDRESS);
#endif
#if defined(LINNET_PROFILE_TRACE)
    config.profile = 2;
#else
    config.profile = 1;
#endif
#if defined(LINNET_IMAGE_BOOT)
    config.image_kind = 1;
#endif
    config.generation = ln_recovery_generation();
    config.capabilities = LINNET_LOADER_CAP_UPLOAD | LINNET_LOADER_CAP_CRASH;
    if (ln_recovery_ready()) config.capabilities |= LINNET_LOADER_CAP_RAM_BOOT | LINNET_LOADER_CAP_REBOOT_BOOT;
    config.capabilities |= ln_recovery_reboot_capabilities();
    for (unsigned i=0; i<32; ++i) config.build_id[i] = build_id[i];
    uint8_t recorded_build[32];
    if (ln_recovery_image_identity(recorded_build, config.image_sha256)) {
        for (unsigned i=0; i<32; ++i)
            if (recorded_build[i] != build_id[i]) {
                for (unsigned j=0; j<32; ++j) config.image_sha256[j]=0;
                break;
            }
    }
    config.actions.progress = progress;
    config.actions.quiesce = ln_recovery_quiesce;
    config.actions.chainload = ln_recovery_chainload;
    config.actions.reboot = ln_recovery_reboot;
    config.actions.can_reboot = can_reboot;
    config.actions.crash = crash;
    if (!ln_loader_init(&loader, &config, &backend)) return 0;
    loader_read = backend.read;
    backend.read = diagnostic_read;
#if defined(LINNET_VIDEO) || defined(LINNET_AUDIO)
    loader_begin=backend.begin;backend.begin=stage_begin;
    loader_write=backend.write;backend.write=stage_write;
#endif
    ln_link_dispatch_init(&dispatcher, &backend);
    ln_link_parser_init(&parser);
    if (!ln_usb_init(LINNET_USB_SERIAL)) return 0;
    usb_generation = ln_usb_generation();
#if defined(LINNET_IMAGE_BOOT)
    LN_BOOT_STAMP(LN_BT_BOOT_USB);
#else
    LN_BOOT_STAMP(LN_BT_OS_USB);
#endif
    return 1;
}

void ln_development_health(uint32_t flags) { ln_loader_set_health(&loader, flags); }

void ln_development_poll(void) {
#if defined(LINNET_IMAGE_OS)
    LN_BOOT_STAMP(LN_BT_OS_POLL);
#endif
    if (!ln_usb_poll()) {
        ln_recovery_return(10);
    }
    if (usb_generation != ln_usb_generation()) {
        usb_generation = ln_usb_generation();
        ln_link_parser_init(&parser);
        ln_link_dispatch_init(&dispatcher, &backend);
    }
    size_t received = ln_usb_read(receive_bytes, sizeof receive_bytes);
    for (size_t i=0; i<received; ++i) {
        ln_link_frame request, response;
        int parsed = ln_link_parser_feed(&parser, receive_bytes[i], &request);
        if (parsed < 0) {
            /* Framing failure is a connection failure, never resynchronize a
             * state-changing stream by accidentally accepting payload bytes. */
            ln_usb_disconnect();
            ln_recovery_return(11);
        }
        if (parsed != 1) continue;
        ln_usb_rx_hint(0);
#if defined(LINNET_IMAGE_OS)
        LN_BOOT_STAMP(LN_BT_OS_REQUEST);
#endif
        int handled = 0;
#if defined(LINNET_IMAGE_BOOT) && LINNET_BOOT_APPLE_FALLBACK
        if(request.command==70u && !request.flags && !request.length){
            struct ln_input_state sample;(void)ln_input_poll(&sample);
            uint32_t words[16];ln_input_diagnostics(words);
            for(uint32_t j=0;j<16u;++j)ln_write_le32(boot_input_reply+j*4u,words[j]);
#if LINNET_BOOT_MENU_QUERY_BEFORE_INPUT || LINNET_BOOT_MENU_QUERY_AFTER_INPUT
            uint32_t query[4];ln_input_boot_menu_diagnostics(query);
            for(uint32_t j=0;j<4u;++j)ln_write_le32(boot_input_reply+(16u+j)*4u,query[j]);
#endif
            response.command=request.command;response.sequence=request.sequence;
            response.flags=LINNET_LINK_ACK;response.payload=boot_input_reply;
            response.length=sizeof boot_input_reply;handled=1;
        }
#endif
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
        handled = ln_platform_link_dispatch(&request, &response, dispatcher.reply);
#if defined(LINNET_AUDIO)
        if(!handled)handled=ln_audio_link_dispatch(&request,&response,dispatcher.reply);
#endif
#endif
#if defined(LINNET_STORAGE)
        if (!handled) handled = ln_storage_link_dispatch(&request, &response, dispatcher.reply);
#endif
        if (!handled && !ln_link_dispatch(&dispatcher, &request, &response)) continue;
        size_t size = ln_link_encode(&response, transmit_bytes, sizeof transmit_bytes);
        if (!size) ln_panic(12,0,0);
        uint32_t deadline = ln_time_us()+2000000u;
        size_t sent=0;
        uint32_t budget=2000000u;
        while (sent<size && budget-- && (int32_t)(deadline-ln_time_us())>0) {
            if (!ln_usb_poll() || usb_generation != ln_usb_generation()) break;
            size_t step = ln_usb_write(transmit_bytes+sent,size-sent);
            sent += step;
            if (step) ln_recovery_feed();
        }
        if (sent!=size || !ln_usb_flush(deadline) || usb_generation != ln_usb_generation()) {
            ln_usb_disconnect();
            ln_recovery_return(13);
        }
#if defined(LINNET_STORAGE)
        if(response.flags==LINNET_LINK_ACK)ln_storage_link_ack_sent(response.command);
#endif
        if (dispatcher.deferred) {
            (void)ln_link_dispatch_ack_sent(&dispatcher, response.sequence);
            /* A successful boot/reboot/crash never returns. */
            ln_recovery_return(14);
        }
        ln_recovery_feed();
    }
    ln_usb_rx_hint(parser.expected-parser.used);
#if defined(LINNET_PLATFORM_CORE) && defined(LINNET_IMAGE_OS)
    /* Reading retires the receive buffer. Reprime before the executive sleeps,
     * including when a packet ended partway through a Link frame. */
    if (!ln_usb_poll()) ln_recovery_return(10);
#endif
}
