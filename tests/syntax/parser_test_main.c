#include "test.h"

bool om_test_parser_ast_golden(void);
bool om_test_parser_language_grammar(void);
bool om_test_parser_errors_and_recovery(void);
bool om_test_parser_malformed_fuzz(void);
bool om_test_parser_depth_limit(void);
bool om_test_parser_block_depth_limit(void);
bool om_test_parser_conformance_fixture(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"parser_ast_golden", om_test_parser_ast_golden},
        {"parser_language_grammar", om_test_parser_language_grammar},
        {"parser_errors_and_recovery", om_test_parser_errors_and_recovery},
        {"parser_malformed_fuzz", om_test_parser_malformed_fuzz},
        {"parser_depth_limit", om_test_parser_depth_limit},
        {"parser_block_depth_limit", om_test_parser_block_depth_limit},
        {"parser_conformance_fixture", om_test_parser_conformance_fixture},
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
