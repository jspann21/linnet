#include "hal/api/power.h"
#include "hal/api/core.h"
#include "hal/api/platform.h"
#include "hal/pp502x/target.h"
#define LINNET_MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define LINNET_MMIO8(a) (*(volatile uint8_t *)(uintptr_t)(a))
static struct ln_power_state ln_power;
static struct ln_clock_state ln_clock;
static bool ln_power_ready;
enum ln_sample_stage {
    LN_SAMPLE_IDLE=0,LN_SAMPLE_ADC_COMMAND,LN_SAMPLE_ADC_POINTER,LN_SAMPLE_ADC_READ,
    LN_SAMPLE_RTC_POINTER_TIME,LN_SAMPLE_RTC_READ_TIME,
    LN_SAMPLE_RTC_POINTER_DATE,LN_SAMPLE_RTC_READ_DATE,
    LN_SAMPLE_RTC_POINTER_VERIFY,LN_SAMPLE_RTC_READ_VERIFY
};
enum ln_sample_transfer { LN_SAMPLE_TRANSFER_RETRY=0,LN_SAMPLE_TRANSFER_OK,
    LN_SAMPLE_TRANSFER_FAILED };
static struct {
    enum ln_sample_stage stage;
    uint32_t started_us;
    uint8_t attempts,flags,data[7],verify_second,age_valid,age_phase;
} ln_sampler;
#define LN_SAMPLE_STEP_DELAY_US 1000u
#define LN_SAMPLE_PERIOD_US 1000000u
#define LN_SAMPLE_ADC_MAX_AGE_US 100000u
#define LN_SAMPLE_RTC_MAX_AGE_US 60000u
#define LN_SAMPLE_AGE_ADC 0u
#define LN_SAMPLE_AGE_RTC 1u
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS) && !defined(LINNET_PROFILE_RELEASE)
#define LN_SAMPLE_DIAGNOSTICS 1
static struct {uint32_t reason,stage,age,active_late,idle_late;} ln_sample_diagnostics;
static struct ln_adc_trace {uint32_t started,command_age,pointer_age,idle_us,deadline_late,cancels;}
    ln_adc_current,ln_adc_failed;
static struct ln_adc_idle_trace {uint32_t requested,actual,power_delta;}
    ln_adc_idle_current,ln_adc_idle_failed;
