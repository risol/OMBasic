#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

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

