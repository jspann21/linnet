#include "hal/pp502x/soc.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/power.h"
#endif
#if defined(LINNET_IMAGE_OS)
#include "hal/api/display.h"
#endif
#if defined(LINNET_AUDIO)
#include "hal/api/audio.h"
#endif
#if !defined(LINNET_PROFILE_RELEASE)
#include "hal/api/recovery.h"
#endif
#define LINNET_MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
static struct ln_core_status ln_status;
static volatile uint32_t ln_wakes;
static uint32_t ln_armed_deadline;
static uint32_t ln_clock_failure;
static volatile uint32_t ln_memory_probe[32] __attribute__((aligned(16)));
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
static uint32_t extended_bytes;
void *ln_platform_extended_memory(uint32_t *bytes){
    if(bytes)*bytes=extended_bytes;
    return extended_bytes?(void *)(uintptr_t)0x12000000u:0;
}
static void probe_extended_memory(uintptr_t address){
    /* PP5022's 32 MiB boards mirror the second bank. Probe an owned low-bank
     * word and its native upper-bank alias while cache and interrupts are off.
     * Restore both values even when they refer to the same physical word. */
    uint32_t info=LINNET_MMIO(0x4001ff1cu);
    if(LINNET_MMIO(0x4001ff18u)!=0x53797349u||(info&3u)||
       info<0x40000000u||info>0x4001fe78u)return;
    uint32_t revision=LINNET_MMIO(info+0x84u);
    if(revision!=0x000b0010u&&revision!=0x000b0011u)return;
    volatile uint32_t *low=(volatile uint32_t *)(address|0x10000000u);
    volatile uint32_t *high=(volatile uint32_t *)(address|0x12000000u);
    uint32_t saved_low=*low,saved_high=*high;
    *low=0x55aa33ccu;*high=0xaa55cc33u;
    bool distinct=*low==0x55aa33ccu&&*high==0xaa55cc33u;
    *high=saved_high;*low=saved_low;
    if(!distinct)return;
    /* Check independence across the complete second bank, preserving bytes. */
    uint32_t saved[32];
    for(uint32_t i=0;i<32u;i++)saved[i]=LINNET_MMIO(0x12000000u+(i<<20));
    bool valid=true;
    for(uint32_t pass=0;pass<2u;pass++){
        for(uint32_t i=0;i<32u;i++)LINNET_MMIO(0x12000000u+(i<<20))=(pass?0xaaaaaaaau:0x55555555u)^(1u<<i);
        for(uint32_t i=0;i<32u;i++)if(LINNET_MMIO(0x12000000u+(i<<20))!=((pass?0xaaaaaaaau:0x55555555u)^(1u<<i)))valid=false;
    }
    for(uint32_t i=0;i<32u;i++)LINNET_MMIO(0x12000000u+(i<<20))=saved[i];
    if(valid)extended_bytes=32u*1024u*1024u;
}
#endif

uint32_t ln_core_irq_save(void)
{
    uint32_t flags, masked;
    __asm__ volatile("mrs %0,cpsr\n\torr %1,%0,#0x80\n\tmsr cpsr_c,%1"
                     : "=&r"(flags), "=&r"(masked) :: "memory");
    return flags;
}
void ln_core_irq_restore(uint32_t flags)
{
    __asm__ volatile("msr cpsr_c,%0" :: "r"(flags) : "memory");
}
bool ln_core_delay_us(uint32_t delay)
{
    if (delay > 10000u) return false;
    uint32_t start = ln_time_us();
    for (uint32_t count = 0; count < 1000000u; ++count)
        if (ln_time_us() - start >= delay) return true;
    return false;
}
static uint32_t ln_clock_hz(void)
{
    uint32_t source = LINNET_MMIO(0x60006020u), pll = LINNET_MMIO(0x60006034u);
    if ((source & 0xffffu) == 0x2222u) return 24000000u;
    if ((source & 0xffffu) == 0x7777u) {
        if (pll == LN_PLL_80) return 80000000u;
        if (pll == LN_PLL_30) return 30000000u;
    }
    return 0; /* Unknown inherited clock is reported, never guessed. */
}

