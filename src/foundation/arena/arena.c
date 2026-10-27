#include "ombasic/foundation/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

_Static_assert(sizeof(uintptr_t) >= sizeof(size_t),
               "Arena requires uintptr_t to represent size_t offsets.");

struct Om_Arena_Segment {
    size_t capacity;
    size_t used;
    struct Om_Arena_Segment *next;
    max_align_t payload[];
};

static unsigned char *segment_bytes(Om_Arena_Segment *segment)
{
    return (unsigned char *)segment->payload;
}

static Om_Status segment_create(size_t capacity, Om_Arena_Segment **out)
{
    size_t rounded = 0;
    size_t total = 0;
    size_t unit = sizeof(max_align_t);
    Om_Arena_Segment *segment = NULL;

    if (out == NULL || unit == 0 || capacity == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (capacity % unit != 0) {
        const size_t remainder = capacity % unit;
        if (!om_checked_size_add(capacity, unit - remainder, &rounded)) {
            return OM_STATUS_OVERFLOW;
        }
    } else {
        rounded = capacity;
    }
    if (!om_checked_size_add(offsetof(Om_Arena_Segment, payload), rounded, &total)) {
        return OM_STATUS_OVERFLOW;
    }
    segment = om_alloc(total);
    if (segment == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    segment->capacity = rounded;
    segment->used = 0;
    segment->next = NULL;
    *out = segment;
    return OM_STATUS_OK;
}

static bool segment_try_alloc(
    Om_Arena_Segment *segment,
    size_t size,
    size_t alignment,
    void **out
)
{
    uintptr_t base_address = 0;
    uintptr_t current_address = 0;
    size_t mask = 0;
    size_t misalignment = 0;
    size_t padding = 0;
    size_t offset = 0;
    size_t end = 0;

    base_address = (uintptr_t)(void *)segment_bytes(segment);
    if (segment->used > UINTPTR_MAX - base_address) {
        return false;
    }
    current_address = base_address + segment->used;
    mask = alignment - 1;
    misalignment = (size_t)(current_address & (uintptr_t)mask);
    padding = misalignment == 0 ? 0 : alignment - misalignment;
    if (!om_checked_size_add(segment->used, padding, &offset)
        || !om_checked_size_add(offset, size, &end)
        || end > segment->capacity) {
        return false;
    }
    *out = segment_bytes(segment) + offset;
    segment->used = end;
    return true;
}

Om_Status om_arena_init(Om_Arena *arena, size_t default_segment_size)
{
    if (arena == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *arena = (Om_Arena){0};
    arena->default_segment_size = default_segment_size == 0 ? 4096 : default_segment_size;
    return OM_STATUS_OK;
}

Om_Status om_arena_alloc(Om_Arena *arena, size_t size, size_t alignment, void **out)
{
    Om_Arena_Segment *segment = NULL;
    size_t minimum_capacity = 0;
    size_t capacity = 0;
    Om_Status status = OM_STATUS_OK;

    if (out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (arena == NULL || size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (arena->default_segment_size == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    segment = arena->current;
    if (segment != NULL && segment_try_alloc(segment, size, alignment, out)) {
        return OM_STATUS_OK;
    }

    if (!om_checked_size_add(size, alignment - 1, &minimum_capacity)) {
        return OM_STATUS_OVERFLOW;
    }
    capacity = arena->default_segment_size > minimum_capacity
        ? arena->default_segment_size
        : minimum_capacity;
    status = segment_create(capacity, &segment);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (!segment_try_alloc(segment, size, alignment, out)) {
        om_free(segment);
        return OM_STATUS_OVERFLOW;
    }
    if (!om_checked_size_add(arena->total_capacity, segment->capacity, &capacity)) {
        om_free(segment);
        *out = NULL;
        return OM_STATUS_OVERFLOW;
    }
    if (arena->current != NULL) {
        arena->current->next = segment;
    } else {
        arena->first = segment;
    }
    arena->current = segment;
    arena->total_capacity = capacity;
    return OM_STATUS_OK;
}

Om_Status om_arena_reset(Om_Arena *arena)
{
    Om_Arena_Segment *segment = NULL;
    Om_Arena_Segment *next = NULL;
    if (arena == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    segment = arena->first;
    if (segment == NULL) {
        arena->current = NULL;
        arena->total_capacity = 0;
        return OM_STATUS_OK;
    }
    next = segment->next;
#if defined(OM_ARENA_DEBUG_POISON)
    if (segment->used > 0) {
        memset(segment_bytes(segment), 0xDD, segment->used);
    }
#endif
    segment->used = 0;
    segment->next = NULL;
    while (next != NULL) {
        Om_Arena_Segment *following = next->next;
#if defined(OM_ARENA_DEBUG_POISON)
        if (next->used > 0) {
            memset(segment_bytes(next), 0xDD, next->used);
        }
#endif
        om_free(next);
        next = following;
    }
    arena->current = segment;
    arena->total_capacity = segment->capacity;
    return OM_STATUS_OK;
}

void om_arena_destroy(Om_Arena *arena)
{
    Om_Arena_Segment *segment = NULL;
    if (arena == NULL) {
        return;
    }
    segment = arena->first;
    while (segment != NULL) {
        Om_Arena_Segment *next = segment->next;
        om_free(segment);
        segment = next;
    }
    *arena = (Om_Arena){0};
}
