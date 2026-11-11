#include "hal/api/recovery.h"
#include "hal/pp502x/recovery_internal.h"
#include "hal/api/platform.h"
#include "boot/include/linnet_boot.h"
#include "boot/include/loader.h"
#include "hal/pp502x/disk_mode.h"
#include "linnet_build.h"
uint32_t ln_recovery_reboot_capabilities(void){return LINNET_LOADER_CAP_REBOOT_OS|LINNET_LOADER_CAP_REBOOT_DISK;}
#if defined(LINNET_AUDIO)
#include "hal/api/core.h"
#endif

#if defined(LINNET_STORAGE)
#include "services/storage/storage.h"
#endif
#define LINNET_MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define LINNET_NATIVE(a) ((a) | 0x10000000u)
#define LINNET_RECOVERY_MAGIC 0x4c4e5256u
struct ln_recovery_control {
    uint32_t magic, snapshot_size, snapshot_crc, generation;
    uint32_t progress, armed, cop_cookie, cop_tick, reason;
    uint8_t build_id[32], image_sha[32];
    uint32_t returned, candidate_address, candidate_size, candidate_entry;
    uint32_t candidate_crc, candidate_healthy;
    uint32_t trace_magic, trace_phase, trace_generation, trace_size;
    uint32_t trace_status, trace_started, trace_elapsed, trace_failure;
    /* Offset 160 is also accessed directly by the COP display mailbox. */
    uint32_t reserved_156, display_capability, reserved_164_188[7];
    uint32_t startup[16];
    uint32_t timeline[48];
};
_Static_assert(offsetof(struct ln_recovery_control, progress) == 16, "COP progress layout");
_Static_assert(offsetof(struct ln_recovery_control, armed) == 20, "COP armed layout");
_Static_assert(offsetof(struct ln_recovery_control, cop_cookie) == 24, "COP cookie layout");
_Static_assert(offsetof(struct ln_recovery_control, cop_tick) == 28, "COP tick layout");
_Static_assert(sizeof(struct ln_recovery_control) <= 4096, "Recovery control capacity");
_Static_assert(offsetof(struct ln_recovery_control, trace_phase) == 128, "Entry trace layout");
_Static_assert(offsetof(struct ln_recovery_control, trace_failure) == 152, "COP trace layout");
_Static_assert(offsetof(struct ln_recovery_control, display_capability) == 160, "COP display capability layout");
_Static_assert(offsetof(struct ln_recovery_control, startup) == LN_STARTUP_TIMING_BASE, "Startup trailer layout");
_Static_assert(offsetof(struct ln_recovery_control, startup[4]) == LN_STARTUP_TIMING_MASK, "Startup mask layout");
_Static_assert(offsetof(struct ln_recovery_control, startup[5]) == LN_STARTUP_TIMING_TICKS, "Startup ticks layout");
_Static_assert(offsetof(struct ln_recovery_control, timeline) == LN_BOOT_TIMELINE_BASE, "Boot timeline layout");
static volatile struct ln_recovery_control *const ln_control =
    (volatile struct ln_recovery_control *)(uintptr_t)0x11ffe000u;
static int ln_running_loader;
static int ln_timeline_native_pending;
#define LN_BOOT_TIMELINE_MAGIC 0x4d495442u
extern const uint8_t __image_end[], ln_copy_trampoline_begin[], ln_copy_trampoline_end[];

__attribute__((weak)) int ln_recovery_transport_stop(void) { return 1; }
__attribute__((weak)) int ln_recovery_presentation_stop(void) { return 1; }
__attribute__((weak)) uint32_t ln_recovery_transport_diagnostics(void) { return 0; }
static void ln_crc_progress(void *context) { (void)context; ln_recovery_feed(); }
static uint32_t ln_recovery_crc(const uint8_t *bytes, uint32_t length)
{
    ln_recovery_feed();
    return ln_crc32_progress(bytes, length, ln_crc_progress, 0);
}

static uint32_t ln_candidate_crc(const uint8_t *bytes,uint32_t length)
{
#if defined(LINNET_OS_CRC_IRAM)
    /* Only called after successful quiescence: audio/COP owners are stopped,
     * cache is disabled, and this path proceeds to the nonreturning copy. */
    (void)ln_crc32_os_iram_init();
#endif
    return ln_recovery_crc(bytes,length);
}

