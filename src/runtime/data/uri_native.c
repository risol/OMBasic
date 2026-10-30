#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/utf8.h"

#include <stdint.h>
#include <string.h>

enum {
    URI_OK = 0,
    URI_INVALID_PERCENT = 1,
    URI_INVALID_UTF8 = 2,
    URI_RESOURCE_EXHAUSTED = 3,
    URI_INVALID_COMPONENT = 4,
    URI_MAX_BYTES = 1048576,
};

static bool has_prefix(const uint8_t *input, size_t length, size_t offset,
                       const char *prefix, size_t prefix_length)
{
    return offset <= length && prefix_length <= length - offset
        && memcmp(input + offset, prefix, prefix_length) == 0;
}

char *om_runtime_uri_remove_dot_segments(const Om_String_View *input)
{
    if (input == NULL || (input->data == NULL && input->length != 0)
        || input->length > URI_MAX_BYTES
        || input->length == SIZE_MAX) {
        return NULL;
    }
    char *output = om_alloc(input->length + 1);
    if (output == NULL) return NULL;
    const uint8_t *source = (const uint8_t *)input->data;
    size_t cursor = 0;
    size_t output_length = 0;
    while (cursor < input->length) {
        if (has_prefix(source, input->length, cursor, "../", 3)) {
            cursor += 3;
        } else if (has_prefix(source, input->length, cursor, "./", 2)) {
            cursor += 2;
        } else if (has_prefix(source, input->length, cursor, "/./", 3)) {
            cursor += 2;
        } else if (input->length - cursor == 2
                   && has_prefix(source, input->length, cursor, "/.", 2)) {
            cursor += 2;
            output[output_length++] = '/';
        } else if (has_prefix(source, input->length, cursor, "/../", 4)) {
            cursor += 3;
            while (output_length > 0 && output[output_length - 1] != '/') {
                --output_length;
            }
            if (output_length > 0) --output_length;
        } else if (input->length - cursor == 3
                   && has_prefix(source, input->length, cursor, "/..", 3)) {
            cursor += 3;
            while (output_length > 0 && output[output_length - 1] != '/') {
                --output_length;
            }
            if (output_length > 0) --output_length;
            output[output_length++] = '/';
        } else if ((input->length - cursor == 1
                    && has_prefix(source, input->length, cursor, ".", 1))
                   || (input->length - cursor == 2
                       && has_prefix(source, input->length, cursor, "..", 2))) {
            cursor = input->length;
        } else {
            size_t segment_end = cursor;
            if (source[segment_end] == '/') ++segment_end;
            while (segment_end < input->length && source[segment_end] != '/') {
                ++segment_end;
            }
            const size_t segment_length = segment_end - cursor;
            memcpy(output + output_length, source + cursor, segment_length);
            output_length += segment_length;
            cursor = segment_end;
        }
    }
    output[output_length] = '\0';
    return output;
}

enum {
    URI_COMPONENT_PATH = 1,
    URI_COMPONENT_QUERY = 2,
    URI_COMPONENT_FRAGMENT = 3,
    URI_COMPONENT_USERINFO = 4,
    URI_COMPONENT_QUERY_FIELD = 5,
};

