#include "runtime_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/runtime/abi/json.h"
#include "ombasic/runtime/abi/text.h"

#define RUNTIME_ASSERT(expression)                                               \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",           \
                          __FILE__, __LINE__, #expression);                      \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

typedef struct Test_Record_Layout {
    void *label;
    int64_t value;
} Test_Record_Layout;

static Om_Status trace_test_record(Om_Execution_Context *context,
                                   const void *value,
                                   Om_Runtime_Trace_Visitor visitor,
                                   void *visitor_context)
{
    (void)context;
    const Test_Record_Layout *record = value;
    return visitor(record->label, visitor_context);
}

static const Om_Runtime_Type_Descriptor TEST_NULL_TYPE = {
    .fingerprint = UINT64_C(8),
    .kind = OM_RUNTIME_TYPE_NULL,
    .size = 0,
    .alignment = 1,
};

static const Om_Runtime_Type_Descriptor TEST_INT_TYPE = {
    .fingerprint = UINT64_C(2),
    .kind = OM_RUNTIME_TYPE_INT,
    .size = sizeof(int64_t),
    .alignment = _Alignof(int64_t),
};

static const Om_Runtime_Type_Descriptor TEST_STRING_TYPE = {
    .fingerprint = UINT64_C(3),
    .kind = OM_RUNTIME_TYPE_STRING,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

static const Om_Runtime_Field_Descriptor TEST_RECORD_FIELDS[] = {
    {.offset = offsetof(Test_Record_Layout, label), .type = &TEST_STRING_TYPE},
    {.offset = offsetof(Test_Record_Layout, value), .type = &TEST_INT_TYPE},
};

static const Om_Runtime_Type_Descriptor TEST_RECORD_TYPE = {
    .fingerprint = UINT64_C(4),
    .kind = OM_RUNTIME_TYPE_RECORD,
    .size = sizeof(Test_Record_Layout),
    .alignment = _Alignof(Test_Record_Layout),
    .fields = TEST_RECORD_FIELDS,
    .field_count = sizeof(TEST_RECORD_FIELDS) / sizeof(TEST_RECORD_FIELDS[0]),
    .trace = trace_test_record,
};

static const Om_Runtime_Type_Descriptor TEST_STRING_LIST_TYPE = {
    .fingerprint = UINT64_C(5),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &TEST_STRING_TYPE,
};

#ifdef OM_RUNTIME_TEST_SUPPORT_ONLY
static const Om_Runtime_Type_Descriptor TEST_STRING_LIST_LIST_TYPE = {
    .fingerprint = UINT64_C(13),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &TEST_STRING_LIST_TYPE,
};

static const Om_Runtime_Type_Descriptor TEST_INT_LIST_TYPE = {
    .fingerprint = UINT64_C(14),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &TEST_INT_TYPE,
};
#endif

static const Om_Runtime_Type_Descriptor TEST_STRING_INT_MAP_TYPE = {
    .fingerprint = UINT64_C(6),
    .kind = OM_RUNTIME_TYPE_MAP,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .key_type = &TEST_STRING_TYPE,
    .value_type = &TEST_INT_TYPE,
};

static const Om_Runtime_Type_Descriptor TEST_NULL_LIST_TYPE = {
    .fingerprint = UINT64_C(9),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &TEST_NULL_TYPE,
};

static const Om_Runtime_Type_Descriptor TEST_INT_NULL_MAP_TYPE = {
    .fingerprint = UINT64_C(10),
    .kind = OM_RUNTIME_TYPE_MAP,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .key_type = &TEST_INT_TYPE,
    .value_type = &TEST_NULL_TYPE,
};

#ifdef OM_RUNTIME_TEST_SUPPORT_ONLY
static const Om_Runtime_Type_Descriptor TEST_INT_INT_MAP_TYPE = {
    .fingerprint = UINT64_C(12),
    .kind = OM_RUNTIME_TYPE_MAP,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .key_type = &TEST_INT_TYPE,
    .value_type = &TEST_INT_TYPE,
};
#endif

static size_t cstring_release_count = 0;
static size_t native_pointer_release_count = 0;

static void release_test_cstring(void *pointer)
{
    ++cstring_release_count;
    free(pointer);
}

static void release_test_native_pointer(void *pointer)
{
    ++native_pointer_release_count;
    free(pointer);
}

static int runtime_fault_entry(Om_Execution_Context *context)
{
    static const uint8_t bytes[] = {'r', 'o', 'o', 't'};
    void *root = NULL;
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, &root, 1);
    if (frame == NULL
        || om_runtime_string_create_utf8_default(context, bytes,
            sizeof(bytes), &root) != OM_STATUS_OK) {
        return 1;
    }
    om_runtime_fault(context, OM_RUNTIME_FAULT_INTEGER_OVERFLOW, 0);
}

static int runtime_success_entry(Om_Execution_Context *context)
{
    return context == NULL ? 1 : 23;
}

