#include "hal/pp502x/target.h"
#include "linnet_target.h"
#if defined(LINNET_SOC_PP5022)
#include "hal/pp5022/usb_phy.h"
#endif
#include "boot/include/linnet_bytes.h"
#include "hal/api/usb.h"
#include "hal/pp502x/usb_storage.h"
#include "hal/api/platform.h"
#include "hal/api/power.h"
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#endif
#if defined(LINNET_IMAGE_OS) && defined(LINNET_AUDIO)
#define LINNET_USB_NEGOTIATED_CHARGE 1
#define LINNET_USB_MAX_POWER 250u /* bMaxPower uses 2mA units. */
#else
#define LINNET_USB_MAX_POWER 50u
#endif

/* PP5020 ARC device-controller register facts and IRAM DMA requirements were
 * checked against the local PP5020 reference drivers. This driver uses its own
 * single-transfer state machine, with no heap or external driver dependency. */
#define LINNET_MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define LINNET_USB_REG(o) LINNET_MMIO(0xc5000000u + (o))
#define LINNET_USB_CMD LINNET_USB_REG(0x140u)
#define LINNET_USB_STS LINNET_USB_REG(0x144u)
#define LINNET_USB_INTR LINNET_USB_REG(0x148u)
#define LINNET_USB_ADDR LINNET_USB_REG(0x154u)
#define LINNET_USB_LIST LINNET_USB_REG(0x158u)
#define LINNET_USB_PORT LINNET_USB_REG(0x184u)
#define LINNET_USB_MODE LINNET_USB_REG(0x1a8u)
#define LINNET_USB_SETUP LINNET_USB_REG(0x1acu)
#define LINNET_USB_PRIME LINNET_USB_REG(0x1b0u)
#define LINNET_USB_FLUSH LINNET_USB_REG(0x1b4u)
#define LINNET_USB_STATUS LINNET_USB_REG(0x1b8u)
#define LINNET_USB_DONE LINNET_USB_REG(0x1bcu)
#define LINNET_USB_EP(n) LINNET_USB_REG(0x1c0u + 4u * (n))
#define LINNET_USB_IRAM 0x4000c000u
#define LINNET_USB_WORKSPACE 0xe00u
#define LINNET_USB_RX ((volatile uint8_t *)(uintptr_t)0x4000c900u)
#define LINNET_USB_TX ((volatile uint8_t *)(uintptr_t)0x4000cb00u)
#define LINNET_USB_CTRL ((volatile uint8_t *)(uintptr_t)0x4000cd00u)
#define LINNET_USB_NOTIFY ((volatile uint8_t *)(uintptr_t)0x4000cd80u)
#define LINNET_USB_PACKET 512u
#define LINNET_USB_FULL_SPEED_PACKET 64u
#define LINNET_USB_TRANSFER_US 2000000u

typedef struct {
    uint32_t next, token, page[5], reserved;
} ln_usb_td;
typedef struct {
    uint32_t capability, current;
    ln_usb_td transfer;
    uint32_t setup[2], reserved[4];
} ln_usb_qh;
_Static_assert(sizeof(ln_usb_td) == 32, "USB dTD size");
_Static_assert(sizeof(ln_usb_qh) == 64, "USB queue-head size");
#define LINNET_USB_QH ((volatile ln_usb_qh *)(uintptr_t)LINNET_USB_IRAM)
#define LINNET_USB_TD ((volatile ln_usb_td *)(uintptr_t)(LINNET_USB_IRAM + 0x800u))

static struct {
    uint32_t generation, started[6];
    uint16_t requested[6], rx_size, rx_offset, rx_hint;
    uint8_t active[6], running, configured, control_stage, stopped, stop_attempted;
    uint8_t storage_mode, storage_reset;
    uint8_t pending_address, pending_configuration, notify_pending;
    uint8_t line[7], serial[66];
    uint16_t line_state;
    uint32_t failure, diagnostic[8], stop_diagnostics;
    uint8_t diagnostic_valid;
    uint8_t enumeration_timing, enumeration_retried, ever_configured;
    uint32_t enumeration_started;
} ln_usb;
static const uint8_t *storage_tx_pointer;
static uint16_t storage_tx_bytes;
static uint8_t *storage_rx_pointer;
static uint16_t storage_rx_bytes,storage_rx_ready;

enum { LINNET_USB_IDLE, LINNET_USB_DATA_IN, LINNET_USB_LINE_OUT,
       LINNET_USB_STATUS_IN, LINNET_USB_STATUS_OUT };

/* Experimental USB identity for Linnet Link. Native Disk Mode substitutes
 * its target PID. These identifiers do not imply an assigned production USB identity. */
static const uint8_t ln_usb_device[] = {
    18,1,0x10,1,2,0,0,64, 0x09,0x12,0x4a,0x50,0,1,1,2,3,1
};
static const uint8_t ln_usb_configuration[] = {
    9,2,67,0,2,1,0,0x80,LINNET_USB_MAX_POWER,
    9,4,0,0,1,2,2,1,0,
    5,0x24,0,0x10,1, 5,0x24,1,0,1, 4,0x24,2,2, 5,0x24,6,0,1,
    7,5,0x81,3,8,0,16,
    9,4,1,0,2,0x0a,0,0,0,
    7,5,0x82,2,64,0,0, 7,5,0x02,2,64,0,0
};
static const uint8_t ln_usb_storage_configuration[] = {
    9,2,32,0,1,1,0,0x80,LINNET_USB_MAX_POWER,
    9,4,0,0,2,8,6,0x50,0,
    7,5,0x82,2,64,0,0, 7,5,0x02,2,64,0,0
};
static const uint8_t ln_usb_language[] = {4,3,9,4};
static const uint8_t ln_usb_manufacturer[] = {14,3,'L',0,'i',0,'n',0,'n',0,'e',0,'t',0};
static const uint8_t ln_usb_product[] = {
    24,3,'L',0,'i',0,'n',0,'n',0,'e',0,'t',0,' ',0,'L',0,'i',0,'n',0,'k',0
};
static const uint8_t ln_usb_storage_product[] = {
    34,3,'L',0,'i',0,'n',0,'n',0,'e',0,'t',0,' ',0,
    'D',0,'i',0,'s',0,'k',0,' ',0,'M',0,'o',0,'d',0,'e',0
};

