#include "test.h"

#include <string.h>

#include "ombasic/syntax/lexer.h"

#include "ombasic/source/file_loader.h"

#define LEX_ASSERT(expression)                                                   \
    do {                                                                          \
        if (!(expression)) {                                                      \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",              \
                          __FILE__, __LINE__, #expression);                       \
            goto cleanup;                                                        \
        }                                                                         \
    } while (false)

static bool scan_text(
    Om_Source_Text *source,
    Om_Lexer_Result *result,
    const uint8_t *bytes,
    size_t length
)
{
    static const char path[] = "lexer_test.bas";
    if (om_source_text_init(
            source,
            (Om_Source_Id){3},
            (Om_String_View){path, sizeof(path) - 1},
            (Om_Const_Byte_Slice){bytes, length},
            0
        ) != OM_STATUS_OK) {
        return false;
    }
    return om_lexer_scan(source, result) == OM_STATUS_OK;
}

static size_t count_tokens(const Om_Lexer_Result *result, Om_Token_Kind kind)
{
    size_t count = 0;
    for (size_t index = 0; index < result->token_count; ++index) {
        if (result->tokens[index].kind == kind) {
            ++count;
        }
    }
    return count;
}

static bool scan_candidate(const uint8_t *bytes, size_t length, uint32_t source_id)
{
    static const char path[] = "fuzz_input.bas";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    const Om_Status source_status = om_source_text_init(
        &source,
        (Om_Source_Id){source_id},
        (Om_String_View){path, sizeof(path) - 1},
        (Om_Const_Byte_Slice){bytes, length},
        0
    );
    if (source_status != OM_STATUS_OK) {
        return false;
    }
    const Om_Status scan_status = om_lexer_scan(&source, &result);
    bool valid = scan_status == OM_STATUS_OK && result.token_count != 0
                 && result.tokens[result.token_count - 1].kind == OM_TOKEN_EOF;
    for (size_t index = 0; valid && index < result.token_count; ++index) {
        const Om_Token *token = &result.tokens[index];
        valid = om_source_span_is_valid(token->span, (uint32_t)source.bytes.length)
                && token->lexeme.length == (size_t)(token->span.end - token->span.start)
                && token->lexeme.data == (const char *)source.bytes.data + token->span.start;
    }
    for (size_t index = 0; valid && index < result.diagnostic_count; ++index) {
        valid = om_source_span_is_valid(
            result.diagnostics[index].span,
            (uint32_t)source.bytes.length
        );
    }
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return valid;
}

bool om_test_lexer_keywords_and_names(void)
{
    static const uint8_t input[] =
        "ACTION main()\nVAR count = 9223372036854775808\nAction badName\nCount\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 2);
    LEX_ASSERT(result.diagnostics[0].code == OM_LEX_DIAGNOSTIC_E0101);
    LEX_ASSERT(result.diagnostics[1].code == OM_LEX_DIAGNOSTIC_E0102);
    LEX_ASSERT(strcmp(om_lex_diagnostic_code_name(result.diagnostics[0].code), "E0101") == 0);
    LEX_ASSERT(result.tokens[0].kind == OM_TOKEN_KEYWORD
               && result.tokens[0].keyword == OM_KEYWORD_ACTION);
    LEX_ASSERT(result.tokens[1].kind == OM_TOKEN_IDENTIFIER);
    bool saw_entity = false;
    bool saw_case_variant_keyword = false;
    bool saw_min_magnitude = false;
    for (size_t index = 0; index < result.token_count; ++index) {
        const Om_Token *token = &result.tokens[index];
        if (token->kind == OM_TOKEN_ENTITY_IDENTIFIER
            && token->lexeme.length == 5
            && memcmp(token->lexeme.data, "Count", 5) == 0) {
            saw_entity = true;
        }
        if (token->kind == OM_TOKEN_KEYWORD && token->keyword == OM_KEYWORD_ACTION
            && token->lexeme.length == 6 && memcmp(token->lexeme.data, "Action", 6) == 0) {
            saw_case_variant_keyword = true;
        }
        if (token->kind == OM_TOKEN_INTEGER
            && token->integer_magnitude == UINT64_C(9223372036854775808)) {
            saw_min_magnitude = token->integer_requires_negative;
        }
    }
    LEX_ASSERT(saw_entity);
    LEX_ASSERT(saw_case_variant_keyword);
    LEX_ASSERT(saw_min_magnitude);
    LEX_ASSERT(result.tokens[result.token_count - 1].kind == OM_TOKEN_EOF);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_list_take_name_compatibility(void)
{
    static const uint8_t input[] = "take TAKE order ORDER query QUERY";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 0);
    LEX_ASSERT(result.tokens[0].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[1].kind == OM_TOKEN_KEYWORD
               && result.tokens[1].keyword == OM_KEYWORD_TAKE);
    LEX_ASSERT(result.tokens[2].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[3].kind == OM_TOKEN_KEYWORD
               && result.tokens[3].keyword == OM_KEYWORD_ORDER);
    LEX_ASSERT(result.tokens[4].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[5].kind == OM_TOKEN_KEYWORD
               && result.tokens[5].keyword == OM_KEYWORD_QUERY);
    passed = true;
cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_all_keywords(void)
{
    static const uint8_t input[] =
        "ACTION ACTOR AND AT BREAK CASE CONTINUE DESCENDING ELSE END EXPORT FALSE "
        "FOR FROM FUNCTION IF IMPORT IN MESSAGE NATIVE NOT NULL OR QUERY ORDER BY "
        "RECORD RECEIVE RETURN SELECT TAKE ASCENDING WHERE SELF SEND SPAWN STOP "
        "STEP THEN TO TRUE TRY VAR WATCH WHILE WITH";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    bool seen[(size_t)OM_KEYWORD_WITH + 1] = {false};
    size_t keyword_count = 0;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 0);
    for (size_t index = 0; index < result.token_count; ++index) {
        const Om_Token *token = &result.tokens[index];
        if (token->kind != OM_TOKEN_KEYWORD) {
            continue;
        }
        LEX_ASSERT(token->keyword > OM_KEYWORD_NONE && token->keyword <= OM_KEYWORD_WITH);
        LEX_ASSERT(!seen[token->keyword]);
        seen[token->keyword] = true;
        ++keyword_count;
    }
    LEX_ASSERT(keyword_count == (size_t)OM_KEYWORD_WITH);
    for (size_t keyword = 1; keyword <= (size_t)OM_KEYWORD_WITH; ++keyword) {
        LEX_ASSERT(seen[keyword]);
    }
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_down_reason_names_are_identifiers(void)
{
    static const uint8_t input[] = "normal failed node_unreachable NORMAL FAILED NODE_UNREACHABLE";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 3);
    LEX_ASSERT(result.token_count == 7);
    for (size_t index = 0; index < 6; ++index) {
        LEX_ASSERT(result.tokens[index].kind != OM_TOKEN_KEYWORD);
    }
    LEX_ASSERT(result.tokens[0].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[1].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[2].kind == OM_TOKEN_IDENTIFIER);
    LEX_ASSERT(result.tokens[3].kind == OM_TOKEN_INVALID_IDENTIFIER);
    LEX_ASSERT(result.tokens[4].kind == OM_TOKEN_INVALID_IDENTIFIER);
    LEX_ASSERT(result.tokens[5].kind == OM_TOKEN_INVALID_IDENTIFIER);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_numbers(void)
{
    static const uint8_t input[] = "0 42 9223372036854775809 1.25 1e3 1e+ 1e9999 12abc";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 4);
    LEX_ASSERT(result.diagnostics[0].code == OM_LEX_DIAGNOSTIC_E0104);
    LEX_ASSERT(result.diagnostics[1].code == OM_LEX_DIAGNOSTIC_E0103);
    LEX_ASSERT(result.diagnostics[2].code == OM_LEX_DIAGNOSTIC_E0104);
    LEX_ASSERT(result.diagnostics[3].code == OM_LEX_DIAGNOSTIC_E0103);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_INTEGER) == 3);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_FLOAT) == 3);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_ERROR) == 2);
    bool saw_fraction = false;
    bool saw_exponent = false;
    for (size_t index = 0; index < result.token_count; ++index) {
        const Om_Token *token = &result.tokens[index];
        if (token->kind == OM_TOKEN_FLOAT && token->lexeme.length == 4
            && token->float_value == 1.25) {
            saw_fraction = true;
        }
        if (token->kind == OM_TOKEN_FLOAT && token->lexeme.length == 3
            && token->float_value == 1000.0) {
            saw_exponent = true;
        }
    }
    LEX_ASSERT(saw_fraction && saw_exponent);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_strings(void)
{
    static const uint8_t input[] =
        "\"plain\\n\\u{1f600}\" $\"Hi {name} {{brace}} {values[0]}\"";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 0);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_STRING) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_INTERPOLATED_STRING_START) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_INTERPOLATED_STRING_END) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_INTERPOLATION_START) == 2);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_INTERPOLATION_END) == 2);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_STRING_TEXT) == 2);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_LEFT_BRACKET) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_RIGHT_BRACKET) == 1);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_newlines_and_comments(void)
{
    static const uint8_t input[] =
        "''' documentation\nvalue = (1 +\n2) ' ordinary\r\nnext = 3\n";
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input) - 1));
    LEX_ASSERT(result.diagnostic_count == 0);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_DOC_COMMENT) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_NEWLINE) == 3);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_LEFT_PAREN) == 1);
    LEX_ASSERT(count_tokens(&result, OM_TOKEN_RIGHT_PAREN) == 1);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_invalid_input(void)
{
    static const uint8_t input[] = {
        (uint8_t)'"', (uint8_t)'b', (uint8_t)'a', (uint8_t)'d', (uint8_t)'\\',
        (uint8_t)'q', (uint8_t)'"', (uint8_t)' ', UINT8_C(0xff), (uint8_t)' ',
        (uint8_t)'"', (uint8_t)'u', (uint8_t)'n', (uint8_t)'t', (uint8_t)'e',
        (uint8_t)'r', (uint8_t)'m', (uint8_t)'i', (uint8_t)'n', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'e', (uint8_t)'d'
    };
    Om_Source_Text source = {0};
    Om_Lexer_Result result = {0};
    bool passed = false;
    LEX_ASSERT(scan_text(&source, &result, input, sizeof(input)));
    LEX_ASSERT(result.diagnostic_count == 3);
    LEX_ASSERT(result.diagnostics[0].code == OM_LEX_DIAGNOSTIC_E0103);
    LEX_ASSERT(result.diagnostics[1].code == OM_LEX_DIAGNOSTIC_E0103);
    LEX_ASSERT(result.diagnostics[2].code == OM_LEX_DIAGNOSTIC_E0103);
    LEX_ASSERT(result.diagnostics[0].span.start == 4);
    LEX_ASSERT(result.diagnostics[1].span.start == 8);
    passed = true;

cleanup:
    om_lexer_result_destroy(&result);
    om_source_text_destroy(&source);
    return passed;
}

