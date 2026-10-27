#include "test.h"

int main(void)
{
    const Om_Test_Case tests[] = {
        {"memory", om_test_memory},
        {"string_view", om_test_string_view},
        {"arena", om_test_arena},
        {"vector", om_test_vector},
        {"utf8", om_test_utf8},
        {"u64_map", om_test_u64_map},
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t failures = 0;

    for (size_t index = 0; index < test_count; ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            failures += 1;
        }
    }
    (void)printf("%zu tests, %zu failures\n", test_count, failures);
    return failures == 0 ? 0 : 1;
}