static int runtime_nested_boundary_entry(Om_Execution_Context *context)
{
    const int nested_result = om_runtime_run(context, runtime_fault_entry);
    return nested_result == 70 ? 29 : 1;
}

static bool test_runtime_fault_boundary(void)
{
    static const Om_Runtime_Source_Span source_spans[] = {
        {.path = "runtime_test.bas", .line = 12, .column = 5},
    };
    Om_Execution_Context *context = om_runtime_context_create(0);
    bool passed = false;
    RUNTIME_ASSERT(context != NULL);
    RUNTIME_ASSERT(om_runtime_set_source_spans(context, source_spans,
        sizeof(source_spans) / sizeof(source_spans[0])) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_run(context, runtime_fault_entry) == 70);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 1);
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    RUNTIME_ASSERT(om_runtime_run(context, runtime_success_entry) == 23);
    RUNTIME_ASSERT(om_runtime_run(context,
        runtime_nested_boundary_entry) == 29);
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    passed = true;

cleanup:
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_native_pointer_lifecycle(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *roots[2] = {NULL, NULL};
    bool passed = false;
    native_pointer_release_count = 0;
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, roots, 2);
    RUNTIME_ASSERT(frame != NULL);

    int *transferred_pointer = malloc(sizeof(*transferred_pointer));
    RUNTIME_ASSERT(transferred_pointer != NULL);
    *transferred_pointer = 17;
    RUNTIME_ASSERT(om_runtime_native_pointer_create(context,
        transferred_pointer, release_test_native_pointer, NULL, &roots[0])
        == OM_STATUS_OK);
    void *borrowed_pointer = NULL;
    RUNTIME_ASSERT(om_runtime_native_pointer_borrow(context, roots[0],
        &borrowed_pointer) == OM_STATUS_OK);
    RUNTIME_ASSERT(borrowed_pointer == transferred_pointer);
    void *taken_pointer = NULL;
    RUNTIME_ASSERT(om_runtime_native_pointer_take(context, roots[0],
        &taken_pointer) == OM_STATUS_OK);
    RUNTIME_ASSERT(taken_pointer == transferred_pointer);
    release_test_native_pointer(taken_pointer);
    RUNTIME_ASSERT(native_pointer_release_count == 1);
    roots[0] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    RUNTIME_ASSERT(native_pointer_release_count == 1);

    int *owned_pointer = malloc(sizeof(*owned_pointer));
    RUNTIME_ASSERT(owned_pointer != NULL);
    *owned_pointer = 23;
    RUNTIME_ASSERT(om_runtime_native_pointer_create(context, owned_pointer,
        release_test_native_pointer, NULL, &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_native_pointer_create(context, owned_pointer,
        NULL, roots[0], &roots[1]) == OM_STATUS_OK);
    roots[0] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 2);
    borrowed_pointer = NULL;
    RUNTIME_ASSERT(om_runtime_native_pointer_borrow(context, roots[1],
        &borrowed_pointer) == OM_STATUS_OK);
    RUNTIME_ASSERT(borrowed_pointer == owned_pointer
        && *(int *)borrowed_pointer == 23);
    RUNTIME_ASSERT(native_pointer_release_count == 1);
    roots[1] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    RUNTIME_ASSERT(native_pointer_release_count == 2);

    int *automatic_pointer = malloc(sizeof(*automatic_pointer));
    RUNTIME_ASSERT(automatic_pointer != NULL);
    *automatic_pointer = 31;
    RUNTIME_ASSERT(om_runtime_native_pointer_create(context,
        automatic_pointer, release_test_native_pointer, NULL, &roots[0])
        == OM_STATUS_OK);
    roots[0] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    RUNTIME_ASSERT(native_pointer_release_count == 3);
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_native_objects_and_collection(void)
{
    Om_Execution_Context *context = om_runtime_context_create(128);
    Om_Runtime_Root_Frame *roots_frame = NULL;
    void *roots[6] = {0};
    bool passed = false;
    RUNTIME_ASSERT(context != NULL);
    roots_frame = om_runtime_roots_push(context, roots, 6);
    RUNTIME_ASSERT(roots_frame != NULL);

    static const uint8_t hello_bytes[] = {'h', 'e', 'l', 'l', 'o'};
    static const uint8_t duplicate_bytes[] = {'h', 'e', 'l', 'l', 'o'};
    static const uint8_t world_bytes[] = {'w', 'o', 'r', 'l', 'd'};
    static const uint8_t invalid_bytes[] = {UINT8_C(0xc3), UINT8_C(0x78)};
    RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
        (Om_Const_Byte_Slice){hello_bytes, sizeof(hello_bytes)}, &roots[0])
        == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
        (Om_Const_Byte_Slice){invalid_bytes, sizeof(invalid_bytes)}, &roots[4])
        == OM_STATUS_INVALID_UTF8);
    RUNTIME_ASSERT(roots[4] == NULL);
    RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
        (Om_Const_Byte_Slice){duplicate_bytes, sizeof(duplicate_bytes)},
        &roots[4]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
        (Om_Const_Byte_Slice){world_bytes, sizeof(world_bytes)}, &roots[5])
        == OM_STATUS_OK);

    RUNTIME_ASSERT(om_runtime_record_create(context, &TEST_RECORD_TYPE,
                                             &roots[1]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_set(context, roots[1], 0,
        (Om_Runtime_Value){.managed = roots[0]}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_set(context, roots[1], 1,
        (Om_Runtime_Value){.integer = 42}) == OM_STATUS_OK);

    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_STRING_LIST_TYPE,
                                           &roots[2]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[2],
        (Om_Runtime_Value){.managed = roots[4]}) == OM_STATUS_OK);

    RUNTIME_ASSERT(om_runtime_map_create(context, &TEST_STRING_INT_MAP_TYPE,
                                          &roots[3]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, roots[3],
        (Om_Runtime_Value){.managed = roots[0]},
        (Om_Runtime_Value){.integer = 6}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, roots[3],
        (Om_Runtime_Value){.managed = roots[4]},
        (Om_Runtime_Value){.integer = 7}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, roots[3],
        (Om_Runtime_Value){.managed = roots[5]},
        (Om_Runtime_Value){.integer = 9}) == OM_STATUS_OK);

    Om_Runtime_Value value = {0};
    RUNTIME_ASSERT(om_runtime_record_get(context, roots[1], 0, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[0]);
    RUNTIME_ASSERT(om_runtime_record_get(context, roots[1], 1, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 42);
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[2], &(size_t){0})
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_get(context, roots[2], 0, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[4]);
    RUNTIME_ASSERT(om_runtime_map_length(context, roots[3], &(size_t){0})
                   == OM_STATUS_OK);
    size_t map_length = 0;
    RUNTIME_ASSERT(om_runtime_map_length(context, roots[3], &map_length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(map_length == 2);
    RUNTIME_ASSERT(om_runtime_map_get(context, roots[3],
        (Om_Runtime_Value){.managed = roots[0]}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 7);
    RUNTIME_ASSERT(om_runtime_map_get(context, roots[3],
        (Om_Runtime_Value){.managed = roots[4]}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 7);
    RUNTIME_ASSERT(om_runtime_map_get(context, roots[3],
        (Om_Runtime_Value){.managed = roots[5]}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 9);
    RUNTIME_ASSERT(om_runtime_map_get(context, roots[3],
        (Om_Runtime_Value){.managed = NULL}, &value) == OM_STATUS_NOT_FOUND);

    Om_String_View view = {0};
    RUNTIME_ASSERT(om_runtime_string_view(context, roots[0], &view)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(view.length == sizeof(hello_bytes)
        && memcmp(view.data, hello_bytes, sizeof(hello_bytes)) == 0);
    roots[0] = NULL;
    roots[4] = NULL;
    roots[5] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 6);

    roots[1] = NULL;
    roots[2] = NULL;
    roots[3] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    RUNTIME_ASSERT(om_runtime_collection_count(context) >= 2);
    passed = true;

cleanup:
    if (roots_frame != NULL) {
        (void)om_runtime_roots_pop(context, roots_frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_collection_threshold_tracks_live_heap(void)
{
    Om_Execution_Context *context = om_runtime_context_create(64);
    Om_Runtime_Root_Frame *frame = NULL;
    void *roots[128] = {0};
    bool passed = false;
    static const uint8_t byte = 'x';
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, roots,
        sizeof(roots) / sizeof(roots[0]));
    RUNTIME_ASSERT(frame != NULL);
    for (size_t index = 0; index < sizeof(roots) / sizeof(roots[0]);
         ++index) {
        RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
            (Om_Const_Byte_Slice){&byte, 1}, &roots[index]) == OM_STATUS_OK);
    }
    RUNTIME_ASSERT(om_runtime_collection_count(context) < 16);
    RUNTIME_ASSERT(om_runtime_live_object_count(context)
                   == sizeof(roots) / sizeof(roots[0]));
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_runtime_cstring_conversion(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *roots[2] = {0};
    bool passed = false;
    static const uint8_t embedded_nul[] = {'a', 0, 'b'};
    static const uint8_t invalid_utf8[] = {UINT8_C(0xc3), UINT8_C(0x78), 0};
    static const uint8_t latin_e_acute[] = {UINT8_C(0xc3), UINT8_C(0xa9)};
    static const uint8_t tokyo[] = {
        UINT8_C(0xe6), UINT8_C(0x9d), UINT8_C(0xb1),
        UINT8_C(0xe4), UINT8_C(0xba), UINT8_C(0xac),
    };
    static const char regular_text[] = "borrowed text";
    static const char owned_text[] = "owned text";

    cstring_release_count = 0;
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, roots, 2);
    RUNTIME_ASSERT(frame != NULL);
    RUNTIME_ASSERT(om_runtime_string_create(context, &TEST_STRING_TYPE,
        (Om_Const_Byte_Slice){embedded_nul, sizeof(embedded_nul)}, &roots[0])
        == OM_STATUS_OK);
    const uint8_t *view_bytes = (const uint8_t *)"not reset";
    size_t view_length = 99;
    RUNTIME_ASSERT(om_runtime_string_cstring_view(context, roots[0],
        &view_bytes, &view_length) == OM_STATUS_INVALID_ARGUMENT);
    RUNTIME_ASSERT(view_bytes == NULL && view_length == 0);

    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context,
        (const uint8_t *)regular_text, sizeof(regular_text) - 1, &roots[0])
        == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_string_cstring_view(context, roots[0],
        &view_bytes, &view_length) == OM_STATUS_OK);
    RUNTIME_ASSERT(view_length == sizeof(regular_text) - 1
        && memcmp(view_bytes, regular_text, view_length) == 0
        && view_bytes[view_length] == 0);

    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context,
        latin_e_acute, sizeof(latin_e_acute), &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context,
        tokyo, sizeof(tokyo), &roots[1]) == OM_STATUS_OK);
    int32_t string_order = 0;
    RUNTIME_ASSERT(om_runtime_string_compare(context, roots[0], roots[1],
        &string_order) == OM_STATUS_OK && string_order < 0);
    RUNTIME_ASSERT(om_runtime_string_compare(context, roots[0], roots[0],
        &string_order) == OM_STATUS_OK && string_order == 0);

    char *owned_copy = malloc(sizeof(owned_text));
    RUNTIME_ASSERT(owned_copy != NULL);
    (void)memcpy(owned_copy, owned_text, sizeof(owned_text));
    RUNTIME_ASSERT(om_runtime_string_create_from_owned_cstring(context,
        owned_copy, release_test_cstring, &roots[1]) == OM_STATUS_OK);
    RUNTIME_ASSERT(cstring_release_count == 1);
    Om_String_View managed_view = {0};
    RUNTIME_ASSERT(om_runtime_string_view(context, roots[1], &managed_view)
        == OM_STATUS_OK);
    RUNTIME_ASSERT(managed_view.length == sizeof(owned_text) - 1
        && memcmp(managed_view.data, owned_text, managed_view.length) == 0);

    char *invalid_copy = malloc(sizeof(invalid_utf8));
    RUNTIME_ASSERT(invalid_copy != NULL);
    (void)memcpy(invalid_copy, invalid_utf8, sizeof(invalid_utf8));
    RUNTIME_ASSERT(om_runtime_string_create_from_owned_cstring(context,
        invalid_copy, release_test_cstring, &roots[1])
        == OM_STATUS_INVALID_UTF8);
    RUNTIME_ASSERT(cstring_release_count == 2 && roots[1] == NULL);

    char *orphaned_copy = malloc(sizeof(owned_text));
    RUNTIME_ASSERT(orphaned_copy != NULL);
    (void)memcpy(orphaned_copy, owned_text, sizeof(owned_text));
    RUNTIME_ASSERT(om_runtime_string_create_from_owned_cstring(context,
        orphaned_copy, release_test_cstring, NULL)
        == OM_STATUS_INVALID_ARGUMENT);
    RUNTIME_ASSERT(cstring_release_count == 3);

    static const char unicode_text[] = {
        'a', (char)0xc3, (char)0xa9, (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, 'z', 0
    };
    static const char unicode_query[] = {
        (char)0xe6, (char)0x9d, (char)0xb1, (char)0xe4, (char)0xba,
        (char)0xac, 0
    };
    static const char expected_slice[] = {
        (char)0xc3, (char)0xa9, (char)0xe6, (char)0x9d, (char)0xb1, 0
    };
    RUNTIME_ASSERT(om_runtime_text_length(unicode_text) == 5);
    RUNTIME_ASSERT(om_runtime_text_byte_length(unicode_text) == 10);
    RUNTIME_ASSERT(om_runtime_text_starts_with(unicode_text, "a"));
    RUNTIME_ASSERT(om_runtime_text_ends_with(unicode_text, "z"));
    RUNTIME_ASSERT(om_runtime_text_find(unicode_text, unicode_query, 0) == 2);
    RUNTIME_ASSERT(om_runtime_text_find_last(unicode_text,
        unicode_query) == 2);
    RUNTIME_ASSERT(om_runtime_text_find_last("a.b.c", ".") == 3);
    RUNTIME_ASSERT(om_runtime_text_find_last("abc", "") == 3);
    RUNTIME_ASSERT(om_runtime_text_find(unicode_text, "", 4) == 4);
    RUNTIME_ASSERT(om_runtime_text_find(unicode_text, "missing", 0) == -1);
    char *slice = om_runtime_text_slice(unicode_text, 1, 2);
    RUNTIME_ASSERT(slice != NULL
        && memcmp(slice, expected_slice, sizeof(expected_slice)) == 0);
    om_runtime_text_release(slice);
    RUNTIME_ASSERT(om_runtime_text_slice(unicode_text, 6, 0) == NULL);
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_json_native(void)
{
    bool passed = false;
    char *owned = NULL;
    char *oversized = NULL;
    static const char source[] =
        " { \"a\" : [true, null, -0, 1.234567890123456789e+999, "
        "\"\\uD834\\uDD1E\"], \"x\":\"line\\nbreak\" } \t";
    static const char compact_expected[] =
        "{\"a\":[true,null,-0,1.234567890123456789e+999,"
        "\"\\uD834\\uDD1E\"],\"x\":\"line\\nbreak\"}";
    RUNTIME_ASSERT(om_runtime_json_valid(source));
    RUNTIME_ASSERT(om_runtime_json_kind(source)
                   == OM_RUNTIME_JSON_KIND_OBJECT);
    owned = om_runtime_json_compact(source);
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, compact_expected) == 0);
    om_runtime_json_release(owned);
    owned = NULL;

    static const char array[] = "[1, {\"x\":[2]}, \"z\"]";
    RUNTIME_ASSERT(om_runtime_json_array_length(array) == 3);
    RUNTIME_ASSERT(om_runtime_json_array_index_valid(array, 1));
    RUNTIME_ASSERT(!om_runtime_json_array_index_valid(array, -1));
    RUNTIME_ASSERT(!om_runtime_json_array_index_valid(array, 3));
    owned = om_runtime_json_array_get(array, 1);
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, "{\"x\":[2]}") == 0);
    om_runtime_json_release(owned);
    owned = NULL;

    static const char escaped_key[] = "{\"\\u0061\":1}";
    RUNTIME_ASSERT(om_runtime_json_object_length(escaped_key) == 1);
    RUNTIME_ASSERT(om_runtime_json_object_length("{}") == 0);
    RUNTIME_ASSERT(om_runtime_json_object_length("[]") == -1);
    RUNTIME_ASSERT(om_runtime_json_object_key_valid(escaped_key, "a"));
    owned = om_runtime_json_object_get(escaped_key, "a");
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, "1") == 0);
    om_runtime_json_release(owned);
    owned = NULL;
    owned = om_runtime_json_object_keys("{\"first\":1,\"\\u0061\":2}");
    RUNTIME_ASSERT(owned != NULL
                   && strcmp(owned, "[\"first\",\"\\u0061\"]") == 0);
    om_runtime_json_release(owned);
    owned = NULL;

    static const char escaped_string[] = "\"line\\n\\u00e9\"";
    owned = om_runtime_json_string_value(escaped_string);
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, "line\n\xc3\xa9") == 0);
    om_runtime_json_release(owned);
    owned = NULL;
    RUNTIME_ASSERT(om_runtime_json_string_value("true") == NULL);
    RUNTIME_ASSERT(om_runtime_json_int64_valid("-9223372036854775808"));
    RUNTIME_ASSERT(om_runtime_json_int64_value("-9223372036854775808")
                   == INT64_MIN);
    RUNTIME_ASSERT(om_runtime_json_int64_valid("9223372036854775807"));
    RUNTIME_ASSERT(om_runtime_json_int64_value("9223372036854775807")
                   == INT64_MAX);
    RUNTIME_ASSERT(!om_runtime_json_int64_valid("9223372036854775808"));
    RUNTIME_ASSERT(!om_runtime_json_int64_valid("1.0"));
    RUNTIME_ASSERT(!om_runtime_json_int64_valid("1e0"));
    RUNTIME_ASSERT(om_runtime_json_float64_valid("1.25e2"));
    RUNTIME_ASSERT(om_runtime_json_float64_value("1.25e2") == 125.0);
    RUNTIME_ASSERT(!om_runtime_json_float64_valid("1e9999"));
    RUNTIME_ASSERT(!om_runtime_json_float64_valid("1e-9999"));

    static const char duplicate_key[] = "{\"a\":1,\"\\u0061\":2}";
    RUNTIME_ASSERT(!om_runtime_json_valid(duplicate_key));
    RUNTIME_ASSERT(om_runtime_json_error_code(duplicate_key)
                   == OM_RUNTIME_JSON_ERROR_DUPLICATE_KEY);
    RUNTIME_ASSERT(om_runtime_json_error_offset(duplicate_key) == 7);
    owned = om_runtime_json_error_message(duplicate_key);
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, "duplicate object key") == 0);
    om_runtime_json_release(owned);
    owned = NULL;

    static const char invalid_number[] = "[01]";
    RUNTIME_ASSERT(om_runtime_json_error_code(invalid_number)
                   == OM_RUNTIME_JSON_ERROR_SYNTAX);
    static const char invalid_surrogate[] = "\"\\uD800\"";
    RUNTIME_ASSERT(om_runtime_json_error_code(invalid_surrogate)
                   == OM_RUNTIME_JSON_ERROR_SYNTAX);
    static const char invalid_utf8[] = {(char)0xc3, (char)0x28, 0};
    RUNTIME_ASSERT(om_runtime_json_error_code(invalid_utf8)
                   == OM_RUNTIME_JSON_ERROR_UTF8);
    RUNTIME_ASSERT(om_runtime_json_error_offset(invalid_utf8) == 0);

    char nested[260] = {0};
    (void)memset(nested, '[', 129);
    nested[129] = '0';
    (void)memset(nested + 130, ']', 129);
    nested[259] = '\0';
    RUNTIME_ASSERT(om_runtime_json_error_code(nested)
                   == OM_RUNTIME_JSON_ERROR_DEPTH);

    const size_t oversized_length = (size_t)1024 * (size_t)1024 + 1;
    oversized = om_alloc(oversized_length + 1);
    RUNTIME_ASSERT(oversized != NULL);
    (void)memset(oversized, ' ', oversized_length);
    oversized[oversized_length] = '\0';
    RUNTIME_ASSERT(om_runtime_json_error_code(oversized)
                   == OM_RUNTIME_JSON_ERROR_SIZE);
    om_free(oversized);
    oversized = NULL;

    static const char quote_input[] = {'a', '\n', '"', '\\', 0};
    static const char quote_expected[] = "\"a\\n\\\"\\\\\"";
    owned = om_runtime_json_quote(quote_input);
    RUNTIME_ASSERT(owned != NULL && strcmp(owned, quote_expected) == 0);
    passed = true;

