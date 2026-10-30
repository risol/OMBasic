#include "ombasic/runtime/abi/json.h"

#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/utf8.h"

#define JSON_MAX_BYTES ((size_t)1024 * (size_t)1024)
#define JSON_MAX_DEPTH ((size_t)128)

typedef struct Json_Parser {
    const char *source;
    size_t length;
    size_t offset;
    size_t depth;
    Om_Runtime_Json_Error error;
    size_t error_offset;
} Json_Parser;

typedef struct Json_Span {
    size_t start;
    size_t end;
    Om_Runtime_Json_Kind kind;
} Json_Span;

typedef struct Json_Key {
    char *bytes;
    size_t length;
} Json_Key;

static void parser_fail(Json_Parser *parser, Om_Runtime_Json_Error error,
                        size_t offset)
{
    if (parser->error == OM_RUNTIME_JSON_ERROR_NONE) {
        parser->error = error;
        parser->error_offset = offset;
    }
}

static void skip_whitespace(Json_Parser *parser)
{
    while (parser->offset < parser->length) {
        const char byte = parser->source[parser->offset];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return;
        }
        ++parser->offset;
    }
}

static bool append_codepoint(char *output, size_t capacity, size_t *length,
                             uint32_t codepoint)
{
    uint8_t bytes[4] = {0};
    size_t count = 0;
    if (codepoint <= UINT32_C(0x7f)) {
        bytes[0] = (uint8_t)codepoint;
        count = 1;
    } else if (codepoint <= UINT32_C(0x7ff)) {
        bytes[0] = (uint8_t)(UINT32_C(0xc0) | (codepoint >> 6));
        bytes[1] = (uint8_t)(UINT32_C(0x80) | (codepoint & 0x3f));
        count = 2;
    } else if (codepoint <= UINT32_C(0xffff)) {
        bytes[0] = (uint8_t)(UINT32_C(0xe0) | (codepoint >> 12));
        bytes[1] = (uint8_t)(UINT32_C(0x80) | ((codepoint >> 6) & 0x3f));
        bytes[2] = (uint8_t)(UINT32_C(0x80) | (codepoint & 0x3f));
        count = 3;
    } else if (codepoint <= UINT32_C(0x10ffff)) {
        bytes[0] = (uint8_t)(UINT32_C(0xf0) | (codepoint >> 18));
        bytes[1] = (uint8_t)(UINT32_C(0x80) | ((codepoint >> 12) & 0x3f));
        bytes[2] = (uint8_t)(UINT32_C(0x80) | ((codepoint >> 6) & 0x3f));
        bytes[3] = (uint8_t)(UINT32_C(0x80) | (codepoint & 0x3f));
        count = 4;
    } else {
        return false;
    }
    if (*length > capacity || count > capacity - *length) {
        return false;
    }
    (void)memcpy(output + *length, bytes, count);
    *length += count;
    return true;
}

static int hex_value(char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static bool parse_hex_quad(Json_Parser *parser, uint32_t *value)
{
    if (parser->length - parser->offset < 4) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    uint32_t result = 0;
    for (size_t index = 0; index < 4; ++index) {
        const int digit = hex_value(parser->source[parser->offset + index]);
        if (digit < 0) {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset + index);
            return false;
        }
        result = (result << 4) | (uint32_t)digit;
    }
    parser->offset += 4;
    *value = result;
    return true;
}