static void ln_sample_failure(uint32_t reason){
    ln_sample_diagnostics.reason=reason;ln_sample_diagnostics.stage=ln_sampler.stage;
    ln_sample_diagnostics.age=ln_time_us()-ln_sampler.started_us;
    struct ln_core_status core;ln_core_snapshot(&core);
    ln_adc_failed=ln_adc_current;
    ln_adc_idle_failed=ln_adc_idle_current;
    ln_adc_failed.idle_us=core.idle_us-ln_adc_current.idle_us;
    ln_adc_failed.deadline_late=core.deadline_late_us;
}
void ln_power_sampler_diagnostics(uint32_t words[16]){
    uint32_t now=ln_time_us();
    words[0]=0x53554d50u;words[1]=ln_sampler.stage;words[2]=ln_sampler.age_phase;
    words[3]=ln_sampler.age_valid;words[4]=now;words[5]=ln_sampler.started_us;
    words[6]=ln_power.next_sample_us;words[7]=ln_power.samples;words[8]=ln_power.failures;
    words[9]=ln_power.bus_resets;words[10]=ln_sample_diagnostics.reason;
    words[11]=ln_sample_diagnostics.stage;words[12]=ln_sample_diagnostics.age;
    words[13]=ln_sample_diagnostics.active_late;words[14]=ln_sample_diagnostics.idle_late;
    words[15]=ln_power.flags;
}
void ln_power_sampler_trace(uint32_t words[16]){
    struct ln_core_status core;ln_core_snapshot(&core);
    uint32_t idle=ln_sampler.age_valid&&ln_sampler.age_phase==LN_SAMPLE_AGE_ADC?
        core.idle_us-ln_adc_current.idle_us:UINT32_MAX;
    uint32_t values[16]={0x31495450u,ln_time_us(),ln_adc_current.started,ln_adc_current.command_age,
        ln_adc_current.pointer_age,idle,core.deadline_late_us,ln_adc_current.cancels,
        ln_adc_failed.started,ln_adc_failed.command_age,ln_adc_failed.pointer_age,ln_adc_failed.idle_us,
        ln_adc_failed.deadline_late,ln_adc_failed.cancels,ln_sample_diagnostics.reason,ln_sample_diagnostics.age};
    for(uint32_t i=0;i<16u;i++)words[i]=values[i];
}
void ln_power_idle_observe(uint32_t requested,uint32_t actual,uint32_t power_delta){
    if(!ln_sampler.age_valid||ln_sampler.age_phase!=LN_SAMPLE_AGE_ADC)return;
    if(requested>ln_adc_idle_current.requested){
        ln_adc_idle_current.requested=requested;ln_adc_idle_current.power_delta=power_delta;
    }
    if(actual>ln_adc_idle_current.actual)ln_adc_idle_current.actual=actual;
}
void ln_power_sampler_idle_trace(uint32_t words[16]){
    words[0]=0x4c444950u;words[1]=ln_time_us();words[2]=ln_adc_current.started;
    words[3]=ln_adc_idle_current.requested;words[4]=ln_adc_idle_current.actual;
    words[5]=ln_adc_idle_current.power_delta;words[6]=ln_adc_failed.started;
    words[7]=ln_adc_idle_failed.requested;words[8]=ln_adc_idle_failed.actual;
    words[9]=ln_adc_idle_failed.power_delta;words[10]=ln_sample_diagnostics.reason;
    words[11]=ln_sample_diagnostics.age;words[12]=ln_sampler.stage;
    words[13]=ln_sampler.age_phase;words[14]=ln_sampler.age_valid;words[15]=ln_power.failures;
}
#endif
uint8_t ln_power_sources(void)
{
    return ln_target_power_sources();
}
static bool ln_bus_idle(void)
{
    uint32_t started = ln_time_us();
    for (uint32_t budget = 0; budget < 100000u; ++budget) {
        if (!(LINNET_MMIO8(0x7000c01cu) & 0x40u)) return true;
        if (ln_time_us() - started >= 5000u) break;
    }
    return false;
}
static void ln_bus_reset(void)
{
    LINNET_MMIO(0x6000600cu) |= 0x1000u;
    LINNET_MMIO(0x60006004u) |= 0x1000u;
    __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
    LINNET_MMIO(0x60006004u) &= ~0x1000u;
    LINNET_MMIO(0x600060a4u) = 0;
    LINNET_MMIO(0x600060a4u) = 0x80u;
    ++ln_power.bus_resets;
}
static bool ln_bus_transfer(uint8_t address, uint8_t *bytes, uint32_t count, bool read)
{
    if (address > 0x7fu || !bytes || !count || count > 4u || !ln_bus_idle()) return false;
    LINNET_MMIO8(0x7000c004u) = (uint8_t)((address << 1) | (read ? 1u : 0u));
    if (!read) for (uint32_t i = 0; i < count; ++i) LINNET_MMIO8(0x7000c00cu + i * 4u) = bytes[i];
    LINNET_MMIO8(0x7000c000u) = (uint8_t)((LINNET_MMIO8(0x7000c000u) & ~0x26u) |
        ((count - 1u) << 1) | (read ? 0x20u : 0u));
    LINNET_MMIO8(0x7000c000u) |= 0x80u;
    if (!ln_bus_idle()) return false;
    if (read) for (uint32_t i = 0; i < count; ++i) bytes[i] = LINNET_MMIO8(0x7000c00cu + i * 4u);
    return true;
}
/* Periodic sampling owns at most one bounded bus transfer per poll call.
 * M25 offers two separated, due-gated opportunities per runtime invocation.
 * Failed transfers retain their retry count across steps instead of monopolizing
 * the executive for all three attempts. Foreground I2C APIs keep their existing
 * synchronous contract. */
