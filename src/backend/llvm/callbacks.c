#include "backend_internal.h"

static Om_Status native_callback_emit_trampoline(
    Om_Llvm_Backend_Context *backend,
    LLVMBuilderRef builder,
    LLVMValueRef callback_function,
    LLVMTypeRef value_array_type,
    const Om_Llvm_Native_Abi_Type *parameter_types,
    size_t parameter_count);
static Om_Status native_callback_thunk_set_get(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    const Om_Omi_Parameter *parameter,
    Om_Llvm_Native_Callback_Thunk_Set **set_out);
static Om_Status native_callback_message_descriptor(
    Om_Llvm_Backend_Context *backend,
    const Om_Resolution_Result *resolution,
    const Om_Omi_Callback_Binding *binding,
    LLVMValueRef *descriptor_out);
static Om_Status create_private_constant_array(
    Om_Llvm_Backend_Context *backend,
    const char *global_name,
    LLVMTypeRef element_type,
    LLVMValueRef *values,
    size_t count,
    LLVMValueRef *pointer_out);

Om_Status callback_status_check(Om_Llvm_Backend_Context *backend,
                                       LLVMValueRef function,
                                       LLVMValueRef result_status)
{
    if (backend->generated_block_count > UINT32_MAX - 2) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const uint32_t index = backend->generated_block_count++;
    char failure_name[48];
    char continuation_name[48];
    int written = snprintf(failure_name, sizeof(failure_name),
        "callback_failure_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(failure_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    written = snprintf(continuation_name, sizeof(continuation_name),
        "callback_continue_%" PRIu32, index);
    if (written < 0 || (size_t)written >= sizeof(continuation_name)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(
        backend->context, function, failure_name);
    LLVMBasicBlockRef continuation = LLVMAppendBasicBlockInContext(
        backend->context, function, continuation_name);
    if (failure == NULL || continuation == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntNE,
        result_status, LLVMConstInt(backend->i32_type, 0, 0),
        "callback_failed");
    if (failed == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMBuildCondBr(backend->builder, failed, failure, continuation);
    LLVMPositionBuilderAtEnd(backend->builder, failure);
    LLVMBuildRet(backend->builder, result_status);
    LLVMPositionBuilderAtEnd(backend->builder, continuation);
    return OM_STATUS_OK;
}

static Om_Status native_callback_emit_trampoline(
    Om_Llvm_Backend_Context *backend,
    LLVMBuilderRef builder,
    LLVMValueRef callback_function,
    LLVMTypeRef value_array_type,
    const Om_Llvm_Native_Abi_Type *parameter_types,
    size_t parameter_count)
{
    if (backend == NULL || builder == NULL || callback_function == NULL
        || (parameter_count != 0
            && (value_array_type == NULL || parameter_types == NULL))
        || parameter_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(backend->context,
        callback_function, "entry");
    if (entry == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef value_storage = NULL;
    LLVMValueRef value_pointer = LLVMConstNull(
        LLVMPointerType(backend->i64_type, 0));
    if (parameter_count != 0) {
        value_storage = LLVMBuildAlloca(builder, value_array_type,
                                        "callback_value_bits");
        if (value_storage == NULL) return OM_STATUS_OUT_OF_MEMORY;
        for (size_t index = 0; index < parameter_count; ++index) {
            LLVMValueRef value = LLVMGetParam(callback_function,
                                               (unsigned)index);
            if (value == NULL) return OM_STATUS_INVALID_ARGUMENT;
            const Om_Llvm_Native_Abi_Type *abi = &parameter_types[index];
            Om_Status status = add_native_integer_extension_attribute(backend,
                callback_function, NULL, (LLVMAttributeIndex)(index + 1), abi);
            if (status != OM_STATUS_OK) return status;
            LLVMValueRef bits = value;
            if (abi->is_float) {
                if (abi->bit_width == 32) {
                    bits = LLVMBuildFPExt(builder, value, backend->double_type,
                                          "callback_float_to_double");
                }
                if (bits != NULL) {
                    bits = LLVMBuildBitCast(builder, bits, backend->i64_type,
                                             "callback_float_bits");
                }
            } else if (abi->is_integer && abi->bit_width < 64) {
                if (abi->is_boolean || !abi->is_signed) {
                    bits = LLVMBuildZExt(builder, value, backend->i64_type,
                                         "callback_unsigned_bits");
                } else {
                    bits = LLVMBuildSExt(builder, value, backend->i64_type,
                                         "callback_signed_bits");
                }
            }
            if (bits == NULL) return OM_STATUS_OUT_OF_MEMORY;
            LLVMValueRef indices[2] = {
                LLVMConstInt(backend->i32_type, 0, 0),
                LLVMConstInt(backend->i32_type, index, 0),
            };
            LLVMValueRef address = LLVMBuildGEP2(builder, value_array_type,
                value_storage, indices, 2, "callback_value_address");
            if (address == NULL || LLVMBuildStore(builder, bits, address) == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
        }
        value_pointer = LLVMBuildBitCast(builder, value_storage,
            LLVMPointerType(backend->i64_type, 0), "callback_value_pointer");
        if (value_pointer == NULL) return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef callback_address = LLVMConstBitCast(callback_function,
        backend->native_callback_function_pointer_type);
    LLVMValueRef post_arguments[3] = {
        callback_address,
        value_pointer,
        LLVMConstInt(backend->i64_type, parameter_count, 0),
    };
    if (callback_address == NULL
        || LLVMBuildCall2(builder,
            LLVMGlobalGetValueType(backend->runtime_native_callback_post),
            backend->runtime_native_callback_post, post_arguments, 3,
            "callback_post_status") == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    return LLVMBuildRetVoid(builder) == NULL
        ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status native_callback_thunk_set_get(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    const Om_Omi_Parameter *parameter,
    Om_Llvm_Native_Callback_Thunk_Set **set_out)
{
    if (backend == NULL || module == NULL || parameter == NULL
        || set_out == NULL || !parameter->is_callback
        || parameter->callback_result_type != OM_NATIVE_ABI_CVOID
        || parameter->callback_parameter_start
            > module->callback_parameter_count
        || parameter->callback_parameter_count
            > module->callback_parameter_count
                - parameter->callback_parameter_start
        || (parameter->callback_parameter_count != 0
            && module->callback_parameters == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *set_out = NULL;
    for (size_t set_index = 0;
         set_index < backend->callback_thunk_set_count; ++set_index) {
        Om_Llvm_Native_Callback_Thunk_Set *existing =
            &backend->callback_thunk_sets[set_index];
        if (existing->parameter_count != parameter->callback_parameter_count) {
            continue;
        }
        bool matches = true;
        for (size_t index = 0; index < existing->parameter_count; ++index) {
            if (existing->parameter_types[index]
                != module->callback_parameters[
                    parameter->callback_parameter_start + index].abi_type) {
                matches = false;
                break;
            }
        }
        if (matches) {
            *set_out = existing;
            return OM_STATUS_OK;
        }
    }
    const size_t parameter_count = parameter->callback_parameter_count;
    if (parameter_count > UINT_MAX
        || backend->callback_thunk_set_count >= UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Native_Abi_Type *metadata_types = parameter_count == 0 ? NULL
        : om_alloc_array(parameter_count, sizeof(*metadata_types));
    Om_Llvm_Native_Abi_Type *native_types = parameter_count == 0 ? NULL
        : om_alloc_array(parameter_count, sizeof(*native_types));
    LLVMTypeRef *llvm_types = parameter_count == 0 ? NULL
        : om_alloc_array(parameter_count, sizeof(*llvm_types));
    if (parameter_count != 0
        && (metadata_types == NULL || native_types == NULL
            || llvm_types == NULL)) {
        om_free(metadata_types);
        om_free(native_types);
        om_free(llvm_types);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count;
         ++index) {
        const Om_Native_Abi_Type abi_type = module->callback_parameters[
            parameter->callback_parameter_start + index].abi_type;
        if (abi_type == OM_NATIVE_ABI_CSTRING
            || abi_type == OM_NATIVE_ABI_CUTF8_SLICE
            || abi_type == OM_NATIVE_ABI_CPOINTER
            || abi_type == OM_NATIVE_ABI_RECORD
            || abi_type == OM_NATIVE_ABI_CVOID
            || abi_type == OM_NATIVE_ABI_INVALID) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        metadata_types[index] = abi_type;
        status = native_abi_type(backend, abi_type, false,
                                 &native_types[index]);
        if (status == OM_STATUS_OK
            && (!native_types[index].is_integer
                && !native_types[index].is_float)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            llvm_types[index] = native_types[index].llvm_type;
        }
    }
    LLVMTypeRef function_type = status == OM_STATUS_OK
        ? LLVMFunctionType(LLVMVoidTypeInContext(backend->context), llvm_types,
            (unsigned)parameter_count, 0) : NULL;
    LLVMTypeRef function_pointer_type = function_type == NULL ? NULL
        : LLVMPointerType(function_type, 0);
    LLVMTypeRef value_array_type = parameter_count == 0 ? NULL
        : LLVMArrayType2(backend->i64_type, parameter_count);
    LLVMValueRef *trampolines = status == OM_STATUS_OK
        ? om_alloc_array(OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT,
                         sizeof(*trampolines)) : NULL;
    if (status == OM_STATUS_OK
        && (function_type == NULL || function_pointer_type == NULL
            || (parameter_count != 0 && value_array_type == NULL)
            || trampolines == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t set_id = (uint32_t)backend->callback_thunk_set_count;
    LLVMBuilderRef builder = status == OM_STATUS_OK
        ? LLVMCreateBuilderInContext(backend->context) : NULL;
    if (status == OM_STATUS_OK && builder == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t slot = 0; status == OM_STATUS_OK
         && slot < OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT; ++slot) {
        char name[72];
        const int name_length = snprintf(name, sizeof(name),
            ".om_native_callback_%" PRIu32 "_%" PRIu32,
            set_id, (uint32_t)slot);
        if (name_length < 0 || (size_t)name_length >= sizeof(name)) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        trampolines[slot] = LLVMAddFunction(backend->module, name,
                                            function_type);
        if (trampolines[slot] == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMSetLinkage(trampolines[slot], LLVMInternalLinkage);
        status = native_callback_emit_trampoline(backend, builder,
            trampolines[slot], value_array_type, native_types,
            parameter_count);
    }
    if (builder != NULL) LLVMDisposeBuilder(builder);
    LLVMValueRef candidates_pointer = NULL;
    if (status == OM_STATUS_OK) {
        LLVMValueRef *candidate_values = om_alloc_array(
            OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT,
            sizeof(*candidate_values));
        if (candidate_values == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            for (size_t index = 0;
                 index < OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT; ++index) {
                candidate_values[index] = LLVMConstBitCast(trampolines[index],
                    backend->native_callback_function_pointer_type);
                if (candidate_values[index] == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                    break;
                }
            }
            char global_name[64];
            const int name_length = snprintf(global_name,
                sizeof(global_name), ".om_native_callback_candidates_%" PRIu32,
                set_id);
            LLVMTypeRef array_type = LLVMArrayType(
                backend->native_callback_function_pointer_type,
                OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT);
            LLVMValueRef global = status == OM_STATUS_OK
                && name_length >= 0 && (size_t)name_length < sizeof(global_name)
                ? LLVMAddGlobal(backend->module, array_type, global_name) : NULL;
            LLVMValueRef initializer = global == NULL ? NULL : LLVMConstArray(
                backend->native_callback_function_pointer_type,
                candidate_values, OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT);
            if (status == OM_STATUS_OK
                && (global == NULL || initializer == NULL)) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
            if (status == OM_STATUS_OK) {
                LLVMSetInitializer(global, initializer);
                LLVMSetGlobalConstant(global, 1);
                LLVMSetLinkage(global, LLVMPrivateLinkage);
                LLVMValueRef indices[2] = {
                    LLVMConstInt(backend->i32_type, 0, 0),
                    LLVMConstInt(backend->i32_type, 0, 0),
                };
                candidates_pointer = LLVMConstInBoundsGEP2(array_type,
                    global, indices, 2);
                if (candidates_pointer == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
            om_free(candidate_values);
        }
    }
    if (status == OM_STATUS_OK
        && backend->callback_thunk_set_count
            == backend->callback_thunk_set_capacity) {
        const size_t capacity = backend->callback_thunk_set_capacity == 0
            ? 4 : backend->callback_thunk_set_capacity * 2;
        if (capacity < backend->callback_thunk_set_capacity
            || capacity > SIZE_MAX / sizeof(*backend->callback_thunk_sets)) {
            status = OM_STATUS_OUT_OF_RANGE;
        } else {
            Om_Llvm_Native_Callback_Thunk_Set *sets = om_realloc_array(
                backend->callback_thunk_sets, capacity, sizeof(*sets));
            if (sets == NULL) status = OM_STATUS_OUT_OF_MEMORY;
            else {
                backend->callback_thunk_sets = sets;
                backend->callback_thunk_set_capacity = capacity;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        Om_Llvm_Native_Callback_Thunk_Set *set =
            &backend->callback_thunk_sets[
                backend->callback_thunk_set_count++];
        *set = (Om_Llvm_Native_Callback_Thunk_Set){
            .parameter_types = metadata_types,
            .parameter_count = parameter_count,
            .function_type = function_type,
            .function_pointer_type = function_pointer_type,
            .candidates = candidates_pointer,
        };
        *set_out = set;
        metadata_types = NULL;
    }
    om_free(metadata_types);
    om_free(native_types);
    om_free(llvm_types);
    om_free(trampolines);
    return status;
}

bool native_metadata_name_equal(Om_String_View left,
                                       Om_String_View right)
{
    if (!om_string_view_is_valid(left) || !om_string_view_is_valid(right)
        || left.length != right.length) {
        return false;
    }
    for (size_t index = 0; index < left.length; ++index) {
        unsigned char left_byte = (unsigned char)left.data[index];
        unsigned char right_byte = (unsigned char)right.data[index];
        if (left_byte >= 'A' && left_byte <= 'Z') {
            left_byte = (unsigned char)(left_byte + ('a' - 'A'));
        }
        if (right_byte >= 'A' && right_byte <= 'Z') {
            right_byte = (unsigned char)(right_byte + ('a' - 'A'));
        }
        if (left_byte != right_byte) {
            return false;
        }
    }
    return true;
}

static Om_Status native_callback_message_descriptor(
    Om_Llvm_Backend_Context *backend,
    const Om_Resolution_Result *resolution,
    const Om_Omi_Callback_Binding *binding,
    LLVMValueRef *descriptor_out)
{
    if (backend == NULL || resolution == NULL || binding == NULL
        || descriptor_out == NULL
        || !om_string_view_is_valid(binding->message_name)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *descriptor_out = NULL;
    const Om_Symbol *message_symbol = NULL;
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *candidate = &resolution->symbols[index];
        if (candidate->kind != OM_SYMBOL_MESSAGE
            || !native_metadata_name_equal(candidate->name,
                                           binding->message_name)) {
            continue;
        }
        if (message_symbol != NULL) return OM_STATUS_INVALID_ARGUMENT;
        message_symbol = candidate;
    }
    if (message_symbol == NULL) return OM_STATUS_NOT_FOUND;

    Om_Type_Id message_type_id = OM_TYPE_ID_INVALID;
    for (size_t index = 0; index < backend->project->actor_handler_count;
         ++index) {
        const Om_Mir_Actor_Handler *handler =
            &backend->project->actor_handlers[index];
        if (handler->message_symbol_id.value != message_symbol->id.value) {
            continue;
        }
        if (message_type_id.value != UINT32_MAX
            && message_type_id.value != handler->message_type_id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        message_type_id = handler->message_type_id;
    }
    if (message_type_id.value == UINT32_MAX) return OM_STATUS_NOT_FOUND;
    return runtime_descriptor(backend, message_type_id, 0, descriptor_out);
}

static Om_Status create_private_constant_array(
    Om_Llvm_Backend_Context *backend,
    const char *global_name,
    LLVMTypeRef element_type,
    LLVMValueRef *values,
    size_t count,
    LLVMValueRef *pointer_out)
{
    if (backend == NULL || global_name == NULL || element_type == NULL
        || values == NULL || count == 0 || count > UINT_MAX
        || pointer_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *pointer_out = NULL;
    LLVMTypeRef array_type = LLVMArrayType(element_type, (unsigned)count);
    LLVMValueRef global = array_type == NULL ? NULL
        : LLVMAddGlobal(backend->module, array_type, global_name);
    LLVMValueRef initializer = global == NULL ? NULL
        : LLVMConstArray(element_type, values, (unsigned)count);
    if (global == NULL || initializer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetInitializer(global, initializer);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMValueRef indices[2] = {
        LLVMConstInt(backend->i32_type, 0, 0),
        LLVMConstInt(backend->i32_type, 0, 0),
    };
    LLVMValueRef first_element = LLVMConstInBoundsGEP2(array_type, global,
                                                        indices, 2);
    *pointer_out = first_element == NULL ? NULL
        : LLVMConstBitCast(first_element, backend->byte_pointer_type);
    return *pointer_out == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

Om_Status emit_native_callback_registration(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Instruction *instruction,
    size_t callback_count,
    Om_Llvm_Native_Callback_Thunk_Set *const *thunk_sets,
    LLVMValueRef *message_descriptors,
    LLVMValueRef *registered_slot_out,
    LLVMTypeRef *registered_array_type_out)
{
    if (backend == NULL || function == NULL || instruction == NULL
        || callback_count == 0 || callback_count > UINT_MAX
        || thunk_sets == NULL || message_descriptors == NULL
        || registered_slot_out == NULL || registered_array_type_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *registered_slot_out = NULL;
    *registered_array_type_out = NULL;
    LLVMValueRef *candidate_sets = om_alloc_array(callback_count,
                                                   sizeof(*candidate_sets));
    LLVMValueRef *candidate_counts = om_alloc_array(callback_count,
                                                     sizeof(*candidate_counts));
    LLVMValueRef *message_types = om_alloc_array(callback_count,
                                                  sizeof(*message_types));
    if (candidate_sets == NULL || candidate_counts == NULL
        || message_types == NULL) {
        om_free(candidate_sets);
        om_free(candidate_counts);
        om_free(message_types);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < callback_count; ++index) {
        if (thunk_sets[index] == NULL || thunk_sets[index]->candidates == NULL
            || message_descriptors[index] == NULL) {
            om_free(candidate_sets);
            om_free(candidate_counts);
            om_free(message_types);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        candidate_sets[index] = LLVMConstBitCast(thunk_sets[index]->candidates,
            backend->byte_pointer_type);
        candidate_counts[index] = LLVMConstInt(backend->i64_type,
            OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT, 0);
        message_types[index] = LLVMConstBitCast(message_descriptors[index],
            backend->byte_pointer_type);
    }

    char global_name[72];
    const int sets_name_length = snprintf(global_name, sizeof(global_name),
        ".om_native_callback_sets_%" PRIu32,
        instruction->result_id.value);
    LLVMValueRef candidate_sets_pointer = NULL;
    Om_Status status = sets_name_length < 0
            || (size_t)sets_name_length >= sizeof(global_name)
        ? OM_STATUS_OUT_OF_RANGE
        : create_private_constant_array(backend, global_name,
            backend->byte_pointer_type, candidate_sets, callback_count,
            &candidate_sets_pointer);
    const int counts_name_length = snprintf(global_name, sizeof(global_name),
        ".om_native_callback_counts_%" PRIu32,
        instruction->result_id.value);
    LLVMValueRef candidate_counts_pointer = NULL;
    if (status == OM_STATUS_OK) {
        status = counts_name_length < 0
                || (size_t)counts_name_length >= sizeof(global_name)
            ? OM_STATUS_OUT_OF_RANGE
            : create_private_constant_array(backend, global_name,
                backend->i64_type, candidate_counts, callback_count,
                &candidate_counts_pointer);
    }
    const int types_name_length = snprintf(global_name, sizeof(global_name),
        ".om_native_callback_types_%" PRIu32,
        instruction->result_id.value);
    LLVMValueRef message_types_pointer = NULL;
    if (status == OM_STATUS_OK) {
        status = types_name_length < 0
                || (size_t)types_name_length >= sizeof(global_name)
            ? OM_STATUS_OUT_OF_RANGE
            : create_private_constant_array(backend, global_name,
                backend->byte_pointer_type, message_types, callback_count,
                &message_types_pointer);
    }
    om_free(candidate_sets);
    om_free(candidate_counts);
    om_free(message_types);
    if (status != OM_STATUS_OK) return status;

    LLVMTypeRef registered_array_type = LLVMArrayType(
        backend->native_callback_function_pointer_type,
        (unsigned)callback_count);
    LLVMValueRef registered_slot = entry_alloca(backend, function,
        registered_array_type, "native_callback_registered_slots");
    if (registered_slot == NULL) return OM_STATUS_OUT_OF_MEMORY;
    LLVMValueRef arguments[6] = {
        context_for_function(backend, function),
        LLVMConstInt(backend->i64_type, callback_count, 0),
        candidate_sets_pointer,
        candidate_counts_pointer,
        message_types_pointer,
        LLVMBuildBitCast(backend->builder, registered_slot,
            backend->byte_pointer_type, "native_callback_registered_pointer"),
    };
    if (arguments[0] == NULL || arguments[5] == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef register_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_native_callback_register_many),
        backend->runtime_native_callback_register_many, arguments, 6,
        "native_callback_register_status");
    status = emit_runtime_status_check(backend, block_id, register_status);
    if (status == OM_STATUS_OK) {
        *registered_slot_out = registered_slot;
        *registered_array_type_out = registered_array_type;
    }
    return status;
}

void native_callback_call_destroy(Om_Llvm_Native_Callback_Call *call)
{
    if (call == NULL) return;
    om_free(call->binding_for_parameter);
    om_free(call->thunk_sets);
    om_free(call->message_descriptors);
    *call = (Om_Llvm_Native_Callback_Call){0};
}

Om_Status native_callback_call_prepare(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *native_function,
    Om_Llvm_Native_Callback_Call *call)
{
    if (backend == NULL || module == NULL || native_function == NULL
        || call == NULL
        || native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || native_function->callback_binding_start
            > module->callback_binding_count
        || native_function->callback_binding_count
            > module->callback_binding_count
                - native_function->callback_binding_start
        || (native_function->parameter_count != 0
            && module->parameters == NULL)
        || (native_function->callback_binding_count != 0
            && module->callback_bindings == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *call = (Om_Llvm_Native_Callback_Call){0};
    call->callback_count = 0;
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) ++call->callback_count;
        else ++call->source_parameter_count;
    }
    if (call->callback_count != native_function->callback_binding_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    call->callback_count = native_function->callback_binding_count;
    if (call->callback_count == 0) return OM_STATUS_OK;
    if (native_function->parameter_count > SIZE_MAX / sizeof(size_t)
        || call->callback_count > UINT_MAX
        || call->callback_count > SIZE_MAX
            / sizeof(*call->thunk_sets)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    call->binding_for_parameter = om_alloc_array(
        native_function->parameter_count,
        sizeof(*call->binding_for_parameter));
    call->thunk_sets = om_alloc_array(call->callback_count,
                                      sizeof(*call->thunk_sets));
    call->message_descriptors = om_alloc_array(call->callback_count,
        sizeof(*call->message_descriptors));
    if (call->binding_for_parameter == NULL || call->thunk_sets == NULL
        || call->message_descriptors == NULL) {
        native_callback_call_destroy(call);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        call->binding_for_parameter[index] = SIZE_MAX;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t binding_index = 0;
         status == OM_STATUS_OK && binding_index < call->callback_count;
         ++binding_index) {
        const Om_Omi_Callback_Binding *binding = &module->callback_bindings[
            native_function->callback_binding_start + binding_index];
        if (binding->parameter_index >= native_function->parameter_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const size_t parameter_index = binding->parameter_index;
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + parameter_index];
        if (!parameter->is_callback
            || call->binding_for_parameter[parameter_index] != SIZE_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        call->binding_for_parameter[parameter_index] = binding_index;
        status = native_callback_thunk_set_get(backend, module, parameter,
            &call->thunk_sets[binding_index]);
        if (status == OM_STATUS_OK) {
            status = native_callback_message_descriptor(backend,
                backend->project->resolution, binding,
                &call->message_descriptors[binding_index]);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback
            != (call->binding_for_parameter[index] != SIZE_MAX)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status != OM_STATUS_OK) native_callback_call_destroy(call);
    return status;
}
