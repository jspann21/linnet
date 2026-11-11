#include "hal/s5l8702/pcm_dma.h"
#include "hal/arm926/cache.h"
#include "hal/arm926/mmu.h"
#include "hal/api/usb.h"
#include "hal/api/input.h"
#include "hal/api/display.h"
#include "hal/api/power.h"
#include "hal/api/platform.h"
#include "hal/s5l8702/board.h"
#include "hal/link/link.h"
#include "hal/s5l8702/registers.h"
#include "boot/include/linnet_bytes.h"
#include "boot/include/loader.h"
#include "targets/classic6g-n25/board.h"
#include "bootstrap_build.h"
#include "hal/s5l8702/boot_state.h"
static ln_link_parser parser;
static ln_link_dispatcher dispatcher;
static struct ln_loader loader;
static ln_link_backend backend;
static uint8_t input[512],output[LINNET_LINK_MAX_FRAME];
/* Writable image data prevents constant propagation across the host's bounded
 * serial/build binding step for precompiled installer templates. */
static uint8_t build_id[32]=LINNET_BOOTSTRAP_BUILD_BYTES;
char ln_bootstrap_serial[17]=LINNET_BOOTSTRAP_SERIAL;
static bool memory_ready,transport_ready;
static uint32_t memory_failure;
static uint32_t usb_recoveries,usb_last_error;
static uint32_t startup_mode,startup_stage,startup_size;
static int (*verify_image)(void *,uint32_t,const uint8_t[32]);
static int (*read_loader)(void *,uint16_t,uint8_t *,uint32_t *);
static int read_status(void *context,uint16_t command,uint8_t *output,uint32_t *size) {
    if(command!=48)return read_loader(context,command,output,size);
    static const char names[]="ram_ready=\0ram_failure=\0panel_type=\0gpio6=\0pmu_identity=\0"
        "usb_recoveries=\0usb_last_error=\0startup_mode=\0startup_stage=\0startup_size=";
    struct ln_n25_power_identity identity={0};
    bool pmu=ln_n25_power_identify(&identity);
    uint32_t gpio=*(volatile uint32_t *)(uintptr_t)0x3cf000c4u;
    uint32_t values[]={memory_ready,memory_failure,(gpio>>4)&3,gpio,
        (uint32_t)identity.version|((uint32_t)identity.variant<<8)|((uint32_t)identity.status<<16)|((uint32_t)pmu<<24),
        usb_recoveries,usb_last_error,startup_mode,startup_stage,startup_size};
    if(*size<320)return 0;
    unsigned used=0;const char *name=names;
    for(unsigned i=0;i<10;i++) {
        while(*name)output[used++]=(uint8_t)*name++;
        name++;
        for(unsigned j=0;j<8;j++)output[used++]=(uint8_t)"0123456789abcdef"[(values[i]>>(28-4*j))&15];
        output[used++]='\n';
    }
    *size=used;return 1;
}
extern const uint8_t ln_sram_enter_start[],ln_sram_enter_end[];
extern int ln_bootstrap_control(const ln_link_frame *,ln_link_frame *,uint8_t *);
extern void ln_bootstrap_audio_poll(void);
extern bool ln_bootstrap_audio_stop(void);
extern unsigned ln_bootstrap_reset_take(void);
extern bool ln_s5l_watchdog_timer(uint32_t milliseconds);
extern void ln_s5l_watchdog_route(bool software);
bool ln_bootstrap_memory_ready(void){return memory_ready;}
uint32_t ln_time_us(void){return LN_TIMER_E(0x14);}
bool ln_core_delay_us(uint32_t delay) {
    if(delay>100000u)return false;
    uint32_t start=ln_time_us(),budget=20000000u;
    while(ln_time_us()-start<delay) {
        if(!budget-- || (transport_ready&&!ln_usb_poll()))return false;
    }
    return true;
}
static void progress(void *context){(void)context;(void)ln_usb_poll();}
static int quiesce(void *context){(void)context;return ln_bootstrap_audio_stop() && ln_display_quiesce() && ln_usb_stop() && ln_arm926_mmu_disable();}
static int verify(void *context,uint32_t size,const uint8_t digest[32]) {
    struct ln_loader *state=context;
    if(size<LINNET_IMAGE_HEADER_SIZE)return 0;
    uint32_t address=ln_read_le32(state->config.stage+24);
    if(address==0x22000000u) {
        state->config.policy.ram_start=0x22000000u;state->config.policy.ram_size=0x10000;
    } else if(memory_ready && address==0x08000000u) {
        state->config.policy.ram_start=0x08000000u;state->config.policy.ram_size=0x800000;
    } else return 0;
    return verify_image(context,size,digest);
}
static int enter(void *context,const uint8_t *payload,uint32_t size,uint32_t load,uint32_t entry,
                 const uint8_t next_build[32],const uint8_t sha[32],uint32_t generation) {
    (void)context;
    uint8_t *handoff=(uint8_t *)(uintptr_t)0x22017e00u;
    ln_write_le32(handoff,0x484e494cu);ln_write_le32(handoff+4,generation);
    for(unsigned n=0;n<32;n++){handoff[8+n]=next_build[n];handoff[40+n]=sha[n];}
    ln_write_le32(handoff+72,ln_crc32(handoff,72));
    uint32_t code_size=(uint32_t)(ln_sram_enter_end-ln_sram_enter_start);
    if(code_size>256)return 0;
    uint8_t *code=(uint8_t *)(uintptr_t)0x22018000u;
    for(unsigned n=0;n<code_size;n++)code[n]=ln_sram_enter_start[n];
    uint32_t zero=0;
    __asm__ volatile("mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c7,c5,0"::"r"(zero):"memory");
    ((void (*)(const uint8_t *,uint32_t,uint32_t,uint32_t))(uintptr_t)0x22018000u)(payload,load,size,entry);
    return 0;
}
static void init_loader(void) {
    struct ln_loader_config config={0};
    extern uint8_t __bss_end[];
    uintptr_t sram_stage=((uintptr_t)__bss_end+255u)&~255u;
    config.stage=(uint8_t *)(uintptr_t)(memory_ready?0x09000000u:sram_stage);
    config.capacity=memory_ready?0x800000u:(uint32_t)(0x22017000u-sram_stage);
    config.policy=(struct ln_image_policy){1238,2,0x22000000u,0x10000};
    config.profile=1;config.image_kind=1;config.health_flags=memory_ready?1:0;
    config.generation=1;config.capabilities=memory_ready?(LINNET_LOADER_CAP_UPLOAD|LINNET_LOADER_CAP_RAM_BOOT):0;
    for(unsigned n=0;n<32;n++)config.build_id[n]=build_id[n];
    const uint8_t *handoff=(const uint8_t *)(uintptr_t)0x22017e00u;
    bool match=ln_read_le32(handoff)==0x484e494cu && ln_read_le32(handoff+72)==ln_crc32(handoff,72);
    for(unsigned n=0;n<32;n++)if(handoff[8+n]!=build_id[n])match=false;
    if(match){config.generation=ln_read_le32(handoff+4);for(unsigned n=0;n<32;n++)config.image_sha256[n]=handoff[40+n];}
    config.actions.progress=progress;config.actions.quiesce=quiesce;config.actions.chainload=enter;
    (void)ln_loader_init(&loader,&config,&backend);
    verify_image=backend.verify;backend.verify=verify;
    read_loader=backend.read;backend.read=read_status;
    ln_link_dispatch_init(&dispatcher,&backend);
}
static void retain_watchdog_fault(void){
    volatile uint32_t *fault=(volatile uint32_t *)(uintptr_t)0x22017b10u;
    if(fault[0]!=0x3246574cu)return;
    uint32_t checksum=fault[1]^fault[2];
    for(unsigned i=4;i<13;i++)checksum^=fault[i];
    if(fault[3]!=~checksum)return;
    struct ln_crash_record record={0};uint8_t bytes[LINNET_CRASH_MAILBOX_SIZE];
    record.reason=9;record.flags=1;record.registers[15]=fault[1];record.cpsr=fault[2];
    record.boot_generation=record.sequence=fault[4];
    for(unsigned i=0;i<32;i++)record.build_id[i]=(uint8_t)(fault[5+i/4]>>(8*(i%4)));
    if(ln_crash_encode(&record,bytes,sizeof bytes)){
        volatile uint8_t *mailbox=(volatile uint8_t *)(uintptr_t)0x0bfff000u;
        for(unsigned i=0;i<4;i++)mailbox[i]=0;
        for(unsigned i=4;i<sizeof bytes;i++)mailbox[i]=bytes[i];
        for(unsigned i=0;i<4;i++)mailbox[i]=bytes[i];
        fault[0]=0;
    }
}
_Noreturn void ln_bootstrap_main(void) {
    *(volatile uint16_t *)(uintptr_t)0x3c500012u=0;
    LN_CLOCK_GATE1&=~(1u<<5);LN_TIMER_E(0)=0x440;LN_TIMER_E(0x10)=11;
    LN_TIMER_E(8)=UINT32_MAX;LN_TIMER_E(4)=3;
    (void)ln_s5l_pcm_stop();
    memory_ready=ln_n25_memory_init(&memory_failure);
    if(!ln_s5l_boot_serial_save(ln_bootstrap_serial))for(;;)__asm__ volatile("nop");
    if(memory_ready)retain_watchdog_fault();
    (void)ln_arm926_instruction_cache(true);
    init_loader();ln_link_parser_init(&parser);
    int mode=ln_s5l_boot_mode();
    startup_mode=(uint32_t)mode;
    const uint8_t *handoff=(const uint8_t *)(uintptr_t)0x22017e00u;
    bool ram_handoff=ln_read_le32(handoff)==0x484e494cu && ln_read_le32(handoff+72)==ln_crc32(handoff,72);
    if(memory_ready && (mode==LN_S5L_BOOT_NORMAL || mode==LN_S5L_BOOT_DISK || (mode<0 && !ram_handoff))) {
        startup_stage=1;
        if(!ln_usb_init(ln_bootstrap_serial))for(;;)__asm__ volatile("nop");
        transport_ready=true;
        extern uint32_t ln_n25_persistent_load(uint8_t *,struct ln_image_info *,uint8_t[32]);
        struct ln_image_info info;uint8_t sha[32];
        ln_s5l_boot_request(mode==LN_S5L_BOOT_DISK?LN_S5L_BOOT_ATTEMPT_DISK:LN_S5L_BOOT_ATTEMPT);
        uint32_t size=ln_n25_persistent_load(loader.config.stage,&info,sha);
        startup_size=size;startup_stage=2;
        if(size && quiesce(0)) {
            startup_stage=3;
            (void)enter(0,loader.config.stage+128,info.payload_size,info.load_address,info.entry_address,info.build_id,sha,0);
            startup_stage=4;
        }
        (void)ln_usb_stop();transport_ready=false;
    }
    if(memory_ready&&ln_s5l_board_recovery_screen(false)&&ln_power_init())ln_power_backlight(true,128);
    bool input_ready=ln_input_init(),input_released=false;
    bool host_connected=false;
    uint32_t previous_buttons=0;
    if(!ln_usb_init(ln_bootstrap_serial)){
        uint32_t stage=((volatile uint32_t *)(uintptr_t)0x22017f00u)[1];
        (void)ln_lcd_fault(0x55000000u|(stage&0xffffu),build_id);
        for(;;)__asm__ volatile("nop");
    }
    transport_ready=true;
    uint32_t connection=ln_usb_generation();
    uint32_t last_request=ln_time_us();
    for(;;) {
        ln_bootstrap_audio_poll();
        struct ln_input_state input_state;
        if(input_ready&&ln_input_poll(&input_state)){
            if(!input_state.buttons)input_released=true;
            if(!host_connected&&memory_ready&&input_released&&input_state.buttons!=previous_buttons&&
               (input_state.buttons==LINNET_BUTTON_SELECT||input_state.buttons==LINNET_BUTTON_PLAY)){
                if(quiesce(0)){
                    ln_s5l_boot_request(input_state.buttons==LINNET_BUTTON_PLAY?LN_S5L_BOOT_DISK:LN_S5L_BOOT_NORMAL);
                    __asm__ volatile("msr cpsr_c,#0xd3":::"memory");
                    LN_REG(0x3c800000u)=0x100000u;
                    for(;;)__asm__ volatile("nop");
                }
            }
            previous_buttons=input_state.buttons;
        }
        /* A Windows CDC open can remain pending without a new SETUP packet.
         * This bootstrap has no background playback or disk writes: after a
         * minute without a Link frame, rebuild the USB connection. Active
         * uploads and diagnostics refresh this deadline on every request. */
        if(!ln_usb_poll() || ln_time_us()-last_request>=60000000u) {
            if(host_connected){host_connected=false;if(memory_ready)(void)ln_s5l_board_recovery_screen(false);}
            uint32_t diagnostic[8];ln_usb_diagnostics(diagnostic);
            usb_last_error=diagnostic[6]?diagnostic[6]:0x100u;
            if(usb_recoveries!=UINT32_MAX)usb_recoveries++;
            (void)ln_usb_stop();
            ln_link_parser_init(&parser);ln_link_dispatch_init(&dispatcher,&backend);loader.verified=0;
            last_request=ln_time_us();
            if(!ln_usb_init(ln_bootstrap_serial))continue;
            connection=ln_usb_generation();
        }
        if(connection!=ln_usb_generation()) {
            connection=ln_usb_generation();ln_link_parser_init(&parser);
            ln_link_dispatch_init(&dispatcher,&backend);loader.verified=0;
        }
        size_t bytes=ln_usb_read(input,sizeof(input));
        for(size_t n=0;n<bytes;n++) {
            ln_link_frame request,response;
            if(ln_link_parser_feed(&parser,input[n],&request)!=1)continue;
            last_request=ln_time_us();
            /* A new host session can follow a manual display diagnostic.
             * Restore setup on every HELLO, even without a USB bus reset. */
            if(request.command==1u){
                host_connected=true;
                if(memory_ready)(void)ln_s5l_board_recovery_screen(true);
            }
            if(request.command==0x7f00u) {
                if(!ln_bootstrap_control(&request,&response,dispatcher.reply))continue;
            } else if(!ln_link_dispatch(&dispatcher,&request,&response))continue;
            size_t length=ln_link_encode(&response,output,sizeof(output)),sent=0;
            uint32_t deadline=ln_time_us()+2000000u;
            while(sent<length && (int32_t)(ln_time_us()-deadline)<0) {
                if(!ln_usb_poll())break;
                sent+=ln_usb_write(output+sent,length-sent);
            }
            unsigned reset_requested=ln_bootstrap_reset_take();
            if(sent==length && ln_usb_flush(deadline)) {
                (void)ln_link_dispatch_ack_sent(&dispatcher,request.sequence);
                if(reset_requested && quiesce(0)) {
                    ln_s5l_boot_request(reset_requested==5?LN_S5L_BOOT_DISK:
                                        reset_requested==4?LN_S5L_BOOT_NORMAL:LN_S5L_BOOT_RECOVERY);
                    if(reset_requested>1 && reset_requested<4) {
                        if(reset_requested==3)(void)ln_s5l_watchdog_timer(1000);
                        ln_s5l_watchdog_route(reset_requested==2);
                        for(;;)__asm__ volatile("nop");
                    }
                    __asm__ volatile("msr cpsr_c,#0xd3":::"memory");
                    LN_REG(0x3c800000u)=0x100000u;
                    for(;;)__asm__ volatile("nop");
                }
            }
        }
    }
}
