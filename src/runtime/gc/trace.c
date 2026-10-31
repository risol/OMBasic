#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

static Om_Status mark_pointer(Om_Execution_Context *context,
                              const void *pointer,
                              Om_Runtime_Object_Header ***worklist,
                              size_t *worklist_count,
                              size_t *worklist_capacity)
{
    Om_Runtime_Object_Header *object = find_object(context, pointer);
    if (object == NULL) {
        return OM_STATUS_OK;
    }
    if (object->fields.marked) {
        return OM_STATUS_OK;
    }
    if (*worklist_count == *worklist_capacity) {
        const size_t capacity = *worklist_capacity == 0 ? 32
            : *worklist_capacity * 2;
        if (capacity < *worklist_capacity
            || capacity > SIZE_MAX / sizeof(**worklist)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_Runtime_Object_Header **items = om_realloc_array(
            *worklist, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        *worklist = items;
        *worklist_capacity = capacity;
    }
    object->fields.marked = true;
    (*worklist)[(*worklist_count)++] = object;
    return OM_STATUS_OK;
}

typedef struct Om_Runtime_Trace_State {
    Om_Execution_Context *context;
    Om_Runtime_Object_Header ***worklist;
    size_t *worklist_count;
    size_t *worklist_capacity;
} Om_Runtime_Trace_State;

static Om_Status trace_visit_mark(void *managed_value, void *visitor_context)
{
    Om_Runtime_Trace_State *state = visitor_context;
    if (state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return mark_pointer(state->context, managed_value, state->worklist,
                        state->worklist_count, state->worklist_capacity);
}

static Om_Status mark_managed_field(Om_Execution_Context *context,
                                    const void *address,
                                    const Om_Runtime_Type_Descriptor *type,
                                    Om_Runtime_Object_Header ***worklist,
                                    size_t *worklist_count,
                                    size_t *worklist_capacity)
{
    if (type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->trace != NULL) {
        Om_Runtime_Trace_State state = {
            context, worklist, worklist_count, worklist_capacity,
        };
        const Om_Status status = type->trace(context, address,
            trace_visit_mark, &state);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)address + type->fields[0].offset,
            sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return mark_managed_field(context,
            (const unsigned char *)address + payload->offset,
            payload->type, worklist, worklist_count, worklist_capacity);
    }
    if (!type_is_managed(type)) {
        return OM_STATUS_OK;
    }
    void *pointer = NULL;
    (void)memcpy(&pointer, address, sizeof(pointer));
    return mark_pointer(context, pointer, worklist, worklist_count,
                        worklist_capacity);
}

Om_Status runtime_collect_internal(Om_Execution_Context *context)
{
    if (context == NULL || context->collecting) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    context->collecting = true;
    Om_Runtime_Object_Header **worklist = NULL;
    size_t worklist_count = 0;
    size_t worklist_capacity = 0;
    Om_Status status = OM_STATUS_OK;
    for (Om_Runtime_Root_Frame *frame = context->roots;
         status == OM_STATUS_OK && frame != NULL; frame = frame->previous) {
        for (size_t index = 0; status == OM_STATUS_OK && index < frame->count;
             ++index) {
            status = mark_pointer(context, frame->slots[index], &worklist,
                                  &worklist_count, &worklist_capacity);
        }
    }
    if (status == OM_STATUS_OK && context->external_root_provider != NULL) {
        Om_Runtime_Trace_State trace_state = {
            context, &worklist, &worklist_count, &worklist_capacity,
        };
        status = context->external_root_provider(context, trace_visit_mark,
                                                  &trace_state);
    }
    size_t cursor = 0;
    while (status == OM_STATUS_OK && cursor < worklist_count) {
        const Om_Runtime_Object_Header *object = worklist[cursor++];
        const Om_Runtime_Type_Descriptor *type = object->fields.type;
        const unsigned char *payload = object_payload_const(object);
        if (type->trace != NULL) {
            Om_Runtime_Trace_State trace_state = {
                context, &worklist, &worklist_count, &worklist_capacity,
            };
            status = type->trace(context, payload, trace_visit_mark,
                                 &trace_state);
            if (status != OM_STATUS_OK) {
                break;
            }
        }
        if (type->kind == OM_RUNTIME_TYPE_RECORD
            || type->kind == OM_RUNTIME_TYPE_MESSAGE) {
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < type->field_count; ++index) {
                const Om_Runtime_Field_Descriptor *field = &type->fields[index];
                status = mark_managed_field(context, payload + field->offset,
                    field->type, &worklist, &worklist_count,
                    &worklist_capacity);
            }
        } else if (type->kind == OM_RUNTIME_TYPE_LIST) {
            const Om_Runtime_List_Body *list = (const Om_Runtime_List_Body *)payload;
            if (type_is_managed(type->element_type) && !list->byte_storage) {
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < list->length; ++index) {
                    status = mark_managed_field(context, &list->items[index],
                        type->element_type, &worklist, &worklist_count,
                        &worklist_capacity);
                }
            }
        } else if (type->kind == OM_RUNTIME_TYPE_MAP) {
            const Om_Runtime_Map_Body *map = (const Om_Runtime_Map_Body *)payload;
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < map->length; ++index) {
                status = mark_managed_field(context, &map->entries[index].key,
                    type->key_type, &worklist, &worklist_count,
                    &worklist_capacity);
                if (status == OM_STATUS_OK) {
                    status = mark_managed_field(context,
                        &map->entries[index].value, type->value_type,
                        &worklist, &worklist_count, &worklist_capacity);
                }
            }
        }
    }
    om_free(worklist);
    if (status != OM_STATUS_OK) {
        for (Om_Runtime_Object_Header *object = context->objects;
             object != NULL; object = object->fields.next) {
            object->fields.marked = false;
        }
        context->collecting = false;
        return status;
    }
    size_t live_bytes = 0;
    Om_Runtime_Object_Header **link = &context->objects;
    while (*link != NULL) {
        Om_Runtime_Object_Header *object = *link;
        if (object->fields.marked) {
            object->fields.marked = false;
            if (object->fields.allocated_size > SIZE_MAX - live_bytes) {
                live_bytes = SIZE_MAX;
            } else {
                live_bytes += object->fields.allocated_size;
            }
            link = &object->fields.next;
        } else {
            *link = object->fields.next;
            finalize_native_pointer(object);
            om_free(object->fields.external_storage);
            om_free(object);
            --context->live_object_count;
        }
    }
    context->allocation_bytes = 0;
    context->collection_threshold = context->minimum_collection_threshold;
    if (live_bytes > SIZE_MAX / 2) {
        context->collection_threshold = SIZE_MAX;
    } else if (live_bytes * 2 > context->collection_threshold) {
        context->collection_threshold = live_bytes * 2;
    }
    ++context->collection_count;
    context->collecting = false;
    return OM_STATUS_OK;
}

