#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef void (*ln_s5l_irq_handler)(uint32_t bank0,uint32_t bank1);
/* Startup owns both VICs. Install vectors and an IRQ stack before unmasking. */
void ln_s5l_irq_init(ln_s5l_irq_handler handler);
void ln_s5l_irq_enable(unsigned source,bool enabled);
void ln_s5l_irq_software(unsigned source,bool asserted);
uint32_t ln_s5l_irq_save(void);
void ln_s5l_irq_restore(uint32_t flags);
/* Caller masks CPU IRQ and arms a hardware wake source first. WFI wakes on
 * the IRQ signal even while masked; restore flags afterward to dispatch it. */
void ln_s5l_irq_wait(void);
void ln_s5l_irq_dispatch(void);
