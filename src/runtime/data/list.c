#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

static Om_Status list_reserve(Om_Execution_Context *context,
                              Om_Runtime_Object_Header *object,
                              size_t requested_capacity,
                              bool exact_capacity);

static Om_Runtime_Value list_item_value(const Om_Runtime_List_Body *list,
                                        size_t index)
{
    if (list->byte_storage) {
        return (Om_Runtime_Value){.integer = list->byte_items[index]};
    }
    return list->items[index];
}

static Om_Status list_store_value(Om_Runtime_List_Body *list,
                                  size_t index,
                                  Om_Runtime_Value value)
{
    if (list->byte_storage) {
        if (value.integer < 0 || value.integer > UINT8_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        list->byte_items[index] = (uint8_t)value.integer;
    } else {
        list->items[index] = value;
    }
    return OM_STATUS_OK;
}

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
        atomic_init(&((Om_Runtime_List_Body *)object_payload(object))
                         ->ownership_state,
                    OM_RUNTIME_LIST_OWNERSHIP_UNIQUE);
        *result = object;
    }
    return status;
}

void om_runtime_list_mark_shared(Om_Execution_Context *context,
                                 const void *list_pointer)
{
    Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object != NULL && object->fields.type->kind == OM_RUNTIME_TYPE_LIST) {
        Om_Runtime_List_Body *list = object_payload(object);
        unsigned int state = atomic_load_explicit(&list->ownership_state,
                                                  memory_order_acquire);
        for (;;) {
            if (state == OM_RUNTIME_LIST_OWNERSHIP_SHARED) return;
            if (state == OM_RUNTIME_LIST_OWNERSHIP_CONSUMING) {
                state = atomic_load_explicit(&list->ownership_state,
                                             memory_order_acquire);
                continue;
            }
            if (state != OM_RUNTIME_LIST_OWNERSHIP_UNIQUE
                && state != OM_RUNTIME_LIST_OWNERSHIP_CONSUMED) return;
            if (atomic_compare_exchange_weak_explicit(&list->ownership_state,
                    &state, OM_RUNTIME_LIST_OWNERSHIP_SHARED,
                    memory_order_acq_rel, memory_order_acquire)) {
                return;
            }
        }
    }
}

void om_runtime_list_mark_value_shared(
    Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    Om_Runtime_Value value)
{
    if (type != NULL && type->kind == OM_RUNTIME_TYPE_LIST
        && value.managed != NULL) {
        om_runtime_list_mark_shared(context, value.managed);
    }
}

static void mark_inline_value_shared_depth(
    Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    const void *value,
    size_t depth)
{
    if (context == NULL || type == NULL || value == NULL || depth > 128) {
        return;
    }
    if (type->kind == OM_RUNTIME_TYPE_LIST) {
        void *list = NULL;
        (void)memcpy(&list, value, sizeof(list));
        if (list != NULL) om_runtime_list_mark_shared(context, list);
        return;
    }
    if (type->kind != OM_RUNTIME_TYPE_RESULT
        || !result_descriptor_valid(type)) {
        return;
    }
    bool is_ok = false;
    (void)memcpy(&is_ok,
        (const unsigned char *)value + type->fields[0].offset,
        sizeof(is_ok));
    const Om_Runtime_Field_Descriptor *payload = &type->fields[is_ok ? 1 : 2];
    mark_inline_value_shared_depth(context, payload->type,
        (const unsigned char *)value + payload->offset, depth + 1);
}

