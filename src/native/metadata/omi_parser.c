#include "ombasic/native/metadata/omi.h"

#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/arena.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/syntax/name_rules.h"

typedef enum Om_Omi_Token_Kind {
    OM_OMI_TOKEN_INVALID = 0,
    OM_OMI_TOKEN_WORD,
    OM_OMI_TOKEN_STRING,
    OM_OMI_TOKEN_NEWLINE,
    OM_OMI_TOKEN_LEFT_PAREN,
    OM_OMI_TOKEN_RIGHT_PAREN,
    OM_OMI_TOKEN_COLON,
    OM_OMI_TOKEN_COMMA,
    OM_OMI_TOKEN_ARROW,
    OM_OMI_TOKEN_EOF
} Om_Omi_Token_Kind;

typedef struct Om_Omi_Token {
    Om_Omi_Token_Kind kind;
    Om_String_View text;
    Om_Source_Span span;
} Om_Omi_Token;

typedef struct Om_Omi_Storage {
    Om_Arena arena;
} Om_Omi_Storage;

typedef struct Om_Omi_Parser {
    const Om_Source_Text *source;
    Om_Omi_Storage *storage;
    Om_Vector tokens;
    Om_Vector functions;
    Om_Vector parameters;
    Om_Vector callback_parameters;
    Om_Vector callback_bindings;
    Om_Vector records;
    Om_Vector record_fields;
    Om_Vector diagnostics;
    Om_String_View library_path;
    size_t cursor;
    Om_Status status;
} Om_Omi_Parser;

static bool ascii_letter(uint8_t byte)
{
    return (byte >= (uint8_t)'a' && byte <= (uint8_t)'z')
        || (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z');
}

static bool ascii_digit(uint8_t byte)
{
    return byte >= (uint8_t)'0' && byte <= (uint8_t)'9';
}

static bool ascii_word_byte(uint8_t byte)
{
    return ascii_letter(byte) || ascii_digit(byte) || byte == (uint8_t)'_';
}

static Om_Source_Span source_span(const Om_Omi_Parser *parser,
                                  size_t start, size_t end)
{
    return (Om_Source_Span){
        .source = parser->source->id,
        .start = (uint32_t)start,
        .end = (uint32_t)end
    };
}

static void add_diagnostic(Om_Omi_Parser *parser,
                           Om_Omi_Diagnostic_Code code,
                           Om_Source_Span span)
{
    if (parser->status != OM_STATUS_OK) {
        return;
    }
    const Om_Omi_Diagnostic diagnostic = {.code = code, .span = span};
    parser->status = om_vector_push(&parser->diagnostics, &diagnostic);
}

static void add_current_diagnostic(Om_Omi_Parser *parser,
                                   Om_Omi_Diagnostic_Code code)
{
    const Om_Omi_Token *token = om_vector_at_const(&parser->tokens,
                                                   parser->cursor);
    if (token != NULL) {
        add_diagnostic(parser, code, token->span);
    }
}

static bool push_token(Om_Omi_Parser *parser, Om_Omi_Token token)
{
    if (parser->status != OM_STATUS_OK) {
        return false;
    }
    parser->status = om_vector_push(&parser->tokens, &token);
    return parser->status == OM_STATUS_OK;
}

static bool push_invalid_token(Om_Omi_Parser *parser, size_t start,
                               size_t end)
{
    add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801,
                   source_span(parser, start, end));
    return push_token(parser, (Om_Omi_Token){
        .kind = OM_OMI_TOKEN_INVALID,
        .span = source_span(parser, start, end)
    });
}

static Om_Status scan_tokens(Om_Omi_Parser *parser)
{
    const uint8_t *bytes = parser->source->bytes.data;
    const size_t length = parser->source->bytes.length;
    size_t cursor = 0;
    while (cursor < length && parser->status == OM_STATUS_OK) {
        const size_t start = cursor;
        const uint8_t byte = bytes[cursor];
        if (byte == (uint8_t)' ' || byte == (uint8_t)'\t') {
            ++cursor;
            continue;
        }
        if (byte == (uint8_t)'\n') {
            ++cursor;
            (void)push_token(parser, (Om_Omi_Token){
                .kind = OM_OMI_TOKEN_NEWLINE,
                .span = source_span(parser, start, cursor)
            });
            continue;
        }
        if (byte == (uint8_t)'\r') {
            if (cursor + 1 < length && bytes[cursor + 1] == (uint8_t)'\n') {
                cursor += 2;
                (void)push_token(parser, (Om_Omi_Token){
                    .kind = OM_OMI_TOKEN_NEWLINE,
                    .span = source_span(parser, start, cursor)
                });
            } else {
                ++cursor;
                (void)push_invalid_token(parser, start, cursor);
            }
            continue;
        }
        if (byte == (uint8_t)'\'') {
            while (cursor < length && bytes[cursor] != (uint8_t)'\n'
                   && bytes[cursor] != (uint8_t)'\r') {
                ++cursor;
            }
            continue;
        }
        if (ascii_letter(byte) || byte == (uint8_t)'_') {
            ++cursor;
            while (cursor < length && ascii_word_byte(bytes[cursor])) {
                ++cursor;
            }
            (void)push_token(parser, (Om_Omi_Token){
                .kind = OM_OMI_TOKEN_WORD,
                .text = {(const char *)bytes + start, cursor - start},
                .span = source_span(parser, start, cursor)
            });
            continue;
        }
        if (byte == (uint8_t)'"') {
            ++cursor;
            const size_t content_start = cursor;
            bool terminated = false;
            while (cursor < length) {
                const uint8_t current = bytes[cursor];
                if (current == (uint8_t)'"') {
                    terminated = true;
                    break;
                }
                if (current == (uint8_t)'\n' || current == (uint8_t)'\r') {
                    break;
                }
                if (current == (uint8_t)'\\') {
                    ++cursor;
                    if (cursor >= length || bytes[cursor] == (uint8_t)'\n'
                        || bytes[cursor] == (uint8_t)'\r') {
                        break;
                    }
                    if (bytes[cursor] == (uint8_t)'u'
                        && cursor + 1 < length
                        && bytes[cursor + 1] == (uint8_t)'{') {
                        cursor += 2;
                        while (cursor < length
                               && bytes[cursor] != (uint8_t)'}'
                               && bytes[cursor] != (uint8_t)'\n'
                               && bytes[cursor] != (uint8_t)'\r') {
                            ++cursor;
                        }
                        if (cursor < length
                            && bytes[cursor] == (uint8_t)'}') {
                            ++cursor;
                        }
                    } else {
                        ++cursor;
                    }
                    continue;
                }
                ++cursor;
            }
            const size_t content_end = cursor;
            if (terminated) {
                ++cursor;
                (void)push_token(parser, (Om_Omi_Token){
                    .kind = OM_OMI_TOKEN_STRING,
                    .text = {(const char *)bytes + content_start,
                             content_end - content_start},
                    .span = source_span(parser, start, cursor)
                });
            } else {
                (void)push_invalid_token(parser, start, content_end);
            }
            continue;
        }

        Om_Omi_Token_Kind kind = OM_OMI_TOKEN_INVALID;
        size_t width = 1;
        if (byte == (uint8_t)'(') {
            kind = OM_OMI_TOKEN_LEFT_PAREN;
        } else if (byte == (uint8_t)')') {
            kind = OM_OMI_TOKEN_RIGHT_PAREN;
        } else if (byte == (uint8_t)':') {
            kind = OM_OMI_TOKEN_COLON;
        } else if (byte == (uint8_t)',') {
            kind = OM_OMI_TOKEN_COMMA;
        } else if (byte == (uint8_t)'-' && cursor + 1 < length
                   && bytes[cursor + 1] == (uint8_t)'>') {
            kind = OM_OMI_TOKEN_ARROW;
            width = 2;
        }
        cursor += width;
        if (kind == OM_OMI_TOKEN_INVALID) {
            if (byte >= UINT8_C(0x80)) {
                size_t next = start;
                uint32_t scalar = 0;
                if (om_utf8_decode_next(parser->source->bytes, &next,
                                        &scalar) == OM_STATUS_OK) {
                    (void)scalar;
                    cursor = next;
                }
            }
            (void)push_invalid_token(parser, start, cursor);
        } else {
            (void)push_token(parser, (Om_Omi_Token){
                .kind = kind,
                .span = source_span(parser, start, cursor)
            });
        }
    }
    if (parser->status != OM_STATUS_OK) {
        return parser->status;
    }
    return push_token(parser, (Om_Omi_Token){
        .kind = OM_OMI_TOKEN_EOF,
        .span = source_span(parser, length, length)
    }) ? OM_STATUS_OK : parser->status;
}

