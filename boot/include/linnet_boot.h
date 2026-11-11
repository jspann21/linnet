#ifndef LINNET_BOOT_H
#define LINNET_BOOT_H

#include <stddef.h>
#include <stdint.h>

#define LINNET_IMAGE_HEADER_SIZE 128u
#define LINNET_IMAGE_TARGET_PHOTO4G_P98 98u
#define LINNET_IMAGE_TARGET_IPOD4G_Q21 8u
#define LINNET_BOOT_STATE_SIZE 32u
#define LINNET_CRASH_MAILBOX_SIZE 144u
#define LINNET_BOOT_NO_SLOT 255u
#define LINNET_BOOT_HEALTH_MEMORY 1u
#define LINNET_BOOT_HEALTH_SERVICES 2u
#define LINNET_BOOT_HEALTH_STORAGE_RESOLVED 4u
#define LINNET_BOOT_HEALTH_FIRST_FRAME 8u
#define LINNET_BOOT_HEALTH_AUDIO_IDLE 16u
#define LINNET_BOOT_HEALTH_ALL 31u

enum ln_image_result {
    LINNET_IMAGE_OK = 0, LINNET_IMAGE_ARGUMENT, LINNET_IMAGE_TRUNCATED,
    LINNET_IMAGE_FORMAT, LINNET_IMAGE_TARGET,
    LINNET_IMAGE_PROFILE, LINNET_IMAGE_RANGE, LINNET_IMAGE_CHECKSUM
};

struct ln_image_policy {
    uint32_t target;
    uint32_t allowed_profiles; /* Bit 0 release, bit 1 dev, bit 2 trace. */
    uint32_t ram_start;
    uint32_t ram_size; /* Executable staging window, excluding loader/mailbox. */
};

struct ln_image_info {
    uint32_t profile;
    uint32_t payload_size;
    uint32_t load_address;
    uint32_t entry_address;
    uint8_t build_id[32];
};

/* CRC32/ISO-HDLC: reflected polynomial 0xedb88320, init/final xor ffffffff. */
uint32_t ln_crc32(const uint8_t *bytes, size_t length);
/* Incremental raw accumulator; callers own the initial/final XOR. */
uint32_t ln_crc32_update(uint32_t crc, const uint8_t *bytes, size_t length);
uint32_t ln_crc32_progress(const uint8_t *bytes, size_t length,
    void (*progress)(void *context), void *context);
#if defined(LINNET_IMAGE_OS) && defined(LINNET_PLATFORM_CORE) && defined(LINNET_AUDIO) && defined(LINNET_TARGET_VIDEO5G_M25)
#define LINNET_OS_CRC_IRAM 1
/* Early C entry and post-quiesce handoff only. Ordinary CRC calls dispatch
 * without rewriting code; a failed copy leaves the full SDRAM fallback active. */
int ln_crc32_os_iram_init(void);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_crc32_os_iram_diagnostics(uint32_t words[8]);
#endif
#endif
enum ln_image_result ln_image_validate(const uint8_t *image, size_t length,
    const struct ln_image_policy *policy, struct ln_image_info *info);
enum ln_image_result ln_image_validate_progress(const uint8_t *image, size_t length,
    const struct ln_image_policy *policy, struct ln_image_info *info,
    void (*progress)(void *context), void *context);
/* For a previously published slot after its complete image SHA256 matches
 * the stored digest. Installation still checks the payload CRC32. */
enum ln_image_result ln_image_validate_sha256_matched(const uint8_t *image, size_t length,
    const struct ln_image_policy *policy, struct ln_image_info *info);

struct ln_boot_state {
    uint32_t generation;
    uint32_t attempts;
    uint8_t confirmed_slot;
    uint8_t candidate_slot;
    uint8_t attempt_pending;
};

void ln_boot_state_init(struct ln_boot_state *state, uint8_t confirmed_slot);
int ln_boot_state_decode(struct ln_boot_state *state, const uint8_t *bytes, size_t length);
int ln_boot_state_encode(const struct ln_boot_state *state, uint8_t *bytes, size_t length);
/* Call once after reset, then durably publish the changed state before booting. */
int ln_boot_after_reset(struct ln_boot_state *state);
int ln_boot_offer_candidate(struct ln_boot_state *state, uint8_t slot);
/* valid_slots is the mask of slots which passed image validation this boot. */
uint8_t ln_boot_select(const struct ln_boot_state *state, uint8_t valid_slots);
/* Publish state transactionally before handing control to the selected image. */
int ln_boot_begin_attempt(struct ln_boot_state *state, uint8_t selected_slot);
int ln_boot_mark_healthy(struct ln_boot_state *state, uint8_t running_slot,
    uint32_t readiness);

struct ln_crash_record {
    uint32_t sequence;
    uint32_t reason;
    uint32_t registers[16]; /* r0..r15, r15 contains captured PC. */
    uint32_t cpsr;
    uint32_t flags;
    uint8_t build_id[32];
    uint32_t boot_generation;
    uint32_t active_service;
    uint32_t last_event;
};

/* Caller owns retained memory placement, cache clean, barriers and reset. */
int ln_crash_encode(const struct ln_crash_record *record, uint8_t *bytes, size_t length);
int ln_crash_decode(struct ln_crash_record *record, const uint8_t *bytes, size_t length);

#endif
