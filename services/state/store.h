#ifndef LINNET_STATE_STORE_H
#define LINNET_STATE_STORE_H
#include <stdint.h>
#define LN_STATE_RECORD_SIZE 512u
#define LN_STATE_PAYLOAD_MAX 480u
/* Callbacks transfer exactly one record; caller owns file placement and deadlines.
 * Flush must durably order completed writes. No automatic mutation retries. */
struct ln_state_io {
    void *context;
    int (*read)(void *, uint32_t copy, uint8_t record[512]);
    int (*write)(void *, uint32_t copy, const uint8_t record[512]);
    int (*flush)(void *);
};
struct ln_state_store {
    struct ln_state_io io;
    uint32_t generation, length, active;
    uint8_t payload[LN_STATE_PAYLOAD_MAX];
    uint8_t ready, poisoned;
};
/* 1 loaded, 0 both records blank, -1 corruption/IO/conflict. */
int ln_state_open(struct ln_state_store *, const struct ln_state_io *);
/* 1 committed; 0 rejected/ambiguous. Failure after write requires reopen. */
int ln_state_publish(struct ln_state_store *, const uint8_t *, uint32_t length);
int ln_state_encode(uint8_t record[512], uint32_t generation, const uint8_t *, uint32_t length);
int ln_state_decode(const uint8_t record[512], uint32_t *generation, uint32_t *length);
#endif
