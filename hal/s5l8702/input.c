#include "hal/api/input.h"
#include "board.h"
#include "registers.h"
#if defined(LINNET_INPUT_REMOTE)
#include "headset_remote.h"
#endif
#if defined(LINNET_IMAGE_OS) && defined(LINNET_TARGET_CLASSIC6G_N25)
#define LN_WHEEL_IRQ 1
#include "irq.h"
#else
#define LN_WHEEL_IRQ 0
#endif
#define WHEEL(offset) LN_REG(0x3c200000u+(offset))
extern uint32_t ln_time_us(void);
static struct ln_input_state state;
/* Only buttons/contact/switches participate in change detection. Keep their
 * published sample compact so the shared driver fits the NOR monitor. */
static uint32_t reported;
static uint32_t last_packet,last_events,switch_deadline,handshakes,seen_buttons,wheel_travel;
static bool ready;
static int previous=-1;
#if LN_WHEEL_IRQ
static int32_t pending_wheel_delta;
#endif
static void wheel_packet(uint32_t now) {
    last_events=WHEEL(0x14)&7u;
    if(!last_events)return;
    uint32_t packet=WHEEL(0x18);WHEEL(0x14)=last_events;last_packet=packet;
    if((packet&0x800000ffu)==0x8000001au &&
       (!(packet&0x40000000u) || ((packet>>16)&127u)<96)) {
        state.packets++;state.buttons=(packet>>8)&31;state.touched=(packet>>30)&1;
        state.wheel_position=(packet>>16)&127;
        if(state.touched && previous>=0) {
            int delta=(int)state.wheel_position-previous;
            if(delta>48)delta-=96;else if(delta< -48)delta+=96;
#if LN_WHEEL_IRQ
            pending_wheel_delta+=delta;
#else
            state.wheel_delta=delta;
#endif
            wheel_travel+=(uint32_t)(delta<0?-delta:delta);
        }
        previous=state.touched?state.wheel_position:-1;state.timestamp_us=now;
    } else if((packet&0x8000ffffu)==0x8000023au) {
        handshakes++;state.buttons=(packet>>16)&31;
    } else {state.invalid_packets++;state.buttons=0;state.touched=0;previous=-1;}
}
#if LN_WHEEL_IRQ
uint32_t ln_input_deadline(uint32_t now){return ready?switch_deadline:now;}
bool ln_input_irq(void) {
    if(!ready)return false;
    wheel_packet(ln_time_us());
    return last_events!=0;
}
#endif
bool ln_input_init(void) {
    ready=false;state=(struct ln_input_state){0};reported=0;previous=-1;handshakes=seen_buttons=wheel_travel=0;
#if LN_WHEEL_IRQ
    pending_wheel_delta=0;
    ln_s5l_remote_init();
#endif
    if(!ln_s5l_board_input_power())return false;
    LN_CLOCK_GATE1&=~2u;
    LN_REG(0x3cf001c0u)=(LN_REG(0x3cf001c0u)&~0x00ffff00u)|0x00222200u;
    WHEEL(0)=0;WHEEL(0x14)=7;WHEEL(0x10)=1;
    WHEEL(0)=0x380000;WHEEL(8)=0x20000;WHEEL(0x1c)=0x8000023a;WHEEL(4)|=1;
    switch_deadline=ln_time_us();ready=true;
#if LN_WHEEL_IRQ
    ln_s5l_irq_enable(23,true);
#endif
    return true;
}
bool ln_input_poll(struct ln_input_state *out) {
    if(!ready || !out)return false;
    uint32_t now=ln_time_us();
#if defined(LINNET_INPUT_REMOTE)
    state.remote_actions=0;state.remote_volume=0;
#endif
    if((int32_t)(now-switch_deadline)>=0) {
        bool hold,headphones;
        if(!ln_s5l_board_switches(&hold,&headphones))return false;
        state.hold=hold;state.headphones=headphones;switch_deadline=now+20000u;
#if defined(LINNET_INPUT_REMOTE)
        ln_s5l_remote_poll(now,headphones,&state.remote_actions,&state.remote_volume);
#endif
    }
#if LN_WHEEL_IRQ
    uint32_t flags=ln_s5l_irq_save();
    state.wheel_delta=pending_wheel_delta;pending_wheel_delta=0;
#else
    state.wheel_delta=0;wheel_packet(now);
#endif
    if(state.hold){state.buttons=0;state.touched=0;state.wheel_delta=0;previous=-1;}
    seen_buttons|=state.buttons;
    *out=state;
    /* A release may arrive before the foreground consumes its last motion. */
    if(out->wheel_delta&&!out->hold)out->touched=1;
    /* IRQ packets update state before this poll. Compare with the last
     * published sample so idle timer polls do not masquerade as input and
     * immediately wake a sleeping system. Include the coalesced motion above
     * so its following release is still delivered. */
    uint32_t sample=out->buttons|((uint32_t)out->touched<<5)|
        ((uint32_t)out->hold<<6)|((uint32_t)out->headphones<<7);
    bool changed=out->wheel_delta || sample!=reported;
#if defined(LINNET_INPUT_REMOTE)
    changed=changed||out->remote_actions||out->remote_volume;
#endif
    reported=sample;
#if LN_WHEEL_IRQ
    ln_s5l_irq_restore(flags);
#endif
    return changed;
}
void ln_input_snapshot(struct ln_input_state *out){if(out)*out=state;}
void ln_input_diagnostics(uint32_t out[16]) {
    for(unsigned i=0;i<16;i++)out[i]=0;
    out[0]=ready;out[1]=last_packet;out[2]=last_events;out[3]=handshakes;
    out[8]=seen_buttons;out[9]=wheel_travel;
#if defined(LINNET_INPUT_REMOTE)
    ln_s5l_remote_diagnostics(out+10);
#endif
    if(ready){out[4]=WHEEL(0);out[5]=WHEEL(4);out[6]=WHEEL(0x14);out[7]=WHEEL(0x18);}
}