static enum ln_sample_transfer ln_sample_transfer(uint8_t *bytes,uint32_t count,bool read,
    bool completes_operation)
{
    if(ln_bus_transfer(8u,bytes,count,read)){
        if(completes_operation)ln_sampler.attempts=0;
        return LN_SAMPLE_TRANSFER_OK;
    }
    ln_bus_reset();
    if(++ln_sampler.attempts<3u)return LN_SAMPLE_TRANSFER_RETRY;
    ln_sampler.attempts=0;return LN_SAMPLE_TRANSFER_FAILED;
}
static void ln_sample_next_step(void)
{
    ln_power.next_sample_us=ln_time_us()+LN_SAMPLE_STEP_DELAY_US;
}
static void ln_sample_next_cycle(void)
{
    ln_sampler.stage=LN_SAMPLE_IDLE;
    ln_sampler.attempts=0;
    ln_sampler.age_valid=0;
    ln_sampler.age_phase=LN_SAMPLE_AGE_ADC;
    ln_power.next_sample_us=ln_time_us()+LN_SAMPLE_PERIOD_US;
}
static bool ln_sample_expired(uint32_t maximum)
{
    /* Unsigned subtraction is defined across the 32-bit microsecond wrap. */
    return ln_time_us()-ln_sampler.started_us>=maximum;
}
static void ln_sample_adc_failed(void)
{
    ln_power.flags=ln_sampler.flags|LINNET_POWER_I2C_FAULT;
    ++ln_power.failures;++ln_power.samples;ln_power.sampled_us=ln_time_us();
    ln_clock.valid=false;ln_sample_next_cycle();
}
#if defined(LN_SAMPLE_DIAGNOSTICS)
static void ln_sample_fail(uint32_t reason){ln_sample_failure(reason);ln_sample_adc_failed();}
#else
#define ln_sample_fail(reason) ln_sample_adc_failed()
#endif
/* Foreground PMU access cancels an unfinished pointer/data sequence first.
 * A completed battery publication remains valid; partial ADC/RTC bytes are
 * never published. The aggregate age and logical phase survive cancellation.
 * An RTC restart begins at its pointer, so an ADC success cannot renew the
 * already-running RTC age. Sampling restarts at an earlier retained deadline
 * or within 1ms; repeated foreground calls while idle cannot postpone it. */
static void ln_sample_cancel(void)
{
    if(ln_sampler.stage!=LN_SAMPLE_IDLE){
#if defined(LN_SAMPLE_DIAGNOSTICS)
        if(ln_sampler.age_valid&&ln_sampler.age_phase==LN_SAMPLE_AGE_ADC&&ln_adc_current.cancels!=UINT32_MAX)
            ++ln_adc_current.cancels;
#endif
        uint32_t prompt=ln_time_us()+LN_SAMPLE_STEP_DELAY_US;
        ln_sampler.stage=LN_SAMPLE_IDLE;ln_sampler.attempts=0;
        /* Never move an already-due or earlier sampling deadline later. */
        if((int32_t)(prompt-ln_power.next_sample_us)<0)ln_power.next_sample_us=prompt;
    }
}
static void ln_sample_foreground(uint8_t address)
{
    (void)address;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
    /* The codec has its own register pointer. Successful traffic to 0x1a
     * cannot replace the PMU's pointer; failed transfers still cancel below. */
    if(address==0x1au)return;
#endif
    ln_sample_cancel();
}
bool ln_power_i2c_read(uint8_t address, uint8_t reg, uint8_t *out, uint32_t length)
{
    if (!out || !length || length > 4u || address > 0x7fu) return false;
    ln_sample_foreground(address);
    for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
        if (ln_bus_transfer(address, &reg, 1u, false) && ln_bus_transfer(address, out, length, true)) return true;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
        ln_sample_cancel();
#endif
        ln_bus_reset();
    }
    return false;
}
bool ln_power_i2c_write(uint8_t address, uint8_t reg, uint8_t value)
{
    if (address > 0x7fu) return false;
    ln_sample_foreground(address);
    uint8_t bytes[2] = {reg, value};
    for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
        if (ln_bus_transfer(address, bytes, 2u, false)) return true;
#if defined(LINNET_TARGET_VIDEO5G_M25) && defined(LINNET_IMAGE_OS)
        ln_sample_cancel();
#endif
        ln_bus_reset();
    }
    return false;
}
bool ln_power_init(void)
{
    ln_clock=(struct ln_clock_state){0};
    ln_sampler=(typeof(ln_sampler)){0};
    ln_bus_reset();
    ln_target_power_prepare(&ln_power);
    ln_power.next_sample_us = ln_time_us();
    ln_power.charge_limit_ma = ln_target_charge_limit_get();
    ln_power_ready = ln_bus_idle();
    /* Source/state sampling does not authorize increasing USB charging. */
    return ln_power_ready;
}
static uint8_t ln_battery_percent(uint16_t mv)
{
    static const uint16_t curve[11] = LINNET_BATTERY_CURVE_MV;
    if (mv <= curve[0]) return 0;
    for (uint32_t i = 1; i < 11u; ++i) {
        if (mv > curve[i]) continue;
        /* Ten bounded comparisons avoid requiring a general divide runtime. */
        uint32_t span = curve[i] - curve[i - 1u], offset = mv - curve[i - 1u];
        uint32_t fraction = 0;
        while (fraction < 10u && (fraction + 1u) * span <= offset * 10u) ++fraction;
        return (uint8_t)((i - 1u) * 10u + fraction);
    }
    return 100u;
}
/* PCF50605 RTC register map: seconds/minutes/hours/weekday at 0x0a..0x0d,
 * day/month/year at 0x0e..0x10. Calendar values use packed BCD (year 2000..2099).
 * Primary reference: Rockbox firmware/drivers/rtc/rtc_pcf50605.c and
 * https://github.com/Rockbox/rockbox/blob/master/firmware/export/pcf5060x.h
 * Clock reads use one bounded attempt; the next one-second poll can retry.
 * Reading time never changes RTC, wake, charging, or alarm registers. */
