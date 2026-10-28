#include "backend_internal.h"

static Om_Status add_wrapper_failure_check(Om_Llvm_Backend_Context *backend,
                                          LLVMValueRef wrapper,
                                          LLVMValueRef failed,
                                          LLVMValueRef context_to_destroy);
static Om_Status add_entry_thunk(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *entry,
                                 size_t entry_index,
                                 LLVMValueRef *thunk_out);

LLVMValueRef value_at(const Om_Llvm_Backend_Context *backend,
                             Om_Mir_Value_Id value_id)
{
    if (value_id.value >= backend->project->value_count) {
        return NULL;
    }
    return backend->values[value_id.value];
}

LLVMValueRef entry_alloca(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 LLVMTypeRef type,
                                 const char *name)
{
    const Om_Mir_Function *target_function = function != NULL ? function
        : backend == NULL ? NULL : backend->current_function;
    if (backend == NULL || target_function == NULL || type == NULL
        || name == NULL
        || target_function->id.value >= backend->project->function_count
        || backend->functions == NULL
        || backend->functions[target_function->id.value] == NULL) {
        return NULL;
    }
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(
        backend->functions[target_function->id.value]);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(backend->context);
    if (entry == NULL || builder == NULL) {
        if (builder != NULL) {
            LLVMDisposeBuilder(builder);
        }
        return NULL;
    }
    LLVMValueRef first_instruction = LLVMGetFirstInstruction(entry);
    if (first_instruction == NULL) {
        LLVMPositionBuilderAtEnd(builder, entry);
    } else {
        LLVMPositionBuilderBefore(builder, first_instruction);
    }
    LLVMValueRef allocation = LLVMBuildAlloca(builder, type, name);
    LLVMDisposeBuilder(builder);
    return allocation;
}

LLVMValueRef function_for_instance(const Om_Llvm_Backend_Context *backend,
                                          Om_Function_Instance_Id instance_id)
{
    for (size_t index = 0; index < backend->project->function_count; ++index) {
        if (backend->project->functions[index].instance_id.value
                == instance_id.value) {
            return backend->functions[index];
        }
    }
    return NULL;
}

Om_Status create_function_declarations(Om_Llvm_Backend_Context *backend)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < backend->project->function_count; ++index) {
        const Om_Mir_Function *function = &backend->project->functions[index];
        LLVMTypeRef signature = NULL;
        status = language_function_type(backend, function->signature_type_id,
                                        0, &signature);
        if (status != OM_STATUS_OK) {
            break;
        }
        char name[48];
        const int length = snprintf(name, sizeof(name), "om_fn_%" PRIu32,
                                    function->instance_id.value);
        if (length < 0 || (size_t)length >= sizeof(name)) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        LLVMValueRef value = LLVMAddFunction(backend->module, name, signature);
        if (value == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMSetLinkage(value, LLVMInternalLinkage);
        backend->functions[index] = value;
    }
    return status;
}

LLVMValueRef create_string_constant(Om_Llvm_Backend_Context *backend,
                                           const Om_Mir_Instruction *instruction)
{
    if (instruction->string_start > backend->project->string_data_length
        || instruction->string_length
            > backend->project->string_data_length - instruction->string_start
        || instruction->string_length > UINT_MAX) {
        return NULL;
    }
    const char *bytes = instruction->string_length == 0
        ? "" : backend->project->string_data + instruction->string_start;
    LLVMTypeRef array_type = LLVMArrayType2(backend->i8_type,
        (uint64_t)instruction->string_length + 1);
    LLVMValueRef initializer = LLVMConstStringInContext(backend->context,
        bytes, (unsigned)instruction->string_length, 0);
    char name[48];
    const int length = snprintf(name, sizeof(name), ".om_str_%" PRIu32,
                                instruction->result_id.value);
    if (length < 0 || (size_t)length >= sizeof(name)) {
        return NULL;
    }
    LLVMValueRef global = LLVMAddGlobal(backend->module, array_type, name);
    if (global == NULL || initializer == NULL) {
        return NULL;
    }
    LLVMSetInitializer(global, initializer);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
    LLVMValueRef indices[2] = {
        LLVMConstInt(backend->i32_type, 0, 0),
        LLVMConstInt(backend->i32_type, 0, 0),
    };
    return LLVMConstInBoundsGEP2(array_type, global, indices, 2);
}