bool ln_core_init(void)
{
#if defined(LINNET_PROFILE_RELEASE)
    LINNET_MMIO(0x11ffe014u) = 0;
    ln_core_watchdog_feed();
#endif
    /* Probe only dedicated owned words through both SDRAM aliases. Do not
     * rewrite live controller geometry or destructive-test occupied RAM. */
    if (LINNET_MMIO(0x6000c000u) & 1u) return false;
    uintptr_t address = (uintptr_t)ln_memory_probe;
    if (address >= 0x00800000u || (LINNET_MMIO(0xf000f000u) & 0x3e00u) != 0x3e00u ||
        LINNET_MMIO(0xf000f004u) != 0x10000f84u) return false;
    volatile uint32_t *native = (volatile uint32_t *)(address | 0x10000000u);
    bool valid = true;
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        for (uint32_t i = 0; i < 32u; ++i)
            ln_memory_probe[i] = (pass ? 0xaaaaaaaau : 0x55555555u) ^ (1u << i);
        for (uint32_t i = 0; i < 32u; ++i)
            if (native[i] != ((pass ? 0xaaaaaaaau : 0x55555555u) ^ (1u << i))) valid = false;
    }
    for (uint32_t i = 0; i < 32u; ++i) ln_memory_probe[i] = 0;
#if defined(LINNET_EXTENDED_RAM) && defined(LINNET_IMAGE_OS)
    if(valid)probe_extended_memory(address);
#endif
    ln_status.sdram_valid = valid;
    ln_status.timer_valid = ln_core_delay_us(10u);
    LINNET_MMIO(0x60005000u) = 0;
    (void)LINNET_MMIO(0x60005004u);
    LINNET_MMIO(0x60004028u) = 1u | (1u << 20) | (1u << 30);
    LINNET_MMIO(0x60004128u) = 0x101u;
    LINNET_MMIO(0x6000402cu) &= ~(1u | (1u << 20) | (1u << 30));
    LINNET_MMIO(0x6000412cu) &= ~0x101u;
    uint32_t flags;
    __asm__ volatile("mrs %0,cpsr\n\tbic %0,%0,#0x80\n\tmsr cpsr_c,%0"
                     : "=&r"(flags) :: "memory");
    return ln_status.sdram_valid && ln_status.timer_valid;
}

void ln_core_input_rearm(void)
{
    LINNET_MMIO(0x60004124u) = 0x101u;
    LINNET_MMIO(0x60004024u) = 1u << 30;
}
void ln_core_usb_rearm(void) { LINNET_MMIO(0x60004024u) = 1u << 20; }

void ln_core_irq_dispatch(void)
{
    uint32_t started = ln_time_us();
    uint32_t pending = LINNET_MMIO(0x60004000u);
    ++ln_status.irq_count;
#if defined(LINNET_AUDIO)
    if(pending&(1u<<26)){ln_audio_irq();ln_wakes|=16u;}
#endif
    if (pending & 1u) {
        LINNET_MMIO(0x60005000u) = 0;
        (void)LINNET_MMIO(0x60005004u);
        LINNET_MMIO(0x60004028u) = 1u;
        ln_wakes |= LINNET_WAKE_TIMER;
        int32_t late = (int32_t)(ln_time_us() - ln_armed_deadline);
        if (late > 0 && (uint32_t)late > ln_status.deadline_late_us)
            ln_status.deadline_late_us = (uint32_t)late;
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
        ln_platform_click_poll(ln_time_us());
        uint32_t click_due;
        if(ln_platform_click_deadline(&click_due))ln_core_wake_at(click_due);
#endif
    }
    if (pending & (1u << 20)) {
        LINNET_MMIO(0x60004028u) = 1u << 20;
        ln_wakes |= LINNET_WAKE_USB;
    }
    if (pending & (1u << 30)) {
        uint32_t high = LINNET_MMIO(0x60004100u);
        LINNET_MMIO(0x60004128u) = high;
        if (high & 0x100u) ln_wakes |= LINNET_WAKE_INPUT;
        if (high & 1u) {
#if defined(LINNET_IMAGE_OS)
            ln_display_sync_irq();
#endif
            ln_wakes |= LINNET_WAKE_GPIO;
        }
        if (high & ~0x101u) ++ln_status.unexpected_irq;
    }
    /* Timer2 belongs exclusively to recovery FIQ, including its enable bit. */
    uint32_t unknown = pending & ~(3u | (1u << 20) | (1u << 30));
#if defined(LINNET_AUDIO)
    unknown&=~(1u<<26);
#endif
    if (unknown) { LINNET_MMIO(0x60004028u) = unknown; ++ln_status.unexpected_irq; }
    uint32_t elapsed = ln_time_us() - started;
    if (elapsed > ln_status.irq_max_us) ln_status.irq_max_us = elapsed;
}