static void ln_trace_phase(uint32_t phase, uint32_t failure, uint32_t status)
{
    if (ln_control->trace_magic != 0x4c4e5452u) {
        ln_control->trace_started = ln_time_us();
        ln_control->trace_failure = 0;
        ln_control->trace_status = 0;
        ln_control->trace_magic = 0x4c4e5452u;
    }
    ln_control->trace_phase = phase;
    ln_control->trace_generation = ln_control->generation;
    ln_control->trace_size = ln_control->candidate_size;
    ln_control->trace_elapsed = ln_time_us() - ln_control->trace_started;
    if (failure && !ln_control->trace_failure) {
        ln_control->trace_failure = failure;
        ln_control->trace_status = status;
    }
}

void ln_recovery_diagnostics(uint32_t output[8])
{
    if (!output) return;
    for (uint32_t i = 0; i < 8u; ++i) output[i] = 0;
    if (ln_control->trace_magic != 0x4c4e5452u) return;
    output[0] = ln_control->reason;
    output[1] = ln_control->trace_phase;
    output[2] = ln_control->trace_generation;
    output[3] = ln_control->trace_size;
    output[4] = ln_control->trace_status;
    output[5] = ln_control->trace_failure;
    output[6] = ln_control->trace_elapsed;
    output[7] = ln_control->generation;
}

void ln_recovery_startup_diagnostics(uint32_t output[16])
{
    if(!output)return;
    for(uint32_t i=0;i<16u;++i)output[i]=0;
    if(ln_control->startup[0]!=LN_STARTUP_TIMING_MAGIC||ln_control->startup[1]!=0u||
       ln_control->startup[2]!=ln_control->generation||
       ln_control->startup[3]!=ln_control->candidate_size)return;
    for(uint32_t i=0;i<16u;++i)output[i]=ln_control->startup[i];
}

static void ln_startup_stamp(uint32_t index)
{
    ln_control->startup[5u+index]=ln_time_us();
    ln_control->startup[4]|=1u<<index;
}

static void ln_timeline_reset(void)
{
    ln_timeline_native_pending=0;
    ln_control->timeline[0]=0;
    for(uint32_t i=1;i<48u;++i)ln_control->timeline[i]=0;
    ln_control->timeline[1]=0u;
    ln_control->timeline[2]=ln_control->generation;
    ln_control->timeline[4]=LINNET_MMIO(0x60006020u);
    ln_control->timeline[5]=LINNET_MMIO(0x60006034u);
    ln_control->timeline[6]=LINNET_MMIO(0x6000c000u);
    ln_control->timeline[0]=LN_BOOT_TIMELINE_MAGIC;
}
void ln_recovery_timeline_stamp(uint32_t stage)
{
    if(stage>=28u||ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[2]!=ln_control->generation)return;
    uint32_t bit=1u<<stage;
    if(ln_control->timeline[3]&bit)return;
    ln_control->timeline[8u+stage]=ln_time_us();
    ln_control->timeline[3]|=bit;
    if(stage==LN_BT_BOOT_VALIDATED&&ln_running_loader)ln_timeline_native_pending=1;
}
void ln_recovery_timeline_begin(int loader,uint32_t c_entry)
{
    /* An OS can inherit only a newly dispatched generation. Old Boot and direct
     * launches produce an OS-only record, never stale native-startup numbers. */
    if(loader||ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[1]!=0u||ln_control->timeline[2]!=ln_control->generation||
       !(ln_control->timeline[3]&(1u<<LN_BT_CHAINLOAD))||
       (ln_control->timeline[3]&(1u<<LN_BT_OS_C)))ln_timeline_reset();
    uint32_t stage=loader?LN_BT_BOOT_C:LN_BT_OS_C;
    ln_control->timeline[8u+stage]=c_entry;
    ln_control->timeline[3]|=1u<<stage;
    ln_recovery_timeline_stamp(loader?LN_BT_BOOT_RECOVERY:LN_BT_OS_RECOVERY);
}
void ln_recovery_timeline_add(uint32_t counter,uint32_t value)
{
    if(counter>=5u||ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[2]!=ln_control->generation||
       !(ln_control->timeline[3]&(1u<<LN_BT_BOOT_VALIDATE))||
       (ln_control->timeline[3]&(1u<<LN_BT_BOOT_VALIDATED)))return;
    uint32_t prior=ln_control->timeline[41u+counter];
    ln_control->timeline[41u+counter]=value>UINT32_MAX-prior?UINT32_MAX:prior+value;
    ln_control->timeline[40]|=1u<<counter;
}
void ln_recovery_timeline_clock(uint32_t status)
{
    if(!ln_running_loader||ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[2]!=ln_control->generation)return;
    ln_control->timeline[1]=0u;
    ln_control->timeline[36]=LINNET_MMIO(0x6000600cu);
    ln_control->timeline[37]=LINNET_MMIO(0x60006004u);
    ln_control->timeline[38]=LINNET_MMIO(0x70000020u);
    ln_control->timeline[39]=LINNET_MMIO(0x70000028u);
    ln_control->timeline[47]=LINNET_MMIO(0x60006020u);
    ln_control->timeline[46]=status;
}
void ln_recovery_timeline_hash_iram(void){
    if(ln_running_loader&&ln_control->timeline[0]==LN_BOOT_TIMELINE_MAGIC&&
       ln_control->timeline[2]==ln_control->generation)ln_control->timeline[7]|=2u;
}
void ln_recovery_timeline_diagnostics(uint32_t output[48])
{
    if(!output)return;
    for(uint32_t i=0;i<48u;++i)output[i]=0;
    if(ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[1]!=0u||
       ln_control->timeline[2]!=ln_control->generation)return;
    for(uint32_t i=0;i<48u;++i)output[i]=ln_control->timeline[i];
}

