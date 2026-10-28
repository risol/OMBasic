#include "ombasic/runtime/abi.h"
#include "ombasic/runtime/embedding.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ABI_ASSERT(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",          \
                          __FILE__, __LINE__, #expression);                      \
            result = 1;                                                          \
            goto cleanup;                                                        \
        }                                                                        \
    } while (false)

int main(void)
{
    int result = 0;
    Om_Execution_Context *context = om_runtime_context_create(1024);
    ABI_ASSERT(context != NULL);

    const Om_Runtime_Program_Descriptor compatible = {
        .abi_version = OM_RUNTIME_ABI_VERSION,
    };
    ABI_ASSERT(om_runtime_validate_program(context, &compatible)
               == OM_STATUS_OK);

    /* Invalid later fields must not be examined before rejecting the ABI. */
    const Om_Runtime_Program_Descriptor incompatible = {
        .abi_version = OM_RUNTIME_ABI_VERSION + UINT32_C(1),
        .type_count = 1,
        .types = NULL,
    };
    ABI_ASSERT(om_runtime_validate_program(context, &incompatible)
               == OM_STATUS_ABI_MISMATCH);
    ABI_ASSERT(om_runtime_actor_system_start(context, &incompatible, 0)
               == OM_STATUS_ABI_MISMATCH);

cleanup:
    om_runtime_context_destroy(context);
    return result;
}
