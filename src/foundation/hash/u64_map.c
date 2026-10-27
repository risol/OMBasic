#include "ombasic/foundation/u64_map.h"

#include "ombasic/foundation/memory.h"

static uint64_t hash_u64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static size_t load_limit(size_t capacity)
{
    return capacity - capacity / 4;
}

static size_t next_power_of_two(size_t minimum)
{
    size_t result = 8;
    while (result < minimum) {
        if (result > SIZE_MAX / 2) {
            return 0;
        }
        result *= 2;
    }
    return result;
}

static size_t find_slot(const Om_U64_Map *map, uint64_t key, bool *found)
{
    const size_t mask = map->capacity - 1;
    size_t index = (size_t)hash_u64(key) & mask;
    for (size_t probes = 0; probes < map->capacity; ++probes) {
        if (map->occupied[index] == 0) {
            *found = false;
            return index;
        }
        if (map->keys[index] == key) {
            *found = true;
            return index;
        }
        index = (index + 1) & mask;
    }
    *found = false;
    return map->capacity;
}

void om_u64_map_init(Om_U64_Map *map)
{
    if (map != NULL) {
        *map = (Om_U64_Map){0};
    }
}

Om_Status om_u64_map_reserve(Om_U64_Map *map, size_t entry_capacity)
{
    uint64_t *new_keys = NULL;
    uint64_t *new_values = NULL;
    unsigned char *new_occupied = NULL;
    size_t new_capacity = 0;
    size_t minimum_capacity = 0;
    Om_U64_Map replacement = {0};

    if (map == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (entry_capacity <= load_limit(map->capacity)) {
        return OM_STATUS_OK;
    }
    if (!om_checked_size_add(entry_capacity, entry_capacity / 2, &minimum_capacity)
        || !om_checked_size_add(minimum_capacity, 1, &minimum_capacity)) {
        return OM_STATUS_OVERFLOW;
    }
    new_capacity = next_power_of_two(minimum_capacity);
    if (new_capacity == 0) {
        return OM_STATUS_OVERFLOW;
    }
    new_keys = om_alloc_array(new_capacity, sizeof(*new_keys));
    new_values = om_alloc_array(new_capacity, sizeof(*new_values));
    new_occupied = om_alloc_array(new_capacity, sizeof(*new_occupied));
    if (new_keys == NULL || new_values == NULL || new_occupied == NULL) {
        om_free(new_keys);
        om_free(new_values);
        om_free(new_occupied);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < new_capacity; ++index) {
        new_occupied[index] = 0;
    }

    replacement.keys = new_keys;
    replacement.values = new_values;
    replacement.occupied = new_occupied;
    replacement.capacity = new_capacity;
    for (size_t index = 0; index < map->capacity; ++index) {
        if (map->occupied[index] != 0) {
            bool found = false;
            const size_t slot = find_slot(&replacement, map->keys[index], &found);
            if (slot >= replacement.capacity || found) {
                om_free(new_keys);
                om_free(new_values);
                om_free(new_occupied);
                return OM_STATUS_OVERFLOW;
            }
            replacement.keys[slot] = map->keys[index];
            replacement.values[slot] = map->values[index];
            replacement.occupied[slot] = 1;
            replacement.length += 1;
        }
    }

    om_free(map->keys);
    om_free(map->values);
    om_free(map->occupied);
    *map = replacement;
    return OM_STATUS_OK;
}

Om_Status om_u64_map_put(Om_U64_Map *map, uint64_t key, uint64_t value)
{
    size_t required = 0;
    size_t slot = 0;
    bool found = false;
    Om_Status status = OM_STATUS_OK;

    if (map == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (map->capacity == 0 || map->length >= load_limit(map->capacity)) {
        if (!om_checked_size_add(map->length, 1, &required)) {
            return OM_STATUS_OVERFLOW;
        }
        status = om_u64_map_reserve(map, required);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    slot = find_slot(map, key, &found);
    if (slot >= map->capacity) {
        return OM_STATUS_OVERFLOW;
    }
    if (!found) {
        map->keys[slot] = key;
        map->occupied[slot] = 1;
        map->length += 1;
    }
    map->values[slot] = value;
    return OM_STATUS_OK;
}

bool om_u64_map_get(const Om_U64_Map *map, uint64_t key, uint64_t *out_value)
{
    bool found = false;
    size_t slot = 0;
    if (map == NULL || out_value == NULL || map->capacity == 0) {
        return false;
    }
    slot = find_slot(map, key, &found);
    if (!found) {
        return false;
    }
    *out_value = map->values[slot];
    return true;
}

void om_u64_map_destroy(Om_U64_Map *map)
{
    if (map == NULL) {
        return;
    }
    om_free(map->keys);
    om_free(map->values);
    om_free(map->occupied);
    *map = (Om_U64_Map){0};
}
