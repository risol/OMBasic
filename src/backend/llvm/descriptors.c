#include "backend_internal.h"

typedef struct Om_Llvm_Fingerprint_Stack {
    Om_Type_Id types[65];
    size_t count;
} Om_Llvm_Fingerprint_Stack;


static Om_Status descriptor_cache_add(Om_Llvm_Backend_Context *backend,
                                      Om_Type_Id type_id,
                                      LLVMValueRef global,
                                      uint64_t fingerprint);
static const Om_Mir_Actor_Descriptor *actor_for_symbol(
    const Om_Mir_Project *project,
    Om_Symbol_Id symbol_id);
static Om_String_View type_kind_name(Om_Type_Kind kind);
static Om_Status actor_identity_fingerprint(
    const Om_Mir_Actor_Descriptor *actor,
    uint64_t *fingerprint);
static Om_Status compute_descriptor_fingerprint(
    Om_Llvm_Backend_Context *backend,
    Om_Type_Id type_id,
    unsigned depth,
    Om_Llvm_Fingerprint_Stack *stack,
    uint64_t *fingerprint);
static Om_Status descriptor_fingerprint(Om_Llvm_Backend_Context *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        uint64_t *fingerprint);
static Om_Status nominal_descriptor_fingerprint(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Nominal_Descriptor *nominal,
    uint64_t *fingerprint);
static void sort_fingerprints(uint64_t *fingerprints, size_t count);
static Om_Status actor_protocol_fingerprint(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Actor_Descriptor *actor,
    uint64_t *fingerprint);
static Om_Status actor_down_layout(Om_Llvm_Backend_Context *backend,
                                   LLVMValueRef *type_descriptor,
                                   uint64_t *actor_id_offset,
                                   uint64_t *reason_offset);
static Om_Status program_fingerprints(Om_Llvm_Backend_Context *backend,
                                     uint64_t *program_fingerprint,
                                     uint64_t *interface_fingerprint);

