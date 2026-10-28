#ifndef OMBASIC_RUNTIME_ABI_BASE_H
#define OMBASIC_RUNTIME_ABI_BASE_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"

#define OM_RUNTIME_ABI_VERSION 1

typedef struct Om_Execution_Context Om_Execution_Context;
typedef struct Om_Runtime_Root_Frame Om_Runtime_Root_Frame;
typedef struct Om_Runtime_Type_Descriptor Om_Runtime_Type_Descriptor;

typedef enum Om_Runtime_Fault_Code {
    OM_RUNTIME_FAULT_NONE = 0,
    OM_RUNTIME_FAULT_INTEGER_OVERFLOW = 1,
    OM_RUNTIME_FAULT_DIVISION_BY_ZERO,
    OM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS,
    OM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND,
    OM_RUNTIME_FAULT_ZERO_RANGE_STEP,
    OM_RUNTIME_FAULT_OPERATION_FAILED,
    OM_RUNTIME_FAULT_ACTOR_FAILED
} Om_Runtime_Fault_Code;

typedef struct Om_Runtime_Source_Span {
    const char *path;
    uint32_t line;
    uint32_t column;
} Om_Runtime_Source_Span;

typedef int (*Om_Runtime_Entry_Function)(Om_Execution_Context *context);

_Noreturn void om_runtime_fault(Om_Execution_Context *context,
                                Om_Runtime_Fault_Code code,
                                uint32_t source_span_id);

#endif
