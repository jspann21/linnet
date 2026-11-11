#ifndef LINNET_LOADER_H
#define LINNET_LOADER_H
#include "linnet_boot.h"
#include "sha256.h"
#include "hal/link/dispatch.h"
#define LINNET_LOADER_IDENTITY_SIZE 96u
#define LINNET_LOADER_CAP_UPLOAD 1u
#define LINNET_LOADER_CAP_RAM_BOOT 2u
#define LINNET_LOADER_CAP_CRASH 4u
#define LINNET_LOADER_CAP_REBOOT_OS 8u
#define LINNET_LOADER_CAP_REBOOT_BOOT 16u
#define LINNET_LOADER_CAP_REBOOT_DISK 32u
struct ln_loader_actions {
    void *context;
    void (*progress)(void *context); /* Called only after actual bounded hash work. */
    int (*quiesce)(void *context);
    int (*chainload)(void *context, const uint8_t *payload, uint32_t size,
        uint32_t load, uint32_t entry, const uint8_t build_id[32],
        const uint8_t image_sha256[32], uint32_t generation);
    int (*reboot)(void *context, uint16_t command);
    int (*can_reboot)(void *context, uint16_t command);
    int (*crash)(void *context);
};
struct ln_loader_config {
    uint8_t *stage;
    uint32_t capacity;
    struct ln_image_policy policy;
    volatile uint8_t *crash_mailbox; /* Exactly LINNET_CRASH_MAILBOX_SIZE reserved bytes. */
    uint32_t profile, image_kind, health_flags, generation, capabilities;
    uint8_t build_id[32], image_sha256[32];
    struct ln_loader_actions actions;
};
struct ln_loader {
    struct ln_loader_config config;
    struct ln_image_info candidate;
    uint32_t size, offset;
    uint32_t metrics[8];
    uint8_t verified;
    uint8_t candidate_sha256[32];
};
int ln_loader_init(struct ln_loader *loader, const struct ln_loader_config *config,
    ln_link_backend *backend);
void ln_loader_set_health(struct ln_loader *loader, uint32_t flags);
#endif
