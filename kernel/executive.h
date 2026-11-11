#ifndef LINNET_EXECUTIVE_H
#define LINNET_EXECUTIVE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LINNET_EVENT_CAPACITY 64u
#define LINNET_SERVICE_COUNT 5u
#define LINNET_DEADLINE_CAPACITY 16u
enum { LN_SERVICE_INPUT, LN_SERVICE_POWER, LN_SERVICE_DISPLAY, LN_SERVICE_SHELL, LN_SERVICE_LINK };
typedef struct { uint32_t kind, time, generation; int32_t value; } ln_event;
typedef struct {
    ln_event event;
    uint16_t next;
} ln_event_slot;
typedef struct { ln_event event; uint32_t due; uint16_t key; uint8_t owner, active; } ln_deadline;
typedef struct {
    ln_event_slot slots[LINNET_EVENT_CAPACITY];
    uint16_t head[LINNET_SERVICE_COUNT], tail[LINNET_SERVICE_COUNT], free_head;
    uint32_t used, high, accepted, processed, dropped, max_latency;
#if !defined(LINNET_PROFILE_RELEASE)
    uint32_t service_accepted[LINNET_SERVICE_COUNT],service_processed[LINNET_SERVICE_COUNT];
    uint32_t service_dropped[LINNET_SERVICE_COUNT],service_max_latency[LINNET_SERVICE_COUNT];
#endif
    ln_deadline deadlines[LINNET_DEADLINE_CAPACITY];
    uint32_t deadline_used, deadline_high;
} ln_executive;
typedef struct { uint8_t *bytes; uint32_t capacity, used, high, generation; } ln_arena;
typedef struct { uint32_t offset, size, generation; } ln_arena_ref;
void ln_exec_init(ln_executive *state);
bool ln_exec_post(ln_executive *state, uint32_t owner, ln_event event);
bool ln_exec_take(ln_executive *state, uint32_t now, uint32_t *owner, ln_event *event);
#if !defined(LINNET_PROFILE_RELEASE)
void ln_exec_diagnostics(const ln_executive *state,uint32_t words[24]);
#endif
bool ln_exec_schedule(ln_executive *state, uint32_t owner, uint16_t key, uint32_t due, ln_event event);
void ln_exec_cancel(ln_executive *state, uint32_t owner, uint16_t key);
void ln_exec_due(ln_executive *state, uint32_t now);
uint32_t ln_exec_next(const ln_executive *state, uint32_t now, uint32_t fallback);
void ln_arena_init(ln_arena *arena, void *memory, uint32_t size);
void ln_arena_reset(ln_arena *arena);
bool ln_arena_alloc(ln_arena *arena, uint32_t size, uint32_t alignment, ln_arena_ref *ref);
void *ln_arena_get(ln_arena *arena, ln_arena_ref ref);
#endif