static bool storage_high_speed_capable(void) {
#if defined(LINNET_SOC_PP5022) || defined(LINNET_TARGET_IPOD4G_Q21) || \
    (defined(LINNET_TARGET_PHOTO4G_P98) && defined(LINNET_IMAGE_OS))
    return ln_usb.storage_mode != 0;
#else
    return false;
#endif
}
static uint32_t bulk_packet(void) {
    return storage_high_speed_capable() && (LINNET_USB_PORT & 0x0c000000u) == 0x08000000u
        ? LINNET_USB_PACKET : LINNET_USB_FULL_SPEED_PACKET;
}

#if defined(LINNET_USB_NEGOTIATED_CHARGE)
static uint16_t charge_budget = UINT16_MAX;
static void charge_limit(uint16_t budget) {
    if (budget != charge_budget && ln_power_charge_limit(budget)) charge_budget = budget;
}
static void charge_policy(bool suspend_event) {
    /* Local Rockbox ARC driver: PORTSC connect bit0, suspend bit7;
     * USBSTS suspend bit8. A suspend event wins even if PORTSC changes while
     * sampling. Only completed SET_CONFIGURATION(1) grants the500mA budget. */
    uint32_t port = LINNET_USB_PORT;
    if (suspend_event || (port & 0x80u) || !(port & 1u) ||
        !(ln_power_sources() & LINNET_POWER_USB)) charge_limit(0);
    else charge_limit(ln_usb.configured == 1u ? 500u : 100u);
}
#endif

static uint32_t pipe_bit(unsigned pipe) {
    return 1u << (pipe / 2u + ((pipe & 1u) ? 16u : 0u));
}

static void diagnostic_capture(uint32_t output[8], uint32_t origin) {
    output[0] = LINNET_USB_QH[5].transfer.token;
    output[1] = LINNET_USB_TD[5].token;
    output[2] = 0;
    for (unsigned i = 0; i < 6; ++i) output[2] |= (uint32_t)ln_usb.active[i] << i;
    output[3] = LINNET_USB_PRIME;
    output[4] = LINNET_USB_STATUS;
    output[5] = LINNET_USB_DONE;
    output[6] = (origin << 16) | ln_usb.failure;
    output[7] = ln_usb.generation;
}

static void diagnostic_save(uint32_t origin) {
    diagnostic_capture(ln_usb.diagnostic, origin);
    ln_usb.diagnostic_valid = 1;
}

void ln_usb_diagnostics(uint32_t output[8]) {
    if (!output) return;
    if (ln_usb.diagnostic_valid) {
        for (unsigned i = 0; i < 8; ++i) output[i] = ln_usb.diagnostic[i];
    } else if (ln_usb.running) diagnostic_capture(output, 0);
    else {
        for (unsigned i = 0; i < 8; ++i) output[i] = 0;
        output[6] = 0xffff0000u | ln_usb.failure;
        output[7] = ln_usb.generation;
    }
}

static bool wait_mask(volatile uint32_t *reg, uint32_t mask, bool set, uint32_t limit) {
    uint32_t started = ln_time_us();
    for (uint32_t budget = 0; budget < 2000000u; ++budget) {
        if (((*reg & mask) != 0) == set) return true;
        if (ln_time_us() - started >= limit) return false;
    }
    return false;
}

static bool settle(uint32_t delay) {
    uint32_t started = ln_time_us();
    for (uint32_t budget = 0; budget < 20000000u; ++budget)
        if (ln_time_us() - started >= delay) return true;
    return false;
}

static bool flush_pipes(uint32_t mask) {
    LINNET_USB_FLUSH = mask;
    if (!wait_mask(&LINNET_USB_FLUSH, mask, false, 20000u)) return false;
    /* ARC requires repeated flush if an endpoint raced an initial prime. */
    if (LINNET_USB_STATUS & mask) {
        LINNET_USB_FLUSH = mask;
        if (!wait_mask(&LINNET_USB_FLUSH, mask, false, 20000u) ||
            (LINNET_USB_STATUS & mask)) return false;
    }
    LINNET_USB_DONE = mask;
    for (unsigned i = 0; i < 6; ++i) if (mask & pipe_bit(i)) {
        if(i==4u){storage_rx_pointer=0;storage_rx_bytes=storage_rx_ready=0;}
        if(i==5u){storage_tx_pointer=0;storage_tx_bytes=0;}
        ln_usb.active[i] = 0;
        LINNET_USB_QH[i].transfer.next = 1;
        LINNET_USB_QH[i].transfer.token = 0;
        LINNET_USB_TD[i].next = 1;
        LINNET_USB_TD[i].token = 0;
    }
    return true;
}

