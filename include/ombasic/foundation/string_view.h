#ifndef OMBASIC_FOUNDATION_STRING_VIEW_H
#define OMBASIC_FOUNDATION_STRING_VIEW_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Om_String_View {
    /* Borrowed UTF-8 bytes; length is in bytes and data need not be NUL-terminated. */
    const char *data;
    size_t length;
} Om_String_View;

bool om_string_view_equal(Om_String_View left, Om_String_View right);
bool om_string_view_is_valid(Om_String_View view);

#endif
