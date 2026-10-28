#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

Om_Status om_runtime_map_create(Om_Execution_Context *context,
                                const Om_Runtime_Type_Descriptor *type,
                                void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_MAP || type->key_type == NULL
        || type->value_type == NULL
        || (type->key_type->kind != OM_RUNTIME_TYPE_INT
            && type->key_type->kind != OM_RUNTIME_TYPE_BOOL
            && type->key_type->kind != OM_RUNTIME_TYPE_STRING
            && type->key_type->kind != OM_RUNTIME_TYPE_ACTOR_REF)
        || type->value_type->kind > OM_RUNTIME_TYPE_ACTOR_REF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type,
        sizeof(Om_Runtime_Map_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

static Om_Status map_key_equal(const Om_Execution_Context *context,
                               const Om_Runtime_Type_Descriptor *type,
                               Om_Runtime_Value left,
                               Om_Runtime_Value right,
                               bool *equal)
{
    if (type->kind == OM_RUNTIME_TYPE_INT
        || type->kind == OM_RUNTIME_TYPE_ACTOR_REF) {
        *equal = left.integer == right.integer;
    } else if (type->kind == OM_RUNTIME_TYPE_BOOL) {
        *equal = left.boolean == right.boolean;
    } else if (type->kind == OM_RUNTIME_TYPE_STRING) {
        if (left.managed == right.managed) {
            *equal = true;
        } else if (left.managed == NULL || right.managed == NULL) {
            *equal = false;
        } else if (find_object(context, left.managed) == NULL
                   || find_object(context, right.managed) == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        } else {
            Om_String_View left_view = {0};
            Om_String_View right_view = {0};
            Om_Status status = om_runtime_string_view(context, left.managed,
                                                       &left_view);
            if (status == OM_STATUS_OK) {
                status = om_runtime_string_view(context, right.managed,
                                                 &right_view);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
            *equal = left_view.length == right_view.length
                && (left_view.length == 0
                    || memcmp(left_view.data, right_view.data,
                              left_view.length) == 0);
        }
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}

static Om_Status map_find_key(const Om_Execution_Context *context,
                              const Om_Runtime_Object_Header *object,
                              Om_Runtime_Value key,
                              size_t *index,
                              bool *found)
{
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *found = false;
    for (size_t offset = 0; offset < map->length; ++offset) {
        bool equal = false;
        const Om_Status status = map_key_equal(context, type->key_type, key,
            map->entries[offset].key, &equal);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (equal) {
            *index = offset;
            *found = true;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status map_reserve(Om_Execution_Context *context,
                             Om_Runtime_Object_Header *object,
                             size_t requested_capacity)
{
    Om_Runtime_Map_Body *map = object_payload(object);
    if (requested_capacity <= map->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = map->capacity == 0 ? 4 : map->capacity;
    while (capacity < requested_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = requested_capacity;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*map->entries)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t new_bytes = capacity * sizeof(*map->entries);
    const size_t old_bytes = map->capacity * sizeof(*map->entries);
    Om_Status status = allocation_safepoint(context,
        new_bytes > old_bytes ? new_bytes - old_bytes : 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Map_Entry *entries = om_realloc_array(map->entries, capacity,
                                                     sizeof(*entries));
    if (entries == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (capacity > map->capacity) {
        (void)memset(entries + map->capacity, 0,
            (capacity - map->capacity) * sizeof(*entries));
    }
    map->entries = entries;
    map->capacity = capacity;
    object->fields.external_storage = entries;
    object->fields.allocated_size += new_bytes - old_bytes;
    context->allocation_bytes += new_bytes - old_bytes;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_set(Om_Execution_Context *context,
                             void *map_pointer,
                             Om_Runtime_Value key,
                             Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->key_type, key)
        || !value_matches_type(context, type->value_type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t index = 0;
    bool found = false;
    Om_Status status = map_find_key(context, object, key, &index, &found);
    Om_Runtime_Map_Body *map = object_payload(object);
    if (status == OM_STATUS_OK && !found) {
        if (map->length == SIZE_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        status = map_reserve(context, object, map->length + 1);
        if (status == OM_STATUS_OK) {
            index = map->length++;
        }
    }
    if (status == OM_STATUS_OK) {
        map->entries[index] = (Om_Runtime_Map_Entry){key, value};
    }
    return status;
}

Om_Status om_runtime_map_set_values(Om_Execution_Context *context,
                                    void *map,
                                    const Om_Runtime_Value *key,
                                    const Om_Runtime_Value *value)
{
    if (key == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_runtime_map_set(context, map, *key, *value);
}

Om_Status om_runtime_map_get(const Om_Execution_Context *context,
                             const void *map_pointer,
                             Om_Runtime_Value key,
                             Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->key_type, key)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t index = 0;
    bool found = false;
    const Om_Status status = map_find_key(context, object, key, &index,
                                           &found);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (!found) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *result = map->entries[index].value;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_get_value(const Om_Execution_Context *context,
                                   const void *map,
                                   const Om_Runtime_Value *key,
                                   Om_Runtime_Value *result)
{
    return key == NULL ? OM_STATUS_INVALID_ARGUMENT
        : om_runtime_map_get(context, map, *key, result);
}

Om_Status om_runtime_map_key_at(const Om_Execution_Context *context,
                                const void *map_pointer,
                                size_t index,
                                Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    if (index >= map->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *result = map->entries[index].key;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_length(const Om_Execution_Context *context,
                                const void *map_pointer,
                                size_t *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *result = map->length;
    return OM_STATUS_OK;
}