static const Om_Omi_Token *peek_token(const Om_Omi_Parser *parser)
{
    return om_vector_at_const(&parser->tokens, parser->cursor);
}

static const Om_Omi_Token *peek_token_at(const Om_Omi_Parser *parser,
                                         size_t offset)
{
    if (offset > SIZE_MAX - parser->cursor) {
        return NULL;
    }
    return om_vector_at_const(&parser->tokens, parser->cursor + offset);
}

static Om_Omi_Token take_token(Om_Omi_Parser *parser)
{
    const Om_Omi_Token *token = peek_token(parser);
    if (token == NULL) {
        return (Om_Omi_Token){.kind = OM_OMI_TOKEN_EOF};
    }
    const Om_Omi_Token result = *token;
    if (result.kind != OM_OMI_TOKEN_EOF) {
        parser->cursor += 1;
    }
    return result;
}

static bool word_equals(Om_String_View word, const char *text)
{
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return word.length == length
        && (length == 0 || memcmp(word.data, text, length) == 0);
}

static bool current_word_is(const Om_Omi_Parser *parser, const char *word)
{
    const Om_Omi_Token *token = peek_token(parser);
    return token != NULL && token->kind == OM_OMI_TOKEN_WORD
        && word_equals(token->text, word);
}

static bool current_end_pair_is(const Om_Omi_Parser *parser,
                                const char *word)
{
    const Om_Omi_Token *first = peek_token(parser);
    const Om_Omi_Token *second = peek_token_at(parser, 1);
    return first != NULL && second != NULL
        && first->kind == OM_OMI_TOKEN_WORD
        && second->kind == OM_OMI_TOKEN_WORD
        && word_equals(first->text, "END")
        && word_equals(second->text, word);
}

static void skip_newlines(Om_Omi_Parser *parser)
{
    while (peek_token(parser) != NULL
           && peek_token(parser)->kind == OM_OMI_TOKEN_NEWLINE) {
        (void)take_token(parser);
    }
}

static void synchronize_line(Om_Omi_Parser *parser)
{
    while (peek_token(parser) != NULL
           && peek_token(parser)->kind != OM_OMI_TOKEN_NEWLINE
           && peek_token(parser)->kind != OM_OMI_TOKEN_EOF) {
        (void)take_token(parser);
    }
    if (peek_token(parser) != NULL
        && peek_token(parser)->kind == OM_OMI_TOKEN_NEWLINE) {
        (void)take_token(parser);
    }
}

static bool expect_word(Om_Omi_Parser *parser, const char *word)
{
    if (current_word_is(parser, word)) {
        (void)take_token(parser);
        return true;
    }
    add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
    return false;
}

static bool expect_token_kind(Om_Omi_Parser *parser,
                              Om_Omi_Token_Kind kind)
{
    const Om_Omi_Token *token = peek_token(parser);
    if (token != NULL && token->kind == kind) {
        (void)take_token(parser);
        return true;
    }
    add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
    return false;
}

static bool expect_line_end(Om_Omi_Parser *parser)
{
    const Om_Omi_Token *token = peek_token(parser);
    if (token == NULL || token->kind == OM_OMI_TOKEN_EOF) {
        return true;
    }
    if (token->kind == OM_OMI_TOKEN_NEWLINE) {
        skip_newlines(parser);
        return true;
    }
    add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
    synchronize_line(parser);
    return false;
}

static bool expect_name(Om_Omi_Parser *parser, Om_Name_Class expected,
                        Om_String_View *name, Om_Source_Span *span)
{
    const Om_Omi_Token *token = peek_token(parser);
    if (token == NULL || token->kind != OM_OMI_TOKEN_WORD) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
        return false;
    }
    const Om_Omi_Token consumed = take_token(parser);
    const Om_Name_Class actual = om_name_classify(consumed.text);
    if (actual != expected) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0802, consumed.span);
        return false;
    }
    if (name != NULL) {
        *name = consumed.text;
    }
    if (span != NULL) {
        *span = consumed.span;
    }
    return true;
}

static int hex_value(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return (int)(byte - (uint8_t)'0');
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return (int)(byte - (uint8_t)'a') + 10;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return (int)(byte - (uint8_t)'A') + 10;
    }
    return -1;
}

