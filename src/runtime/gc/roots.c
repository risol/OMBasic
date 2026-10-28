#include "runtime_internal.h"

#include "ombasic/foundation/memory.h"

Om_Runtime_Root_Frame *om_runtime_roots_push(Om_Execution_Context *context,
                                             void **root_slots,
                                             size_t root_count)
{
    if (context == NULL || (root_count != 0 && root_slots == NULL)) {
        return NULL;
    }
    Om_Runtime_Root_Frame *frame = om_alloc(sizeof(*frame));
    if (frame == NULL) {
        return NULL;
    }
    *frame = (Om_Runtime_Root_Frame){
        .previous = context->roots,
        .slots = root_slots,
        .count = root_count,
    };
    context->roots = frame;
    return frame;
}

Om_Status om_runtime_roots_pop(Om_Execution_Context *context,
                               Om_Runtime_Root_Frame *frame)
{
    if (context == NULL || frame == NULL || context->roots != frame) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    context->roots = frame->previous;
    om_free(frame);
    return OM_STATUS_OK;
}