__attribute__((naked)) void ln_core_irq(void)
{
    __asm__ volatile("sub lr,lr,#4\n\tstmdb sp!,{r0-r12,lr}\n\tmrs r0,spsr\n\t"
                     "stmdb sp!,{r0,r1}\n\tbl ln_core_irq_dispatch\n\t"
                     "ldmia sp!,{r0,r1}\n\tmsr spsr_cxsf,r0\n\t"
                     "ldmia sp!,{r0-r12,lr}\n\tmovs pc,lr");
}

uint32_t ln_core_take_wakes(void)
{
    uint32_t flags = ln_core_irq_save(), wakes = ln_wakes;
    ln_wakes = 0;
    ln_core_irq_restore(flags);
    return wakes;
}
void ln_core_wake_at(uint32_t deadline_us)
{
    uint32_t flags=ln_core_irq_save(),now=ln_time_us();
    /* Preserve a pending interrupt; only a live future timer owns a wake. */
    if((LINNET_MMIO(0x60004000u)&1u)||
       ((LINNET_MMIO(0x60005000u)&0x80000000u)&&
       (LINNET_MMIO(0x60004020u)&1u)&&
       (int32_t)(ln_armed_deadline-now)>0&&
       (int32_t)(ln_armed_deadline-deadline_us)<=0)){ln_core_irq_restore(flags);return;}
    int32_t delta=(int32_t)(deadline_us-now);
    if(delta<1)delta=1;
    if(delta>200000)delta=200000;
    ln_armed_deadline=now+(uint32_t)delta;
    LINNET_MMIO(0x60005000u)=0;
    (void)LINNET_MMIO(0x60005004u);
    LINNET_MMIO(0x60005000u)=0xc0000000u|((uint32_t)delta-1u);
    LINNET_MMIO(0x60004024u)=1u;
    ln_core_irq_restore(flags);
}
void ln_core_idle_until(uint32_t deadline_us)
{
    uint32_t flags = ln_core_irq_save(), now = ln_time_us();
    int32_t delta = (int32_t)(deadline_us - now);
    if (delta <= 0 || ln_wakes || LINNET_MMIO(0x60004000u) || LINNET_MMIO(0x60004100u)) {
        goto awake;
    }
    /* Keep watchdog progress available; this is a maximum sleep, not a poll. */
    if (delta > 200000) delta = 200000;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    uint32_t power_delta=ln_power_next_deadline()-now;
#endif
    ln_core_wake_at(now+(uint32_t)delta);
    /* Arming may consume the last part of a short wait. Do not halt with
     * a due deadline or an interrupt already awaiting dispatch. */
    if((int32_t)(deadline_us-ln_time_us())<=0||
       LINNET_MMIO(0x60004000u))goto awake;
    uint32_t control = LINNET_MMIO(0x60007000u);
    LINNET_MMIO(0x60007000u) = 0x80000000u;
    __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
    LINNET_MMIO(0x60007000u) = control & ~0x80000000u;
    uint32_t elapsed=ln_time_us()-now;
    ln_status.idle_us += elapsed;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
    ln_power_idle_observe((uint32_t)delta,elapsed,power_delta);
#endif
    ++ln_status.idle_count;
awake:
    ln_core_irq_restore(flags);
}