static bool prime(unsigned pipe, volatile uint8_t *buffer, uint16_t size) {
    uint32_t bit = pipe_bit(pipe);
    if (ln_usb.active[pipe] || (LINNET_USB_PRIME & bit) || (LINNET_USB_STATUS & bit))
        return false;
    volatile ln_usb_td *td = &LINNET_USB_TD[pipe];
    uint32_t address = (uint32_t)(uintptr_t)buffer;
    td->next = 1;
    td->token = ((uint32_t)size << 16) | 0x8080u;
    td->page[0] = address;
    for (unsigned i = 1; i < 5; ++i) td->page[i] = (address & ~0xfffu) + i * 0x1000u;
    td->reserved = 0;
    LINNET_USB_QH[pipe].current = 0;
    LINNET_USB_QH[pipe].transfer.next = (uint32_t)(uintptr_t)td;
    LINNET_USB_QH[pipe].transfer.token = 0;
    ln_usb.requested[pipe] = size;
    ln_usb.started[pipe] = ln_time_us();
    ln_usb.active[pipe] = 1;
    __asm__ volatile("" ::: "memory");
    LINNET_USB_PRIME = bit;
    return true;
}

static void endpoints(void) {
    for (unsigned i = 0; i < 6; ++i) {
        LINNET_USB_QH[i].capability = (64u << 16) | 0x20000000u;
        LINNET_USB_QH[i].transfer.next = 1;
        LINNET_USB_QH[i].transfer.token = 0;
    }
    LINNET_USB_QH[0].capability |= 0x8000u; /* setup interrupt on OUT control */
    /* Control-IN ZLP policy is selected per request against its wLength. */
    LINNET_USB_QH[3].capability = (8u << 16) | 0x20000000u;
    LINNET_USB_QH[4].capability = LINNET_USB_QH[5].capability = (bulk_packet() << 16) | 0x20000000u;
    if (!ln_usb.storage_mode) LINNET_USB_QH[5].capability &= ~0x20000000u;
    LINNET_USB_EP(0) = 0x00800080u;
    LINNET_USB_EP(1) = ln_usb.storage_mode ? 0u : 0x00cc0000u;
    LINNET_USB_EP(2) = 0x00c800c8u;
}

static bool configure(uint8_t value) {
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
    if (!value) charge_limit(100);
#endif
    if (!flush_pipes(0x00060006u)) return false;
    if (ln_usb.storage_mode)
        LINNET_USB_QH[4].capability = LINNET_USB_QH[5].capability = (bulk_packet() << 16) | 0x20000000u;
    ln_usb.configured = value;
    if (value) ln_usb.ever_configured = 1;
    ln_usb.rx_size = ln_usb.rx_offset = ln_usb.rx_hint = 0;
    ln_usb.line_state = 0;
    ln_usb.notify_pending = 0;
    ln_usb.storage_reset = 0;
    ++ln_usb.generation;
    LINNET_USB_EP(2) = 0x00c800c8u;
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
    charge_policy(false);
#endif
    return true;
}

static bool control_reply(const uint8_t *data, uint16_t size, uint16_t wanted) {
    if (size > wanted) size = wanted;
    if (size > 128) return false;
    /* Append a terminating ZLP only when the descriptor is shorter than the
     * requested length and ends on a packet boundary. If size == wLength,
     * the host advances directly to status and will not consume another IN. */
    LINNET_USB_QH[1].capability |= 0x20000000u;
    if (size < wanted && size && size % LINNET_USB_FULL_SPEED_PACKET == 0)
        LINNET_USB_QH[1].capability &= ~0x20000000u;
    for (unsigned i = 0; i < size; ++i) LINNET_USB_CTRL[i] = data[i];
    ln_usb.control_stage = LINNET_USB_DATA_IN;
    return prime(1, LINNET_USB_CTRL, size);
}

static bool status_in(void) {
    LINNET_USB_QH[1].capability |= 0x20000000u;
    ln_usb.control_stage = LINNET_USB_STATUS_IN;
    return prime(1, LINNET_USB_CTRL, 0);
}

