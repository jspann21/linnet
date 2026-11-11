#ifndef LINNET_DISPLAY_PREFERENCES_H
#define LINNET_DISPLAY_PREFERENCES_H
#include "ui/runtime/ui.h"
uint32_t ln_preferences_brightness(void);
void ln_preferences_restore_brightness(uint32_t value);
void ln_preferences_changed(void);
void ln_preferences_open(struct ln_ui_state *state);
bool ln_preferences_input(struct ln_ui_state *state,uint32_t kind,int32_t value);
uint32_t ln_preferences_count(const struct ln_ui_node *node,uint32_t fallback);
const char *ln_preferences_text(const struct ln_ui_node *node,uint32_t item,const char *fallback);
const char *ln_preferences_status(void);
bool ln_preferences_editable(void);
bool ln_preferences_timeout_adjust(int32_t direction);
const char *ln_preferences_timeout_label(void);
#endif