__attribute__((weak)) bool ln_core_cop_pause(bool paused)
{
    (void)paused;
    return (LINNET_MMIO(0x60007004u) & 0x80000000u) != 0;
}
bool ln_core_clock_set(uint32_t hz)
{
#if defined(LINNET_AUDIO)
    struct ln_audio_status audio;ln_audio_snapshot(&audio);
    if(audio.initialized&&hz!=ln_clock_hz())return false;
#endif
    ln_clock_failure = 0;
    if (hz != 24000000u && hz != 30000000u && hz != 80000000u) {
        ln_clock_failure = 1; return false;
    }
    if (ln_clock_hz() == hz) return true;
    ln_core_watchdog_feed();
    if (!ln_core_cop_pause(true)) { ln_clock_failure = 2; return false; }
    extern const uint32_t ln_clock_stub_begin[], ln_clock_stub_end[];
    uint32_t words = (uint32_t)(ln_clock_stub_end - ln_clock_stub_begin);
    if (words > 128u) { ln_clock_failure = 3; (void)ln_core_cop_pause(false); return false; }
    volatile uint32_t *stub = (volatile uint32_t *)(uintptr_t)0x4000d000u;
    for (uint32_t i = 0; i < words; ++i) stub[i] = ln_clock_stub_begin[i];
    uint32_t flags = ln_core_irq_save(), masked;
    __asm__ volatile("mrs %0,cpsr\n\torr %0,%0,#0x40\n\tmsr cpsr_c,%0"
                     : "=&r"(masked) :: "memory");
    uint32_t pll = hz == 80000000u ? LN_PLL_80 :
                   hz == 30000000u ? LN_PLL_30 : 0u;
    bool ready = ((uint32_t (*)(uint32_t))(uintptr_t)0x4000d000u)(pll) != 0;
    bool matched = ready && ln_clock_hz() == hz;
#if LINNET_CLOCK_SETTLE_US
    /* Some boards briefly read the old selection after switching back from idle.
     * Require the requested registers, allowing a bounded bridge settle. */
    uint32_t settle_started=ln_time_us();
    for(uint32_t budget=100000u;ready&&!matched&&budget;--budget){
        matched=ln_clock_hz()==hz;
        if(ln_time_us()-settle_started>=LINNET_CLOCK_SETTLE_US)break;
    }
#endif
    if (!ready) ln_clock_failure = 4;
    else if (!matched) ln_clock_failure = 5;
    ln_core_irq_restore(flags);
    ln_core_watchdog_feed();
    bool resumed = ln_core_cop_pause(false);
    if (!resumed && !ln_clock_failure) ln_clock_failure = 6;
    return resumed && matched;
}
uint32_t ln_core_clock_failure(void) { return ln_clock_failure; }

#if defined(LINNET_AUDIO)
#if defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
/* Reserved diagnostic phases travel through the retained trace interface.
 * Only enable emits them; later clean/disable must preserve the last milestone.
 * r9/r10 are scratch exclusively in this instrumented stub. */
#define LN_CACHE_MARK(phase) "cmp r4,#2\nbne 99f\nldr r9,=0x11ffe000\nmov r10,#" #phase "\norr r10,r10,#0xca00\nstr r10,[r9,#152]\n99:\n"
#define LN_CACHE_SAVED "{r4-r10,lr}"
#else
#define LN_CACHE_MARK(phase) ""
#define LN_CACHE_SAVED "{r4-r8,lr}"
#endif
/* PP5020 cache control must not fetch instructions or spill its working stack
 * through the cache it is flushing. Rockbox's system-pp502x.c puts its flush in
 * ICODE and uses four NOPs after BUSY clears. This bounded, position-independent
 * transaction implements that sequence in our reserved IRAM, including init.
 * r0: clean=0, disable=1, enable=2; r1: cached probe address. */