static bool setup_request(const uint8_t setup[8]) {
    uint8_t type = setup[0], request = setup[1];
    uint16_t value = ln_read_le16(setup + 2), index = ln_read_le16(setup + 4), length = ln_read_le16(setup + 6);
    ln_usb.pending_address = 0xffu;
    ln_usb.pending_configuration = 0xffu;
    LINNET_USB_EP(0) &= ~0x00010001u;
    if (type == 0x80 && request == 6) {
        const uint8_t *descriptor = 0;
        uint16_t size = 0;
        if (value == 0x100 && index == 0) {
            if (ln_usb.storage_mode) {
                uint8_t copy[18];
                for (unsigned i = 0; i < sizeof(copy); ++i) copy[i] = ln_usb_device[i];
                if(storage_high_speed_capable()){copy[2]=0;copy[3]=2;}
                copy[4] = 0;
                copy[10] = (uint8_t)LINNET_NATIVE_STORAGE_USB_PID;
                copy[11] = (uint8_t)(LINNET_NATIVE_STORAGE_USB_PID >> 8);
                return control_reply(copy, sizeof(copy), length);
            }
            descriptor = ln_usb_device; size = sizeof(ln_usb_device);
        }
        if (storage_high_speed_capable() && value == 0x600 && index == 0) {
            const uint8_t qualifier[] = {10,6,0,2,0,0,0,64,1,0};
            return control_reply(qualifier,sizeof(qualifier),length);
        }
        if (ln_usb.storage_mode && index == 0 &&
            (value == 0x200 || (value == 0x700 && storage_high_speed_capable()))) {
            uint8_t copy[sizeof(ln_usb_storage_configuration)];
            for(unsigned i=0;i<sizeof(copy);++i)copy[i]=ln_usb_storage_configuration[i];
            bool high=bulk_packet()==512u;
            if(value==0x700){high=!high;copy[1]=7;}
            copy[22]=copy[29]=high?0:64;copy[23]=copy[30]=high?2:0;
            return control_reply(copy,sizeof(copy),length);
        }
        if (value == 0x200 && index == 0) {
            descriptor = ln_usb.storage_mode ? ln_usb_storage_configuration : ln_usb_configuration;
            size = ln_usb.storage_mode ? sizeof(ln_usb_storage_configuration) : sizeof(ln_usb_configuration);
        }
        if (value == 0x300 && index == 0) { descriptor = ln_usb_language; size = sizeof(ln_usb_language); }
        if (index == 0x409) {
            if (value == 0x301) descriptor = ln_usb_manufacturer;
            if (value == 0x302) descriptor = ln_usb.storage_mode ? ln_usb_storage_product : ln_usb_product;
            if (value == 0x303) descriptor = ln_usb.serial;
            if (descriptor) size = descriptor[0];
        }
        return descriptor && control_reply(descriptor, size, length);
    }
    if (type == 0 && request == 5 && value < 128 && !index && !length) {
        ln_usb.pending_address = (uint8_t)value;
        return status_in();
    }
    if (type == 0 && request == 9 && value <= 1 && !index && !length) {
        ln_usb.pending_configuration = (uint8_t)value;
        return status_in();
    }
    if (type == 0x80 && request == 8 && !value && !index && length == 1)
        return control_reply(&ln_usb.configured, 1, length);
    if ((type == 0x80 || type == 0x81 || type == 0x82) && request == 0 && !value && length == 2) {
        uint8_t status[2] = {0,0};
        if (type == 0x80 && index != 0) return false;
        if (type == 0x81 && (index > (ln_usb.storage_mode ? 0u : 1u) || !ln_usb.configured)) return false;
        if (type == 0x82) {
            if (index != 0 && index != 0x80 && index != 0x82 && index != 2 &&
                !(index == 0x81 && !ln_usb.storage_mode)) return false;
            uint32_t stall = (index & 0x80u) ? 0x10000u : 1u;
            status[0] = (LINNET_USB_EP(index & 0xfu) & stall) ? 1 : 0;
        }
        return control_reply(status, 2, length);
    }
    if (type == 2 && (request == 1 || request == 3) && !value && !length &&
        (index == 0x82 || index == 2 || (index == 0x81 && !ln_usb.storage_mode)) && ln_usb.configured) {
        uint32_t stall = (index & 0x80u) ? 0x10000u : 1u;
        uint32_t toggle = (index & 0x80u) ? 0x400000u : 0x40u;
        if (request == 3) LINNET_USB_EP(index & 0xfu) |= stall;
        else LINNET_USB_EP(index & 0xfu) = (LINNET_USB_EP(index & 0xfu) & ~stall) | toggle;
        return status_in();
    }
    if (type == 0x81 && request == 10 && !value && index <= (ln_usb.storage_mode ? 0u : 1u) && length == 1 && ln_usb.configured) {
        uint8_t zero = 0;
        return control_reply(&zero, 1, length);
    }
    if (type == 1 && request == 11 && !value && index <= (ln_usb.storage_mode ? 0u : 1u) && !length && ln_usb.configured)
        return status_in();
    if (index != 0 || !ln_usb.configured) return false;
    if (ln_usb.storage_mode) {
        if (type == 0xa1 && request == 0xfe && !value && length == 1) {
            uint8_t zero = 0;
            return control_reply(&zero, 1, length);
        }
        if (type == 0x21 && request == 0xff && !value && !length) {
            ln_usb.storage_reset = 1;
            return true; /* ATA owner completes EP0 status after draining. */
        }
        return false;
    }
    if (type == 0xa1 && request == 0x21 && !value && length == 7)
        return control_reply(ln_usb.line, 7, 7);
    if (type == 0x21 && request == 0x20 && !value && length == 7) {
        ln_usb.control_stage = LINNET_USB_LINE_OUT;
        return prime(0, LINNET_USB_CTRL, 7);
    }
    if (type == 0x21 && request == 0x22 && value <= 3 && !length) {
        /* Opening/closing a serial handle starts/ends the Link sequence space
         * without necessarily resetting the USB device or its configuration. */
        if ((ln_usb.line_state ^ value) & 1u) {
            /* Keep the interrupted state across the next rising DTR edge.
             * An idle reopen must not erase a more useful pending-TX record. */
            if (!(value & 1u) && (!ln_usb.diagnostic_valid || ln_usb.active[5] ||
                (ln_usb.active[4] && !(LINNET_USB_TD[4].token & 0x80u))))
                diagnostic_save(1);
            if (!flush_pipes(0x00040004u)) return false;
            ln_usb.rx_size = ln_usb.rx_offset = 0;
            ln_usb.rx_hint = 0;
            ++ln_usb.generation;
        }
        ln_usb.line_state = value;
        ln_usb.notify_pending = 1;
        return status_in();
    }
    if (type == 0x21 && request == 0x23 && !length) return status_in(); /* SEND_BREAK */
    return false;
}