static size_t encode_utf8(uint32_t scalar, char output[4])
{
    if (scalar <= UINT32_C(0x7f)) {
        output[0] = (char)scalar;
        return 1;
    }
    if (scalar <= UINT32_C(0x7ff)) {
        output[0] = (char)(UINT32_C(0xc0) | (scalar >> 6));
        output[1] = (char)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        return 2;
    }
    if (scalar <= UINT32_C(0xffff)) {
        output[0] = (char)(UINT32_C(0xe0) | (scalar >> 12));
        output[1] = (char)(UINT32_C(0x80)
                           | ((scalar >> 6) & UINT32_C(0x3f)));
        output[2] = (char)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
        return 3;
    }
    output[0] = (char)(UINT32_C(0xf0) | (scalar >> 18));
    output[1] = (char)(UINT32_C(0x80)
                       | ((scalar >> 12) & UINT32_C(0x3f)));
    output[2] = (char)(UINT32_C(0x80)
                       | ((scalar >> 6) & UINT32_C(0x3f)));
    output[3] = (char)(UINT32_C(0x80) | (scalar & UINT32_C(0x3f)));
    return 4;
}

static bool decode_string(Om_Omi_Parser *parser,
                          const Om_Omi_Token *token,
                          Om_String_View *decoded)
{
    if (token == NULL || token->kind != OM_OMI_TOKEN_STRING
        || decoded == NULL) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
        return false;
    }
    if (token->text.length == SIZE_MAX) {
        parser->status = OM_STATUS_OVERFLOW;
        return false;
    }
    char *output = NULL;
    parser->status = om_arena_alloc(&parser->storage->arena,
                                   token->text.length + 1, 1,
                                   (void **)&output);
    if (parser->status != OM_STATUS_OK) {
        return false;
    }
    size_t input_index = 0;
    size_t output_length = 0;
    while (input_index < token->text.length) {
        const uint8_t current = (uint8_t)token->text.data[input_index++];
        if (current != (uint8_t)'\\') {
            output[output_length++] = (char)current;
            continue;
        }
        if (input_index >= token->text.length) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801, token->span);
            return false;
        }
        const uint8_t escape = (uint8_t)token->text.data[input_index++];
        switch (escape) {
        case (uint8_t)'\\': output[output_length++] = '\\'; break;
        case (uint8_t)'"': output[output_length++] = '"'; break;
        case (uint8_t)'n': output[output_length++] = '\n'; break;
        case (uint8_t)'r': output[output_length++] = '\r'; break;
        case (uint8_t)'t': output[output_length++] = '\t'; break;
        case (uint8_t)'0': output[output_length++] = '\0'; break;
        case (uint8_t)'u': {
            if (input_index >= token->text.length
                || token->text.data[input_index] != '{') {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801,
                               token->span);
                return false;
            }
            ++input_index;
            uint32_t scalar = 0;
            size_t digits = 0;
            bool valid = true;
            while (input_index < token->text.length
                   && token->text.data[input_index] != '}') {
                const int digit = hex_value(
                    (uint8_t)token->text.data[input_index++]);
                if (digit < 0 || digits >= 6
                    || scalar > (UINT32_C(0x10ffff) - (uint32_t)digit)
                        / UINT32_C(16)) {
                    valid = false;
                } else {
                    scalar = scalar * UINT32_C(16) + (uint32_t)digit;
                    ++digits;
                }
            }
            if (input_index >= token->text.length
                || token->text.data[input_index] != '}' || digits == 0
                || (scalar >= UINT32_C(0xd800)
                    && scalar <= UINT32_C(0xdfff))) {
                valid = false;
            } else {
                ++input_index;
            }
            if (!valid) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801,
                               token->span);
                return false;
            }
            char encoded[4];
            const size_t encoded_length = encode_utf8(scalar, encoded);
            (void)memcpy(output + output_length, encoded, encoded_length);
            output_length += encoded_length;
            break;
        }
        default:
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801, token->span);
            return false;
        }
    }
    output[output_length] = '\0';
    *decoded = (Om_String_View){output, output_length};
    return true;
}

static bool string_contains_nul(Om_String_View text)
{
    return text.length != 0 && memchr(text.data, '\0', text.length) != NULL;
}

static bool current_ownership(const Om_Omi_Parser *parser)
{
    return current_word_is(parser, "BORROWED")
        || current_word_is(parser, "OWNED")
        || current_word_is(parser, "STATIC");
}

static Om_Native_Ownership parse_ownership(Om_Omi_Parser *parser)
{
    if (current_word_is(parser, "BORROWED")) {
        (void)take_token(parser);
        return OM_NATIVE_OWNERSHIP_BORROWED;
    }
    if (current_word_is(parser, "OWNED")) {
        (void)take_token(parser);
        return OM_NATIVE_OWNERSHIP_OWNED;
    }
    if (current_word_is(parser, "STATIC")) {
        (void)take_token(parser);
        return OM_NATIVE_OWNERSHIP_STATIC;
    }
    return OM_NATIVE_OWNERSHIP_NONE;
}

static Om_Native_Abi_Type parse_abi_type(Om_Omi_Parser *parser,
                                         Om_Source_Span *span,
                                         bool allow_record,
                                         Om_String_View *record_name)
{
    static const struct {
        const char *name;
        Om_Native_Abi_Type type;
    } types[] = {
        {"CVoid", OM_NATIVE_ABI_CVOID},
        {"CInt8", OM_NATIVE_ABI_CINT8},
        {"CInt16", OM_NATIVE_ABI_CINT16},
        {"CInt32", OM_NATIVE_ABI_CINT32},
        {"CInt64", OM_NATIVE_ABI_CINT64},
        {"CUInt8", OM_NATIVE_ABI_CUINT8},
        {"CUInt16", OM_NATIVE_ABI_CUINT16},
        {"CUInt32", OM_NATIVE_ABI_CUINT32},
        {"CUInt64", OM_NATIVE_ABI_CUINT64},
        {"CInt", OM_NATIVE_ABI_CINT},
        {"CUInt", OM_NATIVE_ABI_CUINT},
        {"CLong", OM_NATIVE_ABI_CLONG},
        {"CULong", OM_NATIVE_ABI_CULONG},
        {"CLongLong", OM_NATIVE_ABI_CLONGLONG},
        {"CULongLong", OM_NATIVE_ABI_CULONGLONG},
        {"CFloat", OM_NATIVE_ABI_CFLOAT},
        {"CDouble", OM_NATIVE_ABI_CDOUBLE},
        {"CSize", OM_NATIVE_ABI_CSIZE},
        {"CBool", OM_NATIVE_ABI_CBOOL},
        {"CPointer", OM_NATIVE_ABI_CPOINTER},
        {"CString", OM_NATIVE_ABI_CSTRING},
        {"CUtf8Slice", OM_NATIVE_ABI_CUTF8_SLICE}
    };
    const Om_Omi_Token *token = peek_token(parser);
    if (token == NULL || token->kind != OM_OMI_TOKEN_WORD) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804);
        return OM_NATIVE_ABI_INVALID;
    }
    const Om_Omi_Token consumed = take_token(parser);
    if (span != NULL) {
        *span = consumed.span;
    }
    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
        if (word_equals(consumed.text, types[index].name)) {
            return types[index].type;
        }
    }
    if (allow_record) {
        if (om_name_classify(consumed.text) != OM_NAME_CLASS_ENTITY) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, consumed.span);
            return OM_NATIVE_ABI_INVALID;
        }
        if (record_name != NULL) {
            *record_name = consumed.text;
        }
        return OM_NATIVE_ABI_RECORD;
    }
    add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, consumed.span);
    return OM_NATIVE_ABI_INVALID;
}

