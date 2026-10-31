#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

const Om_Runtime_Type_Descriptor RUNTIME_STRING_TYPE = {
    .fingerprint = UINT64_C(0x6f6d737472696e67),
    .kind = OM_RUNTIME_TYPE_STRING,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

struct Om_Runtime_String_Builder {
    size_t length;
    size_t capacity;
    uint8_t *bytes;
};

static Om_Status string_builder_reserve(Om_Runtime_String_Builder *builder,
                                       size_t requested)
{
    if (builder == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (requested <= builder->capacity) return OM_STATUS_OK;
    size_t capacity = builder->capacity == 0 ? 16 : builder->capacity;
    while (capacity < requested) {
        if (capacity > SIZE_MAX / 2) {
            capacity = requested;
            break;
        }
        capacity *= 2;
    }
    uint8_t *bytes = om_realloc_array(builder->bytes, capacity,
                                       sizeof(*bytes));
    if (bytes == NULL) return OM_STATUS_OUT_OF_MEMORY;
    builder->bytes = bytes;
    builder->capacity = capacity;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_builder_create(
    size_t initial_capacity,
    Om_Runtime_String_Builder **result)
{
    if (result == NULL) return OM_STATUS_INVALID_ARGUMENT;
    *result = NULL;
    Om_Runtime_String_Builder *builder = om_alloc(sizeof(*builder));
    if (builder == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *builder = (Om_Runtime_String_Builder){0};
    Om_Status status = string_builder_reserve(builder, initial_capacity);
    if (status != OM_STATUS_OK) {
        om_free(builder->bytes);
        om_free(builder);
        return status;
    }
    *result = builder;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_builder_append_string(
    Om_Execution_Context *context,
    Om_Runtime_String_Builder *builder,
    const void *string)
{
    if (context == NULL || builder == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string, &view);
    size_t output_length = 0;
    if (status == OM_STATUS_OK
        && !om_checked_size_add(builder->length, view.length,
                                &output_length)) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    if (status == OM_STATUS_OK) {
        status = string_builder_reserve(builder, output_length);
    }
    if (status == OM_STATUS_OK && view.length != 0) {
        (void)memcpy(builder->bytes + builder->length, view.data, view.length);
    }
    if (status == OM_STATUS_OK) builder->length = output_length;
    return status;
}

Om_Status om_runtime_string_builder_append_scalar(
    Om_Runtime_String_Builder *builder,
    uint32_t scalar)
{
    if (builder == NULL || scalar > UINT32_C(0x10ffff)
        || (scalar >= UINT32_C(0xd800) && scalar <= UINT32_C(0xdfff))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint8_t encoded[4];
    size_t encoded_length = 0;
    if (scalar <= UINT32_C(0x7f)) {
        encoded[0] = (uint8_t)scalar;
        encoded_length = 1;
    } else if (scalar <= UINT32_C(0x7ff)) {
        encoded[0] = (uint8_t)(UINT32_C(0xc0) | (scalar >> 6));
        encoded[1] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 2;
    } else if (scalar <= UINT32_C(0xffff)) {
        encoded[0] = (uint8_t)(UINT32_C(0xe0) | (scalar >> 12));
        encoded[1] = (uint8_t)(UINT32_C(0x80)
            | ((scalar >> 6) & UINT32_C(0x3f)));
        encoded[2] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 3;
    } else {
        encoded[0] = (uint8_t)(UINT32_C(0xf0) | (scalar >> 18));
        encoded[1] = (uint8_t)(UINT32_C(0x80)
            | ((scalar >> 12) & UINT32_C(0x3f)));
        encoded[2] = (uint8_t)(UINT32_C(0x80)
            | ((scalar >> 6) & UINT32_C(0x3f)));
        encoded[3] = (uint8_t)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        encoded_length = 4;
    }
    size_t output_length = 0;
    if (!om_checked_size_add(builder->length, encoded_length, &output_length)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = string_builder_reserve(builder, output_length);
    if (status == OM_STATUS_OK) {
        (void)memcpy(builder->bytes + builder->length, encoded,
                     encoded_length);
        builder->length = output_length;
    }
    return status;
}

Om_Status om_runtime_string_builder_seal(
    Om_Execution_Context *context,
    Om_Runtime_String_Builder *builder,
    void **result)
{
    if (context == NULL || builder == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Status status = om_runtime_string_create_utf8_default(context,
        builder->bytes, builder->length, result);
    om_free(builder->bytes);
    om_free(builder);
    return status;
}

void om_runtime_string_builder_destroy(Om_Runtime_String_Builder *builder)
{
    if (builder == NULL) return;
    om_free(builder->bytes);
    om_free(builder);
}

Om_Status om_runtime_string_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   Om_Const_Byte_Slice utf8,
                                   void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_STRING
        || (utf8.data == NULL && utf8.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Status status = om_utf8_validate(utf8);
    if (status != OM_STATUS_OK) {
        return status;
    }
    size_t payload_size = 0;
    if (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                             utf8.length, &payload_size)
        || !om_checked_size_add(payload_size, 1, &payload_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    status = allocate_object(context, type, payload_size, 0, &object);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_String_Body *string = object_payload(object);
    string->length = utf8.length;
    if (utf8.length != 0) {
        (void)memcpy(string->bytes, utf8.data, utf8.length);
    }
    string->bytes[utf8.length] = '\0';
    *result = object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_create_utf8(Om_Execution_Context *context,
                                        const Om_Runtime_Type_Descriptor *type,
                                        const uint8_t *bytes,
                                        size_t length,
                                        void **result)
{
    return om_runtime_string_create(context, type,
        (Om_Const_Byte_Slice){bytes, length}, result);
}

Om_Status om_runtime_string_create_utf8_default(Om_Execution_Context *context,
                                                const uint8_t *bytes,
                                                size_t length,
                                                void **result)
{
    return om_runtime_string_create_utf8(context, &RUNTIME_STRING_TYPE,
                                         bytes, length, result);
}

Om_Status om_runtime_string_concat(Om_Execution_Context *context,
                                   const void *left,
                                   const void *right,
                                   void **result)
{
    if (context == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_String_View left_view = {0};
    Om_String_View right_view = {0};
    Om_Status status = om_runtime_string_view(context, left, &left_view);
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, right, &right_view);
    }
    size_t length = 0;
    size_t payload_size = 0;
    if (status == OM_STATUS_OK
        && (!om_checked_size_add(left_view.length, right_view.length, &length)
            || !om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                   length, &payload_size)
            || !om_checked_size_add(payload_size, 1, &payload_size))) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    if (status == OM_STATUS_OK) {
        status = allocate_object(context, &RUNTIME_STRING_TYPE, payload_size,
                                 0, &object);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_String_Body *string = object_payload(object);
    string->length = length;
    if (left_view.length != 0) {
        (void)memcpy(string->bytes, left_view.data, left_view.length);
    }
    if (right_view.length != 0) {
        (void)memcpy(string->bytes + left_view.length, right_view.data,
                     right_view.length);
    }
    string->bytes[length] = '\0';
    *result = object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_concat_many(Om_Execution_Context *context,
                                        const void *const *strings,
                                        size_t count,
                                        void **result)
{
    if (context == NULL || result == NULL
        || (strings == NULL && count != 0)
        || count > SIZE_MAX / sizeof(void *)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void **roots = count == 0 ? NULL
        : om_alloc_array(count, sizeof(*roots));
    if (count != 0 && roots == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < count; ++index) {
        roots[index] = (void *)strings[index];
    }
    Om_Runtime_Root_Frame *frame = count == 0 ? NULL
        : om_runtime_roots_push(context, roots, count);
    if (count != 0 && frame == NULL) {
        om_free(roots);
        return OM_STATUS_OUT_OF_MEMORY;
    }

    Om_Status status = OM_STATUS_OK;
    size_t length = 0;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        Om_String_View view = {0};
        status = om_runtime_string_view(context, roots[index], &view);
        if (status == OM_STATUS_OK
            && !om_checked_size_add(length, view.length, &length)) {
            status = OM_STATUS_OUT_OF_RANGE;
        }
    }
    size_t payload_size = 0;
    if (status == OM_STATUS_OK
        && (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                 length, &payload_size)
            || !om_checked_size_add(payload_size, 1, &payload_size))) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    if (status == OM_STATUS_OK) {
        status = allocate_object(context, &RUNTIME_STRING_TYPE, payload_size,
                                 0, &object);
    }
    if (status == OM_STATUS_OK) {
        Om_Runtime_String_Body *destination = object_payload(object);
        size_t offset = 0;
        for (size_t index = 0; index < count; ++index) {
            Om_String_View view = {0};
            status = om_runtime_string_view(context, roots[index], &view);
            if (status != OM_STATUS_OK) {
                break;
            }
            if (view.length != 0) {
                (void)memcpy(destination->bytes + offset, view.data,
                             view.length);
                offset += view.length;
            }
        }
        if (status == OM_STATUS_OK) {
            destination->length = length;
            destination->bytes[length] = '\0';
            *result = object;
        }
    }
    if (frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, frame);
        if (status == OM_STATUS_OK) {
            status = pop_status;
        }
    }
    om_free(roots);
    return status;
}

Om_Status om_runtime_string_join_list(Om_Execution_Context *context,
                                      const void *list_pointer,
                                      const void *separator_pointer,
                                      void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || list_object == NULL || result == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || list_object->fields.type->element_type == NULL
        || list_object->fields.type->element_type->kind
            != OM_RUNTIME_TYPE_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_List_Body *source = object_payload(list_object);
    if (source->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *roots[2] = {(void *)list_pointer, (void *)separator_pointer};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_String_View separator = {0};
    Om_Status status = om_runtime_string_view(context, roots[1], &separator);
    list_object = find_object(context, roots[0]);
    source = object_payload(list_object);
    size_t length = 0;
    if (status == OM_STATUS_OK && source->length > 1
        && separator.length != 0) {
        const size_t separator_count = source->length - 1;
        if (separator_count > SIZE_MAX / separator.length) {
            status = OM_STATUS_OUT_OF_RANGE;
        } else {
            length = separator_count * separator.length;
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < source->length;
         ++index) {
        Om_String_View item = {0};
        status = om_runtime_string_view(context,
            source->items[index].managed, &item);
        if (status == OM_STATUS_OK
            && !om_checked_size_add(length, item.length, &length)) {
            status = OM_STATUS_OUT_OF_RANGE;
        }
    }
    size_t payload_size = 0;
    if (status == OM_STATUS_OK
        && (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                 length, &payload_size)
            || !om_checked_size_add(payload_size, 1, &payload_size))) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    if (status == OM_STATUS_OK) {
        status = allocate_object(context, &RUNTIME_STRING_TYPE, payload_size,
                                 0, &object);
    }
    if (status == OM_STATUS_OK) {
        list_object = find_object(context, roots[0]);
        source = object_payload(list_object);
        (void)om_runtime_string_view(context, roots[1], &separator);
        Om_Runtime_String_Body *destination = object_payload(object);
        size_t offset = 0;
        for (size_t index = 0; index < source->length; ++index) {
            Om_String_View item = {0};
            status = om_runtime_string_view(context,
                source->items[index].managed, &item);
            if (status != OM_STATUS_OK) {
                break;
            }
            if (item.length != 0) {
                (void)memcpy(destination->bytes + offset, item.data,
                             item.length);
                offset += item.length;
            }
            if (index + 1 < source->length && separator.length != 0) {
                (void)memcpy(destination->bytes + offset, separator.data,
                             separator.length);
                offset += separator.length;
            }
        }
        if (status == OM_STATUS_OK) {
            destination->length = length;
            destination->bytes[length] = '\0';
            *result = object;
        }
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

static size_t find_utf8_bytes(const char *value,
                              size_t value_length,
                              const char *needle,
                              size_t needle_length,
                              size_t start)
{
    if (start > value_length) {
        return SIZE_MAX;
    }
    if (needle_length == 0) {
        return start;
    }
    if (needle_length > value_length - start) {
        return SIZE_MAX;
    }
    const size_t last = value_length - needle_length;
    for (size_t index = start; index <= last; ++index) {
        if (value[index] == needle[0]
            && memcmp(value + index, needle, needle_length) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

Om_Status om_runtime_string_replace(Om_Execution_Context *context,
                                    const void *value_pointer,
                                    const void *needle_pointer,
                                    const void *replacement_pointer,
                                    void **result)
{
    if (context == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *roots[3] = {
        (void *)value_pointer, (void *)needle_pointer,
        (void *)replacement_pointer,
    };
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 3);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }

    Om_String_View input = {0};
    Om_String_View needle = {0};
    Om_String_View replacement = {0};
    Om_Status status = om_runtime_string_view(context, roots[0], &input);
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, roots[1], &needle);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, roots[2], &replacement);
    }

    size_t match_count = 0;
    if (status == OM_STATUS_OK && needle.length != 0) {
        size_t cursor = 0;
        while (status == OM_STATUS_OK) {
            const size_t match = find_utf8_bytes(input.data, input.length,
                needle.data, needle.length, cursor);
            if (match == SIZE_MAX) {
                break;
            }
            if (match_count == SIZE_MAX) {
                status = OM_STATUS_OUT_OF_RANGE;
                break;
            }
            ++match_count;
            cursor = match + needle.length;
        }
    }

    size_t output_length = input.length;
    if (status == OM_STATUS_OK && replacement.length >= needle.length) {
        size_t added = 0;
        if (!om_checked_size_mul(match_count,
                replacement.length - needle.length, &added)
            || !om_checked_size_add(input.length, added, &output_length)) {
            status = OM_STATUS_OUT_OF_RANGE;
        }
    } else if (status == OM_STATUS_OK) {
        size_t removed = 0;
        if (!om_checked_size_mul(match_count,
                needle.length - replacement.length, &removed)
            || removed > input.length) {
            status = OM_STATUS_OUT_OF_RANGE;
        } else {
            output_length = input.length - removed;
        }
    }

    size_t payload_size = 0;
    if (status == OM_STATUS_OK
        && (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                 output_length, &payload_size)
            || !om_checked_size_add(payload_size, 1, &payload_size))) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    if (status == OM_STATUS_OK) {
        status = allocate_object(context, &RUNTIME_STRING_TYPE, payload_size,
                                 0, &object);
    }
    if (status == OM_STATUS_OK) {
        (void)om_runtime_string_view(context, roots[0], &input);
        (void)om_runtime_string_view(context, roots[1], &needle);
        (void)om_runtime_string_view(context, roots[2], &replacement);
        Om_Runtime_String_Body *destination = object_payload(object);
        size_t cursor = 0;
        size_t output_offset = 0;
        while (cursor < input.length && needle.length != 0) {
            const size_t match = find_utf8_bytes(input.data, input.length,
                needle.data, needle.length, cursor);
            if (match == SIZE_MAX) {
                break;
            }
            const size_t prefix_length = match - cursor;
            if (prefix_length != 0) {
                (void)memcpy(destination->bytes + output_offset,
                    input.data + cursor, prefix_length);
                output_offset += prefix_length;
            }
            if (replacement.length != 0) {
                (void)memcpy(destination->bytes + output_offset,
                    replacement.data, replacement.length);
                output_offset += replacement.length;
            }
            cursor = match + needle.length;
        }
        const size_t suffix_length = input.length - cursor;
        if (suffix_length != 0) {
            (void)memcpy(destination->bytes + output_offset,
                input.data + cursor, suffix_length);
            output_offset += suffix_length;
        }
        if (output_offset != output_length) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            destination->length = output_length;
            destination->bytes[output_length] = '\0';
            *result = object;
        }
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_string_utf8_bytes(
    Om_Execution_Context *context,
    const void *value_pointer,
    const Om_Runtime_Type_Descriptor *list_type,
    void **result)
{
    if (context == NULL || list_type == NULL || result == NULL
        || list_type->kind != OM_RUNTIME_TYPE_LIST
        || list_type->element_type == NULL
        || list_type->element_type->kind != OM_RUNTIME_TYPE_INT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *roots[2] = {(void *)value_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_String_View input = {0};
    Om_Status status = om_runtime_string_view(context, roots[0], &input);
    if (status == OM_STATUS_OK && input.length > (size_t)INT64_MAX) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_byte_builder_create(context, list_type,
                                                     &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_reserve(context, roots[1],
            (int64_t)input.length, (int64_t)input.length);
    }
    if (status == OM_STATUS_OK) {
        (void)om_runtime_string_view(context, roots[0], &input);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < input.length; ++index) {
            status = om_runtime_list_byte_builder_append(context, roots[1],
                (unsigned char)input.data[index]);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

static int hex_nibble(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return byte - (uint8_t)'0';
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return byte - (uint8_t)'a' + 10;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return byte - (uint8_t)'A' + 10;
    }
    return -1;
}

Om_Status om_runtime_string_hex_decode_bytes(
    Om_Execution_Context *context,
    const void *value_pointer,
    const Om_Runtime_Type_Descriptor *list_type,
    void **result)
{
    if (context == NULL || list_type == NULL || result == NULL
        || list_type->kind != OM_RUNTIME_TYPE_LIST
        || list_type->element_type == NULL
        || list_type->element_type->kind != OM_RUNTIME_TYPE_INT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *roots[2] = {(void *)value_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_String_View input = {0};
    Om_Status status = om_runtime_string_view(context, roots[0], &input);
    if (status == OM_STATUS_OK
        && ((input.length & 1U) != 0
            || input.length / 2 > (size_t)INT64_MAX)) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < input.length; ++index) {
            if (hex_nibble((uint8_t)input.data[index]) < 0) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_byte_builder_create(context, list_type,
                                                     &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_reserve(context, roots[1],
            (int64_t)(input.length / 2), (int64_t)(input.length / 2));
    }
    if (status == OM_STATUS_OK) {
        (void)om_runtime_string_view(context, roots[0], &input);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < input.length; index += 2) {
            const int high = hex_nibble((uint8_t)input.data[index]);
            const int low = hex_nibble((uint8_t)input.data[index + 1]);
            status = om_runtime_list_byte_builder_append(context, roots[1],
                high * 16 + low);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_string_hex_encode_bytes(
    Om_Execution_Context *context,
    const void *list_pointer,
    void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || result == NULL || list_object == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || list_object->fields.type->element_type == NULL
        || list_object->fields.type->element_type->kind != OM_RUNTIME_TYPE_INT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_List_Body *list = object_payload_const(list_object);
    if (list->transient || list->length > SIZE_MAX / 2) {
        return list->transient ? OM_STATUS_INVALID_ARGUMENT
            : OM_STATUS_OUT_OF_RANGE;
    }
    void *roots[2] = {(void *)list_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const size_t output_capacity = list->length * 2;
    Om_Runtime_String_Builder *builder = NULL;
    Om_Status status = om_runtime_string_builder_create(output_capacity,
                                                         &builder);
    static const uint8_t HEX[] = "0123456789abcdef";
    for (size_t index = 0; status == OM_STATUS_OK && index < list->length;
         ++index) {
        Om_Runtime_Value value = {0};
        status = om_runtime_list_get(context, roots[0], index, &value);
        if (status == OM_STATUS_OK
            && (value.integer < 0 || value.integer > UINT8_MAX)) {
            status = OM_STATUS_OUT_OF_RANGE;
        }
        if (status == OM_STATUS_OK) {
            const uint8_t byte = (uint8_t)value.integer;
            status = om_runtime_string_builder_append_scalar(builder,
                HEX[byte >> 4]);
            if (status == OM_STATUS_OK) {
                status = om_runtime_string_builder_append_scalar(builder,
                    HEX[byte & 0x0f]);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_builder_seal(context, builder, &roots[1]);
        builder = NULL;
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    om_runtime_string_builder_destroy(builder);
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

static Om_Status append_string_split_part(Om_Execution_Context *context,
                                          void *builder,
                                          const char *bytes,
                                          size_t length,
                                          void **segment_root)
{
    Om_Status status = om_runtime_string_create_utf8_default(context,
        (const uint8_t *)bytes, length, segment_root);
    if (status == OM_STATUS_OK) {
        const Om_Runtime_Value value = {.managed = *segment_root};
        status = om_runtime_list_builder_append_value(context, builder,
                                                       &value);
    }
    if (status == OM_STATUS_OK) {
        *segment_root = NULL;
    }
    return status;
}

Om_Status om_runtime_string_split_list(
    Om_Execution_Context *context,
    const void *value_pointer,
    const void *separator_pointer,
    const Om_Runtime_Type_Descriptor *list_type,
    void **result)
{
    if (context == NULL || list_type == NULL || result == NULL
        || list_type->kind != OM_RUNTIME_TYPE_LIST
        || list_type->element_type == NULL
        || list_type->element_type->kind != OM_RUNTIME_TYPE_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_String_View input = {0};
    Om_String_View separator = {0};
    Om_Status status = om_runtime_string_view(context, value_pointer, &input);
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, separator_pointer,
                                        &separator);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    void *roots[4] = {(void *)value_pointer, (void *)separator_pointer,
                      NULL, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 4);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t part_count = 0;
    if (separator.length == 0) {
        size_t offset = 0;
        while (status == OM_STATUS_OK && offset < input.length) {
            uint32_t codepoint = 0;
            status = om_utf8_decode_next(
                (Om_Const_Byte_Slice){(const uint8_t *)input.data,
                                      input.length},
                &offset, &codepoint);
            if (status == OM_STATUS_OK) {
                if (part_count == SIZE_MAX) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    ++part_count;
                }
            }
        }
    } else {
        part_count = 1;
        size_t cursor = 0;
        while (status == OM_STATUS_OK) {
            const size_t match = find_utf8_bytes(input.data, input.length,
                separator.data, separator.length, cursor);
            if (match == SIZE_MAX) {
                break;
            }
            if (part_count == SIZE_MAX) {
                status = OM_STATUS_OUT_OF_RANGE;
            } else {
                ++part_count;
                cursor = match + separator.length;
            }
        }
    }
    if (part_count > (size_t)INT64_MAX) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_create(context, list_type, &roots[2]);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_reserve(context, roots[2],
            (int64_t)part_count, (int64_t)part_count);
    }
    if (status == OM_STATUS_OK) {
        (void)om_runtime_string_view(context, roots[0], &input);
        (void)om_runtime_string_view(context, roots[1], &separator);
        if (separator.length == 0) {
            size_t offset = 0;
            while (status == OM_STATUS_OK && offset < input.length) {
                const size_t start = offset;
                uint32_t codepoint = 0;
                status = om_utf8_decode_next(
                    (Om_Const_Byte_Slice){(const uint8_t *)input.data,
                                          input.length},
                    &offset, &codepoint);
                if (status == OM_STATUS_OK) {
                    status = append_string_split_part(context, roots[2],
                        input.data + start, offset - start, &roots[3]);
                }
            }
        } else {
            size_t start = 0;
            size_t cursor = 0;
            while (status == OM_STATUS_OK) {
                const size_t match = find_utf8_bytes(input.data,
                    input.length, separator.data, separator.length, cursor);
                if (match == SIZE_MAX) {
                    break;
                }
                status = append_string_split_part(context, roots[2],
                    input.data + start, match - start, &roots[3]);
                start = match + separator.length;
                cursor = start;
            }
            if (status == OM_STATUS_OK) {
                status = append_string_split_part(context, roots[2],
                    input.data + start, input.length - start, &roots[3]);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[2], &roots[2]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[2];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_string_format_value(Om_Execution_Context *context,
                                         Om_Runtime_Type_Kind kind,
                                         const Om_Runtime_Value *value,
                                         void **result)
{
    if (context == NULL || value == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    static const uint8_t NULL_TEXT[] = "null";
    static const uint8_t UNIT_TEXT[] = "()";
    static const uint8_t TRUE_TEXT[] = "true";
    static const uint8_t FALSE_TEXT[] = "false";
    const uint8_t *bytes = NULL;
    size_t length = 0;
    char buffer[64];
    int written = 0;
    switch (kind) {
    case OM_RUNTIME_TYPE_UNIT:
        bytes = UNIT_TEXT;
        length = sizeof(UNIT_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_NULL:
        bytes = NULL_TEXT;
        length = sizeof(NULL_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_BOOL:
        bytes = value->boolean ? TRUE_TEXT : FALSE_TEXT;
        length = value->boolean ? sizeof(TRUE_TEXT) - 1
                                : sizeof(FALSE_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_INT:
        written = snprintf(buffer, sizeof(buffer), "%lld",
                           (long long)value->integer);
        break;
    case OM_RUNTIME_TYPE_FLOAT:
        written = snprintf(buffer, sizeof(buffer), "%.17g", value->floating);
        break;
    case OM_RUNTIME_TYPE_STRING: {
        Om_String_View view = {0};
        const Om_Status status = om_runtime_string_view(context,
            value->managed, &view);
        if (status != OM_STATUS_OK) {
            return status;
        }
        bytes = (const uint8_t *)view.data;
        length = view.length;
        break;
    }
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (kind == OM_RUNTIME_TYPE_INT || kind == OM_RUNTIME_TYPE_FLOAT) {
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        bytes = (const uint8_t *)buffer;
        length = (size_t)written;
    }
    return om_runtime_string_create_utf8_default(context, bytes, length,
                                                  result);
}

Om_Status om_runtime_string_view(const Om_Execution_Context *context,
                                 const void *string,
                                 Om_String_View *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, string);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_String_Body *body = object_payload_const(object);
    *result = (Om_String_View){body->bytes, body->length};
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_compare(const Om_Execution_Context *context,
                                    const void *left,
                                    const void *right,
                                    int32_t *result)
{
    if (result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = 0;
    Om_String_View left_view = {0};
    Om_String_View right_view = {0};
    Om_Status status = om_runtime_string_view(context, left, &left_view);
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, right, &right_view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }

    const size_t common_length = left_view.length < right_view.length
        ? left_view.length : right_view.length;
    const int byte_order = common_length == 0 ? 0
        : memcmp(left_view.data, right_view.data, common_length);
    if (byte_order < 0) {
        *result = -1;
    } else if (byte_order > 0) {
        *result = 1;
    } else if (left_view.length < right_view.length) {
        *result = -1;
    } else if (left_view.length > right_view.length) {
        *result = 1;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_cstring_view(const Om_Execution_Context *context,
                                         const void *string,
                                         const uint8_t **bytes,
                                         size_t *length)
{
    if (bytes == NULL || length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = NULL;
    *length = 0;
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.length != 0 && memchr(view.data, '\0', view.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = (const uint8_t *)view.data;
    *length = view.length;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_create_from_cstring(
    Om_Execution_Context *context,
    const char *cstring,
    void **result)
{
    if (context == NULL || cstring == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t length = strlen(cstring);
    return om_runtime_string_create_utf8_default(context,
        (const uint8_t *)cstring, length, result);
}

Om_Status om_runtime_string_create_from_owned_cstring(
    Om_Execution_Context *context,
    char *cstring,
    Om_Runtime_CString_Release release,
    void **result)
{
    if (release == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (cstring == NULL) {
        if (result != NULL) {
            *result = NULL;
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result == NULL) {
        release(cstring);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Status status = om_runtime_string_create_from_cstring(
        context, cstring, result);
    release(cstring);
    return status;
}

Om_Status om_runtime_string_create_from_owned_utf8_slice(
    Om_Execution_Context *context,
    Om_String_View *slice,
    Om_Runtime_CString_Release release,
    void **result)
{
    if (release == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (slice == NULL) {
        if (result != NULL) *result = NULL;
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result == NULL) {
        release(slice);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Status status = om_runtime_string_create_utf8_default(
        context, (const uint8_t *)slice->data, slice->length, result);
    release(slice);
    return status;
}

Om_Status om_runtime_print(Om_Execution_Context *context,
                           const void *string)
{
    if (context == NULL || find_object(context, string) == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if ((view.length != 0 && fwrite(view.data, 1, view.length, stdout) != view.length)
        || fputc('\n', stdout) == EOF) {
        return OM_STATUS_IO_ERROR;
    }
    return OM_STATUS_OK;
}

