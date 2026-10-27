#if !defined(_WIN32)
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "ombasic/syntax/lexer.h"

#include <math.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "ombasic/syntax/name_rules.h"
#include "ombasic/foundation/vector.h"

#if defined(_WIN32)
typedef _locale_t Om_C_Locale;
#else
typedef locale_t Om_C_Locale;
#endif

typedef struct Om_Keyword_Entry {
    const char *spelling;
    size_t length;
    Om_Keyword keyword;
} Om_Keyword_Entry;

typedef struct Om_Lexer_State {
    const Om_Source_Text *source;
    Om_Vector tokens;
    Om_Vector diagnostics;
    size_t cursor;
    bool line_has_code;
    Om_C_Locale c_locale;
} Om_Lexer_State;

static const Om_Keyword_Entry KEYWORDS[] = {
    {"ACTION", 6, OM_KEYWORD_ACTION},
    {"ACTOR", 5, OM_KEYWORD_ACTOR},
    {"AND", 3, OM_KEYWORD_AND},
    {"AT", 2, OM_KEYWORD_AT},
    {"BREAK", 5, OM_KEYWORD_BREAK},
    {"CASE", 4, OM_KEYWORD_CASE},
    {"CONTINUE", 8, OM_KEYWORD_CONTINUE},
    {"ELSE", 4, OM_KEYWORD_ELSE},
    {"END", 3, OM_KEYWORD_END},
    {"EXPORT", 6, OM_KEYWORD_EXPORT},
    {"FAILED", 6, OM_KEYWORD_FAILED},
    {"FALSE", 5, OM_KEYWORD_FALSE},
    {"FOR", 3, OM_KEYWORD_FOR},
    {"FROM", 4, OM_KEYWORD_FROM},
    {"FUNCTION", 8, OM_KEYWORD_FUNCTION},
    {"IF", 2, OM_KEYWORD_IF},
    {"IMPORT", 6, OM_KEYWORD_IMPORT},
    {"IN", 2, OM_KEYWORD_IN},
    {"MESSAGE", 7, OM_KEYWORD_MESSAGE},
    {"NATIVE", 6, OM_KEYWORD_NATIVE},
    {"NODE_UNREACHABLE", 16, OM_KEYWORD_NODE_UNREACHABLE},
    {"NOT", 3, OM_KEYWORD_NOT},
    {"NORMAL", 6, OM_KEYWORD_NORMAL},
    {"NULL", 4, OM_KEYWORD_NULL},
    {"OR", 2, OM_KEYWORD_OR},
    {"RECORD", 6, OM_KEYWORD_RECORD},
    {"RECEIVE", 7, OM_KEYWORD_RECEIVE},
    {"RETURN", 6, OM_KEYWORD_RETURN},
    {"SELECT", 6, OM_KEYWORD_SELECT},
    {"SELF", 4, OM_KEYWORD_SELF},
    {"SEND", 4, OM_KEYWORD_SEND},
    {"SPAWN", 5, OM_KEYWORD_SPAWN},
    {"STOP", 4, OM_KEYWORD_STOP},
    {"STEP", 4, OM_KEYWORD_STEP},
    {"THEN", 4, OM_KEYWORD_THEN},
    {"TO", 2, OM_KEYWORD_TO},
    {"TRUE", 4, OM_KEYWORD_TRUE},
    {"TRY", 3, OM_KEYWORD_TRY},
    {"VAR", 3, OM_KEYWORD_VAR},
    {"WATCH", 5, OM_KEYWORD_WATCH},
    {"WHILE", 5, OM_KEYWORD_WHILE},
    {"WITH", 4, OM_KEYWORD_WITH}
};

static bool is_ascii_digit(uint8_t byte)
{
    return byte >= (uint8_t)'0' && byte <= (uint8_t)'9';
}

static bool is_ascii_lower(uint8_t byte)
{
    return byte >= (uint8_t)'a' && byte <= (uint8_t)'z';
}

static bool is_ascii_upper(uint8_t byte)
{
    return byte >= (uint8_t)'A' && byte <= (uint8_t)'Z';
}

static bool is_ascii_alpha(uint8_t byte)
{
    return is_ascii_lower(byte) || is_ascii_upper(byte);
}

