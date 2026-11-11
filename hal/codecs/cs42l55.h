#ifndef LINNET_CS42L55_H
#define LINNET_CS42L55_H
#include "codec.h"
/* MCLK must remain valid throughout preparation, settling and PDN. A failed
 * write/delay leaves power and clock ownership with the caller for retry. */
bool ln_cs42l55_shutdown(ln_codec_writer write,uint32_t settling_us);
#endif
