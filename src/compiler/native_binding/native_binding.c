#include "ombasic/compiler/native_binding.h"

#include <stdint.h>

#include "ombasic/foundation/memory.h"

static Om_Status native_message_symbol_by_name(
    const Om_Resolution_Result *resolution, Om_String_View name,
    Om_Symbol_Id *message_symbol)
{
    if (resolution == NULL || message_symbol == NULL
        || (resolution->symbol_count != 0 && resolution->symbols == NULL)
        || (name.length != 0 && name.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *message_symbol = OM_SYMBOL_ID_INVALID;
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &resolution->symbols[index];
        if (symbol->kind != OM_SYMBOL_MESSAGE
            || !om_string_view_equal(symbol->name, name)) {
            continue;
        }
        if (message_symbol->value != UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *message_symbol = symbol->id;
    }
    return message_symbol->value == UINT32_MAX
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
}

static bool callback_abi_matches_type(Om_Native_Abi_Type abi_type,
                                      Om_Type_Kind type_kind)
{
    switch (abi_type) {
    case OM_NATIVE_ABI_CINT8:
    case OM_NATIVE_ABI_CINT16:
    case OM_NATIVE_ABI_CINT32:
    case OM_NATIVE_ABI_CINT64:
    case OM_NATIVE_ABI_CUINT8:
    case OM_NATIVE_ABI_CUINT16:
    case OM_NATIVE_ABI_CUINT32:
    case OM_NATIVE_ABI_CUINT64:
    case OM_NATIVE_ABI_CINT:
    case OM_NATIVE_ABI_CUINT:
    case OM_NATIVE_ABI_CLONG:
    case OM_NATIVE_ABI_CULONG:
    case OM_NATIVE_ABI_CLONGLONG:
    case OM_NATIVE_ABI_CULONGLONG:
    case OM_NATIVE_ABI_CSIZE:
        return type_kind == OM_TYPE_INT;
    case OM_NATIVE_ABI_CFLOAT:
    case OM_NATIVE_ABI_CDOUBLE:
        return type_kind == OM_TYPE_FLOAT;
    case OM_NATIVE_ABI_CBOOL:
        return type_kind == OM_TYPE_BOOL;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_CVOID:
    case OM_NATIVE_ABI_CPOINTER:
    case OM_NATIVE_ABI_CSTRING:
    case OM_NATIVE_ABI_RECORD:
        return false;
    }
    return false;
}

static Om_Status validate_callback_message(
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *function,
    const Om_Omi_Callback_Binding *callback_binding,
    Om_Symbol_Id *message_symbol)
{
    if (resolution == NULL || inference == NULL || module == NULL
        || function == NULL || callback_binding == NULL
        || message_symbol == NULL
        || function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || (function->parameter_count != 0 && module->parameters == NULL)
        || callback_binding->parameter_index >= function->parameter_count
        || resolution->scopes == NULL
        || (resolution->symbol_count != 0 && resolution->symbols == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Parameter *parameter = &module->parameters[
        function->parameter_start + callback_binding->parameter_index];
    if (!parameter->is_callback
        || parameter->callback_result_type != OM_NATIVE_ABI_CVOID
        || parameter->callback_parameter_count
            > OM_NATIVE_MAX_CALLBACK_PARAMETERS
        || parameter->callback_parameter_start
            > module->callback_parameter_count
        || parameter->callback_parameter_count
            > module->callback_parameter_count
                - parameter->callback_parameter_start
        || (parameter->callback_parameter_count != 0
            && module->callback_parameters == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = native_message_symbol_by_name(
        resolution, callback_binding->message_name, message_symbol);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Symbol *message = om_resolution_symbol_by_id(
        resolution, *message_symbol);
    if (message == NULL || message->kind != OM_SYMBOL_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t field_count = 0;
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *field = &resolution->symbols[index];
        if (field->kind != OM_SYMBOL_FIELD
            || field->scope_id.value >= resolution->scope_count) {
            continue;
        }
        const Om_Scope *scope = &resolution->scopes[field->scope_id.value];
        if (scope->kind == OM_SCOPE_MESSAGE
            && scope->module_id.value == message->module_id.value
            && scope->owner_node.value == message->declaration_node.value) {
            field_count += 1;
        }
    }
    if (field_count != parameter->callback_parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Symbol_Id *fields = field_count == 0 ? NULL
        : om_alloc_array(field_count, sizeof(*fields));
    if (field_count != 0 && fields == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t field_index = 0;
    for (size_t index = 0; index < resolution->symbol_count; ++index) {
        const Om_Symbol *field = &resolution->symbols[index];
        if (field->kind != OM_SYMBOL_FIELD
            || field->scope_id.value >= resolution->scope_count) {
            continue;
        }
        const Om_Scope *scope = &resolution->scopes[field->scope_id.value];
        if (scope->kind == OM_SCOPE_MESSAGE
            && scope->module_id.value == message->module_id.value
            && scope->owner_node.value == message->declaration_node.value) {
            fields[field_index++] = field->id;
        }
    }
    for (size_t index = 1; index < field_count; ++index) {
        const Om_Symbol_Id value = fields[index];
        const Om_Symbol *value_symbol = om_resolution_symbol_by_id(
            resolution, value);
        size_t position = index;
        while (position > 0) {
            const Om_Symbol *previous_symbol = om_resolution_symbol_by_id(
                resolution, fields[position - 1]);
            if (previous_symbol == NULL || value_symbol == NULL
                || previous_symbol->span.start <= value_symbol->span.start) {
                break;
            }
            fields[position] = fields[position - 1];
            position -= 1;
        }
        fields[position] = value;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
         ++index) {
        const Om_Type_Scheme *scheme = om_type_inference_scheme_for_symbol(
            inference, fields[index]);
        Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
        Om_Type_View view = {0};
        if (scheme == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = om_type_store_resolve(&inference->type_store,
                                       scheme->type_id, &resolved_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(&inference->type_store,
                                       resolved_type, &view);
        }
        if (status == OM_STATUS_OK
            && !callback_abi_matches_type(
                module->callback_parameters[
                    parameter->callback_parameter_start + index].abi_type,
                view.kind)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    om_free(fields);
    return status;
}

static bool source_native_abi_type_supported(Om_Native_Abi_Type type,
                                             bool is_result)
{
    if (type == OM_NATIVE_ABI_CVOID) {
        return is_result;
    }
    switch (type) {
    case OM_NATIVE_ABI_CINT8:
    case OM_NATIVE_ABI_CINT16:
    case OM_NATIVE_ABI_CINT32:
    case OM_NATIVE_ABI_CINT64:
    case OM_NATIVE_ABI_CUINT8:
    case OM_NATIVE_ABI_CUINT16:
    case OM_NATIVE_ABI_CUINT32:
    case OM_NATIVE_ABI_CUINT64:
    case OM_NATIVE_ABI_CINT:
    case OM_NATIVE_ABI_CUINT:
    case OM_NATIVE_ABI_CLONG:
    case OM_NATIVE_ABI_CULONG:
    case OM_NATIVE_ABI_CLONGLONG:
    case OM_NATIVE_ABI_CULONGLONG:
    case OM_NATIVE_ABI_CFLOAT:
    case OM_NATIVE_ABI_CDOUBLE:
    case OM_NATIVE_ABI_CSIZE:
    case OM_NATIVE_ABI_CBOOL:
    case OM_NATIVE_ABI_CSTRING:
        return true;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_CVOID:
    case OM_NATIVE_ABI_CPOINTER:
    case OM_NATIVE_ABI_RECORD:
        return false;
    }
    return false;
}

static bool native_function_source_callable(const Om_Omi_Module *module,
                                            size_t function_index)
{
    if (module == NULL || function_index >= module->function_count
        || module->functions == NULL) {
        return false;
    }
    const Om_Omi_Native_Function *function =
        &module->functions[function_index];
    if (function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || (function->parameter_count != 0 && module->parameters == NULL)
        || !source_native_abi_type_supported(function->result_type, true)) {
        return false;
    }
    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter =
            &module->parameters[function->parameter_start + index];
        if (!parameter->is_callback) {
            if (!source_native_abi_type_supported(parameter->abi_type,
                                                  false)) {
                return false;
            }
            continue;
        }
        if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
            || parameter->callback_parameter_start
                > module->callback_parameter_count
            || parameter->callback_parameter_count
                > module->callback_parameter_count
                    - parameter->callback_parameter_start
            || (parameter->callback_parameter_count != 0
                && module->callback_parameters == NULL)) {
            return false;
        }
        for (size_t callback_index = 0;
             callback_index < parameter->callback_parameter_count;
             ++callback_index) {
            const Om_Native_Abi_Type callback_type =
                module->callback_parameters[
                    parameter->callback_parameter_start + callback_index]
                    .abi_type;
            if (!source_native_abi_type_supported(callback_type, false)
                || callback_type == OM_NATIVE_ABI_CSTRING) {
                return false;
            }
        }
    }
    return true;
}

Om_Status om_compiler_bind_native_module(
    Om_VM *vm, const Om_Bytecode_Image *image,
    const Om_Native_Library *library, const Om_Omi_Module *module,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    size_t maximum_cstring_bytes)
{
    if (vm == NULL || image == NULL || library == NULL || module == NULL
        || resolution == NULL || inference == NULL
        || module->diagnostic_count != 0
        || (module->function_count != 0 && module->functions == NULL)
        || (module->parameter_count != 0 && module->parameters == NULL)
        || (module->callback_parameter_count != 0
            && module->callback_parameters == NULL)
        || (module->callback_binding_count != 0
            && module->callback_bindings == NULL)
        || resolution->diagnostic_count != 0
        || (resolution->symbol_count != 0 && resolution->symbols == NULL)
        || (resolution->scope_count != 0 && resolution->scopes == NULL)
        || inference->diagnostic_count != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Native_Callback_Message *messages =
        module->callback_binding_count == 0 ? NULL
        : om_alloc_array(module->callback_binding_count, sizeof(*messages));
    if (module->callback_binding_count != 0 && messages == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t message_count = 0;
    Om_Status status = OM_STATUS_OK;
    for (size_t function_index = 0; status == OM_STATUS_OK
                                   && function_index < module->function_count;
         ++function_index) {
        const Om_Omi_Native_Function *function =
            &module->functions[function_index];
        if (function->callback_binding_start
                > module->callback_binding_count
            || function->callback_binding_count
                > module->callback_binding_count
                    - function->callback_binding_start) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (!native_function_source_callable(module, function_index)) {
            continue;
        }
        uint32_t bytecode_function = UINT32_MAX;
        const Om_Status lookup_status = om_vm_find_function(
            image, function->name, &bytecode_function);
        if (lookup_status == OM_STATUS_NOT_FOUND) {
            continue;
        }
        if (lookup_status != OM_STATUS_OK) {
            status = lookup_status;
            break;
        }
        for (size_t binding_index = 0;
             status == OM_STATUS_OK
                 && binding_index < function->callback_binding_count;
             ++binding_index) {
            const Om_Omi_Callback_Binding *callback_binding =
                &module->callback_bindings[
                    function->callback_binding_start + binding_index];
            Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
            status = validate_callback_message(
                resolution, inference, module, function, callback_binding,
                &message_symbol);
            if (status == OM_STATUS_OK) {
                for (size_t prior = 0; prior < message_count; ++prior) {
                    if (messages[prior].native_function_index == function_index
                        && messages[prior].parameter_index
                            == callback_binding->parameter_index) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                        break;
                    }
                }
            }
            if (status == OM_STATUS_OK) {
                messages[message_count++] = (Om_VM_Native_Callback_Message){
                    .native_function_index = function_index,
                    .parameter_index = callback_binding->parameter_index,
                    .message_symbol = message_symbol
                };
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_vm_bind_native_module(
            vm, library, module, messages, message_count,
            maximum_cstring_bytes);
    }
    om_free(messages);
    return status;
}