static void ln_mask(void)
{
    uint32_t flags;
    __asm__ volatile("mrs %0, cpsr\n\torr %0, %0, #0xc0\n\tmsr cpsr_c, %0"
                     : "=&r"(flags) :: "memory");
}

static int ln_snapshot_valid(void)
{
    uint32_t size = ln_control->snapshot_size;
    return ln_control->magic == LINNET_RECOVERY_MAGIC && size >= 32u &&
        size <= LINNET_LOADER_CAPACITY &&
        ln_recovery_crc((const uint8_t *)(uintptr_t)LINNET_NATIVE(LINNET_LOADER_SNAPSHOT), size)
            == ln_control->snapshot_crc;
}

int ln_recovery_init(int loader)
{
    /* Native reset clears only the anchor magic in entry.S. Preserve an
     * unfinished candidate's trace before a fallback boot replaces it. This
     * uses the existing development crash mailbox and performs no disk I/O.
     * A candidate already returned to the anchor is not interrupted again by
     * an orderly restart after its crash mailbox was explicitly cleared.
     * B0070001 means an interrupted startup, not a diagnosed CPU exception. */
    if(loader&&ln_control->magic!=LINNET_RECOVERY_MAGIC&&
       ln_control->trace_magic==0x4c4e5452u&&ln_control->candidate_size>=32u&&
       !ln_control->returned&&!ln_control->candidate_healthy){
        volatile uint8_t *mailbox=(volatile uint8_t *)(uintptr_t)0x11fff000u;
        uint8_t raw[LINNET_CRASH_MAILBOX_SIZE];struct ln_crash_record record;
        for(uint32_t i=0;i<sizeof raw;++i)raw[i]=mailbox[i];
        if(!ln_crash_decode(&record,raw,sizeof raw)){
            record.sequence=ln_control->generation;record.reason=0xb0070001u;
            for(uint32_t i=0;i<16;++i)record.registers[i]=0;
            record.registers[0]=ln_control->trace_phase;
            record.registers[1]=ln_control->trace_size;
            record.registers[2]=ln_control->trace_status;
            record.registers[3]=ln_control->trace_elapsed;
            record.cpsr=0;record.flags=2; /* startup trace; no CPU registers captured */
            for(uint32_t i=0;i<32;++i)record.build_id[i]=ln_control->build_id[i];
            record.boot_generation=ln_control->generation;record.active_service=0;
            record.last_event=ln_control->trace_failure;
            if(ln_crash_encode(&record,raw,sizeof raw)){
                for(uint32_t i=0;i<4;++i)mailbox[i]=0;
                for(uint32_t i=4;i<sizeof raw;++i)mailbox[i]=raw[i];
                __asm__ volatile("" ::: "memory");
                for(uint32_t i=0;i<4;++i)mailbox[i]=raw[i];
            }
        }
    }
    ln_running_loader = loader != 0;
    ln_control->armed = 0;
    ln_control->progress = ln_time_us();
    if (!loader) ln_trace_phase(0x50u, 0, 0);
    if (loader) {
        /* Low-address uploaded Boot candidates cannot replace the anchor.
         * Only native-alias CPU bootstrap invalidates it in entry.S. */
        if (!ln_snapshot_valid()) {
            uint32_t length = (uint32_t)(uintptr_t)__image_end;
            if (length < 32u || length > LINNET_LOADER_CAPACITY) return 0;
            ln_control->magic = 0;
            volatile uint8_t *destination = (volatile uint8_t *)(uintptr_t)
                LINNET_NATIVE(LINNET_LOADER_SNAPSHOT);
            const volatile uint8_t *source = (const volatile uint8_t *)(uintptr_t)0x10000000u;
            for (uint32_t i = 0; i < length; ++i) destination[i] = source[i];
            ln_control->snapshot_size = length;
            ln_control->snapshot_crc = ln_recovery_crc((const uint8_t *)(uintptr_t)destination, length);
            ln_control->generation = 0;
            ln_control->returned = 0;
            ln_control->candidate_healthy = 0;
            ln_control->candidate_size = 0;
            ln_control->magic = LINNET_RECOVERY_MAGIC;
        }
    }
    return ln_snapshot_valid();
}