static bool complete(unsigned pipe) {
    if (!ln_usb.active[pipe]) return true;
    uint32_t token = LINNET_USB_TD[pipe].token;
    /* PP5020 ARC can publish ENDPTCOMPLETE while a dTD is still active.
     * Retain DMA ownership until its ACTIVE bit clears; do not disconnect or
     * expose partially received data on this early completion indication. */
    if (token & 0x80u) return true;
    uint32_t remaining = (token >> 16) & 0x7fffu;
    if ((token & 0x68u) || remaining > ln_usb.requested[pipe]) {
        ln_usb.failure = 0x100u | pipe;
        return false;
    }
    uint32_t received = ln_usb.requested[pipe] - remaining;
    ln_usb.active[pipe] = 0;
    if (pipe == 4) {
        if(storage_rx_bytes){
            if(received!=storage_rx_bytes){ln_usb.failure=0x206u;return false;}
            storage_rx_ready=(uint16_t)received;
        }else{ln_usb.rx_size=(uint16_t)received;ln_usb.rx_offset=0;}
    }
    if (pipe == 5 && remaining) { ln_usb.failure = 0x205u; return false; }
    if (pipe > 1) return true;
    if (pipe == 1 && remaining) return false;
    if (pipe == 1 && ln_usb.control_stage == LINNET_USB_DATA_IN) {
        ln_usb.control_stage = LINNET_USB_STATUS_OUT;
        return prime(0, LINNET_USB_CTRL, 0);
    }
    if (pipe == 0 && ln_usb.control_stage == LINNET_USB_LINE_OUT) {
        if (received != 7) return false;
        for (unsigned i = 0; i < 7; ++i) ln_usb.line[i] = LINNET_USB_CTRL[i];
        return status_in();
    }
    if (pipe == 1 && ln_usb.control_stage == LINNET_USB_STATUS_IN) {
        if (ln_usb.pending_address != 0xffu) LINNET_USB_ADDR = (uint32_t)ln_usb.pending_address << 25;
        if (ln_usb.pending_configuration != 0xffu && !configure(ln_usb.pending_configuration)) return false;
    }
    ln_usb.control_stage = LINNET_USB_IDLE;
    return true;
}

static bool controller_start(void);
static void enumeration_progress(void) {
#if !defined(LINNET_PROFILE_RELEASE)
    /* Only a completed retry stage advances the watchdog, never a wait loop. */
    if (ln_usb.enumeration_retried) ln_recovery_feed();
#endif
}

static bool usb_init(const char *stable_serial) {
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
    /* Drop any inherited high-current selection before enumeration. */
    charge_budget = UINT16_MAX; charge_limit(100);
#endif
    if (!stable_serial) return false;
    unsigned length = 0;
    for (; length < 33 && stable_serial[length]; ++length)
        if ((uint8_t)stable_serial[length] < 0x21 || (uint8_t)stable_serial[length] > 0x7e) return false;
    if (!length || length > 32) return false;
    ln_usb.serial[0] = (uint8_t)(2 + length * 2); ln_usb.serial[1] = 3;
    for (unsigned i = 0; i < length; ++i) {
        ln_usb.serial[2 + i * 2] = (uint8_t)stable_serial[i]; ln_usb.serial[3 + i * 2] = 0;
    }
    ln_usb.enumeration_timing = ln_usb.enumeration_retried = ln_usb.ever_configured = 0;
    return controller_start();
}

static bool controller_start(void) {
    ln_usb.running = ln_usb.configured = ln_usb.stopped = ln_usb.stop_attempted = 0;
    ln_target_usb_prepare();
    LINNET_MMIO(0x7000002cu) |= 0x03000000u;
    LINNET_MMIO(0x6000600cu) |= 0x00400008u;
    LINNET_MMIO(0x60006004u) |= 8u; LINNET_MMIO(0x60006004u) &= ~8u;
    LINNET_MMIO(0x60006004u) |= 0x00400000u; LINNET_MMIO(0x60006004u) &= ~0x00400000u;
    LINNET_MMIO(0x70000020u) |= 0x80000000u;
    if (!wait_mask(&LINNET_MMIO(0x70000028u), 0x80u, true, 100000u)) goto failed;
    enumeration_progress();
    LINNET_MMIO(0x70000028u) |= 2u;
    if (!settle(100000u)) goto failed;
    enumeration_progress();
    LINNET_MMIO(0x7000003cu) |= 0x47au;
    LINNET_USB_CMD &= ~1u;
    if (!settle(250000u)) goto failed;
    enumeration_progress();
    LINNET_USB_CMD |= 2u;
    if (!wait_mask(&LINNET_USB_CMD, 2u, false, 100000u)) goto failed;
    enumeration_progress();
#if defined(LINNET_SOC_PP5022)
    if(!ln_pp5022_usb_phy_reset())goto failed;
#endif
    LINNET_USB_MODE = 0x12u; /* device, streaming off, setup lockout enabled */
    if(storage_high_speed_capable())LINNET_USB_PORT &= ~0x01000000u;
    else LINNET_USB_PORT |= 0x01000000u; /* Link retains full speed. */
    volatile uint32_t *workspace = (volatile uint32_t *)(uintptr_t)LINNET_USB_IRAM;
    for (unsigned i = 0; i < LINNET_USB_WORKSPACE / 4u; ++i) workspace[i] = 0;
    for (unsigned i = 0; i < 6; ++i) ln_usb.active[i] = 0;
    storage_tx_pointer=0;storage_tx_bytes=0;
    storage_rx_pointer=0;storage_rx_bytes=storage_rx_ready=0;
    endpoints();
    ln_usb.control_stage = LINNET_USB_IDLE;
    ln_usb.pending_address = ln_usb.pending_configuration = 0xffu;
    ln_usb.rx_size = ln_usb.rx_offset = ln_usb.rx_hint = 0;
    ln_usb.line_state = 0; ln_usb.notify_pending = 0;
    ln_usb.line[0] = 0; ln_usb.line[1] = 0xc2; ln_usb.line[2] = 1;
    ln_usb.line[3] = ln_usb.line[4] = ln_usb.line[5] = 0; ln_usb.line[6] = 8;
    LINNET_USB_LIST = LINNET_USB_IRAM;
    LINNET_USB_ADDR = 0;
    LINNET_USB_INTR = 0; /* polled owner, no asynchronous shared state */
    LINNET_USB_SETUP = 0xffffffffu; LINNET_USB_DONE = 0xffffffffu;
    LINNET_USB_STS = 0xffffffffu;
    LINNET_USB_CMD |= 1u;
    ln_usb.running = 1;
    ++ln_usb.generation;
    return true;
failed:
    ln_usb_disconnect();
    return false;
}