static bool parse_json_string(Json_Parser *parser, char **decoded,
                              size_t *decoded_length)
{
    if (parser->offset >= parser->length
        || parser->source[parser->offset] != '"') {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    ++parser->offset;
    char *output = decoded == NULL ? NULL
        : om_alloc(parser->length - parser->offset + 1);
    if (decoded != NULL && output == NULL) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE, parser->offset);
        return false;
    }
    size_t output_length = 0;
    while (parser->offset < parser->length) {
        const unsigned char byte =
            (unsigned char)parser->source[parser->offset++];
        if (byte == '"') {
            if (output != NULL) {
                output[output_length] = '\0';
                *decoded = output;
                *decoded_length = output_length;
            }
            return true;
        }
        if (byte < 0x20) {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset - 1);
            om_free(output);
            return false;
        }
        uint32_t codepoint = byte;
        if (byte == '\\') {
            if (parser->offset >= parser->length) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                            parser->offset);
                om_free(output);
                return false;
            }
            const size_t escape_offset = parser->offset - 1;
            const char escape = parser->source[parser->offset++];
            switch (escape) {
            case '"': codepoint = '"'; break;
            case '\\': codepoint = '\\'; break;
            case '/': codepoint = '/'; break;
            case 'b': codepoint = '\b'; break;
            case 'f': codepoint = '\f'; break;
            case 'n': codepoint = '\n'; break;
            case 'r': codepoint = '\r'; break;
            case 't': codepoint = '\t'; break;
            case 'u': {
                if (!parse_hex_quad(parser, &codepoint)) {
                    om_free(output);
                    return false;
                }
                if (codepoint >= UINT32_C(0xd800)
                    && codepoint <= UINT32_C(0xdbff)) {
                    if (parser->length - parser->offset < 6
                        || parser->source[parser->offset] != '\\'
                        || parser->source[parser->offset + 1] != 'u') {
                        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                                    escape_offset);
                        om_free(output);
                        return false;
                    }
                    parser->offset += 2;
                    uint32_t low = 0;
                    if (!parse_hex_quad(parser, &low)) {
                        om_free(output);
                        return false;
                    }
                    if (low < UINT32_C(0xdc00)
                        || low > UINT32_C(0xdfff)) {
                        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                                    parser->offset - 4);
                        om_free(output);
                        return false;
                    }
                    codepoint = UINT32_C(0x10000)
                        + ((codepoint - UINT32_C(0xd800)) << 10)
                        + (low - UINT32_C(0xdc00));
                } else if (codepoint >= UINT32_C(0xdc00)
                           && codepoint <= UINT32_C(0xdfff)) {
                    parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                                escape_offset);
                    om_free(output);
                    return false;
                }
                break;
            }
            default:
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                            parser->offset - 1);
                om_free(output);
                return false;
            }
            if (output != NULL
                && !append_codepoint(output, parser->length, &output_length,
                                     codepoint)) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE,
                            escape_offset);
                om_free(output);
                return false;
            }
        } else if (output != NULL) {
            if (output_length >= parser->length) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE,
                            parser->offset - 1);
                om_free(output);
                return false;
            }
            output[output_length++] = (char)byte;
        }
    }
    parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
    om_free(output);
    return false;
}

static bool parse_value(Json_Parser *parser, Json_Span *span);

static bool parse_literal(Json_Parser *parser, const char *literal,
                          size_t literal_length)
{
    if (literal_length > parser->length - parser->offset
        || memcmp(parser->source + parser->offset, literal,
                  literal_length) != 0) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    parser->offset += literal_length;
    return true;
}

