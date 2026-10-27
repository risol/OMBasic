#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/tooling/formatter.h"
#include "ombasic/tooling/diagnostics_json.h"

#define ASSERT(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",             \
                          __FILE__, __LINE__, #condition);                        \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

static bool format_once(const char *text, size_t length,
                        Om_Source_Text *source,
                        Om_Lexer_Result *lexed,
                        Om_Parser_Result *parsed,
                        Om_Formatter_Result *formatted)
{
    const Om_Status source_status = om_source_text_init(
        source, (Om_Source_Id){0}, (Om_String_View){"main.bas", 8},
        (Om_Const_Byte_Slice){(const uint8_t *)text, length}, 0);
    if (source_status != OM_STATUS_OK
        || om_lexer_scan(source, lexed) != OM_STATUS_OK
        || om_parser_parse(lexed, parsed) != OM_STATUS_OK
        || lexed->diagnostic_count != 0 || parsed->diagnostic_count != 0
        || om_formatter_format(source, lexed, parsed, formatted) != OM_STATUS_OK) {
        return false;
    }
    return true;
}

int main(void)
{
    static const char input[] =
        "''' doc for main\r\n"
        "FUNCTION   main( a,b )\r\n"
        "' keep this comment\r\n"
        "IF a>0 THEN\r\n"
        "RETURN a+b ' keep trailing comment\r\n"
        "ELSE\r\n"
        "RETURN -b\r\n"
        "END IF\r\n"
        "END FUNCTION";
    static const char expected_fragment[] =
        "''' doc for main\n"
        "FUNCTION main(a, b)\n"
        "    ' keep this comment\n"
        "    IF a > 0 THEN\n"
        "        RETURN a + b ' keep trailing comment\n"
        "    ELSE\n"
        "        RETURN -b\n"
        "    END IF\n"
        "END FUNCTION\n";
    Om_Source_Text first_source = {0};
    Om_Source_Text second_source = {0};
    Om_Lexer_Result first_lexed = {0};
    Om_Lexer_Result second_lexed = {0};
    Om_Parser_Result first_parsed = {0};
    Om_Parser_Result second_parsed = {0};
    Om_Formatter_Result first_formatted = {0};
    Om_Formatter_Result second_formatted = {0};
    Om_Tooling_Json_Result json = {0};
    bool passed = false;

    ASSERT(format_once(input, sizeof(input) - 1, &first_source, &first_lexed,
                       &first_parsed, &first_formatted));
    ASSERT(first_formatted.length == sizeof(expected_fragment) - 1);
    ASSERT(memcmp(first_formatted.data, expected_fragment,
                  sizeof(expected_fragment) - 1) == 0);
    ASSERT(format_once(first_formatted.data, first_formatted.length,
                       &second_source, &second_lexed, &second_parsed,
                       &second_formatted));
    ASSERT(first_formatted.length == second_formatted.length);
    ASSERT(memcmp(first_formatted.data, second_formatted.data,
                  first_formatted.length) == 0);
    const Om_Tooling_Diagnostic diagnostic = {
        .stage = {"parser", 6},
        .code = {"E0202", 5},
        .message = {"unexpected \"token\"", 18},
        .source = &first_source,
        .span = {.source = {0}, .start = 0, .end = 8}
    };
    ASSERT(om_tooling_diagnostics_to_json(&diagnostic, 1, &json) == OM_STATUS_OK);
    ASSERT(strstr(json.data, "\"stage\":\"parser\"") != NULL);
    ASSERT(strstr(json.data, "\"code\":\"E0202\"") != NULL);
    ASSERT(strstr(json.data, "\"message\":\"unexpected \\\"token\\\"\"") != NULL);
    ASSERT(strstr(json.data, "\"line\":1,\"byteColumn\":1") != NULL);
    passed = true;

cleanup:
    om_tooling_json_result_destroy(&json);
    om_formatter_result_destroy(&second_formatted);
    om_formatter_result_destroy(&first_formatted);
    om_parser_result_destroy(&second_parsed);
    om_parser_result_destroy(&first_parsed);
    om_lexer_result_destroy(&second_lexed);
    om_lexer_result_destroy(&first_lexed);
    om_source_text_destroy(&second_source);
    om_source_text_destroy(&first_source);
    if (passed) {
        (void)puts("PASS formatter_idempotent_and_preserves_comments");
    }
    return passed ? 0 : 1;
}