bool ln_usb_connected(void) {
    return ln_usb.running && ln_usb.configured &&
        (ln_power_sources() & LINNET_POWER_USB) && (LINNET_USB_PORT & 1u);
}

uint32_t ln_usb_generation(void) { return ln_usb.generation; }

void ln_usb_irq_enable(void) {
    if (ln_usb.running) LINNET_USB_INTR =
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
        0x147u;
#else
        0x47u;
#endif
}

static bool enumeration_recover(uint32_t status) {
    /* One fresh attachment can recover a missed handoff enumeration. Never
     * disturb a session that has configured, even after a later bus reset. */
    if (ln_usb.ever_configured || ln_usb.enumeration_retried) return true;
    if (!(ln_power_sources() & LINNET_POWER_USB) ||
        (status & 0x100u) || (LINNET_USB_PORT & 0x80u)) {
        ln_usb.enumeration_timing = 0;
        return true;
    }
    uint32_t now = ln_time_us();
    if (!ln_usb.enumeration_timing) {
        ln_usb.enumeration_started = now;
        ln_usb.enumeration_timing = 1;
        return true;
    }
    if (now - ln_usb.enumeration_started < 5000000u) return true;
    ln_usb.enumeration_retried = 1;
    ln_usb.enumeration_timing = 0;
    /* Stop must release DMA before rebuilding descriptors. The private start
     * preserves both the owned serial descriptor and the one-shot latch, and
     * calls no poll/progress callbacks that could reenter this owner. */
    uint32_t interrupts = LINNET_USB_INTR;
    enumeration_progress();
    if (!ln_usb_stop()) return false;
    enumeration_progress();
    if (!controller_start()) return false;
    LINNET_USB_INTR = interrupts;
    enumeration_progress();
    return true;
}

bool ln_usb_poll(void) {
    if (!ln_usb.running) return false;
    uint32_t status = LINNET_USB_STS;
    LINNET_USB_STS = status;
    if (status & 0x40u) {
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
        ln_usb.configured = 0; charge_limit(100);
#endif
        LINNET_USB_ADDR = 0;
        LINNET_USB_SETUP = LINNET_USB_SETUP;
        if (!wait_mask(&LINNET_USB_PRIME, 0xffffffffu, false, 20000u)) goto failed;
        if (!flush_pipes(0xffffffffu)) goto failed;
        ln_usb.configured = 0; ln_usb.rx_size = ln_usb.rx_offset = ln_usb.rx_hint = 0;
        ln_usb.control_stage = LINNET_USB_IDLE;
        ln_usb.pending_address = ln_usb.pending_configuration = 0xffu;
        ln_usb.line_state = 0; ln_usb.notify_pending = 0;
        ln_usb.storage_reset = 0;
        LINNET_USB_ADDR = 0; endpoints(); ++ln_usb.generation;
    }
    if (status & 2u) { ln_usb.failure = 0x300u; goto failed; }
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
    charge_policy((status & 0x100u) != 0);
    if ((status & 0x100u) || (LINNET_USB_PORT & 0x80u)) return enumeration_recover(status);
    if (!(LINNET_USB_PORT & 1u)) {
        if (ln_usb.configured && !configure(0)) goto failed;
        charge_limit(0); return enumeration_recover(status);
    }
#endif
    if (!(ln_power_sources() & LINNET_POWER_USB)) {
        if (ln_usb.configured && !configure(0)) goto failed;
        return enumeration_recover(status);
    }
    uint32_t done = LINNET_USB_DONE;
    LINNET_USB_DONE = done;
    /* Retire a completed status ACK before a simultaneously arriving SETUP. */
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t bit = pipe_bit(i);
        /* An early DONE edge may have been acknowledged while ACTIVE was
         * still set. Revisit owned descriptors after hardware retires them,
         * without depending on another interrupt edge. Never retire an idle
         * descriptor or a newly primed transfer twice. */
        bool retired = ln_usb.active[i] && !(LINNET_USB_TD[i].token & 0x80u) &&
            !(LINNET_USB_PRIME & bit) && !(LINNET_USB_STATUS & bit);
        if (((done & bit) || retired) && !complete(i)) goto failed;
    }
    if (LINNET_USB_SETUP & 1u) {
        uint8_t setup[8];
        uint32_t words[2] = {LINNET_USB_QH[0].setup[0], LINNET_USB_QH[0].setup[1]};
        for (unsigned i = 0; i < 8; ++i) setup[i] = (uint8_t)(words[i / 4] >> (8u * (i % 4)));
        LINNET_USB_SETUP = 1u;
        if (!flush_pipes(0x00010001u)) goto failed;
        if (!setup_request(setup)) {
            LINNET_USB_EP(0) |= 0x00010001u;
            ln_usb.control_stage = LINNET_USB_IDLE;
        }
    }
    if (ln_usb_connected() && !ln_usb.storage_reset) {
        /* Start each Link frame with one packet: exact 64-byte commands need
         * no ZLP. Once its header is validated, receive whole packets within
         * the known remainder without waiting beyond that frame's boundary. */
        uint32_t receive_size=bulk_packet();
        if(!ln_usb.storage_mode && ln_usb.rx_hint>receive_size)receive_size=ln_usb.rx_hint;
        if (!ln_usb.active[4] && !ln_usb.rx_size && !storage_rx_bytes &&
            !prime(4, LINNET_USB_RX, (uint16_t)receive_size)) goto failed;
        if (!ln_usb.storage_mode && ln_usb.notify_pending && !ln_usb.active[3]) {
            const uint8_t note[10] = {0xa1,0x20,0,0,0,0,2,0,3,0};
            for (unsigned i = 0; i < 10; ++i) LINNET_USB_NOTIFY[i] = note[i];
            if (!prime(3, LINNET_USB_NOTIFY, 10)) goto failed;
            ln_usb.notify_pending = 0;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        /* OUT/notification endpoints may legitimately wait indefinitely for
         * host traffic; their service call is still constant-time. */
        if (ln_usb.active[i] && i != 4 && i != 3 &&
            ln_time_us() - ln_usb.started[i] >= LINNET_USB_TRANSFER_US) {
            ln_usb.failure = 0x400u | i;
            goto failed;
        }
        if (ln_usb.active[i] && (LINNET_USB_PRIME & pipe_bit(i)) &&
            ln_time_us() - ln_usb.started[i] >= 20000u) {
            ln_usb.failure = 0x500u | i;
            goto failed;
        }
    }
    return enumeration_recover(status);
failed:
    diagnostic_save(2);
    ln_usb_disconnect();
    return false;
}