static bool parse_number(Json_Parser *parser)
{
    const size_t start = parser->offset;
    if (parser->source[parser->offset] == '-') {
        ++parser->offset;
        if (parser->offset == parser->length) {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, start);
            return false;
        }
    }
    if (parser->source[parser->offset] == '0') {
        ++parser->offset;
        if (parser->offset < parser->length
            && parser->source[parser->offset] >= '0'
            && parser->source[parser->offset] <= '9') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset);
            return false;
        }
    } else if (parser->source[parser->offset] >= '1'
               && parser->source[parser->offset] <= '9') {
        do {
            ++parser->offset;
        } while (parser->offset < parser->length
                 && parser->source[parser->offset] >= '0'
                 && parser->source[parser->offset] <= '9');
    } else {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    if (parser->offset < parser->length
        && parser->source[parser->offset] == '.') {
        ++parser->offset;
        if (parser->offset == parser->length
            || parser->source[parser->offset] < '0'
            || parser->source[parser->offset] > '9') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
            return false;
        }
        do {
            ++parser->offset;
        } while (parser->offset < parser->length
                 && parser->source[parser->offset] >= '0'
                 && parser->source[parser->offset] <= '9');
    }
    if (parser->offset < parser->length
        && (parser->source[parser->offset] == 'e'
            || parser->source[parser->offset] == 'E')) {
        ++parser->offset;
        if (parser->offset < parser->length
            && (parser->source[parser->offset] == '+'
                || parser->source[parser->offset] == '-')) {
            ++parser->offset;
        }
        if (parser->offset == parser->length
            || parser->source[parser->offset] < '0'
            || parser->source[parser->offset] > '9') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
            return false;
        }
        do {
            ++parser->offset;
        } while (parser->offset < parser->length
                 && parser->source[parser->offset] >= '0'
                 && parser->source[parser->offset] <= '9');
    }
    return parser->offset > start;
}

static bool parse_array(Json_Parser *parser)
{
    if (parser->depth >= JSON_MAX_DEPTH) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_DEPTH, parser->offset);
        return false;
    }
    ++parser->depth;
    ++parser->offset;
    skip_whitespace(parser);
    if (parser->offset < parser->length
        && parser->source[parser->offset] == ']') {
        ++parser->offset;
        --parser->depth;
        return true;
    }
    for (;;) {
        Json_Span item = {0};
        if (!parse_value(parser, &item)) {
            --parser->depth;
            return false;
        }
        skip_whitespace(parser);
        if (parser->offset >= parser->length) {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset);
            --parser->depth;
            return false;
        }
        const char delimiter = parser->source[parser->offset++];
        if (delimiter == ']') {
            --parser->depth;
            return true;
        }
        if (delimiter != ',') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset - 1);
            --parser->depth;
            return false;
        }
        skip_whitespace(parser);
    }
}

static void free_keys(Json_Key *keys, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        om_free(keys[index].bytes);
    }
    om_free(keys);
}

static bool parse_object(Json_Parser *parser)
{
    if (parser->depth >= JSON_MAX_DEPTH) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_DEPTH, parser->offset);
        return false;
    }
    ++parser->depth;
    ++parser->offset;
    skip_whitespace(parser);
    if (parser->offset < parser->length
        && parser->source[parser->offset] == '}') {
        ++parser->offset;
        --parser->depth;
        return true;
    }
    Json_Key *keys = NULL;
    size_t key_count = 0;
    size_t key_capacity = 0;
    for (;;) {
        const size_t key_offset = parser->offset;
        char *key = NULL;
        size_t key_length = 0;
        if (!parse_json_string(parser, &key, &key_length)) {
            free_keys(keys, key_count);
            --parser->depth;
            return false;
        }
        for (size_t index = 0; index < key_count; ++index) {
            if (keys[index].length == key_length
                && memcmp(keys[index].bytes, key, key_length) == 0) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_DUPLICATE_KEY,
                            key_offset);
                om_free(key);
                free_keys(keys, key_count);
                --parser->depth;
                return false;
            }
        }
        if (key_count == key_capacity) {
            const size_t next_capacity = key_capacity == 0 ? 4
                : key_capacity > SIZE_MAX / 2 ? SIZE_MAX : key_capacity * 2;
            if (next_capacity == SIZE_MAX
                || next_capacity > SIZE_MAX / sizeof(*keys)) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE, key_offset);
                om_free(key);
                free_keys(keys, key_count);
                --parser->depth;
                return false;
            }
            Json_Key *next = om_realloc_array(keys, next_capacity,
                                               sizeof(*next));
            if (next == NULL) {
                parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE, key_offset);
                om_free(key);
                free_keys(keys, key_count);
                --parser->depth;
                return false;
            }
            keys = next;
            key_capacity = next_capacity;
        }
        keys[key_count++] = (Json_Key){key, key_length};
        skip_whitespace(parser);
        if (parser->offset >= parser->length
            || parser->source[parser->offset] != ':') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset);
            free_keys(keys, key_count);
            --parser->depth;
            return false;
        }
        ++parser->offset;
        skip_whitespace(parser);
        Json_Span value = {0};
        if (!parse_value(parser, &value)) {
            free_keys(keys, key_count);
            --parser->depth;
            return false;
        }
        skip_whitespace(parser);
        if (parser->offset >= parser->length) {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset);
            free_keys(keys, key_count);
            --parser->depth;
            return false;
        }
        const char delimiter = parser->source[parser->offset++];
        if (delimiter == '}') {
            free_keys(keys, key_count);
            --parser->depth;
            return true;
        }
        if (delimiter != ',') {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX,
                        parser->offset - 1);
            free_keys(keys, key_count);
            --parser->depth;
            return false;
        }
        skip_whitespace(parser);
    }
}

