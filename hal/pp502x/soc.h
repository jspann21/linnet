#ifndef LINNET_PP502X_SOC_H
#define LINNET_PP502X_SOC_H
#include "linnet_target.h"
#if defined(LINNET_SOC_PP5022)
#define LN_PLL_80 0x8a121403u
#define LN_PLL_30 0x8a220501u
#else
#define LN_PLL_80 0x8a020a03u
#define LN_PLL_30 0x8a020504u
#endif
#endif
