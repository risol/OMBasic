#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

Om_Runtime_Object_Header *find_object(
    const Om_Execution_Context *context,
    const void *pointer
)
{
    if (context == NULL || pointer == NULL) {
        return NULL;
    }
    if (context->is_actor_mailbox) {
        /* The mailbox heap is Runtime-private. All object handles reaching it
           come from queued transit objects or their validated typed fields,
           so the owner word is a constant-time membership check. */
        Om_Runtime_Object_Header *object = (Om_Runtime_Object_Header *)pointer;
        return object->fields.owner == context && object->fields.type != NULL
            ? object : NULL;
    }
    for (Om_Runtime_Object_Header *object = context->objects;
         object != NULL; object = object->fields.next) {
        if ((const void *)object == pointer) {
            return object;
        }
    }
    return NULL;
}

void *object_payload(Om_Runtime_Object_Header *object)
{
    return object == NULL ? NULL : (void *)(object + 1);
}

const void *object_payload_const(const Om_Runtime_Object_Header *object)
{
    return object == NULL ? NULL : (const void *)(object + 1);
}


Om_Status allocation_safepoint(Om_Execution_Context *context,
                               size_t expected_bytes)
{
    if (context == NULL || context->collecting) {
        return context == NULL ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    }
    if (expected_bytes > SIZE_MAX - context->allocation_bytes) {
        Om_Status status = runtime_collect_internal(context);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    if (expected_bytes > SIZE_MAX - context->allocation_bytes) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (context->allocation_bytes + expected_bytes
        > context->collection_threshold) {
        return runtime_collect_internal(context);
    }
    return OM_STATUS_OK;
}

Om_Status allocate_object(Om_Execution_Context *context,
                          const Om_Runtime_Type_Descriptor *type,
                          size_t payload_size,
                          size_t external_size,
                          Om_Runtime_Object_Header **result)
{
    if (context == NULL || type == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    if (type->alignment != 0
        && (!runtime_is_power_of_two(type->alignment)
            || type->alignment > _Alignof(Om_Runtime_Object_Header))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(sizeof(Om_Runtime_Object_Header), payload_size,
                             &allocation_size)
        || !om_checked_size_add(allocation_size, external_size,
                                &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = allocation_safepoint(context, allocation_size);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Object_Header *object = om_alloc(allocation_size);
    if (object == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(object, 0, allocation_size);
    object->fields.type = type;
    object->fields.owner = context;
    object->fields.next = context->objects;
    object->fields.allocated_size = allocation_size;
    context->objects = object;
    ++context->live_object_count;
    context->allocation_bytes += allocation_size;
    *result = object;
    return OM_STATUS_OK;
}
