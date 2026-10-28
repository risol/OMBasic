#include "runtime_internal.h"

#include <inttypes.h>
#include <stdio.h>

Om_Status om_runtime_validate_program(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program)
{
    if (program == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t program_abi_version = program->abi_version;
    if (program_abi_version != OM_RUNTIME_ABI_VERSION) {
        (void)fprintf(stderr,
            "ombasic: generated program requires Runtime ABI v%" PRIu32
            "; Runtime provides v%u\n",
            program_abi_version, (unsigned)OM_RUNTIME_ABI_VERSION);
        return OM_STATUS_ABI_MISMATCH;
    }
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}
