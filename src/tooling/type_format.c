#include "ombasic/tooling/type_format.h"

#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Type_Format_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Type_Format_Buffer;

static Om_Status reserve(Om_Type_Format_Buffer *buffer, size_t extra)
{
    if (buffer == NULL || extra > SIZE_MAX - buffer->length - 1) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
    if (data == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append(Om_Type_Format_Buffer *buffer, const char *data,
                        size_t length)
{
    if (length != 0 && data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = reserve(buffer, length);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (length != 0) {
        (void)memcpy(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append_literal(Om_Type_Format_Buffer *buffer, const char *text)
{
    return append(buffer, text, strlen(text));
}

static Om_Status append_u32(Om_Type_Format_Buffer *buffer, uint32_t value)
{
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    Om_Status status = reserve(buffer, count);
    while (status == OM_STATUS_OK && count != 0) {
        status = append(buffer, &digits[--count], 1);
    }
    return status;
}

static Om_Status format_type(Om_Type_Format_Buffer *buffer,
                             const Om_Type_Store *store,
                             const Om_Resolution_Result *resolution,
                             Om_Type_Id type_id,
                             size_t depth)
{
    if (depth >= 128) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = om_type_store_resolve(store, type_id, &resolved);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved, &view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.kind == OM_TYPE_VARIABLE) {
        status = append_literal(buffer, "T");
        if (status == OM_STATUS_OK) {
            status = append_u32(buffer, resolved.value);
        }
        return status;
    }
    if (view.nominal_symbol.value != UINT32_MAX) {
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            resolution, view.nominal_symbol);
        if (symbol == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return append(buffer, symbol->name.data, symbol->name.length);
    }
    if (view.kind == OM_TYPE_LIST || view.kind == OM_TYPE_ACTOR_REF) {
        status = append_literal(buffer, view.kind == OM_TYPE_LIST ? "List<" : "ActorRef<");
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(store, resolved, 0, &child);
        }
        if (status == OM_STATUS_OK) {
            status = format_type(buffer, store, resolution, child, depth + 1);
        }
        if (status == OM_STATUS_OK) {
            status = append_literal(buffer, ">");
        }
        return status;
    }
    if (view.kind == OM_TYPE_MAP || view.kind == OM_TYPE_RESULT) {
        status = append_literal(buffer, view.kind == OM_TYPE_MAP ? "Map<" : "Result<");
        for (size_t index = 0; status == OM_STATUS_OK && index < view.child_count; ++index) {
            if (index != 0) {
                status = append_literal(buffer, ", ");
            }
            Om_Type_Id child = OM_TYPE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = om_type_store_child_at(store, resolved, index, &child);
            }
            if (status == OM_STATUS_OK) {
                status = format_type(buffer, store, resolution, child, depth + 1);
            }
        }
        if (status == OM_STATUS_OK) {
            status = append_literal(buffer, ">");
        }
        return status;
    }
    if (view.kind == OM_TYPE_FUNCTION || view.kind == OM_TYPE_ACTION) {
        status = append_literal(buffer,
            view.kind == OM_TYPE_FUNCTION ? "Function(" : "Action(");
        const size_t parameter_count = view.child_count == 0
            ? 0 : view.child_count - 1;
        for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count; ++index) {
            if (index != 0) {
                status = append_literal(buffer, ", ");
            }
            Om_Type_Id child = OM_TYPE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = om_type_store_child_at(store, resolved, index, &child);
            }
            if (status == OM_STATUS_OK) {
                status = format_type(buffer, store, resolution, child, depth + 1);
            }
        }
        if (status == OM_STATUS_OK) {
            status = append_literal(buffer, ") -> ");
        }
        Om_Type_Id return_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK && view.child_count != 0) {
            status = om_type_store_child_at(store, resolved, view.child_count - 1,
                                            &return_type);
        }
        if (status == OM_STATUS_OK && view.child_count != 0) {
            status = format_type(buffer, store, resolution, return_type, depth + 1);
        }
        return status;
    }
    return append_literal(buffer, om_type_kind_name(view.kind));
}

Om_Status om_tooling_type_to_string(
    const Om_Type_Store *type_store,
    const Om_Resolution_Result *resolution,
    Om_Type_Id type_id,
    Om_Tooling_String *result
)
{
    Om_Type_Format_Buffer buffer = {0};
    if (type_store == NULL || type_store->owned_storage == NULL
        || resolution == NULL || result == NULL || result->data != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = format_type(&buffer, type_store, resolution, type_id, 0);
    if (status != OM_STATUS_OK) {
        om_free(buffer.data);
        return status;
    }
    if (buffer.data == NULL) {
        buffer.data = om_alloc(1);
        if (buffer.data == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        buffer.data[0] = '\0';
    }
    result->data = buffer.data;
    result->length = buffer.length;
    return OM_STATUS_OK;
}

void om_tooling_string_destroy(Om_Tooling_String *string)
{
    if (string == NULL) {
        return;
    }
    om_free(string->data);
    *string = (Om_Tooling_String){0};
}