int ln_recovery_ready(void) { return ln_snapshot_valid(); }
int ln_recovery_image_identity(uint8_t build[32], uint8_t image_sha[32])
{
    if (!build || !image_sha || ln_control->magic != LINNET_RECOVERY_MAGIC ||
        ln_control->returned || !ln_control->candidate_size) return 0;
    for (uint32_t i = 0; i < 32u; ++i) {
        build[i] = ln_control->build_id[i];
        image_sha[i] = ln_control->image_sha[i];
    }
    return 1;
}

uint32_t ln_recovery_generation(void) { return ln_control->generation; }
void ln_recovery_feed(void) { ln_control->progress = ln_time_us(); }

int ln_recovery_cop_active(void)
{
    if (ln_control->cop_cookie != 0x4c4e434fu) return 0;
    uint32_t tick = ln_control->cop_tick, start = ln_time_us();
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (ln_control->cop_tick != tick) return 1;
        if (ln_time_us() - start > 1000u) break;
    }
    return 0;
}

void ln_recovery_arm(void)
{
    ln_recovery_feed();
    ln_control->armed = 1;
    LINNET_MMIO(0x60005008u) = 0;
    (void)LINNET_MMIO(0x6000500cu);
    LINNET_MMIO(0x6000401cu) = 2u;
    LINNET_MMIO(0x6000402cu) |= 2u; /* Timer2 routes to FIQ. */
    LINNET_MMIO(0x60004024u) = 2u;
    LINNET_MMIO(0x60005008u) = 0xc0000000u | (100000u - 1u);
    uint32_t flags;
    __asm__ volatile("mrs %0, cpsr\n\tbic %0, %0, #0x40\n\tmsr cpsr_c, %0"
                     : "=&r"(flags) :: "memory");
}

void ln_recovery_watchdog_interrupt(uint32_t pc, uint32_t cpsr)
{
    (void)LINNET_MMIO(0x6000500cu);
    LINNET_MMIO(0x6000401cu) = 2u;
    if (ln_control->armed == 1u && ln_time_us() - ln_control->progress >= 500000u)
        ln_panic(9u, pc, cpsr);
}

