#include "pcf50635.h"
bool ln_pcf50635_battery_begin(struct ln_pcf50635_adc *s,ln_pcf_write write,uint32_t now) {
    if(!s || !write)return false;
    *s=(struct ln_pcf50635_adc){.deadline=now+100000u};
    /* BATSNS / 3, 2.0 V reference, 10-bit conversion. */
    if(!write(0x52,0) || !write(0x53,0) || !write(0x54,1))return false;
    s->phase=1;return true;
}
int ln_pcf50635_battery_poll(struct ln_pcf50635_adc *s,ln_pcf_read read,uint32_t now) {
    if(!s || !read)return -1;
    if(s->phase==3)return 1;
    if(!s->phase || (int32_t)(now-s->deadline)>=0){s->phase=0;return -1;}
    uint8_t value;
    if(!read(s->phase==1?0x57:0x55,&value,1)){s->phase=0;return -1;}
    if(s->phase==1) {
        if(value&0x80){s->low=value&3;s->phase=2;}
        return 0;
    }
    s->raw=((uint16_t)value<<2)|s->low;
    s->millivolts=(uint16_t)(((uint32_t)s->raw*6000u)>>10);
    s->phase=3;return 1;
}
