#ifndef LINNET_STORAGE_LINK_H
#define LINNET_STORAGE_LINK_H
#include "hal/link/link.h"
/* Development builds only, after transport CRC/sequence validation. */
int ln_storage_link_dispatch(const ln_link_frame *,ln_link_frame *,uint8_t *reply);
void ln_storage_link_ack_sent(uint16_t command);
#endif
