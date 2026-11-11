#ifndef LINNET_SHELL_H
#define LINNET_SHELL_H
#include <stdint.h>
#include <stdbool.h>
enum { LN_BUTTON_MENU=1,LN_BUTTON_PLAY=2,LN_BUTTON_NEXT=4,LN_BUTTON_PREV=8,LN_BUTTON_SELECT=16 };
#include "ui/runtime/ui.h"
typedef struct ln_ui_state ln_shell;
uint32_t ln_shell_take_action(ln_shell *state);
void ln_shell_init(ln_shell *state);
bool ln_shell_input(ln_shell *state,uint32_t kind,int32_t value);
void ln_shell_render(ln_shell *state,uint16_t *pixels);
void ln_shell_animate(ln_shell *state,uint16_t *pixels,uint32_t frame);
#endif
