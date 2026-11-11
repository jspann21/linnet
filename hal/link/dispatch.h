#ifndef LINNET_LINK_DISPATCH_H
#define LINNET_LINK_DISPATCH_H

#include "link.h"

#define LINNET_LINK_MAX_IMAGE (16u * 1024u * 1024u)

typedef struct {
    void *context;
    /* Callbacks are bounded service steps. Return 1 on success, 0 on failure.
     * Stage ownership/address comes from the board memory plan, never the wire. */
    int (*begin)(void *context, uint32_t length);
    int (*write)(void *context, uint32_t offset, const uint8_t *data, uint32_t length);
    /* Must validate SHA256, image container, target, load bounds and entry. */
    int (*verify)(void *context, uint32_t length, const uint8_t sha256[32]);
    int (*read)(void *context, uint16_t command, uint8_t *output, uint32_t *length);
    int (*clear_crash)(void *context);
    int (*quiesce)(void *context);
    int (*chainload)(void *context);
    int (*reboot)(void *context, uint16_t command);
    /* Must reject unsupported transition modes before an acceptance ACK. */
    int (*can_action)(void *context, uint16_t command);
    int (*crash)(void *context);
} ln_link_backend;

typedef struct {
    ln_link_backend backend;
    uint32_t size;
    uint32_t offset;
    uint32_t expected_crc;
    uint32_t crc;
    uint8_t sha256[32];
    uint8_t staging;
    uint8_t complete;
    uint8_t verified;
    uint16_t deferred;
    uint32_t deferred_sequence;
    uint8_t reply[LINNET_LINK_MAX_PAYLOAD];
} ln_link_dispatcher;

void ln_link_dispatch_init(ln_link_dispatcher *state, const ln_link_backend *backend);
/* Invoke only after parser sequence/CRC acceptance; response borrows state. */
int ln_link_dispatch(ln_link_dispatcher *state, const ln_link_frame *request,
                     ln_link_frame *response);
/* Invoke only after the successful action ACK is fully transmitted. Never
 * invoke following a short write/disconnect. Failed action is not replayed. */
int ln_link_dispatch_ack_sent(ln_link_dispatcher *state, uint32_t sequence);

#endif