__asm__(
    ".pushsection .text.ln_cache_stub,\"ax\",%progbits\n"
    ".balign 4\n.global ln_cache_stub_begin\n.global ln_cache_stub_end\n"
    "ln_cache_stub_begin:\n"
    "stmdb sp!," LN_CACHE_SAVED "\nmov r4,r0\nmov r5,r1\n"
    LN_CACHE_MARK(3)
    "ldr r6,=0x6000c000\nldr r7,=0xf000f044\nldr r8,=0x60005010\n"
    "ldr r0,[r6]\ntst r0,#1\nbeq 1f\nbl 8f\ncmp r0,#0\nbeq 7f\n"
    "1: cmp r4,#0\nbeq 6f\nbl 12f\ncmp r0,#0\nbeq 7f\n"
    "cmp r4,#3\nbeq 6f\n"
    "mov r0,#0\nstr r0,[r6]\nnop\nnop\nnop\nnop\n"
    "cmp r4,#1\nbeq 6f\n"
    "mov r0,#4\nstr r0,[r6]\nldr r1,=0x60006044\nldr r0,[r1]\n"
    "orr r0,r0,#0x10\nstr r0,[r1]\nldr r0,=0x1c00\nstr r0,[r7,#-4]\n"
    "ldr r0,=0xfc0\nstr r0,[r7]\n"
    LN_CACHE_MARK(4)
    "mov r0,#7\nstr r0,[r6]\nnop\nnop\nnop\nnop\n"
    LN_CACHE_MARK(5)
    /* Prime every 16-byte line; hardware invalidation is deliberately unused. */
    "mov r0,#0x2000\n2: ldrb r1,[r0],#16\ncmp r0,#0x4000\nblo 2b\n"
    LN_CACHE_MARK(6)
    "mov r0,#0\nldr r2,=0x4c4e0000\n3: eor r1,r2,r0\nstr r1,[r5,r0,lsl #2]\n"
    "add r0,r0,#1\ncmp r0,#32\nblo 3b\n"
    LN_CACHE_MARK(7)
    "bl 8f\ncmp r0,#0\nbeq 7f\n"
    LN_CACHE_MARK(8)
    "orr r1,r5,#0x10000000\nldr r2,=0x4c4e0000\nmov r0,#0\n"
    "4: ldr r3,[r1,r0,lsl #2]\neor r12,r2,r0\ncmp r3,r12\nbne 5f\n"
    "add r0,r0,#1\ncmp r0,#32\nblo 4b\n"
    LN_CACHE_MARK(9)
    "b 6f\n"
    /* A completed clean permits disabling safely on failed alias validation. */
    "5:\n" LN_CACHE_MARK(242)
    "bl 12f\ncmp r0,#0\nbeq 7f\n"
    "mov r0,#0\nstr r0,[r6]\nnop\nnop\nnop\nnop\nb 7f\n"
    "6:\n" LN_CACHE_MARK(10)
    "mov r0,#1\n7: ldmia sp!," LN_CACHE_SAVED "\nbx lr\n"
    /* Only IRAM and MMIO are touched between FLUSH and the four NOPs. */
    "8: ldr r0,[r7]\norr r0,r0,#2\nstr r0,[r7]\nldr r2,[r8]\n"
    "ldr r3,=10000\nldr r12,=1000000\n"
    "9: ldr r0,[r6]\ntst r0,#0x8000\nbeq 10f\nldr r1,[r8]\nsub r1,r1,r2\n"
    "cmp r1,r3\nbhs 11f\nsubs r12,r12,#1\nbne 9b\n"
    "11:\n" LN_CACHE_MARK(241)
    /* BUSY timeout cannot return through cached C: that instruction fetch
     * may itself stall. The COP lease may already be disarmed during recovery.
     * Preserve a retained failure marker, then request reset entirely in IRAM. */
    "ldr r0,=0x11ffe000\nldr r1,=0x4c4e5452\nstr r1,[r0,#124]\n"
    "mov r1,#0xf1\norr r1,r1,#0xca00\nstr r1,[r0,#152]\n"
    "ldr r1,[r6]\nstr r1,[r0,#140]\n"
    "ldr r0,=0x60006004\nldr r1,[r0]\norr r1,r1,#4\nstr r1,[r0]\n"
    "16: nop\nb 16b\n"
    "10: nop\nnop\nnop\nnop\nmov r0,#1\nbx lr\n"
    /* Rockbox cache_invalidate_special: never create invalid cache lines.
     * First verify every CPU tag is clean, then replace all512 tags with a
     * valid, clean, unreachable address. No SDRAM fetch/store or interrupt
     * can dirty a line between these two bounded IRAM loops. This also clears
     * clean stale tags inherited while disabled before a new image is cached.
     * Status words are mirrored four times; touch one word per16-byte line. */
    "12: ldr r0,=0xf0004000\nldr r2,=0xf0006000\n"
    "13: ldr r1,[r0],#16\nand r3,r1,#0x00c00000\ncmp r3,#0x00c00000\nbeq 15f\n"
    "cmp r0,r2\nblo 13b\nldr r0,=0xf0004000\nldr r1,=0x009fffff\n"
    "14: str r1,[r0],#16\ncmp r0,r2\nblo 14b\n"
    "nop\nnop\nnop\nnop\nmov r0,#1\nbx lr\n"
    "15:\n" LN_CACHE_MARK(243)
    "mov r0,#0\nbx lr\n"
    ".ltorg\nln_cache_stub_end:\n.popsection\n");
