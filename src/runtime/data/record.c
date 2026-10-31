#include "runtime_internal.h"

#include <string.h>

#include "ombasic/foundation/memory.h"

Om_Status om_runtime_record_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   void **result)
{
    if (context == NULL || result == NULL || !descriptor_valid_for_record(type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type, type->size, 0,
                                             &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

Om_Status om_runtime_record_set(Om_Execution_Context *context,
                                void *record,
                                size_t field_index,
                                Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (!value_matches_type(context, field->type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    om_runtime_list_mark_value_shared(context, field->type, value);
    store_value((unsigned char *)object_payload(object) + field->offset,
                field->type, value);
    return OM_STATUS_OK;
}

static bool record_has_other_list_field(const Om_Runtime_Object_Header *object,
                                        size_t excluded_field,
                                        const void *list)
{
    if (object == NULL || list == NULL) return false;
    const unsigned char *payload = object_payload_const(object);
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    for (size_t index = 0; index < type->field_count; ++index) {
        if (index == excluded_field
            || type->fields[index].type == NULL
            || type->fields[index].type->kind != OM_RUNTIME_TYPE_LIST) {
            continue;
        }
        void *field_list = NULL;
        (void)memcpy(&field_list,
            payload + type->fields[index].offset, sizeof(field_list));
        if (field_list == list) return true;
    }
    return false;
}

Om_Status om_runtime_record_set_actor_state_init(
    Om_Execution_Context *context,
    void *record,
    size_t field_index,
    const void *value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || value == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type == NULL || field->type->kind != OM_RUNTIME_TYPE_LIST) {
        return om_runtime_record_set_value(context, record, field_index, value);
    }
    Om_Runtime_Value scalar = {0};
    const size_t value_size = stored_value_size(field->type);
    if (value_size == 0 || value_size > sizeof(scalar)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memcpy(&scalar, value, value_size);
    if (!value_matches_type(context, field->type, scalar)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (scalar.managed != NULL
        && record_has_other_list_field(object, field_index, scalar.managed)) {
        om_runtime_list_mark_shared(context, scalar.managed);
    }
    store_value((unsigned char *)object_payload(object) + field->offset,
                field->type, scalar);
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_set_actor_state(
    Om_Execution_Context *context,
    void *record,
    size_t field_index,
    const void *value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || value == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type == NULL || field->type->kind != OM_RUNTIME_TYPE_LIST) {
        return om_runtime_record_set_value(context, record, field_index, value);
    }
    Om_Runtime_Value scalar = {0};
    const size_t value_size = stored_value_size(field->type);
    if (value_size == 0 || value_size > sizeof(scalar)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memcpy(&scalar, value, value_size);
    if (!value_matches_type(context, field->type, scalar)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *existing_list = NULL;
    (void)memcpy(&existing_list,
        (unsigned char *)object_payload(object) + field->offset,
        sizeof(existing_list));
    bool preserve_consumed_unique = existing_list != NULL
        && existing_list == scalar.managed
        && !record_has_other_list_field(object, field_index, scalar.managed);
    Om_Runtime_Object_Header *list_object = preserve_consumed_unique
        ? find_object(context, scalar.managed) : NULL;
    if (list_object == NULL || list_object->fields.type->kind
            != OM_RUNTIME_TYPE_LIST) {
        preserve_consumed_unique = false;
    }
    if (preserve_consumed_unique) {
        const Om_Runtime_List_Body *list = object_payload_const(list_object);
        preserve_consumed_unique = atomic_load_explicit(&list->ownership_state,
            memory_order_acquire) == OM_RUNTIME_LIST_OWNERSHIP_CONSUMED;
    }
    if (!preserve_consumed_unique) {
        om_runtime_list_mark_value_shared(context, field->type, scalar);
    }
    store_value((unsigned char *)object_payload(object) + field->offset,
                field->type, scalar);
    return OM_STATUS_OK;
}

static Om_Status validate_inline_value_data(
    const Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    const void *value,
    size_t depth)
{
    if (context == NULL || type == NULL || value == NULL || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)value + type->fields[0].offset,
            sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return validate_inline_value_data(context, payload->type,
            (const unsigned char *)value + payload->offset, depth + 1);
    }
    if (type_is_managed(type)) {
        void *managed = NULL;
        (void)memcpy(&managed, value, sizeof(managed));
        if (managed == NULL) return OM_STATUS_OK;
        const Om_Runtime_Object_Header *object = find_object(context, managed);
        if (object == NULL || object->fields.type->kind != type->kind
            || (type->kind != OM_RUNTIME_TYPE_STRING
                && type->system_message_id != OM_SYSTEM_MESSAGE_ID_ANY
                && !runtime_descriptor_compatible(type,
                    object->fields.type, 0))) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return OM_STATUS_OK;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_set_value(Om_Execution_Context *context,
                                      void *record,
                                      size_t field_index,
                                      const void *value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || value == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type->kind != OM_RUNTIME_TYPE_RESULT) {
        Om_Runtime_Value scalar = {0};
        const size_t size = stored_value_size(field->type);
        if (size > sizeof(scalar)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        (void)memcpy(&scalar, value, size);
        return om_runtime_record_set(context, record, field_index, scalar);
    }
    if (!result_descriptor_valid(field->type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status value_status = validate_inline_value_data(context,
        field->type, value, 0);
    if (value_status != OM_STATUS_OK) return value_status;
    om_runtime_list_mark_inline_value_shared(context, field->type, value);
    (void)memcpy((unsigned char *)object_payload(object) + field->offset,
                 value, field->type->size);
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_get(const Om_Execution_Context *context,
                                const void *record,
                                size_t field_index,
                                void *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || result == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(field->type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        (void)memcpy(result,
            (const unsigned char *)object_payload_const(object) + field->offset,
            field->type->size);
        return OM_STATUS_OK;
    }
    const size_t result_size = stored_value_size(field->type);
    if (result_size != 0) {
        (void)memcpy(result,
            (const unsigned char *)object_payload_const(object) + field->offset,
            result_size);
    }
    return OM_STATUS_OK;
}