Om_Status om_runtime_collect(Om_Execution_Context *context)
{
    return runtime_collect_internal(context);
}

size_t om_runtime_live_object_count(const Om_Execution_Context *context)
{
    return context == NULL ? 0 : context->live_object_count;
}

size_t om_runtime_collection_count(const Om_Execution_Context *context)
{
    return context == NULL ? 0 : context->collection_count;
}

static Om_Status clone_managed_value(Om_Runtime_Object_Copy_Context *copy,
                                     const Om_Runtime_Type_Descriptor *type,
                                     const void *source,
                                     void *destination,
                                     size_t depth);

static Om_Status clone_object_value(Om_Runtime_Object_Copy_Context *copy,
                                    const Om_Runtime_Object_Header *source,
                                    void **destination,
                                    size_t depth)
{
    if (copy == NULL || source == NULL || destination == NULL || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < copy->pair_count; ++index) {
        if (copy->pairs[index].source == source) {
            const void *existing_destination =
                copy->pairs[index].destination;
            if (source->fields.type != NULL
                && source->fields.type->kind == OM_RUNTIME_TYPE_LIST) {
                om_runtime_list_mark_shared(copy->destination_context,
                    existing_destination);
            }
            *destination = (void *)existing_destination;
            return OM_STATUS_OK;
        }
    }
    const Om_Runtime_Type_Descriptor *type = source->fields.type;
    if (type == NULL || type->trace != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Runtime_Object_Header *object = NULL;
    Om_Status status = OM_STATUS_OK;
    if (type->kind == OM_RUNTIME_TYPE_STRING) {
        const Om_Runtime_String_Body *source_string =
            object_payload_const(source);
        size_t payload_size = 0;
        if (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                 source_string->length + 1, &payload_size)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        status = allocate_object(copy->destination_context, type,
            payload_size, 0, &object);
        if (status == OM_STATUS_OK) {
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            Om_Runtime_String_Body *destination_string = object_payload(object);
            destination_string->length = source_string->length;
            (void)memcpy(destination_string->bytes, source_string->bytes,
                         source_string->length + 1);
            status = om_runtime_roots_pop(copy->destination_context, root);
        }
    } else if (type->kind == OM_RUNTIME_TYPE_RECORD
               || type->kind == OM_RUNTIME_TYPE_MESSAGE) {
        if (!descriptor_valid_for_record(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type, type->size,
                                 0, &object);
        if (status == OM_STATUS_OK) {
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const unsigned char *source_payload =
                    object_payload_const(source);
                unsigned char *destination_payload = object_payload(object);
                (void)memcpy(destination_payload, source_payload, type->size);
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < type->field_count; ++index) {
                    const Om_Runtime_Field_Descriptor *field =
                        &type->fields[index];
                    status = clone_managed_value(copy, field->type,
                        source_payload + field->offset,
                        destination_payload + field->offset, depth + 1);
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    status = pop_status;
                }
            }
        }
    } else if (type->kind == OM_RUNTIME_TYPE_LIST) {
        if (type->element_type == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type,
            sizeof(Om_Runtime_List_Body), 0, &object);
        if (status == OM_STATUS_OK) {
            atomic_init(&((Om_Runtime_List_Body *)object_payload(object))
                             ->ownership_state,
                        OM_RUNTIME_LIST_OWNERSHIP_UNIQUE);
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Runtime_List_Body *source_list =
                    object_payload_const(source);
                if (source_list->transient) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                }
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < source_list->length; ++index) {
                    Om_Runtime_Value value = {0};
                    const Om_Runtime_Value source_value =
                        source_list->byte_storage
                            ? (Om_Runtime_Value){.integer =
                                  source_list->byte_items[index]}
                            : source_list->items[index];
                    if (type_is_managed(type->element_type)) {
                        status = clone_managed_value(copy, type->element_type,
                            &source_value.managed, &value.managed, depth + 1);
                    } else {
                        value = source_value;
                    }
                    void *value_root = value.managed;
                    Om_Runtime_Root_Frame *value_root_frame = NULL;
                    if (status == OM_STATUS_OK
                        && type_is_managed(type->element_type)) {
                        value_root_frame = om_runtime_roots_push(
                            copy->destination_context, &value_root, 1);
                        if (value_root_frame == NULL) {
                            status = OM_STATUS_OUT_OF_MEMORY;
                        }
                    }
                    if (status == OM_STATUS_OK) {
                        status = om_runtime_list_append(copy->destination_context,
                            object, value);
                    }
                    if (value_root_frame != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, value_root_frame);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    ((Om_Runtime_List_Body *)object_payload(object))->sealed =
                        true;
                    status = pop_status;
                }
            }
        }
    } else if (type->kind == OM_RUNTIME_TYPE_MAP) {
        if (type->key_type == NULL || type->value_type == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type,
            sizeof(Om_Runtime_Map_Body), 0, &object);
        if (status == OM_STATUS_OK) {
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Runtime_Map_Body *source_map =
                    object_payload_const(source);
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < source_map->length; ++index) {
                    Om_Runtime_Value key = source_map->entries[index].key;
                    Om_Runtime_Value value = source_map->entries[index].value;
                    Om_Runtime_Root_Frame *key_root = NULL;
                    Om_Runtime_Root_Frame *value_root = NULL;
                    if (type_is_managed(type->key_type)) {
                        key.managed = NULL;
                        status = clone_managed_value(copy,
                            type->key_type,
                            &source_map->entries[index].key.managed,
                            &key.managed, depth + 1);
                        if (status == OM_STATUS_OK) {
                            key_root = om_runtime_roots_push(
                                copy->destination_context, &key.managed, 1);
                            if (key_root == NULL) {
                                status = OM_STATUS_OUT_OF_MEMORY;
                            }
                        }
                    }
                    if (status == OM_STATUS_OK
                        && type_is_managed(type->value_type)) {
                        value.managed = NULL;
                        status = clone_managed_value(copy,
                            type->value_type,
                            &source_map->entries[index].value.managed,
                            &value.managed, depth + 1);
                        if (status == OM_STATUS_OK) {
                            value_root = om_runtime_roots_push(
                                copy->destination_context, &value.managed, 1);
                            if (value_root == NULL) {
                                status = OM_STATUS_OUT_OF_MEMORY;
                            }
                        }
                    }
                    if (status == OM_STATUS_OK) {
                        status = om_runtime_map_set(copy->destination_context,
                            object, key, value);
                    }
                    if (value_root != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, value_root);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                    if (key_root != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, key_root);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    status = pop_status;
                }
            }
        }
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    if (status == OM_STATUS_OK) {
        *destination = object;
    }
    return status;
}