static bool ln_clock_bcd(uint8_t raw,uint8_t maximum,uint8_t *value)
{
    if((raw&15u)>9u||(raw>>4)>9u)return false;
    *value=(uint8_t)((raw>>4)*10u+(raw&15u));return *value<=maximum;
}
static bool calendar_decode(const uint8_t raw[7],struct ln_calendar *c);
static bool calendar_raw(uint32_t deadline,uint8_t raw[7]);
/* RTC editing is an explicit foreground operation, independent of power poll. */
_Static_assert(sizeof(struct ln_calendar)==8u,"Calendar layout");
static uint8_t calendar_days(uint16_t year,uint8_t month){
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1u]+(month==2u&&!(year&3u));
}
static bool calendar_valid(const struct ln_calendar *c){
    return c&&c->year>=2000u&&c->year<=2099u&&c->month>=1u&&c->month<=12u&&
        c->day>=1u&&c->day<=calendar_days(c->year,c->month)&&c->hour<24u&&c->minute<60u&&c->second<60u;
}
static void calendar_weekday(struct ln_calendar *c){
    uint32_t year=c->year-2000u,days=year*365u+(year+3u)/4u+c->day-1u;
    for(uint8_t month=1;month<c->month;++month)days+=calendar_days(c->year,month);
    c->weekday=(uint8_t)((days+6u)%7u);
}
static bool calendar_decode(const uint8_t raw[7],struct ln_calendar *c){
    uint8_t year;
    if(!ln_clock_bcd(raw[0],59,&c->second)||!ln_clock_bcd(raw[1],59,&c->minute)||
       !ln_clock_bcd(raw[2],23,&c->hour)||!ln_clock_bcd(raw[4],31,&c->day)||
       !ln_clock_bcd(raw[5],12,&c->month)||!ln_clock_bcd(raw[6],99,&year)||raw[3]>6u)return false;
    c->year=2000u+year;c->weekday=raw[3];return calendar_valid(c);
}
static bool calendar_transfer(uint32_t deadline,uint8_t *bytes,uint32_t count,bool read){
    return (int32_t)(ln_time_us()-deadline)<0&&ln_bus_transfer(8u,bytes,count,read);
}
static bool calendar_raw(uint32_t deadline,uint8_t raw[7]){
    uint8_t reg=0x0a,second;
    if(!calendar_transfer(deadline,&reg,1,false)||!calendar_transfer(deadline,raw,4,true))return false;
    reg=0x0e;
    if(!calendar_transfer(deadline,&reg,1,false)||!calendar_transfer(deadline,raw+4,3,true))return false;
    reg=0x0a;
    return calendar_transfer(deadline,&reg,1,false)&&calendar_transfer(deadline,&second,1,true)&&second==raw[0];
}
static bool calendar_write(uint32_t deadline,const struct ln_calendar *c,bool *attempted){
    uint8_t values[]={c->second,c->minute,c->hour,c->weekday,c->day,c->month,(uint8_t)(c->year-2000u)};
    for(uint32_t i=0;i<7u;++i){
        if((int32_t)(ln_time_us()-deadline)>=0)return false;
        uint8_t bytes[2]={(uint8_t)(0x0au+i),(uint8_t)((values[i]/10u)*16u+values[i]%10u)};
        *attempted=true;if(!calendar_transfer(deadline,bytes,2,false))return false;
    }return true;
}
static void calendar_tick(struct ln_calendar *c){
    if(++c->second<60u)return;c->second=0;if(++c->minute<60u)return;c->minute=0;
    if(++c->hour<24u)return;c->hour=0;c->weekday=(c->weekday+1u)%7u;
    if(++c->day<=calendar_days(c->year,c->month))return;c->day=1;
    if(++c->month<=12u)return;c->month=1;if(++c->year>2099u)c->year=2000u;
}
static bool calendar_equal(const struct ln_calendar *a,const struct ln_calendar *b){
    return a->year==b->year&&a->month==b->month&&a->day==b->day&&a->hour==b->hour&&
        a->minute==b->minute&&a->second==b->second&&a->weekday==b->weekday;
}
static bool calendar_verify(uint32_t deadline,const struct ln_calendar *expected,struct ln_calendar *actual){
    uint8_t raw[7];if(!calendar_raw(deadline,raw)||!calendar_decode(raw,actual))return false;
    if(calendar_equal(expected,actual))return true;
    struct ln_calendar next=*expected;calendar_tick(&next);return calendar_equal(&next,actual);
}
static void calendar_publish(const struct ln_calendar *c){
    ln_clock=(struct ln_clock_state){c->hour,c->minute,c->second,true};
}
enum ln_calendar_read_result ln_power_calendar_read_status(struct ln_calendar *out){
    if(!out||!ln_power_ready)return LN_CALENDAR_READ_UNAVAILABLE;uint8_t raw[7];struct ln_calendar c;
    ln_sample_cancel();
    if(!calendar_raw(ln_time_us()+60000u,raw))return LN_CALENDAR_READ_UNAVAILABLE;
    if(!calendar_decode(raw,&c))return LN_CALENDAR_READ_INVALID;
    *out=c;return LN_CALENDAR_READ_VALID;
}
bool ln_power_calendar_read(struct ln_calendar *out){return ln_power_calendar_read_status(out)==LN_CALENDAR_READ_VALID;}
/* PCF50605/50606 specification 8.19: RTC writes latch at the next 1 Hz
 * edge. Verification and any rollback must wait for that edge without
 * blocking the executive, USB, or watchdog. */