cleanup:
    om_runtime_json_release(owned);
    om_free(oversized);
    return passed;
}

static bool test_unit_null_containers_and_root_order(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *outer = NULL;
    Om_Runtime_Root_Frame *inner = NULL;
    void *roots[2] = {0};
    bool passed = false;
    RUNTIME_ASSERT(context != NULL);
    outer = om_runtime_roots_push(context, roots, 2);
    RUNTIME_ASSERT(outer != NULL);
    inner = om_runtime_roots_push(context, NULL, 0);
    RUNTIME_ASSERT(inner != NULL);
    RUNTIME_ASSERT(om_runtime_roots_pop(context, outer)
                   == OM_STATUS_INVALID_ARGUMENT);
    RUNTIME_ASSERT(om_runtime_roots_pop(context, inner) == OM_STATUS_OK);
    inner = NULL;

    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_NULL_LIST_TYPE,
                                           &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[0],
        (Om_Runtime_Value){.managed = NULL}) == OM_STATUS_OK);
    size_t length = 0;
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[0], &length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(length == 1);

    RUNTIME_ASSERT(om_runtime_map_create(context, &TEST_INT_NULL_MAP_TYPE,
                                          &roots[1]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, roots[1],
        (Om_Runtime_Value){.integer = 7},
        (Om_Runtime_Value){.managed = NULL}) == OM_STATUS_OK);
    Om_Runtime_Value value = {.integer = -1};
    RUNTIME_ASSERT(om_runtime_map_get(context, roots[1],
        (Om_Runtime_Value){.integer = 7}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == NULL);
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 2);
    passed = true;