extern const uint32_t ln_cache_stub_begin[],ln_cache_stub_end[];
static bool __attribute__((used,noinline)) ln_cache_transaction_inner(uint32_t operation)
{
#if defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    if(operation==2u){
        LINNET_MMIO(0x11ffe08cu)=LINNET_MMIO(0x60006044u); /* inherited priority */
        LINNET_MMIO(0x11ffe098u)=0xca01u;
        LINNET_MMIO(0x11ffe07cu)=0x4c4e5452u;
    }
#endif
    uint32_t bytes=(uint32_t)((uintptr_t)ln_cache_stub_end-(uintptr_t)ln_cache_stub_begin);
    if(!bytes||(bytes&3u)||bytes>0xe00u)return false;
    volatile uint32_t *iram=(volatile uint32_t *)(uintptr_t)0x4000d200u;
    for(uint32_t i=0;i<bytes/4u;i++)iram[i]=ln_cache_stub_begin[i];
    __asm__ volatile("" ::: "memory");
#if defined(LINNET_AUDIO_CACHE_DIAGNOSTIC)
    if(operation==2u)LINNET_MMIO(0x11ffe098u)=0xca02u;
#endif
    return ((uint32_t (*)(uint32_t,uintptr_t))(uintptr_t)0x4000d200u)
        (operation,(uintptr_t)ln_memory_probe)!=0;
}
static __attribute__((naked,noinline)) bool ln_cache_transaction(uint32_t operation __attribute__((unused)))
{
    __asm__ volatile("stmdb sp!,{r4-r6,lr}\n\tmov r4,sp\n\tmrs r5,cpsr\n\t"
                     "orr r6,r5,#0xc0\n\tmsr cpsr_c,r6\n\tldr sp,=0x4000f000\n\t"
                     "bl ln_cache_transaction_inner\n\tmov sp,r4\n\t"
                     "msr cpsr_c,r5\n\tldmia sp!,{r4-r6,lr}\n\tbx lr");
}
bool ln_core_cache_clean(void){return ln_cache_transaction(0);}
bool ln_core_cache_disable(void){return ln_cache_transaction(1);}
bool ln_core_cache_enable(void){return ln_cache_transaction(2);}
bool ln_core_code_publish(const void *address,uint32_t bytes){
    uintptr_t at=(uintptr_t)address;
    if(!bytes||(at&31u)||(bytes&31u)||at>UINT32_MAX-bytes)return false;
    /* Clean and replace unified CPU tags entirely from IRAM; retain enable. */
    return ln_cache_transaction(3);
}
#else
bool ln_core_code_publish(const void *address,uint32_t bytes){
    uintptr_t at=(uintptr_t)address;
    if(!bytes||(at&31u)||(bytes&31u)||at>UINT32_MAX-bytes)return false;
    return ln_core_cache_disable()&&ln_core_cache_enable();
}
bool ln_core_cache_clean(void)
{
    if (!(LINNET_MMIO(0x6000c000u) & 1u)) return true;
    __asm__ volatile("" ::: "memory");
    LINNET_MMIO(0xf000f044u) |= 2u;
    uint32_t start = ln_time_us();
    for (uint32_t i = 0; i < 1000000u; ++i) {
        if (!(LINNET_MMIO(0x6000c000u) & 0x8000u)) return true;
        if (ln_time_us() - start >= 10000u) break;
    }
    return false;
}
bool ln_core_cache_disable(void)
{
    if (!ln_core_cache_clean()) return false;
    LINNET_MMIO(0x6000c000u) = 0;
    __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
    return true;
}
static bool __attribute__((used,noinline)) ln_cache_enable_inner(void)
{
    if (!ln_core_cache_disable()) return false;
    LINNET_MMIO(0x6000c000u) = 4u;
    LINNET_MMIO(0x60006044u) |= 0x10u;
    LINNET_MMIO(0xf000f040u) = 0x1c00u;
    LINNET_MMIO(0xf000f044u) = 0xfc0u;
    LINNET_MMIO(0x6000c000u) = 7u;
    for (uintptr_t offset = 0; offset < 8192u; offset += 16u)
        (void)*(const volatile uint8_t *)(uintptr_t)(0x2000u + offset);
    for (uint32_t i = 0; i < 32u; ++i) ln_memory_probe[i] = 0x4c4e0000u ^ i;
    if (!ln_core_cache_clean()) return false;
    const volatile uint32_t *native = (const volatile uint32_t *)((uintptr_t)ln_memory_probe | 0x10000000u);
    for (uint32_t i = 0; i < 32u; ++i)
        if (native[i] != (0x4c4e0000u ^ i)) { (void)ln_core_cache_disable(); return false; }
    return true;
}
__attribute__((naked)) bool ln_core_cache_enable(void)
{
    /* IRQ/FIQ masked around the IRAM-stack transaction. Cache callers must
     * clean published COP buffers; the native SDRAM alias is never cached. */
    __asm__ volatile("stmdb sp!,{r4-r6,lr}\n\tmov r4,sp\n\tmrs r5,cpsr\n\t"
                     "orr r6,r5,#0xc0\n\tmsr cpsr_c,r6\n\tldr sp,=0x4000f000\n\t"
                     "bl ln_cache_enable_inner\n\tmov sp,r4\n\tmsr cpsr_c,r5\n\t"
                     "ldmia sp!,{r4-r6,lr}\n\tbx lr");
}
#endif
void ln_core_snapshot(struct ln_core_status *out)
{
    if (!out) return;
    uint32_t flags = ln_core_irq_save();
    ln_status.clock_hz = ln_clock_hz();
    ln_status.clock_source = LINNET_MMIO(0x60006020u);
    ln_status.pll_control = LINNET_MMIO(0x60006034u);
    ln_status.cache_control = LINNET_MMIO(0x6000c000u);
    ln_status.wake_sources = ln_wakes;
    *out = ln_status;
    ln_core_irq_restore(flags);
}

