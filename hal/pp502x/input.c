#include "hal/api/input.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "hal/pp502x/target.h"
#define LINNET_MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
static struct ln_input_state ln_input;
static int32_t ln_last_wheel = -1;
static bool ln_input_ready;
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t ln_input_polls,ln_input_ready_reads,ln_input_last_packet,ln_input_last_status;
#endif

bool ln_input_init(void)
{
    ln_input_ready = false;
    LINNET_MMIO(0x6000600cu) |= 0x10000u;
    LINNET_MMIO(0x60006004u) |= 0x10000u;
    if (!ln_core_delay_us(5u)) return false;
    LINNET_MMIO(0x60006004u) &= ~0x10000u;
    LINNET_MMIO(0x70000010u) |= 0x40000u;
    ln_target_input_prepare();
    LINNET_MMIO(0x7000c100u) = 0xc00a1f00u;
    LINNET_MMIO(0x7000c104u) = 0x01000000u;
#if LINNET_INPUT_FIRST_PACKET_FIX
    LINNET_MMIO(0x7000c100u) &= ~0x60000000u;
    LINNET_MMIO(0x7000c104u) |= 0x04000000u;
    LINNET_MMIO(0x7000c100u) |= 0x60000000u;
#endif
    ln_target_switches_prepare();
    uint32_t pins = ln_target_switches_read();
    ln_input.hold = !(pins & 0x20u);
    ln_input.headphones = (pins & 0x80u) != 0;
    ln_input.buttons = 0;
    ln_input.touched = 0;
    ln_input.wheel_delta = 0;
    ln_input.timestamp_us = ln_time_us();
    ln_last_wheel = -1;
    ln_target_switches_rearm(pins);
    ln_input_ready = true;
    ln_core_input_rearm();
    return true;
}
bool ln_input_poll(struct ln_input_state *out)
{
    if (!ln_input_ready || !out) return false;
#if !defined(LINNET_PROFILE_RELEASE)
    ++ln_input_polls;
    ln_input_last_status=LINNET_MMIO(0x7000c104u);
#endif
    struct ln_input_state next = ln_input;
    next.wheel_delta = 0;
    uint32_t pins = ln_target_switches_read();
    next.hold = !(pins & 0x20u);
    next.headphones = (pins & 0x80u) != 0;
    if (LINNET_MMIO(0x7000c104u) & 0x04000000u) {
        /* PP502x clickwheel receive data needs 50 us to settle. */
        if (!ln_core_delay_us(50u)) return false;
        uint32_t packet = LINNET_MMIO(0x7000c140u);
#if !defined(LINNET_PROFILE_RELEASE)
        ++ln_input_ready_reads;ln_input_last_packet=packet;
#endif
        if ((packet & 0x800000ffu) == 0x8000001au &&
            (!(packet & 0x40000000u) || ((packet >> 16) & 0x7fu) < 96u)) {
            ++next.packets;
            next.buttons = (packet >> 8) & 31u;
            next.touched = (packet & 0x40000000u) != 0;
            next.wheel_position = (uint8_t)((packet >> 16) & 0x7fu);
            if (next.touched && ln_last_wheel >= 0) {
                int32_t delta = (int32_t)next.wheel_position - ln_last_wheel;
                if (delta > 48) delta -= 96;
                if (delta < -48) delta += 96;
                next.wheel_delta = delta;
            }
            ln_last_wheel = next.touched ? next.wheel_position : -1;
        } else if ((packet & 0x8000ffffu) == 0x8000023au) {
            /* Startup handshakes can carry a held button before ordinary
             * wheel packets arrive. */
            next.buttons = (packet >> 16) & 31u;
        } else if ((packet & 0x800000ffu) != 0x8000003au && packet != UINT32_MAX) {
            ++next.invalid_packets;
            next.buttons = 0;
            next.touched = 0;
            ln_last_wheel = -1;
#if defined(LINNET_IMAGE_BOOT)
            /* A malformed startup frame can leave the wheel receive latch
             * stuck. Restart reception so a held key can be sampled again. */
            if (!ln_core_delay_us(2000u)) return false;
            LINNET_MMIO(0x7000c100u) &= ~0x60000000u;
            LINNET_MMIO(0x7000c104u) |= 0x04000000u;
            LINNET_MMIO(0x7000c100u) |= 0x60000000u;
#endif
        }
        LINNET_MMIO(0x7000c104u) |= 0x0c000000u;
        LINNET_MMIO(0x7000c100u) = 0x400a1f00u;
    }
    if (next.hold || next.hold != ln_input.hold) {
        next.buttons = 0;
        next.wheel_delta = 0;
        next.touched = 0;
        ln_last_wheel = -1;
    }
    bool changed = next.buttons != ln_input.buttons || next.wheel_delta ||
        next.touched != ln_input.touched || next.hold != ln_input.hold ||
        next.headphones != ln_input.headphones;
    next.timestamp_us = ln_time_us();
    ln_input = next;
    *out = next;
    ln_target_switches_rearm(pins);
    ln_core_input_rearm();
    return changed;
}
void ln_input_snapshot(struct ln_input_state *out) { if (out) *out = ln_input; }
#if !defined(LINNET_PROFILE_RELEASE)
void ln_input_diagnostics(uint32_t output[16])
{
    if(!output)return;
    output[0]=0x31504e49u;output[1]=ln_input_polls;output[2]=ln_input_ready_reads;
    output[3]=ln_input.packets;output[4]=ln_input.invalid_packets;
    output[5]=ln_input_last_status;output[6]=ln_input_last_packet;
    output[7]=(uint32_t)ln_last_wheel;
    output[8]=LINNET_MMIO(0x6000600cu);output[9]=LINNET_MMIO(0x60006004u);
    output[10]=LINNET_MMIO(0x70000010u);output[11]=LINNET_MMIO(0x7000c100u);
    output[12]=LINNET_MMIO(0x7000c104u);output[13]=LINNET_MMIO(0x7000c120u);
    output[14]=LINNET_MMIO(0x60004120u);output[15]=LINNET_MMIO(0x60004100u);
}
#endif