bool om_test_lexer_identifier_conformance(void)
{
#if !defined(OM_LEXER_CONFORMANCE_ROOT)
#error OM_LEXER_CONFORMANCE_ROOT must name the identifier fixture directory
#endif
    static const char fixture_root[] = OM_LEXER_CONFORMANCE_ROOT;
    typedef struct Conformance_Case {
        const char *path;
        size_t diagnostic_count;
        Om_Lex_Diagnostic_Code first_diagnostic;
        const char *expected_name;
        Om_Token_Kind expected_kind;
        uint32_t expected_line;
        uint32_t expected_column;
    } Conformance_Case;
    static const Conformance_Case cases[] = {
        {"keyword_uppercase.bas", 0, 0, "ACTION", OM_TOKEN_KEYWORD, 1, 1},
        {"keyword_wrong_case.bas", 3, OM_LEX_DIAGNOSTIC_E0101, "action", OM_TOKEN_KEYWORD, 1, 1},
        {"ordinary_lower_snake.bas", 0, 0, "account_id", OM_TOKEN_IDENTIFIER, 2, 5},
        {"ordinary_mixed_case.bas", 1, OM_LEX_DIAGNOSTIC_E0102, "accountId",
         OM_TOKEN_INVALID_IDENTIFIER, 2, 5},
        {"entity_pascal_snake.bas", 0, 0, "Customer_Record", OM_TOKEN_ENTITY_IDENTIFIER, 1, 8},
        {"entity_invalid_case.bas", 0, 0, "customer_record", OM_TOKEN_IDENTIFIER, 1, 8},
    };
    Om_Source_Root root = {0};
    bool passed = false;
    if (om_source_root_init(
            &root,
            (Om_String_View){fixture_root, sizeof(fixture_root) - 1}
        ) != OM_STATUS_OK) {
        goto cleanup;
    }
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        const Conformance_Case *test_case = &cases[case_index];
        Om_Source_Text source = {0};
        Om_Lexer_Result result = {0};
        const size_t path_length = strlen(test_case->path);
        Om_Status status = om_source_root_load(
            &root,
            (Om_Source_Id){(uint32_t)(case_index + 100)},
            (Om_String_View){test_case->path, path_length},
            4096,
            &source
        );
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "fixture load failed: %s (%s)\n", test_case->path,
                          om_status_name(status));
            om_source_text_destroy(&source);
            om_lexer_result_destroy(&result);
            goto cleanup;
        }
        status = om_lexer_scan(&source, &result);
        if (status != OM_STATUS_OK || result.diagnostic_count != test_case->diagnostic_count
            || (result.diagnostic_count != 0
                && result.diagnostics[0].code != test_case->first_diagnostic)) {
            (void)fprintf(stderr,
                          "fixture diagnostics mismatch: %s count=%zu expected=%zu first=%d\n",
                          test_case->path,
                          result.diagnostic_count,
                          test_case->diagnostic_count,
                          result.diagnostic_count == 0 ? 0 : (int)result.diagnostics[0].code);
            for (size_t diagnostic_index = 0; diagnostic_index < result.diagnostic_count;
                 ++diagnostic_index) {
                const Om_Source_Span span = result.diagnostics[diagnostic_index].span;
                (void)fprintf(stderr, "  %s at %u..%u: %.*s\n",
                              om_lex_diagnostic_code_name(result.diagnostics[diagnostic_index].code),
                              span.start,
                              span.end,
                              (int)(span.end - span.start),
                              (const char *)source.bytes.data + span.start);
            }
            om_source_text_destroy(&source);
            om_lexer_result_destroy(&result);
            goto cleanup;
        }
        bool found_name = false;
        for (size_t token_index = 0; token_index < result.token_count; ++token_index) {
            const Om_Token *token = &result.tokens[token_index];
            if (token->lexeme.length != strlen(test_case->expected_name)
                || memcmp(token->lexeme.data, test_case->expected_name, token->lexeme.length) != 0) {
                continue;
            }
            Om_Source_Location location = {0};
            if (token->kind != test_case->expected_kind
                || om_source_text_location(&source, token->span.start, &location) != OM_STATUS_OK
                || location.line != test_case->expected_line
                || location.byte_column != test_case->expected_column) {
                break;
            }
            found_name = true;
            break;
        }
        if (!found_name) {
            (void)fprintf(stderr, "fixture token mismatch: %s name=%s kind=%d\n",
                          test_case->path,
                          test_case->expected_name,
                          (int)test_case->expected_kind);
            om_source_text_destroy(&source);
            om_lexer_result_destroy(&result);
            goto cleanup;
        }
        if (result.diagnostic_count != 0) {
            Om_Source_Location location = {0};
            if (om_source_text_location(&source, result.diagnostics[0].span.start, &location)
                    != OM_STATUS_OK
                || location.line != test_case->expected_line
                || location.byte_column != test_case->expected_column) {
                (void)fprintf(stderr, "fixture diagnostic span mismatch: %s at %u:%u\n",
                              test_case->path, location.line, location.byte_column);
                om_source_text_destroy(&source);
                om_lexer_result_destroy(&result);
                goto cleanup;
            }
        }
        om_lexer_result_destroy(&result);
        om_source_text_destroy(&source);
    }
    passed = true;

