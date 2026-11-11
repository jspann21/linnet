#include "hal/api/power.h"
#include "hal/api/platform.h"
#include "hal/s5l8702/i2c.h"
#include "hal/pmu/pcf50635.h"
#include "hal/s5l8702/registers.h"
static struct ln_power_state state;
static struct ln_clock_state clock_state;
static struct ln_pcf50635_adc adc;
static uint8_t phase;
static bool read_pmu(uint8_t reg,uint8_t *data,unsigned size){return ln_s5l_i2c_read(0,0x73,reg,data,size);}
static bool write_pmu(uint8_t reg,uint8_t value){return ln_s5l_i2c_write(0,0x73,reg,&value,1);}
bool ln_power_i2c_read(uint8_t address,uint8_t reg,uint8_t *out,uint32_t size){return ln_s5l_i2c_read(0,address,reg,out,size);}
bool ln_power_i2c_write(uint8_t address,uint8_t reg,uint8_t value){return ln_s5l_i2c_write(0,address,reg,&value,1);}
static uint8_t decimal(uint8_t byte){return (byte&15)>9 || (byte>>4)>9?255:(byte>>4)*10+(byte&15);}
static uint8_t bcd(unsigned value){return (uint8_t)((value/10)*16+value%10);}
static bool calendar_valid(const struct ln_calendar *c) {
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return c && c->year>=2000 && c->year<=2099 && c->month>=1 && c->month<=12 &&
        c->day>=1 && c->day<=days[c->month-1]+(c->month==2 && !(c->year%4)) &&
        c->hour<24 && c->minute<60 && c->second<60;
}
enum ln_calendar_read_result ln_power_calendar_read_status(struct ln_calendar *out) {
    uint8_t raw[7],seconds;
    if(!out)return LN_CALENDAR_READ_INVALID;
    /* Re-read seconds to reject a rollover during the calendar burst. */
    if(!read_pmu(0x59,raw,7) || !read_pmu(0x59,&seconds,1) || seconds!=raw[0])return LN_CALENDAR_READ_UNAVAILABLE;
    *out=(struct ln_calendar){2000u+decimal(raw[6]),decimal(raw[5]),decimal(raw[4]),
        decimal(raw[2]),decimal(raw[1]),decimal(raw[0]),decimal(raw[3])};
    return calendar_valid(out) && out->weekday<7?LN_CALENDAR_READ_VALID:LN_CALENDAR_READ_INVALID;
}
bool ln_power_calendar_read(struct ln_calendar *out){return ln_power_calendar_read_status(out)==LN_CALENDAR_READ_VALID;}
static struct ln_calendar desired,previous;
static uint8_t calendar_phase,calendar_second,calendar_verify_retries;
static bool calendar_previous_valid;
static uint32_t calendar_deadline,calendar_written_at,calendar_baseline_at;
static enum ln_calendar_result calendar_result=LN_CALENDAR_UNAVAILABLE;
static bool calendar_write(const struct ln_calendar *c) {
    uint8_t raw[]={bcd(c->second),bcd(c->minute),bcd(c->hour),bcd(c->weekday),bcd(c->day),bcd(c->month),bcd(c->year-2000)};
    return ln_s5l_i2c_write(0,0x73,0x59,raw,7);
}
static bool calendar_matches(const struct ln_calendar *a,const struct ln_calendar *b) {
    return a->year==b->year && a->month==b->month && a->day==b->day && a->hour==b->hour &&
        a->minute==b->minute && a->second==b->second && a->weekday==b->weekday;
}
static void calendar_tick(struct ln_calendar *c) {
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(++c->second<60)return;c->second=0;
    if(++c->minute<60)return;c->minute=0;
    if(++c->hour<24)return;c->hour=0;c->weekday=(c->weekday+1u)%7u;
    if(++c->day<=days[c->month-1]+(c->month==2&&!(c->year%4)))return;c->day=1;
    if(++c->month<=12)return;c->month=1;if(++c->year>2099)c->year=2000;
}
static bool calendar_verified(const struct ln_calendar *actual,const struct ln_calendar *written) {
    uint32_t elapsed=(ln_time_us()-calendar_written_at)/1000000u;
    if(!elapsed||elapsed>30u)return false;
    struct ln_calendar expected=*written;
    while(elapsed>1u){calendar_tick(&expected);--elapsed;}
    if(calendar_matches(actual,&expected))return true;
    calendar_tick(&expected);return calendar_matches(actual,&expected);
}
enum ln_calendar_result ln_power_calendar_set(const struct ln_calendar *requested) {
    if(calendar_phase)return LN_CALENDAR_PENDING;
    if(!calendar_valid(requested))return LN_CALENDAR_INVALID;
    enum ln_calendar_read_result previous_status=ln_power_calendar_read_status(&previous);
    if(previous_status==LN_CALENDAR_READ_UNAVAILABLE)return LN_CALENDAR_UNAVAILABLE;
    calendar_previous_valid=previous_status==LN_CALENDAR_READ_VALID;
    calendar_baseline_at=ln_time_us();
    desired=*requested;
    unsigned days=6; /* 2000-01-01 was Saturday (Sunday = 0). */
    for(unsigned year=2000;year<desired.year;year++)days+=365+!(year%4);
    static const uint8_t month_days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    for(unsigned month=1;month<desired.month;month++)days+=month_days[month-1]+(month==2 && !(desired.year%4));
    desired.weekday=(days+desired.day-1)%7;
    calendar_second=calendar_previous_valid?previous.second:255;calendar_deadline=ln_time_us()+1200000u;
    calendar_phase=calendar_previous_valid?1:2;
    return calendar_result=LN_CALENDAR_PENDING;
}
bool ln_power_calendar_busy(void){return calendar_phase!=0;}
enum ln_calendar_result ln_power_calendar_poll(void) {
    if(!calendar_phase)return calendar_result;
    static uint8_t verify_bytes[7];
    uint8_t second;
    uint32_t now=ln_time_us();
    if(calendar_phase==1) {
        if((int32_t)(now-calendar_deadline)>=0){calendar_phase=0;return calendar_result=LN_CALENDAR_UNAVAILABLE;}
        if(!read_pmu(0x59,&second,1))goto failed;
        if(calendar_second==255){calendar_second=decimal(second);return LN_CALENDAR_PENDING;}
        if(decimal(second)==calendar_second)return LN_CALENDAR_PENDING;
        calendar_phase++;return LN_CALENDAR_PENDING;
    }
    if(calendar_phase==6){
        if((int32_t)(now-calendar_deadline)<0)return LN_CALENDAR_PENDING;
        calendar_phase=7;return LN_CALENDAR_PENDING;
    }
    if(calendar_phase==2 || calendar_phase==7) {
        if(calendar_phase==7){
            uint32_t elapsed=(now-calendar_baseline_at)/1000000u;
            if(elapsed>30u)goto failed;
            while(elapsed--){calendar_tick(&previous);}
        }
        if(!calendar_write(calendar_phase==2?&desired:&previous))goto failed;
        calendar_written_at=ln_time_us();calendar_deadline=calendar_written_at+1100000u;
        calendar_verify_retries=0;
        calendar_phase++;return LN_CALENDAR_PENDING;
    }
    if(calendar_phase==3 || calendar_phase==8){
        if((int32_t)(now-calendar_deadline)<0)return LN_CALENDAR_PENDING;
        calendar_phase++;return LN_CALENDAR_PENDING;
    }
    if(calendar_phase==4 || calendar_phase==9) {
        if(!read_pmu(0x59,verify_bytes,7))goto failed;
        calendar_phase++;return LN_CALENDAR_PENDING;
    }
    if(!read_pmu(0x59,&second,1))goto failed;
    if(second!=verify_bytes[0]){
        if(calendar_verify_retries++<2u){calendar_phase--;return LN_CALENDAR_PENDING;}
        goto failed;
    }
    struct ln_calendar check={2000u+decimal(verify_bytes[6]),decimal(verify_bytes[5]),decimal(verify_bytes[4]),
        decimal(verify_bytes[2]),decimal(verify_bytes[1]),decimal(verify_bytes[0]),decimal(verify_bytes[3])};
    if(!calendar_valid(&check)||check.weekday>=7||
       !calendar_verified(&check,calendar_phase==5?&desired:&previous))goto failed;
    clock_state=(struct ln_clock_state){check.hour,check.minute,check.second,true};
    calendar_result=calendar_phase==5?LN_CALENDAR_SAVED:LN_CALENDAR_RESTORED;
    calendar_phase=0;return calendar_result;
failed:
    if(calendar_phase==1){calendar_phase=0;return calendar_result=LN_CALENDAR_UNAVAILABLE;}
    if(!calendar_previous_valid){calendar_phase=0;return calendar_result=LN_CALENDAR_UNKNOWN;}
    if(calendar_phase>=6){calendar_phase=0;return calendar_result=LN_CALENDAR_UNKNOWN;}
    calendar_phase=6;calendar_deadline=ln_time_us()+1100000u;
    return LN_CALENDAR_PENDING;
}