static struct {
    struct ln_calendar desired,baseline;
    uint32_t baseline_at,written_at,verify_at;
    uint8_t phase;
    bool baseline_valid,wrote;
} calendar_save;
bool ln_power_calendar_busy(void){return calendar_save.phase!=0;}
static void calendar_wait(uint8_t phase){
    calendar_save.phase=phase;calendar_save.written_at=ln_time_us();
    calendar_save.verify_at=calendar_save.written_at+1100000u;
}
enum ln_calendar_result ln_power_calendar_set(const struct ln_calendar *requested){
    if(!calendar_valid(requested))return LN_CALENDAR_INVALID;
    if(!ln_power_ready||ln_power_calendar_busy())return LN_CALENDAR_UNAVAILABLE;
    ln_sample_cancel();
    calendar_save=(typeof(calendar_save)){0};
    calendar_save.desired=*requested;calendar_weekday(&calendar_save.desired);
    uint32_t started=ln_time_us();uint8_t raw[7];bool attempted=false;
    if(!calendar_raw(started+60000u,raw))return LN_CALENDAR_UNAVAILABLE;
    calendar_save.baseline_at=ln_time_us();
    calendar_save.baseline_valid=calendar_decode(raw,&calendar_save.baseline);
    calendar_save.wrote=calendar_write(started+150000u,&calendar_save.desired,&attempted);
    if(!attempted)return LN_CALENDAR_UNAVAILABLE;
    ln_clock.valid=false;calendar_wait(1);return LN_CALENDAR_PENDING;
}
enum ln_calendar_result ln_power_calendar_poll(void){
    if(!calendar_save.phase)return LN_CALENDAR_UNAVAILABLE;
    uint32_t now=ln_time_us();
    if((int32_t)(now-calendar_save.verify_at)<0)return LN_CALENDAR_PENDING;
    ln_sample_cancel();
    struct ln_calendar expected=calendar_save.desired,actual;
    /* A delayed runtime step may observe additional elapsed seconds. The
     * latch can occur anywhere within the first second after the write. */
    uint32_t elapsed=(now-calendar_save.written_at)/1000000u;
    if(elapsed>30u){calendar_save.phase=0;ln_clock.valid=false;return LN_CALENDAR_UNKNOWN;}
    while(elapsed>1u){calendar_tick(&expected);--elapsed;}
    if(calendar_save.wrote&&calendar_verify(now+60000u,&expected,&actual)){
        enum ln_calendar_result result=calendar_save.phase==1?LN_CALENDAR_SAVED:LN_CALENDAR_RESTORED;
        calendar_save.phase=0;calendar_publish(&actual);return result;
    }
    if(calendar_save.phase==1&&calendar_save.baseline_valid){
        uint32_t seconds=(ln_time_us()-calendar_save.baseline_at)/1000000u;
        if(seconds<=30u){
            calendar_save.desired=calendar_save.baseline;
            while(seconds--)calendar_tick(&calendar_save.desired);
            bool attempted=false;
            calendar_save.wrote=calendar_write(ln_time_us()+150000u,&calendar_save.desired,&attempted);
            if(attempted){calendar_wait(2);return LN_CALENDAR_PENDING;}
        }
    }
    calendar_save.phase=0;ln_clock.valid=false;return LN_CALENDAR_UNKNOWN;
}

