#ifndef OMBASIC_FOUNDATION_ARENA_H
#define OMBASIC_FOUNDATION_ARENA_H

#include <stddef.h>

#include "ombasic/foundation/status.h"

typedef struct Om_Arena_Segment Om_Arena_Segment;

typedef struct Om_Arena {
    Om_Arena_Segment *first;
    Om_Arena_Segment *current;
    size_t default_segment_size;
    size_t total_capacity;
} Om_Arena;

/* The arena owns its segments; allocations remain valid until reset or destroy. */
Om_Status om_arena_init(Om_Arena *arena, size_t default_segment_size);
/* Size is nonzero and alignment is a nonzero power of two. */
Om_Status om_arena_alloc(
    Om_Arena *arena,
    size_t size,
    size_t alignment,
    void **out
);
Om_Status om_arena_reset(Om_Arena *arena);
void om_arena_destroy(Om_Arena *arena);

#endif