bool ln_usb_rx_pending(void) { return ln_usb.rx_size != ln_usb.rx_offset; }

void ln_usb_rx_hint(uint32_t remaining) {
    if(remaining>LINNET_USB_PACKET)remaining=LINNET_USB_PACKET;
    ln_usb.rx_hint=(uint16_t)(remaining&~(LINNET_USB_FULL_SPEED_PACKET-1u));
}

size_t ln_usb_read(uint8_t *output, size_t capacity) {
    if (!output || !ln_usb_connected()) return 0;
    size_t available = (size_t)(ln_usb.rx_size - ln_usb.rx_offset);
    if (capacity > available) capacity = available;
    for (size_t i = 0; i < capacity; ++i) output[i] = LINNET_USB_RX[ln_usb.rx_offset + i];
    ln_usb.rx_offset += (uint16_t)capacity;
    if (ln_usb.rx_offset == ln_usb.rx_size) ln_usb.rx_offset = ln_usb.rx_size = 0;
    return capacity;
}

size_t ln_usb_write(const uint8_t *input, size_t length) {
    if (!input || !length || !ln_usb_connected() || ln_usb.active[5]) return 0;
    if (length > LINNET_USB_PACKET) length = LINNET_USB_PACKET;
    for (size_t i = 0; i < length; ++i) LINNET_USB_TX[i] = input[i];
    return prime(5, LINNET_USB_TX, (uint16_t)length) ? length : 0;
}

bool ln_usb_flush(uint32_t deadline_us) {
    if ((uint32_t)(deadline_us - ln_time_us()) > LINNET_USB_TRANSFER_US) return false;
    uint32_t generation = ln_usb.generation;
    for (uint32_t budget = 0; budget < 2000000u; ++budget) {
        if (!ln_usb_connected() || generation != ln_usb.generation || !ln_usb_poll()) return false;
        if (!ln_usb_connected() || generation != ln_usb.generation) return false;
        if (!ln_usb.active[5]) return true;
        if ((int32_t)(ln_time_us() - deadline_us) >= 0) {
            ln_usb.failure = 0x600u;
            diagnostic_save(3);
            return false;
        }
    }
    ln_usb.failure = 0x601u;
    diagnostic_save(3);
    return false;
}

bool ln_usb_stop(void) {
#if defined(LINNET_USB_NEGOTIATED_CHARGE)
    /* Capture the bus state before stopping/resetting ARC. A connected awake
     * host permits the unconfigured100mA budget across ROM/Boot handoff.
     * Leaving LTC4066 SUSP asserted would remove external power through that
     * handoff; neither the next loader nor ROM is assumed to restore it.
     * Suspend/detach must still draw no USB power, and500mA never survives. */
    if (!ln_usb.stop_attempted) {
        uint32_t port = LINNET_USB_PORT;
        bool awake = (ln_power_sources() & LINNET_POWER_USB) && (port & 1u) &&
            !(port & 0x80u) && !(LINNET_USB_STS & 0x100u);
        charge_limit(awake ? 100u : 0u);
    }
#endif
    ln_usb.running = ln_usb.configured = 0;
    ++ln_usb.generation;
    /* This owner never restarts the controller until init clears the latch.
     * Repeated quiesce calls reuse the proof and perform no peripheral MMIO. */
    if (ln_usb.stop_attempted) return ln_usb.stopped != 0;
    ln_usb.stop_attempted = 1;
    ln_usb.stop_diagnostics = 0;
    LINNET_USB_INTR = 0;
    LINNET_USB_CMD &= ~1u;
    if (!(LINNET_USB_CMD & 1u)) ln_usb.stop_diagnostics |= 1u;
    bool flushed = flush_pipes(0xffffffffu);
    if (flushed) ln_usb.stop_diagnostics |= 2u;
    /* Match the proven PP5020 stop-to-reset interval. A cleared RUN bit is
     * not itself proof that the controller's bus transactions have drained. */
    bool settled = settle(50000u);
    if (settled) ln_usb.stop_diagnostics |= 32u;
    LINNET_USB_CMD |= 2u;
    bool reset = wait_mask(&LINNET_USB_CMD, 2u, false, 100000u);
    if (reset) ln_usb.stop_diagnostics |= 4u;
    bool phy_ready = reset;
#if defined(LINNET_SOC_PP5022)
    /* The PP5022 PHY latch must be restored after every ARC reset, including
     * shutdown before a warm reboot into Apple's Disk Mode. */
    phy_ready = reset && ln_pp5022_usb_phy_reset();
    if (phy_ready) ln_usb.stop_diagnostics |= 64u;
#endif
    bool idle = reset && !(LINNET_USB_CMD & 3u) &&
        !(LINNET_USB_PRIME | LINNET_USB_STATUS | LINNET_USB_FLUSH);
    if (idle) ln_usb.stop_diagnostics |= 8u;
    /* RUN clear, completed ARC reset and idle endpoint registers prove no
     * descriptor can perform DMA. PP5020 boot handoff deliberately retains
     * USB module clocks: the established bootloader path warns that disabling
     * USB0 breaks the firmware loader/pin detection. Sticky module-reset and
     * PHY power readbacks are not DMA admission criteria. */
    ln_usb.rx_size = ln_usb.rx_offset = 0;
    for (unsigned i = 0; i < 6; ++i) ln_usb.active[i] = 0;
    storage_tx_pointer=0;storage_tx_bytes=0;
    storage_rx_pointer=0;storage_rx_bytes=storage_rx_ready=0;
    ln_usb.stopped = settled && reset && phy_ready && idle;
    return ln_usb.stopped != 0;
}