static bool is_pointer_type(Om_Native_Abi_Type type)
{
    return type == OM_NATIVE_ABI_CPOINTER
        || type == OM_NATIVE_ABI_CSTRING
        || type == OM_NATIVE_ABI_CUTF8_SLICE;
}

static bool push_value(Om_Omi_Parser *parser, Om_Vector *vector,
                       const void *value)
{
    if (parser->status != OM_STATUS_OK) {
        return false;
    }
    parser->status = om_vector_push(vector, value);
    return parser->status == OM_STATUS_OK;
}

static bool parse_callback_signature(Om_Omi_Parser *parser,
                                     Om_Omi_Parameter *parameter)
{
    (void)take_token(parser); /* CALLBACK */
    if (!expect_token_kind(parser, OM_OMI_TOKEN_LEFT_PAREN)) {
        return false;
    }
    parameter->is_callback = true;
    parameter->abi_type = OM_NATIVE_ABI_INVALID;
    parameter->callback_parameter_start =
        parser->callback_parameters.length;
    const Om_Omi_Token *next = peek_token(parser);
    if (next == NULL) {
        return false;
    }
    if (next->kind != OM_OMI_TOKEN_RIGHT_PAREN) {
        for (;;) {
            Om_Source_Span type_span = {0};
            const Om_Native_Abi_Type type = parse_abi_type(
                parser, &type_span, false, NULL);
            if (type == OM_NATIVE_ABI_CVOID) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, type_span);
            }
            if (type == OM_NATIVE_ABI_INVALID) {
                return false;
            }
            const Om_Omi_Callback_Parameter callback_parameter = {
                .abi_type = type,
                .span = type_span
            };
            if (!push_value(parser, &parser->callback_parameters,
                            &callback_parameter)) {
                return false;
            }
            next = peek_token(parser);
            if (next != NULL && next->kind == OM_OMI_TOKEN_COMMA) {
                (void)take_token(parser);
                continue;
            }
            break;
        }
    }
    if (!expect_token_kind(parser, OM_OMI_TOKEN_RIGHT_PAREN)
        || !expect_token_kind(parser, OM_OMI_TOKEN_ARROW)) {
        return false;
    }
    Om_Source_Span result_span = {0};
    parameter->callback_result_type = parse_abi_type(
        parser, &result_span, false, NULL);
    if (parameter->callback_result_type == OM_NATIVE_ABI_INVALID) {
        return false;
    }
    if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, result_span);
    }
    parameter->callback_parameter_count = parser->callback_parameters.length
        - parameter->callback_parameter_start;
    return true;
}

static bool parse_parameter(Om_Omi_Parser *parser,
                            Om_Omi_Parameter *parameter)
{
    *parameter = (Om_Omi_Parameter){0};
    Om_Source_Span name_span = {0};
    if (!expect_name(parser, OM_NAME_CLASS_ORDINARY, &parameter->name,
                     &name_span)
        || !expect_token_kind(parser, OM_OMI_TOKEN_COLON)) {
        return false;
    }
    parameter->span = name_span;
    if (current_word_is(parser, "CALLBACK")) {
        if (!parse_callback_signature(parser, parameter)) {
            return false;
        }
        if (current_ownership(parser)) {
            const Om_Omi_Token ownership_token = take_token(parser);
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805,
                           ownership_token.span);
        }
    } else {
        Om_Source_Span type_span = {0};
        parameter->record_index = SIZE_MAX;
        parameter->abi_type = parse_abi_type(
            parser, &type_span, true, &parameter->record_name);
        if (parameter->abi_type == OM_NATIVE_ABI_INVALID) {
            return false;
        }
        parameter->span.end = type_span.end;
        parameter->ownership = parse_ownership(parser);
        if (is_pointer_type(parameter->abi_type)
            != (parameter->ownership != OM_NATIVE_OWNERSHIP_NONE)) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805,
                           parameter->span);
        }
        if (parameter->abi_type == OM_NATIVE_ABI_CVOID) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, type_span);
        }
    }
    return true;
}

static size_t find_parameter(const Om_Omi_Parser *parser,
                             size_t first, size_t count,
                             Om_String_View name)
{
    for (size_t index = 0; index < count; ++index) {
        const Om_Omi_Parameter *parameter = om_vector_at_const(
            &parser->parameters, first + index);
        if (parameter != NULL
            && om_name_equal_case_insensitive(parameter->name, name)) {
            return first + index;
        }
    }
    return SIZE_MAX;
}

static bool parse_effects(Om_Omi_Parser *parser, uint32_t *effects)
{
    static const struct {
        const char *name;
        uint32_t bit;
    } effect_names[] = {
        {"NATIVE", OM_NATIVE_EFFECT_NATIVE},
        {"FILE_IO", OM_NATIVE_EFFECT_FILE_IO},
        {"NETWORK_IO", OM_NATIVE_EFFECT_NETWORK_IO},
        {"CLOCK", OM_NATIVE_EFFECT_CLOCK}
    };
    if (current_word_is(parser, "PURE")) {
        (void)take_token(parser);
        *effects = OM_NATIVE_EFFECT_NONE;
        if (peek_token(parser) != NULL
            && peek_token(parser)->kind != OM_OMI_TOKEN_NEWLINE
            && peek_token(parser)->kind != OM_OMI_TOKEN_EOF) {
            add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808);
            return false;
        }
        return true;
    }
    uint32_t result = 0;
    bool saw_effect = false;
    for (;;) {
        const Om_Omi_Token *token = peek_token(parser);
        if (token == NULL || token->kind != OM_OMI_TOKEN_WORD) {
            add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808);
            return false;
        }
        const Om_Omi_Token consumed = take_token(parser);
        uint32_t bit = 0;
        for (size_t index = 0;
             index < sizeof(effect_names) / sizeof(effect_names[0]); ++index) {
            if (word_equals(consumed.text, effect_names[index].name)) {
                bit = effect_names[index].bit;
                break;
            }
        }
        if (bit == 0) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808, consumed.span);
        } else if ((result & bit) != 0) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803, consumed.span);
        } else {
            result |= bit;
            saw_effect = true;
        }
        const Om_Omi_Token *next = peek_token(parser);
        if (next != NULL && next->kind == OM_OMI_TOKEN_COMMA) {
            (void)take_token(parser);
            continue;
        }
        break;
    }
    if (!saw_effect) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808);
        return false;
    }
    *effects = result;
    return true;
}

