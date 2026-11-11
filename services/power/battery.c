#include "services/power/battery.h"
bool ln_battery_shutdown_due(struct ln_battery_guard *g,const struct ln_power_state *p,uint32_t now){
    if(!g||!p)return false;
    if(!(p->flags&LINNET_POWER_BATTERY_VALID)||now-p->sampled_us>2000000u){
        g->consecutive=0;return false;
    }
    bool charging=(p->flags&LINNET_POWER_CHARGING)&&
        ((p->flags&LINNET_POWER_FIREWIRE)||((p->flags&LINNET_POWER_USB)&&p->charge_limit_ma));
    /* Photo/Color thresholds from the existing target power measurements:
     * disk-safe 3450 mV, absolute shutdown floor 3300 mV. A plugged-in cable
     * alone does not exempt a suspended USB device from protection. */
    if(p->battery_mv>3450u||(charging&&p->battery_mv>3300u)){
        g->consecutive=0;return false;
    }
    if(p->samples==g->sample)return g->consecutive==3u;
    if(g->consecutive&&p->sampled_us-g->sampled_us>2000000u)g->consecutive=0;
    g->sample=p->samples;g->sampled_us=p->sampled_us;
    if(g->consecutive<3u)++g->consecutive;
    return g->consecutive==3u;
}