#if defined(LINNET_AUDIO)
static _Noreturn void ln_failed_handoff(void)
{
    /* Storage has already drained. A partially stopped transport cannot
     * return to an executive that keeps feeding the watchdog without USB. */
    LINNET_MMIO(0x60006004u)|=4u;
    for(;;)__asm__ volatile("nop");
}
#endif
static int recovery_quiesce(void *context, bool chainload)
{
    (void)context;
    #if defined(LINNET_STORAGE)
    if (!(chainload?ln_storage_handoff_quiesce():ln_storage_quiesce())) return 0;
    #else
    (void)chainload;
    #endif
    if (!ln_recovery_presentation_stop()) {
        ln_trace_phase(0x0fu, 0x0fu, 0);
        return 0;
    }
    if (ln_control->armed == 1u) {
        ln_control->trace_magic = 0;
        ln_control->reason = 0;
    }
    ln_trace_phase(0x10u, 0, 0);
    int transport_stopped = ln_recovery_transport_stop();
    ln_control->trace_status = ln_recovery_transport_diagnostics();
    if (!transport_stopped) {
        ln_trace_phase(0x11u, 0x11u, ln_control->trace_status);
#if defined(LINNET_AUDIO)
        ln_failed_handoff();
#endif
        return 0;
    }
    /* Leave the independent FIQ deadline alive during loader verification. */
    ln_platform_quiesce();
    if (ln_control->armed == 1u) LINNET_MMIO(0x60004024u) = 2u;
    if (!ln_platform_dma_idle()) {
        ln_trace_phase(0x12u, 0x12u, LINNET_MMIO(0x6000a004u));
#if defined(LINNET_AUDIO)
        ln_failed_handoff();
#endif
        return 0;
    }
    LINNET_MMIO(0x70008a24u) = 0; /* Cancel LCD block submissions. */
#if defined(LINNET_AUDIO)
    /* Clean and disable from IRAM. PP5020 hardware invalidation can leave
     * invalid lines that corrupt subsequent cached instructions/stack writes. */
    if(!ln_core_cache_disable()){
        ln_trace_phase(0x13u,0x13u,LINNET_MMIO(0x6000c000u));
        ln_failed_handoff();
    }
#else
    if (LINNET_MMIO(0x6000c000u) & 1u) {
        LINNET_MMIO(0xf000f044u) |= 6u;
        uint32_t start = ln_time_us();
        for (uint32_t count = 0; count < 100000u; ++count) {
            if (!(LINNET_MMIO(0x6000c000u) & 0x8000u)) break;
            if (count == 99999u || ln_time_us() - start > 10000u) {
                ln_trace_phase(0x13u, 0x13u, LINNET_MMIO(0x6000c000u));
                return 0;
            }
        }
        LINNET_MMIO(0x6000c000u) = 0;
    }
#endif
    ln_trace_phase(0x14u, 0, 0);
    return 1;
}
int ln_recovery_quiesce(void *context){return recovery_quiesce(context,false);}

static _Noreturn void ln_copy_and_enter(const uint8_t *source, uint32_t size,
                                       uint32_t load, uint32_t entry)
{
    ln_mask();
    ln_trace_phase(0x30u, 0, 0);
    ln_control->armed = 0;
    LINNET_MMIO(0x60005008u) = 0;
    LINNET_MMIO(0x60004028u) = UINT32_MAX;
    LINNET_MMIO(0x60004128u) = UINT32_MAX;
    volatile uint8_t *iram = (volatile uint8_t *)(uintptr_t)0x40000000u;
    uint32_t stub_size = (uint32_t)(ln_copy_trampoline_end - ln_copy_trampoline_begin);
    for (uint32_t i = 0; i < stub_size; ++i) iram[i] = ln_copy_trampoline_begin[i];
    __asm__ volatile("" ::: "memory");
    ((void (*)(const uint8_t *, uint32_t, uint32_t, uint32_t))(uintptr_t)0x40000000u)
        (source, size, load, entry);
    __builtin_unreachable();
}