void ln_core_watchdog_feed(void)
{
    /* Fixed native alias is also consumed by the resident COP monitor. */
    LINNET_MMIO(0x11ffe010u) = ln_time_us();
}
void ln_core_watchdog_disarm(void)
{
    uint32_t flags;
    __asm__ volatile("mrs %0,cpsr\n\torr %0,%0,#0xc0\n\tmsr cpsr_c,%0"
                     : "=&r"(flags) :: "memory");
    /* Refresh before clearing armed so an in-flight COP iteration observes
     * a current lease. Both stores use the uncached native SDRAM alias. */
    ln_core_watchdog_feed();
    LINNET_MMIO(0x11ffe014u) = 0;
    LINNET_MMIO(0x60005008u) = 0;
    (void)LINNET_MMIO(0x6000500cu);
    LINNET_MMIO(0x60004028u) = 2u;
    LINNET_MMIO(0x6000401cu) = 2u;
}
void ln_core_watchdog_arm(void)
{
#if defined(LINNET_PROFILE_RELEASE)
    ln_core_watchdog_feed();
    LINNET_MMIO(0x11ffe014u) = 1u;
    LINNET_MMIO(0x60005008u) = 0;
    (void)LINNET_MMIO(0x6000500cu);
    LINNET_MMIO(0x6000401cu) = 2u;
    LINNET_MMIO(0x6000402cu) |= 2u;
    LINNET_MMIO(0x60004024u) = 2u;
    LINNET_MMIO(0x60005008u) = 0xc0000000u | 99999u;
    uint32_t flags;
    __asm__ volatile("mrs %0,cpsr\n\tbic %0,%0,#0x40\n\tmsr cpsr_c,%0"
                     : "=&r"(flags) :: "memory");
#else
    ln_recovery_arm();
#endif
}
#if defined(LINNET_PROFILE_RELEASE)
static __attribute__((used,noinline)) void ln_core_watchdog_interrupt(void)
{
    (void)LINNET_MMIO(0x6000500cu);
    LINNET_MMIO(0x6000401cu) = 2u;
    if (LINNET_MMIO(0x11ffe014u) == 1u &&
        ln_time_us() - LINNET_MMIO(0x11ffe010u) >= 500000u) {
        LINNET_MMIO(0x11ffe020u) = 9u;
        LINNET_MMIO(0x60006004u) |= 4u;
        for (;;) __asm__ volatile("nop");
    }
}
__attribute__((naked)) void ln_core_watchdog_fiq(void)
{
    __asm__ volatile("sub lr,lr,#4\n\tstmdb sp!,{r0-r12,lr}\n\tmrs r0,spsr\n\t"
                     "stmdb sp!,{r0,r1}\n\tbl ln_core_watchdog_interrupt\n\t"
                     "ldmia sp!,{r0,r1}\n\tmsr spsr_cxsf,r0\n\t"
                     "ldmia sp!,{r0-r12,lr}\n\tmovs pc,lr");
}
#endif