bool ln_usb_init(const char *stable_serial) {
    ln_usb.storage_mode = 0;
    return usb_init(stable_serial);
}

bool ln_pp_usb_storage_init(const char *stable_serial) {
    ln_usb.storage_mode = 1;
    return usb_init(stable_serial);
}

uint32_t ln_usb_stop_diagnostics(void) { return ln_usb.stop_diagnostics; }

void ln_usb_disconnect(void) { (void)ln_usb_stop(); }

size_t ln_pp_usb_storage_read(uint8_t *output, size_t capacity) {
    if (!ln_usb.storage_mode || ln_usb.storage_reset) return 0;
    /* A WRITE batch owns this uncached buffer until its OUT dTD retires.
     * Keep CBWs and short responses on the packet-sized shared receiver. */
    if(bulk_packet()!=512u || capacity<512u)return ln_usb_read(output,capacity);
    if(storage_rx_bytes){
        if(!storage_rx_ready)return 0;
        if(output!=storage_rx_pointer || capacity<storage_rx_ready){
            ln_usb.failure=0x207u;ln_usb_disconnect();return 0;
        }
        size_t received=storage_rx_ready;
        storage_rx_pointer=0;storage_rx_bytes=storage_rx_ready=0;
        output+=received;capacity-=received;
        if(capacity>=512u && !ln_usb.active[4] && !ln_usb.rx_size){
            size_t span=capacity>16384u?16384u:capacity;
            span&=~511u;
            if(prime(4,(volatile uint8_t *)ln_platform_uncached_address((uintptr_t)output),
                     (uint16_t)span)){
                storage_rx_pointer=output;storage_rx_bytes=(uint16_t)span;
            }
        }
        return received;
    }
    size_t copied=ln_usb_read(output,capacity);
    if(capacity-copied>=512u && !ln_usb.active[4] && !ln_usb.rx_size){
        size_t span=capacity-copied;
        if(span>16384u)span=16384u;
        span&=~511u;
        uint8_t *next=output+copied;
        if(prime(4,(volatile uint8_t *)ln_platform_uncached_address((uintptr_t)next),
                 (uint16_t)span)){
            storage_rx_pointer=next;storage_rx_bytes=(uint16_t)span;
        }
    }
    return copied;
}

size_t ln_pp_usb_storage_write(const uint8_t *input, size_t length) {
    if (!ln_usb.storage_mode || ln_usb.storage_reset) return 0;
    if(bulk_packet()==512u){
        /* Disk Mode supplies uncached buffers. Retain the caller's buffer until
         * DMA completes, so the next ATA read cannot overwrite an active send. */
        if(storage_tx_bytes){
            if(ln_usb.active[5]||input!=storage_tx_pointer||length<storage_tx_bytes)return 0;
            size_t sent=storage_tx_bytes;storage_tx_pointer=0;storage_tx_bytes=0;return sent;
        }
        if(!input||!length||!ln_usb_connected()||ln_usb.active[5])return 0;
        /* One dTD describes five 4 KiB pages; a 16 KiB span fits even when
         * the ATA buffer begins partway into its first page. */
        if(length>16384u)length=16384u;
        volatile uint8_t *physical=(volatile uint8_t *)ln_platform_uncached_address((uintptr_t)input);
        if(prime(5,physical,(uint16_t)length)){storage_tx_pointer=input;storage_tx_bytes=(uint16_t)length;}
        return 0;
    }
    return ln_usb_write(input, length);
}

bool ln_pp_usb_storage_zlp(void) {
    return ln_usb.storage_mode && !ln_usb.storage_reset && ln_usb_connected() &&
        !ln_usb.active[5] && prime(5, LINNET_USB_TX, 0);
}

size_t ln_pp_usb_storage_packet_size(void) { return bulk_packet(); }

void ln_pp_usb_storage_stall(bool input) {
    if (ln_usb.storage_mode) LINNET_USB_EP(2) |= input ? 0x10000u : 1u;
}

bool ln_pp_usb_storage_reset_pending(void) { return ln_usb.storage_reset != 0; }

bool ln_pp_usb_storage_reset_complete(void) {
    if (!ln_usb.storage_reset || !flush_pipes(0x00040004u)) return false;
    ln_usb.rx_size = ln_usb.rx_offset = 0;
    ln_usb.storage_reset = 0;
    return status_in();
}