cleanup:
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}

bool om_test_lexer_fuzz(void)
{
    static const uint8_t alphabet[] = {
        (uint8_t)'A', (uint8_t)'a', (uint8_t)'Z', (uint8_t)'z', (uint8_t)'0', (uint8_t)'9',
        (uint8_t)'_', (uint8_t)' ', (uint8_t)'\t', (uint8_t)'\n', (uint8_t)'\r', (uint8_t)'\'',
        (uint8_t)'"', (uint8_t)'$', (uint8_t)'\\', (uint8_t)'{', (uint8_t)'}', (uint8_t)'(',
        (uint8_t)')', (uint8_t)'[', (uint8_t)']', (uint8_t)'<', (uint8_t)'>', (uint8_t)'=',
        (uint8_t)'.', (uint8_t)'+', (uint8_t)'-', (uint8_t)'*', (uint8_t)'/', (uint8_t)'%',
        (uint8_t)':', (uint8_t)',', UINT8_C(0), UINT8_C(0x80), UINT8_C(0xc3), UINT8_C(0xa9),
        UINT8_C(0xff)
    };
    static const char path[] = "lexer_fuzz.bas";
    uint32_t random_state = UINT32_C(0x9e3779b9);
    uint8_t candidate[128] = {0};

    for (size_t iteration = 0; iteration < 4096; ++iteration) {
        Om_Source_Text source = {0};
        Om_Lexer_Result result = {0};
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        const size_t length = (size_t)(random_state % (uint32_t)sizeof(candidate));
        for (size_t index = 0; index < length; ++index) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            candidate[index] = alphabet[random_state % (uint32_t)sizeof(alphabet)];
        }
        const Om_Status source_status = om_source_text_init(
            &source,
            (Om_Source_Id){(uint32_t)iteration},
            (Om_String_View){path, sizeof(path) - 1},
            (Om_Const_Byte_Slice){candidate, length},
            0
        );
        if (source_status != OM_STATUS_OK) {
            return false;
        }
        const Om_Status scan_status = om_lexer_scan(&source, &result);
        bool valid = scan_status == OM_STATUS_OK && result.token_count != 0
                     && result.tokens[result.token_count - 1].kind == OM_TOKEN_EOF;
        for (size_t index = 0; valid && index < result.token_count; ++index) {
            const Om_Token *token = &result.tokens[index];
            valid = om_source_span_is_valid(token->span, (uint32_t)source.bytes.length)
                    && token->lexeme.length == (size_t)(token->span.end - token->span.start)
                    && token->lexeme.data
                           == (const char *)source.bytes.data + token->span.start;
        }
        for (size_t index = 0; valid && index < result.diagnostic_count; ++index) {
            valid = om_source_span_is_valid(
                result.diagnostics[index].span,
                (uint32_t)source.bytes.length
            );
        }
        om_lexer_result_destroy(&result);
        om_source_text_destroy(&source);
        if (!valid) {
            OM_TEST_ASSERT(false);
        }
    }
    return true;
}