static bool is_identifier_start(uint8_t byte)
{
    return is_ascii_alpha(byte) || byte == (uint8_t)'_';
}

static bool is_identifier_continue(uint8_t byte)
{
    return is_ascii_alpha(byte) || is_ascii_digit(byte) || byte == (uint8_t)'_';
}

static bool is_horizontal_space(uint8_t byte)
{
    return byte == (uint8_t)' ' || byte == (uint8_t)'\t';
}

static bool is_line_ending(uint8_t byte)
{
    return byte == (uint8_t)'\n' || byte == (uint8_t)'\r';
}

static int hex_value(uint8_t byte)
{
    if (is_ascii_digit(byte)) {
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

static Om_Status emit_token(
    Om_Lexer_State *lexer,
    Om_Token_Kind kind,
    Om_Keyword keyword,
    size_t start,
    size_t end,
    uint64_t integer_magnitude,
    double float_value,
    bool integer_requires_negative
)
{
    Om_Token token = {
        .kind = kind,
        .keyword = keyword,
        .span = {
            .source = lexer->source->id,
            .start = (uint32_t)start,
            .end = (uint32_t)end
        },
        .lexeme = {
            .data = (const char *)lexer->source->bytes.data + start,
            .length = end - start
        },
        .integer_magnitude = integer_magnitude,
        .float_value = float_value,
        .integer_requires_negative = integer_requires_negative
    };
    const Om_Status status = om_vector_push(&lexer->tokens, &token);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (kind != OM_TOKEN_NEWLINE && kind != OM_TOKEN_DOC_COMMENT && kind != OM_TOKEN_EOF) {
        lexer->line_has_code = true;
    }
    return OM_STATUS_OK;
}

static Om_Status emit_diagnostic(
    Om_Lexer_State *lexer,
    Om_Lex_Diagnostic_Code code,
    size_t start,
    size_t end
)
{
    const Om_Lex_Diagnostic diagnostic = {
        .code = code,
        .span = {
            .source = lexer->source->id,
            .start = (uint32_t)start,
            .end = (uint32_t)end
        }
    };
    return om_vector_push(&lexer->diagnostics, &diagnostic);
}

static Om_Keyword find_keyword(const uint8_t *bytes, size_t length, bool *wrong_case)
{
    if (wrong_case != NULL) {
        *wrong_case = false;
    }
    for (size_t entry_index = 0; entry_index < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]);
         ++entry_index) {
        const Om_Keyword_Entry *entry = &KEYWORDS[entry_index];
        if (entry->length != length) {
            continue;
        }
        bool equal_ignore_case = true;
        bool equal_exact = true;
        for (size_t index = 0; index < length; ++index) {
            uint8_t upper = bytes[index];
            if (upper >= (uint8_t)'a' && upper <= (uint8_t)'z') {
                upper = (uint8_t)(upper - (uint8_t)'a' + (uint8_t)'A');
            }
            if (upper != (uint8_t)entry->spelling[index]) {
                equal_ignore_case = false;
                break;
            }
            if (bytes[index] != (uint8_t)entry->spelling[index]) {
                equal_exact = false;
            }
        }
        if (equal_ignore_case) {
            if (wrong_case != NULL) {
                *wrong_case = !equal_exact;
            }
            return entry->keyword;
        }
    }
    return OM_KEYWORD_NONE;
}