void ln_power_clock_snapshot(struct ln_clock_state *out){if(out)*out=clock_state;}
void ln_power_snapshot(struct ln_power_state *out){if(out)*out=state;}
uint32_t ln_power_next_deadline(void){return state.next_sample_us;}
uint8_t ln_power_sources(void){return state.flags&(LINNET_POWER_USB|LINNET_POWER_FIREWIRE);}
bool ln_power_init(void) {
    uint8_t id[2],status;
    if(!read_pmu(0,id,2) || id[0]!=0x12 || id[1]!=3 || !read_pmu(0x12,&status,1))return false;
    /* N25 uses an external LTC4066 for USB charging, not the PMU USB charger.
     * B6=HPWR, B7=SUSP; C0 doubles the limit and C1/C2 inhibit charging. */
    LN_CLOCK_GATE1&=~(1u<<12);
    LN_REG(0x3cf00160u)=(LN_REG(0x3cf00160u)&0xffffu)|0xee000000u;
    LN_REG(0x3cf00180u)=(LN_REG(0x3cf00180u)&0xffff0000u)|0xeeeu;
    state=(struct ln_power_state){.brightness=128,.next_sample_us=ln_time_us(),
        .charge_limit_ma=100,.flags=(status&4)?LINNET_POWER_USB:0};
    phase=0;clock_state.valid=false;return true;
}
bool ln_power_poll(uint32_t now,struct ln_power_state *out) {
    if((int32_t)(now-state.next_sample_us)>=0 && !calendar_phase) {
        bool ok=true;
        if(phase==0){ok=ln_pcf50635_battery_begin(&adc,write_pmu,now);phase=1;}
        else if(phase==1) {
            int result=ln_pcf50635_battery_poll(&adc,read_pmu,now);ok=result>=0;
            if(result==1) {
                state.battery_mv=adc.millivolts;state.battery_raw=adc.raw;
                /* Voltage estimate only; the transplanted battery has no gauge. */
                state.battery_percent=adc.millivolts<=3300?0:adc.millivolts>=4200?100:((uint32_t)(adc.millivolts-3300)*7282u)>>16;
                state.flags=(state.flags|LINNET_POWER_BATTERY_VALID)&~LINNET_POWER_I2C_FAULT;
                state.samples++;state.sampled_us=now;phase=2;
            }
        } else if(phase==2) {
            uint8_t charger;ok=read_pmu(0x4b,&charger,1);
            if(ok) {
                state.flags=(state.flags&~LINNET_POWER_FIREWIRE)|((charger&4)?LINNET_POWER_FIREWIRE:0);
                phase=3;
            }
        } else if(phase==3) {
            uint8_t status;ok=read_pmu(0x12,&status,1);
            if(ok) {
                state.flags=(state.flags&~(LINNET_POWER_USB|LINNET_POWER_CHARGING))|
                    ((status&4)?LINNET_POWER_USB:0)|((LN_REG(0x3cf00164u)&0x10)?0:LINNET_POWER_CHARGING);
                phase=4;
            }
        } else {
            struct ln_calendar calendar;clock_state.valid=ln_power_calendar_read(&calendar);
            if(clock_state.valid){clock_state.hour=calendar.hour;clock_state.minute=calendar.minute;clock_state.second=calendar.second;}
            phase=0;
        }
        if(!ok){state.failures++;state.flags=(state.flags|LINNET_POWER_I2C_FAULT)&~LINNET_POWER_BATTERY_VALID;phase=0;}
        state.next_sample_us=now+(phase?1000u:1000000u);
    }
    if(out)*out=state;return !(state.flags&LINNET_POWER_I2C_FAULT);
}
void ln_power_backlight(bool on,uint8_t brightness) {
    unsigned scaled=(unsigned)brightness*63u+127u;
    uint8_t level=(uint8_t)((scaled+1u+(scaled>>8))>>8);
    bool ok=write_pmu(0x2a,5) && write_pmu(0x28,level) &&
        write_pmu(0x29,on && brightness?1:0);
    if(ok){state.backlight_on=on && brightness;state.brightness=brightness;}
    else {state.failures++;state.flags|=LINNET_POWER_I2C_FAULT;}
}
bool ln_power_sleep(void){
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    uint32_t failures=state.failures;
    ln_power_backlight(false,state.brightness);
    if(state.failures!=failures)return false;
    state.sleeping=true;return true;
}
bool ln_power_wake(void){
    uint32_t failures=state.failures;
    ln_power_backlight(true,state.brightness);
    /* Brightness zero keeps the panel dark but is still a successful wake. */
    if(state.failures!=failures)return false;
    state.sleeping=false;return true;
}
bool ln_power_shutdown(uint8_t sources) {
#if defined(LINNET_HARDWARE_CLICKER) && defined(LINNET_IMAGE_OS)
    ln_platform_click_stop();
#endif
    if(!sources || (sources&~(LINNET_POWER_USB|LINNET_POWER_FIREWIRE)))return false;
    return write_pmu(0x0d,1u|((sources&LINNET_POWER_USB)?4u:0)|((sources&LINNET_POWER_FIREWIRE)?0x80u:0)) && write_pmu(0x0c,1);
}
bool ln_power_charge_limit(uint16_t milliamps) {
    if(milliamps!=0 && milliamps!=100 && milliamps!=500)return false;
    LN_CLOCK_GATE1&=~(1u<<12);
    LN_REG(0x3cf00160u)=(LN_REG(0x3cf00160u)&0xffffu)|0xee000000u;
    LN_REG(0x3cf00180u)=(LN_REG(0x3cf00180u)&0xffff0000u)|0xeeeu;
    LN_REG(0x3cf00200u)=0xb070eu|(milliamps==0);
    LN_REG(0x3cf00200u)=0xb060eu|(milliamps==500);
    uint32_t expected=(milliamps==0?0x80u:0)|(milliamps==500?0x40u:0);
    if((LN_REG(0x3cf00164u)&0xc0u)!=expected)return false;
    state.charge_limit_ma=milliamps;return true;
}