bool om_test_lexer_string_fuzz(void)
{
    static const uint8_t alphabet[] = {
        (uint8_t)'a', (uint8_t)'Z', (uint8_t)'0', (uint8_t)' ', (uint8_t)'\\',
        (uint8_t)'"', (uint8_t)'n', (uint8_t)'r', (uint8_t)'t', (uint8_t)'0',
        (uint8_t)'u', (uint8_t)'{', (uint8_t)'}', (uint8_t)'$', (uint8_t)'\n',
        (uint8_t)'\r', (uint8_t)'[', (uint8_t)']', (uint8_t)',', UINT8_C(0x80),
        UINT8_C(0xc3), UINT8_C(0xa9), UINT8_C(0xff)
    };
    uint32_t random_state = UINT32_C(0x243f6a88);
    uint8_t input[160] = {0};
    for (size_t iteration = 0; iteration < 2048; ++iteration) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        const size_t content_length = (size_t)(random_state % UINT32_C(96));
        const bool interpolated = (iteration & 1U) != 0;
        size_t offset = 0;
        if (interpolated) {
            input[offset++] = (uint8_t)'$';
        }
        input[offset++] = (uint8_t)'"';
        for (size_t index = 0; index < content_length; ++index) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            input[offset + index] = alphabet[random_state % (uint32_t)sizeof(alphabet)];
        }
        offset += content_length;
        input[offset++] = (uint8_t)'"';
        if (!scan_candidate(input, offset, (uint32_t)iteration)) {
            OM_TEST_ASSERT(false);
        }
    }
    return true;
}