static bool parse_callback_binding(Om_Omi_Parser *parser,
                                   size_t parameter_start,
                                   size_t parameter_count)
{
    const Om_Omi_Token *annotation = peek_token(parser);
    Om_Omi_Callback_Binding binding = {0};
    Om_String_View parameter_name = {0};
    (void)take_token(parser); /* CALLBACK */
    if (!expect_name(parser, OM_NAME_CLASS_ORDINARY, &parameter_name, NULL)
        || !expect_word(parser, "FOREIGN_THREAD")
        || !expect_word(parser, "MESSAGE")
        || !expect_name(parser, OM_NAME_CLASS_ENTITY,
                        &binding.message_name, NULL)) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0806,
                       annotation != NULL ? annotation->span
                                          : source_span(parser, 0, 0));
        return false;
    }
    binding.parameter_index = find_parameter(parser, parameter_start,
                                             parameter_count, parameter_name);
    if (binding.parameter_index == SIZE_MAX) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0806,
                       annotation->span);
        return false;
    }
    const Om_Omi_Parameter *parameter = om_vector_at_const(
        &parser->parameters, binding.parameter_index);
    if (parameter == NULL || !parameter->is_callback) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0806,
                       annotation->span);
        return false;
    }
    for (size_t index = 0; index < parser->callback_bindings.length; ++index) {
        const Om_Omi_Callback_Binding *existing = om_vector_at_const(
            &parser->callback_bindings, index);
        if (existing->parameter_index == binding.parameter_index) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803,
                           annotation->span);
            return false;
        }
    }
    binding.span = annotation->span;
    return push_value(parser, &parser->callback_bindings, &binding);
}

static void validate_function_metadata(Om_Omi_Parser *parser,
                                       Om_Omi_Native_Function *function,
                                       bool has_symbol,
                                       bool has_effect,
                                       bool has_blocking,
                                       bool has_return_annotation)
{
    if (!has_symbol || !has_effect || !has_blocking) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803, function->span);
    }
    if (has_symbol && (function->symbol.length == 0
                       || string_contains_nul(function->symbol))) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808, function->span);
    }
    if (is_pointer_type(function->result_type) != has_return_annotation) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805, function->span);
    }
    if (function->result_ownership == OM_NATIVE_OWNERSHIP_BORROWED) {
        const size_t owner_index = find_parameter(
            parser, function->parameter_start, function->parameter_count,
            function->result_owner_parameter);
        const Om_Omi_Parameter *owner = owner_index == SIZE_MAX ? NULL
            : om_vector_at_const(&parser->parameters, owner_index);
        if (owner == NULL || owner->is_callback
            || !is_pointer_type(owner->abi_type)
            || (function->result_type == OM_NATIVE_ABI_CPOINTER
                && (owner->abi_type != OM_NATIVE_ABI_CPOINTER
                    || owner->ownership == OM_NATIVE_OWNERSHIP_OWNED))) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805, function->span);
        }
    }
    if (function->result_type == OM_NATIVE_ABI_CUTF8_SLICE
        && function->result_ownership != OM_NATIVE_OWNERSHIP_OWNED) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805, function->span);
    }
    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = om_vector_at_const(
            &parser->parameters, function->parameter_start + index);
        if (parameter == NULL) {
            continue;
        }
        for (size_t prior = 0; prior < index; ++prior) {
            const Om_Omi_Parameter *earlier = om_vector_at_const(
                &parser->parameters, function->parameter_start + prior);
            if (earlier != NULL
                && om_name_equal_case_insensitive(earlier->name,
                                                   parameter->name)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               parameter->span);
                break;
            }
        }
        if (!parameter->is_callback) {
            continue;
        }
        size_t binding_count = 0;
        for (size_t binding_index = 0;
             binding_index < function->callback_binding_count;
             ++binding_index) {
            const Om_Omi_Callback_Binding *binding = om_vector_at_const(
                &parser->callback_bindings,
                function->callback_binding_start + binding_index);
            if (binding != NULL
                && binding->parameter_index
                    == function->parameter_start + index) {
                ++binding_count;
            }
        }
        if (binding_count != 1) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0806,
                           parameter->span);
        }
    }
}

