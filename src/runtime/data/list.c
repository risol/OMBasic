#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

Om_Status om_runtime_list_create(Om_Execution_Context *context,
                                 const Om_Runtime_Type_Descriptor *type,
                                 void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_LIST || type->element_type == NULL
        || type->element_type->kind > OM_RUNTIME_TYPE_ACTOR_REF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type,
        sizeof(Om_Runtime_List_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

Om_Status om_runtime_list_concat(Om_Execution_Context *context,
                                 const void *left_pointer,
                                 const void *right_pointer,
                                 void **result)
{
    Om_Runtime_Object_Header *left = find_object(context, left_pointer);
    Om_Runtime_Object_Header *right = find_object(context, right_pointer);
    if (context == NULL || left == NULL || right == NULL || result == NULL
        || left->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || right->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || !runtime_descriptor_compatible(left->fields.type,
                                          right->fields.type, 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *roots[3] = {(void *)left_pointer, (void *)right_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 3);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = om_runtime_list_create(context, left->fields.type,
                                               &roots[2]);
    for (size_t source_index = 0; status == OM_STATUS_OK && source_index < 2;
         ++source_index) {
        size_t length = 0;
        status = om_runtime_list_length(context, roots[source_index], &length);
        for (size_t index = 0; status == OM_STATUS_OK && index < length;
             ++index) {
            Om_Runtime_Value item = {0};
            status = om_runtime_list_get(context, roots[source_index], index,
                                         &item);
            if (status == OM_STATUS_OK) {
                status = om_runtime_list_append(context, roots[2], item);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        *result = roots[2];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

static Om_Status list_reserve(Om_Execution_Context *context,
                              Om_Runtime_Object_Header *object,
                              size_t requested_capacity)
{
    Om_Runtime_List_Body *list = object_payload(object);
    if (requested_capacity <= list->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = list->capacity == 0 ? 4 : list->capacity;
    while (capacity < requested_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = requested_capacity;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*list->items)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t new_bytes = capacity * sizeof(*list->items);
    const size_t old_bytes = list->capacity * sizeof(*list->items);
    Om_Status status = allocation_safepoint(context,
        new_bytes > old_bytes ? new_bytes - old_bytes : 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Value *items = om_realloc_array(list->items, capacity,
                                               sizeof(*items));
    if (items == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    list->items = items;
    list->capacity = capacity;
    object->fields.external_storage = items;
    object->fields.allocated_size += new_bytes - old_bytes;
    context->allocation_bytes += new_bytes - old_bytes;
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_append(Om_Execution_Context *context,
                                 void *list_pointer,
                                 Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->element_type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_List_Body *list = object_payload(object);
    if (list->length == SIZE_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = list_reserve(context, object, list->length + 1);
    if (status == OM_STATUS_OK) {
        list->items[list->length++] = value;
    }
    return status;
}

Om_Status om_runtime_list_append_value(Om_Execution_Context *context,
                                      void *list,
                                      const Om_Runtime_Value *value)
{
    return value == NULL ? OM_STATUS_INVALID_ARGUMENT
        : om_runtime_list_append(context, list, *value);
}

Om_Status om_runtime_list_get(const Om_Execution_Context *context,
                              const void *list_pointer,
                              size_t index,
                              Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *list = object_payload_const(object);
    if (index >= list->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *result = list->items[index];
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_length(const Om_Execution_Context *context,
                                 const void *list_pointer,
                                 size_t *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *list = object_payload_const(object);
    *result = list->length;
    return OM_STATUS_OK;
}

