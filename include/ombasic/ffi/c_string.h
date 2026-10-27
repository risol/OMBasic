#ifndef OMBASIC_FFI_C_STRING_H
#define OMBASIC_FFI_C_STRING_H

#include <stddef.h>

#include "ombasic/ffi/dynamic_library.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

typedef struct Om_Native_C_String {
    char *data;
    size_t length;
} Om_Native_C_String;

typedef struct Om_Native_String {
    char *data;
    size_t length;
} Om_Native_String;

/* Result must be zero-initialized and destroyed before it is reused. */
/* Makes a NUL-terminated copy of an OMBasic UTF-8 string for a C call. */
Om_Status om_native_cstring_from_utf8(Om_String_View source,
                                      size_t maximum_byte_length,
                                      Om_Native_C_String *result);

/* Result must be zero-initialized and destroyed before it is reused. */
/* Copies a bounded, NUL-terminated C string into owned UTF-8 storage. */
Om_Status om_native_utf8_from_cstring(const char *source,
                                      size_t maximum_byte_length,
                                      Om_Native_String *result);

void om_native_cstring_destroy(Om_Native_C_String *string);
void om_native_string_destroy(Om_Native_String *string);

/* Calls an .omi-validated release function with the owned pointer. */
Om_Status om_native_owned_pointer_release(
    const Om_Native_Library *library,
    Om_String_View release_symbol,
    void *pointer);

#endif
