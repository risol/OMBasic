#include "runtime_internal.h"

#include <string.h>

bool type_is_managed(const Om_Runtime_Type_Descriptor *type)
{
    return type != NULL && (type->kind == OM_RUNTIME_TYPE_STRING
        || type->kind == OM_RUNTIME_TYPE_RECORD
        || type->kind == OM_RUNTIME_TYPE_MESSAGE
        || type->kind == OM_RUNTIME_TYPE_LIST
        || type->kind == OM_RUNTIME_TYPE_MAP
        || type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER);
}

bool result_descriptor_valid(const Om_Runtime_Type_Descriptor *type)
{
    if (type == NULL || type->kind != OM_RUNTIME_TYPE_RESULT
        || type->fields == NULL || type->field_count != 3
        || type->fields[0].type == NULL
        || type->fields[0].type->kind != OM_RUNTIME_TYPE_BOOL) {
        return false;
    }
    for (size_t index = 0; index < type->field_count; ++index) {
        const Om_Runtime_Field_Descriptor *field = &type->fields[index];
        const size_t field_size = stored_value_size(field->type);
        if (field->type == NULL || field_size > type->size
            || field->offset > type->size - field_size) {
            return false;
        }
    }
    return true;
}

bool runtime_descriptor_compatible(
    const Om_Runtime_Type_Descriptor *expected,
    const Om_Runtime_Type_Descriptor *actual,
    size_t depth)
{
    if (expected == actual) return true;
    if (expected == NULL || actual == NULL || depth > 64
        || expected->kind != actual->kind
        || expected->size != actual->size
        || expected->alignment != actual->alignment) {
        return false;
    }
    switch (expected->kind) {
    case OM_RUNTIME_TYPE_STRING:
        return true;
    case OM_RUNTIME_TYPE_LIST:
        return expected->element_type != NULL && actual->element_type != NULL
            && runtime_descriptor_compatible(expected->element_type,
                                             actual->element_type, depth + 1);
    case OM_RUNTIME_TYPE_MAP:
        return expected->key_type != NULL && actual->key_type != NULL
            && expected->value_type != NULL && actual->value_type != NULL
            && runtime_descriptor_compatible(expected->key_type,
                                             actual->key_type, depth + 1)
            && runtime_descriptor_compatible(expected->value_type,
                                             actual->value_type, depth + 1);
    case OM_RUNTIME_TYPE_RESULT:
        return result_descriptor_valid(expected)
            && result_descriptor_valid(actual)
            && expected->fields[0].offset == actual->fields[0].offset
            && expected->fields[1].offset == actual->fields[1].offset
            && expected->fields[2].offset == actual->fields[2].offset
            && runtime_descriptor_compatible(expected->fields[1].type,
                                             actual->fields[1].type, depth + 1)
            && runtime_descriptor_compatible(expected->fields[2].type,
                                             actual->fields[2].type, depth + 1);
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
        return expected->fingerprint == actual->fingerprint
            && expected->field_count == actual->field_count;
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return expected->fingerprint == actual->fingerprint;
    case OM_RUNTIME_TYPE_UNIT:
    case OM_RUNTIME_TYPE_NULL:
    case OM_RUNTIME_TYPE_BOOL:
    case OM_RUNTIME_TYPE_INT:
    case OM_RUNTIME_TYPE_FLOAT:
    case OM_RUNTIME_TYPE_ACTOR_REF:
        return true;
    }
    return false;
}

bool runtime_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

size_t stored_value_size(const Om_Runtime_Type_Descriptor *type)
{
    if (type_is_managed(type)) {
        return sizeof(void *);
    }
    return type == NULL ? 0 : type->size;
}

bool descriptor_valid_for_record(
    const Om_Runtime_Type_Descriptor *type
)
{
    if (type == NULL || (type->kind != OM_RUNTIME_TYPE_RECORD
                         && type->kind != OM_RUNTIME_TYPE_MESSAGE)
        || (type->field_count != 0 && type->fields == NULL)
        || (type->alignment != 0
            && (!runtime_is_power_of_two(type->alignment)
                || type->alignment > _Alignof(Om_Runtime_Object_Header)))) {
        return false;
    }
    for (size_t index = 0; index < type->field_count; ++index) {
        const Om_Runtime_Field_Descriptor *field = &type->fields[index];
        const size_t field_size = stored_value_size(field->type);
        if (field->type == NULL || field_size > type->size
            || field->offset > type->size - field_size) {
            return false;
        }
    }
    return true;
}

bool value_matches_type(const Om_Execution_Context *context,
                        const Om_Runtime_Type_Descriptor *type,
                        Om_Runtime_Value value)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case OM_RUNTIME_TYPE_UNIT:
        return true;
    case OM_RUNTIME_TYPE_NULL:
        return value.managed == NULL;
    case OM_RUNTIME_TYPE_BOOL:
        return type->size == sizeof(value.boolean);
    case OM_RUNTIME_TYPE_INT:
        return type->size == sizeof(value.integer);
    case OM_RUNTIME_TYPE_ACTOR_REF:
        return type->size == sizeof(value.integer);
    case OM_RUNTIME_TYPE_FLOAT:
        return type->size == sizeof(value.floating);
    case OM_RUNTIME_TYPE_STRING:
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
    case OM_RUNTIME_TYPE_LIST:
    case OM_RUNTIME_TYPE_MAP:
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return value.managed == NULL
            || find_object(context, value.managed) != NULL;
    case OM_RUNTIME_TYPE_RESULT:
        return false;
    }
    return false;
}

void store_value(void *address,
                 const Om_Runtime_Type_Descriptor *type,
                 Om_Runtime_Value value)
{
    if (type->kind == OM_RUNTIME_TYPE_BOOL) {
        (void)memcpy(address, &value.boolean, sizeof(value.boolean));
    } else if (type->kind == OM_RUNTIME_TYPE_INT) {
        (void)memcpy(address, &value.integer, sizeof(value.integer));
    } else if (type->kind == OM_RUNTIME_TYPE_ACTOR_REF) {
        (void)memcpy(address, &value.integer, sizeof(value.integer));
    } else if (type->kind == OM_RUNTIME_TYPE_FLOAT) {
        (void)memcpy(address, &value.floating, sizeof(value.floating));
    } else if (type_is_managed(type)) {
        (void)memcpy(address, &value.managed, sizeof(value.managed));
    }
}