static bool parse_function(Om_Omi_Parser *parser)
{
    const Om_Omi_Token *start_token = peek_token(parser);
    Om_Omi_Native_Function function = {0};
    function.span = start_token != NULL ? start_token->span
                                        : source_span(parser, 0, 0);
    function.parameter_start = parser->parameters.length;
    function.callback_binding_start = parser->callback_bindings.length;
    (void)take_token(parser); /* FUNCTION */
    if (!expect_name(parser, OM_NAME_CLASS_ORDINARY, &function.name, NULL)
        || !expect_token_kind(parser, OM_OMI_TOKEN_LEFT_PAREN)) {
        synchronize_line(parser);
        return false;
    }
    const Om_Omi_Token *next = peek_token(parser);
    if (next == NULL) {
        return false;
    }
    if (next->kind != OM_OMI_TOKEN_RIGHT_PAREN) {
        for (;;) {
            Om_Omi_Parameter parameter = {0};
            if (!parse_parameter(parser, &parameter)
                || !push_value(parser, &parser->parameters, &parameter)) {
                synchronize_line(parser);
                return false;
            }
            next = peek_token(parser);
            if (next != NULL && next->kind == OM_OMI_TOKEN_COMMA) {
                (void)take_token(parser);
                continue;
            }
            break;
        }
    }
    if (!expect_token_kind(parser, OM_OMI_TOKEN_RIGHT_PAREN)
        || !expect_token_kind(parser, OM_OMI_TOKEN_COLON)) {
        synchronize_line(parser);
        return false;
    }
    function.result_record_index = SIZE_MAX;
    function.result_type = parse_abi_type(
        parser, &function.result_type_span, true,
        &function.result_record_name);
    if (function.result_type == OM_NATIVE_ABI_INVALID
        || !expect_line_end(parser)) {
        synchronize_line(parser);
        return false;
    }
    function.parameter_count = parser->parameters.length
        - function.parameter_start;

    bool has_symbol = false;
    bool has_effect = false;
    bool has_blocking = false;
    bool has_return_annotation = false;
    bool ended = false;
    while (peek_token(parser) != NULL
           && peek_token(parser)->kind != OM_OMI_TOKEN_EOF
           && parser->status == OM_STATUS_OK) {
        skip_newlines(parser);
        if (current_end_pair_is(parser, "FUNCTION")) {
            (void)take_token(parser);
            (void)take_token(parser);
            (void)expect_line_end(parser);
            ended = true;
            break;
        }
        if (current_end_pair_is(parser, "NATIVE")) {
            add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
            break;
        }
        const Om_Omi_Token *annotation = peek_token(parser);
        if (annotation == NULL || annotation->kind == OM_OMI_TOKEN_EOF) {
            break;
        }
        if (current_word_is(parser, "SYMBOL")) {
            (void)take_token(parser);
            const Om_Omi_Token *string_token = peek_token(parser);
            Om_String_View symbol = {0};
            if (!decode_string(parser, string_token, &symbol)) {
                synchronize_line(parser);
                continue;
            }
            (void)take_token(parser);
            if (has_symbol) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803,
                               annotation->span);
            } else {
                function.symbol = symbol;
                has_symbol = true;
            }
        } else if (current_word_is(parser, "EFFECT")) {
            (void)take_token(parser);
            uint32_t effects = 0;
            const bool parsed = parse_effects(parser, &effects);
            if (has_effect) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803,
                               annotation->span);
            } else {
                function.effects = effects;
                has_effect = parsed;
            }
        } else if (current_word_is(parser, "BLOCKING")
                   || current_word_is(parser, "NONBLOCKING")) {
            const bool blocking = current_word_is(parser, "BLOCKING");
            (void)take_token(parser);
            if (has_blocking) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803,
                               annotation->span);
            } else {
                function.blocking = blocking ? OM_NATIVE_BLOCKING_YES
                                             : OM_NATIVE_BLOCKING_NO;
                has_blocking = true;
            }
        } else if (current_word_is(parser, "RETURNS")) {
            (void)take_token(parser);
            if (has_return_annotation) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0803,
                               annotation->span);
                synchronize_line(parser);
                continue;
            }
            has_return_annotation = true;
            if (current_word_is(parser, "BORROWED")) {
                (void)take_token(parser);
                if (!expect_word(parser, "FROM")
                    || !expect_name(parser, OM_NAME_CLASS_ORDINARY,
                                    &function.result_owner_parameter, NULL)) {
                    synchronize_line(parser);
                    continue;
                }
                function.result_ownership = OM_NATIVE_OWNERSHIP_BORROWED;
            } else if (current_word_is(parser, "STATIC")) {
                (void)take_token(parser);
                function.result_ownership = OM_NATIVE_OWNERSHIP_STATIC;
            } else if (current_word_is(parser, "OWNED")) {
                (void)take_token(parser);
                if (!expect_word(parser, "WITH")
                    || !expect_name(parser, OM_NAME_CLASS_ORDINARY,
                                    &function.release_function, NULL)) {
                    synchronize_line(parser);
                    continue;
                }
                function.result_ownership = OM_NATIVE_OWNERSHIP_OWNED;
            } else {
                add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805);
                synchronize_line(parser);
                continue;
            }
        } else if (current_word_is(parser, "CALLBACK")) {
            if (!parse_callback_binding(parser, function.parameter_start,
                                        function.parameter_count)) {
                synchronize_line(parser);
                continue;
            }
        } else {
            add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
            synchronize_line(parser);
            continue;
        }
        (void)expect_line_end(parser);
    }
    function.callback_binding_count = parser->callback_bindings.length
        - function.callback_binding_start;
    if (!ended) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801, function.span);
    }
    validate_function_metadata(parser, &function, has_symbol, has_effect,
                               has_blocking, has_return_annotation);
    if (!push_value(parser, &parser->functions, &function)) {
        return false;
    }
    return ended;
}

static bool parse_record(Om_Omi_Parser *parser)
{
    const Om_Omi_Token *start_token = peek_token(parser);
    Om_Omi_Native_Record record = {0};
    record.span = start_token != NULL ? start_token->span
                                     : source_span(parser, 0, 0);
    record.field_start = parser->record_fields.length;
    (void)take_token(parser); /* NATIVE */
    if (!expect_word(parser, "RECORD")
        || !expect_name(parser, OM_NAME_CLASS_ENTITY, &record.name, NULL)
        || !expect_line_end(parser)) {
        synchronize_line(parser);
        return false;
    }
    bool ended = false;
    while (peek_token(parser) != NULL
           && peek_token(parser)->kind != OM_OMI_TOKEN_EOF
           && parser->status == OM_STATUS_OK) {
        skip_newlines(parser);
        const Om_Omi_Token *third_end_token = peek_token_at(parser, 2);
        if (current_end_pair_is(parser, "NATIVE")
            && third_end_token != NULL
            && third_end_token->kind == OM_OMI_TOKEN_WORD
            && word_equals(third_end_token->text, "RECORD")) {
            (void)take_token(parser);
            (void)take_token(parser);
            (void)take_token(parser);
            (void)expect_line_end(parser);
            ended = true;
            break;
        }
        if (current_end_pair_is(parser, "NATIVE")) {
            add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
            break;
        }
        Om_Omi_Record_Field field = {0};
        Om_Source_Span name_span = {0};
        if (!expect_name(parser, OM_NAME_CLASS_ORDINARY, &field.name,
                         &name_span)
            || !expect_token_kind(parser, OM_OMI_TOKEN_COLON)) {
            synchronize_line(parser);
            continue;
        }
        field.span = name_span;
        Om_Source_Span type_span = {0};
        field.abi_type = parse_abi_type(parser, &type_span, false, NULL);
        field.span.end = type_span.end;
        if (field.abi_type == OM_NATIVE_ABI_INVALID) {
            synchronize_line(parser);
            continue;
        }
        field.ownership = parse_ownership(parser);
        if (is_pointer_type(field.abi_type)
            != (field.ownership != OM_NATIVE_OWNERSHIP_NONE)) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805, field.span);
        }
        if (field.abi_type == OM_NATIVE_ABI_CVOID) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, type_span);
        }
        if (!expect_line_end(parser)) {
            continue;
        }
        for (size_t index = record.field_start;
             index < parser->record_fields.length; ++index) {
            const Om_Omi_Record_Field *existing = om_vector_at_const(
                &parser->record_fields, index);
            if (existing != NULL
                && om_name_equal_case_insensitive(existing->name,
                                                   field.name)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               field.span);
                break;
            }
        }
        if (!push_value(parser, &parser->record_fields, &field)) {
            return false;
        }
    }
    record.field_count = parser->record_fields.length - record.field_start;
    if (record.field_count == 0) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804, record.span);
    }
    if (!ended) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801, record.span);
    }
    if (!push_value(parser, &parser->records, &record)) {
        return false;
    }
    return ended;
}

