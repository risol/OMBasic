#include "test.h"

bool om_test_bytecode_golden_round_trip(void);
bool om_test_bytecode_verifier_rejects_invalid_images(void);
bool om_test_bytecode_decoder_fuzz(void);
bool om_test_bytecode_emitter_project(void);

int main(void)
{
    const Om_Test_Case tests[] = {
        {"bytecode_golden_round_trip", om_test_bytecode_golden_round_trip},
        {"bytecode_verifier_rejects_invalid_images",
         om_test_bytecode_verifier_rejects_invalid_images},
        {"bytecode_decoder_fuzz", om_test_bytecode_decoder_fuzz},
        {"bytecode_emitter_project", om_test_bytecode_emitter_project},
    };
    size_t failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].function()) {
            (void)printf("PASS %s\n", tests[index].name);
        } else {
            (void)fprintf(stderr, "FAIL %s\n", tests[index].name);
            ++failures;
        }
    }
    (void)printf("%zu tests, %zu failures\n",
                 sizeof(tests) / sizeof(tests[0]), failures);
    return failures == 0 ? 0 : 1;
}
