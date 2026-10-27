#ifndef OMBASIC_FOUNDATION_U64_MAP_H
#define OMBASIC_FOUNDATION_U64_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"

typedef struct Om_U64_Map {
    uint64_t *keys;
    uint64_t *values;
    unsigned char *occupied;
    size_t length;
    size_t capacity;
} Om_U64_Map;

/* The map owns its buffers and stores uint64_t keys and values by value. */
void om_u64_map_init(Om_U64_Map *map);
Om_Status om_u64_map_reserve(Om_U64_Map *map, size_t entry_capacity);
Om_Status om_u64_map_put(Om_U64_Map *map, uint64_t key, uint64_t value);
bool om_u64_map_get(const Om_U64_Map *map, uint64_t key, uint64_t *out_value);
void om_u64_map_destroy(Om_U64_Map *map);

#endif
