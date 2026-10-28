#ifndef OMBASIC_RUNTIME_ABI_NATIVE_H
#define OMBASIC_RUNTIME_ABI_NATIVE_H

#include <stdint.h>

#include "ombasic/runtime/abi/program_descriptor.h"

typedef void (*Om_Runtime_Native_Callback_Function)(void);
typedef void (*Om_Runtime_CString_Release)(void *pointer);
typedef void (*Om_Runtime_Native_Pointer_Release)(void *pointer);

/* Callback functions, candidate arrays, and message descriptors are borrowed
   from the loaded program module. A Native library must stop and join every
   callback thread before the Native function returns, and must not invoke a
   callback afterward. Generated code unregisters callbacks after return;
   unregister waits for callbacks already in flight before it completes. */
Om_Status om_runtime_native_callback_register_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *const *candidate_sets,
    const uint64_t *candidate_counts,
    const Om_Runtime_Type_Descriptor *const *message_types,
    Om_Runtime_Native_Callback_Function *registered_functions);
Om_Status om_runtime_native_callback_unregister_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *registered_functions);
Om_Status om_runtime_native_callback_post(
    Om_Runtime_Native_Callback_Function callback,
    const uint64_t *value_bits,
    uint64_t value_count);

Om_Status om_runtime_native_pointer_create(
    Om_Execution_Context *context,
    void *pointer,
    Om_Runtime_Native_Pointer_Release release,
    void *owner,
    void **result);
Om_Status om_runtime_native_pointer_borrow(
    const Om_Execution_Context *context,
    const void *native_pointer,
    void **result);
Om_Status om_runtime_native_pointer_take(
    Om_Execution_Context *context,
    void *native_pointer,
    void **result);

Om_Status om_runtime_string_create_from_cstring(
    Om_Execution_Context *context,
    const char *cstring,
    void **result);
Om_Status om_runtime_string_create_from_owned_cstring(
    Om_Execution_Context *context,
    char *cstring,
    Om_Runtime_CString_Release release,
    void **result);

#endif
