#include "test.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ombasic/foundation/arena.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/u64_map.h"
#include "ombasic/foundation/utf8.h"
#include "ombasic/foundation/vector.h"

bool om_test_memory(void)
{
    size_t result = 0;
    int *items = NULL;

    OM_TEST_ASSERT(om_checked_size_add(4, 9, &result));
    OM_TEST_ASSERT(result == 13);
    OM_TEST_ASSERT(!om_checked_size_add(SIZE_MAX, 1, &result));
    OM_TEST_ASSERT(result == 0);
    OM_TEST_ASSERT(om_checked_size_mul(7, 8, &result));
    OM_TEST_ASSERT(result == 56);
    OM_TEST_ASSERT(!om_checked_size_mul(SIZE_MAX, 2, &result));
    OM_TEST_ASSERT(result == 0);
    OM_TEST_ASSERT(!om_checked_size_add(1, 2, NULL));
    OM_TEST_ASSERT(om_alloc(0) == NULL);
    OM_TEST_ASSERT(om_alloc_array(SIZE_MAX, 2) == NULL);
    items = om_alloc_array(4, sizeof(*items));
    OM_TEST_ASSERT(items != NULL);
    items[0] = 42;
    OM_TEST_ASSERT(items[0] == 42);
    OM_TEST_ASSERT(om_realloc_array(items, SIZE_MAX, 2) == NULL);
    OM_TEST_ASSERT(items[0] == 42);
    om_free(items);
    return true;
}

bool om_test_string_view(void)
{
    const Om_String_View hello = {"hello", 5};
    const Om_String_View same = {"hello", 5};
    const Om_String_View other = {"world", 5};
    const Om_String_View empty = {NULL, 0};
    const Om_String_View invalid = {NULL, 1};

    OM_TEST_ASSERT(om_string_view_is_valid(hello));
    OM_TEST_ASSERT(om_string_view_equal(hello, same));
    OM_TEST_ASSERT(!om_string_view_equal(hello, other));
    OM_TEST_ASSERT(om_string_view_equal(empty, (Om_String_View){"", 0}));
    OM_TEST_ASSERT(!om_string_view_is_valid(invalid));
    return true;
}

bool om_test_arena(void)
{
    Om_Arena arena = {0};
    void *block = NULL;
    const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};

    OM_TEST_ASSERT(om_arena_init(&arena, 32) == OM_STATUS_OK);
    for (size_t index = 0; index < sizeof(alignments) / sizeof(alignments[0]); ++index) {
        const size_t alignment = alignments[index];
        OM_TEST_ASSERT(om_arena_alloc(&arena, 7, alignment, &block) == OM_STATUS_OK);
        OM_TEST_ASSERT(block != NULL);
        OM_TEST_ASSERT(((uintptr_t)block & (alignment - 1)) == 0);
        (void)memset(block, (int)index, 7);
    }
    OM_TEST_ASSERT(arena.total_capacity > 32);
    OM_TEST_ASSERT(om_arena_alloc(&arena, 1, 3, &block) == OM_STATUS_INVALID_ARGUMENT);
    OM_TEST_ASSERT(block == NULL);
    OM_TEST_ASSERT(om_arena_alloc(&arena, 0, 1, &block) == OM_STATUS_INVALID_ARGUMENT);
    OM_TEST_ASSERT(om_arena_alloc(&arena, SIZE_MAX, 1, &block) == OM_STATUS_OVERFLOW);
    OM_TEST_ASSERT(om_arena_reset(&arena) == OM_STATUS_OK);
    OM_TEST_ASSERT(arena.total_capacity > 0);
    OM_TEST_ASSERT(om_arena_alloc(&arena, 16, 16, &block) == OM_STATUS_OK);
    om_arena_destroy(&arena);
    OM_TEST_ASSERT(arena.first == NULL && arena.current == NULL);
    return true;
}

bool om_test_vector(void)
{
    Om_Vector vector = {0};
    Om_Status status = OM_STATUS_OK;
    uint32_t random_state = UINT32_C(0x6d2b79f5);
    uint64_t expected[4096];
    size_t expected_length = 0;

    OM_TEST_ASSERT(om_vector_init(&vector, sizeof(uint64_t)) == OM_STATUS_OK);
    for (uint64_t index = 0; index < 1000; ++index) {
        status = om_vector_push(&vector, &index);
        OM_TEST_ASSERT(status == OM_STATUS_OK);
    }
    OM_TEST_ASSERT(vector.length == 1000);
    OM_TEST_ASSERT(*(const uint64_t *)om_vector_at_const(&vector, 0) == 0);
    OM_TEST_ASSERT(*(const uint64_t *)om_vector_at_const(&vector, 999) == 999);
    OM_TEST_ASSERT(om_vector_at(&vector, vector.length) == NULL);
    OM_TEST_ASSERT(om_vector_reserve(&vector, SIZE_MAX) == OM_STATUS_OVERFLOW);
    OM_TEST_ASSERT(vector.length == 1000);
    om_vector_clear(&vector);
    OM_TEST_ASSERT(vector.length == 0);

    for (size_t iteration = 0; iteration < sizeof(expected) / sizeof(expected[0]); ++iteration) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        if ((random_state & UINT32_C(15)) == 0) {
            om_vector_clear(&vector);
            expected_length = 0;
        } else {
            const uint64_t value = (uint64_t)random_state * UINT64_C(0x9e3779b1)
                                   + (uint64_t)iteration;
            OM_TEST_ASSERT(om_vector_push(&vector, &value) == OM_STATUS_OK);
            expected[expected_length++] = value;
        }
        if ((random_state & UINT32_C(7)) == 0) {
            const size_t extra_capacity = (size_t)(random_state & UINT32_C(31));
            OM_TEST_ASSERT(om_vector_reserve(&vector, expected_length + extra_capacity)
                           == OM_STATUS_OK);
        }
        OM_TEST_ASSERT(vector.length == expected_length);
        for (size_t index = 0; index < expected_length; ++index) {
            const uint64_t *item = om_vector_at_const(&vector, index);
            OM_TEST_ASSERT(item != NULL && *item == expected[index]);
        }
    }
    om_vector_destroy(&vector);
    OM_TEST_ASSERT(vector.data == NULL && vector.item_size == 0);
    OM_TEST_ASSERT(om_vector_init(&vector, 0) == OM_STATUS_INVALID_ARGUMENT);
    return true;
}

