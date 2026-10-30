#include "backend_internal.h"

static Om_Status emit_checked_integer(Om_Llvm_Backend_Context *backend,
                                      Om_Mir_Block_Id block_id,
                                      const Om_Mir_Instruction *instruction,
                                      LLVMValueRef left,
                                      LLVMValueRef right,
                                      const char *intrinsic_name,
                                      LLVMValueRef *result);
static Om_Status emit_integer_division(Om_Llvm_Backend_Context *backend,
                                       Om_Mir_Block_Id block_id,
                                       const Om_Mir_Instruction *instruction,
                                       LLVMValueRef left,
                                       LLVMValueRef right,
                                       bool remainder,
                                       LLVMValueRef *result);
static LLVMIntPredicate integer_predicate(Om_Hir_Operator operator_kind);
static LLVMRealPredicate real_predicate(Om_Hir_Operator operator_kind);

Om_Status create_basic_blocks(Om_Llvm_Backend_Context *backend)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < backend->project->block_count; ++index) {
        const Om_Mir_Block *block = &backend->project->blocks[index];
        if (block->function_id.value >= backend->project->function_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Mir_Function *function = &backend->project->functions[
            block->function_id.value];
        char name[48];
        const int length = snprintf(name, sizeof(name), "bb_%" PRIu32,
                                    block->id.value);
        if (length < 0 || (size_t)length >= sizeof(name)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        LLVMBasicBlockRef llvm_block = LLVMAppendBasicBlockInContext(
            backend->context, backend->functions[function->id.value], name);
        if (llvm_block == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        backend->blocks[block->id.value] = llvm_block;
        backend->block_tails[block->id.value] = llvm_block;
    }
    return status;
}

Om_Status create_block_parameters(Om_Llvm_Backend_Context *backend)
{
    Om_Status status = OM_STATUS_OK;
    for (size_t function_index = 0; status == OM_STATUS_OK
         && function_index < backend->project->function_count; ++function_index) {
        const Om_Mir_Function *function = &backend->project->functions[
            function_index];
        if (function->entry_block_id.value >= backend->project->block_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Mir_Block *entry = &backend->project->blocks[
            function->entry_block_id.value];
        LLVMValueRef llvm_function = backend->functions[function_index];
        if (entry->parameter_count + 1
            != (size_t)LLVMCountParams(llvm_function)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK && parameter_index < entry->parameter_count;
             ++parameter_index) {
            const Om_Mir_Value *parameter = &backend->project->values[
                entry->parameter_start + parameter_index];
            LLVMValueRef value = LLVMGetParam(llvm_function,
                                               (unsigned)parameter_index + 1);
            if (parameter->id.value >= backend->project->value_count
                || value == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            backend->values[parameter->id.value] = value;
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
        for (size_t index = 0; status == OM_STATUS_OK
             && index < block->parameter_count; ++index) {
            const Om_Mir_Value *parameter = &backend->project->values[
                block->parameter_start + index];
            LLVMTypeRef type = NULL;
            status = language_type(backend, parameter->type_id, 0, &type);
            if (status != OM_STATUS_OK) {
                break;
            }
            char name[48];
            const int length = snprintf(name, sizeof(name), "v_%" PRIu32,
                                        parameter->id.value);
            if (length < 0 || (size_t)length >= sizeof(name)) {
                status = OM_STATUS_OUT_OF_RANGE;
                break;
            }
            LLVMValueRef phi = LLVMBuildPhi(backend->builder, type, name);
            if (phi == NULL || parameter->id.value >= backend->project->value_count) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            backend->values[parameter->id.value] = phi;
        }
    }
    return status;
}

Om_Status add_checked_continuation(Om_Llvm_Backend_Context *backend,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef failure_condition,
                                          uint32_t fault_code,
                                          uint32_t source_span_id)
{
    if (block_id.value >= backend->project->block_count
        || backend->generated_block_count > UINT32_MAX - 2) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Mir_Block *block = &backend->project->blocks[block_id.value];
    const Om_Mir_Function *function = &backend->project->functions[
        block->function_id.value];
    char failure_name[48];
    char continuation_name[48];
    const uint32_t index = backend->generated_block_count++;
    int written = snprintf(failure_name, sizeof(failure_name),
                           "checked_fault_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(failure_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    written = snprintf(continuation_name, sizeof(continuation_name),
                       "checked_continue_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(continuation_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMValueRef llvm_function = backend->functions[function->id.value];
    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(
        backend->context, llvm_function, failure_name);
    LLVMBasicBlockRef continuation = LLVMAppendBasicBlockInContext(
        backend->context, llvm_function, continuation_name);
    if (failure == NULL || continuation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildCondBr(backend->builder, failure_condition, failure,
                    continuation);
    LLVMPositionBuilderAtEnd(backend->builder, failure);
    LLVMValueRef fault_arguments[3] = {
        context_for_function(backend, function),
        LLVMConstInt(backend->i32_type, fault_code, 0),
        LLVMConstInt(backend->i32_type, source_span_id, 0),
    };
    LLVMValueRef fault_call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_fault),
        backend->runtime_fault, fault_arguments, 3, "");
    if (fault_call == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildUnreachable(backend->builder);
    LLVMPositionBuilderAtEnd(backend->builder, continuation);
    backend->block_tails[block_id.value] = continuation;
    return OM_STATUS_OK;
}

static Om_Status emit_checked_integer(Om_Llvm_Backend_Context *backend,
                                      Om_Mir_Block_Id block_id,
                                      const Om_Mir_Instruction *instruction,
                                      LLVMValueRef left,
                                      LLVMValueRef right,
                                      const char *intrinsic_name,
                                      LLVMValueRef *result)
{
    const unsigned intrinsic_id = LLVMLookupIntrinsicID(intrinsic_name,
        strlen(intrinsic_name));
    if (intrinsic_id == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMTypeRef parameter_types[1] = {backend->i64_type};
    LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(backend->module,
        intrinsic_id, parameter_types, 1);
    if (intrinsic == NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMValueRef arguments[2] = {left, right};
    LLVMValueRef pair = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(intrinsic), intrinsic, arguments,
        2, "checked_pair");
    if (pair == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef value = LLVMBuildExtractValue(backend->builder, pair, 0,
                                               "checked_value");
    LLVMValueRef overflow = LLVMBuildExtractValue(backend->builder, pair, 1,
                                                  "checked_overflow");
    if (value == NULL || overflow == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = instruction == NULL
        ? UINT32_MAX : instruction->id.value;
    Om_Status status = add_checked_continuation(backend, block_id, overflow,
        OM_LLVM_RUNTIME_FAULT_INTEGER_OVERFLOW, span_id);
    if (status == OM_STATUS_OK) {
        *result = value;
    }
    return status;
}

static Om_Status emit_integer_division(Om_Llvm_Backend_Context *backend,
                                       Om_Mir_Block_Id block_id,
                                       const Om_Mir_Instruction *instruction,
                                       LLVMValueRef left,
                                       LLVMValueRef right,
                                       bool remainder,
                                       LLVMValueRef *result)
{
    LLVMValueRef zero = LLVMConstInt(backend->i64_type, 0, 0);
    LLVMValueRef minus_one = LLVMConstInt(backend->i64_type, UINT64_MAX, 1);
    LLVMValueRef minimum = LLVMConstInt(backend->i64_type,
                                        UINT64_C(1) << 63, 0);
    LLVMValueRef divisor_is_zero = LLVMBuildICmp(backend->builder,
        LLVMIntEQ, right, zero, "divisor_is_zero");
    LLVMValueRef lhs_is_min = LLVMBuildICmp(backend->builder,
        LLVMIntEQ, left, minimum, "dividend_is_min");
    LLVMValueRef rhs_is_negative_one = LLVMBuildICmp(backend->builder,
        LLVMIntEQ, right, minus_one, "divisor_is_negative_one");
    LLVMValueRef signed_overflow = LLVMBuildAnd(backend->builder, lhs_is_min,
        rhs_is_negative_one, "division_overflow");
    if (divisor_is_zero == NULL || lhs_is_min == NULL
        || rhs_is_negative_one == NULL || signed_overflow == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = instruction == NULL
        ? UINT32_MAX : instruction->id.value;
    Om_Status status = add_checked_continuation(backend, block_id,
        divisor_is_zero, OM_LLVM_RUNTIME_FAULT_DIVISION_BY_ZERO, span_id);
    if (status == OM_STATUS_OK) {
        status = add_checked_continuation(backend, block_id, signed_overflow,
            OM_LLVM_RUNTIME_FAULT_INTEGER_OVERFLOW, span_id);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    *result = remainder
        ? LLVMBuildSRem(backend->builder, left, right, "checked_rem")
        : LLVMBuildSDiv(backend->builder, left, right, "checked_div");
    return *result == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static LLVMIntPredicate integer_predicate(Om_Hir_Operator operator_kind)
{
    switch (operator_kind) {
    case OM_HIR_OPERATOR_EQUAL: return LLVMIntEQ;
    case OM_HIR_OPERATOR_NOT_EQUAL: return LLVMIntNE;
    case OM_HIR_OPERATOR_LESS: return LLVMIntSLT;
    case OM_HIR_OPERATOR_GREATER: return LLVMIntSGT;
    case OM_HIR_OPERATOR_LESS_EQUAL: return LLVMIntSLE;
    case OM_HIR_OPERATOR_GREATER_EQUAL: return LLVMIntSGE;
    default: return LLVMIntEQ;
    }
}

static LLVMRealPredicate real_predicate(Om_Hir_Operator operator_kind)
{
    switch (operator_kind) {
    case OM_HIR_OPERATOR_EQUAL: return LLVMRealOEQ;
    case OM_HIR_OPERATOR_NOT_EQUAL: return LLVMRealUNE;
    case OM_HIR_OPERATOR_LESS: return LLVMRealOLT;
    case OM_HIR_OPERATOR_GREATER: return LLVMRealOGT;
    case OM_HIR_OPERATOR_LESS_EQUAL: return LLVMRealOLE;
    case OM_HIR_OPERATOR_GREATER_EQUAL: return LLVMRealOGE;
    default: return LLVMRealOEQ;
    }
}

Om_Status emit_instruction(Om_Llvm_Backend_Context *backend,
                                  const Om_Mir_Function *function,
                                  const Om_Mir_Block *block,
                                  const Om_Mir_Instruction *instruction)
{
    const Om_Mir_Value_Id *operands = instruction->operand_count == 0 ? NULL
        : backend->project->operands + instruction->operand_start;
    LLVMValueRef left = instruction->operand_count > 0
        ? value_at(backend, operands[0]) : NULL;
    LLVMValueRef right = instruction->operand_count > 1
        ? value_at(backend, operands[1]) : NULL;
    if ((instruction->operand_count > 0 && left == NULL)
        || (instruction->operand_count > 1 && right == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef result = NULL;
    Om_Status status = OM_STATUS_OK;
    const Om_Mir_Block_Id block_id = block->id;
    backend->current_instruction = instruction;
    set_instruction_debug_location(backend, function, instruction);
    bool actor_instruction_handled = false;
    status = emit_actor_instruction(backend, function, block_id, instruction,
        operands, &result, &actor_instruction_handled);
    if (status == OM_STATUS_OK && !actor_instruction_handled) {
    switch (instruction->opcode) {
    case OM_MIR_CONST_INT:
        result = LLVMConstInt(backend->i64_type, instruction->integer_value, 1);
        break;
    case OM_MIR_CONST_FLOAT:
        result = LLVMConstReal(backend->double_type, instruction->float_value);
        break;
    case OM_MIR_CONST_BOOL:
        result = LLVMConstInt(backend->i1_type,
                              instruction->integer_value != 0 ? 1 : 0, 0);
        break;
    case OM_MIR_CONST_NULL:
        result = LLVMConstNull(backend->byte_pointer_type);
        break;
    case OM_MIR_RUNTIME_STRING_CREATE:
    {
        LLVMValueRef bytes = create_string_constant(backend, instruction);
        LLVMValueRef arguments[4] = {
            NULL,
            bytes,
            LLVMConstInt(backend->i64_type, instruction->string_length, 0),
            NULL,
        };
        if (bytes == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        status = emit_runtime_object_call(backend, function, block_id,
            backend->runtime_string_create, arguments, 4, &result);
        break;
    }
    case OM_MIR_RUNTIME_STRING_FORMAT: {
        if (instruction->operand_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Mir_Value *operand_value = &backend->project->values[
            operands[0].value];
        Om_Type_Kind kind = OM_TYPE_VARIABLE;
        uint32_t runtime_kind = 0;
        LLVMTypeRef value_type = NULL;
        status = type_kind(backend, operand_value->type_id, &kind);
        if (status == OM_STATUS_OK) {
            status = runtime_value_kind(kind, &runtime_kind);
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, operand_value->type_id, 0,
                                   &value_type);
        }
        LLVMValueRef value_storage = NULL;
        if (status == OM_STATUS_OK) {
            status = create_runtime_value_storage(backend, left, value_type,
                                                   kind, &value_storage);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[4] = {
                NULL,
                LLVMConstInt(backend->i32_type, runtime_kind, 0),
                value_storage,
                NULL,
            };
            status = emit_runtime_object_call(backend, function, block_id,
                backend->runtime_string_format, arguments, 4, &result);
        }
        break;
    }
    case OM_MIR_RUNTIME_STRING_CONCAT: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef result_slot = entry_alloca(backend, function,
            backend->byte_pointer_type, "runtime_concat_result_slot");
        LLVMValueRef context = context_for_function(backend, function);
        if (result_slot == NULL || context == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMBuildStore(backend->builder,
            LLVMConstNull(backend->byte_pointer_type), result_slot);
        LLVMValueRef arguments[4] = {context, left, right, result_slot};
        LLVMValueRef call = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_string_concat),
            backend->runtime_string_concat, arguments, 4,
            "runtime_concat_status");
        status = emit_runtime_status_check(backend, block_id, call);
        if (status == OM_STATUS_OK) {
            result = LLVMBuildLoad2(backend->builder,
                backend->byte_pointer_type, result_slot,
                "runtime_concat_result");
        }
        break;
    }
    case OM_MIR_RUNTIME_PRINT:
    {
        LLVMValueRef arguments[2] = {
            context_for_function(backend, function), left,
        };
        LLVMValueRef call = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_print),
            backend->runtime_print, arguments, 2, "runtime_print_status");
        status = emit_runtime_status_check(backend, block_id, call);
        result = LLVMConstNull(backend->i8_type);
        break;
    }
    case OM_MIR_ALLOC_OBJECT: {
        Om_Type_Id object_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View object_type = {0};
        const Om_Mir_Nominal_Descriptor *nominal = NULL;
        LLVMValueRef type_descriptor = NULL;
        status = resolved_type(backend->project->type_store,
            backend->project->values[instruction->result_id.value].type_id,
            &object_type_id, &object_type);
        if (status == OM_STATUS_OK) {
            nominal = nominal_for_type(backend->project, &object_type);
            status = nominal == NULL
                ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, object_type_id, 0,
                                       &type_descriptor);
        }
        if (status == OM_STATUS_OK
            && instruction->operand_count != nominal->field_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[3] = {NULL, type_descriptor, NULL};
            status = emit_runtime_object_call(backend, function, block_id,
                backend->runtime_record_create, arguments, 3, &result);
        }
        if (status == OM_STATUS_OK) {
            status = store_value_root(backend, function->id,
                instruction->result_id, result);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < instruction->operand_count; ++index) {
            LLVMValueRef storage = NULL;
            status = get_mir_value_storage(backend, operands[index], &storage,
                                            NULL);
            if (status == OM_STATUS_OK) {
                LLVMValueRef arguments[4] = {
                    context_for_function(backend, function),
                    result,
                    LLVMConstInt(backend->i64_type, index, 0),
                    storage,
                };
                status = emit_runtime_status_call(backend, block_id,
                    backend->runtime_record_set, arguments, 4);
            }
        }
        break;
    }
    case OM_MIR_LOAD_FIELD: {
        if (instruction->operand_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Mir_Value *base_value = &backend->project->values[
            operands[0].value];
        Om_Type_Id base_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View base_type = {0};
        status = resolved_type(backend->project->type_store,
            base_value->type_id, &base_type_id, &base_type);
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_type(backend->project, &base_type) : NULL;
        size_t field_index = SIZE_MAX;
        if (nominal != NULL) {
            for (size_t index = 0; index < nominal->field_count; ++index) {
                if (backend->project->fields[nominal->field_start + index]
                        .symbol_id.value == instruction->symbol_id.value) {
                    field_index = index;
                    break;
                }
            }
        }
        if (status == OM_STATUS_OK
            && (nominal == NULL || field_index == SIZE_MAX)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef storage = NULL;
        if (status == OM_STATUS_OK) {
            status = allocate_runtime_value_storage(backend,
                backend->project->values[instruction->result_id.value].type_id,
                &storage);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[4] = {
                context_for_function(backend, function), left,
                LLVMConstInt(backend->i64_type, field_index, 0), storage,
            };
            status = emit_runtime_status_call(backend, block_id,
                backend->runtime_record_get, arguments, 4);
        }
        if (status == OM_STATUS_OK) {
            status = load_runtime_value_storage(backend, storage,
                backend->project->values[instruction->result_id.value].type_id,
                &result);
        }
        break;
    }
    case OM_MIR_STORE_INIT_FIELD: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Mir_Value *base_value = &backend->project->values[
            operands[0].value];
        Om_Type_Id base_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View base_type = {0};
        status = resolved_type(backend->project->type_store,
            base_value->type_id, &base_type_id, &base_type);
        const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
            ? nominal_for_type(backend->project, &base_type) : NULL;
        size_t field_index = SIZE_MAX;
        if (nominal != NULL) {
            for (size_t index = 0; index < nominal->field_count; ++index) {
                if (backend->project->fields[nominal->field_start + index]
                        .symbol_id.value == instruction->symbol_id.value) {
                    field_index = index;
                    break;
                }
            }
        }
        LLVMValueRef storage = NULL;
        if (status == OM_STATUS_OK
            && (nominal == NULL || field_index == SIZE_MAX)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = get_mir_value_storage(backend, operands[1], &storage, NULL);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[4] = {
                context_for_function(backend, function), left,
                LLVMConstInt(backend->i64_type, field_index, 0), storage,
            };
            status = emit_runtime_status_call(backend, block_id,
                backend->runtime_record_set, arguments, 4);
        }
        if (status == OM_STATUS_OK) {
            result = LLVMConstNull(backend->i8_type);
        }
        break;
    }
    case OM_MIR_COPY_MANAGED: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        Om_Type_Id record_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View record_type = {0};
        const Om_Mir_Nominal_Descriptor *nominal = NULL;
        LLVMValueRef type_descriptor = NULL;
        status = resolved_type(backend->project->type_store,
            backend->project->values[instruction->result_id.value].type_id,
            &record_type_id, &record_type);
        if (status == OM_STATUS_OK) {
            nominal = nominal_for_type(backend->project, &record_type);
            status = nominal == NULL
                ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
        }
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, record_type_id, 0,
                                       &type_descriptor);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[3] = {NULL, type_descriptor, NULL};
            status = emit_runtime_object_call(backend, function, block_id,
                backend->runtime_record_create, arguments, 3, &result);
        }
        if (status == OM_STATUS_OK) {
            status = store_value_root(backend, function->id,
                instruction->result_id, result);
        }
        size_t update_index = SIZE_MAX;
        for (size_t index = 0; nominal != NULL
             && index < nominal->field_count; ++index) {
            if (backend->project->fields[nominal->field_start + index]
                    .symbol_id.value == instruction->symbol_id.value) {
                update_index = index;
                break;
            }
        }
        if (status == OM_STATUS_OK && update_index == SIZE_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < nominal->field_count; ++index) {
            LLVMValueRef storage = NULL;
            if (index == update_index) {
                status = get_mir_value_storage(backend, operands[1], &storage,
                                                NULL);
            } else {
                status = allocate_runtime_value_storage(backend,
                    backend->project->fields[
                        nominal->field_start + index].type_id, &storage);
                if (status == OM_STATUS_OK) {
                    LLVMValueRef get_arguments[4] = {
                        context_for_function(backend, function), left,
                        LLVMConstInt(backend->i64_type, index, 0), storage,
                    };
                    status = emit_runtime_status_call(backend, block_id,
                        backend->runtime_record_get, get_arguments, 4);
                }
            }
            if (status == OM_STATUS_OK) {
                LLVMValueRef set_arguments[4] = {
                    context_for_function(backend, function), result,
                    LLVMConstInt(backend->i64_type, index, 0), storage,
                };
                status = emit_runtime_status_call(backend, block_id,
                    backend->runtime_record_set, set_arguments, 4);
            }
        }
        break;
    }
    case OM_MIR_LIST_CREATE: {
        Om_Type_Id list_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View list_type = {0};
        LLVMValueRef type_descriptor = NULL;
        status = resolved_type(backend->project->type_store,
            backend->project->values[instruction->result_id.value].type_id,
            &list_type_id, &list_type);
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, list_type_id, 0,
                                       &type_descriptor);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[3] = {NULL, type_descriptor, NULL};
            status = emit_runtime_object_call(backend, function, block_id,
                backend->runtime_list_create, arguments, 3, &result);
        }
        if (status == OM_STATUS_OK) {
            status = store_value_root(backend, function->id,
                instruction->result_id, result);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < instruction->operand_count; ++index) {
            LLVMValueRef storage = NULL;
            status = get_mir_value_storage(backend, operands[index], &storage,
                                            NULL);
            if (status == OM_STATUS_OK) {
                LLVMValueRef arguments[3] = {
                    context_for_function(backend, function), result, storage,
                };
                status = emit_runtime_status_call(backend, block_id,
                    backend->runtime_list_append, arguments, 3);
            }
        }
        break;
    }
    case OM_MIR_LIST_CONCAT: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef arguments[4] = {NULL, left, right, NULL};
        status = emit_runtime_object_call(backend, function, block_id,
            backend->runtime_list_concat, arguments, 4, &result);
        if (status == OM_STATUS_OK) {
            status = store_value_root(backend, function->id,
                instruction->result_id, result);
        }
        break;
    }
    case OM_MIR_LIST_LENGTH:
    case OM_MIR_MAP_LENGTH: {
        if (instruction->operand_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef length_slot = entry_alloca(backend, function,
            backend->i64_type, "runtime_collection_length");
        if (length_slot == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMBuildStore(backend->builder,
            LLVMConstInt(backend->i64_type, 0, 0), length_slot);
        LLVMValueRef arguments[3] = {
            context_for_function(backend, function), left, length_slot,
        };
        LLVMValueRef callee = instruction->opcode == OM_MIR_LIST_LENGTH
            ? backend->runtime_list_length : backend->runtime_map_length;
        status = emit_runtime_status_call(backend, block_id, callee,
                                           arguments, 3);
        if (status == OM_STATUS_OK) {
            result = LLVMBuildLoad2(backend->builder, backend->i64_type,
                                    length_slot, "runtime_length_value");
        }
        break;
    }
    case OM_MIR_LIST_GET:
    case OM_MIR_MAP_KEY_AT:
    case OM_MIR_MAP_GET: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef storage = NULL;
        LLVMValueRef callee = NULL;
        LLVMValueRef arguments[4] = {0};
        if (instruction->opcode == OM_MIR_MAP_GET) {
            status = get_mir_value_storage(backend, operands[1], &storage,
                                            NULL);
            arguments[0] = context_for_function(backend, function);
            arguments[1] = left;
            arguments[2] = storage;
            if (status == OM_STATUS_OK) {
                status = allocate_runtime_value_storage(backend,
                    backend->project->values[
                        instruction->result_id.value].type_id,
                    &arguments[3]);
            }
            callee = backend->runtime_map_get;
        } else {
            status = allocate_runtime_value_storage(backend,
                backend->project->values[instruction->result_id.value].type_id,
                &storage);
            arguments[0] = context_for_function(backend, function);
            arguments[1] = left;
            arguments[2] = right;
            arguments[3] = storage;
            callee = instruction->opcode == OM_MIR_LIST_GET
                ? backend->runtime_list_get : backend->runtime_map_key_at;
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef call = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(callee), callee, arguments, 4,
                "collection_lookup_status");
            const uint32_t fault_code =
                instruction->opcode == OM_MIR_MAP_GET
                    ? OM_LLVM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND
                    : OM_LLVM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS;
            status = emit_runtime_status_check_as(backend, block_id, call,
                                                   fault_code);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef result_storage = instruction->opcode == OM_MIR_MAP_GET
                ? arguments[3] : storage;
            status = load_runtime_value_storage(backend, result_storage,
                backend->project->values[instruction->result_id.value].type_id,
                &result);
        }
        break;
    }
    case OM_MIR_MAP_CREATE: {
        Om_Type_Id map_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View map_type = {0};
        LLVMValueRef type_descriptor = NULL;
        status = resolved_type(backend->project->type_store,
            backend->project->values[instruction->result_id.value].type_id,
            &map_type_id, &map_type);
        if (status == OM_STATUS_OK) {
            status = runtime_descriptor(backend, map_type_id, 0,
                                       &type_descriptor);
        }
        if (status == OM_STATUS_OK) {
            LLVMValueRef arguments[3] = {NULL, type_descriptor, NULL};
            status = emit_runtime_object_call(backend, function, block_id,
                backend->runtime_map_create, arguments, 3, &result);
        }
        if (status == OM_STATUS_OK) {
            status = store_value_root(backend, function->id,
                instruction->result_id, result);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < instruction->operand_count; index += 2) {
            LLVMValueRef key_storage = NULL;
            LLVMValueRef value_storage = NULL;
            status = get_mir_value_storage(backend, operands[index],
                                            &key_storage, NULL);
            if (status == OM_STATUS_OK) {
                status = get_mir_value_storage(backend, operands[index + 1],
                                                &value_storage, NULL);
            }
            if (status == OM_STATUS_OK) {
                LLVMValueRef arguments[4] = {
                    context_for_function(backend, function), result,
                    key_storage, value_storage,
                };
                status = emit_runtime_status_call(backend, block_id,
                    backend->runtime_map_set, arguments, 4);
            }
        }
        break;
    }
    case OM_MIR_INT_ADD_CHECKED:
    case OM_MIR_INT_SUB_CHECKED:
    case OM_MIR_INT_MUL_CHECKED: {
        const char *intrinsic_name = instruction->opcode == OM_MIR_INT_ADD_CHECKED
            ? "llvm.sadd.with.overflow.i64"
            : instruction->opcode == OM_MIR_INT_SUB_CHECKED
                ? "llvm.ssub.with.overflow.i64"
                : "llvm.smul.with.overflow.i64";
        status = emit_checked_integer(backend, block_id, instruction,
            left, right, intrinsic_name, &result);
        break;
    }
    case OM_MIR_INT_NEGATE_CHECKED: {
        LLVMValueRef zero = LLVMConstInt(backend->i64_type, 0, 0);
        status = emit_checked_integer(backend, block_id, instruction,
            zero, left, "llvm.ssub.with.overflow.i64", &result);
        break;
    }
    case OM_MIR_INT_DIV_CHECKED:
    case OM_MIR_INT_REM_CHECKED:
        status = emit_integer_division(backend, block_id, instruction,
            left, right,
            instruction->opcode == OM_MIR_INT_REM_CHECKED, &result);
        break;
    case OM_MIR_FLOAT_ADD:
        result = LLVMBuildFAdd(backend->builder, left, right, "float_add");
        break;
    case OM_MIR_FLOAT_SUB:
        result = LLVMBuildFSub(backend->builder, left, right, "float_sub");
        break;
    case OM_MIR_FLOAT_MUL:
        result = LLVMBuildFMul(backend->builder, left, right, "float_mul");
        break;
    case OM_MIR_FLOAT_DIV:
        result = LLVMBuildFDiv(backend->builder, left, right, "float_div");
        break;
    case OM_MIR_FLOAT_NEGATE:
        result = LLVMBuildFNeg(backend->builder, left, "float_negate");
        break;
    case OM_MIR_BOOL_AND:
        result = LLVMBuildAnd(backend->builder, left, right, "bool_and");
        break;
    case OM_MIR_BOOL_OR:
        result = LLVMBuildOr(backend->builder, left, right, "bool_or");
        break;
    case OM_MIR_BOOL_NOT:
        result = LLVMBuildNot(backend->builder, left, "bool_not");
        break;
    case OM_MIR_COMPARE: {
        const Om_Mir_Value *left_value = &backend->project->values[
            operands[0].value];
        Om_Type_Kind operand_kind = OM_TYPE_VARIABLE;
        status = type_kind(backend, left_value->type_id, &operand_kind);
        if (status != OM_STATUS_OK) {
            break;
        }
        if (operand_kind == OM_TYPE_INT || operand_kind == OM_TYPE_BOOL
            || operand_kind == OM_TYPE_NULL
            || operand_kind == OM_TYPE_ACTOR_ID
            || operand_kind == OM_TYPE_DOWN_REASON
            || operand_kind == OM_TYPE_ACTOR_REF) {
            result = LLVMBuildICmp(backend->builder,
                integer_predicate(instruction->operator_kind), left, right,
                "compare");
        } else if (operand_kind == OM_TYPE_FLOAT) {
            result = LLVMBuildFCmp(backend->builder,
                real_predicate(instruction->operator_kind), left, right,
                "compare");
        } else if (operand_kind == OM_TYPE_STRING) {
            LLVMValueRef comparison_slot = entry_alloca(backend, function,
                backend->i32_type, "string_comparison_slot");
            LLVMValueRef context = context_for_function(backend, function);
            if (comparison_slot == NULL || context == NULL) {
                status = comparison_slot == NULL ? OM_STATUS_OUT_OF_MEMORY
                                                 : OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            LLVMValueRef arguments[4] = {
                context, left, right, comparison_slot,
            };
            LLVMValueRef compare_status = LLVMBuildCall2(
                backend->builder,
                LLVMGlobalGetValueType(backend->runtime_string_compare),
                backend->runtime_string_compare, arguments, 4,
                "string_compare_status");
            status = emit_runtime_status_check(backend, block_id,
                                               compare_status);
            if (status != OM_STATUS_OK) {
                break;
            }
            LLVMValueRef comparison = LLVMBuildLoad2(backend->builder,
                backend->i32_type, comparison_slot, "string_comparison");
            result = LLVMBuildICmp(backend->builder,
                integer_predicate(instruction->operator_kind), comparison,
                LLVMConstInt(backend->i32_type, 0, 0), "string_compare");
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        break;
    }
    case OM_MIR_CALL_DIRECT: {
        LLVMValueRef callee = function_for_instance(backend,
            instruction->callee_instance_id);
        if (callee == NULL || instruction->operand_count > UINT_MAX) {
            status = OM_STATUS_NOT_FOUND;
            break;
        }
        if (instruction->operand_count >= UINT_MAX) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        LLVMValueRef *arguments = om_alloc_array(
            instruction->operand_count + 1, sizeof(*arguments));
        if (arguments == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        arguments[0] = context_for_function(backend, function);
        for (size_t index = 0; index < instruction->operand_count; ++index) {
            arguments[index + 1] = value_at(backend, operands[index]);
        }
        const LLVMTypeRef callee_type = LLVMGlobalGetValueType(callee);
        const bool returns_void = LLVMGetTypeKind(
            LLVMGetReturnType(callee_type)) == LLVMVoidTypeKind;
        LLVMValueRef call = LLVMBuildCall2(backend->builder, callee_type,
            callee, arguments, (unsigned)instruction->operand_count + 1,
            returns_void ? "" : "direct_call");
        om_free(arguments);
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        result = LLVMGetTypeKind(LLVMGetReturnType(callee_type))
                == LLVMVoidTypeKind
            ? LLVMConstNull(backend->i8_type) : call;
        break;
    }
    case OM_MIR_FUNCTION_REF:
        result = function_for_instance(backend,
                                       instruction->callee_instance_id);
        if (result == NULL) {
            status = OM_STATUS_NOT_FOUND;
        }
        break;
    case OM_MIR_CALL_INDIRECT: {
        if (instruction->operand_count == 0
            || instruction->operand_count - 1 > UINT_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Mir_Value *callee_value = &backend->project->values[
            operands[0].value];
        LLVMTypeRef signature = NULL;
        status = language_function_type(backend, callee_value->type_id, 0,
                                        &signature);
        if (status != OM_STATUS_OK) {
            break;
        }
        const size_t source_argument_count = instruction->operand_count - 1;
        if (source_argument_count >= UINT_MAX) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        const size_t argument_count = source_argument_count + 1;
        LLVMValueRef *arguments = om_alloc_array(argument_count,
                                                  sizeof(*arguments));
        if (arguments == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        arguments[0] = context_for_function(backend, function);
        for (size_t index = 1; index < instruction->operand_count; ++index) {
            arguments[index] = value_at(backend, operands[index]);
        }
        const bool returns_void = LLVMGetTypeKind(
            LLVMGetReturnType(signature)) == LLVMVoidTypeKind;
        LLVMValueRef call = LLVMBuildCall2(backend->builder, signature, left,
            arguments, (unsigned)argument_count,
            returns_void ? "" : "indirect_call");
        om_free(arguments);
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        result = LLVMGetTypeKind(LLVMGetReturnType(signature))
                == LLVMVoidTypeKind
            ? LLVMConstNull(backend->i8_type) : call;
        break;
    }
    case OM_MIR_RESULT_MAKE_OK:
    case OM_MIR_RESULT_MAKE_ERROR: {
        if (instruction->operand_count != 1
            || instruction->result_id.value >= backend->project->value_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const bool is_ok = instruction->opcode == OM_MIR_RESULT_MAKE_OK;
        Om_Type_Id result_type_id = OM_TYPE_ID_INVALID;
        Om_Type_View result_type = {0};
        LLVMTypeRef aggregate_type = NULL;
        status = resolved_type(backend->project->type_store,
            backend->project->values[instruction->result_id.value].type_id,
            &result_type_id, &result_type);
        if (status == OM_STATUS_OK
            && (result_type.kind != OM_TYPE_RESULT
                || result_type.child_count != 2)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = language_type(backend, result_type_id, 0,
                                  &aggregate_type);
        }
        if (status == OM_STATUS_OK) {
            result = LLVMBuildInsertValue(backend->builder,
                LLVMConstNull(aggregate_type), left, is_ok ? 1 : 2,
                is_ok ? "result_ok_payload" : "result_error_payload");
        }
        if (status == OM_STATUS_OK && result != NULL) {
            result = LLVMBuildInsertValue(backend->builder, result,
                LLVMConstInt(backend->i1_type, is_ok ? 1 : 0, 0), 0,
                "result_tag");
        }
        break;
    }
    case OM_MIR_RESULT_TAG:
    case OM_MIR_RESULT_OK_PAYLOAD:
    case OM_MIR_RESULT_ERROR_PAYLOAD: {
        if (instruction->operand_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const unsigned member_index = instruction->opcode
                == OM_MIR_RESULT_TAG ? 0
            : instruction->opcode == OM_MIR_RESULT_OK_PAYLOAD ? 1 : 2;
        result = LLVMBuildExtractValue(backend->builder, left, member_index,
            instruction->opcode == OM_MIR_RESULT_TAG ? "result_tag"
                : instruction->opcode == OM_MIR_RESULT_OK_PAYLOAD
                    ? "result_ok_payload" : "result_error_payload");
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_SPAWN:
    case OM_MIR_RUNTIME_ACTOR_SELF:
    case OM_MIR_RUNTIME_ACTOR_SEND:
    case OM_MIR_RUNTIME_ACTOR_WATCH:
    case OM_MIR_RUNTIME_ACTOR_STOP:
    case OM_MIR_RUNTIME_SYSTEM_ACTOR:
    case OM_MIR_ACTOR_IDENTITY:
        status = OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_MIR_RUNTIME_FAULT: {
        uint32_t fault_code = OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED;
        if (instruction->integer_value
            == OM_MIR_RUNTIME_FAULT_ZERO_RANGE_STEP) {
            fault_code = OM_LLVM_RUNTIME_FAULT_ZERO_RANGE_STEP;
        } else if (instruction->integer_value
            == OM_MIR_RUNTIME_FAULT_ACTOR_FAILED) {
            fault_code = OM_LLVM_RUNTIME_FAULT_ACTOR_FAILED;
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef fault_arguments[3] = {
            context_for_function(backend, function),
            LLVMConstInt(backend->i32_type, fault_code, 0),
            LLVMConstInt(backend->i32_type, instruction->id.value, 0),
        };
        LLVMValueRef fault_call = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_fault),
            backend->runtime_fault, fault_arguments, 3, "");
        if (fault_call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            result = LLVMConstNull(backend->i8_type);
        }
        break;
    }
    case OM_MIR_NATIVE_CALL:
        status = emit_native_call(backend, function, block_id, instruction,
                                  operands, &result);
        break;
    case OM_MIR_NATIVE_CALLBACK_ADDRESS:
    case OM_MIR_CSTRING_CONVERT:
        status = OM_STATUS_INVALID_ARGUMENT;
        break;
    }
    }
    if (status == OM_STATUS_OK && result == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        if (instruction->result_id.value >= backend->project->value_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            backend->values[instruction->result_id.value] = result;
        }
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: LLVM lowering failed for %s in f%" PRIu32
            " (%s)\n", om_mir_opcode_name(instruction->opcode),
            function->id.value, om_status_name(status));
    }
    backend->current_instruction = NULL;
    return status;
}

Om_Status emit_terminator(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 const Om_Mir_Block *block)
{
    const Om_Mir_Terminator *terminator = &block->terminator;
    const Om_Mir_Edge *edges = terminator->edge_count == 0 ? NULL
        : backend->project->edges + terminator->edge_start;
    LLVMPositionBuilderAtEnd(backend->builder,
                             backend->block_tails[block->id.value]);
    LLVMSetCurrentDebugLocation2(backend->builder, NULL);
    switch (terminator->kind) {
    case OM_MIR_BR:
        if (edges == NULL || edges[0].target_block_id.value
                >= backend->project->block_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMBuildBr(backend->builder,
            backend->blocks[edges[0].target_block_id.value]);
        return OM_STATUS_OK;
    case OM_MIR_COND_BR:
        if (edges == NULL || edges[0].target_block_id.value
                >= backend->project->block_count
            || edges[1].target_block_id.value >= backend->project->block_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMBuildCondBr(backend->builder,
            value_at(backend, terminator->value_id),
            backend->blocks[edges[0].target_block_id.value],
            backend->blocks[edges[1].target_block_id.value]);
        return OM_STATUS_OK;
    case OM_MIR_SWITCH: {
        if (edges == NULL || terminator->edge_count == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Mir_Edge *default_edge = NULL;
        size_t case_count = 0;
        for (size_t index = 0; index < terminator->edge_count; ++index) {
            if (edges[index].is_default_case) {
                default_edge = &edges[index];
            } else {
                ++case_count;
            }
        }
        if (default_edge == NULL || default_edge->target_block_id.value
                >= backend->project->block_count
            || case_count > UINT_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef switch_value = value_at(backend, terminator->value_id);
        LLVMValueRef switch_inst = LLVMBuildSwitch(backend->builder,
            switch_value, backend->blocks[default_edge->target_block_id.value],
            (unsigned)case_count);
        if (switch_inst == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMTypeRef switch_type = LLVMTypeOf(switch_value);
        for (size_t index = 0; index < terminator->edge_count; ++index) {
            if (edges[index].is_default_case) {
                continue;
            }
            if (!edges[index].has_case_value
                || edges[index].target_block_id.value
                    >= backend->project->block_count) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            LLVMValueRef case_value = LLVMConstInt(switch_type,
                (uint64_t)edges[index].case_value, 1);
            LLVMAddCase(switch_inst, case_value,
                backend->blocks[edges[index].target_block_id.value]);
        }
        return OM_STATUS_OK;
    }
    case OM_MIR_RETURN:
        if (terminator->value_id.value != UINT32_MAX) {
            LLVMValueRef value = value_at(backend, terminator->value_id);
            if (value == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            LLVMValueRef arguments[2] = {
                context_for_function(backend, function),
                backend->root_frames[function->id.value],
            };
            LLVMValueRef call = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(backend->runtime_roots_pop),
                backend->runtime_roots_pop, arguments, 2,
                "managed_root_pop_status");
            Om_Status status = emit_runtime_status_check(backend, block->id,
                                                          call);
            if (status != OM_STATUS_OK) {
                return status;
            }
            LLVMBuildRet(backend->builder, value);
            return OM_STATUS_OK;
        }
        if (function->kind == OM_MIR_ACTION
            || function->kind == OM_MIR_RECEIVE) {
            LLVMValueRef arguments[2] = {
                context_for_function(backend, function),
                backend->root_frames[function->id.value],
            };
            LLVMValueRef call = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(backend->runtime_roots_pop),
                backend->runtime_roots_pop, arguments, 2,
                "managed_root_pop_status");
            Om_Status status = emit_runtime_status_check(backend, block->id,
                                                          call);
            if (status != OM_STATUS_OK) {
                return status;
            }
            LLVMBuildRetVoid(backend->builder);
            return OM_STATUS_OK;
        }
        {
            Om_Type_Id signature_id = OM_TYPE_ID_INVALID;
            Om_Type_View signature = {0};
            Om_Status status = resolved_type(backend->project->type_store,
                function->signature_type_id, &signature_id, &signature);
            if (status != OM_STATUS_OK || signature.child_count == 0) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            Om_Type_Id return_type_id = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(backend->project->type_store,
                signature_id, signature.child_count - 1, &return_type_id);
            LLVMTypeRef return_type = NULL;
            if (status == OM_STATUS_OK) {
                status = language_type(backend, return_type_id, 0, &return_type);
            }
            if (status == OM_STATUS_OK) {
                LLVMValueRef arguments[2] = {
                    context_for_function(backend, function),
                    backend->root_frames[function->id.value],
                };
                LLVMValueRef call = LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(backend->runtime_roots_pop),
                    backend->runtime_roots_pop, arguments, 2,
                    "managed_root_pop_status");
                status = emit_runtime_status_check(backend, block->id, call);
                if (status == OM_STATUS_OK) {
                    LLVMBuildRet(backend->builder,
                                 LLVMConstNull(return_type));
                }
            }
            return status;
        }
    case OM_MIR_UNREACHABLE:
        LLVMBuildUnreachable(backend->builder);
        return OM_STATUS_OK;
    case OM_MIR_TERMINATOR_NONE:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}
