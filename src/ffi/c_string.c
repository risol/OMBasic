#include "ombasic/ffi/c_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

Om_Status om_native_cstring_from_utf8(Om_String_View source,
                                      size_t maximum_byte_length,
                                      Om_Native_C_String *result)
{
    if (result == NULL || (source.data == NULL && source.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result->data != NULL || result->length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Native_C_String){0};
    if (source.length > maximum_byte_length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Const_Byte_Slice source_bytes = {
        (const uint8_t *)source.data, source.length
    };
    if (om_utf8_validate(source_bytes) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_UTF8;
    }
    if (source.length != 0 && memchr(source.data, '\0', source.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(source.length, 1, &allocation_size)) {
        return OM_STATUS_OVERFLOW;
    }
    char *copy = om_alloc(allocation_size);
    if (copy == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (source.length != 0) {
        (void)memcpy(copy, source.data, source.length);
    }
    copy[source.length] = '\0';
    *result = (Om_Native_C_String){copy, source.length};
    return OM_STATUS_OK;
}

Om_Status om_native_utf8_from_cstring(const char *source,
                                      size_t maximum_byte_length,
                                      Om_Native_String *result)
{
    if (source == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result->data != NULL || result->length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Native_String){0};
    size_t length = 0;
    while (length <= maximum_byte_length && source[length] != '\0') {
        if (length == SIZE_MAX) {
            return OM_STATUS_OVERFLOW;
        }
        ++length;
    }
    if (length > maximum_byte_length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Const_Byte_Slice source_bytes = {
        (const uint8_t *)source, length
    };
    if (om_utf8_validate(source_bytes) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_UTF8;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(length, 1, &allocation_size)) {
        return OM_STATUS_OVERFLOW;
    }
    char *copy = om_alloc(allocation_size);
    if (copy == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (length != 0) {
        (void)memcpy(copy, source, length);
    }
    copy[length] = '\0';
    *result = (Om_Native_String){copy, length};
    return OM_STATUS_OK;
}

void om_native_cstring_destroy(Om_Native_C_String *string)
{
    if (string == NULL) {
        return;
    }
    om_free(string->data);
    *string = (Om_Native_C_String){0};
}

void om_native_string_destroy(Om_Native_String *string)
{
    if (string == NULL) {
        return;
    }
    om_free(string->data);
    *string = (Om_Native_String){0};
}

Om_Status om_native_owned_pointer_release(
    const Om_Native_Library *library,
    Om_String_View release_symbol,
    void *pointer)
{
    void *address = NULL;
    const Om_Status status = om_native_library_lookup(
        library, release_symbol, &address);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (address == NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    typedef void (*Om_Native_Release_Function)(void *);
    Om_Native_Release_Function release = NULL;
    if (sizeof(release) != sizeof(address)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memcpy(&release, &address, sizeof(release));
    release(pointer);
    return OM_STATUS_OK;
}
