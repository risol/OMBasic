#ifndef OMBASIC_FFI_DYNAMIC_LIBRARY_H
#define OMBASIC_FFI_DYNAMIC_LIBRARY_H

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

typedef struct Om_Native_Library {
    void *handle;
} Om_Native_Library;

/* Paths are UTF-8. The handle must be zero-initialized and closed once. */
Om_Status om_native_library_open(Om_Native_Library *library,
                                 Om_String_View path);
Om_Status om_native_library_lookup(const Om_Native_Library *library,
                                   Om_String_View symbol_name,
                                   void **symbol_address);
void om_native_library_close(Om_Native_Library *library);

#endif