static bool parse_value(Json_Parser *parser, Json_Span *span)
{
    skip_whitespace(parser);
    if (parser->offset >= parser->length) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    span->start = parser->offset;
    const char first = parser->source[parser->offset];
    bool valid = false;
    switch (first) {
    case 'n':
        valid = parse_literal(parser, "null", 4);
        span->kind = OM_RUNTIME_JSON_KIND_NULL;
        break;
    case 't':
        valid = parse_literal(parser, "true", 4);
        span->kind = OM_RUNTIME_JSON_KIND_BOOLEAN;
        break;
    case 'f':
        valid = parse_literal(parser, "false", 5);
        span->kind = OM_RUNTIME_JSON_KIND_BOOLEAN;
        break;
    case '"':
        valid = parse_json_string(parser, NULL, NULL);
        span->kind = OM_RUNTIME_JSON_KIND_STRING;
        break;
    case '[':
        valid = parse_array(parser);
        span->kind = OM_RUNTIME_JSON_KIND_ARRAY;
        break;
    case '{':
        valid = parse_object(parser);
        span->kind = OM_RUNTIME_JSON_KIND_OBJECT;
        break;
    default:
        if (first == '-' || (first >= '0' && first <= '9')) {
            valid = parse_number(parser);
            span->kind = OM_RUNTIME_JSON_KIND_NUMBER;
        } else {
            parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        }
        break;
    }
    if (valid) {
        span->end = parser->offset;
    }
    return valid;
}

static bool first_invalid_utf8(const char *source, size_t length,
                              size_t *invalid_offset)
{
    size_t offset = 0;
    while (offset < length) {
        const size_t start = offset;
        uint32_t scalar = 0;
        if (om_utf8_decode_next((Om_Const_Byte_Slice){
                (const uint8_t *)source, length}, &offset, &scalar)
            != OM_STATUS_OK) {
            *invalid_offset = start;
            return true;
        }
    }
    return false;
}

static bool parse_root(const char *source, Json_Parser *parser,
                       Json_Span *root)
{
    if (parser == NULL) {
        return false;
    }
    *parser = (Json_Parser){.source = source};
    if (source == NULL) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, 0);
        return false;
    }
    parser->length = strlen(source);
    if (parser->length > JSON_MAX_BYTES) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SIZE, JSON_MAX_BYTES);
        return false;
    }
    size_t invalid_offset = 0;
    if (first_invalid_utf8(source, parser->length, &invalid_offset)) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_UTF8, invalid_offset);
        return false;
    }
    skip_whitespace(parser);
    if (!parse_value(parser, root)) {
        return false;
    }
    skip_whitespace(parser);
    if (parser->offset != parser->length) {
        parser_fail(parser, OM_RUNTIME_JSON_ERROR_SYNTAX, parser->offset);
        return false;
    }
    return true;
}

