#ifndef OMBASIC_NATIVE_RUNTIME_TEST_SUPPORT_H
#define OMBASIC_NATIVE_RUNTIME_TEST_SUPPORT_H

#include <stdbool.h>

bool om_runtime_test_fault_boundary(void);
bool om_runtime_test_root_order(void);
bool om_runtime_test_gc(void);
bool om_runtime_test_strings(void);
bool om_runtime_test_records(void);
bool om_runtime_test_lists(void);
bool om_runtime_test_maps(void);
bool om_runtime_test_native_pointers(void);
bool om_runtime_test_json(void);

#endif
