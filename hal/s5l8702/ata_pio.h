#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Controller handshake; offsets are ATA taskfile register offsets. */
bool ln_s5l_ata_read(unsigned offset,uint16_t *value);
bool ln_s5l_ata_write(unsigned offset,uint16_t value);
#if defined(LINNET_SRAM_MONITOR)
/* Read-only cached IDENTIFY for the SRAM installer's production-driver probe. */
bool ln_s5l_ata_identify_copy(uint16_t out[256]);
#endif
