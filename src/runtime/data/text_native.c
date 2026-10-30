#include "ombasic/runtime/abi/text.h"

#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/utf8.h"

static bool next_scalar(const char *value, size_t length, size_t *offset)
{
    uint32_t scalar = 0;
    return om_utf8_decode_next((Om_Const_Byte_Slice){
        (const uint8_t *)value, length}, offset, &scalar) == OM_STATUS_OK;
}

static bool scalar_offset(const char *value, size_t byte_length,
                          int64_t scalar_index, size_t *byte_offset)
{
    size_t offset = 0;
    if (scalar_index < 0 || byte_offset == NULL) {
        return false;
    }
    for (int64_t index = 0; index < scalar_index; ++index) {
        if (offset == byte_length || !next_scalar(value, byte_length, &offset)) {
            return false;
        }
    }
    *byte_offset = offset;
    return true;
}

static bool is_unicode_whitespace(uint32_t scalar)
{
    return (scalar >= UINT32_C(0x0009) && scalar <= UINT32_C(0x000d))
        || scalar == UINT32_C(0x0020) || scalar == UINT32_C(0x0085)
        || scalar == UINT32_C(0x00a0) || scalar == UINT32_C(0x1680)
        || (scalar >= UINT32_C(0x2000) && scalar <= UINT32_C(0x200a))
        || scalar == UINT32_C(0x2028) || scalar == UINT32_C(0x2029)
        || scalar == UINT32_C(0x202f) || scalar == UINT32_C(0x205f)
        || scalar == UINT32_C(0x3000);
}

char *om_runtime_text_trim(const char *value, int64_t mode)
{
    if (value == NULL || mode < 0 || mode > 2) return NULL;
    const size_t byte_length = strlen(value);
    size_t offset = 0;
    size_t first_nonspace = byte_length;
    size_t last_nonspace_end = 0;
    while (offset < byte_length) {
        const size_t scalar_start = offset;
        uint32_t scalar = 0;
        if (om_utf8_decode_next((Om_Const_Byte_Slice){
                (const uint8_t *)value, byte_length}, &offset, &scalar)
            != OM_STATUS_OK) {
            return NULL;
        }
        if (!is_unicode_whitespace(scalar)) {
            if (first_nonspace == byte_length) first_nonspace = scalar_start;
            last_nonspace_end = offset;
        }
    }
    const size_t start = mode == 2 ? 0 : first_nonspace;
    const size_t end = mode == 1 ? byte_length : last_nonspace_end;
    const size_t result_length = end >= start ? end - start : 0;
    char *result = om_alloc(result_length + 1);
    if (result == NULL) return NULL;
    if (result_length != 0) (void)memcpy(result, value + start, result_length);
    result[result_length] = '\0';
    return result;
}

int64_t om_runtime_text_length(const char *value)
{
    if (value == NULL) {
        return -1;
    }
    const size_t byte_length = strlen(value);
    size_t offset = 0;
    int64_t count = 0;
    while (offset < byte_length) {
        if (!next_scalar(value, byte_length, &offset) || count == INT64_MAX) {
            return -1;
        }
        ++count;
    }
    return count;
}

int64_t om_runtime_text_byte_length(const char *value)
{
    if (value == NULL) return -1;
    const size_t length = strlen(value);
    return length > (size_t)INT64_MAX ? -1 : (int64_t)length;
}

bool om_runtime_text_starts_with(const char *value, const char *prefix)
{
    if (value == NULL || prefix == NULL) {
        return false;
    }
    const size_t value_length = strlen(value);
    const size_t prefix_length = strlen(prefix);
    return prefix_length <= value_length
        && memcmp(value, prefix, prefix_length) == 0;
}

bool om_runtime_text_ends_with(const char *value, const char *suffix)
{
    if (value == NULL || suffix == NULL) {
        return false;
    }
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    return suffix_length <= value_length
        && memcmp(value + value_length - suffix_length, suffix,
                  suffix_length) == 0;
}

int64_t om_runtime_text_find(const char *value, const char *needle,
                             int64_t start_index)
{
    if (value == NULL || needle == NULL || start_index < 0) {
        return -1;
    }
    const size_t value_length = strlen(value);
    const size_t needle_length = strlen(needle);
    size_t start_byte = 0;
    if (!scalar_offset(value, value_length, start_index, &start_byte)) {
        return -1;
    }
    const char *match = NULL;
    if (needle_length == 0) {
        match = value + start_byte;
    } else if (needle_length <= value_length - start_byte) {
        for (size_t offset = start_byte;
             offset <= value_length - needle_length; ++offset) {
            if (memcmp(value + offset, needle, needle_length) == 0) {
                size_t scalar_count = 0;
                size_t cursor = 0;
                while (cursor < offset) {
                    if (!next_scalar(value, offset, &cursor)) {
                        return -1;
                    }
                    ++scalar_count;
                }
                if (scalar_count > (size_t)INT64_MAX) {
                    return -1;
                }
                return (int64_t)scalar_count;
            }
        }
    }
    if (match != NULL) {
        return start_index;
    }
    return -1;
}

int64_t om_runtime_text_find_last(const char *value, const char *needle)
{
    if (value == NULL || needle == NULL) {
        return -1;
    }
    const size_t value_length = strlen(value);
    const size_t needle_length = strlen(needle);
    size_t byte_offset = 0;
    int64_t scalar_index = 0;
    int64_t last_match = needle_length == 0 ? 0 : -1;
    while (byte_offset < value_length) {
        if (needle_length <= value_length - byte_offset
            && memcmp(value + byte_offset, needle, needle_length) == 0) {
            last_match = scalar_index;
        }
        if (!next_scalar(value, value_length, &byte_offset)
            || scalar_index == INT64_MAX) {
            return -1;
        }
        ++scalar_index;
    }
    if (needle_length == 0) {
        return scalar_index;
    }
    return last_match;
}

char *om_runtime_text_slice(const char *value, int64_t start_index,
                            int64_t scalar_count)
{
    if (value == NULL || scalar_count < 0) {
        return NULL;
    }
    const size_t byte_length = strlen(value);
    size_t start_byte = 0;
    size_t end_byte = 0;
    if (!scalar_offset(value, byte_length, start_index, &start_byte)
        || scalar_count > INT64_MAX - start_index
        || !scalar_offset(value, byte_length, start_index + scalar_count,
                          &end_byte)) {
        return NULL;
    }
    const size_t result_length = end_byte - start_byte;
    char *result = om_alloc(result_length + 1);
    if (result == NULL) {
        return NULL;
    }
    if (result_length != 0) {
        (void)memcpy(result, value + start_byte, result_length);
    }
    result[result_length] = '\0';
    return result;
}

void om_runtime_text_release(void *value)
{
    om_free(value);
}