/* Position-independent IRAM clock transaction: no SDRAM instructions, data,
 * stack accesses, or interrupts while changing SDRAM bus timing. r0 is PLL
 * configuration (zero selects 24 MHz). PP5020 uses a fixed 500 us relock
 * interval; its status register has no documented PP5022-style lock bit. */
__asm__(
    ".pushsection .text.clock_stub,\"ax\",%progbits\n"
    ".align 2\n.global ln_clock_stub_begin\nln_clock_stub_begin:\n"
    "ldr r1,1f\nldr r2,[r1]\norr r2,r2,#0x40000000\nstr r2,[r1]\n"
    "ldr r1,2f\nldr r2,[r1]\norr r2,r2,#0x88000000\nstr r2,[r1]\n"
    "ldr r1,3f\nldr r2,4f\nstr r2,[r1]\n"
    "ldr r1,5f\nldr r2,6f\nstr r2,[r1]\n"
    "cmp r0,#0\nbeq 10f\n"
#if defined(LINNET_SOC_PP5022)
    "ldr r1,2f\nstr r0,[r1]\nldr r1,9f\nldr r3,[r1]\nmov r12,#0x10000\n"
    "12: ldr r1,7f\nldr r2,[r1]\ntst r2,#0x80000000\nbne 13f\n"
    "ldr r1,9f\nldr r2,[r1]\nsub r2,r2,r3\ncmp r2,#1000\nbhs 16f\n"
    "subs r12,r12,#1\nbne 12b\n16: mov r0,#0\nbx lr\n"
    "13: ldr r1,5f\ntst r0,#0x200000\nldreq r2,14f\nldrne r2,6f\nstr r2,[r1]\n"
#else
    "ldr r1,2f\nstr r0,[r1]\ntst r0,#1\nbeq 11f\n"
    "ldr r1,7f\nldr r2,8f\nstr r2,[r1]\nldr r1,2f\nstr r0,[r1]\n"
    "11: ldr r1,9f\nldr r3,[r1]\nmov r12,#0x10000\n"
    "12: ldr r2,[r1]\nsub r2,r2,r3\ncmp r2,#500\nbhs 13f\n"
    "subs r12,r12,#1\nbne 12b\nmov r0,#0\nbx lr\n"
    "13: ldr r1,5f\ntst r0,#1\nldrne r2,14f\nldreq r2,6f\nstr r2,[r1]\n"
#endif
    "ldr r1,3f\nldr r2,15f\nstr r2,[r1]\nmov r0,#1\nbx lr\n"
    "10: ldr r1,2f\nldr r2,[r1]\nbic r2,r2,#0x80000000\nstr r2,[r1]\n"
    "ldr r1,1f\nldr r2,[r1]\nbic r2,r2,#0x40000000\nstr r2,[r1]\n"
    "mov r0,#1\nbx lr\n"
    "1: .word 0x70000020\n2: .word 0x60006034\n3: .word 0x60006020\n"
    "4: .word 0x20002222\n5: .word 0x70000034\n6: .word 0x303\n"
    "7: .word 0x6000603c\n8: .word 0xd19b\n9: .word 0x60005010\n"
    "14: .word 0x808\n15: .word 0x20007777\n"
    ".global ln_clock_stub_end\nln_clock_stub_end:\n.popsection\n");