cleanup:
    if (inner != NULL) {
        (void)om_runtime_roots_pop(context, inner);
    }
    if (outer != NULL) {
        (void)om_runtime_roots_pop(context, outer);
    }
    om_runtime_context_destroy(context);
    return passed;
}

static bool test_cycle_collection(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *root = NULL;
    bool passed = false;
    RUNTIME_ASSERT(context != NULL);

    Om_Runtime_Type_Descriptor recursive_type = {
        .fingerprint = UINT64_C(7),
        .kind = OM_RUNTIME_TYPE_RECORD,
        .size = sizeof(void *),
        .alignment = _Alignof(void *),
    };
    Om_Runtime_Field_Descriptor recursive_field = {
        .offset = 0,
        .type = &recursive_type,
    };
    recursive_type.fields = &recursive_field;
    recursive_type.field_count = 1;

    frame = om_runtime_roots_push(context, &root, 1);
    RUNTIME_ASSERT(frame != NULL);
    RUNTIME_ASSERT(om_runtime_record_create(context, &recursive_type, &root)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_set(context, root, 0,
        (Om_Runtime_Value){.managed = root}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 1);
    root = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

#ifdef OM_RUNTIME_TEST_SUPPORT_ONLY
bool om_runtime_test_fault_boundary(void)
{
    return test_runtime_fault_boundary();
}

bool om_runtime_test_root_order(void)
{
    return test_unit_null_containers_and_root_order();
}

bool om_runtime_test_gc(void)
{
    return test_collection_threshold_tracks_live_heap()
        && test_cycle_collection()
        && test_native_objects_and_collection();
}

bool om_runtime_test_strings(void)
{
    return test_runtime_cstring_conversion();
}

bool om_runtime_test_records(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *roots[2] = {NULL, NULL};
    bool passed = false;
    static const uint8_t label[] = "record value";
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, roots, 2);
    RUNTIME_ASSERT(frame != NULL);
    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context, label,
        sizeof(label) - 1, &roots[1]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_create(context, &TEST_RECORD_TYPE,
        &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_set(context, roots[0], 0,
        (Om_Runtime_Value){.managed = roots[1]}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_record_set(context, roots[0], 1,
        (Om_Runtime_Value){.integer = 42}) == OM_STATUS_OK);
    Om_Runtime_Value value = {0};
    RUNTIME_ASSERT(om_runtime_record_get(context, roots[0], 0, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[1]);
    RUNTIME_ASSERT(om_runtime_record_get(context, roots[0], 1, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 42);
    roots[1] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 2);
    roots[0] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

bool om_runtime_test_lists(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *roots[10] = {NULL};
    bool passed = false;
    static const uint8_t item[] = "list item";
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, roots, 10);
    RUNTIME_ASSERT(frame != NULL);
    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context, item,
        sizeof(item) - 1, &roots[1]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_STRING_LIST_TYPE,
        &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[0],
        (Om_Runtime_Value){.managed = roots[1]}) == OM_STATUS_OK);
    size_t length = 0;
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[0], &length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(length == 1);
    Om_Runtime_Value value = {0};
    RUNTIME_ASSERT(om_runtime_list_get(context, roots[0], 0, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[1]);
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_STRING_LIST_TYPE,
        &roots[2]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[2],
        (Om_Runtime_Value){.managed = roots[1]}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_concat(context, roots[0], roots[2],
        &roots[3]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[3], &length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(length == 2);
    RUNTIME_ASSERT(om_runtime_list_get(context, roots[3], 1, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[1]);
    RUNTIME_ASSERT(om_runtime_list_create(context,
        &TEST_STRING_LIST_LIST_TYPE, &roots[4]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[4],
        (Om_Runtime_Value){.managed = roots[0]}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_INT_LIST_TYPE,
        &roots[5]) == OM_STATUS_OK);
    for (int64_t index = 0; index < 1024; ++index) {
        RUNTIME_ASSERT(om_runtime_list_append(context, roots[5],
            (Om_Runtime_Value){.integer = index}) == OM_STATUS_OK);
    }
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[5], &length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(length == 1024);
    RUNTIME_ASSERT(om_runtime_list_get(context, roots[5], 1023, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 1023);
    RUNTIME_ASSERT(om_runtime_list_get(context, roots[4], 0, &value)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(value.managed == roots[0]);
    roots[1] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 6);
    roots[0] = NULL;
    roots[2] = NULL;
    roots[3] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 4);
    roots[4] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 1);
    roots[5] = NULL;
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 0);
#ifdef OM_RUNTIME_TEST_SUPPORT_ONLY
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_INT_LIST_TYPE,
        &roots[0]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_INT_LIST_TYPE,
        &roots[1]) == OM_STATUS_OK);
    const int64_t payloads[] = {4, 2, 3, 2, 2};
    const int64_t keys[] = {1, 0, 0, 0, 0};
    for (size_t index = 0; index < 5; ++index) {
        RUNTIME_ASSERT(om_runtime_list_append(context, roots[0],
            (Om_Runtime_Value){.integer = payloads[index]}) == OM_STATUS_OK);
        RUNTIME_ASSERT(om_runtime_list_append(context, roots[1],
            (Om_Runtime_Value){.integer = keys[index]}) == OM_STATUS_OK);
    }
    const void *integer_keys[] = {roots[1]};
    RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
        integer_keys, 1, 0, 3, &roots[2]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[2], &length)
                   == OM_STATUS_OK && length == 3);
    const int64_t expected_top[] = {2, 3, 2};
    for (size_t index = 0; index < 3; ++index) {
        RUNTIME_ASSERT(om_runtime_list_get(context, roots[2], index, &value)
                       == OM_STATUS_OK);
        RUNTIME_ASSERT(value.integer == expected_top[index]);
    }
    RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
        integer_keys, 1, 0, 0, &roots[8]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_length(context, roots[8], &length)
                   == OM_STATUS_OK && length == 0);
    RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
        integer_keys, 1, UINT64_C(1), INT64_MAX, &roots[3]) == OM_STATUS_OK);
    const int64_t expected_descending[] = {4, 2, 3, 2, 2};
    for (size_t index = 0; index < 5; ++index) {
        RUNTIME_ASSERT(om_runtime_list_get(context, roots[3], index, &value)
                       == OM_STATUS_OK);
        RUNTIME_ASSERT(value.integer == expected_descending[index]);
    }
    RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
        integer_keys, 1, 0, INT64_MAX, &roots[9]) == OM_STATUS_OK);
    for (int64_t take = 0; take <= 5; ++take) {
        RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
            integer_keys, 1, 0, take, &roots[8]) == OM_STATUS_OK);
        RUNTIME_ASSERT(om_runtime_list_length(context, roots[8], &length)
                       == OM_STATUS_OK && length == (size_t)take);
        for (size_t index = 0; index < (size_t)take; ++index) {
            Om_Runtime_Value expected = {0};
            RUNTIME_ASSERT(om_runtime_list_get(context, roots[9], index,
                &expected) == OM_STATUS_OK);
            RUNTIME_ASSERT(om_runtime_list_get(context, roots[8], index,
                &value) == OM_STATUS_OK);
            RUNTIME_ASSERT(value.integer == expected.integer);
        }
    }
    RUNTIME_ASSERT(om_runtime_list_create(context, &TEST_STRING_LIST_TYPE,
        &roots[4]) == OM_STATUS_OK);
    static const uint8_t key_a[] = "a";
    static const uint8_t key_b[] = "b";
    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context, key_b, 1,
        &roots[5]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_string_create_utf8_default(context, key_a, 1,
        &roots[6]) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[4],
        (Om_Runtime_Value){.managed = roots[5]}) == OM_STATUS_OK);
    for (size_t index = 0; index < 3; ++index) {
        RUNTIME_ASSERT(om_runtime_list_append(context, roots[4],
            (Om_Runtime_Value){.managed = roots[6]}) == OM_STATUS_OK);
    }
    RUNTIME_ASSERT(om_runtime_list_append(context, roots[4],
        (Om_Runtime_Value){.managed = roots[5]}) == OM_STATUS_OK);
    const void *string_keys[] = {roots[4]};
    RUNTIME_ASSERT(om_runtime_list_stable_order(context, roots[0],
        string_keys, 1, 0, INT64_MAX, &roots[7]) == OM_STATUS_OK);
    const int64_t expected_strings[] = {2, 3, 2, 4, 2};
    for (size_t index = 0; index < 5; ++index) {
        RUNTIME_ASSERT(om_runtime_list_get(context, roots[7], index, &value)
                       == OM_STATUS_OK);
        RUNTIME_ASSERT(value.integer == expected_strings[index]);
    }