static Om_Status clone_managed_value(Om_Runtime_Object_Copy_Context *copy,
                                     const Om_Runtime_Type_Descriptor *type,
                                     const void *source,
                                     void *destination,
                                     size_t depth)
{
    if (copy == NULL || type == NULL || source == NULL || destination == NULL
        || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type_is_managed(type)) {
        void *source_object = NULL;
        (void)memcpy(&source_object, source, sizeof(source_object));
        if (source_object == NULL) {
            void *null_object = NULL;
            (void)memcpy(destination, &null_object, sizeof(null_object));
            return OM_STATUS_OK;
        }
        const Om_Runtime_Object_Header *header = find_object(
            copy->source_context, source_object);
        if (header == NULL || !runtime_descriptor_compatible(type,
                header->fields.type, 0)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *cloned_object = NULL;
        Om_Status status = clone_object_value(copy, header, &cloned_object,
                                              depth + 1);
        if (status == OM_STATUS_OK) {
            (void)memcpy(destination, &cloned_object, sizeof(cloned_object));
        }
        return status;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)source + type->fields[0].offset,
            sizeof(is_ok));
        (void)memset(destination, 0, type->size);
        (void)memcpy((unsigned char *)destination + type->fields[0].offset,
            &is_ok, sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return clone_managed_value(copy, payload->type,
            (const unsigned char *)source + payload->offset,
            (unsigned char *)destination + payload->offset, depth + 1);
    }
    if (type->kind == OM_RUNTIME_TYPE_UNIT) {
        return OM_STATUS_OK;
    }
    if (type->kind != OM_RUNTIME_TYPE_NULL
        && type->kind != OM_RUNTIME_TYPE_BOOL
        && type->kind != OM_RUNTIME_TYPE_INT
        && type->kind != OM_RUNTIME_TYPE_ACTOR_REF
        && type->kind != OM_RUNTIME_TYPE_FLOAT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->size != 0) {
        (void)memcpy(destination, source, type->size);
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_object_clone(Om_Execution_Context *source_context,
                                  Om_Execution_Context *destination_context,
                                  const void *source_object,
                                  void **result)
{
    if (source_context == NULL || destination_context == NULL
        || source_object == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_Object_Header *source = find_object(source_context,
                                                          source_object);
    if (source == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Object_Copy_Context copy = {
        .source_context = source_context,
        .destination_context = destination_context,
    };
    const Om_Status status = clone_object_value(&copy, source, result, 0);
    om_free(copy.pairs);
    return status;
}
