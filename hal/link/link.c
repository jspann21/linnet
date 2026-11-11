#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "link.h"

static int valid_header(const uint8_t *p) {
    uint16_t flags = ln_read_le16(p + 6);
    return p[0] == 'L' && p[1] == 'I' && p[2] == 'N' && p[3] == 'K' &&
           p[4] == 0 && p[5] == 0 && (flags == 0 || flags == 1 || flags == 3) &&
           ln_read_le16(p + 10) == 0 && ln_read_le32(p + 12) != 0 &&
           ln_read_le32(p + 16) <= LINNET_LINK_MAX_PAYLOAD;
}

void ln_link_parser_init(ln_link_parser *parser) {
    parser->used = 0;
    parser->expected = LINNET_LINK_HEADER_SIZE;
    parser->last_sequence = 0;
}

int ln_link_parser_feed(ln_link_parser *parser, uint8_t byte, ln_link_frame *frame) {
    if (parser->used >= LINNET_LINK_MAX_FRAME) goto invalid;
    parser->bytes[parser->used++] = byte;
    if (parser->used == LINNET_LINK_HEADER_SIZE) {
        if (!valid_header(parser->bytes)) goto invalid;
        parser->expected = LINNET_LINK_HEADER_SIZE + ln_read_le32(parser->bytes + 16) + 4u;
    }
    if (parser->used < parser->expected) return 0;
    if (ln_crc32(parser->bytes, parser->used - 4u) != ln_read_le32(parser->bytes + parser->used - 4u))
        goto invalid;
    frame->sequence = ln_read_le32(parser->bytes + 12);
    if (frame->sequence <= parser->last_sequence) goto invalid;
    parser->last_sequence = frame->sequence;
    frame->command = ln_read_le16(parser->bytes + 8);
    frame->flags = ln_read_le16(parser->bytes + 6);
    frame->length = ln_read_le32(parser->bytes + 16);
    frame->payload = parser->bytes + LINNET_LINK_HEADER_SIZE;
    parser->used = 0;
    parser->expected = LINNET_LINK_HEADER_SIZE;
    return 1;
invalid:
    parser->used = 0;
    parser->expected = LINNET_LINK_HEADER_SIZE;
    return -1;
}

size_t ln_link_encode(const ln_link_frame *frame, uint8_t *output, size_t capacity) {
    if (frame->length > LINNET_LINK_MAX_PAYLOAD || frame->sequence == 0 ||
        (frame->flags != 0 && frame->flags != 1 && frame->flags != 3) ||
        (frame->length != 0 && frame->payload == NULL)) return 0;
    size_t total = LINNET_LINK_HEADER_SIZE + frame->length + 4u;
    if (capacity < total) return 0;
    output[0] = 'L'; output[1] = 'I'; output[2] = 'N'; output[3] = 'K';
    output[4] = 0; output[5] = 0;
    ln_write_le16(output + 6, frame->flags);
    ln_write_le16(output + 8, frame->command);
    ln_write_le16(output + 10, 0);
    ln_write_le32(output + 12, frame->sequence);
    ln_write_le32(output + 16, frame->length);
    for (uint32_t i = 0; i < frame->length; ++i) output[20u + i] = frame->payload[i];
    ln_write_le32(output + total - 4u, ln_crc32(output, (uint32_t)total - 4u));
    return total;
}
