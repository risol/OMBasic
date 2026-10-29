#include "test.h"

bool om_test_module_graph_order(void);
bool om_test_module_graph_cycle(void);
bool om_test_module_project_load(void);
bool om_test_module_project_cycle(void);
bool om_test_module_project_errors(void);
bool om_test_module_project_relative_identity(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"module_graph_order", om_test_module_graph_order},
        {"module_graph_cycle", om_test_module_graph_cycle},
        {"module_project_load", om_test_module_project_load},
        {"module_project_cycle", om_test_module_project_cycle},
        {"module_project_errors", om_test_module_project_errors},
        {"module_project_relative_identity", om_test_module_project_relative_identity},
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
