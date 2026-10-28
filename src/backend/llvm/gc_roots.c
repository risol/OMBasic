#include "backend_internal.h"

static bool is_directly_managed_type(const Om_Llvm_Backend_Context *backend,
                                     const Om_Type_View *type);
static Om_Status managed_root_slot_count(Om_Llvm_Backend_Context *backend,
                                         Om_Type_Id type_id,
                                         unsigned depth,
                                         size_t *count);
static LLVMValueRef root_slot_address(Om_Llvm_Backend_Context *backend,
                                      Om_Mir_Function_Id function_id,
                                      uint32_t slot_index);
static Om_Status write_managed_root_values(
    Om_Llvm_Backend_Context *backend,
    Om_Mir_Function_Id function_id,
    Om_Type_Id type_id,
    LLVMValueRef value,
    LLVMValueRef active,
    uint32_t *next_slot,
    unsigned depth
);

static bool is_directly_managed_type(const Om_Llvm_Backend_Context *backend,
                                     const Om_Type_View *type)
{
    if (backend == NULL || type == NULL) {
        return false;
    }
    if (type->kind == OM_TYPE_RECORD) {
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            backend->project->resolution, type->nominal_symbol);
        if (symbol != NULL && symbol->native_module != NULL) {
            return false;
        }
    }
    return type->kind == OM_TYPE_STRING || type->kind == OM_TYPE_RECORD
        || type->kind == OM_TYPE_MESSAGE || type->kind == OM_TYPE_LIST
        || type->kind == OM_TYPE_MAP || type->kind == OM_TYPE_NATIVE_POINTER;
}

