#include "test.h"

bool om_test_lexer_keywords_and_names(void);
bool om_test_lexer_all_keywords(void);
bool om_test_lexer_list_take_name_compatibility(void);
bool om_test_lexer_down_reason_names_are_identifiers(void);
bool om_test_lexer_numbers(void);
bool om_test_lexer_strings(void);
bool om_test_lexer_newlines_and_comments(void);
bool om_test_lexer_invalid_input(void);
bool om_test_lexer_identifier_conformance(void);
bool om_test_lexer_literal_conformance(void);
bool om_test_lexer_fuzz(void);
bool om_test_lexer_string_fuzz(void);
bool om_test_lexer_comment_fuzz(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"lexer_keywords_and_names", om_test_lexer_keywords_and_names},
        {"lexer_all_keywords", om_test_lexer_all_keywords},
        {"lexer_list_take_name_compatibility",
         om_test_lexer_list_take_name_compatibility},
        {"lexer_down_reason_names_are_identifiers",
         om_test_lexer_down_reason_names_are_identifiers},
        {"lexer_numbers", om_test_lexer_numbers},
        {"lexer_strings", om_test_lexer_strings},
        {"lexer_newlines_and_comments", om_test_lexer_newlines_and_comments},
        {"lexer_invalid_input", om_test_lexer_invalid_input},
        {"lexer_identifier_conformance", om_test_lexer_identifier_conformance},
        {"lexer_literal_conformance", om_test_lexer_literal_conformance},
        {"lexer_fuzz", om_test_lexer_fuzz},
        {"lexer_string_fuzz", om_test_lexer_string_fuzz},
        {"lexer_comment_fuzz", om_test_lexer_comment_fuzz},
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t failures = 0;
    for (size_t index = 0; index < test_count; ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            ++failures;
        }
    }
    (void)printf("%zu tests, %zu failures\n", test_count, failures);
    return failures == 0 ? 0 : 1;
}