static char *compact_span(const char *source, size_t start, size_t end)
{
    if (source == NULL || start > end) {
        return NULL;
    }
    const size_t capacity = end - start;
    char *output = om_alloc(capacity + 1);
    if (output == NULL) {
        return NULL;
    }
    size_t output_length = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t index = start; index < end; ++index) {
        const char byte = source[index];
        if (in_string) {
            output[output_length++] = byte;
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            }
        } else if (byte == '"') {
            in_string = true;
            output[output_length++] = byte;
        } else if (byte != ' ' && byte != '\t' && byte != '\n'
                   && byte != '\r') {
            output[output_length++] = byte;
        }
    }
    output[output_length] = '\0';
    return output;
}

static bool get_array_item(const char *source, int64_t target,
                           Json_Span *selected)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (target < 0 || !parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_ARRAY) {
        return false;
    }
    parser.offset = root.start + 1;
    skip_whitespace(&parser);
    int64_t index = 0;
    while (parser.offset < root.end && parser.source[parser.offset] != ']') {
        Json_Span item = {0};
        if (!parse_value(&parser, &item)) {
            return false;
        }
        if (index == target) {
            *selected = item;
            return true;
        }
        ++index;
        skip_whitespace(&parser);
        if (parser.offset < parser.length
            && parser.source[parser.offset] == ',') {
            ++parser.offset;
            skip_whitespace(&parser);
        }
    }
    return false;
}

static bool get_object_item(const char *source, const char *target_key,
                            Json_Span *selected)
{
    if (target_key == NULL
        || om_utf8_validate((Om_Const_Byte_Slice){
            (const uint8_t *)target_key, strlen(target_key)}) != OM_STATUS_OK) {
        return false;
    }
    const size_t target_length = strlen(target_key);
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_OBJECT) {
        return false;
    }
    parser.offset = root.start + 1;
    skip_whitespace(&parser);
    while (parser.offset < root.end && parser.source[parser.offset] != '}') {
        char *key = NULL;
        size_t key_length = 0;
        if (!parse_json_string(&parser, &key, &key_length)) {
            return false;
        }
        skip_whitespace(&parser);
        ++parser.offset; /* colon, guaranteed by parse_root */
        skip_whitespace(&parser);
        Json_Span value = {0};
        const bool parsed = parse_value(&parser, &value);
        const bool matches = parsed && key_length == target_length
            && memcmp(key, target_key, target_length) == 0;
        om_free(key);
        if (!parsed) {
            return false;
        }
        if (matches) {
            *selected = value;
            return true;
        }
        skip_whitespace(&parser);
        if (parser.offset < parser.length
            && parser.source[parser.offset] == ',') {
            ++parser.offset;
            skip_whitespace(&parser);
        }
    }
    return false;
}

static const char *error_message(Om_Runtime_Json_Error error)
{
    switch (error) {
    case OM_RUNTIME_JSON_ERROR_NONE: return "";
    case OM_RUNTIME_JSON_ERROR_SYNTAX: return "invalid JSON syntax";
    case OM_RUNTIME_JSON_ERROR_UTF8: return "input is not valid UTF-8";
    case OM_RUNTIME_JSON_ERROR_DEPTH: return "JSON nesting limit exceeded";
    case OM_RUNTIME_JSON_ERROR_SIZE: return "JSON input size limit exceeded";
    case OM_RUNTIME_JSON_ERROR_DUPLICATE_KEY:
        return "duplicate object key";
    case OM_RUNTIME_JSON_ERROR_TYPE_OR_LOOKUP:
        return "JSON value kind or lookup mismatch";
    default: return "unknown JSON error";
    }
}

bool om_runtime_json_valid(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    return parse_root(source, &parser, &root);
}

int64_t om_runtime_json_kind(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    return parse_root(source, &parser, &root) ? (int64_t)root.kind : -1;
}

