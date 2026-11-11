#ifndef LINNET_SHA256_H
#define LINNET_SHA256_H
#include <stddef.h>
#include <stdint.h>
struct ln_sha256 {
    uint32_t words[8];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
};
void ln_sha256_init(struct ln_sha256 *state);
void ln_sha256_update(struct ln_sha256 *state, const uint8_t *bytes, size_t size);
void ln_sha256_finish(struct ln_sha256 *state, uint8_t digest[32]);
/* Boot optimization only: call after ARC idle proof, before admitting service.
 * SHA and CRC share the verified copy. Failure retains SDRAM implementations;
 * startup clears readiness before either internal-RAM path can be selected. */
#if defined(LINNET_IMAGE_BOOT) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_STORAGE)
int ln_sha256_boot_iram_init(void);
int ln_sha256_boot_iram_active(void);
#endif
#endif
