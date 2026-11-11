#ifndef LINNET_PLATFORM_LINK_H
#define LINNET_PLATFORM_LINK_H
#include "hal/link/link.h"
/* Dev/trace source only. Returns0 for unrelated commands,1 for a response.
 * Transport must apply normal sequence/CRC validation before this adapter. */
int ln_platform_link_dispatch(const ln_link_frame *request, ln_link_frame *response, uint8_t *reply);
#endif
