#include "kernel/shell.h"
#include "hal/api/display.h"
#include "services/display/settings.h"
#include "services/display/clock.h"
#include "hal/api/power.h"
uint32_t ln_ui_battery_status(void){struct ln_power_state p;ln_power_snapshot(&p);return (p.flags&LINNET_POWER_BATTERY_VALID)?p.battery_percent|((p.flags&LINNET_POWER_CHARGING)?256u:0u)|((p.flags&(LINNET_POWER_USB|LINNET_POWER_FIREWIRE))?512u:0u):UINT32_MAX;}
uint32_t ln_ui_clock_seconds(void){struct ln_clock_state c;ln_power_clock_snapshot(&c);return c.valid?(uint32_t)c.hour*3600u+(uint32_t)c.minute*60u+c.second:UINT32_MAX;}
uint32_t ln_ui_theme_preference(void){return ln_display_theme_index();}
void ln_ui_theme_changed(uint32_t index){(void)ln_display_theme_set(index);}
const char *ln_ui_theme_status(void){return ln_display_theme_status();}
static void damage(int32_t x,int32_t y,int32_t w,int32_t h){(void)ln_display_damage(x,y,w,h);}
void ln_shell_init(ln_shell *state){ln_ui_init(state);}
bool ln_shell_input(ln_shell *state,uint32_t kind,int32_t value){return ln_ui_input(state,kind,value);}
void ln_shell_render(ln_shell *state,uint16_t *pixels){ln_ui_render(state,pixels,damage);}
void ln_shell_animate(ln_shell *state,uint16_t *pixels,uint32_t frame){ln_ui_animate(state,pixels,frame,damage);}
uint32_t ln_shell_take_action(ln_shell *state){return ln_ui_take_action(state);}

int ln_ui_clock_input(struct ln_ui_state*s,uint32_t kind,int32_t value){return ln_clock_editor_input(s,kind,value);}
void ln_ui_clock_cancel(void){ln_clock_editor_cancel();}
const char *ln_ui_clock_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){return ln_clock_editor_text(n,item,fallback);}

bool ln_ui_clock_12(void){return ln_display_clock_12();}
void ln_ui_clock_toggle(void){ln_display_clock_toggle();}