static const Om_Omi_Native_Function *find_function_by_name(
    const Om_Omi_Parser *parser, Om_String_View name)
{
    for (size_t index = 0; index < parser->functions.length; ++index) {
        const Om_Omi_Native_Function *function = om_vector_at_const(
            &parser->functions, index);
        if (function != NULL
            && om_name_equal_case_insensitive(function->name, name)) {
            return function;
        }
    }
    return NULL;
}

static void validate_release_functions(Om_Omi_Parser *parser)
{
    for (size_t index = 0; index < parser->functions.length; ++index) {
        const Om_Omi_Native_Function *function = om_vector_at_const(
            &parser->functions, index);
        if (function == NULL
            || function->result_ownership != OM_NATIVE_OWNERSHIP_OWNED) {
            continue;
        }
        const Om_Omi_Native_Function *release = find_function_by_name(
            parser, function->release_function);
        bool valid = release != NULL
            && release->result_type == OM_NATIVE_ABI_CVOID
            && release->parameter_count == 1;
        if (valid) {
            const Om_Omi_Parameter *parameter = om_vector_at_const(
                &parser->parameters, release->parameter_start);
            valid = parameter != NULL && !parameter->is_callback
                && is_pointer_type(parameter->abi_type)
                && parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED;
        }
        if (!valid) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0805,
                           function->span);
        }
    }
}

static void validate_unique_declarations(Om_Omi_Parser *parser)
{
    for (size_t index = 0; index < parser->functions.length; ++index) {
        const Om_Omi_Native_Function *function = om_vector_at_const(
            &parser->functions, index);
        if (function == NULL) {
            continue;
        }
        for (size_t prior = 0; prior < index; ++prior) {
            const Om_Omi_Native_Function *earlier = om_vector_at_const(
                &parser->functions, prior);
            if (earlier != NULL
                && om_name_equal_case_insensitive(earlier->name,
                                                   function->name)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               function->span);
            }
            if (earlier != NULL && function->symbol.length != 0
                && om_string_view_equal(earlier->symbol, function->symbol)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               function->span);
            }
        }
        for (size_t record_index = 0;
             record_index < parser->records.length; ++record_index) {
            const Om_Omi_Native_Record *record = om_vector_at_const(
                &parser->records, record_index);
            if (record != NULL
                && om_name_equal_case_insensitive(record->name,
                                                   function->name)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               function->span);
            }
        }
    }
    for (size_t index = 0; index < parser->records.length; ++index) {
        const Om_Omi_Native_Record *record = om_vector_at_const(
            &parser->records, index);
        if (record == NULL) {
            continue;
        }
        for (size_t prior = 0; prior < index; ++prior) {
            const Om_Omi_Native_Record *earlier = om_vector_at_const(
                &parser->records, prior);
            if (earlier != NULL
                && om_name_equal_case_insensitive(earlier->name,
                                                   record->name)) {
                add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0807,
                               record->span);
            }
        }
    }
}

static size_t find_record_index(const Om_Omi_Parser *parser,
                                Om_String_View record_name)
{
    for (size_t index = 0; index < parser->records.length; ++index) {
        const Om_Omi_Native_Record *record = om_vector_at_const(
            &parser->records, index);
        if (record != NULL
            && om_name_equal_case_insensitive(record->name, record_name)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static void validate_record_type_references(Om_Omi_Parser *parser)
{
    for (size_t index = 0; index < parser->parameters.length; ++index) {
        Om_Omi_Parameter *parameter = om_vector_at(
            &parser->parameters, index);
        if (parameter == NULL
            || parameter->abi_type != OM_NATIVE_ABI_RECORD) {
            continue;
        }
        parameter->record_index = find_record_index(
            parser, parameter->record_name);
        if (parameter->record_index == SIZE_MAX) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804,
                           parameter->span);
        }
    }
    for (size_t index = 0; index < parser->functions.length; ++index) {
        Om_Omi_Native_Function *function = om_vector_at(
            &parser->functions, index);
        if (function == NULL
            || function->result_type != OM_NATIVE_ABI_RECORD) {
            continue;
        }
        function->result_record_index = find_record_index(
            parser, function->result_record_name);
        if (function->result_record_index == SIZE_MAX) {
            add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0804,
                           function->result_type_span);
        }
    }
}

static bool parse_module_body(Om_Omi_Parser *parser)
{
    skip_newlines(parser);
    const Om_Omi_Token *header = peek_token(parser);
    if (!expect_word(parser, "NATIVE")) {
        synchronize_line(parser);
        return false;
    }
    const Om_Omi_Token *path_token = peek_token(parser);
    if (path_token == NULL || path_token->kind != OM_OMI_TOKEN_STRING) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808);
        synchronize_line(parser);
        return false;
    }
    if (!decode_string(parser, path_token, &parser->library_path)) {
        synchronize_line(parser);
        return false;
    }
    if (parser->library_path.length == 0
        || string_contains_nul(parser->library_path)) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0808, path_token->span);
    }
    (void)take_token(parser);
    if (!expect_line_end(parser)) {
        return false;
    }
    bool ended = false;
    while (peek_token(parser) != NULL
           && peek_token(parser)->kind != OM_OMI_TOKEN_EOF
           && parser->status == OM_STATUS_OK) {
        skip_newlines(parser);
        if (current_end_pair_is(parser, "NATIVE")) {
            (void)take_token(parser);
            (void)take_token(parser);
            (void)expect_line_end(parser);
            ended = true;
            break;
        }
        if (current_word_is(parser, "FUNCTION")) {
            if (!parse_function(parser)) {
                synchronize_line(parser);
            }
            continue;
        }
        if (current_word_is(parser, "NATIVE")
            && peek_token_at(parser, 1) != NULL
            && peek_token_at(parser, 1)->kind == OM_OMI_TOKEN_WORD
            && word_equals(peek_token_at(parser, 1)->text, "RECORD")) {
            if (!parse_record(parser)) {
                synchronize_line(parser);
            }
            continue;
        }
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
        synchronize_line(parser);
    }
    if (!ended) {
        add_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801,
                       header != NULL ? header->span
                                      : source_span(parser, 0, 0));
        return false;
    }
    skip_newlines(parser);
    if (peek_token(parser) != NULL
        && peek_token(parser)->kind != OM_OMI_TOKEN_EOF) {
        add_current_diagnostic(parser, OM_OMI_DIAGNOSTIC_E0801);
        synchronize_line(parser);
    }
    validate_unique_declarations(parser);
    validate_release_functions(parser);
    validate_record_type_references(parser);
    return true;
}

