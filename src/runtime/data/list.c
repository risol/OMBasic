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

typedef struct Om_List_Order_Context {
    Om_Execution_Context *execution;
    const void *const *keys;
    size_t key_count;
    uint64_t descending_mask;
} Om_List_Order_Context;

static Om_Status compare_list_order_indices(const Om_List_Order_Context *order,
                                            size_t left,
                                            size_t right,
                                            int *result)
{
    for (size_t key_index = 0; key_index < order->key_count; ++key_index) {
        Om_Runtime_Value left_value = {0};
        Om_Runtime_Value right_value = {0};
        Om_Status status = om_runtime_list_get(order->execution,
            order->keys[key_index], left, &left_value);
        if (status == OM_STATUS_OK) {
            status = om_runtime_list_get(order->execution,
                order->keys[key_index], right, &right_value);
        }
        if (status != OM_STATUS_OK) return status;
        const Om_Runtime_Object_Header *key_list = find_object(
            order->execution, order->keys[key_index]);
        const Om_Runtime_Type_Kind key_kind =
            key_list->fields.type->element_type->kind;
        int comparison = 0;
        if (key_kind == OM_RUNTIME_TYPE_INT) {
            comparison = left_value.integer < right_value.integer ? -1
                : left_value.integer > right_value.integer ? 1 : 0;
        } else if (key_kind == OM_RUNTIME_TYPE_STRING) {
            int32_t string_comparison = 0;
            status = om_runtime_string_compare(order->execution,
                left_value.managed, right_value.managed, &string_comparison);
            if (status != OM_STATUS_OK) return status;
            comparison = string_comparison < 0 ? -1
                : string_comparison > 0 ? 1 : 0;
        } else {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (comparison != 0) {
            const bool descending = (order->descending_mask
                & (UINT64_C(1) << key_index)) != 0;
            *result = descending ? -comparison : comparison;
            return OM_STATUS_OK;
        }
    }
    *result = left < right ? -1 : left > right ? 1 : 0;
    return OM_STATUS_OK;
}

static Om_Status stable_merge_indices(const Om_List_Order_Context *order,
                                      size_t *indices,
                                      size_t *scratch,
                                      size_t count)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t width = 1; status == OM_STATUS_OK && width < count;) {
        size_t start = 0;
        while (status == OM_STATUS_OK && start < count) {
            const size_t middle = start + (count - start < width
                ? count - start : width);
            const size_t remaining = count - middle;
            const size_t end = middle + (remaining < width ? remaining : width);
            size_t left = start;
            size_t right = middle;
            size_t output = start;
            while (status == OM_STATUS_OK && left < middle && right < end) {
                int comparison = 0;
                status = compare_list_order_indices(order, indices[left],
                    indices[right], &comparison);
                if (status == OM_STATUS_OK) {
                    scratch[output++] = comparison <= 0
                        ? indices[left++] : indices[right++];
                }
            }
            while (status == OM_STATUS_OK && left < middle) {
                scratch[output++] = indices[left++];
            }
            while (status == OM_STATUS_OK && right < end) {
                scratch[output++] = indices[right++];
            }
            start = end;
        }
        if (status == OM_STATUS_OK) {
            (void)memcpy(indices, scratch, count * sizeof(*indices));
            width = width > count / 2 ? count : width * 2;
        }
    }
    return status;
}