LLVMValueRef context_for_function(const Om_Llvm_Backend_Context *backend,
                                         const Om_Mir_Function *function)
{
    if (backend == NULL || function == NULL
        || function->id.value >= backend->project->function_count) {
        return NULL;
    }
    return LLVMGetParam(backend->functions[function->id.value], 0);
}

const Om_Mir_Function *mir_function_for_instance(
    const Om_Mir_Project *project,
    Om_Function_Instance_Id instance_id)
{
    for (size_t index = 0; index < project->function_count; ++index) {
        if (project->functions[index].instance_id.value
            == instance_id.value) {
            return &project->functions[index];
        }
    }
    return NULL;
}

Om_Status create_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                               LLVMValueRef value,
                                               LLVMTypeRef value_type,
                                               Om_Type_Kind kind,
                                               LLVMValueRef *storage)
{
    LLVMTypeRef bytes_type = LLVMArrayType2(backend->i8_type, sizeof(uint64_t));
    LLVMValueRef allocation = entry_alloca(backend, NULL, bytes_type,
                                            "runtime_value_storage");
    if (allocation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder, LLVMConstNull(bytes_type), allocation);
    LLVMValueRef indices[2] = {
        LLVMConstInt(backend->i32_type, 0, 0),
        LLVMConstInt(backend->i32_type, 0, 0),
    };
    LLVMValueRef first_byte = LLVMBuildInBoundsGEP2(backend->builder,
        bytes_type, allocation, indices, 2, "runtime_value_bytes");
    LLVMTypeRef value_pointer_type = LLVMPointerType(value_type, 0);
    LLVMValueRef typed_address = LLVMBuildPointerCast(backend->builder,
        first_byte, value_pointer_type, "runtime_value_address");
    LLVMValueRef stored_value = value;
    if (kind == OM_TYPE_BOOL) {
        stored_value = LLVMBuildZExt(backend->builder, value,
            backend->i8_type, "runtime_bool_storage");
        typed_address = LLVMBuildPointerCast(backend->builder, first_byte,
            LLVMPointerType(backend->i8_type, 0), "runtime_bool_address");
    }
    if (typed_address == NULL || stored_value == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder, stored_value, typed_address);
    *storage = first_byte;
    return OM_STATUS_OK;
}