static Om_Status copy_vector_to_arena(Om_Omi_Storage *storage,
                                      const Om_Vector *vector,
                                      size_t alignment,
                                      const void **output)
{
    *output = NULL;
    if (vector->length == 0) {
        return OM_STATUS_OK;
    }
    size_t byte_count = 0;
    if (!om_checked_size_mul(vector->length, vector->item_size,
                             &byte_count)) {
        return OM_STATUS_OVERFLOW;
    }
    void *allocation = NULL;
    const Om_Status status = om_arena_alloc(&storage->arena, byte_count,
                                            alignment, &allocation);
    if (status != OM_STATUS_OK) {
        return status;
    }
    (void)memcpy(allocation, vector->data, byte_count);
    *output = allocation;
    return OM_STATUS_OK;
}

static Om_Status initialize_parser(Om_Omi_Parser *parser)
{
    Om_Status status = om_vector_init(&parser->tokens,
                                     sizeof(Om_Omi_Token));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->functions,
                                sizeof(Om_Omi_Native_Function));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->parameters,
                                sizeof(Om_Omi_Parameter));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->callback_parameters,
                                sizeof(Om_Omi_Callback_Parameter));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->callback_bindings,
                                sizeof(Om_Omi_Callback_Binding));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->records,
                                sizeof(Om_Omi_Native_Record));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->record_fields,
                                sizeof(Om_Omi_Record_Field));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&parser->diagnostics,
                                sizeof(Om_Omi_Diagnostic));
    }
    return status;
}

static void destroy_parser(Om_Omi_Parser *parser)
{
    om_vector_destroy(&parser->tokens);
    om_vector_destroy(&parser->functions);
    om_vector_destroy(&parser->parameters);
    om_vector_destroy(&parser->callback_parameters);
    om_vector_destroy(&parser->callback_bindings);
    om_vector_destroy(&parser->records);
    om_vector_destroy(&parser->record_fields);
    om_vector_destroy(&parser->diagnostics);
}

static Om_Status finish_module(Om_Omi_Parser *parser, Om_Omi_Module *module)
{
    const void *array = NULL;
    Om_Status status = copy_vector_to_arena(
        parser->storage, &parser->functions, _Alignof(Om_Omi_Native_Function),
        &array);
    module->functions = array;
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->parameters,
            _Alignof(Om_Omi_Parameter), &array);
        module->parameters = array;
    }
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->callback_parameters,
            _Alignof(Om_Omi_Callback_Parameter), &array);
        module->callback_parameters = array;
    }
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->callback_bindings,
            _Alignof(Om_Omi_Callback_Binding), &array);
        module->callback_bindings = array;
    }
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->records,
            _Alignof(Om_Omi_Native_Record), &array);
        module->records = array;
    }
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->record_fields,
            _Alignof(Om_Omi_Record_Field), &array);
        module->record_fields = array;
    }
    if (status == OM_STATUS_OK) {
        status = copy_vector_to_arena(
            parser->storage, &parser->diagnostics,
            _Alignof(Om_Omi_Diagnostic), &array);
        module->diagnostics = array;
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    module->library_path = parser->library_path;
    module->function_count = parser->functions.length;
    module->parameter_count = parser->parameters.length;
    module->callback_parameter_count = parser->callback_parameters.length;
    module->callback_binding_count = parser->callback_bindings.length;
    module->record_count = parser->records.length;
    module->record_field_count = parser->record_fields.length;
    module->diagnostic_count = parser->diagnostics.length;
    module->owned_storage = parser->storage;
    parser->storage = NULL;
    return OM_STATUS_OK;
}

Om_Status om_omi_parse(const Om_Source_Text *source, Om_Omi_Module *module)
{
    if (source == NULL || module == NULL || module->owned_storage != NULL
        || (source->bytes.length != 0 && source->bytes.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (source->bytes.length > UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Status utf8_status = om_utf8_validate(source->bytes);
    if (utf8_status != OM_STATUS_OK) {
        return utf8_status;
    }
    *module = (Om_Omi_Module){0};
    Om_Omi_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Status arena_status = om_arena_init(&storage->arena, 4096);
    if (arena_status != OM_STATUS_OK) {
        om_free(storage);
        return arena_status;
    }
    Om_Omi_Parser parser = {
        .source = source,
        .storage = storage,
        .status = OM_STATUS_OK
    };
    Om_Status status = initialize_parser(&parser);
    if (status == OM_STATUS_OK) {
        status = scan_tokens(&parser);
    }
    if (status == OM_STATUS_OK) {
        (void)parse_module_body(&parser);
        status = parser.status;
    }
    if (status == OM_STATUS_OK) {
        status = finish_module(&parser, module);
    }
    destroy_parser(&parser);
    if (status != OM_STATUS_OK) {
        om_arena_destroy(&storage->arena);
        om_free(storage);
        *module = (Om_Omi_Module){0};
    }
    return status;
}

void om_omi_module_destroy(Om_Omi_Module *module)
{
    if (module == NULL) {
        return;
    }
    Om_Omi_Storage *storage = module->owned_storage;
    if (storage != NULL) {
        om_arena_destroy(&storage->arena);
        om_free(storage);
    }
    *module = (Om_Omi_Module){0};
}

const char *om_omi_diagnostic_code_name(Om_Omi_Diagnostic_Code code)
{
    switch (code) {
    case OM_OMI_DIAGNOSTIC_E0801: return "E0801";
    case OM_OMI_DIAGNOSTIC_E0802: return "E0802";
    case OM_OMI_DIAGNOSTIC_E0803: return "E0803";
    case OM_OMI_DIAGNOSTIC_E0804: return "E0804";
    case OM_OMI_DIAGNOSTIC_E0805: return "E0805";
    case OM_OMI_DIAGNOSTIC_E0806: return "E0806";
    case OM_OMI_DIAGNOSTIC_E0807: return "E0807";
    case OM_OMI_DIAGNOSTIC_E0808: return "E0808";
    }
    return "E0000";
}