#endif
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

bool om_runtime_test_maps(void)
{
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Root_Frame *frame = NULL;
    void *root = NULL;
    bool passed = false;
    RUNTIME_ASSERT(context != NULL);
    frame = om_runtime_roots_push(context, &root, 1);
    RUNTIME_ASSERT(frame != NULL);
    RUNTIME_ASSERT(om_runtime_map_create(context, &TEST_INT_INT_MAP_TYPE,
        &root) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, root,
        (Om_Runtime_Value){.integer = 7},
        (Om_Runtime_Value){.integer = 70}) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_map_set(context, root,
        (Om_Runtime_Value){.integer = 9},
        (Om_Runtime_Value){.integer = 90}) == OM_STATUS_OK);
    size_t length = 0;
    RUNTIME_ASSERT(om_runtime_map_length(context, root, &length)
                   == OM_STATUS_OK);
    RUNTIME_ASSERT(length == 2);
    Om_Runtime_Value value = {0};
    RUNTIME_ASSERT(om_runtime_map_get(context, root,
        (Om_Runtime_Value){.integer = 7}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 70);
    RUNTIME_ASSERT(om_runtime_map_get(context, root,
        (Om_Runtime_Value){.integer = 9}, &value) == OM_STATUS_OK);
    RUNTIME_ASSERT(value.integer == 90);
    RUNTIME_ASSERT(om_runtime_map_get(context, root,
        (Om_Runtime_Value){.integer = 11}, &value) == OM_STATUS_NOT_FOUND);
    RUNTIME_ASSERT(om_runtime_collect(context) == OM_STATUS_OK);
    RUNTIME_ASSERT(om_runtime_live_object_count(context) == 1);
    passed = true;

cleanup:
    if (frame != NULL) {
        (void)om_runtime_roots_pop(context, frame);
    }
    om_runtime_context_destroy(context);
    return passed;
}

bool om_runtime_test_native_pointers(void)
{
    return test_native_pointer_lifecycle();
}

bool om_runtime_test_json(void)
{
    return test_json_native();
}
#endif

#ifndef OM_RUNTIME_TEST_SUPPORT_ONLY
int main(void)
{
    const bool objects_passed = test_native_objects_and_collection();
    const bool threshold_passed = test_collection_threshold_tracks_live_heap();
    const bool cstring_passed = test_runtime_cstring_conversion();
    const bool cycles_passed = test_cycle_collection();
    const bool container_passed = test_unit_null_containers_and_root_order();
    const bool fault_passed = test_runtime_fault_boundary();
    const bool native_pointer_passed = test_native_pointer_lifecycle();
    const bool json_passed = test_json_native();
    (void)fprintf(stdout, "Native runtime tests: %s\n",
        objects_passed && threshold_passed && cstring_passed && cycles_passed
            && container_passed && fault_passed && native_pointer_passed
            && json_passed
            ? "passed" : "failed");
    return objects_passed && threshold_passed && cstring_passed && cycles_passed
            && container_passed && fault_passed && native_pointer_passed
            && json_passed
        ? 0 : 1;
}
#endif