int64_t om_runtime_json_error_code(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (parse_root(source, &parser, &root)) {
        return OM_RUNTIME_JSON_ERROR_NONE;
    }
    return parser.error;
}

int64_t om_runtime_json_error_offset(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (parse_root(source, &parser, &root)) {
        return -1;
    }
    return (int64_t)parser.error_offset;
}

char *om_runtime_json_error_message(const char *source)
{
    const int64_t code = om_runtime_json_error_code(source);
    const char *message = error_message((Om_Runtime_Json_Error)code);
    const size_t length = strlen(message);
    char *copy = om_alloc(length + 1);
    if (copy != NULL) {
        (void)memcpy(copy, message, length + 1);
    }
    return copy;
}

char *om_runtime_json_compact(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    return parse_root(source, &parser, &root)
        ? compact_span(source, root.start, root.end) : NULL;
}

char *om_runtime_json_quote(const char *source)
{
    if (source == NULL) {
        return NULL;
    }
    const size_t length = strlen(source);
    if (length > JSON_MAX_BYTES
        || om_utf8_validate((Om_Const_Byte_Slice){
            (const uint8_t *)source, length}) != OM_STATUS_OK
        || length > (SIZE_MAX - 3) / 6) {
        return NULL;
    }
    const size_t capacity = length * 6 + 2;
    char *output = om_alloc(capacity + 1);
    if (output == NULL) {
        return NULL;
    }
    size_t output_length = 0;
    output[output_length++] = '"';
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = (unsigned char)source[index];
        if (byte == '"' || byte == '\\') {
            output[output_length++] = '\\';
            output[output_length++] = (char)byte;
        } else if (byte == '\b' || byte == '\f' || byte == '\n'
                   || byte == '\r' || byte == '\t') {
            output[output_length++] = '\\';
            output[output_length++] = byte == '\b' ? 'b'
                : byte == '\f' ? 'f' : byte == '\n' ? 'n'
                : byte == '\r' ? 'r' : 't';
        } else if (byte < 0x20) {
            output[output_length++] = '\\';
            output[output_length++] = 'u';
            output[output_length++] = '0';
            output[output_length++] = '0';
            output[output_length++] = hex[byte >> 4];
            output[output_length++] = hex[byte & 0x0f];
        } else {
            output[output_length++] = (char)byte;
        }
    }
    output[output_length++] = '"';
    output[output_length] = '\0';
    return output;
}

int64_t om_runtime_json_array_length(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_ARRAY) {
        return -1;
    }
    parser.offset = root.start + 1;
    skip_whitespace(&parser);
    int64_t count = 0;
    while (parser.offset < root.end && parser.source[parser.offset] != ']') {
        Json_Span item = {0};
        if (!parse_value(&parser, &item) || count == INT64_MAX) {
            return -1;
        }
        ++count;
        skip_whitespace(&parser);
        if (parser.offset < parser.length
            && parser.source[parser.offset] == ',') {
            ++parser.offset;
            skip_whitespace(&parser);
        }
    }
    return count;
}

bool om_runtime_json_array_index_valid(const char *source, int64_t index)
{
    Json_Span item = {0};
    return get_array_item(source, index, &item);
}

char *om_runtime_json_array_get(const char *source, int64_t index)
{
    Json_Span item = {0};
    return get_array_item(source, index, &item)
        ? compact_span(source, item.start, item.end) : NULL;
}

bool om_runtime_json_object_key_valid(const char *source, const char *key)
{
    Json_Span item = {0};
    return get_object_item(source, key, &item);
}

char *om_runtime_json_object_get(const char *source, const char *key)
{
    Json_Span item = {0};
    return get_object_item(source, key, &item)
        ? compact_span(source, item.start, item.end) : NULL;
}

char *om_runtime_json_string_value(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_STRING) {
        return NULL;
    }
    parser.offset = root.start;
    char *decoded = NULL;
    size_t decoded_length = 0;
    if (!parse_json_string(&parser, &decoded, &decoded_length)) {
        return NULL;
    }
    (void)decoded_length;
    return decoded;
}

