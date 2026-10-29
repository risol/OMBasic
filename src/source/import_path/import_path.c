#include "ombasic/source/import_path.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

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

Om_Status om_import_path_resolve(Om_String_View importer_canonical_path,
                                 Om_String_View import_literal,
                                 Om_Resolved_Import_Path *out)
{
    if (out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *out = (Om_Resolved_Import_Path){0};
    if (om_import_path_validate(importer_canonical_path) != OM_STATUS_OK
        || !om_string_view_is_valid(import_literal)
        || import_literal.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    const bool module_relative = import_literal.length >= 2
        && import_literal.data[0] == '.' && import_literal.data[1] == '/';
    Om_String_View normalized_literal = import_literal;
    size_t directory_length = 0;
    if (module_relative) {
        normalized_literal.data += 2;
        normalized_literal.length -= 2;
        for (size_t index = 0; index < importer_canonical_path.length; ++index) {
            if (importer_canonical_path.data[index] == '/') {
                directory_length = index + 1;
            }
        }
    }
    if (om_import_path_validate(normalized_literal) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    size_t resolved_length = normalized_literal.length;
    size_t allocation_size = 0;
    if (module_relative
        && !om_checked_size_add(directory_length, normalized_literal.length,
                                &resolved_length)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (!om_checked_size_add(resolved_length, 1, &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    char *resolved = om_alloc(allocation_size);
    if (resolved == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (module_relative && directory_length != 0) {
        (void)memcpy(resolved, importer_canonical_path.data, directory_length);
    }
    (void)memcpy(resolved + (module_relative ? directory_length : 0),
                 normalized_literal.data, normalized_literal.length);
    resolved[resolved_length] = '\0';
    *out = (Om_Resolved_Import_Path){resolved, resolved_length};
    return OM_STATUS_OK;
}

void om_resolved_import_path_destroy(Om_Resolved_Import_Path *path)
{
    if (path == NULL) {
        return;
    }
    om_free(path->data);
    *path = (Om_Resolved_Import_Path){0};
}
