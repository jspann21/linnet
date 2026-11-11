#ifndef LINNET_VIDEO_CATALOG_H
#define LINNET_VIDEO_CATALOG_H
#include <stdbool.h>
#include <stdint.h>
void ln_video_catalog_step(bool busy);
bool ln_video_catalog_pending(void);
/* Files may use the aligned tail after all resident video rows validate.
 * Recheck before every access; phase changes or a new mount revoke the loan.
 * Catalog DMA never writes beyond the published row count. */
uint8_t *ln_video_catalog_files_memory(uint32_t *bytes,uint32_t *epoch);
#endif
