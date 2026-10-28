#include "backend_internal.h"

Om_Status resolved_type(const Om_Type_Store *store,
                               Om_Type_Id type_id,
                               Om_Type_Id *resolved_id,
                               Om_Type_View *view)
{
    if (store == NULL || resolved_id == NULL || view == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = om_type_store_resolve(store, type_id, resolved_id);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, *resolved_id, view);
    }
    return status;
}

Om_Status language_function_type(Om_Llvm_Backend_Context *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        LLVMTypeRef *function_type)
{
    if (backend == NULL || function_type == NULL || depth > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store, type_id,
                                     &resolved_id, &view);
    if (status != OM_STATUS_OK
        || (view.kind != OM_TYPE_FUNCTION && view.kind != OM_TYPE_ACTION)
        || view.child_count == 0 || view.child_count - 1 > UINT_MAX) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    const size_t parameter_count = view.child_count - 1;
    if (parameter_count >= UINT_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMTypeRef *parameters = om_alloc_array(parameter_count + 1,
                                              sizeof(*parameters));
    if (parameters == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    parameters[0] = backend->byte_pointer_type;
    for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count;
         ++index) {
        Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, index, &parameter_type);
        if (status == OM_STATUS_OK) {
            status = language_type(backend, parameter_type, depth + 1,
                                   &parameters[index + 1]);
        }
    }
    LLVMTypeRef return_type = LLVMVoidTypeInContext(backend->context);
    if (status == OM_STATUS_OK && view.kind != OM_TYPE_ACTION) {
        Om_Type_Id return_type_id = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, parameter_count, &return_type_id);
        if (status == OM_STATUS_OK) {
            status = language_type(backend, return_type_id, depth + 1,
                                   &return_type);
        }
    }
    if (status == OM_STATUS_OK) {
        *function_type = LLVMFunctionType(return_type, parameters,
            (unsigned)(parameter_count + 1), 0);
    }
    om_free(parameters);
    return status;
}

Om_Status language_type(Om_Llvm_Backend_Context *backend,
                               Om_Type_Id type_id,
                               unsigned depth,
                               LLVMTypeRef *llvm_type)
{
    if (backend == NULL || llvm_type == NULL || depth > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store, type_id,
                                     &resolved_id, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    switch (view.kind) {
    case OM_TYPE_INT:
        *llvm_type = backend->i64_type;
        return OM_STATUS_OK;
    case OM_TYPE_FLOAT:
        *llvm_type = backend->double_type;
        return OM_STATUS_OK;
    case OM_TYPE_BOOL:
        *llvm_type = backend->i1_type;
        return OM_STATUS_OK;
    case OM_TYPE_STRING:
    case OM_TYPE_NULL:
    case OM_TYPE_NATIVE_POINTER:
        *llvm_type = backend->byte_pointer_type;
        return OM_STATUS_OK;
    case OM_TYPE_UNIT:
        *llvm_type = backend->i8_type;
        return OM_STATUS_OK;
    case OM_TYPE_FUNCTION:
    case OM_TYPE_ACTION: {
        LLVMTypeRef signature = NULL;
        status = language_function_type(backend, resolved_id, depth + 1,
                                       &signature);
        if (status == OM_STATUS_OK) {
            *llvm_type = LLVMPointerType(signature, 0);
        }
        return status;
    }
    case OM_TYPE_LIST:
    case OM_TYPE_MAP:
    case OM_TYPE_MESSAGE:
        *llvm_type = backend->byte_pointer_type;
        return OM_STATUS_OK;
    case OM_TYPE_RECORD: {
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            backend->project->resolution, view.nominal_symbol);
        if (symbol != NULL && symbol->native_module != NULL) {
            return native_record_llvm_type(backend, symbol->native_module,
                symbol->native_record_index, llvm_type);
        }
        *llvm_type = backend->byte_pointer_type;
        return OM_STATUS_OK;
    }
    case OM_TYPE_RESULT: {
        if (view.child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id ok_type_id = OM_TYPE_ID_INVALID;
        Om_Type_Id error_type_id = OM_TYPE_ID_INVALID;
        LLVMTypeRef members[3] = {backend->i1_type, NULL, NULL};
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 0, &ok_type_id);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(backend->project->type_store,
                resolved_id, 1, &error_type_id);
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, ok_type_id, depth + 1,
                                   &members[1]);
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, error_type_id, depth + 1,
                                   &members[2]);
        }
        if (status == OM_STATUS_OK) {
            *llvm_type = LLVMStructTypeInContext(backend->context, members,
                                                  3, 0);
            if (*llvm_type == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
        return status;
    }
    case OM_TYPE_NODE_REF:
    case OM_TYPE_ACTOR_ID:
    case OM_TYPE_DOWN_REASON:
    case OM_TYPE_ACTOR_REF:
        *llvm_type = backend->i64_type;
        return OM_STATUS_OK;
    case OM_TYPE_ACTOR_PROTOCOL:
    case OM_TYPE_VARIABLE:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

Om_Status type_kind(Om_Llvm_Backend_Context *backend,
                           Om_Type_Id type_id,
                           Om_Type_Kind *kind)
{
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    const Om_Status status = resolved_type(backend->project->type_store,
        type_id, &resolved_id, &view);
    if (status == OM_STATUS_OK) {
        *kind = view.kind;
    }
    return status;
}

Om_Status native_record_llvm_type(Om_Llvm_Backend_Context *backend,
                                         const Om_Omi_Module *module,
                                         size_t record_index,
                                         LLVMTypeRef *llvm_type)
{
    if (backend == NULL || module == NULL || llvm_type == NULL
        || record_index >= module->record_count || module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Record *record = &module->records[record_index];
    if (record->field_start > module->record_field_count
        || record->field_count
            > module->record_field_count - record->field_start
        || record->field_count == 0 || record->field_count > UINT_MAX
        || module->record_fields == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef *fields = om_alloc_array(record->field_count,
                                         sizeof(*fields));
    if (fields == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < record->field_count; ++index) {
        const Om_Native_Abi_Type field_type =
            module->record_fields[record->field_start + index].abi_type;
        if (field_type == OM_NATIVE_ABI_CBOOL) {
            fields[index] = backend->i8_type;
        } else {
            Om_Llvm_Native_Abi_Type field_abi = {0};
            status = native_abi_type(backend, field_type, false, &field_abi);
            if (status == OM_STATUS_OK) {
                fields[index] = field_abi.llvm_type;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        *llvm_type = LLVMStructTypeInContext(backend->context, fields,
            (unsigned)record->field_count, 0);
        if (*llvm_type == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    om_free(fields);
    return status;
}
