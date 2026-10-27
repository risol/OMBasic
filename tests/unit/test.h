#ifndef OMBASIC_TEST_H
#define OMBASIC_TEST_H

#include <stdbool.h>
#include <stdio.h>

#define OM_TEST_ASSERT(expression)                                                \
    do {                                                                          \
        if (!(expression)) {                                                      \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",              \
                          __FILE__, __LINE__, #expression);                       \
            return false;                                                         \
        }                                                                         \
    } while (false)

typedef bool (*Om_Test_Function)(void);

typedef struct Om_Test_Case {
    const char *name;
    Om_Test_Function function;
} Om_Test_Case;

bool om_test_memory(void);
bool om_test_string_view(void);
bool om_test_arena(void);
bool om_test_vector(void);
bool om_test_utf8(void);
bool om_test_u64_map(void);

#endif