void om_runtime_list_mark_inline_value_shared(
    Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    const void *value)
{
    mark_inline_value_shared_depth(context, type, value, 0);
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
    const Om_Runtime_List_Body *left_list = object_payload_const(left);
    const Om_Runtime_List_Body *right_list = object_payload_const(right);
    const bool compact_bytes = left_list->byte_storage
        && right_list->byte_storage;
    Om_Status status = compact_bytes
        ? om_runtime_list_byte_builder_create(context, left->fields.type,
                                               &roots[2])
        : om_runtime_list_builder_create(context, left->fields.type,
                                          &roots[2]);
    if (status == OM_STATUS_OK) {
        left = find_object(context, roots[0]);
        right = find_object(context, roots[1]);
        Om_Runtime_Object_Header *builder = find_object(context, roots[2]);
        left_list = object_payload_const(left);
        right_list = object_payload_const(right);
        if (left_list->transient || right_list->transient) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else if (left_list->length > SIZE_MAX - right_list->length) {
            status = OM_STATUS_OUT_OF_RANGE;
        } else {
            const size_t total = left_list->length + right_list->length;
            status = list_reserve(context, builder, total, true);
            if (status == OM_STATUS_OK) {
                Om_Runtime_List_Body *output = object_payload(builder);
                if (compact_bytes) {
                    if (left_list->length != 0) {
                        (void)memcpy(output->byte_items, left_list->byte_items,
                                     left_list->length);
                    }
                    if (right_list->length != 0) {
                        (void)memcpy(output->byte_items + left_list->length,
                                     right_list->byte_items,
                                     right_list->length);
                    }
                } else {
                    for (size_t index = 0; index < left_list->length; ++index) {
                        (void)list_store_value(output, index,
                                               list_item_value(left_list, index));
                    }
                    for (size_t index = 0; index < right_list->length; ++index) {
                        (void)list_store_value(output, left_list->length + index,
                                               list_item_value(right_list, index));
                    }
                }
                output->length = total;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[2], &roots[2]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[2];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

static Om_Status list_reserve(Om_Execution_Context *context,
                              Om_Runtime_Object_Header *object,
                              size_t requested_capacity,
                              bool exact_capacity)
{
    Om_Runtime_List_Body *list = object_payload(object);
    if (requested_capacity <= list->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = requested_capacity;
    if (!exact_capacity) {
        capacity = list->capacity == 0 ? 4 : list->capacity;
        while (capacity < requested_capacity) {
            if (capacity > SIZE_MAX / 2) {
                capacity = requested_capacity;
                break;
            }
            capacity *= 2;
        }
    }
    const size_t item_size = list->byte_storage
        ? sizeof(*list->byte_items) : sizeof(*list->items);
    if (capacity > SIZE_MAX / item_size) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t new_bytes = capacity * item_size;
    const size_t old_bytes = list->capacity * item_size;
    const size_t growth = new_bytes - old_bytes;
    if (growth > SIZE_MAX - object->fields.allocated_size
        || growth > SIZE_MAX - context->allocation_bytes) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = allocation_safepoint(context,
        growth);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (list->byte_storage) {
        uint8_t *bytes = om_realloc_array(list->byte_items, capacity,
                                          sizeof(*bytes));
        if (bytes == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        list->byte_items = bytes;
        object->fields.external_storage = bytes;
    } else {
        Om_Runtime_Value *items = om_realloc_array(list->items, capacity,
                                                   sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        list->items = items;
        object->fields.external_storage = items;
    }
    list->capacity = capacity;
    object->fields.allocated_size += growth;
    context->allocation_bytes += growth;
    return OM_STATUS_OK;
}

static Om_Status list_append_internal(Om_Execution_Context *context,
                                      void *list_pointer,
                                      Om_Runtime_Value value,
                                      bool builder_operation)
{
    Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_List_Body *list = object_payload(object);
    if ((builder_operation && !list->transient)
        || (!builder_operation && (list->transient || list->sealed))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->element_type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (list->length == SIZE_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = list_reserve(context, object, list->length + 1, false);
    if (status == OM_STATUS_OK) {
        om_runtime_list_mark_value_shared(context, type->element_type, value);
        status = list_store_value(list, list->length, value);
        if (status == OM_STATUS_OK) {
            ++list->length;
        }
    }
    return status;
}

Om_Status om_runtime_list_byte_builder_create(
    Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    void **result)
{
    if (type == NULL || type->kind != OM_RUNTIME_TYPE_LIST
        || type->element_type == NULL
        || type->element_type->kind != OM_RUNTIME_TYPE_INT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = om_runtime_list_builder_create(context, type, result);
    if (status == OM_STATUS_OK) {
        Om_Runtime_Object_Header *object = find_object(context, *result);
        if (object == NULL) {
            *result = NULL;
            return OM_STATUS_INVALID_ARGUMENT;
        }
        ((Om_Runtime_List_Body *)object_payload(object))->byte_storage = true;
    }
    return status;
}

Om_Status om_runtime_list_append(Om_Execution_Context *context,
                                 void *list_pointer,
                                 Om_Runtime_Value value)
{
    return list_append_internal(context, list_pointer, value, false);
}

Om_Status om_runtime_list_builder_create(
    Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    void **result)
{
    Om_Status status = om_runtime_list_create(context, type, result);
    if (status == OM_STATUS_OK) {
        Om_Runtime_Object_Header *object = find_object(context, *result);
        if (object == NULL) {
            *result = NULL;
            return OM_STATUS_INVALID_ARGUMENT;
        }
        ((Om_Runtime_List_Body *)object_payload(object))->transient = true;
    }
    return status;
}

Om_Status om_runtime_list_builder_reserve(Om_Execution_Context *context,
                                          void *builder,
                                          int64_t requested_capacity,
                                          int64_t capacity_limit)
{
    Om_Runtime_Object_Header *object = find_object(context, builder);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || capacity_limit < 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_List_Body *list = object_payload(object);
    if (!list->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint64_t requested = requested_capacity <= 0 ? 0
        : (uint64_t)requested_capacity;
    const uint64_t limit = (uint64_t)capacity_limit;
    const uint64_t bounded = requested < limit ? requested : limit;
    if (bounded > SIZE_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    return list_reserve(context, object, (size_t)bounded, true);
}

Om_Status om_runtime_list_builder_append_value(
    Om_Execution_Context *context,
    void *builder,
    const Om_Runtime_Value *value)
{
    return value == NULL ? OM_STATUS_INVALID_ARGUMENT
        : list_append_internal(context, builder, *value, true);
}

Om_Status om_runtime_list_byte_builder_append(Om_Execution_Context *context,
                                              void *builder,
                                              int64_t value)
{
    if (value < 0 || value > UINT8_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Runtime_Value item = {.integer = value};
    return list_append_internal(context, builder, item, true);
}

Om_Status om_runtime_list_byte_builder_append_list(
    Om_Execution_Context *context,
    void *builder_pointer,
    const void *list_pointer)
{
    Om_Runtime_Object_Header *builder = find_object(context, builder_pointer);
    Om_Runtime_Object_Header *source_object = find_object(context, list_pointer);
    if (context == NULL || builder == NULL || source_object == NULL
        || builder->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || source_object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || builder->fields.type->element_type == NULL
        || builder->fields.type->element_type->kind != OM_RUNTIME_TYPE_INT
        || !runtime_descriptor_compatible(builder->fields.type,
                                           source_object->fields.type, 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *builder_list = object_payload_const(builder);
    const Om_Runtime_List_Body *source = object_payload_const(source_object);
    if (!builder_list->transient || !builder_list->byte_storage
        || source->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (source->length > SIZE_MAX - builder_list->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    for (size_t index = 0; index < source->length; ++index) {
        const Om_Runtime_Value value = list_item_value(source, index);
        if (value.integer < 0 || value.integer > UINT8_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
    }
    if (source->length > (size_t)INT64_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    void *roots[2] = {builder_pointer, (void *)list_pointer};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    builder = find_object(context, roots[0]);
    source_object = find_object(context, roots[1]);
    builder_list = object_payload_const(builder);
    source = object_payload_const(source_object);
    const size_t output_length = builder_list->length + source->length;
    Om_Status status = output_length > (size_t)INT64_MAX
        ? OM_STATUS_OUT_OF_RANGE
        : list_reserve(context, builder, output_length, true);
    if (status == OM_STATUS_OK && source->length != 0) {
        builder = find_object(context, roots[0]);
        source_object = find_object(context, roots[1]);
        Om_Runtime_List_Body *destination = object_payload(builder);
        source = object_payload_const(source_object);
        if (source->byte_storage) {
            (void)memcpy(destination->byte_items + destination->length,
                         source->byte_items, source->length);
        } else {
            for (size_t index = 0; index < source->length; ++index) {
                destination->byte_items[destination->length + index] =
                    (uint8_t)list_item_value(source, index).integer;
            }
        }
        destination->length = output_length;
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_list_compact_bytes(
    Om_Execution_Context *context,
    const void *list_pointer,
    const Om_Runtime_Type_Descriptor *list_type,
    void **result)
{
    Om_Runtime_Object_Header *source = find_object(context, list_pointer);
    if (context == NULL || list_type == NULL || result == NULL
        || list_type->kind != OM_RUNTIME_TYPE_LIST
        || list_type->element_type == NULL
        || list_type->element_type->kind != OM_RUNTIME_TYPE_INT
        || source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || !runtime_descriptor_compatible(list_type, source->fields.type, 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *roots[2] = {(void *)list_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = om_runtime_list_byte_builder_create(context, list_type,
                                                            &roots[1]);
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_byte_builder_append_list(context, roots[1],
                                                          roots[0]);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_list_builder_seal(Om_Execution_Context *context,
                                       void *builder,
                                       void **result)
{
    Om_Runtime_Object_Header *object = find_object(context, builder);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_LIST
        || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_List_Body *list = object_payload(object);
    if (!list->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    list->transient = false;
    list->sealed = true;
    *result = builder;
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_set_copy(Om_Execution_Context *context,
                                   const void *list_pointer,
                                   int64_t index,
                                   const Om_Runtime_Value *replacement,
                                   void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || list_object == NULL || replacement == NULL
        || result == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_List_Body *source = object_payload_const(list_object);
    if (source->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || (uint64_t)index >= (uint64_t)source->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Runtime_Type_Descriptor *list_type = list_object->fields.type;
    if (!value_matches_type(context, list_type->element_type, *replacement)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *roots[3] = {(void *)list_pointer, NULL, NULL};
    if (type_is_managed(list_type->element_type)) {
        roots[2] = replacement->managed;
    }
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 3);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const bool compact_bytes = source->byte_storage
        && replacement->integer >= 0 && replacement->integer <= UINT8_MAX;
    Om_Status status = compact_bytes
        ? om_runtime_list_byte_builder_create(context, list_type, &roots[1])
        : om_runtime_list_builder_create(context, list_type, &roots[1]);
    if (status == OM_STATUS_OK) {
        list_object = find_object(context, roots[0]);
        Om_Runtime_Object_Header *builder = find_object(context, roots[1]);
        source = object_payload_const(list_object);
        status = list_reserve(context, builder, source->length, true);
        if (status == OM_STATUS_OK) {
            om_runtime_list_mark_value_shared(context,
                list_type->element_type, *replacement);
            Om_Runtime_List_Body *destination = object_payload(builder);
            for (size_t item_index = 0; item_index < source->length;
                 ++item_index) {
                status = list_store_value(destination, item_index,
                    item_index == (size_t)index ? *replacement
                        : list_item_value(source, item_index));
                if (status != OM_STATUS_OK) break;
            }
            destination->length = status == OM_STATUS_OK ? source->length : 0;
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_list_set_consume(Om_Execution_Context *context,
                                      void *list_pointer,
                                      int64_t index,
                                      const Om_Runtime_Value *replacement,
                                      void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || list_object == NULL || replacement == NULL
        || result == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_List_Body *list = object_payload(list_object);
    if (list->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || (uint64_t)index >= (uint64_t)list->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (!value_matches_type(context, list_object->fields.type->element_type,
                            *replacement)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (list->byte_storage
        && (replacement->integer < 0 || replacement->integer > UINT8_MAX)) {
        return om_runtime_list_set_copy(context, list_pointer, index,
                                        replacement, result);
    }
    if (!list->sealed) {
        return om_runtime_list_set_copy(context, list_pointer, index,
                                        replacement, result);
    }
    unsigned int expected = atomic_load_explicit(&list->ownership_state,
                                                 memory_order_acquire);
    while (expected == OM_RUNTIME_LIST_OWNERSHIP_UNIQUE
           || expected == OM_RUNTIME_LIST_OWNERSHIP_CONSUMED) {
        if (atomic_compare_exchange_weak_explicit(&list->ownership_state,
                &expected, OM_RUNTIME_LIST_OWNERSHIP_CONSUMING,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    if (expected != OM_RUNTIME_LIST_OWNERSHIP_UNIQUE
        && expected != OM_RUNTIME_LIST_OWNERSHIP_CONSUMED) {
        if (expected == OM_RUNTIME_LIST_OWNERSHIP_CONSUMING) {
            do {
                expected = atomic_load_explicit(&list->ownership_state,
                                                memory_order_acquire);
            } while (expected == OM_RUNTIME_LIST_OWNERSHIP_CONSUMING);
        }
        return om_runtime_list_set_copy(context, list_pointer, index,
                                        replacement, result);
    }
    om_runtime_list_mark_value_shared(context,
        list_object->fields.type->element_type, *replacement);
    const unsigned int prior_state = expected;
    const Om_Status store_status = list_store_value(list, (size_t)index,
                                                     *replacement);
    if (store_status != OM_STATUS_OK) {
        atomic_store_explicit(&list->ownership_state, prior_state,
                              memory_order_release);
        return store_status;
    }
    atomic_store_explicit(&list->ownership_state,
        OM_RUNTIME_LIST_OWNERSHIP_CONSUMED, memory_order_release);
    *result = list_pointer;
    return OM_STATUS_OK;
}

typedef enum Om_List_Range_Update_Kind {
    OM_LIST_RANGE_SET = 0,
    OM_LIST_RANGE_FILL,
    OM_LIST_RANGE_REPLACE
} Om_List_Range_Update_Kind;

static Om_Status list_range_update_copy(
    Om_Execution_Context *context,
    const void *list_pointer,
    int64_t start,
    int64_t requested_remove_count,
    const void *replacement_pointer,
    const Om_Runtime_Value *fill_value,
    Om_List_Range_Update_Kind kind,
    void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || list_object == NULL || result == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_Type_Descriptor *list_type = list_object->fields.type;
    const Om_Runtime_List_Body *source = object_payload_const(list_object);
    if (source->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Runtime_Object_Header *replacement_object = NULL;
    const Om_Runtime_List_Body *replacement_list = NULL;
    if (kind == OM_LIST_RANGE_FILL) {
        if (fill_value == NULL
            || !value_matches_type(context, list_type->element_type,
                                   *fill_value)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    } else {
        replacement_object = find_object(context, replacement_pointer);
        if (replacement_object == NULL
            || replacement_object->fields.type->kind != OM_RUNTIME_TYPE_LIST
            || !runtime_descriptor_compatible(list_type,
                replacement_object->fields.type, 0)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        replacement_list = object_payload_const(replacement_object);
        if (replacement_list->transient) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    if (start < 0 || (uint64_t)start > (uint64_t)source->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    size_t remove_count = 0;
    size_t replacement_count = 0;
    if (kind == OM_LIST_RANGE_FILL) {
        if (requested_remove_count < 0
            || (uint64_t)requested_remove_count
                > (uint64_t)(source->length - (size_t)start)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        remove_count = (size_t)requested_remove_count;
        replacement_count = remove_count;
    } else {
        replacement_count = replacement_list->length;
        if (kind == OM_LIST_RANGE_SET) {
            remove_count = replacement_count;
        } else {
            if (requested_remove_count < 0
                || (uint64_t)requested_remove_count
                    > (uint64_t)(source->length - (size_t)start)) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            remove_count = (size_t)requested_remove_count;
        }
        if (kind == OM_LIST_RANGE_SET
            && replacement_count > source->length - (size_t)start) {
            return OM_STATUS_OUT_OF_RANGE;
        }
    }

    const size_t prefix_count = (size_t)start;
    const size_t suffix_start = prefix_count + remove_count;
    const size_t suffix_count = source->length - suffix_start;
    const size_t retained_count = prefix_count + suffix_count;
    if (replacement_count > SIZE_MAX - retained_count) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t output_count = retained_count + replacement_count;

    void *roots[4] = {
        (void *)list_pointer,
        kind == OM_LIST_RANGE_FILL ? NULL : (void *)replacement_pointer,
        NULL,
        kind == OM_LIST_RANGE_FILL && type_is_managed(list_type->element_type)
            ? fill_value->managed : NULL,
    };
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 4);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = om_runtime_list_builder_create(context, list_type,
                                                        &roots[2]);
    if (status == OM_STATUS_OK) {
        list_object = find_object(context, roots[0]);
        Om_Runtime_Object_Header *builder = find_object(context, roots[2]);
        source = object_payload_const(list_object);
        if (kind != OM_LIST_RANGE_FILL) {
            replacement_object = find_object(context, roots[1]);
            replacement_list = object_payload_const(replacement_object);
        }
        status = list_reserve(context, builder, output_count, true);
        if (status == OM_STATUS_OK) {
            list_object = find_object(context, roots[0]);
            builder = find_object(context, roots[2]);
            source = object_payload_const(list_object);
            Om_Runtime_List_Body *destination = object_payload(builder);
            if (kind != OM_LIST_RANGE_FILL) {
                replacement_object = find_object(context, roots[1]);
                replacement_list = object_payload_const(replacement_object);
            }
            for (size_t index = 0; index < prefix_count; ++index) {
                status = list_store_value(destination, index,
                    list_item_value(source, index));
                if (status != OM_STATUS_OK) break;
            }
            if (kind == OM_LIST_RANGE_FILL) {
                Om_Runtime_Value rooted_value = *fill_value;
                if (type_is_managed(list_type->element_type)) {
                    rooted_value.managed = roots[3];
                }
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < replacement_count; ++index) {
                    status = list_store_value(destination,
                        prefix_count + index, rooted_value);
                }
            } else {
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < replacement_count; ++index) {
                    status = list_store_value(destination, prefix_count + index,
                        list_item_value(replacement_list, index));
                }
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < suffix_count; ++index) {
                status = list_store_value(destination,
                    prefix_count + replacement_count + index,
                    list_item_value(source, suffix_start + index));
            }
            if (status == OM_STATUS_OK) {
                destination->length = output_count;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[2], &roots[2]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[2];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_list_set_range_copy(Om_Execution_Context *context,
                                         const void *list,
                                         int64_t start,
                                         const void *replacements,
                                         void **result)
{
    return list_range_update_copy(context, list, start, 0, replacements,
        NULL, OM_LIST_RANGE_SET, result);
}

Om_Status om_runtime_list_fill_range_copy(Om_Execution_Context *context,
                                          const void *list,
                                          int64_t start,
                                          int64_t count,
                                          const Om_Runtime_Value *value,
                                          void **result)
{
    return list_range_update_copy(context, list, start, count, NULL, value,
        OM_LIST_RANGE_FILL, result);
}

Om_Status om_runtime_list_replace_range_copy(Om_Execution_Context *context,
                                             const void *list,
                                             int64_t start,
                                             int64_t remove_count,
                                             const void *replacements,
                                             void **result)
{
    return list_range_update_copy(context, list, start, remove_count,
        replacements, NULL, OM_LIST_RANGE_REPLACE, result);
}

static Om_Status list_copy_range(Om_Execution_Context *context,
                                 const void *list_pointer,
                                 int64_t start,
                                 int64_t requested_count,
                                 bool reverse,
                                 void **result)
{
    Om_Runtime_Object_Header *list_object = find_object(context, list_pointer);
    if (context == NULL || list_object == NULL || result == NULL
        || list_object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_List_Body *source = object_payload_const(list_object);
    if (source->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *roots[2] = {(void *)list_pointer, NULL};
    Om_Runtime_Root_Frame *frame = om_runtime_roots_push(context, roots, 2);
    if (frame == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const bool compact_bytes = source->byte_storage;
    Om_Status status = compact_bytes
        ? om_runtime_list_byte_builder_create(context,
            list_object->fields.type, &roots[1])
        : om_runtime_list_builder_create(context,
            list_object->fields.type, &roots[1]);
    if (status == OM_STATUS_OK) {
        list_object = find_object(context, roots[0]);
        Om_Runtime_Object_Header *builder = find_object(context, roots[1]);
        source = object_payload_const(list_object);
        size_t copy_start = 0;
        size_t copy_count = 0;
        if (reverse) {
            copy_count = source->length;
        } else if (start >= 0 && requested_count > 0
                   && (uint64_t)start < (uint64_t)source->length) {
            copy_start = (size_t)start;
            const size_t available = source->length - copy_start;
            copy_count = (uint64_t)requested_count > (uint64_t)available
                ? available : (size_t)requested_count;
        }
        status = list_reserve(context, builder, copy_count, true);
        if (status == OM_STATUS_OK) {
            list_object = find_object(context, roots[0]);
            builder = find_object(context, roots[1]);
            source = object_payload_const(list_object);
            Om_Runtime_List_Body *destination = object_payload(builder);
            if (reverse) {
                for (size_t index = 0; index < copy_count; ++index) {
                    status = list_store_value(destination, index,
                        list_item_value(source, copy_count - index - 1));
                    if (status != OM_STATUS_OK) break;
                }
            } else {
                for (size_t index = 0; index < copy_count; ++index) {
                    status = list_store_value(destination, index,
                        list_item_value(source, copy_start + index));
                    if (status != OM_STATUS_OK) break;
                }
            }
            if (status == OM_STATUS_OK) {
                destination->length = copy_count;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) {
        *result = roots[1];
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    return status == OM_STATUS_OK ? pop_status : status;
}

Om_Status om_runtime_list_reverse_copy(Om_Execution_Context *context,
                                      const void *list,
                                      void **result)
{
    return list_copy_range(context, list, 0, 0, true, result);
}

Om_Status om_runtime_list_slice_copy(Om_Execution_Context *context,
                                     const void *list,
                                     int64_t start,
                                     int64_t count,
                                     void **result)
{
    return list_copy_range(context, list, start, count, false, result);
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
    if (list->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (index >= list->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *result = list_item_value(list, index);
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
    if (list->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
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
    if (values->transient) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
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
            if (keys->transient || keys->length != values->length) {
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
        status = om_runtime_list_builder_create(context,
            values_object->fields.type, &roots[1]);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < requested; ++index) {
        Om_Runtime_Value value = {0};
        status = om_runtime_list_get(context, roots[0], indices[index], &value);
        if (status == OM_STATUS_OK) {
            status = om_runtime_list_builder_append_value(context,
                roots[1], &value);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_list_builder_seal(context, roots[1], &roots[1]);
    }
    if (status == OM_STATUS_OK) *result = roots[1];
    const Om_Status pop_status = om_runtime_roots_pop(context, frame);
    om_free(scratch);
    om_free(indices);
    om_free(roots);
    return status == OM_STATUS_OK ? pop_status : status;
}

