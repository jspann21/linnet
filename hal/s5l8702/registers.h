#pragma once
#include <stdint.h>
#define LN_REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define LN_CLOCK_GATE0 LN_REG(0x3c500048u)
#define LN_CLOCK_GATE1 LN_REG(0x3c50004cu)
#define LN_TIMER_E(offset) LN_REG(0x3c7000a0u+(offset))
#define LN_DWC(offset) LN_REG(0x38400000u+(offset))
#define LN_PHY(offset) LN_REG(0x3c400000u+(offset))
#define LN_DWC_IN(ep,offset) LN_DWC(0x900u+(ep)*32u+(offset))
#define LN_DWC_OUT(ep,offset) LN_DWC(0xb00u+(ep)*32u+(offset))
/* Register facts: S5L8702 / Synopsys device core. Reference locations are in
 * targets/classic6g-n25/target.toml. No external driver is linked. */

