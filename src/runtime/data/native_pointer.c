#include "runtime_internal.h"

static Om_Status trace_native_pointer(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context)
{
    if (context == NULL || value == NULL || visitor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Native_Pointer_Body *body = value;
    return body->owner == NULL ? OM_STATUS_OK
        : visitor(body->owner, visitor_context);
}

const Om_Runtime_Type_Descriptor RUNTIME_NATIVE_POINTER_TYPE = {
    .fingerprint = UINT64_C(0x6f6d6e6174697665),
    .kind = OM_RUNTIME_TYPE_NATIVE_POINTER,
    .size = sizeof(Om_Runtime_Native_Pointer_Body),
    .alignment = _Alignof(Om_Runtime_Native_Pointer_Body),
    .trace = trace_native_pointer,
};

void finalize_native_pointer(Om_Runtime_Object_Header *object)
{
    if (object == NULL || object->fields.type == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_NATIVE_POINTER) {
        return;
    }
    Om_Runtime_Native_Pointer_Body *body = object_payload(object);
    Om_Runtime_Native_Pointer_Release release = body->release;
    void *pointer = body->pointer;
    body->release = NULL;
    body->pointer = NULL;
    body->owner = NULL;
    if (release != NULL && pointer != NULL) {
        release(pointer);
    }
}

Om_Status om_runtime_native_pointer_create(
    Om_Execution_Context *context,
    void *pointer,
    Om_Runtime_Native_Pointer_Release release,
    void *owner,
    void **result)
{
    if (context == NULL || result == NULL
        || (owner != NULL && find_object(context, owner) == NULL)) {
        if (release != NULL && pointer != NULL) {
            release(pointer);
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *owner_root = owner;
    Om_Runtime_Root_Frame *owner_frame = owner == NULL ? NULL
        : om_runtime_roots_push(context, &owner_root, 1);
    if (owner != NULL && owner_frame == NULL) {
        if (release != NULL && pointer != NULL) {
            release(pointer);
        }
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Runtime_Object_Header *object = NULL;
    Om_Status status = allocate_object(context, &RUNTIME_NATIVE_POINTER_TYPE,
        sizeof(Om_Runtime_Native_Pointer_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        Om_Runtime_Native_Pointer_Body *body = object_payload(object);
        *body = (Om_Runtime_Native_Pointer_Body){
            .pointer = pointer,
            .release = release,
            .owner = owner_root,
        };
    } else if (release != NULL && pointer != NULL) {
        release(pointer);
    }
    if (owner_frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context,
                                                           owner_frame);
        if (status == OM_STATUS_OK && pop_status != OM_STATUS_OK) {
            finalize_native_pointer(object);
            status = pop_status;
        }
    }
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

Om_Status om_runtime_native_pointer_borrow(
    const Om_Execution_Context *context,
    const void *native_pointer,
    void **result)
{
    if (context == NULL || native_pointer == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Object_Header *object = find_object(context,
                                                          native_pointer);
    if (object == NULL || object->fields.type
            != &RUNTIME_NATIVE_POINTER_TYPE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Native_Pointer_Body *body = object_payload_const(object);
    *result = body->pointer;
    return OM_STATUS_OK;
}

Om_Status om_runtime_native_pointer_take(
    Om_Execution_Context *context,
    void *native_pointer,
    void **result)
{
    if (context == NULL || native_pointer == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Object_Header *object = find_object(context, native_pointer);
    if (object == NULL || object->fields.type
            != &RUNTIME_NATIVE_POINTER_TYPE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Native_Pointer_Body *body = object_payload(object);
    if (body->release == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = body->pointer;
    body->pointer = NULL;
    body->release = NULL;
    body->owner = NULL;
    return OM_STATUS_OK;
}