static Om_Status scan_comment(Om_Lexer_State *lexer, size_t *cursor)
{
    while (*cursor < lexer->source->bytes.length) {
        const uint8_t byte = lexer->source->bytes.data[*cursor];
        if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
            break;
        }
        size_t next = *cursor;
        uint32_t scalar = 0;
        const Om_Status status = om_utf8_decode_next(lexer->source->bytes, &next, &scalar);
        (void)scalar;
        if (status == OM_STATUS_OK) {
            *cursor = next;
        } else {
            const Om_Status diagnostic_status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                *cursor,
                *cursor + 1
            );
            if (diagnostic_status != OM_STATUS_OK) {
                return diagnostic_status;
            }
            ++*cursor;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status scan_escape(Om_Lexer_State *lexer, size_t *cursor)
{
    const size_t start = *cursor;
    const size_t length = lexer->source->bytes.length;
    ++*cursor;
    bool valid = true;
    if (*cursor >= length) {
        valid = false;
    } else {
        const uint8_t escape = lexer->source->bytes.data[(*cursor)++];
        if (escape == (uint8_t)'u') {
            if (*cursor >= length || lexer->source->bytes.data[*cursor] != (uint8_t)'{') {
                valid = false;
            } else {
                ++*cursor;
                uint32_t scalar = 0;
                size_t digits = 0;
                while (*cursor < length && lexer->source->bytes.data[*cursor] != (uint8_t)'}'
                       && lexer->source->bytes.data[*cursor] != (uint8_t)'"'
                       && !is_line_ending(lexer->source->bytes.data[*cursor])) {
                    const int digit = hex_value(lexer->source->bytes.data[*cursor]);
                    if (digit < 0 || digits == 6
                        || scalar > (UINT32_C(0x10ffff) - (uint32_t)digit) / UINT32_C(16)) {
                        valid = false;
                    } else {
                        scalar = scalar * UINT32_C(16) + (uint32_t)digit;
                        ++digits;
                    }
                    ++*cursor;
                }
                if (*cursor >= length
                    || lexer->source->bytes.data[*cursor] != (uint8_t)'}'
                    || digits == 0 || (scalar >= UINT32_C(0xd800)
                                       && scalar <= UINT32_C(0xdfff))) {
                    valid = false;
                } else {
                    ++*cursor;
                }
            }
        } else if (escape != (uint8_t)'\\' && escape != (uint8_t)'"'
                   && escape != (uint8_t)'n' && escape != (uint8_t)'r'
                   && escape != (uint8_t)'t' && escape != (uint8_t)'0') {
            valid = false;
        }
    }
    if (!valid) {
        return emit_diagnostic(lexer, OM_LEX_DIAGNOSTIC_E0103, start, *cursor);
    }
    return OM_STATUS_OK;
}

static Om_Status scan_string_literal(Om_Lexer_State *lexer)
{
    const size_t start = lexer->cursor;
    const size_t length = lexer->source->bytes.length;
    bool terminated = false;
    ++lexer->cursor;
    while (lexer->cursor < length) {
        const uint8_t byte = lexer->source->bytes.data[lexer->cursor];
        if (byte == (uint8_t)'"') {
            ++lexer->cursor;
            terminated = true;
            break;
        }
        if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
            break;
        }
        if (byte == (uint8_t)'\\') {
            const Om_Status status = scan_escape(lexer, &lexer->cursor);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        size_t next = lexer->cursor;
        uint32_t scalar = 0;
        const Om_Status utf8_status = om_utf8_decode_next(lexer->source->bytes, &next, &scalar);
        (void)scalar;
        if (utf8_status == OM_STATUS_OK) {
            lexer->cursor = next;
        } else {
            const Om_Status status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                lexer->cursor,
                lexer->cursor + 1
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            ++lexer->cursor;
        }
    }
    if (!terminated) {
        const Om_Status status = emit_diagnostic(
            lexer,
            OM_LEX_DIAGNOSTIC_E0103,
            start,
            lexer->cursor
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
        return emit_token(
            lexer,
            OM_TOKEN_ERROR,
            OM_KEYWORD_NONE,
            start,
            lexer->cursor,
            0,
            0.0,
            false
        );
    }
    return emit_token(
        lexer,
        OM_TOKEN_STRING,
        OM_KEYWORD_NONE,
        start,
        lexer->cursor,
        0,
        0.0,
        false
    );
}

static Om_Status scan_tokens(
    Om_Lexer_State *lexer,
    bool stop_at_interpolation_end,
    size_t *delimiter_depth,
    bool *stopped_at_interpolation_end
);

static Om_Status scan_interpolated_string(Om_Lexer_State *lexer)
{
    const size_t start = lexer->cursor;
    Om_Status status = emit_token(
        lexer,
        OM_TOKEN_INTERPOLATED_STRING_START,
        OM_KEYWORD_NONE,
        start,
        start + 2,
        0,
        0.0,
        false
    );
    if (status != OM_STATUS_OK) {
        return status;
    }
    lexer->cursor += 2;
    size_t text_start = lexer->cursor;
    const size_t length = lexer->source->bytes.length;
    while (lexer->cursor < length) {
        const uint8_t byte = lexer->source->bytes.data[lexer->cursor];
        if (byte == (uint8_t)'"') {
            if (lexer->cursor > text_start) {
                status = emit_token(
                    lexer,
                    OM_TOKEN_STRING_TEXT,
                    OM_KEYWORD_NONE,
                    text_start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            const size_t end = lexer->cursor++;
            return emit_token(
                lexer,
                OM_TOKEN_INTERPOLATED_STRING_END,
                OM_KEYWORD_NONE,
                end,
                end + 1,
                0,
                0.0,
                false
            );
        }
        if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
            status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                start,
                lexer->cursor + 1
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            if (lexer->cursor > text_start) {
                status = emit_token(
                    lexer,
                    OM_TOKEN_STRING_TEXT,
                    OM_KEYWORD_NONE,
                    text_start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            return OM_STATUS_OK;
        }
        if (byte == (uint8_t)'\\') {
            status = scan_escape(lexer, &lexer->cursor);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (byte == (uint8_t)'{' && lexer->cursor + 1 < length
            && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'{') {
            lexer->cursor += 2;
            continue;
        }
        if (byte == (uint8_t)'}' && lexer->cursor + 1 < length
            && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'}') {
            lexer->cursor += 2;
            continue;
        }
        if (byte == (uint8_t)'{') {
            if (lexer->cursor > text_start) {
                status = emit_token(
                    lexer,
                    OM_TOKEN_STRING_TEXT,
                    OM_KEYWORD_NONE,
                    text_start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            const size_t open = lexer->cursor++;
            status = emit_token(
                lexer,
                OM_TOKEN_INTERPOLATION_START,
                OM_KEYWORD_NONE,
                open,
                open + 1,
                0,
                0.0,
                false
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            size_t delimiter_depth = 0;
            bool stopped = false;
            status = scan_tokens(lexer, true, &delimiter_depth, &stopped);
            if (status != OM_STATUS_OK) {
                return status;
            }
            if (!stopped) {
                return emit_diagnostic(
                    lexer,
                    OM_LEX_DIAGNOSTIC_E0103,
                    open,
                    lexer->source->bytes.length
                );
            }
            const size_t close = lexer->cursor++;
            status = emit_token(
                lexer,
                OM_TOKEN_INTERPOLATION_END,
                OM_KEYWORD_NONE,
                close,
                close + 1,
                0,
                0.0,
                false
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            text_start = lexer->cursor;
            continue;
        }
        if (byte == (uint8_t)'}') {
            status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                lexer->cursor,
                lexer->cursor + 1
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            ++lexer->cursor;
            continue;
        }
        size_t next = lexer->cursor;
        uint32_t scalar = 0;
        const Om_Status utf8_status = om_utf8_decode_next(lexer->source->bytes, &next, &scalar);
        (void)scalar;
        if (utf8_status == OM_STATUS_OK) {
            lexer->cursor = next;
        } else {
            status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                lexer->cursor,
                lexer->cursor + 1
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
            ++lexer->cursor;
        }
    }
    if (lexer->cursor > text_start) {
        status = emit_token(
            lexer,
            OM_TOKEN_STRING_TEXT,
            OM_KEYWORD_NONE,
            text_start,
            lexer->cursor,
            0,
            0.0,
            false
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return emit_diagnostic(
        lexer,
        OM_LEX_DIAGNOSTIC_E0103,
        start,
        lexer->source->bytes.length
    );
}

static Om_C_Locale create_c_locale(void)
{
#if defined(_WIN32)
    return _create_locale(LC_NUMERIC, "C");
#else
    return newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
}

static void destroy_c_locale(Om_C_Locale locale)
{
    if (locale == NULL) {
        return;
    }
#if defined(_WIN32)
    _free_locale(locale);
#else
    (void)freelocale(locale);
#endif
}

static double parse_c_float(const char *text, char **end, Om_C_Locale locale)
{
#if defined(_WIN32)
    return _strtod_l(text, end, locale);
#else
    return strtod_l(text, end, locale);
#endif
}

static Om_Status parse_float_value(
    Om_Lexer_State *lexer,
    size_t start,
    size_t end,
    double *value
)
{
    size_t allocation_size = 0;
    char *text = NULL;
    if (lexer->c_locale == NULL) {
        lexer->c_locale = create_c_locale();
        if (lexer->c_locale == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (!om_checked_size_add(end - start, 1, &allocation_size)) {
        return OM_STATUS_OVERFLOW;
    }
    text = om_alloc(allocation_size);
    if (text == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(text, lexer->source->bytes.data + start, end - start);
    text[end - start] = '\0';
    char *parsed_end = NULL;
    *value = parse_c_float(text, &parsed_end, lexer->c_locale);
    const bool fully_parsed = parsed_end == text + (end - start);
    om_free(text);
    return fully_parsed ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status scan_number(Om_Lexer_State *lexer)
{
    const size_t start = lexer->cursor;
    const size_t length = lexer->source->bytes.length;
    bool is_float = false;
    bool malformed = false;
    while (lexer->cursor < length && is_ascii_digit(lexer->source->bytes.data[lexer->cursor])) {
        ++lexer->cursor;
    }
    if (lexer->cursor + 1 < length && lexer->source->bytes.data[lexer->cursor] == (uint8_t)'.'
        && is_ascii_digit(lexer->source->bytes.data[lexer->cursor + 1])) {
        is_float = true;
        lexer->cursor += 2;
        while (lexer->cursor < length && is_ascii_digit(lexer->source->bytes.data[lexer->cursor])) {
            ++lexer->cursor;
        }
    }
    if (lexer->cursor < length
        && (lexer->source->bytes.data[lexer->cursor] == (uint8_t)'e'
            || lexer->source->bytes.data[lexer->cursor] == (uint8_t)'E')) {
        is_float = true;
        ++lexer->cursor;
        if (lexer->cursor < length
            && (lexer->source->bytes.data[lexer->cursor] == (uint8_t)'+'
                || lexer->source->bytes.data[lexer->cursor] == (uint8_t)'-')) {
            ++lexer->cursor;
        }
        const size_t exponent_digits = lexer->cursor;
        while (lexer->cursor < length && is_ascii_digit(lexer->source->bytes.data[lexer->cursor])) {
            ++lexer->cursor;
        }
        if (lexer->cursor == exponent_digits) {
            malformed = true;
        }
    }
    if (lexer->cursor < length
        && (is_ascii_alpha(lexer->source->bytes.data[lexer->cursor])
            || lexer->source->bytes.data[lexer->cursor] == (uint8_t)'_')) {
        malformed = true;
        while (lexer->cursor < length && is_identifier_continue(lexer->source->bytes.data[lexer->cursor])) {
            ++lexer->cursor;
        }
    }
    if (malformed) {
        Om_Status status = emit_diagnostic(
            lexer,
            OM_LEX_DIAGNOSTIC_E0103,
            start,
            lexer->cursor
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
        return emit_token(
            lexer,
            OM_TOKEN_ERROR,
            OM_KEYWORD_NONE,
            start,
            lexer->cursor,
            0,
            0.0,
            false
        );
    }
    if (is_float) {
        double value = 0.0;
        Om_Status status = parse_float_value(lexer, start, lexer->cursor, &value);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (!isfinite(value)) {
            status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0104,
                start,
                lexer->cursor
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return emit_token(
            lexer,
            OM_TOKEN_FLOAT,
            OM_KEYWORD_NONE,
            start,
            lexer->cursor,
            0,
            value,
            false
        );
    }
    uint64_t magnitude = 0;
    bool overflow = false;
    for (size_t index = start; index < lexer->cursor; ++index) {
        const uint64_t digit = (uint64_t)(lexer->source->bytes.data[index] - (uint8_t)'0');
        if (magnitude > (UINT64_MAX - digit) / UINT64_C(10)) {
            overflow = true;
        } else if (!overflow) {
            magnitude = magnitude * UINT64_C(10) + digit;
        }
    }
    const uint64_t int64_limit = UINT64_C(9223372036854775808);
    if (overflow || magnitude > int64_limit) {
        const Om_Status status = emit_diagnostic(
            lexer,
            OM_LEX_DIAGNOSTIC_E0104,
            start,
            lexer->cursor
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return emit_token(
        lexer,
        OM_TOKEN_INTEGER,
        OM_KEYWORD_NONE,
        start,
        lexer->cursor,
        magnitude,
        0.0,
        magnitude == int64_limit
    );
}

static Om_Status scan_identifier(Om_Lexer_State *lexer)
{
    const size_t start = lexer->cursor;
    while (lexer->cursor < lexer->source->bytes.length
           && is_identifier_continue(lexer->source->bytes.data[lexer->cursor])) {
        ++lexer->cursor;
    }
    const size_t length = lexer->cursor - start;
    bool wrong_case = false;
    const Om_Keyword keyword = find_keyword(
        lexer->source->bytes.data + start,
        length,
        &wrong_case
    );
    if (keyword != OM_KEYWORD_NONE) {
        if (wrong_case) {
            const Om_Status status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0101,
                start,
                lexer->cursor
            );
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return emit_token(
            lexer,
            OM_TOKEN_KEYWORD,
            keyword,
            start,
            lexer->cursor,
            0,
            0.0,
            false
        );
    }
    const Om_String_View name = {
        (const char *)lexer->source->bytes.data + start,
        length
    };
    const Om_Name_Class name_class = om_name_classify(name);
    Om_Token_Kind kind = OM_TOKEN_INVALID_IDENTIFIER;
    if (name_class == OM_NAME_CLASS_ORDINARY) {
        kind = OM_TOKEN_IDENTIFIER;
    } else if (name_class == OM_NAME_CLASS_ENTITY) {
        kind = OM_TOKEN_ENTITY_IDENTIFIER;
    }
    if (kind == OM_TOKEN_INVALID_IDENTIFIER) {
        const Om_Status status = emit_diagnostic(
            lexer,
            OM_LEX_DIAGNOSTIC_E0102,
            start,
            lexer->cursor
        );
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return emit_token(
        lexer,
        kind,
        OM_KEYWORD_NONE,
        start,
        lexer->cursor,
        0,
        0.0,
        false
    );
}

static Om_Status emit_fixed_token(
    Om_Lexer_State *lexer,
    Om_Token_Kind kind,
    size_t width,
    size_t *delimiter_depth
)
{
    const size_t start = lexer->cursor;
    lexer->cursor += width;
    if (kind == OM_TOKEN_LEFT_PAREN || kind == OM_TOKEN_LEFT_BRACKET
        || kind == OM_TOKEN_LEFT_BRACE) {
        ++*delimiter_depth;
    } else if ((kind == OM_TOKEN_RIGHT_PAREN || kind == OM_TOKEN_RIGHT_BRACKET
                || kind == OM_TOKEN_RIGHT_BRACE)
               && *delimiter_depth != 0) {
        --*delimiter_depth;
    }
    return emit_token(lexer, kind, OM_KEYWORD_NONE, start, lexer->cursor, 0, 0.0, false);
}

static Om_Status scan_tokens(
    Om_Lexer_State *lexer,
    bool stop_at_interpolation_end,
    size_t *delimiter_depth,
    bool *stopped_at_interpolation_end
)
{
    Om_Status status = OM_STATUS_OK;
    if (stopped_at_interpolation_end != NULL) {
        *stopped_at_interpolation_end = false;
    }
    while (lexer->cursor < lexer->source->bytes.length) {
        const uint8_t byte = lexer->source->bytes.data[lexer->cursor];
        const size_t start = lexer->cursor;
        if (stop_at_interpolation_end && byte == (uint8_t)'}' && *delimiter_depth == 0) {
            if (stopped_at_interpolation_end != NULL) {
                *stopped_at_interpolation_end = true;
            }
            return OM_STATUS_OK;
        }
        if (is_horizontal_space(byte)) {
            ++lexer->cursor;
            continue;
        }
        if (byte == (uint8_t)'\n' || byte == (uint8_t)'\r') {
            size_t newline_width = 1;
            if (byte == (uint8_t)'\r') {
                if (lexer->cursor + 1 < lexer->source->bytes.length
                    && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'\n') {
                    newline_width = 2;
                } else {
                    status = emit_diagnostic(
                        lexer,
                        OM_LEX_DIAGNOSTIC_E0103,
                        start,
                        start + 1
                    );
                    if (status != OM_STATUS_OK) {
                        return status;
                    }
                    ++lexer->cursor;
                    continue;
                }
            }
            lexer->cursor += newline_width;
            lexer->line_has_code = false;
            if (!stop_at_interpolation_end && *delimiter_depth == 0) {
                status = emit_token(
                    lexer,
                    OM_TOKEN_NEWLINE,
                    OM_KEYWORD_NONE,
                    start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            continue;
        }
        if (byte == (uint8_t)'\'') {
            const bool is_doc_comment = !lexer->line_has_code
                                        && lexer->cursor + 2 < lexer->source->bytes.length
                                        && lexer->source->bytes.data[lexer->cursor + 1]
                                               == (uint8_t)'\''
                                        && lexer->source->bytes.data[lexer->cursor + 2]
                                               == (uint8_t)'\'';
            if (is_doc_comment) {
                lexer->cursor += 3;
                status = scan_comment(lexer, &lexer->cursor);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                status = emit_token(
                    lexer,
                    OM_TOKEN_DOC_COMMENT,
                    OM_KEYWORD_NONE,
                    start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
                if (status != OM_STATUS_OK) {
                    return status;
                }
                lexer->line_has_code = false;
            } else {
                ++lexer->cursor;
                status = scan_comment(lexer, &lexer->cursor);
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            continue;
        }
        if (byte == (uint8_t)'"') {
            status = scan_string_literal(lexer);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (byte == (uint8_t)'$' && lexer->cursor + 1 < lexer->source->bytes.length
            && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'"') {
            status = scan_interpolated_string(lexer);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (is_identifier_start(byte)) {
            status = scan_identifier(lexer);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (is_ascii_digit(byte)) {
            status = scan_number(lexer);
            if (status != OM_STATUS_OK) {
                return status;
            }
            continue;
        }
        switch (byte) {
        case (uint8_t)'<':
            if (lexer->cursor + 1 < lexer->source->bytes.length
                && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'>') {
                status = emit_fixed_token(lexer, OM_TOKEN_NOT_EQUAL, 2, delimiter_depth);
            } else if (lexer->cursor + 1 < lexer->source->bytes.length
                       && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'=') {
                status = emit_fixed_token(lexer, OM_TOKEN_LESS_EQUAL, 2, delimiter_depth);
            } else {
                status = emit_fixed_token(lexer, OM_TOKEN_LESS, 1, delimiter_depth);
            }
            break;
        case (uint8_t)'>':
            if (lexer->cursor + 1 < lexer->source->bytes.length
                && lexer->source->bytes.data[lexer->cursor + 1] == (uint8_t)'=') {
                status = emit_fixed_token(lexer, OM_TOKEN_GREATER_EQUAL, 2, delimiter_depth);
            } else {
                status = emit_fixed_token(lexer, OM_TOKEN_GREATER, 1, delimiter_depth);
            }
            break;
        case (uint8_t)'+': status = emit_fixed_token(lexer, OM_TOKEN_PLUS, 1, delimiter_depth); break;
        case (uint8_t)'-': status = emit_fixed_token(lexer, OM_TOKEN_MINUS, 1, delimiter_depth); break;
        case (uint8_t)'*': status = emit_fixed_token(lexer, OM_TOKEN_STAR, 1, delimiter_depth); break;
        case (uint8_t)'/': status = emit_fixed_token(lexer, OM_TOKEN_SLASH, 1, delimiter_depth); break;
        case (uint8_t)'%': status = emit_fixed_token(lexer, OM_TOKEN_PERCENT, 1, delimiter_depth); break;
        case (uint8_t)'=': status = emit_fixed_token(lexer, OM_TOKEN_EQUAL, 1, delimiter_depth); break;
        case (uint8_t)'.': status = emit_fixed_token(lexer, OM_TOKEN_DOT, 1, delimiter_depth); break;
        case (uint8_t)'(': status = emit_fixed_token(lexer, OM_TOKEN_LEFT_PAREN, 1, delimiter_depth); break;
        case (uint8_t)')': status = emit_fixed_token(lexer, OM_TOKEN_RIGHT_PAREN, 1, delimiter_depth); break;
        case (uint8_t)'[': status = emit_fixed_token(lexer, OM_TOKEN_LEFT_BRACKET, 1, delimiter_depth); break;
        case (uint8_t)']': status = emit_fixed_token(lexer, OM_TOKEN_RIGHT_BRACKET, 1, delimiter_depth); break;
        case (uint8_t)'{': status = emit_fixed_token(lexer, OM_TOKEN_LEFT_BRACE, 1, delimiter_depth); break;
        case (uint8_t)'}': status = emit_fixed_token(lexer, OM_TOKEN_RIGHT_BRACE, 1, delimiter_depth); break;
        case (uint8_t)',': status = emit_fixed_token(lexer, OM_TOKEN_COMMA, 1, delimiter_depth); break;
        case (uint8_t)':': status = emit_fixed_token(lexer, OM_TOKEN_COLON, 1, delimiter_depth); break;
        default: {
            size_t next = lexer->cursor;
            uint32_t scalar = 0;
            const Om_Status utf8_status = om_utf8_decode_next(
                lexer->source->bytes,
                &next,
                &scalar
            );
            (void)scalar;
            if (utf8_status != OM_STATUS_OK) {
                next = lexer->cursor + 1;
            }
            lexer->cursor = next;
            status = emit_diagnostic(
                lexer,
                OM_LEX_DIAGNOSTIC_E0103,
                start,
                lexer->cursor
            );
            if (status == OM_STATUS_OK) {
                status = emit_token(
                    lexer,
                    OM_TOKEN_ERROR,
                    OM_KEYWORD_NONE,
                    start,
                    lexer->cursor,
                    0,
                    0.0,
                    false
                );
            }
            break;
        }
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

void om_lexer_result_destroy(Om_Lexer_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->tokens);
    om_free(result->diagnostics);
    *result = (Om_Lexer_Result){0};
}

const char *om_lex_diagnostic_code_name(Om_Lex_Diagnostic_Code code)
{
    switch (code) {
    case OM_LEX_DIAGNOSTIC_E0101: return "E0101";
    case OM_LEX_DIAGNOSTIC_E0102: return "E0102";
    case OM_LEX_DIAGNOSTIC_E0103: return "E0103";
    case OM_LEX_DIAGNOSTIC_E0104: return "E0104";
    default: return "E0000";
    }
}

Om_Status om_lexer_scan(const Om_Source_Text *source, Om_Lexer_Result *result)
{
    Om_Lexer_State lexer = {0};
    size_t delimiter_depth = 0;
    Om_Status status = OM_STATUS_OK;
    if (source == NULL || result == NULL || result->tokens != NULL
        || result->diagnostics != NULL || source->owned_storage == NULL
        || source->id.value == UINT32_MAX
        || (source->bytes.data == NULL && source->bytes.length != 0)
        || source->bytes.length >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_vector_init(&lexer.tokens, sizeof(Om_Token));
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_vector_init(&lexer.diagnostics, sizeof(Om_Lex_Diagnostic));
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&lexer.tokens);
        return status;
    }
    lexer.source = source;
    status = scan_tokens(&lexer, false, &delimiter_depth, NULL);
    if (status == OM_STATUS_OK) {
        status = emit_token(
            &lexer,
            OM_TOKEN_EOF,
            OM_KEYWORD_NONE,
            source->bytes.length,
            source->bytes.length,
            0,
            0.0,
            false
        );
    }
    if (status != OM_STATUS_OK) {
        destroy_c_locale(lexer.c_locale);
        om_vector_destroy(&lexer.tokens);
        om_vector_destroy(&lexer.diagnostics);
        return status;
    }
    result->tokens = lexer.tokens.data;
    result->token_count = lexer.tokens.length;
    result->diagnostics = lexer.diagnostics.data;
    result->diagnostic_count = lexer.diagnostics.length;
    lexer.tokens = (Om_Vector){0};
    lexer.diagnostics = (Om_Vector){0};
    destroy_c_locale(lexer.c_locale);
    return OM_STATUS_OK;
}
