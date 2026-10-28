#include "runtime_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "ombasic/foundation/memory.h"

static void restore_roots(Om_Execution_Context *context,
                          Om_Runtime_Root_Frame *base)
{
    if (context == NULL) {
        return;
    }
    Om_Runtime_Root_Frame *frame = context->roots;
    while (frame != NULL && frame != base) {
        Om_Runtime_Root_Frame *previous = frame->previous;
        om_free(frame);
        frame = previous;
    }
    context->roots = base;
}

static const char *runtime_fault_message(Om_Runtime_Fault_Code code)
{
    switch (code) {
    case OM_RUNTIME_FAULT_INTEGER_OVERFLOW:
        return "integer overflow";
    case OM_RUNTIME_FAULT_DIVISION_BY_ZERO:
        return "division by zero";
    case OM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS:
        return "collection index out of bounds";
    case OM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND:
        return "map key not found";
    case OM_RUNTIME_FAULT_ZERO_RANGE_STEP:
        return "numeric range step cannot be zero";
    case OM_RUNTIME_FAULT_OPERATION_FAILED:
        return "runtime operation failed";
    case OM_RUNTIME_FAULT_ACTOR_FAILED:
        return "Actor handler failed";
    case OM_RUNTIME_FAULT_NONE:
        break;
    }
    return "unknown runtime fault";
}

static void report_runtime_fault(const Om_Execution_Context *context,
                                 Om_Runtime_Fault_Code code,
                                 uint32_t source_span_id)
{
    const char *message = runtime_fault_message(code);
    if (context != NULL && source_span_id < context->source_span_count) {
        const Om_Runtime_Source_Span *span =
            &context->source_spans[source_span_id];
        if (span->path != NULL && span->path[0] != '\0'
            && span->line != 0 && span->column != 0) {
            (void)fprintf(stderr, "%s:%" PRIu32 ":%" PRIu32
                ": runtime fault: %s\n", span->path, span->line,
                span->column, message);
            return;
        }
    }
    (void)fprintf(stderr, "ombasic runtime fault: %s\n", message);
}

int om_runtime_run(Om_Execution_Context *context,
                   Om_Runtime_Entry_Function entry)
{
    if (context == NULL || entry == NULL) {
        return 70;
    }
    Om_Runtime_Fault_Boundary boundary = {
        .previous = context->fault_boundary,
        .root_base = context->roots,
        .source_span_id = UINT32_MAX,
    };
    context->fault_boundary = &boundary;
    const int jump_status = setjmp(boundary.environment);
    int result = 70;
    if (jump_status == 0) {
        result = entry(context);
        if (context->roots != boundary.root_base) {
            restore_roots(context, boundary.root_base);
            report_runtime_fault(context, OM_RUNTIME_FAULT_OPERATION_FAILED,
                                 UINT32_MAX);
            result = 70;
        }
    } else {
        restore_roots(context, boundary.root_base);
        if (boundary.code != OM_RUNTIME_FAULT_ACTOR_FAILED) {
            report_runtime_fault(context, boundary.code,
                                 boundary.source_span_id);
        }
        result = 70;
    }
    context->fault_boundary = boundary.previous;
    return result;
}

_Noreturn void om_runtime_fault(Om_Execution_Context *context,
                                Om_Runtime_Fault_Code code,
                                uint32_t source_span_id)
{
    if (context != NULL && context->fault_boundary != NULL) {
        Om_Runtime_Fault_Boundary *boundary = context->fault_boundary;
        boundary->code = code;
        boundary->source_span_id = source_span_id;
        longjmp(boundary->environment, 1);
    }
    report_runtime_fault(context, code, source_span_id);
    abort();
}