static Om_Status managed_root_slot_count(Om_Llvm_Backend_Context *backend,
                                         Om_Type_Id type_id,
                                         unsigned depth,
                                         size_t *count)
{
    if (backend == NULL || count == NULL || depth > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *count = 0;
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store,
        type_id, &resolved_id, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (is_directly_managed_type(backend, &view)) {
        *count = 1;
        return OM_STATUS_OK;
    }
    if (view.kind != OM_TYPE_RESULT) {
        return OM_STATUS_OK;
    }
    if (view.child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t ok_count = 0;
    size_t error_count = 0;
    Om_Type_Id child = OM_TYPE_ID_INVALID;
    status = om_type_store_child_at(backend->project->type_store,
        resolved_id, 0, &child);
    if (status == OM_STATUS_OK) {
        status = managed_root_slot_count(backend, child, depth + 1,
                                         &ok_count);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 1, &child);
    }
    if (status == OM_STATUS_OK) {
        status = managed_root_slot_count(backend, child, depth + 1,
                                         &error_count);
    }
    if (status == OM_STATUS_OK
        && !om_checked_size_add(ok_count, error_count, count)) {
        status = OM_STATUS_OVERFLOW;
    }
    return status;
}

static LLVMValueRef root_slot_address(Om_Llvm_Backend_Context *backend,
                                      Om_Mir_Function_Id function_id,
                                      uint32_t slot_index)
{
    if (function_id.value >= backend->project->function_count
        || backend->root_arrays[function_id.value] == NULL) {
        return NULL;
    }
    const size_t count = backend->root_counts[function_id.value];
    LLVMTypeRef array_type = LLVMArrayType2(backend->byte_pointer_type,
        count == 0 ? 1 : count);
    LLVMValueRef indices[2] = {
        LLVMConstInt(backend->i32_type, 0, 0),
        LLVMConstInt(backend->i32_type, slot_index, 0),
    };
    return LLVMBuildInBoundsGEP2(backend->builder, array_type,
        backend->root_arrays[function_id.value], indices, 2,
        "managed_root_slot");
}

static Om_Status write_managed_root_values(
    Om_Llvm_Backend_Context *backend,
    Om_Mir_Function_Id function_id,
    Om_Type_Id type_id,
    LLVMValueRef value,
    LLVMValueRef active,
    uint32_t *next_slot,
    unsigned depth
)
{
    if (backend == NULL || value == NULL || next_slot == NULL || depth > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved_id = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    Om_Status status = resolved_type(backend->project->type_store,
        type_id, &resolved_id, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (is_directly_managed_type(backend, &view)) {
        LLVMValueRef rooted_value = value;
        if (active != NULL) {
            rooted_value = LLVMBuildSelect(backend->builder, active, value,
                LLVMConstNull(backend->byte_pointer_type),
                "active_result_root");
        }
        LLVMValueRef address = root_slot_address(backend, function_id,
                                                 *next_slot);
        if (rooted_value == NULL || address == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMBuildStore(backend->builder, rooted_value, address);
        if (*next_slot == UINT32_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        ++*next_slot;
        return OM_STATUS_OK;
    }
    if (view.kind != OM_TYPE_RESULT) {
        return OM_STATUS_OK;
    }
    if (view.child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef tag = LLVMBuildExtractValue(backend->builder, value, 0,
                                              "result_is_ok");
    LLVMValueRef ok_payload = LLVMBuildExtractValue(backend->builder, value,
        1, "result_ok_for_roots");
    LLVMValueRef error_payload = LLVMBuildExtractValue(backend->builder,
        value, 2, "result_error_for_roots");
    LLVMValueRef not_tag = tag == NULL ? NULL
        : LLVMBuildNot(backend->builder, tag, "result_is_error");
    if (tag == NULL || ok_payload == NULL || error_payload == NULL
        || not_tag == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef ok_active = tag;
    LLVMValueRef error_active = not_tag;
    if (active != NULL) {
        ok_active = LLVMBuildAnd(backend->builder, active, tag,
                                 "nested_result_ok_active");
        error_active = LLVMBuildAnd(backend->builder, active, not_tag,
                                    "nested_result_error_active");
        if (ok_active == NULL || error_active == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    Om_Type_Id child = OM_TYPE_ID_INVALID;
    status = om_type_store_child_at(backend->project->type_store,
        resolved_id, 0, &child);
    if (status == OM_STATUS_OK) {
        status = write_managed_root_values(backend, function_id, child,
            ok_payload, ok_active, next_slot, depth + 1);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_child_at(backend->project->type_store,
            resolved_id, 1, &child);
    }
    if (status == OM_STATUS_OK) {
        status = write_managed_root_values(backend, function_id, child,
            error_payload, error_active, next_slot, depth + 1);
    }
    return status;
}

Om_Status store_value_root(Om_Llvm_Backend_Context *backend,
                                  Om_Mir_Function_Id function_id,
                                  Om_Mir_Value_Id value_id,
                                  LLVMValueRef value)
{
    if (value_id.value >= backend->project->value_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t slot_index = backend->root_slot_indices[value_id.value];
    const uint32_t slot_count =
        backend->root_slot_counts_by_value[value_id.value];
    if (slot_count == 0) {
        return OM_STATUS_OK;
    }
    if (slot_index == UINT32_MAX || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t next_slot = slot_index;
    const Om_Status status = write_managed_root_values(backend, function_id,
        backend->project->values[value_id.value].type_id, value, NULL,
        &next_slot, 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (next_slot - slot_index != slot_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}

Om_Status initialize_function_roots(Om_Llvm_Backend_Context *backend)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t value_index = 0; status == OM_STATUS_OK
         && value_index < backend->project->value_count; ++value_index) {
        const Om_Mir_Value *value = &backend->project->values[value_index];
        size_t slot_count = 0;
        status = managed_root_slot_count(backend, value->type_id, 0,
                                         &slot_count);
        if (status != OM_STATUS_OK || slot_count == 0) {
            continue;
        }
        if (value->defining_block_id.value >= backend->project->block_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Mir_Function_Id function_id = backend->project->blocks[
            value->defining_block_id.value].function_id;
        if (function_id.value >= backend->project->function_count
            || slot_count > UINT32_MAX
            || backend->root_counts[function_id.value] > UINT32_MAX - slot_count) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        backend->root_slot_indices[value_index] = (uint32_t)
            backend->root_counts[function_id.value];
        backend->root_slot_counts_by_value[value_index] =
            (uint32_t)slot_count;
        backend->root_counts[function_id.value] += slot_count;
    }
    for (size_t function_index = 0; status == OM_STATUS_OK
         && function_index < backend->project->function_count; ++function_index) {
        const Om_Mir_Function *function =
            &backend->project->functions[function_index];
        if (function->entry_block_id.value >= backend->project->block_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const size_t root_count = backend->root_counts[function_index];
        const size_t allocation_count = root_count == 0 ? 1 : root_count;
        LLVMTypeRef array_type = LLVMArrayType2(backend->byte_pointer_type,
                                                allocation_count);
        LLVMPositionBuilderAtEnd(backend->builder,
            backend->blocks[function->entry_block_id.value]);
        LLVMValueRef root_array = LLVMBuildAlloca(backend->builder, array_type,
                                                   "managed_roots");
        if (root_array == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        backend->root_arrays[function_index] = root_array;
        for (size_t slot = 0; slot < allocation_count; ++slot) {
            LLVMValueRef indices[2] = {
                LLVMConstInt(backend->i32_type, 0, 0),
                LLVMConstInt(backend->i32_type, slot, 0),
            };
            LLVMValueRef address = LLVMBuildInBoundsGEP2(backend->builder,
                array_type, root_array, indices, 2, "managed_root_init_slot");
            if (address == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            LLVMBuildStore(backend->builder,
                LLVMConstNull(backend->byte_pointer_type), address);
        }
        LLVMValueRef root_slots = root_count == 0
            ? LLVMConstNull(backend->byte_pointer_type) : root_array;
        LLVMValueRef arguments[3] = {
            context_for_function(backend, function), root_slots,
            LLVMConstInt(backend->i64_type, root_count, 0),
        };
        LLVMValueRef frame = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_roots_push),
            backend->runtime_roots_push, arguments, 3, "managed_root_frame");
        if (frame == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntEQ,
            frame, LLVMConstNull(backend->byte_pointer_type),
            "managed_root_push_failed");
        if (failed == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        status = add_checked_continuation(backend, function->entry_block_id,
            failed, OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED, UINT32_MAX);
        if (status != OM_STATUS_OK) {
            break;
        }
        backend->root_frames[function_index] = frame;

        const Om_Mir_Block *entry = &backend->project->blocks[
            function->entry_block_id.value];
        if (entry->parameter_count + 1
            != (size_t)LLVMCountParams(backend->functions[function_index])) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK && parameter_index < entry->parameter_count;
             ++parameter_index) {
            const Om_Mir_Value *parameter = &backend->project->values[
                entry->parameter_start + parameter_index];
            status = store_value_root(backend, function->id, parameter->id,
                LLVMGetParam(backend->functions[function_index],
                             (unsigned)parameter_index + 1));
        }
    }
    for (size_t block_index = 0; status == OM_STATUS_OK
         && block_index < backend->project->block_count; ++block_index) {
        const Om_Mir_Block *block = &backend->project->blocks[block_index];
        const Om_Mir_Function *function = &backend->project->functions[
            block->function_id.value];
        if (block->id.value == function->entry_block_id.value) {
            continue;
        }
        LLVMPositionBuilderAtEnd(backend->builder,
                                 backend->blocks[block->id.value]);
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK && parameter_index < block->parameter_count;
             ++parameter_index) {
            const Om_Mir_Value *parameter = &backend->project->values[
                block->parameter_start + parameter_index];
            status = store_value_root(backend, function->id, parameter->id,
                value_at(backend, parameter->id));
        }
    }
    return status;
}