bool om_test_lexer_comment_fuzz(void)
{
    static const uint8_t alphabet[] = {
        (uint8_t)'a', (uint8_t)'Z', (uint8_t)'0', (uint8_t)' ', (uint8_t)'\\',
        (uint8_t)'"', (uint8_t)'\'', (uint8_t)'{', (uint8_t)'}', (uint8_t)'\n',
        (uint8_t)'\r', (uint8_t)'\t', UINT8_C(0x80), UINT8_C(0xc3), UINT8_C(0xa9),
        UINT8_C(0xff)
    };
    uint32_t random_state = UINT32_C(0xb7e15162);
    uint8_t input[160] = {0};
    for (size_t iteration = 0; iteration < 2048; ++iteration) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        const size_t comment_length = (size_t)(random_state % UINT32_C(48));
        size_t offset = 0;
        input[offset++] = (uint8_t)'\'';
        input[offset++] = (uint8_t)'\'';
        input[offset++] = (uint8_t)'\'';
        for (size_t index = 0; index < comment_length; ++index) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            input[offset++] = alphabet[random_state % (uint32_t)sizeof(alphabet)];
        }
        input[offset++] = (uint8_t)'\n';
        static const uint8_t body[] = "ACTION main()\n    value = 1 '";
        (void)memcpy(input + offset, body, sizeof(body) - 1);
        offset += sizeof(body) - 1;
        for (size_t index = 0; index < comment_length; ++index) {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            input[offset++] = alphabet[random_state % (uint32_t)sizeof(alphabet)];
        }
        input[offset++] = (uint8_t)'\n';
        static const uint8_t tail[] = "END ACTION\n";
        (void)memcpy(input + offset, tail, sizeof(tail) - 1);
        offset += sizeof(tail) - 1;
        if (!scan_candidate(input, offset, (uint32_t)(iteration + UINT32_C(5000)))) {
            OM_TEST_ASSERT(false);
        }
    }
    return true;
}

