#include "boot/include/linnet_bytes.h"
#include "boot/include/linnet_boot.h"
#include "dispatch.h"

void ln_link_dispatch_init(ln_link_dispatcher *state, const ln_link_backend *backend) {
    state->backend = *backend;
    state->size = 0;
    state->offset = 0;
    state->expected_crc = 0;
    state->crc = 0xffffffffu;
    state->staging = 0;
    state->complete = 0;
    state->verified = 0;
    state->deferred = 0;
    state->deferred_sequence = 0;
}

int ln_link_dispatch(ln_link_dispatcher *state, const ln_link_frame *request,
                     ln_link_frame *response) {
    if (request->flags != 0 || request->length > LINNET_LINK_MAX_PAYLOAD ||
        (request->length != 0 && request->payload == NULL)) return 0;
    response->command = request->command;
    response->sequence = request->sequence;
    response->flags = LINNET_LINK_ACK | LINNET_LINK_ERROR;
    response->payload = state->reply;
    response->length = 2;
    state->reply[0] = 1; /* Rejected state, arguments, integrity or backend. */
    state->reply[1] = 0;
    if (state->deferred) return 1;
    switch (request->command) {
    case 1: case 2: case 3: case 48: case 49: case 50: case 53: case 54: {
        uint32_t length = LINNET_LINK_MAX_PAYLOAD;
        if (request->length || !state->backend.read ||
            !state->backend.read(state->backend.context, request->command, state->reply, &length) ||
            length > LINNET_LINK_MAX_PAYLOAD) return 1;
        response->length = length;
        break;
    }
    case 16: {
        if (request->length != 40 || !state->backend.begin) return 1;
        uint32_t size = ln_read_le32(request->payload);
        if (!size || size > LINNET_LINK_MAX_IMAGE) return 1;
        /* Invalidating previous verification precedes any backend mutation. */
        state->staging = state->complete = state->verified = 0;
        if (!state->backend.begin(state->backend.context, size)) return 1;
        state->size = size;
        state->expected_crc = ln_read_le32(request->payload + 4);
        for (unsigned i = 0; i < 32; ++i) state->sha256[i] = request->payload[8u + i];
        state->offset = 0;
        state->crc = 0xffffffffu;
        state->staging = 1;
        ln_write_le32(state->reply, 0);
        response->length = 4;
        break;
    }
    case 17: {
        if (!state->staging || state->complete || request->length <= 4 || !state->backend.write)
            return 1;
        uint32_t size = request->length - 4u;
        if (ln_read_le32(request->payload) != state->offset || size > state->size - state->offset)
            return 1;
        if (!state->backend.write(state->backend.context, state->offset, request->payload + 4, size)) {
            state->staging = 0; /* A failed write may be partial: never resume it. */
            return 1;
        }
        state->crc = ln_crc32_update(state->crc, request->payload + 4, size);
        state->offset += size;
        ln_write_le32(state->reply, state->offset);
        response->length = 4;
        break;
    }
    case 18:
        if (request->length || !state->staging || state->complete || state->offset != state->size)
            return 1;
        if (~state->crc != state->expected_crc || !state->backend.verify ||
            !state->backend.verify(state->backend.context, state->size, state->sha256)) {
            state->staging = state->verified = 0;
            return 1;
        }
        state->complete = 1;
        ln_write_le32(state->reply, state->offset);
        response->length = 4;
        break;
    case 19:
        if (request->length || !state->complete || !state->backend.verify) return 1;
        state->verified = 0;
        if (!state->backend.verify(state->backend.context, state->size, state->sha256)) return 1;
        state->verified = 1;
        for (unsigned i = 0; i < 32; ++i) state->reply[i] = state->sha256[i];
        response->length = 32;
        break;
    case 20: case 32: case 33: case 34: case 52:
        if (request->command == 52) {
            if (request->length != 4 || ln_read_le32(request->payload) != 0 || !state->backend.crash) return 1;
        } else if (request->length) return 1;
        if (!state->backend.quiesce || !state->backend.can_action ||
            !state->backend.can_action(state->backend.context, request->command)) return 1;
        if (request->command == 20 && (!state->verified || !state->backend.chainload)) return 1;
        if (request->command != 20 && request->command != 52 && !state->backend.reboot) return 1;
        state->deferred = request->command;
        state->deferred_sequence = request->sequence;
        response->length = 0;
        break;
    case 51:
        if (request->length || !state->backend.clear_crash ||
            !state->backend.clear_crash(state->backend.context)) return 1;
        response->length = 0;
        break;
    default:
        state->reply[0] = 2; /* Unsupported command. */
        return 1;
    }
    response->flags = LINNET_LINK_ACK;
    return 1;
}

int ln_link_dispatch_ack_sent(ln_link_dispatcher *state, uint32_t sequence) {
    if (!state->deferred || sequence != state->deferred_sequence) return 0;
    uint16_t action = state->deferred;
    state->deferred = 0;
    state->deferred_sequence = 0;
    state->verified = 0;
    if (!state->backend.quiesce(state->backend.context)) return 0;
    if (action == 20) return state->backend.chainload(state->backend.context);
    if (action == 52) return state->backend.crash(state->backend.context);
    return state->backend.reboot(state->backend.context, action);
}
