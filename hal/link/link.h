#ifndef LINNET_LINK_H
#define LINNET_LINK_H

#include <stddef.h>
#include <stdint.h>

#define LINNET_LINK_MAX_PAYLOAD 1024u
#define LINNET_LINK_HEADER_SIZE 20u
#define LINNET_LINK_MAX_FRAME 1048u
#define LINNET_LINK_ACK 1u
#define LINNET_LINK_ERROR 2u

typedef struct {
    uint8_t bytes[LINNET_LINK_MAX_FRAME];
    uint32_t used;
    uint32_t expected;
    uint32_t last_sequence;
} ln_link_parser;

typedef struct {
    uint16_t command;
    uint16_t flags;
    uint32_t sequence;
    uint32_t length;
    const uint8_t *payload;
} ln_link_frame;

void ln_link_parser_init(ln_link_parser *parser);
/* One byte of bounded work; 1=frame ready, 0=incomplete, -1=invalid.
 * Payload borrows parser memory only until the next feed. Sequence acceptance
 * happens before dispatch; an invalid frame resets framing, never replay state. */
int ln_link_parser_feed(ln_link_parser *parser, uint8_t byte, ln_link_frame *frame);
size_t ln_link_encode(const ln_link_frame *frame, uint8_t *output, size_t capacity);

#endif