Om_Status runtime_value_kind(Om_Type_Kind kind, uint32_t *runtime_kind)
{
    if (runtime_kind == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    switch (kind) {
    case OM_TYPE_UNIT: *runtime_kind = 0; return OM_STATUS_OK;
    case OM_TYPE_NULL: *runtime_kind = 1; return OM_STATUS_OK;
    case OM_TYPE_BOOL: *runtime_kind = 2; return OM_STATUS_OK;
    case OM_TYPE_INT: *runtime_kind = 3; return OM_STATUS_OK;
    case OM_TYPE_FLOAT: *runtime_kind = 4; return OM_STATUS_OK;
    case OM_TYPE_STRING: *runtime_kind = 5; return OM_STATUS_OK;
    default: return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status descriptor_cache_add(Om_Llvm_Backend_Context *backend,
                                      Om_Type_Id type_id,
                                      LLVMValueRef global,
                                      uint64_t fingerprint)
{
    if (backend->descriptor_count == backend->descriptor_capacity) {
        const size_t capacity = backend->descriptor_capacity == 0 ? 16
            : backend->descriptor_capacity * 2;
        if (capacity < backend->descriptor_capacity
            || capacity > SIZE_MAX / sizeof(*backend->descriptors)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_Llvm_Type_Descriptor_Cache *items = om_realloc_array(
            backend->descriptors, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        backend->descriptors = items;
        backend->descriptor_capacity = capacity;
    }
    backend->descriptors[backend->descriptor_count++] =
        (Om_Llvm_Type_Descriptor_Cache){type_id, global, fingerprint};
    return OM_STATUS_OK;
}

const Om_Mir_Nominal_Descriptor *nominal_for_type(
    const Om_Mir_Project *project,
    const Om_Type_View *type
)
{
    if (project == NULL || type == NULL
        || (type->kind != OM_TYPE_RECORD && type->kind != OM_TYPE_MESSAGE)) {
        return NULL;
    }
    for (size_t index = 0; index < project->nominal_count; ++index) {
        const Om_Mir_Nominal_Descriptor *nominal = &project->nominals[index];
        if (nominal->symbol_id.value == type->nominal_symbol.value
            && ((type->kind == OM_TYPE_RECORD
                 && nominal->type_kind == OM_TYPE_RECORD)
                || (type->kind == OM_TYPE_MESSAGE
                    && nominal->type_kind == OM_TYPE_MESSAGE))) {
            return nominal;
        }
    }
    return NULL;
}

static const Om_Mir_Actor_Descriptor *actor_for_symbol(
    const Om_Mir_Project *project,
    Om_Symbol_Id symbol_id)
{
    if (project == NULL) return NULL;
    for (size_t index = 0; index < project->actor_count; ++index) {
        const Om_Mir_Actor_Descriptor *actor = &project->actors[index];
        if (actor->symbol_id.value == symbol_id.value) return actor;
    }
    return NULL;
}

static Om_String_View type_kind_name(Om_Type_Kind kind)
{
    switch (kind) {
    case OM_TYPE_INT: return (Om_String_View){"Int", 3};
    case OM_TYPE_FLOAT: return (Om_String_View){"Float", 5};
    case OM_TYPE_BOOL: return (Om_String_View){"Bool", 4};
    case OM_TYPE_STRING: return (Om_String_View){"String", 6};
    case OM_TYPE_NULL: return (Om_String_View){"Null", 4};
    case OM_TYPE_UNIT: return (Om_String_View){"Unit", 4};
    case OM_TYPE_NODE_REF: return (Om_String_View){"NodeRef", 7};
    case OM_TYPE_ACTOR_ID: return (Om_String_View){"ActorId", 7};
    case OM_TYPE_DOWN_REASON:
        return (Om_String_View){"Actor_Down_Reason", 17};
    case OM_TYPE_NATIVE_POINTER: return (Om_String_View){"NativePointer", 13};
    case OM_TYPE_LIST: return (Om_String_View){"List", 4};
    case OM_TYPE_MAP: return (Om_String_View){"Map", 3};
    case OM_TYPE_RECORD: return (Om_String_View){"Record", 6};
    case OM_TYPE_MESSAGE: return (Om_String_View){"Message", 7};
    case OM_TYPE_ACTOR_PROTOCOL:
        return (Om_String_View){"ActorProtocol", 13};
    case OM_TYPE_ACTOR_REF: return (Om_String_View){"ActorRef", 8};
    case OM_TYPE_FUNCTION: return (Om_String_View){"Function", 8};
    case OM_TYPE_ACTION: return (Om_String_View){"Action", 6};
    case OM_TYPE_RESULT: return (Om_String_View){"Result", 6};
    case OM_TYPE_VARIABLE: return (Om_String_View){"Variable", 8};
    }
    return (Om_String_View){"Unknown", 7};
}

static Om_Status actor_identity_fingerprint(
    const Om_Mir_Actor_Descriptor *actor,
    uint64_t *fingerprint)
{
    static const char domain_text[] = "OMBASIC.Actor.Identity.v1";
    if (actor == NULL || fingerprint == NULL || actor->name.data == NULL
        || actor->name.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_fingerprint64_schema(
        (Om_String_View){domain_text, sizeof(domain_text) - 1},
        actor->canonical_module_path, actor->name, NULL, 0, fingerprint);
}

static Om_Status compute_descriptor_fingerprint(
    Om_Llvm_Backend_Context *backend,
    Om_Type_Id type_id,
    unsigned depth,
    Om_Llvm_Fingerprint_Stack *stack,
    uint64_t *fingerprint)
{
    static const char general_domain[] = "OMBASIC.Type.v1";
    static const char record_domain[] = "OMBASIC.Record.v1";
    static const char message_domain[] = "OMBASIC.Message.v1";
    static const char builtin_nominal_domain[] = "OMBASIC.BuiltinNominal.v1";
    static const char builtin_member_domain[] = "OMBASIC.BuiltinMember.v1";
    static const Om_String_View down_reason_members[] = {
        {"normal", 6}, {"failed", 6}, {"node_unreachable", 16}
    };
    if (backend == NULL || stack == NULL || fingerprint == NULL
        || depth >= sizeof(stack->types) / sizeof(stack->types[0])) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store, type_id,
                                     &resolved_id, &view);
    if (status != OM_STATUS_OK) return status;
    for (size_t index = 0; index < stack->count; ++index) {
        if (stack->types[index].value == resolved_id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    stack->types[stack->count++] = resolved_id;

    if (view.kind == OM_TYPE_ACTOR_PROTOCOL) {
        const Om_Mir_Actor_Descriptor *actor = actor_for_symbol(
            backend->project, view.nominal_symbol);
        status = actor_identity_fingerprint(actor, fingerprint);
        --stack->count;
        return status;
    }

    const Om_Mir_Nominal_Descriptor *nominal = nominal_for_type(
        backend->project, &view);
    const Om_Mir_Field_Descriptor *fields = NULL;
    size_t child_count = view.child_count;
    Om_String_View canonical_path = {0};
    Om_String_View entity_name = type_kind_name(view.kind);
    const char *domain = general_domain;
    size_t domain_length = sizeof(general_domain) - 1;
    if (view.kind == OM_TYPE_DOWN_REASON) {
        if (view.nominal_symbol.value == UINT32_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        domain = builtin_nominal_domain;
        domain_length = sizeof(builtin_nominal_domain) - 1;
        canonical_path = (Om_String_View){"builtin", 7};
        entity_name = (Om_String_View){"Actor_Down_Reason", 17};
        child_count = sizeof(down_reason_members)
            / sizeof(down_reason_members[0]);
    }
    if (view.kind == OM_TYPE_RECORD || view.kind == OM_TYPE_MESSAGE) {
        if (nominal != NULL) {
            if (nominal->field_start > backend->project->field_count
                || nominal->field_count > backend->project->field_count
                    - nominal->field_start) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            fields = nominal->field_count == 0 ? NULL
                : backend->project->fields + nominal->field_start;
            child_count = nominal->field_count;
            canonical_path = nominal->canonical_module_path;
            if (nominal->name.data == NULL || nominal->name.length == 0) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            entity_name = nominal->name;
        } else {
            const Om_Symbol *symbol = om_resolution_symbol_by_id(
                backend->project->resolution, view.nominal_symbol);
            if (symbol == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            entity_name = symbol->name;
            child_count = 0;
        }
        if (view.kind == OM_TYPE_MESSAGE) {
            domain = message_domain;
            domain_length = sizeof(message_domain) - 1;
        } else {
            domain = record_domain;
            domain_length = sizeof(record_domain) - 1;
        }
    }

    uint64_t *child_fingerprints = child_count == 0 ? NULL
        : om_alloc_array(child_count, sizeof(*child_fingerprints));
    if (child_count != 0 && child_fingerprints == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < child_count;
         ++index) {
        Om_Type_Id child_type = OM_TYPE_ID_INVALID;
        if (view.kind == OM_TYPE_DOWN_REASON) {
            status = om_fingerprint64_schema(
                (Om_String_View){builtin_member_domain,
                                 sizeof(builtin_member_domain) - 1},
                (Om_String_View){"builtin::Actor_Down_Reason", 26},
                down_reason_members[index], NULL, 0,
                &child_fingerprints[index]);
        } else if (fields != NULL) {
            child_type = fields[index].type_id;
        } else {
            status = om_type_store_child_at(backend->project->type_store,
                resolved_id, index, &child_type);
        }
        if (status == OM_STATUS_OK && view.kind != OM_TYPE_DOWN_REASON) {
            status = compute_descriptor_fingerprint(backend, child_type,
                depth + 1, stack, &child_fingerprints[index]);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_fingerprint64_schema(
            (Om_String_View){domain, domain_length}, canonical_path,
            entity_name, child_fingerprints, child_count, fingerprint);
    }
    om_free(child_fingerprints);

cleanup:
    --stack->count;
    return status;
}

static Om_Status descriptor_fingerprint(Om_Llvm_Backend_Context *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        uint64_t *fingerprint)
{
    Om_Llvm_Fingerprint_Stack stack = {0};
    return compute_descriptor_fingerprint(backend, type_id, depth, &stack,
                                         fingerprint);
}

static Om_Status nominal_descriptor_fingerprint(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Nominal_Descriptor *nominal,
    uint64_t *fingerprint)
{
    static const char record_domain[] = "OMBASIC.Record.v1";
    static const char message_domain[] = "OMBASIC.Message.v1";
    if (backend == NULL || nominal == NULL || fingerprint == NULL
        || nominal->field_start > backend->project->field_count
        || nominal->field_count > backend->project->field_count
            - nominal->field_start
        || nominal->name.data == NULL || nominal->name.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint64_t *field_fingerprints = nominal->field_count == 0 ? NULL
        : om_alloc_array(nominal->field_count, sizeof(*field_fingerprints));
    if (nominal->field_count != 0 && field_fingerprints == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < nominal->field_count; ++index) {
        const Om_Mir_Field_Descriptor *field = &backend->project->fields[
            nominal->field_start + index];
        status = descriptor_fingerprint(backend, field->type_id, 0,
                                        &field_fingerprints[index]);
    }
    if (status == OM_STATUS_OK) {
        const bool is_message = nominal->type_kind == OM_TYPE_MESSAGE;
        const char *domain = is_message ? message_domain : record_domain;
        const size_t domain_length = is_message
            ? sizeof(message_domain) - 1 : sizeof(record_domain) - 1;
        status = om_fingerprint64_schema(
            (Om_String_View){domain, domain_length},
            nominal->canonical_module_path, nominal->name,
            field_fingerprints, nominal->field_count, fingerprint);
    }
    om_free(field_fingerprints);
    return status;
}

static void sort_fingerprints(uint64_t *fingerprints, size_t count)
{
    for (size_t index = 1; index < count; ++index) {
        const uint64_t value = fingerprints[index];
        size_t insertion = index;
        while (insertion != 0 && fingerprints[insertion - 1] > value) {
            fingerprints[insertion] = fingerprints[insertion - 1];
            --insertion;
        }
        fingerprints[insertion] = value;
    }
}

static Om_Status actor_protocol_fingerprint(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Actor_Descriptor *actor,
    uint64_t *fingerprint)
{
    static const char domain_text[] = "OMBASIC.Actor.Protocol.v1";
    if (backend == NULL || actor == NULL || fingerprint == NULL
        || actor->handler_start > backend->project->actor_handler_count
        || actor->handler_count > backend->project->actor_handler_count
            - actor->handler_start) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint64_t *messages = actor->handler_count == 0 ? NULL
        : om_alloc_array(actor->handler_count, sizeof(*messages));
    if (actor->handler_count != 0 && messages == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < actor->handler_count; ++index) {
        status = descriptor_fingerprint(backend,
            backend->project->actor_handlers[actor->handler_start + index]
                .message_type_id,
            0, &messages[index]);
    }
    if (status == OM_STATUS_OK) {
        sort_fingerprints(messages, actor->handler_count);
        status = om_fingerprint64_schema(
            (Om_String_View){domain_text, sizeof(domain_text) - 1},
            actor->canonical_module_path, actor->name, messages,
            actor->handler_count, fingerprint);
    }
    om_free(messages);
    return status;
}

Om_Status runtime_descriptor(Om_Llvm_Backend_Context *backend,
                                    Om_Type_Id type_id,
                                    unsigned depth,
                                    LLVMValueRef *descriptor)
{
    if (backend == NULL || descriptor == NULL || depth > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *descriptor = NULL;
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store, type_id,
                                     &resolved_id, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    for (size_t index = 0; index < backend->descriptor_count; ++index) {
        if (backend->descriptors[index].type_id.value == resolved_id.value) {
            *descriptor = backend->descriptors[index].global;
            return OM_STATUS_OK;
        }
    }
    char global_name[48];
    const int name_length = snprintf(global_name, sizeof(global_name),
        ".om_type_%" PRIu32, resolved_id.value);
    if (name_length < 0 || (size_t)name_length >= sizeof(global_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMValueRef global = LLVMAddGlobal(backend->module,
        backend->runtime_type_descriptor_type, global_name);
    if (global == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    uint64_t fingerprint = 0;
    status = descriptor_fingerprint(backend, resolved_id, depth,
                                    &fingerprint);
    if (status == OM_STATUS_OK) {
        status = descriptor_cache_add(backend, resolved_id, global,
                                      fingerprint);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }

    uint32_t runtime_kind = 0;
    switch (view.kind) {
    case OM_TYPE_UNIT: runtime_kind = 0; break;
    case OM_TYPE_NULL: runtime_kind = 1; break;
    case OM_TYPE_BOOL: runtime_kind = 2; break;
    case OM_TYPE_INT: runtime_kind = 3; break;
    case OM_TYPE_FLOAT: runtime_kind = 4; break;
    case OM_TYPE_STRING: runtime_kind = 5; break;
    case OM_TYPE_RECORD: runtime_kind = 6; break;
    case OM_TYPE_MESSAGE: runtime_kind = 7; break;
    case OM_TYPE_LIST: runtime_kind = 8; break;
    case OM_TYPE_MAP: runtime_kind = 9; break;
    case OM_TYPE_RESULT: runtime_kind = 10; break;
    case OM_TYPE_NATIVE_POINTER: runtime_kind = 11; break;
    case OM_TYPE_ACTOR_REF: runtime_kind = 12; break;
    case OM_TYPE_NODE_REF:
    case OM_TYPE_ACTOR_ID:
    case OM_TYPE_DOWN_REASON: runtime_kind = 3; break;
    default: return OM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t size = 0;
    uint64_t alignment = 1;
    LLVMValueRef field_array_pointer = LLVMConstNull(
        backend->byte_pointer_type);
    uint64_t field_count = 0;
    LLVMValueRef element_type_pointer = LLVMConstNull(
        backend->byte_pointer_type);
    LLVMValueRef key_type_pointer = LLVMConstNull(
        backend->byte_pointer_type);
    LLVMValueRef value_type_pointer = LLVMConstNull(
        backend->byte_pointer_type);

    if (view.kind == OM_TYPE_UNIT || view.kind == OM_TYPE_BOOL) {
        size = 1;
    } else if (view.kind == OM_TYPE_INT || view.kind == OM_TYPE_FLOAT
               || view.kind == OM_TYPE_NODE_REF
               || view.kind == OM_TYPE_ACTOR_ID
               || view.kind == OM_TYPE_DOWN_REASON
               || view.kind == OM_TYPE_ACTOR_REF) {
        size = 8;
        alignment = 8;
    } else if (view.kind == OM_TYPE_NULL || view.kind == OM_TYPE_STRING
               || view.kind == OM_TYPE_LIST || view.kind == OM_TYPE_MAP
               || view.kind == OM_TYPE_NATIVE_POINTER) {
        size = LLVMABISizeOfType(backend->target_data,
                                 backend->byte_pointer_type);
        alignment = LLVMABIAlignmentOfType(backend->target_data,
                                           backend->byte_pointer_type);
    }
    if (view.kind == OM_TYPE_LIST) {
        if (view.child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 0, &child);
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, child, depth + 1,
                                        &element_type_pointer);
        }
    } else if (view.kind == OM_TYPE_MAP) {
        if (view.child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id mapped_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 0, &key_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(backend->project->type_store,
                resolved_id, 1, &mapped_type);
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, key_type, depth + 1,
                                        &key_type_pointer);
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, mapped_type, depth + 1,
                                        &value_type_pointer);
        }
    } else if (view.kind == OM_TYPE_RESULT) {
        if (view.child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id ok_type = OM_TYPE_ID_INVALID;
        Om_Type_Id error_type = OM_TYPE_ID_INVALID;
        Om_Type_Id tag_type = OM_TYPE_ID_INVALID;
        LLVMValueRef ok_descriptor = NULL;
        LLVMValueRef error_descriptor = NULL;
        LLVMValueRef tag_descriptor = NULL;
        LLVMTypeRef ok_llvm_type = NULL;
        LLVMTypeRef error_llvm_type = NULL;
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 0, &ok_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(backend->project->type_store,
                resolved_id, 1, &error_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_primitive(backend->project->type_store,
                OM_TYPE_BOOL, &tag_type);
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, ok_type, depth + 1,
                                        &ok_descriptor);
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, error_type, depth + 1,
                                        &error_descriptor);
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, tag_type, depth + 1,
                                        &tag_descriptor);
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, ok_type, depth + 1,
                                   &ok_llvm_type);
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, error_type, depth + 1,
                                   &error_llvm_type);
        }
        if (status != OM_STATUS_OK) return status;

        LLVMTypeRef result_members[3] = {
            backend->i1_type, ok_llvm_type, error_llvm_type,
        };
        LLVMTypeRef result_layout = LLVMStructTypeInContext(
            backend->context, result_members, 3, 0);
        if (result_layout == NULL) return OM_STATUS_OUT_OF_MEMORY;
        size = LLVMABISizeOfType(backend->target_data, result_layout);
        alignment = LLVMABIAlignmentOfType(backend->target_data,
                                           result_layout);
        field_count = 3;

        LLVMValueRef descriptors[3] = {NULL, NULL, NULL};
        LLVMValueRef type_descriptors[3] = {
            tag_descriptor, ok_descriptor, error_descriptor,
        };
        for (size_t index = 0; index < 3; ++index) {
            LLVMValueRef parts[2] = {
                LLVMConstInt(backend->i64_type,
                    LLVMOffsetOfElement(backend->target_data, result_layout,
                                        (unsigned)index), 0),
                type_descriptors[index],
            };
            descriptors[index] = LLVMConstNamedStruct(
                backend->runtime_field_descriptor_type, parts, 2);
            if (descriptors[index] == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
        }
        LLVMTypeRef fields_type = LLVMArrayType(
            backend->runtime_field_descriptor_type, 3);
        char fields_name[52];
        const int fields_name_length = snprintf(fields_name,
            sizeof(fields_name), ".om_result_fields_%" PRIu32,
            resolved_id.value);
        if (fields_name_length < 0
            || (size_t)fields_name_length >= sizeof(fields_name)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        LLVMValueRef fields_global = LLVMAddGlobal(backend->module,
            fields_type, fields_name);
        LLVMValueRef fields_initializer = LLVMConstArray(
            backend->runtime_field_descriptor_type, descriptors, 3);
        if (fields_global == NULL || fields_initializer == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMSetInitializer(fields_global, fields_initializer);
        LLVMSetGlobalConstant(fields_global, 1);
        LLVMSetLinkage(fields_global, LLVMPrivateLinkage);
        field_array_pointer = LLVMConstBitCast(fields_global,
            backend->byte_pointer_type);
    } else if (view.kind == OM_TYPE_RECORD || view.kind == OM_TYPE_MESSAGE) {
        const Om_Mir_Nominal_Descriptor *nominal = nominal_for_type(
            backend->project, &view);
        if (nominal == NULL
            || nominal->field_start > backend->project->field_count
            || nominal->field_count
                > backend->project->field_count - nominal->field_start
            || nominal->field_count > UINT_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        field_count = nominal->field_count;
        const size_t count = nominal->field_count;
        LLVMTypeRef *field_llvm_types = count == 0 ? NULL
            : om_alloc_array(count, sizeof(*field_llvm_types));
        LLVMValueRef *field_descriptors = count == 0 ? NULL
            : om_alloc_array(count, sizeof(*field_descriptors));
        if (count != 0
            && (field_llvm_types == NULL || field_descriptors == NULL)) {
            om_free(field_llvm_types);
            om_free(field_descriptors);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t index = 0; status == OM_STATUS_OK && index < count;
             ++index) {
            const Om_Mir_Field_Descriptor *field =
                &backend->project->fields[nominal->field_start + index];
            LLVMValueRef field_type_descriptor = NULL;
            status = runtime_descriptor(backend, field->type_id, depth + 1,
                                        &field_type_descriptor);
            if (status == OM_STATUS_OK) {
                status = language_type(backend, field->type_id, 0,
                                       &field_llvm_types[index]);
            }
            if (status == OM_STATUS_OK) {
                LLVMValueRef parts[2] = {
                    LLVMConstInt(backend->i64_type,
                        (uint64_t)field->symbol_id.value, 0),
                    field_type_descriptor,
                };
                field_descriptors[index] = LLVMConstNamedStruct(
                    backend->runtime_field_descriptor_type, parts, 2);
                if (field_descriptors[index] == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
        }
        LLVMTypeRef record_layout = NULL;
        if (status == OM_STATUS_OK) {
            record_layout = LLVMStructTypeInContext(backend->context,
                field_llvm_types, (unsigned)count, 0);
            if (record_layout == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
        if (status == OM_STATUS_OK) {
            size = LLVMABISizeOfType(backend->target_data, record_layout);
            alignment = LLVMABIAlignmentOfType(backend->target_data,
                                               record_layout);
            for (size_t index = 0; index < count; ++index) {
                const uint64_t offset = LLVMOffsetOfElement(
                    backend->target_data, record_layout, (unsigned)index);
                LLVMValueRef old_constant = field_descriptors[index];
                LLVMValueRef old_parts[2] = {
                    LLVMConstInt(backend->i64_type, offset, 0),
                    LLVMGetOperand(old_constant, 1),
                };
                field_descriptors[index] = LLVMConstNamedStruct(
                    backend->runtime_field_descriptor_type, old_parts, 2);
            }
        }
        if (status == OM_STATUS_OK && count != 0) {
            LLVMTypeRef array_type = LLVMArrayType(
                backend->runtime_field_descriptor_type, (unsigned)count);
            char fields_name[52];
            const int fields_name_length = snprintf(fields_name,
                sizeof(fields_name), ".om_fields_%" PRIu32,
                resolved_id.value);
            if (fields_name_length < 0
                || (size_t)fields_name_length >= sizeof(fields_name)) {
                status = OM_STATUS_OUT_OF_RANGE;
            } else {
                LLVMValueRef fields_global = LLVMAddGlobal(backend->module,
                    array_type, fields_name);
                LLVMValueRef initializer = LLVMConstArray(
                    backend->runtime_field_descriptor_type,
                    field_descriptors, (unsigned)count);
                if (fields_global == NULL || initializer == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                } else {
                    LLVMSetInitializer(fields_global, initializer);
                    LLVMSetGlobalConstant(fields_global, 1);
                    LLVMSetLinkage(fields_global, LLVMPrivateLinkage);
                    field_array_pointer = LLVMConstBitCast(fields_global,
                        backend->byte_pointer_type);
                }
            }
        }
        om_free(field_llvm_types);
        om_free(field_descriptors);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_System_Message_Id system_message_id = OM_SYSTEM_MESSAGE_ID_NONE;
    if (view.kind == OM_TYPE_RECORD || view.kind == OM_TYPE_MESSAGE) {
        const Om_Mir_Nominal_Descriptor *nominal = nominal_for_type(
            backend->project, &view);
        if (nominal == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        system_message_id = nominal->system_message_id;
    }

    LLVMValueRef members[15] = {
        LLVMConstInt(backend->i64_type, fingerprint, 0),
        LLVMConstInt(backend->i32_type, runtime_kind, 0),
        LLVMConstInt(backend->i64_type, size, 0),
        LLVMConstInt(backend->i64_type, alignment, 0),
        field_array_pointer,
        LLVMConstInt(backend->i64_type, field_count, 0),
        element_type_pointer,
        key_type_pointer,
        value_type_pointer,
        LLVMConstNull(backend->byte_pointer_type),
        LLVMConstNull(backend->byte_pointer_type),
        LLVMConstNull(backend->byte_pointer_type),
        LLVMConstNull(backend->byte_pointer_type),
        LLVMConstNull(backend->byte_pointer_type),
        LLVMConstInt(backend->i32_type, (uint32_t)system_message_id, 0),
    };
    LLVMValueRef initializer = LLVMConstNamedStruct(
        backend->runtime_type_descriptor_type, members, 15);
    if (initializer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetInitializer(global, initializer);
    LLVMSetGlobalConstant(global, 1);
    *descriptor = global;
    return OM_STATUS_OK;
}

Om_Status create_actor_runtime_descriptors(Om_Llvm_Backend_Context *backend)
{
    if (backend->project->actor_count == 0) return OM_STATUS_OK;
    if (backend->actor_descriptors == NULL) return OM_STATUS_OUT_OF_MEMORY;
    for (size_t actor_index = 0;
         actor_index < backend->project->actor_count; ++actor_index) {
        const Om_Mir_Actor_Descriptor *actor =
            &backend->project->actors[actor_index];
        if (actor->handler_start > backend->project->actor_handler_count
            || actor->handler_count > backend->project->actor_handler_count
                - actor->handler_start) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef state_type_descriptor = NULL;
        Om_Status status = runtime_descriptor(backend, actor->state_type_id,
            0, &state_type_descriptor);
        LLVMValueRef initializer = NULL;
        if (status == OM_STATUS_OK) {
            status = create_actor_state_initializer(backend, actor_index,
                                                     &initializer);
        }
        const size_t handler_count = actor->handler_count;
        LLVMValueRef *handler_values = handler_count == 0 ? NULL
            : om_alloc_array(handler_count, sizeof(*handler_values));
        if (status == OM_STATUS_OK && handler_count != 0
            && handler_values == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t handler_index = 0;
             status == OM_STATUS_OK && handler_index < handler_count;
             ++handler_index) {
            const Om_Mir_Actor_Handler *handler =
                &backend->project->actor_handlers[
                    actor->handler_start + handler_index];
            LLVMValueRef message_type_descriptor = NULL;
            status = runtime_descriptor(backend, handler->message_type_id,
                0, &message_type_descriptor);
            LLVMValueRef handler_function = NULL;
            if (status == OM_STATUS_OK) {
                status = create_actor_handler_wrapper(backend, handler,
                    &handler_function);
            }
            if (status == OM_STATUS_OK) {
                LLVMValueRef members[2] = {
                    LLVMConstBitCast(message_type_descriptor,
                        backend->byte_pointer_type),
                    handler_function,
                };
                handler_values[handler_index] = LLVMConstNamedStruct(
                    backend->runtime_actor_handler_descriptor_type,
                    members, 2);
                if (handler_values[handler_index] == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
        }
        LLVMValueRef handlers_pointer = LLVMConstNull(
            backend->byte_pointer_type);
        if (status == OM_STATUS_OK && handler_count != 0) {
            LLVMTypeRef array_type = LLVMArrayType(
                backend->runtime_actor_handler_descriptor_type,
                (unsigned)handler_count);
            char global_name[64];
            const int written = snprintf(global_name, sizeof(global_name),
                ".om_actor_%" PRIu32 "_handlers", actor->symbol_id.value);
            if (written < 0 || (size_t)written >= sizeof(global_name)) {
                status = OM_STATUS_OUT_OF_RANGE;
            } else {
                LLVMValueRef global = LLVMAddGlobal(backend->module,
                    array_type, global_name);
                LLVMValueRef initializer_value = LLVMConstArray(
                    backend->runtime_actor_handler_descriptor_type,
                    handler_values, (unsigned)handler_count);
                if (global == NULL || initializer_value == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                } else {
                    LLVMSetInitializer(global, initializer_value);
                    LLVMSetGlobalConstant(global, 1);
                    LLVMSetLinkage(global, LLVMPrivateLinkage);
                    handlers_pointer = LLVMConstBitCast(global,
                        backend->byte_pointer_type);
                }
            }
        }
        om_free(handler_values);
        if (status != OM_STATUS_OK) return status;

        char name_global[48];
        const int global_written = snprintf(name_global, sizeof(name_global),
            ".om_actor_%" PRIu32 "_name", actor->symbol_id.value);
        if (global_written < 0
            || (size_t)global_written >= sizeof(name_global)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        char *actor_name = NULL;
        status = actor_canonical_name_text(actor, &actor_name);
        if (status != OM_STATUS_OK) return status;
        LLVMValueRef canonical_name = create_private_string(backend,
            name_global, actor_name);
        om_free(actor_name);
        if (canonical_name == NULL) return OM_STATUS_OUT_OF_MEMORY;
        uint64_t fingerprint = 0;
        status = actor_protocol_fingerprint(backend, actor, &fingerprint);
        if (status != OM_STATUS_OK) return status;
        LLVMValueRef members[6] = {
            LLVMConstInt(backend->i64_type, fingerprint, 0),
            canonical_name,
            LLVMConstBitCast(state_type_descriptor,
                backend->byte_pointer_type),
            initializer,
            handlers_pointer,
            LLVMConstInt(backend->i64_type, handler_count, 0),
        };
        LLVMValueRef initializer_value = LLVMConstNamedStruct(
            backend->runtime_actor_descriptor_type, members, 6);
        if (initializer_value == NULL) return OM_STATUS_OUT_OF_MEMORY;
        char descriptor_name[48];
        const int descriptor_name_length = snprintf(descriptor_name,
            sizeof(descriptor_name), ".om_actor_%" PRIu32,
            actor->symbol_id.value);
        if (descriptor_name_length < 0
            || (size_t)descriptor_name_length >= sizeof(descriptor_name)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        LLVMValueRef descriptor_global = LLVMAddGlobal(backend->module,
            backend->runtime_actor_descriptor_type, descriptor_name);
        if (descriptor_global == NULL) return OM_STATUS_OUT_OF_MEMORY;
        LLVMSetInitializer(descriptor_global, initializer_value);
        LLVMSetGlobalConstant(descriptor_global, 1);
        LLVMSetLinkage(descriptor_global, LLVMPrivateLinkage);
        backend->actor_descriptors[actor_index] = descriptor_global;
    }
    return OM_STATUS_OK;
}

static Om_Status actor_down_layout(Om_Llvm_Backend_Context *backend,
                                   LLVMValueRef *type_descriptor,
                                   uint64_t *actor_id_offset,
                                   uint64_t *reason_offset)
{
    if (backend == NULL || type_descriptor == NULL
        || actor_id_offset == NULL || reason_offset == NULL
        || backend->project->actor_down_message_type_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View type = {0};
    Om_Status status = resolved_type(backend->project->type_store,
        backend->project->actor_down_message_type_id, &resolved_id, &type);
    const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
        ? nominal_for_type(backend->project, &type) : NULL;
    if (status != OM_STATUS_OK || type.kind != OM_TYPE_MESSAGE
        || nominal == NULL || nominal->field_count != 2
        || nominal->field_start > backend->project->field_count
        || nominal->field_count > backend->project->field_count
            - nominal->field_start) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    LLVMTypeRef field_types[2] = {NULL, NULL};
    Om_Type_Kind field_kinds[2] = {OM_TYPE_VARIABLE, OM_TYPE_VARIABLE};
    for (size_t index = 0; status == OM_STATUS_OK && index < 2; ++index) {
        const Om_Mir_Field_Descriptor *field = &backend->project->fields[
            nominal->field_start + index];
        status = type_kind(backend, field->type_id, &field_kinds[index]);
        if (status == OM_STATUS_OK) {
            status = language_type(backend, field->type_id, 0,
                                   &field_types[index]);
        }
    }
    if (status != OM_STATUS_OK) return status;
    if (field_kinds[0] != OM_TYPE_ACTOR_ID
        || field_kinds[1] != OM_TYPE_DOWN_REASON) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef layout = LLVMStructTypeInContext(backend->context,
        field_types, 2, 0);
    if (layout == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *actor_id_offset = LLVMOffsetOfElement(backend->target_data, layout, 0);
    *reason_offset = LLVMOffsetOfElement(backend->target_data, layout, 1);
    return runtime_descriptor(backend, resolved_id, 0, type_descriptor);
}

static Om_Status program_fingerprints(Om_Llvm_Backend_Context *backend,
                                     uint64_t *program_fingerprint,
                                     uint64_t *interface_fingerprint)
{
    static const char interface_domain[] = "OMBASIC.Interface.v1";
    static const char program_domain[] = "OMBASIC.Program.v1";
    if (backend == NULL || program_fingerprint == NULL
        || interface_fingerprint == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t message_count = 0;
    for (size_t index = 0; index < backend->project->nominal_count; ++index) {
        if (backend->project->nominals[index].type_kind == OM_TYPE_MESSAGE) {
            ++message_count;
        }
    }
    const size_t actor_count = backend->project->actor_count;
    const size_t type_count = backend->descriptor_count;
    if (actor_count > SIZE_MAX - 3
        || message_count > SIZE_MAX - actor_count - 2
        || type_count > SIZE_MAX - actor_count - 3) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    uint64_t *messages = message_count == 0 ? NULL
        : om_alloc_array(message_count, sizeof(*messages));
    uint64_t *actors = actor_count == 0 ? NULL
        : om_alloc_array(actor_count, sizeof(*actors));
    uint64_t *types = type_count == 0 ? NULL
        : om_alloc_array(type_count, sizeof(*types));
    uint64_t *interface_values = om_alloc_array(
        message_count + actor_count + 2, sizeof(*interface_values));
    uint64_t *program_values = om_alloc_array(
        type_count + actor_count + 3, sizeof(*program_values));
    if ((message_count != 0 && messages == NULL)
        || (actor_count != 0 && actors == NULL)
        || (type_count != 0 && types == NULL)
        || interface_values == NULL || program_values == NULL) {
        om_free(messages);
        om_free(actors);
        om_free(types);
        om_free(interface_values);
        om_free(program_values);
        return OM_STATUS_OUT_OF_MEMORY;
    }

    Om_Status status = OM_STATUS_OK;
    size_t message_index = 0;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < backend->project->nominal_count; ++index) {
        const Om_Mir_Nominal_Descriptor *nominal =
            &backend->project->nominals[index];
        if (nominal->type_kind != OM_TYPE_MESSAGE) continue;
        status = nominal_descriptor_fingerprint(backend, nominal,
                                                &messages[message_index]);
        if (status == OM_STATUS_OK) ++message_index;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < actor_count;
         ++index) {
        status = actor_protocol_fingerprint(backend,
            &backend->project->actors[index], &actors[index]);
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < type_count; ++index) {
            types[index] = backend->descriptors[index].fingerprint;
        }
        sort_fingerprints(messages, message_count);
        sort_fingerprints(actors, actor_count);
        sort_fingerprints(types, type_count);

        interface_values[0] = (uint64_t)message_count;
        if (message_count != 0) {
            (void)memcpy(interface_values + 1, messages,
                         message_count * sizeof(*messages));
        }
        interface_values[message_count + 1] = (uint64_t)actor_count;
        if (actor_count != 0) {
            (void)memcpy(interface_values + message_count + 2, actors,
                         actor_count * sizeof(*actors));
        }
        status = om_fingerprint64_schema(
            (Om_String_View){interface_domain, sizeof(interface_domain) - 1},
            (Om_String_View){0}, (Om_String_View){"interface", 9},
            interface_values, message_count + actor_count + 2,
            interface_fingerprint);
    }
    if (status == OM_STATUS_OK) {
        program_values[0] = *interface_fingerprint;
        program_values[1] = (uint64_t)type_count;
        if (type_count != 0) {
            (void)memcpy(program_values + 2, types,
                         type_count * sizeof(*types));
        }
        program_values[type_count + 2] = (uint64_t)actor_count;
        if (actor_count != 0) {
            (void)memcpy(program_values + type_count + 3, actors,
                         actor_count * sizeof(*actors));
        }
        status = om_fingerprint64_schema(
            (Om_String_View){program_domain, sizeof(program_domain) - 1},
            (Om_String_View){0}, (Om_String_View){"program", 7},
            program_values, type_count + actor_count + 3,
            program_fingerprint);
    }
    om_free(messages);
    om_free(actors);
    om_free(types);
    om_free(interface_values);
    om_free(program_values);
    return status;
}

Om_Status create_actor_program_descriptor(
    Om_Llvm_Backend_Context *backend,
    LLVMValueRef source_span_table,
    LLVMValueRef entry_thunk)
{
    if (backend == NULL || source_span_table == NULL || entry_thunk == NULL
        || backend->project->actor_count == 0
        || backend->project->actor_count > UINT_MAX
        || backend->descriptor_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (backend->program_descriptor != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    LLVMValueRef actor_down_descriptor = NULL;
    uint64_t actor_id_offset = 0;
    uint64_t reason_offset = 0;
    Om_Status status = actor_down_layout(backend, &actor_down_descriptor,
                                        &actor_id_offset, &reason_offset);
    if (status != OM_STATUS_OK) return status;

    LLVMValueRef *type_values = backend->descriptor_count == 0 ? NULL
        : om_alloc_array(backend->descriptor_count, sizeof(*type_values));
    if (backend->descriptor_count != 0 && type_values == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < backend->descriptor_count; ++index) {
        type_values[index] = LLVMConstBitCast(
            backend->descriptors[index].global, backend->byte_pointer_type);
    }
    LLVMValueRef types_pointer = LLVMConstNull(backend->byte_pointer_type);
    status = OM_STATUS_OK;
    if (backend->descriptor_count != 0) {
        LLVMTypeRef array_type = LLVMArrayType(backend->byte_pointer_type,
            (unsigned)backend->descriptor_count);
        LLVMValueRef initializer = LLVMConstArray(backend->byte_pointer_type,
            type_values, (unsigned)backend->descriptor_count);
        LLVMValueRef global = LLVMAddGlobal(backend->module, array_type,
                                             ".om_program_types");
        if (array_type == NULL || initializer == NULL || global == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            LLVMSetInitializer(global, initializer);
            LLVMSetGlobalConstant(global, 1);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            types_pointer = LLVMConstBitCast(global, backend->byte_pointer_type);
        }
    }
    om_free(type_values);

    LLVMValueRef *actor_values = status == OM_STATUS_OK
        ? om_alloc_array(backend->project->actor_count, sizeof(*actor_values))
        : NULL;
    if (status == OM_STATUS_OK && actor_values == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < backend->project->actor_count; ++index) {
        if (backend->actor_descriptors == NULL
            || backend->actor_descriptors[index] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        actor_values[index] = LLVMConstBitCast(
            backend->actor_descriptors[index], backend->byte_pointer_type);
    }
    LLVMValueRef actors_pointer = LLVMConstNull(backend->byte_pointer_type);
    if (status == OM_STATUS_OK) {
        LLVMTypeRef array_type = LLVMArrayType(backend->byte_pointer_type,
            (unsigned)backend->project->actor_count);
        LLVMValueRef initializer = LLVMConstArray(backend->byte_pointer_type,
            actor_values, (unsigned)backend->project->actor_count);
        LLVMValueRef global = LLVMAddGlobal(backend->module, array_type,
                                             ".om_program_actors");
        if (array_type == NULL || initializer == NULL || global == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            LLVMSetInitializer(global, initializer);
            LLVMSetGlobalConstant(global, 1);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            actors_pointer = LLVMConstBitCast(global,
                                               backend->byte_pointer_type);
        }
    }
    om_free(actor_values);

    if (status != OM_STATUS_OK) return status;

    uint64_t program_fingerprint = 0;
    uint64_t interface_fingerprint = 0;
    status = program_fingerprints(backend, &program_fingerprint,
                                  &interface_fingerprint);
    if (status != OM_STATUS_OK) return status;
    LLVMValueRef members[13] = {
        LLVMConstInt(backend->i64_type, program_fingerprint, 0),
        LLVMConstInt(backend->i64_type, interface_fingerprint, 0),
        types_pointer,
        LLVMConstInt(backend->i64_type, backend->descriptor_count, 0),
        actors_pointer,
        LLVMConstInt(backend->i64_type, backend->project->actor_count, 0),
        LLVMConstBitCast(actor_down_descriptor, backend->byte_pointer_type),
        LLVMConstInt(backend->i64_type, actor_id_offset, 0),
        LLVMConstInt(backend->i64_type, reason_offset, 0),
        LLVMConstBitCast(source_span_table, backend->byte_pointer_type),
        LLVMConstInt(backend->i64_type,
            backend->source_location_count, 0),
        LLVMConstBitCast(entry_thunk, backend->byte_pointer_type),
        LLVMConstInt(backend->i64_type, 0, 0),
    };
    LLVMValueRef initializer = LLVMConstNamedStruct(
        backend->runtime_program_descriptor_type, members, 13);
    LLVMValueRef global = LLVMAddGlobal(backend->module,
        backend->runtime_program_descriptor_type, ".om_program_descriptor");
    if (initializer == NULL || global == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetInitializer(global, initializer);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    backend->program_descriptor = global;
    return OM_STATUS_OK;
}
