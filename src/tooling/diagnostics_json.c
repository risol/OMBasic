#include "ombasic/tooling/diagnostics_json.h"

#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

typedef struct Om_Json_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Json_Buffer;

static Om_Status reserve(Om_Json_Buffer *buffer, size_t extra)
{
    if (buffer == NULL || extra > SIZE_MAX - buffer->length - 1) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
    if (data == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append(Om_Json_Buffer *buffer, const char *data, size_t length)
{
    if (length != 0 && data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = reserve(buffer, length);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (length != 0) {
        (void)memcpy(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status append_literal(Om_Json_Buffer *buffer, const char *literal)
{
    return append(buffer, literal, strlen(literal));
}

static Om_Status append_uint(Om_Json_Buffer *buffer, uint32_t value)
{
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    Om_Status status = reserve(buffer, count);
    while (status == OM_STATUS_OK && count != 0) {
        status = append(buffer, &digits[--count], 1);
    }
    return status;
}

static Om_Status append_string(Om_Json_Buffer *buffer, Om_String_View value)
{
    static const char hex[] = "0123456789abcdef";
    Om_Status status = append_literal(buffer, "\"");
    for (size_t index = 0; status == OM_STATUS_OK && index < value.length; ++index) {
        const unsigned char byte = (unsigned char)value.data[index];
        if (byte == '"' || byte == '\\') {
            const char escaped[2] = {'\\', (char)byte};
            status = append(buffer, escaped, sizeof(escaped));
        } else if (byte == '\b') {
            status = append_literal(buffer, "\\b");
        } else if (byte == '\f') {
            status = append_literal(buffer, "\\f");
        } else if (byte == '\n') {
            status = append_literal(buffer, "\\n");
        } else if (byte == '\r') {
            status = append_literal(buffer, "\\r");
        } else if (byte == '\t') {
            status = append_literal(buffer, "\\t");
        } else if (byte < UINT8_C(0x20)) {
            const char escaped[6] = {
                '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0f]
            };
            status = append(buffer, escaped, sizeof(escaped));
        } else {
            status = append(buffer, (const char *)&value.data[index], 1);
        }
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, "\"");
    }
    return status;
}

static Om_Status append_location(Om_Json_Buffer *buffer,
                                 const Om_Source_Text *source,
                                 uint32_t offset)
{
    Om_Source_Location location = {0};
    Om_Status status = om_source_text_location(source, offset, &location);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = append_literal(buffer, "{\"line\":");
    if (status == OM_STATUS_OK) {
        status = append_uint(buffer, location.line);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"byteColumn\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_uint(buffer, location.byte_column);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, "}");
    }
    return status;
}

static Om_Status append_range(Om_Json_Buffer *buffer,
                              const Om_Source_Text *source,
                              Om_Source_Span span)
{
    if (source == NULL || span.source.value != source->id.value
        || span.end < span.start || span.end > source->bytes.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = append_literal(buffer, "{\"start\":");
    if (status == OM_STATUS_OK) {
        status = append_location(buffer, source, span.start);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"end\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_location(buffer, source, span.end);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, "}");
    }
    return status;
}

static Om_Status append_diagnostic(Om_Json_Buffer *buffer,
                                   const Om_Tooling_Diagnostic *diagnostic)
{
    if (diagnostic == NULL || diagnostic->source == NULL
        || !om_string_view_is_valid(diagnostic->stage)
        || !om_string_view_is_valid(diagnostic->code)
        || !om_string_view_is_valid(diagnostic->message)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = append_literal(buffer, "{\"stage\":");
    if (status == OM_STATUS_OK) {
        status = append_string(buffer, diagnostic->stage);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"code\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_string(buffer, diagnostic->code);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"severity\":\"error\",\"message\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_string(buffer, diagnostic->message);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"path\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_string(buffer, diagnostic->source->canonical_path);
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, ",\"range\":");
    }
    if (status == OM_STATUS_OK) {
        status = append_range(buffer, diagnostic->source, diagnostic->span);
    }
    if (status == OM_STATUS_OK && diagnostic->has_related_span) {
        if (diagnostic->related_source == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = append_literal(buffer, ",\"related\":{\"path\":");
        if (status == OM_STATUS_OK) {
            status = append_string(buffer, diagnostic->related_source->canonical_path);
        }
        if (status == OM_STATUS_OK) {
            status = append_literal(buffer, ",\"range\":");
        }
        if (status == OM_STATUS_OK) {
            status = append_range(buffer, diagnostic->related_source,
                                  diagnostic->related_span);
        }
        if (status == OM_STATUS_OK) {
            status = append_literal(buffer, "}");
        }
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(buffer, "}");
    }
    return status;
}

Om_Status om_tooling_diagnostics_to_json(
    const Om_Tooling_Diagnostic *diagnostics,
    size_t diagnostic_count,
    Om_Tooling_Json_Result *result
)
{
    Om_Json_Buffer buffer = {0};
    Om_Status status = OM_STATUS_OK;
    if (result == NULL || result->data != NULL
        || (diagnostics == NULL && diagnostic_count != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = append_literal(&buffer, "{\"diagnostics\":[");
    for (size_t index = 0; status == OM_STATUS_OK && index < diagnostic_count; ++index) {
        if (index != 0) {
            status = append_literal(&buffer, ",");
        }
        if (status == OM_STATUS_OK) {
            status = append_diagnostic(&buffer, &diagnostics[index]);
        }
    }
    if (status == OM_STATUS_OK) {
        status = append_literal(&buffer, "]}");
    }
    if (status != OM_STATUS_OK) {
        om_free(buffer.data);
        return status;
    }
    result->data = buffer.data;
    result->length = buffer.length;
    return OM_STATUS_OK;
}

void om_tooling_json_result_destroy(Om_Tooling_Json_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->data);
    *result = (Om_Tooling_Json_Result){0};
}
