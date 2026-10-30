#include "backend_internal.h"

Om_Status emit_actor_instruction(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 Om_Mir_Block_Id block_id,
                                 const Om_Mir_Instruction *instruction,
                                 const Om_Mir_Value_Id *operands,
                                 LLVMValueRef *result,
                                 bool *handled)
{
    if (backend == NULL || function == NULL || instruction == NULL
        || result == NULL || handled == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *handled = true;
    *result = NULL;
    switch (instruction->opcode) {
    case OM_MIR_ACTOR_IDENTITY:
        if (instruction->operand_count == 0) {
            *result = LLVMConstInt(backend->i64_type, 0, 0);
            return OM_STATUS_OK;
        }
        if (instruction->operand_count != 1 || operands == NULL
            || value_at(backend, operands[0]) == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *result = value_at(backend, operands[0]);
        return OM_STATUS_OK;
    case OM_MIR_RUNTIME_ACTOR_SPAWN: {
        if (instruction->operand_count > 1
            || backend->actor_descriptors == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        size_t actor_index = SIZE_MAX;
        for (size_t index = 0; index < backend->project->actor_count; ++index) {
            if (backend->project->actors[index].symbol_id.value
                == instruction->symbol_id.value) {
                actor_index = index;
                break;
            }
        }
        if (actor_index == SIZE_MAX
            || backend->actor_descriptors[actor_index] == NULL) {
            return OM_STATUS_NOT_FOUND;
        }
        LLVMValueRef actor_id_slot = entry_alloca(backend, function,
            backend->i64_type, "actor_id_slot");
        if (actor_id_slot == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMValueRef actor_descriptor = LLVMConstBitCast(
            backend->actor_descriptors[actor_index],
            backend->byte_pointer_type);
        if (instruction->operand_count == 0) {
            LLVMValueRef arguments[3] = {
                context_for_function(backend, function), actor_descriptor,
                actor_id_slot,
            };
            Om_Status status = emit_runtime_status_call(backend, block_id,
                backend->runtime_actor_spawn_id, arguments, 3);
            if (status != OM_STATUS_OK) {
                return status;
            }
        } else {
            LLVMValueRef initial_state = value_at(backend, operands[0]);
            if (initial_state == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            LLVMValueRef actor_reference_slot = LLVMBuildBitCast(
                backend->builder, actor_id_slot,
                LLVMPointerType(backend->runtime_actor_ref_type, 0),
                "actor_reference_slot");
            if (actor_reference_slot == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            LLVMValueRef arguments[4] = {
                context_for_function(backend, function), actor_descriptor,
                initial_state, actor_reference_slot,
            };
            Om_Status status = emit_runtime_status_call(backend, block_id,
                backend->runtime_actor_spawn_at, arguments, 4);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        *result = LLVMBuildLoad2(backend->builder, backend->i64_type,
            actor_id_slot, "spawned_actor_id");
        return *result == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
    }
    case OM_MIR_RUNTIME_ACTOR_SELF: {
        if (instruction->operand_count != 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef actor_id_slot = entry_alloca(backend, function,
            backend->i64_type, "self_actor_id_slot");
        if (actor_id_slot == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMValueRef arguments[2] = {
            context_for_function(backend, function), actor_id_slot,
        };
        Om_Status status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_self_id, arguments, 2);
        if (status != OM_STATUS_OK) {
            return status;
        }
        *result = LLVMBuildLoad2(backend->builder, backend->i64_type,
            actor_id_slot, "self_actor_id");
        return *result == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
    }
    case OM_MIR_RUNTIME_ACTOR_SEND: {
        if (instruction->operand_count != 2 || operands == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef arguments[3] = {
            context_for_function(backend, function),
            value_at(backend, operands[0]), value_at(backend, operands[1]),
        };
        if (arguments[1] == NULL || arguments[2] == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Status status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_send_id, arguments, 3);
        if (status == OM_STATUS_OK) {
            *result = LLVMConstNull(backend->i8_type);
        }
        return status;
    }
    case OM_MIR_RUNTIME_ACTOR_WATCH: {
        if (instruction->operand_count != 1 || operands == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef actor_id = value_at(backend, operands[0]);
        if (actor_id == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef arguments[2] = {
            context_for_function(backend, function), actor_id,
        };
        Om_Status status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_watch_id, arguments, 2);
        if (status == OM_STATUS_OK) {
            *result = LLVMConstNull(backend->i8_type);
        }
        return status;
    }
    case OM_MIR_RUNTIME_ACTOR_STOP: {
        if (instruction->operand_count != 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef arguments[1] = {
            context_for_function(backend, function),
        };
        Om_Status status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_stop, arguments, 1);
        if (status == OM_STATUS_OK) {
            *result = LLVMConstNull(backend->i8_type);
        }
        return status;
    }
    case OM_MIR_RUNTIME_SYSTEM_ACTOR:
        if (instruction->operand_count != 0
            || (instruction->integer_value != OM_SYSTEM_ACTOR_REF_CONSOLE
                && instruction->integer_value != OM_SYSTEM_ACTOR_REF_TIMER
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_FILE_SYSTEM
                && instruction->integer_value != OM_SYSTEM_ACTOR_REF_CLOCK
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_SECURE_RANDOM
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_LOGGER
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_NETWORK
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_PROCESS
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_ENVIRONMENT)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *result = LLVMConstInt(backend->i64_type,
                               instruction->integer_value, 0);
        return OM_STATUS_OK;
    default:
        *handled = false;
        return OM_STATUS_OK;
    }
}

Om_Status create_actor_state_initializer(
    Om_Llvm_Backend_Context *backend,
    size_t actor_index,
    LLVMValueRef *initializer_out)
{
    const Om_Mir_Actor_Descriptor *actor =
        &backend->project->actors[actor_index];
    *initializer_out = LLVMConstNull(backend->byte_pointer_type);
    if (actor->state_initializer_count == 0) return OM_STATUS_OK;
    if (actor->state_initializer_start >
            backend->project->actor_state_initializer_count
        || actor->state_initializer_count >
            backend->project->actor_state_initializer_count
                - actor->state_initializer_start) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id state_type_id = OM_TYPE_ID_INVALID;
    Om_Type_View state_type = {0};
    Om_Status status = resolved_type(backend->project->type_store,
        actor->state_type_id, &state_type_id, &state_type);
    const Om_Mir_Nominal_Descriptor *nominal = status == OM_STATUS_OK
        ? nominal_for_type(backend->project, &state_type) : NULL;
    if (status != OM_STATUS_OK || nominal == NULL
        || nominal->field_count != actor->state_initializer_count) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    char function_name[64];
    const int name_length = snprintf(function_name, sizeof(function_name),
        ".om_actor_%" PRIu32 "_initialize", actor->symbol_id.value);
    if (name_length < 0 || (size_t)name_length >= sizeof(function_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMTypeRef parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef function_type = LLVMFunctionType(backend->i32_type,
        parameters, 2, 0);
    LLVMValueRef function = LLVMAddFunction(backend->module, function_name,
                                             function_type);
    if (function == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMSetLinkage(function, LLVMInternalLinkage);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(
        backend->context, function, "entry");
    if (entry == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMPositionBuilderAtEnd(backend->builder, entry);
    LLVMValueRef context_value = LLVMGetParam(function, 0);
    LLVMValueRef state_payload = LLVMGetParam(function, 1);
    LLVMValueRef state_object_slot = LLVMBuildAlloca(backend->builder,
        backend->byte_pointer_type, "state_object_slot");
    if (state_object_slot == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMValueRef get_state_arguments[2] = {context_value, state_object_slot};
    LLVMValueRef get_state_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_actor_state_object),
        backend->runtime_actor_state_object, get_state_arguments, 2,
        "state_object_status");
    if (get_state_status == NULL) return OM_STATUS_OUT_OF_MEMORY;
    status = callback_status_check(backend, function, get_state_status);
    if (status != OM_STATUS_OK) return status;
    LLVMValueRef state_object = LLVMBuildLoad2(backend->builder,
        backend->byte_pointer_type, state_object_slot, "state_object");
    if (state_object == NULL) return OM_STATUS_OUT_OF_MEMORY;

    for (size_t initializer_index = 0;
         status == OM_STATUS_OK
         && initializer_index < actor->state_initializer_count;
         ++initializer_index) {
        const Om_Mir_Actor_State_Initializer *initializer =
            &backend->project->actor_state_initializers[
                actor->state_initializer_start + initializer_index];
        const Om_Mir_Function *mir_function = mir_function_for_instance(
            backend->project, initializer->function_instance_id);
        LLVMValueRef init_function = function_for_instance(backend,
            initializer->function_instance_id);
        size_t field_index = SIZE_MAX;
        for (size_t index = 0; index < nominal->field_count; ++index) {
            if (backend->project->fields[nominal->field_start + index]
                    .symbol_id.value == initializer->field_symbol_id.value) {
                field_index = index;
                break;
            }
        }
        if (mir_function == NULL || init_function == NULL
            || mir_function->kind != OM_MIR_ACTOR_STATE_INIT
            || field_index == SIZE_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        Om_Type_Id function_signature_id = OM_TYPE_ID_INVALID;
        Om_Type_View function_signature = {0};
        status = resolved_type(backend->project->type_store,
            mir_function->signature_type_id, &function_signature_id,
            &function_signature);
        Om_Type_Id result_type_id = OM_TYPE_ID_INVALID;
        Om_Type_Kind result_kind = OM_TYPE_VARIABLE;
        if (status == OM_STATUS_OK && function_signature.child_count < 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(backend->project->type_store,
                function_signature_id, function_signature.child_count - 1,
                &result_type_id);
        }
        if (status == OM_STATUS_OK) {
            status = type_kind(backend, result_type_id, &result_kind);
        }
        LLVMValueRef init_arguments[2] = {context_value, state_object};
        LLVMValueRef initialized_value = status == OM_STATUS_OK
            ? LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(init_function), init_function,
                init_arguments, 2, "initialized_state_value") : NULL;
        if (status == OM_STATUS_OK && initialized_value == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMValueRef union_storage = status == OM_STATUS_OK
            ? LLVMBuildAlloca(backend->builder, backend->i64_type,
                              "runtime_value_storage") : NULL;
        if (status == OM_STATUS_OK && union_storage == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMValueRef union_bits = initialized_value;
        if (status == OM_STATUS_OK) {
            if (result_kind == OM_TYPE_BOOL) {
                union_bits = LLVMBuildZExt(backend->builder, initialized_value,
                    backend->i64_type, "runtime_value_bool");
            } else if (result_kind == OM_TYPE_FLOAT) {
                union_bits = LLVMBuildBitCast(backend->builder,
                    initialized_value, backend->i64_type,
                    "runtime_value_float");
            } else if (result_kind == OM_TYPE_STRING
                       || result_kind == OM_TYPE_NULL
                       || result_kind == OM_TYPE_RECORD
                       || result_kind == OM_TYPE_MESSAGE
                       || result_kind == OM_TYPE_LIST
                       || result_kind == OM_TYPE_MAP) {
                union_bits = LLVMBuildPtrToInt(backend->builder,
                    initialized_value, backend->i64_type,
                    "runtime_value_pointer");
            } else if (result_kind != OM_TYPE_INT
                       && result_kind != OM_TYPE_ACTOR_ID
                       && result_kind != OM_TYPE_DOWN_REASON
                       && result_kind != OM_TYPE_ACTOR_REF
                       && result_kind != OM_TYPE_NODE_REF
                       && result_kind != OM_TYPE_UNIT) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        if (status == OM_STATUS_OK) {
            (void)LLVMBuildStore(backend->builder, union_bits, union_storage);
            LLVMValueRef value_storage = LLVMBuildBitCast(backend->builder,
                union_storage, backend->byte_pointer_type,
                "runtime_value_pointer");
            LLVMValueRef set_arguments[4] = {
                context_value,
                state_object,
                LLVMConstInt(backend->i64_type, field_index, 0),
                value_storage,
            };
            LLVMValueRef set_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(backend->runtime_record_set),
                backend->runtime_record_set, set_arguments, 4,
                "state_field_status");
            status = set_status == NULL ? OM_STATUS_OUT_OF_MEMORY
                : callback_status_check(backend, function, set_status);
        }
        (void)state_payload;
    }
    if (status == OM_STATUS_OK) {
        LLVMBuildRet(backend->builder, LLVMConstInt(backend->i32_type, 0, 0));
        *initializer_out = LLVMConstBitCast(function,
                                             backend->byte_pointer_type);
    }
    return status;
}

Om_Status create_actor_handler_wrapper(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Actor_Handler *handler,
    LLVMValueRef *wrapper_out)
{
    *wrapper_out = NULL;
    LLVMValueRef receive_function = function_for_instance(backend,
        handler->function_instance_id);
    if (receive_function == NULL) return OM_STATUS_INVALID_ARGUMENT;
    char function_name[72];
    const int name_length = snprintf(function_name, sizeof(function_name),
        ".om_actor_handler_%" PRIu32 "_%" PRIu32,
        handler->message_symbol_id.value,
        handler->function_instance_id.value);
    if (name_length < 0 || (size_t)name_length >= sizeof(function_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMTypeRef parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef wrapper_type = LLVMFunctionType(backend->i32_type,
        parameters, 3, 0);
    LLVMValueRef wrapper = LLVMAddFunction(backend->module, function_name,
                                            wrapper_type);
    if (wrapper == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMSetLinkage(wrapper, LLVMInternalLinkage);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(
        backend->context, wrapper, "entry");
    if (entry == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMPositionBuilderAtEnd(backend->builder, entry);
    LLVMValueRef context_value = LLVMGetParam(wrapper, 0);
    LLVMValueRef state_payload = LLVMGetParam(wrapper, 1);
    LLVMValueRef message_payload = LLVMGetParam(wrapper, 2);
    LLVMValueRef state_slot = LLVMBuildAlloca(backend->builder,
        backend->byte_pointer_type, "state_object_slot");
    LLVMValueRef message_slot = LLVMBuildAlloca(backend->builder,
        backend->byte_pointer_type, "message_object_slot");
    if (state_slot == NULL || message_slot == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef state_arguments[2] = {context_value, state_slot};
    LLVMValueRef state_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_actor_state_object),
        backend->runtime_actor_state_object, state_arguments, 2,
        "state_object_status");
    if (state_status == NULL) return OM_STATUS_OUT_OF_MEMORY;
    Om_Status status = callback_status_check(backend, wrapper, state_status);
    LLVMValueRef message_arguments[2] = {context_value, message_slot};
    LLVMValueRef message_status = status == OM_STATUS_OK
        ? LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(backend->runtime_actor_message_object),
            backend->runtime_actor_message_object, message_arguments, 2,
            "message_object_status") : NULL;
    if (status == OM_STATUS_OK && message_status == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        status = callback_status_check(backend, wrapper, message_status);
    }
    if (status == OM_STATUS_OK) {
        LLVMValueRef state_object = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, state_slot, "state_object");
        LLVMValueRef message_object = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, message_slot, "message_object");
        if (state_object == NULL || message_object == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            LLVMValueRef receive_arguments[3] = {
                context_value, state_object, message_object,
            };
            LLVMValueRef call = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(receive_function), receive_function,
                receive_arguments, 3, "");
            if (call == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    (void)state_payload;
    (void)message_payload;
    if (status == OM_STATUS_OK) {
        LLVMBuildRet(backend->builder, LLVMConstInt(backend->i32_type, 0, 0));
        *wrapper_out = LLVMConstBitCast(wrapper, backend->byte_pointer_type);
    }
    return status;
}

LLVMValueRef create_private_string(Om_Llvm_Backend_Context *backend,
                                          const char *name,
                                          const char *text)
{
    const size_t length = strlen(text);
    if (length >= UINT_MAX) return NULL;
    LLVMValueRef value = LLVMConstStringInContext(backend->context, text,
        (unsigned)length, 0);
    if (value == NULL) return NULL;
    LLVMValueRef global = LLVMAddGlobal(backend->module, LLVMTypeOf(value), name);
    if (global == NULL) return NULL;
    LLVMSetInitializer(global, value);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    return LLVMConstBitCast(global, backend->byte_pointer_type);
}

Om_Status actor_canonical_name_text(
    const Om_Mir_Actor_Descriptor *actor,
    char **text)
{
    if (actor == NULL || text == NULL || actor->name.data == NULL
        || actor->name.length == 0
        || (actor->canonical_module_path.length != 0
            && actor->canonical_module_path.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *text = NULL;
    size_t length = actor->canonical_module_path.length;
    const size_t separator_length = length == 0 ? 0 : 2;
    if (actor->name.length > SIZE_MAX - length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    length += actor->name.length;
    if (separator_length > SIZE_MAX - length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    length += separator_length;
    if (length >= UINT_MAX) return OM_STATUS_OUT_OF_RANGE;
    char *buffer = om_alloc_array(length + 1, sizeof(*buffer));
    if (buffer == NULL) return OM_STATUS_OUT_OF_MEMORY;
    size_t cursor = 0;
    if (actor->canonical_module_path.length != 0) {
        (void)memcpy(buffer, actor->canonical_module_path.data,
                     actor->canonical_module_path.length);
        cursor += actor->canonical_module_path.length;
        buffer[cursor++] = ':';
        buffer[cursor++] = ':';
    }
    (void)memcpy(buffer + cursor, actor->name.data, actor->name.length);
    buffer[length] = '\0';
    *text = buffer;
    return OM_STATUS_OK;
}