static Om_Status top_k_indices(const Om_List_Order_Context *order,
                               size_t *heap,
                               size_t *count,
                               size_t capacity,
                               size_t candidate)
{
    if (*count < capacity) {
        size_t child = (*count)++;
        heap[child] = candidate;
        while (child != 0) {
            const size_t parent = (child - 1) / 2;
            int comparison = 0;
            Om_Status status = compare_list_order_indices(order,
                heap[parent], heap[child], &comparison);
            if (status != OM_STATUS_OK) return status;
            if (comparison >= 0) break;
            const size_t temporary = heap[parent];
            heap[parent] = heap[child];
            heap[child] = temporary;
            child = parent;
        }
        return OM_STATUS_OK;
    }
    int comparison = 0;
    Om_Status status = compare_list_order_indices(order, candidate, heap[0],
                                                  &comparison);
    if (status != OM_STATUS_OK || comparison >= 0) return status;
    heap[0] = candidate;
    size_t parent = 0;
    for (;;) {
        const size_t left = parent * 2 + 1;
        if (left >= *count) break;
        const size_t right = left + 1;
        size_t worse = left;
        if (right < *count) {
            status = compare_list_order_indices(order, heap[left], heap[right],
                                                &comparison);
            if (status != OM_STATUS_OK) return status;
            if (comparison < 0) worse = right;
        }
        status = compare_list_order_indices(order, heap[parent], heap[worse],
                                            &comparison);
        if (status != OM_STATUS_OK || comparison >= 0) return status;
        const size_t temporary = heap[parent];
        heap[parent] = heap[worse];
        heap[worse] = temporary;
        parent = worse;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_stable_order(
    Om_Execution_Context *context,
    const void *values_pointer,
    const void *const *key_lists,
    size_t key_count,
    uint64_t descending_mask,
    int64_t take_count,
    void **result)
{
    Om_Runtime_Object_Header *values_object = find_object(context,
                                                          values_pointer);
    if (context == NULL || values_object == NULL || result == NULL
        || values_object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || key_count == 0 || key_count > 64 || key_lists == NULL
        || (key_count < 64
            && (descending_mask >> key_count) != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *values = object_payload_const(values_object);
    if (key_count > SIZE_MAX - 2) return OM_STATUS_OUT_OF_RANGE;
    void **roots = om_alloc_array(key_count + 2, sizeof(*roots));
    if (roots == NULL) return OM_STATUS_OUT_OF_MEMORY;
    roots[0] = (void *)values_pointer;
    roots[1] = NULL;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < key_count; ++index) {
        Om_Runtime_Object_Header *key_list = find_object(context,
                                                         key_lists[index]);
        if (key_list == NULL || key_list->fields.type->kind != OM_RUNTIME_TYPE_LIST
            || (key_list->fields.type->element_type->kind != OM_RUNTIME_TYPE_INT
                && key_list->fields.type->element_type->kind
                    != OM_RUNTIME_TYPE_STRING)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            const Om_Runtime_List_Body *keys = object_payload_const(key_list);
            if (keys->length != values->length) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            roots[index + 2] = (void *)key_lists[index];
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(roots);
        return status;
    }
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots,
                                                          key_count + 2);
    if (frame == NULL) {
        om_free(roots);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_List_Order_Context order = {
        context, (const void *const *)(roots + 2), key_count, descending_mask,
    };
    const size_t count = values->length;
    const size_t requested = take_count <= 0 ? 0
        : (uint64_t)take_count < (uint64_t)count ? (size_t)take_count : count;
    const bool use_top_k = requested != 0 && requested < count;
    const size_t ordering_capacity = requested;
    size_t *indices = ordering_capacity == 0 ? NULL
        : om_alloc_array(ordering_capacity, sizeof(*indices));
    size_t *scratch = ordering_capacity == 0 ? NULL
        : om_alloc_array(ordering_capacity, sizeof(*scratch));
    if (ordering_capacity != 0 && (indices == NULL || scratch == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK && use_top_k) {
        size_t heap_count = 0;
        for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
            status = top_k_indices(&order, indices, &heap_count, requested, index);
        }
        if (status == OM_STATUS_OK) {
            status = stable_merge_indices(&order, indices, scratch, heap_count);
        }
    } else if (status == OM_STATUS_OK && requested > 1) {
        for (size_t index = 0; status == OM_STATUS_OK && index < requested; ++index) {
            indices[index] = index;
        }
        status = stable_merge_indices(&order, indices, scratch, requested);
    } else if (status == OM_STATUS_OK && requested == 1) {
        indices[0] = 0;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_create(context, values_object->fields.type,
                                        &roots[1]);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < requested; ++index) {
        Om_Runtime_Value value = {0};
        status = om_runtime_list_get(context, roots[0], indices[index], &value);
        if (status == OM_STATUS_OK) {
            status = om_runtime_list_append(context, roots[1], value);
        }
    }
    if (status == OM_STATUS_OK) *result = roots[1];
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    om_free(scratch);
    om_free(indices);
    om_free(roots);
    return status == OM_STATUS_OK ? pop_status : status;
}