bool ln_power_poll(uint32_t now, struct ln_power_state *out)
{
    if (!ln_power_ready || !out) return false;
    if ((int32_t)(now - ln_power.next_sample_us) < 0) { *out = ln_power; return false; }
#if defined(LN_SAMPLE_DIAGNOSTICS)
    uint32_t late=now-ln_power.next_sample_us;
    uint32_t *maximum=ln_sampler.age_valid?&ln_sample_diagnostics.active_late:&ln_sample_diagnostics.idle_late;
    if(late>*maximum)*maximum=late;
#endif
    uint8_t old_flags = ln_power.flags;
    uint16_t old_mv = ln_power.battery_mv;
    struct ln_clock_state old_clock=ln_clock;
    enum ln_sample_transfer transfer;
    uint8_t bytes[2];
    if(ln_sampler.stage==LN_SAMPLE_IDLE){
        if(!ln_sampler.age_valid){
            ln_sampler.started_us=ln_time_us();
            ln_sampler.age_valid=1;
            ln_sampler.age_phase=LN_SAMPLE_AGE_ADC;
#if defined(LN_SAMPLE_DIAGNOSTICS)
            struct ln_core_status core;ln_core_snapshot(&core);
            ln_adc_current=(struct ln_adc_trace){.started=ln_sampler.started_us,.idle_us=core.idle_us};
            ln_adc_idle_current=(struct ln_adc_idle_trace){0};
#endif
        }
        if(ln_sampler.age_phase==LN_SAMPLE_AGE_RTC){
            /* The ADC result is already published; discard any partial RTC
             * bytes and restart its complete sequence at the pointer. */
            ln_sampler.stage=LN_SAMPLE_RTC_POINTER_TIME;
        }else{
            ln_sampler.flags=ln_power_sources();
            if(ln_target_battery_charging())ln_sampler.flags|=LINNET_POWER_CHARGING;
            ln_sampler.stage=LN_SAMPLE_ADC_COMMAND;
        }
    }
    if(ln_sampler.age_phase==LN_SAMPLE_AGE_RTC){
        if(ln_sample_expired(LN_SAMPLE_RTC_MAX_AGE_US)){
            ln_clock.valid=false;ln_sample_next_cycle();goto complete;
        }
    }else if(ln_sample_expired(LN_SAMPLE_ADC_MAX_AGE_US)){
        ln_sample_fail(1u);goto complete;
    }
    switch(ln_sampler.stage){
    case LN_SAMPLE_ADC_COMMAND:
        bytes[0]=0x2fu;bytes[1]=5u;
        transfer=ln_sample_transfer(bytes,2u,false,true);
        if(transfer==LN_SAMPLE_TRANSFER_OK){ln_sampler.stage=LN_SAMPLE_ADC_POINTER;
#if defined(LN_SAMPLE_DIAGNOSTICS)
            ln_adc_current.command_age=ln_time_us()-ln_sampler.started_us;ln_adc_current.pointer_age=0;
#endif
        }
        break;
    case LN_SAMPLE_ADC_POINTER:
        bytes[0]=0x30u;
        transfer=ln_sample_transfer(bytes,1u,false,false);
        if(transfer==LN_SAMPLE_TRANSFER_OK){ln_sampler.stage=LN_SAMPLE_ADC_READ;
#if defined(LN_SAMPLE_DIAGNOSTICS)
            ln_adc_current.pointer_age=ln_time_us()-ln_sampler.started_us;
#endif
        }
        break;
    case LN_SAMPLE_ADC_READ:
        transfer=ln_sample_transfer(ln_sampler.data,2u,true,true);
        if(transfer==LN_SAMPLE_TRANSFER_RETRY)ln_sampler.stage=LN_SAMPLE_ADC_POINTER;
        if(transfer==LN_SAMPLE_TRANSFER_OK){
            if(ln_sample_expired(ln_sampler.age_phase==LN_SAMPLE_AGE_RTC?
                LN_SAMPLE_RTC_MAX_AGE_US:LN_SAMPLE_ADC_MAX_AGE_US)){
                ln_sample_fail(2u);break;
            }
            uint16_t raw=(uint16_t)(((uint16_t)ln_sampler.data[0]<<2)|(ln_sampler.data[1]&3u));
            uint16_t mv=(uint16_t)(((uint32_t)raw*6000u)>>10);
            ln_power.flags=ln_sampler.flags;ln_power.battery_raw=raw;
            if(mv>=2500u&&mv<=5000u){
                ln_power.battery_mv=mv;ln_power.battery_percent=ln_battery_percent(mv);
                ln_power.flags|=LINNET_POWER_BATTERY_VALID;
            }else {
#if defined(LN_SAMPLE_DIAGNOSTICS)
                ln_sample_failure(4u);
#endif
                ++ln_power.failures;
            }
            ++ln_power.samples;ln_power.sampled_us=ln_time_us();
            ln_sampler.stage=LN_SAMPLE_RTC_POINTER_TIME;
            if(ln_sampler.age_phase==LN_SAMPLE_AGE_ADC){
                ln_sampler.started_us=ln_time_us();
                ln_sampler.age_phase=LN_SAMPLE_AGE_RTC;
            }
        }
        break;
    case LN_SAMPLE_RTC_POINTER_TIME:
        bytes[0]=0x0au;transfer=ln_sample_transfer(bytes,1u,false,false);
        if(transfer==LN_SAMPLE_TRANSFER_OK)ln_sampler.stage=LN_SAMPLE_RTC_READ_TIME;
        break;
    case LN_SAMPLE_RTC_READ_TIME:
        transfer=ln_sample_transfer(ln_sampler.data,4u,true,true);
        if(transfer==LN_SAMPLE_TRANSFER_RETRY)ln_sampler.stage=LN_SAMPLE_RTC_POINTER_TIME;
        if(transfer==LN_SAMPLE_TRANSFER_OK)ln_sampler.stage=LN_SAMPLE_RTC_POINTER_DATE;
        break;
    case LN_SAMPLE_RTC_POINTER_DATE:
        bytes[0]=0x0eu;transfer=ln_sample_transfer(bytes,1u,false,false);
        if(transfer==LN_SAMPLE_TRANSFER_OK)ln_sampler.stage=LN_SAMPLE_RTC_READ_DATE;
        break;
    case LN_SAMPLE_RTC_READ_DATE:
        transfer=ln_sample_transfer(ln_sampler.data+4u,3u,true,true);
        if(transfer==LN_SAMPLE_TRANSFER_RETRY)ln_sampler.stage=LN_SAMPLE_RTC_POINTER_DATE;
        if(transfer==LN_SAMPLE_TRANSFER_OK)ln_sampler.stage=LN_SAMPLE_RTC_POINTER_VERIFY;
        break;
    case LN_SAMPLE_RTC_POINTER_VERIFY:
        bytes[0]=0x0au;transfer=ln_sample_transfer(bytes,1u,false,false);
        if(transfer==LN_SAMPLE_TRANSFER_OK)ln_sampler.stage=LN_SAMPLE_RTC_READ_VERIFY;
        break;
    case LN_SAMPLE_RTC_READ_VERIFY:
        transfer=ln_sample_transfer(&ln_sampler.verify_second,1u,true,true);
        if(transfer==LN_SAMPLE_TRANSFER_RETRY)ln_sampler.stage=LN_SAMPLE_RTC_POINTER_VERIFY;
        if(transfer==LN_SAMPLE_TRANSFER_OK){
            struct ln_calendar calendar;ln_clock.valid=false;
            if(!ln_sample_expired(LN_SAMPLE_RTC_MAX_AGE_US)&&
               ln_sampler.verify_second==ln_sampler.data[0]&&calendar_decode(ln_sampler.data,&calendar))
                ln_clock=(struct ln_clock_state){calendar.hour,calendar.minute,calendar.second,true};
            ln_sample_next_cycle();
        }
        break;
    default:
        transfer=LN_SAMPLE_TRANSFER_FAILED;
        break;
    }
    if(transfer==LN_SAMPLE_TRANSFER_FAILED){
        if(ln_sampler.age_phase==LN_SAMPLE_AGE_ADC)ln_sample_fail(3u);
        else {ln_clock.valid=false;ln_sample_next_cycle();}
    }else if(ln_sampler.stage!=LN_SAMPLE_IDLE)ln_sample_next_step();
complete:
    *out = ln_power;
    return old_flags != ln_power.flags || old_mv != ln_power.battery_mv ||
        old_clock.valid!=ln_clock.valid || (ln_clock.valid&&
        (old_clock.hour!=ln_clock.hour||old_clock.minute!=ln_clock.minute));
}
void ln_power_snapshot(struct ln_power_state *out) { if (out) *out = ln_power; }
void ln_power_clock_snapshot(struct ln_clock_state *out) { if (out) *out = ln_clock; }
uint32_t ln_power_next_deadline(void) { return ln_power.next_sample_us; }
void ln_power_backlight(bool on, uint8_t brightness)
{
    ln_target_backlight(on, brightness);
    ln_power.backlight_on = on;
    ln_power.brightness = brightness;
}
bool ln_power_sleep(void)
{
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    ln_power_backlight(false, ln_power.brightness);
    ln_power.sleeping = 1;
    return true; /* Retained idle; executive sleeps the CPU until wake events. */
}
bool ln_power_wake(void)
{
    ln_power.sleeping = 0;
    ln_power_backlight(true, ln_power.brightness);
    return true;
}
bool ln_power_charge_limit(uint16_t milliamps)
{
    if (milliamps != 0 && milliamps != 100 && milliamps != 500) return false;
    ln_target_charge_limit_set(milliamps);
    ln_power.charge_limit_ma = milliamps;
    return true;
}
bool ln_power_shutdown(uint8_t wake_sources)
{
    if (!(wake_sources & 3u) || (wake_sources & ~3u)) return false;
    uint8_t command = 1u;
    if (wake_sources & LINNET_POWER_USB) command |= 0x20u;
    if (wake_sources & LINNET_POWER_FIREWIRE) command |= 0x40u;
    /* Caller first stops USB/audio and flushes storage. Wake flags are mandatory. */
    return ln_power_i2c_write(8u, 8u, command);
}