Om_Status allocate_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                                Om_Type_Id type_id,
                                                LLVMValueRef *storage)
{
    LLVMTypeRef value_type = NULL;
    Om_Status status = language_type(backend, type_id, 0, &value_type);
    if (status != OM_STATUS_OK) return status;
    LLVMValueRef allocation = entry_alloca(backend, NULL, value_type,
                                            "runtime_value_result");
    if (allocation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder, LLVMConstNull(value_type), allocation);
    *storage = LLVMBuildPointerCast(backend->builder, allocation,
        backend->byte_pointer_type, "runtime_value_result_bytes");
    return *storage == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

Om_Status load_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                            LLVMValueRef storage,
                                            Om_Type_Id type_id,
                                            LLVMValueRef *result)
{
    Om_Type_Kind kind = OM_TYPE_VARIABLE;
    LLVMTypeRef value_type = NULL;
    Om_Status status = type_kind(backend, type_id, &kind);
    if (status == OM_STATUS_OK) {
        status = language_type(backend, type_id, 0, &value_type);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (kind == OM_TYPE_UNIT || kind == OM_TYPE_NULL) {
        *result = LLVMConstNull(value_type);
        return OM_STATUS_OK;
    }
    LLVMTypeRef load_type = kind == OM_TYPE_BOOL
        ? backend->i8_type : value_type;
    LLVMValueRef address = LLVMBuildPointerCast(backend->builder, storage,
        LLVMPointerType(load_type, 0), "runtime_value_result_address");
    LLVMValueRef loaded = LLVMBuildLoad2(backend->builder, load_type, address,
                                         "runtime_value_result_loaded");
    if (kind == OM_TYPE_BOOL) {
        loaded = LLVMBuildTrunc(backend->builder, loaded, backend->i1_type,
                                "runtime_bool_result");
    }
    if (address == NULL || loaded == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *result = loaded;
    return OM_STATUS_OK;
}

Om_Status get_mir_value_storage(Om_Llvm_Backend_Context *backend,
                                       Om_Mir_Value_Id value_id,
                                       LLVMValueRef *storage,
                                       Om_Type_Kind *kind_out)
{
    if (value_id.value >= backend->project->value_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Mir_Value *value = &backend->project->values[value_id.value];
    Om_Type_Kind kind = OM_TYPE_VARIABLE;
    LLVMTypeRef type = NULL;
    Om_Status status = type_kind(backend, value->type_id, &kind);
    if (status == OM_STATUS_OK) {
        status = language_type(backend, value->type_id, 0, &type);
    }
    if (status == OM_STATUS_OK) {
        status = create_runtime_value_storage(backend,
            value_at(backend, value_id), type, kind, storage);
    }
    if (status == OM_STATUS_OK && kind_out != NULL) {
        *kind_out = kind;
    }
    return status;
}

Om_Status emit_function_bodies(Om_Llvm_Backend_Context *backend)
{
    Om_Status status = create_block_parameters(backend);
    if (status == OM_STATUS_OK) {
        status = initialize_function_roots(backend);
    }
    for (size_t function_index = 0; status == OM_STATUS_OK
         && function_index < backend->project->function_count; ++function_index) {
        const Om_Mir_Function *function = &backend->project->functions[
            function_index];
        backend->current_function = function;
        for (size_t block_index = 0; status == OM_STATUS_OK
             && block_index < backend->project->block_count; ++block_index) {
            const Om_Mir_Block *block = &backend->project->blocks[block_index];
            if (block->function_id.value != function->id.value) {
                continue;
            }
            LLVMPositionBuilderAtEnd(backend->builder,
                                     backend->block_tails[block->id.value]);
            for (size_t offset = 0; status == OM_STATUS_OK
                 && offset < block->instruction_count; ++offset) {
                const Om_Mir_Instruction *instruction =
                    &backend->project->instructions[block->instruction_start
                                                    + offset];
                status = emit_instruction(backend, function, block,
                                          instruction);
                if (status == OM_STATUS_OK) {
                    status = store_value_root(backend, function->id,
                        instruction->result_id,
                        value_at(backend, instruction->result_id));
                }
            }
            if (status == OM_STATUS_OK) {
                status = emit_terminator(backend, function, block);
            }
        }
    }
    backend->current_function = NULL;
    for (size_t block_index = 0; status == OM_STATUS_OK
         && block_index < backend->project->block_count; ++block_index) {
        const Om_Mir_Block *source = &backend->project->blocks[block_index];
        for (size_t edge_offset = 0; status == OM_STATUS_OK
             && edge_offset < source->terminator.edge_count; ++edge_offset) {
            const Om_Mir_Edge *edge = &backend->project->edges[
                source->terminator.edge_start + edge_offset];
            if (edge->target_block_id.value >= backend->project->block_count) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            const Om_Mir_Block *target = &backend->project->blocks[
                edge->target_block_id.value];
            if (edge->argument_count != target->parameter_count) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < edge->argument_count; ++index) {
                const Om_Mir_Value *parameter = &backend->project->values[
                    target->parameter_start + index];
                const Om_Mir_Value_Id argument_id =
                    backend->project->edge_arguments[edge->argument_start + index];
                LLVMValueRef argument = value_at(backend, argument_id);
                LLVMValueRef phi = value_at(backend, parameter->id);
                if (argument == NULL || phi == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                LLVMBasicBlockRef incoming_block =
                    backend->block_tails[source->id.value];
                LLVMAddIncoming(phi, &argument, &incoming_block, 1);
            }
        }
    }
    return status;
}

static Om_Status add_wrapper_failure_check(Om_Llvm_Backend_Context *backend,
                                          LLVMValueRef wrapper,
                                          LLVMValueRef failed,
                                          LLVMValueRef context_to_destroy)
{
    if (backend->generated_block_count > UINT32_MAX - 2) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const uint32_t index = backend->generated_block_count++;
    char failure_name[48];
    char continuation_name[48];
    int written = snprintf(failure_name, sizeof(failure_name),
                           "entry_failure_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(failure_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    written = snprintf(continuation_name, sizeof(continuation_name),
                       "entry_continue_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(continuation_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(
        backend->context, wrapper, failure_name);
    LLVMBasicBlockRef continuation = LLVMAppendBasicBlockInContext(
        backend->context, wrapper, continuation_name);
    if (failure == NULL || continuation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildCondBr(backend->builder, failed, failure, continuation);
    LLVMPositionBuilderAtEnd(backend->builder, failure);
    if (context_to_destroy != NULL) {
        LLVMValueRef destroy_arguments[1] = {context_to_destroy};
        (void)LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_context_destroy),
            backend->runtime_context_destroy, destroy_arguments, 1, "");
    }
    LLVMBuildRet(backend->builder, LLVMConstInt(backend->i32_type, 70, 0));
    LLVMPositionBuilderAtEnd(backend->builder, continuation);
    return OM_STATUS_OK;
}

static Om_Status add_entry_thunk(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *entry,
                                 size_t entry_index,
                                 LLVMValueRef *thunk_out)
{
    if (backend == NULL || entry == NULL || thunk_out == NULL
        || entry_index >= backend->project->function_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *thunk_out = NULL;
    Om_Type_Id signature_id = OM_TYPE_ID_INVALID;
    Om_Type_View signature = {0};
    Om_Status status = resolved_type(backend->project->type_store,
        entry->signature_type_id, &signature_id, &signature);
    if (status != OM_STATUS_OK || signature.child_count != 1) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    LLVMTypeRef parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef thunk_type = LLVMFunctionType(backend->i32_type, parameters,
                                               1, 0);
    LLVMValueRef thunk = LLVMAddFunction(backend->module,
        ".om_entry_thunk", thunk_type);
    if (thunk == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetLinkage(thunk, LLVMInternalLinkage);
    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(backend->context,
        thunk, "entry");
    if (block == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMPositionBuilderAtEnd(backend->builder, block);
    LLVMValueRef arguments[1] = {LLVMGetParam(thunk, 0)};
    LLVMValueRef function = backend->functions[entry_index];
    if (backend->debug_builder != NULL
        && backend->debug_subprograms != NULL
        && backend->debug_subprograms[entry_index] != NULL) {
        /* Keep the source-level entry frame so optimized code retains a
           usable native debug line table for the first execution path. */
        status = add_native_enum_attribute(backend, function, NULL,
            (LLVMAttributeIndex)LLVMAttributeFunctionIndex, "noinline", 0);
        if (status != OM_STATUS_OK) return status;
    }
    LLVMTypeRef function_type = LLVMGlobalGetValueType(function);
    LLVMValueRef call = LLVMBuildCall2(backend->builder, function_type,
        function, arguments, 1, entry->kind == OM_MIR_FUNCTION
            ? "om_entry_result" : "");
    if (call == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef exit_code = LLVMConstInt(backend->i32_type, 0, 0);
    if (entry->kind == OM_MIR_FUNCTION) {
        Om_Type_Id return_type_id = OM_TYPE_ID_INVALID;
        Om_Type_Kind return_kind = OM_TYPE_VARIABLE;
        status = om_type_store_child_at(backend->project->type_store,
            signature_id, 0, &return_type_id);
        if (status == OM_STATUS_OK) {
            status = type_kind(backend, return_type_id, &return_kind);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (return_kind == OM_TYPE_INT) {
            exit_code = LLVMBuildTrunc(backend->builder, call,
                backend->i32_type, "entry_exit_code");
            if (exit_code == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    LLVMBuildRet(backend->builder, exit_code);
    *thunk_out = thunk;
    return OM_STATUS_OK;
}

Om_Status add_entry_wrapper(Om_Llvm_Backend_Context *backend,
                                   Om_Symbol_Id entry_symbol)
{
    bool uses_distribution = false;
    for (size_t index = 0; index < backend->project->instruction_count;
         ++index) {
        const Om_Mir_Instruction *instruction =
            &backend->project->instructions[index];
        uses_distribution = uses_distribution
            || (instruction->opcode == OM_MIR_RUNTIME_ACTOR_SPAWN
                && instruction->operand_count == 1);
    }
    const Om_Mir_Function *entry = NULL;
    size_t entry_index = 0;
    for (size_t index = 0; index < backend->project->function_count; ++index) {
        const Om_Mir_Function *candidate = &backend->project->functions[index];
        if (candidate->symbol_id.value != entry_symbol.value) {
            continue;
        }
        if (entry != NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        entry = candidate;
        entry_index = index;
    }
    if (entry == NULL || entry->kind == OM_MIR_RECEIVE
        || entry->kind == OM_MIR_ACTOR_STATE_INIT) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMValueRef entry_thunk = NULL;
    Om_Status status = add_entry_thunk(backend, entry, entry_index,
                                       &entry_thunk);
    if (status != OM_STATUS_OK) {
        return status;
    }
    LLVMTypeRef wrapper_type = LLVMFunctionType(backend->i32_type, NULL, 0, 0);
    LLVMValueRef wrapper = LLVMAddFunction(backend->module, "main", wrapper_type);
    if (wrapper == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(
        backend->context, wrapper, "entry");
    if (block == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMPositionBuilderAtEnd(backend->builder, block);
    LLVMValueRef context_arguments[1] = {
        LLVMConstInt(backend->i64_type, 0, 0),
    };
    LLVMValueRef execution_context = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_context_create),
        backend->runtime_context_create, context_arguments, 1,
        "execution_context");
    if (execution_context == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef context_failed = LLVMBuildICmp(backend->builder, LLVMIntEQ,
        execution_context, LLVMConstNull(backend->byte_pointer_type),
        "execution_context_failed");
    status = context_failed == NULL ? OM_STATUS_OUT_OF_MEMORY
        : add_wrapper_failure_check(backend, wrapper, context_failed, NULL);
    if (status != OM_STATUS_OK) {
        return status;
    }
    LLVMValueRef source_span_table = NULL;
    status = create_source_span_table(backend, &source_span_table);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (backend->project->actor_count != 0) {
        status = create_actor_program_descriptor(backend, source_span_table,
                                                  entry_thunk);
        if (status != OM_STATUS_OK) return status;
    }
    LLVMValueRef span_arguments[3] = {
        execution_context,
        source_span_table,
        LLVMConstInt(backend->i64_type,
            backend->source_location_count, 0),
    };
    LLVMValueRef span_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_set_source_spans),
        backend->runtime_set_source_spans, span_arguments, 3,
        "source_span_setup_status");
    if (span_status == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef span_setup_failed = LLVMBuildICmp(backend->builder,
        LLVMIntNE, span_status, LLVMConstInt(backend->i32_type, 0, 0),
        "source_span_setup_failed");
    if (span_setup_failed == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    status = add_wrapper_failure_check(backend, wrapper, span_setup_failed,
                                       execution_context);
    if (status != OM_STATUS_OK) {
        return status;
    }
    LLVMValueRef distribution_state_slot = NULL;
    if (backend->project->actor_count != 0) {
        LLVMValueRef actor_start_arguments[3] = {
            execution_context,
            LLVMConstBitCast(backend->program_descriptor,
                backend->byte_pointer_type),
            LLVMConstInt(backend->i64_type, 0, 0),
        };
        LLVMValueRef actor_start_status = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_actor_system_start),
            backend->runtime_actor_system_start, actor_start_arguments, 3,
            "actor_system_start_status");
        LLVMValueRef actor_start_failed = actor_start_status == NULL ? NULL
            : LLVMBuildICmp(backend->builder, LLVMIntNE,
                actor_start_status, LLVMConstInt(backend->i32_type, 0, 0),
                "actor_system_start_failed");
        if (actor_start_failed == NULL) return OM_STATUS_OUT_OF_MEMORY;
        status = add_wrapper_failure_check(backend, wrapper,
            actor_start_failed, execution_context);
        if (status != OM_STATUS_OK) return status;

        if (uses_distribution) {
            distribution_state_slot = LLVMBuildAlloca(backend->builder,
                backend->byte_pointer_type, "distribution_state_slot");
            if (distribution_state_slot == NULL
                || LLVMBuildStore(backend->builder,
                    LLVMConstNull(backend->byte_pointer_type),
                    distribution_state_slot) == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            LLVMValueRef distribution_slot_argument = LLVMBuildBitCast(
                backend->builder, distribution_state_slot,
                backend->byte_pointer_type, "distribution_state_pointer");
            LLVMValueRef distribution_create_arguments[3] = {
                execution_context,
                LLVMConstBitCast(backend->program_descriptor,
                    backend->byte_pointer_type),
                distribution_slot_argument,
            };
            LLVMValueRef distribution_create_status = LLVMBuildCall2(
                backend->builder,
                LLVMGlobalGetValueType(backend->runtime_distribution_create),
                backend->runtime_distribution_create,
                distribution_create_arguments, 3,
                "distribution_create_status");
            LLVMValueRef distribution_create_failed =
                distribution_create_status == NULL ? NULL
                : LLVMBuildICmp(backend->builder, LLVMIntNE,
                    distribution_create_status,
                    LLVMConstInt(backend->i32_type, 0, 0),
                    "distribution_create_failed");
            if (distribution_create_failed == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            status = add_wrapper_failure_check(backend, wrapper,
                distribution_create_failed, execution_context);
            if (status != OM_STATUS_OK) return status;
        }
    }
    LLVMValueRef run_arguments[2] = {execution_context, entry_thunk};
    LLVMValueRef exit_code = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_run),
        backend->runtime_run, run_arguments, 2, "program_exit_code");
    if (exit_code == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (backend->project->actor_count != 0) {
        LLVMValueRef actor_wait_arguments[1] = {execution_context};
        LLVMValueRef actor_wait_status = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_actor_system_wait),
            backend->runtime_actor_system_wait, actor_wait_arguments, 1,
            "actor_system_wait_status");
        LLVMValueRef actor_wait_failed = actor_wait_status == NULL ? NULL
            : LLVMBuildICmp(backend->builder, LLVMIntNE,
                actor_wait_status, LLVMConstInt(backend->i32_type, 0, 0),
                "actor_system_wait_failed");
        if (actor_wait_failed == NULL) return OM_STATUS_OUT_OF_MEMORY;
        status = add_wrapper_failure_check(backend, wrapper,
            actor_wait_failed, execution_context);
        if (status != OM_STATUS_OK) return status;
        if (uses_distribution) {
            LLVMValueRef distribution_slot_argument = LLVMBuildBitCast(
                backend->builder, distribution_state_slot,
                backend->byte_pointer_type, "distribution_state_pointer");
            LLVMValueRef distribution_destroy_arguments[1] = {
                distribution_slot_argument,
            };
            if (LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(backend->runtime_distribution_destroy),
                    backend->runtime_distribution_destroy,
                    distribution_destroy_arguments, 1, "") == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    LLVMValueRef destroy_arguments[1] = {execution_context};
    (void)LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_context_destroy),
        backend->runtime_context_destroy, destroy_arguments, 1, "");
    LLVMBuildRet(backend->builder, exit_code);
    return OM_STATUS_OK;
}