bool om_test_lexer_literal_conformance(void)
{
#if !defined(OM_LEXER_LITERAL_ROOT)
#error OM_LEXER_LITERAL_ROOT must name the lexical fixture directory
#endif
    static const char fixture_root[] = OM_LEXER_LITERAL_ROOT;
    typedef struct Literal_Case {
        const char *path;
        Om_Lex_Diagnostic_Code code;
        const char *span_text;
        uint32_t line;
        uint32_t column;
    } Literal_Case;
    static const Literal_Case cases[] = {
        {"numeric_suffix.bas", OM_LEX_DIAGNOSTIC_E0103, "12abc", 2, 13},
        {"integer_overflow.bas", OM_LEX_DIAGNOSTIC_E0104, "9223372036854775809", 2, 13},
        {"float_overflow.bas", OM_LEX_DIAGNOSTIC_E0104, "1e9999", 2, 13},
        {"malformed_exponent.bas", OM_LEX_DIAGNOSTIC_E0103, "1e+", 2, 13},
        {"invalid_escape.bas", OM_LEX_DIAGNOSTIC_E0103, "\\q", 2, 17},
        {"valid_escape.bas", 0, NULL, 0, 0},
        {"valid_interpolation.bas", 0, NULL, 0, 0},
        {"float_underflow.bas", 0, NULL, 0, 0},
    };
    Om_Source_Root root = {0};
    bool passed = false;
    if (om_source_root_init(
            &root,
            (Om_String_View){fixture_root, sizeof(fixture_root) - 1}
        ) != OM_STATUS_OK) {
        goto cleanup;
    }
    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        const Literal_Case *test_case = &cases[case_index];
        Om_Source_Text source = {0};
        Om_Lexer_Result result = {0};
        Om_Status status = om_source_root_load(
            &root,
            (Om_Source_Id){(uint32_t)(case_index + 200)},
            (Om_String_View){test_case->path, strlen(test_case->path)},
            4096,
            &source
        );
        if (status == OM_STATUS_OK) {
            status = om_lexer_scan(&source, &result);
        }
        if (status != OM_STATUS_OK) {
            om_lexer_result_destroy(&result);
            om_source_text_destroy(&source);
            goto cleanup;
        }
        const size_t expected_diagnostics = test_case->code == 0 ? 0 : 1;
        if (result.diagnostic_count != expected_diagnostics) {
            om_lexer_result_destroy(&result);
            om_source_text_destroy(&source);
            goto cleanup;
        }
        if (expected_diagnostics != 0) {
            const Om_Lex_Diagnostic diagnostic = result.diagnostics[0];
            Om_Source_Location location = {0};
            Om_Const_Byte_Slice selected = {0};
            if (diagnostic.code != test_case->code
                || om_source_text_location(&source, diagnostic.span.start, &location)
                       != OM_STATUS_OK
                || location.line != test_case->line || location.byte_column != test_case->column
                || om_source_text_slice(&source, diagnostic.span, &selected) != OM_STATUS_OK
                || selected.length != strlen(test_case->span_text)
                || memcmp(selected.data, test_case->span_text, selected.length) != 0) {
                om_lexer_result_destroy(&result);
                om_source_text_destroy(&source);
                goto cleanup;
            }
        }
        om_lexer_result_destroy(&result);
        om_source_text_destroy(&source);
    }
    passed = true;

cleanup:
    om_source_root_destroy(&root);
    OM_TEST_ASSERT(passed);
    return true;
}

#undef LEX_ASSERT
