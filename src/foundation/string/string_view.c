#include "ombasic/foundation/string_view.h"

#include <string.h>

bool om_string_view_is_valid(Om_String_View view)
{
    return view.data != NULL || view.length == 0;
}

bool om_string_view_equal(Om_String_View left, Om_String_View right)
{
    if (left.length != right.length || !om_string_view_is_valid(left)
        || !om_string_view_is_valid(right)) {
        return false;
    }
    if (left.length == 0) {
        return true;
    }
    return memcmp(left.data, right.data, left.length) == 0;
}
