#include "ombasic/runtime/vm.h"

#include "../vm_internal.h"

Om_VM_Value om_vm_value_null(void)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_NULL};
}

Om_VM_Value om_vm_value_unit(void)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_UNIT};
}

Om_VM_Value om_vm_value_bool(bool value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_BOOL, .as.boolean = value};
}

Om_VM_Value om_vm_value_int(int64_t value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_INT, .as.integer = value};
}

Om_VM_Value om_vm_value_float(double value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_FLOAT, .as.floating = value};
}

Om_VM_Value om_vm_value_actor_id(uint64_t value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_ACTOR_ID, .as.actor_id = value};
}

Om_VM_Value om_vm_value_node_ref(uint64_t value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_NODE_REF, .as.node_ref = value};
}

Om_VM_Value om_vm_value_actor_ref(uint64_t value)
{
    return (Om_VM_Value){.tag = OM_VM_VALUE_ACTOR_REF, .as.actor_id = value};
}

bool om_vm_value_as_bool(Om_VM_Value value, bool *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_BOOL) {
        return false;
    }
    *out = value.as.boolean;
    return true;
}

bool om_vm_value_as_int(Om_VM_Value value, int64_t *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_INT) {
        return false;
    }
    *out = value.as.integer;
    return true;
}

bool om_vm_value_as_float(Om_VM_Value value, double *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_FLOAT) {
        return false;
    }
    *out = value.as.floating;
    return true;
}

bool om_vm_value_as_actor_id(Om_VM_Value value, uint64_t *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_ACTOR_ID) {
        return false;
    }
    *out = value.as.actor_id;
    return true;
}

bool om_vm_value_as_node_ref(Om_VM_Value value, uint64_t *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_NODE_REF) {
        return false;
    }
    *out = value.as.node_ref;
    return true;
}

bool om_vm_value_as_actor_ref(Om_VM_Value value, uint64_t *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_ACTOR_REF) {
        return false;
    }
    *out = value.as.actor_id;
    return true;
}

bool om_vm_value_as_string(Om_VM_Value value, Om_String_View *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_OBJECT
        || value.as.object == NULL
        || value.as.object->kind != OM_VM_OBJECT_STRING) {
        return false;
    }
    const Om_VM_String_Object *string = (const Om_VM_String_Object *)
        value.as.object;
    *out = (Om_String_View){string->bytes, string->length};
    return true;
}

bool om_vm_value_as_result(Om_VM_Value value, bool *is_ok,
                           Om_VM_Value *payload)
{
    if (value.tag != OM_VM_VALUE_OBJECT || value.as.object == NULL
        || value.as.object->kind != OM_VM_OBJECT_RESULT) {
        return false;
    }
    const Om_VM_Result_Object *result = (const Om_VM_Result_Object *)
        value.as.object;
    if (is_ok != NULL) {
        *is_ok = result->is_ok;
    }
    if (payload != NULL) {
        *payload = result->payload;
    }
    return true;
}

bool om_vm_value_as_list(Om_VM_Value value, const Om_VM_Value **items,
                         size_t *item_count)
{
    if (value.tag != OM_VM_VALUE_OBJECT || value.as.object == NULL
        || value.as.object->kind != OM_VM_OBJECT_LIST) {
        return false;
    }
    const Om_VM_List_Object *list = (const Om_VM_List_Object *)value.as.object;
    if (items != NULL) {
        *items = list->items;
    }
    if (item_count != NULL) {
        *item_count = list->length;
    }
    return true;
}

bool om_vm_value_object_kind(Om_VM_Value value, Om_VM_Object_Kind *out)
{
    if (out == NULL || value.tag != OM_VM_VALUE_OBJECT
        || value.as.object == NULL) {
        return false;
    }
    *out = value.as.object->kind;
    return true;
}