static int hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static bool is_unreserved(uint8_t value)
{
    return (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9')
        || value == '-' || value == '.' || value == '_' || value == '~';
}

static bool is_sub_delimiter(uint8_t value)
{
    return value == '!' || value == '$' || value == '&' || value == '\''
        || value == '(' || value == ')' || value == '*' || value == '+'
        || value == ',' || value == ';' || value == '=';
}

static bool is_safe(uint8_t value, int64_t component)
{
    if (is_unreserved(value)) return true;
    if (component == URI_COMPONENT_PATH) {
        return is_sub_delimiter(value) || value == ':' || value == '@'
            || value == '/';
    }
    if (component == URI_COMPONENT_QUERY
        || component == URI_COMPONENT_FRAGMENT) {
        return is_sub_delimiter(value) || value == ':' || value == '@'
            || value == '/' || value == '?';
    }
    if (component == URI_COMPONENT_QUERY_FIELD) {
        return (is_sub_delimiter(value) && value != '&' && value != '=')
            || value == ':' || value == '@' || value == '/' || value == '?';
    }
    if (component == URI_COMPONENT_USERINFO) {
        return is_sub_delimiter(value) || value == ':';
    }
    return false;
}

int64_t om_runtime_uri_percent_encode_status(const Om_String_View *input,
                                             int64_t component)
{
    if (input == NULL || (input->data == NULL && input->length != 0)) {
        return URI_INVALID_COMPONENT;
    }
    if (component < URI_COMPONENT_PATH || component > URI_COMPONENT_QUERY_FIELD) {
        return URI_INVALID_COMPONENT;
    }
    return input->length <= URI_MAX_BYTES
        ? URI_OK : URI_RESOURCE_EXHAUSTED;
}

int64_t om_runtime_uri_ascii_status(const Om_String_View *input)
{
    if (input == NULL || (input->data == NULL && input->length != 0)) {
        return URI_INVALID_COMPONENT;
    }
    if (input->length > URI_MAX_BYTES) return URI_RESOURCE_EXHAUSTED;
    const uint8_t *bytes = (const uint8_t *)input->data;
    for (size_t index = 0; index < input->length; ++index) {
        if (bytes[index] == 0 || bytes[index] > 0x7f) {
            return URI_INVALID_COMPONENT;
        }
    }
    return URI_OK;
}

int64_t om_runtime_uri_ascii_offset(const Om_String_View *input)
{
    if (input == NULL || (input->data == NULL && input->length != 0)) return 0;
    const uint8_t *bytes = (const uint8_t *)input->data;
    for (size_t index = 0; index < input->length; ++index) {
        if (bytes[index] == 0 || bytes[index] > 0x7f) {
            return (int64_t)index;
        }
    }
    return 0;
}

int64_t om_runtime_uri_percent_syntax_status(const Om_String_View *input)
{
    if (input == NULL || (input->data == NULL && input->length != 0)) {
        return URI_INVALID_PERCENT;
    }
    if (input->length > URI_MAX_BYTES) return URI_RESOURCE_EXHAUSTED;
    const uint8_t *bytes = (const uint8_t *)input->data;
    for (size_t index = 0; index < input->length; ++index) {
        if (bytes[index] != '%') continue;
        if (index + 2 >= input->length
            || hex_digit(bytes[index + 1]) < 0
            || hex_digit(bytes[index + 2]) < 0) {
            return URI_INVALID_PERCENT;
        }
        index += 2;
    }
    return URI_OK;
}

int64_t om_runtime_uri_percent_syntax_offset(const Om_String_View *input)
{
    if (input == NULL || (input->data == NULL && input->length != 0)) return 0;
    const uint8_t *bytes = (const uint8_t *)input->data;
    for (size_t index = 0; index < input->length; ++index) {
        if (bytes[index] == '%' && (index + 2 >= input->length
            || hex_digit(bytes[index + 1]) < 0
            || hex_digit(bytes[index + 2]) < 0)) {
            return (int64_t)index;
        }
        if (bytes[index] == '%') index += 2;
    }
    return 0;
}

char *om_runtime_uri_percent_encode(const Om_String_View *input,
                                    int64_t component)
{
    static const char hex[] = "0123456789ABCDEF";
    if (om_runtime_uri_percent_encode_status(input, component) != URI_OK
        || input->length > (SIZE_MAX - 1) / 3) {
        return NULL;
    }
    const size_t capacity = input->length * 3 + 1;
    char *output = om_alloc(capacity);
    if (output == NULL) return NULL;
    const uint8_t *bytes = (const uint8_t *)input->data;
    size_t output_length = 0;
    for (size_t index = 0; index < input->length; ++index) {
        const uint8_t value = bytes[index];
        if (value < 0x80 && is_safe(value, component)) {
            output[output_length++] = (char)value;
        } else {
            output[output_length++] = '%';
            output[output_length++] = hex[value >> 4];
            output[output_length++] = hex[value & 15u];
        }
    }
    output[output_length] = '\0';
    return output;
}

static uint8_t *decode_percent(const Om_String_View *input,
                               size_t *output_length,
                               int64_t *error_offset,
                               int64_t *status)
{
    *output_length = 0;
    *error_offset = 0;
    *status = URI_OK;
    if (input == NULL || (input->data == NULL && input->length != 0)) {
        *status = URI_INVALID_PERCENT;
        return NULL;
    }
    if (input->length > URI_MAX_BYTES) {
        *status = URI_RESOURCE_EXHAUSTED;
        return NULL;
    }
    uint8_t *output = input->length == 0 ? NULL : om_alloc(input->length);
    if (input->length != 0 && output == NULL) {
        *status = URI_RESOURCE_EXHAUSTED;
        return NULL;
    }
    const uint8_t *source = (const uint8_t *)input->data;
    size_t source_index = 0;
    size_t destination_index = 0;
    while (source_index < input->length) {
        if (source[source_index] != '%') {
            output[destination_index++] = source[source_index++];
            continue;
        }
        if (source_index + 2 >= input->length) {
            *status = URI_INVALID_PERCENT;
            *error_offset = (int64_t)source_index;
            om_free(output);
            return NULL;
        }
        const int high = hex_digit(source[source_index + 1]);
        const int low = hex_digit(source[source_index + 2]);
        if (high < 0 || low < 0) {
            *status = URI_INVALID_PERCENT;
            *error_offset = (int64_t)source_index;
            om_free(output);
            return NULL;
        }
        output[destination_index++] = (uint8_t)(high * 16 + low);
        source_index += 3;
    }
    *output_length = destination_index;
    return output;
}

static int64_t validate_utf8(const uint8_t *bytes, size_t length,
                             int64_t *error_offset)
{
    size_t offset = 0;
    while (offset < length) {
        const size_t sequence_start = offset;
        uint32_t scalar = 0;
        if (om_utf8_decode_next((Om_Const_Byte_Slice){bytes, length},
                                &offset, &scalar) != OM_STATUS_OK) {
            *error_offset = (int64_t)sequence_start;
            return URI_INVALID_UTF8;
        }
    }
    return URI_OK;
}

static int64_t validate_percent_utf8(const Om_String_View *input,
                                     int64_t *error_offset)
{
    size_t decoded_length = 0;
    int64_t source_error_offset = 0;
    int64_t status = URI_OK;
    uint8_t *decoded = decode_percent(input, &decoded_length,
        &source_error_offset, &status);
    if (status != URI_OK) {
        *error_offset = source_error_offset;
        return status;
    }
    status = validate_utf8(decoded, decoded_length, error_offset);
    om_free(decoded);
    return status;
}

int64_t om_runtime_uri_percent_decode_status(const Om_String_View *input)
{
    int64_t error_offset = 0;
    return validate_percent_utf8(input, &error_offset);
}

int64_t om_runtime_uri_percent_decode_offset(const Om_String_View *input)
{
    int64_t error_offset = 0;
    (void)validate_percent_utf8(input, &error_offset);
    return error_offset;
}

Om_String_View *om_runtime_uri_percent_decode(const Om_String_View *input)
{
    size_t decoded_length = 0;
    int64_t error_offset = 0;
    int64_t status = URI_OK;
    uint8_t *decoded = decode_percent(input, &decoded_length,
        &error_offset, &status);
    if (status != URI_OK
        || validate_utf8(decoded, decoded_length, &error_offset) != URI_OK) {
        om_free(decoded);
        return NULL;
    }
    Om_String_View *result = om_alloc(sizeof(*result));
    if (result == NULL) {
        om_free(decoded);
        return NULL;
    }
    result->data = (const char *)decoded;
    result->length = decoded_length;
    return result;
}

void om_runtime_uri_percent_release(void *value)
{
    om_free(value);
}

void om_runtime_uri_slice_release(void *value)
{
    Om_String_View *slice = value;
    if (slice == NULL) return;
    om_free((void *)slice->data);
    om_free(slice);
}