int64_t om_runtime_json_object_length(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_OBJECT) {
        return -1;
    }
    parser.offset = root.start + 1;
    skip_whitespace(&parser);
    int64_t count = 0;
    while (parser.offset < root.end && parser.source[parser.offset] != '}') {
        char *key = NULL;
        size_t key_length = 0;
        Json_Span value = {0};
        if (!parse_json_string(&parser, &key, &key_length)) {
            return -1;
        }
        (void)key_length;
        om_free(key);
        skip_whitespace(&parser);
        if (parser.offset >= parser.length || parser.source[parser.offset++] != ':') {
            return -1;
        }
        if (!parse_value(&parser, &value) || count == INT64_MAX) {
            return -1;
        }
        ++count;
        skip_whitespace(&parser);
        if (parser.offset < parser.length && parser.source[parser.offset] == ',') {
            ++parser.offset;
            skip_whitespace(&parser);
        }
    }
    return count;
}

char *om_runtime_json_object_keys(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_OBJECT) {
        return NULL;
    }
    char *output = om_alloc(root.end - root.start + 3);
    if (output == NULL) {
        return NULL;
    }
    size_t output_length = 0;
    output[output_length++] = '[';
    parser.offset = root.start + 1;
    skip_whitespace(&parser);
    bool first = true;
    while (parser.offset < root.end && parser.source[parser.offset] != '}') {
        const size_t key_start = parser.offset;
        if (!parse_json_string(&parser, NULL, NULL)) {
            om_free(output);
            return NULL;
        }
        const size_t key_end = parser.offset;
        if (!first) {
            output[output_length++] = ',';
        }
        const size_t key_length = key_end - key_start;
        (void)memcpy(output + output_length, parser.source + key_start,
                     key_length);
        output_length += key_length;
        first = false;
        skip_whitespace(&parser);
        if (parser.offset >= parser.length || parser.source[parser.offset++] != ':') {
            om_free(output);
            return NULL;
        }
        Json_Span value = {0};
        if (!parse_value(&parser, &value)) {
            om_free(output);
            return NULL;
        }
        skip_whitespace(&parser);
        if (parser.offset < parser.length && parser.source[parser.offset] == ',') {
            ++parser.offset;
            skip_whitespace(&parser);
        }
    }
    output[output_length++] = ']';
    output[output_length] = '\0';
    return output;
}

bool om_runtime_json_int64_valid(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_NUMBER) {
        return false;
    }
    for (size_t index = root.start; index < root.end; ++index) {
        if (source[index] == '.' || source[index] == 'e' || source[index] == 'E') {
            return false;
        }
    }
    errno = 0;
    char *end = NULL;
    (void)strtoimax(source + root.start, &end, 10);
    return errno != ERANGE && end == source + root.end;
}

int64_t om_runtime_json_int64_value(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_NUMBER) {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    const intmax_t value = strtoimax(source + root.start, &end, 10);
    if (errno == ERANGE || end != source + root.end
        || value < INT64_MIN || value > INT64_MAX) {
        return 0;
    }
    return (int64_t)value;
}

bool om_runtime_json_float64_valid(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_NUMBER) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const double value = strtod(source + root.start, &end);
    return errno != ERANGE && end == source + root.end && isfinite(value);
}

double om_runtime_json_float64_value(const char *source)
{
    Json_Parser parser = {0};
    Json_Span root = {0};
    if (!parse_root(source, &parser, &root)
        || root.kind != OM_RUNTIME_JSON_KIND_NUMBER) {
        return 0.0;
    }
    char *end = NULL;
    const double value = strtod(source + root.start, &end);
    return end == source + root.end && isfinite(value) ? value : 0.0;
}

void om_runtime_json_release(void *value)
{
    om_free(value);
}
