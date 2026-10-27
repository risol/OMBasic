#include "ombasic/source/import_path.h"

#include <stdbool.h>
#include <stddef.h>

static bool is_lower(unsigned char byte)
{
    return byte >= (unsigned char)'a' && byte <= (unsigned char)'z';
}

static bool is_digit(unsigned char byte)
{
    return byte >= (unsigned char)'0' && byte <= (unsigned char)'9';
}

static bool is_ordinary_name(const char *bytes, size_t length)
{
    size_t index = 0;
    if (length == 0 || !is_lower((unsigned char)bytes[0])) {
        return false;
    }
    index = 1;
    while (index < length) {
        const unsigned char byte = (unsigned char)bytes[index];
        if (is_lower(byte) || is_digit(byte)) {
            ++index;
        } else if (byte == (unsigned char)'_') {
            if (index + 1 >= length || !is_lower((unsigned char)bytes[index + 1])) {
                return false;
            }
            index += 2;
        } else {
            return false;
        }
    }
    return true;
}

Om_Status om_source_path_validate(Om_String_View path,
                                  Om_String_View required_extension)
{
    size_t segment_start = 0;
    if (!om_string_view_is_valid(path) || path.length == 0
        || !om_string_view_is_valid(required_extension)
        || required_extension.length < 2
        || required_extension.data[0] != '.'
        || path.data[0] == '/' || path.data[0] == '\\') {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    for (size_t index = 0; index <= path.length; ++index) {
        const bool at_end = index == path.length;
        if (!at_end && path.data[index] != '/') {
            continue;
        }
        const size_t segment_length = index - segment_start;
        if (segment_length == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (at_end) {
            if (segment_length <= required_extension.length) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            const size_t stem_length =
                segment_length - required_extension.length;
            const size_t extension_start = segment_start + stem_length;
            for (size_t extension_index = 0;
                 extension_index < required_extension.length;
                 ++extension_index) {
                if (path.data[extension_start + extension_index]
                    != required_extension.data[extension_index]) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
            }
            if (!is_ordinary_name(path.data + segment_start, stem_length)) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        } else if (!is_ordinary_name(path.data + segment_start, segment_length)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        segment_start = index + 1;
    }
    return OM_STATUS_OK;
}

Om_Status om_import_path_validate(Om_String_View path)
{
    return om_source_path_validate(path, (Om_String_View){".bas", 4});
}
