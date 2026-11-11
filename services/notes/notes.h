#ifndef LINNET_NOTES_H
#define LINNET_NOTES_H
#include "ui/runtime/ui.h"
void ln_notes_step(struct ln_ui_state *shell);
bool ln_notes_input(struct ln_ui_state *shell,uint32_t kind,int32_t value);
bool ln_notes_pending(void);
void ln_notes_close(void);
uint32_t ln_notes_count(void);
const char *ln_notes_label(uint32_t row);
const char *ln_notes_title(void);
#endif