bool om_test_utf8(void)
{
    const uint8_t valid_bytes[] = {
        UINT8_C(0x41), UINT8_C(0xc2), UINT8_C(0xa2),
        UINT8_C(0xf0), UINT8_C(0x9f), UINT8_C(0x8c), UINT8_C(0x9f)
    };
    const uint8_t overlong[] = {UINT8_C(0xc0), UINT8_C(0xaf)};
    const uint8_t surrogate[] = {UINT8_C(0xed), UINT8_C(0xa0), UINT8_C(0x80)};
    const uint8_t too_large[] = {
        UINT8_C(0xf4), UINT8_C(0x90), UINT8_C(0x80), UINT8_C(0x80)
    };
    const uint8_t truncated[] = {UINT8_C(0xe2), UINT8_C(0x82)};
    const uint8_t isolated[] = {UINT8_C(0x80)};
    const Om_Const_Byte_Slice valid = {valid_bytes, sizeof(valid_bytes)};
    size_t offset = 0;
    uint32_t codepoint = 0;

    OM_TEST_ASSERT(om_utf8_validate(valid) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){NULL, 0}) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_utf8_decode_next(valid, &offset, &codepoint) == OM_STATUS_OK);
    OM_TEST_ASSERT(codepoint == UINT32_C(0x41) && offset == 1);
    OM_TEST_ASSERT(om_utf8_decode_next(valid, &offset, &codepoint) == OM_STATUS_OK);
    OM_TEST_ASSERT(codepoint == UINT32_C(0xa2) && offset == 3);
    OM_TEST_ASSERT(om_utf8_decode_next(valid, &offset, &codepoint) == OM_STATUS_OK);
    OM_TEST_ASSERT(codepoint == UINT32_C(0x1f31f) && offset == sizeof(valid_bytes));
    OM_TEST_ASSERT(om_utf8_decode_next(valid, &offset, &codepoint) == OM_STATUS_END_OF_INPUT);
    offset += 1;
    OM_TEST_ASSERT(om_utf8_decode_next(valid, &offset, &codepoint) == OM_STATUS_OUT_OF_RANGE);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){overlong, sizeof(overlong)})
                   == OM_STATUS_INVALID_UTF8);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){surrogate, sizeof(surrogate)})
                   == OM_STATUS_INVALID_UTF8);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){too_large, sizeof(too_large)})
                   == OM_STATUS_INVALID_UTF8);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){truncated, sizeof(truncated)})
                   == OM_STATUS_INVALID_UTF8);
    OM_TEST_ASSERT(om_utf8_validate((Om_Const_Byte_Slice){isolated, sizeof(isolated)})
                   == OM_STATUS_INVALID_UTF8);
    return true;
}

bool om_test_u64_map(void)
{
    Om_U64_Map map = {0};
    uint64_t value = 0;
    OM_TEST_ASSERT(om_u64_map_put(&map, 0, 17) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_u64_map_put(&map, UINT64_MAX, 23) == OM_STATUS_OK);
    for (uint64_t key = 1; key <= 1000; ++key) {
        OM_TEST_ASSERT(om_u64_map_put(&map, key, key * key + 3) == OM_STATUS_OK);
    }
    OM_TEST_ASSERT(map.length == 1002);
    OM_TEST_ASSERT(om_u64_map_get(&map, 0, &value) && value == 17);
    OM_TEST_ASSERT(om_u64_map_get(&map, UINT64_MAX, &value) && value == 23);
    for (uint64_t key = 1; key <= 1000; ++key) {
        OM_TEST_ASSERT(om_u64_map_get(&map, key, &value));
        OM_TEST_ASSERT(value == key * key + 3);
    }
    OM_TEST_ASSERT(!om_u64_map_get(&map, 1001, &value));
    OM_TEST_ASSERT(om_u64_map_put(&map, 10, 777) == OM_STATUS_OK);
    OM_TEST_ASSERT(om_u64_map_get(&map, 10, &value) && value == 777);
    OM_TEST_ASSERT(om_u64_map_reserve(&map, SIZE_MAX) == OM_STATUS_OVERFLOW);
    OM_TEST_ASSERT(om_u64_map_get(&map, 10, &value) && value == 777);
    om_u64_map_destroy(&map);
    OM_TEST_ASSERT(map.capacity == 0 && map.length == 0);
    return true;
}
