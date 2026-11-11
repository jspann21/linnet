#ifndef LINNET_UPDATE_H
#define LINNET_UPDATE_H
#include "services/state/store.h"
#include "boot/include/linnet_boot.h"
#include "linnet_target.h"
#define LN_UPDATE_METADATA_SIZE 184u
#define LN_UPDATE_IMAGE_MAX LINNET_OS_CAPACITY
#define LN_UPDATE_CHUNK_MAX 4096u
struct ln_update_image {uint32_t length,profile;uint8_t build_id[32],sha256[32];};
struct ln_update_io {
    void *context;
    /* Exact transfer, 1 success, anything else failure. Bounded by adapter. */
    int (*read)(void *,uint32_t slot,uint32_t offset,uint8_t *,uint32_t length);
    int (*write)(void *,uint32_t slot,uint32_t offset,const uint8_t *,uint32_t length);
    int (*flush)(void *);
    uint32_t (*capacity)(void *,uint32_t slot);
    void (*progress)(void *); /* Only called after finite verified work. */
};
struct ln_update {
    struct ln_state_store *store;
    struct ln_update_io io;
    struct ln_image_policy policy;
    struct ln_boot_state boot;
    struct ln_update_image image[2];
    uint32_t upload_slot,upload_size,upload_offset;
    uint8_t upload_sha[32],upload_active,ready;
    uint8_t rejected_slot; /* Transient failed candidate validation, or255. */
};
/* Metadata path /.LINNET/BOOT{0,1}.BIN; image paths /.LINNET/OS{0,1}.LOS.
 * Open store first; only initialize on explicitly blank store. */
int ln_update_open(struct ln_update *,struct ln_state_store *,const struct ln_update_io *,const struct ln_image_policy *);
int ln_update_initialize(struct ln_update *,uint32_t confirmed_slot,uint32_t image_length,
                         const uint8_t expected_sha[32],uint8_t *scratch,uint32_t capacity);
int ln_update_begin(struct ln_update *,uint32_t length,const uint8_t expected_sha[32]);
int ln_update_write(struct ln_update *,uint32_t offset,const uint8_t *,uint32_t length);
int ln_update_finish(struct ln_update *,uint8_t *scratch,uint32_t capacity);
int ln_update_validate(struct ln_update *,uint32_t slot,uint8_t *scratch,uint32_t capacity,struct ln_image_info *);
/* Call exactly once per reset before selection; persistent pending candidate rolls back. */
int ln_update_after_reset(struct ln_update *);
/* Validates candidate then confirmed fallback, retaining selected image in
 * scratch, and durably marks attempt. rejected_slot is transient, not metadata.
 * Returns 0/1 slot, or255 failure. Do not boot on failure. */
uint8_t ln_update_prepare_boot(struct ln_update *,uint8_t *scratch,uint32_t capacity,struct ln_image_info *);
int ln_update_mark_healthy(struct ln_update *,uint32_t running_slot,const uint8_t running_sha[32],uint32_t readiness);
int ln_update_metadata_encode(const struct ln_update *,uint8_t payload[LN_UPDATE_METADATA_SIZE]);
#endif
