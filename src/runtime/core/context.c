#include "runtime_internal.h"

#include "ombasic/foundation/memory.h"

#define OM_RUNTIME_DEFAULT_COLLECTION_THRESHOLD ((size_t)64 * 1024)

Om_Execution_Context *om_runtime_context_create(size_t collection_threshold)
{
    Om_Execution_Context *context = om_alloc(sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    const size_t threshold = collection_threshold == 0
        ? OM_RUNTIME_DEFAULT_COLLECTION_THRESHOLD : collection_threshold;
    *context = (Om_Execution_Context){
        .collection_threshold = threshold,
        .minimum_collection_threshold = threshold,
    };
    return context;
}

Om_Status om_runtime_actor_system_context_create(
    Om_Execution_Context *context,
    Om_Execution_Context **result)
{
    if (context == NULL || context->actor_system == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Execution_Context *worker = om_runtime_context_create(
        context->collection_threshold);
    if (worker == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    worker->actor_system = context->actor_system;
    worker->source_spans = context->source_spans;
    worker->source_span_count = context->source_span_count;
    *result = worker;
    return OM_STATUS_OK;
}

Om_Status om_runtime_set_source_spans(
    Om_Execution_Context *context,
    const Om_Runtime_Source_Span *spans,
    size_t span_count
)
{
    if (context == NULL || (span_count != 0 && spans == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < span_count; ++index) {
        if (spans[index].path == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    context->source_spans = spans;
    context->source_span_count = span_count;
    return OM_STATUS_OK;
}

void om_runtime_context_destroy(Om_Execution_Context *context)
{
    if (context == NULL) {
        return;
    }
    if (context->owns_actor_system && context->actor_system != NULL) {
        destroy_actor_system(context->actor_system);
        context->actor_system = NULL;
    }
    Om_Runtime_Object_Header *object = context->objects;
    while (object != NULL) {
        Om_Runtime_Object_Header *next = object->fields.next;
        finalize_native_pointer(object);
        om_free(object->fields.external_storage);
        om_free(object);
        object = next;
    }
    Om_Runtime_Root_Frame *frame = context->roots;
    while (frame != NULL) {
        Om_Runtime_Root_Frame *previous = frame->previous;
        om_free(frame);
        frame = previous;
    }
    om_free(context);
}
