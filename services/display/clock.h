#ifndef LINNET_DISPLAY_CLOCK_H
#define LINNET_DISPLAY_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include "hal/api/power.h"
struct ln_ui_state;
struct ln_ui_node;
/* Memory-only draft controller. Caller owns admission and every RTC operation. */
void ln_clock_editor_begin(const struct ln_calendar *baseline,bool valid);
void ln_clock_editor_cancel(void);
/* 0 unhandled, 1 changed, 2 consumed without changing the draft. */
int ln_clock_editor_input(struct ln_ui_state *state,uint32_t kind,int32_t value);
bool ln_clock_editor_pending(void);
bool ln_clock_editor_request(struct ln_calendar *out);
void ln_clock_editor_result(enum ln_calendar_result result);
void ln_clock_editor_block(const char *reason);
const char *ln_clock_editor_text(const struct ln_ui_node *node,uint32_t item,const char *fallback);
#endif