int ln_recovery_chainload(void *context, const uint8_t *payload, uint32_t size,
    uint32_t load, uint32_t entry, const uint8_t build_id[32],
    const uint8_t image_sha[32], uint32_t next_generation)
{
    int native_timeline=ln_timeline_native_pending;
    ln_timeline_native_pending=0;
    uint32_t address = (uint32_t)(uintptr_t)payload;
    if (!payload || !build_id || !image_sha || !ln_snapshot_valid() ||
        size < 32u || size > LINNET_OS_CAPACITY || load != 0 || (address & 3u) ||
        (entry & 3u) || entry > size - 4u ||
        address < LINNET_STAGE_ADDRESS || address >= LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY ||
        size > LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY - address) {
        ln_trace_phase(0x21u, 0x21u, address);
        return 0;
    }
    if (!recovery_quiesce(context,true)) return 0;
    /* Only native prepare may carry mount/validation timing across the copy.
     * RAM launches reset it, including launches from an already running OS. */
    if(!native_timeline||!ln_running_loader||ln_control->timeline[0]!=LN_BOOT_TIMELINE_MAGIC||
       ln_control->timeline[2]!=ln_control->generation||
       !(ln_control->timeline[3]&(1u<<LN_BT_BOOT_VALIDATED)))ln_timeline_reset();
    else ln_control->timeline[7]|=1u;
    ln_control->generation = next_generation;
    ln_control->timeline[2]=next_generation;
    ln_recovery_timeline_stamp(LN_BT_CHAINLOAD);
    ln_control->candidate_address = address;
    ln_control->candidate_size = size;
    ln_control->candidate_entry = entry;
    ln_control->startup[0]=0;
    for(uint32_t i=1;i<16u;++i)ln_control->startup[i]=0;
    ln_control->startup[1]=0u;
    ln_control->startup[2]=next_generation;
    ln_control->startup[3]=size;
    ln_control->startup[0]=LN_STARTUP_TIMING_MAGIC;
    ln_startup_stamp(0);
    ln_control->candidate_crc = ln_candidate_crc(payload, size);
    ln_startup_stamp(1);
    ln_control->candidate_healthy = 0;
    ln_control->returned = 0;
    for (uint32_t i = 0; i < 32u; ++i) {
        ln_control->build_id[i] = build_id[i];
        ln_control->image_sha[i] = image_sha[i];
    }
    ln_trace_phase(0x22u, 0, 0);
    ln_startup_stamp(2);
    ln_copy_and_enter(payload, size, load, entry);
}

void ln_recovery_mark_healthy(void)
{
    /* A restored Boot health marker must never promote the failed candidate. */
    if (!ln_running_loader && ln_control->magic == LINNET_RECOVERY_MAGIC && !ln_control->returned &&
        ln_control->candidate_size >= 32u) ln_control->candidate_healthy = 1;
}

_Noreturn void ln_recovery_return(uint32_t reason)
{
    ln_mask();
    ln_control->armed = 0;
    ln_control->reason = reason;
    ln_control->returned = 1;
    ++ln_control->generation;
    ln_trace_phase(0x60u, 0, 0);
    if (ln_snapshot_valid() && ln_recovery_quiesce(0))
        ln_copy_and_enter((const uint8_t *)(uintptr_t)LINNET_NATIVE(LINNET_LOADER_SNAPSHOT),
                          ln_control->snapshot_size, 0, 0);
    ln_trace_phase(0x61u, 0, 0);
    LINNET_MMIO(0x60006004u) |= 4u;
    for (;;) __asm__ volatile("nop");
}

static int ln_recovery_storage_ready(void)
{
#if defined(LINNET_STORAGE)
    return ln_storage_quiesce_ready();
#else
    return 1;
#endif
}
int ln_recovery_can_reboot(uint16_t command)
{
    if (command == 33u) return ln_snapshot_valid() && ln_recovery_storage_ready();
    if (command == 34u) return ln_storage_mounted() && !ln_storage_busy() && ln_recovery_storage_ready();
    if (command != 32u || ln_control->magic != LINNET_RECOVERY_MAGIC) return 0;
    uint32_t address = ln_control->candidate_address, size = ln_control->candidate_size;
    return ln_control->candidate_healthy == 1u && size >= 32u && !(address & 3u) &&
        address >= LINNET_STAGE_ADDRESS && address < LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY &&
        size <= LINNET_STAGE_ADDRESS + LINNET_STAGE_CAPACITY - address &&
        ln_control->candidate_entry <= size - 4u && !(ln_control->candidate_entry & 3u) &&
        ln_recovery_crc((const uint8_t *)(uintptr_t)address, size) == ln_control->candidate_crc &&
        ln_recovery_storage_ready();
}

int ln_recovery_reboot(void *context, uint16_t command)
{
    if (!ln_recovery_can_reboot(command)) return 0;
    if (command == 33u) ln_recovery_return(0);
    if (command == 32u) {
        uint32_t address = ln_control->candidate_address, size = ln_control->candidate_size;
        if (!ln_recovery_quiesce(context)) return 0;
        ++ln_control->generation;
        ln_control->returned = 0;
        ln_copy_and_enter((const uint8_t *)(uintptr_t)address, size, 0, ln_control->candidate_entry);
    }
    if (command == 34u) {
        uint32_t sector_bytes = ln_storage_disk_sector_bytes();
        if (!ln_recovery_quiesce(context)) return 0;
        ln_pp_disk_mode(LINNET_USB_SERIAL, sector_bytes ? sector_bytes : 512u, true);
    }
    return 0;
}
