#include "test.h"

bool om_test_source_text(void);
bool om_test_import_path(void);
bool om_test_file_loader(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"source_text", om_test_source_text},
        {"import_path", om_test_import_path},
        {"file_loader", om_test_file_loader},
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
