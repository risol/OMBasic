#include "ombasic/ffi/native_call.h"

#include <ffi.h>

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "ombasic/ffi/abi_type.h"
#include "ombasic/ffi/c_string.h"
#include "ombasic/foundation/memory.h"

typedef union Om_Native_Abi_Storage {
    int8_t signed_8;
    int16_t signed_16;
    int32_t signed_32;
    int64_t signed_64;
    uint8_t unsigned_8;
    uint16_t unsigned_16;
    uint32_t unsigned_32;
    uint64_t unsigned_64;
    float float_32;
    double float_64;
    bool boolean;
    void *pointer;
} Om_Native_Abi_Storage;

typedef struct Om_Native_Ffi_Record {
    ffi_type type;
    ffi_type **elements;
    Om_Native_Record_Layout layout;
} Om_Native_Ffi_Record;

typedef struct Om_Native_Call_Storage {
    ffi_cif cif;
    ffi_type **argument_types;
    Om_Native_Abi_Type *parameter_types;
    Om_Native_Ffi_Record *record_types;
    size_t record_count;
    void (*function)(void);
    Om_Native_Library library;
    const Om_Omi_Module *module;
    size_t function_index;
    size_t maximum_cstring_bytes;
    size_t owner_parameter_index;
    bool has_owner_parameter;
} Om_Native_Call_Storage;

typedef struct Om_Native_Callback_Storage {
    ffi_cif cif;
    ffi_type **argument_types;
    Om_Native_Abi_Type *parameter_types;
    size_t parameter_count;
    Om_Native_Callback_Entry entry;
    void *context;
    ffi_closure *closure;
    void *code;
} Om_Native_Callback_Storage;

static ffi_type *ffi_integer_type(bool is_signed, size_t size)
{
    switch (size) {
    case 1:
        return is_signed ? &ffi_type_sint8 : &ffi_type_uint8;
    case 2:
        return is_signed ? &ffi_type_sint16 : &ffi_type_uint16;
    case 4:
        return is_signed ? &ffi_type_sint32 : &ffi_type_uint32;
    case 8:
        return is_signed ? &ffi_type_sint64 : &ffi_type_uint64;
    default:
        return NULL;
    }
}

static ffi_type *ffi_type_for_abi(Om_Native_Abi_Type abi_type,
                                  bool allow_void)
{
    if (abi_type == OM_NATIVE_ABI_CVOID) {
        return allow_void ? &ffi_type_void : NULL;
    }
    if (abi_type == OM_NATIVE_ABI_CFLOAT) {
        return &ffi_type_float;
    }
    if (abi_type == OM_NATIVE_ABI_CDOUBLE) {
        return &ffi_type_double;
    }
    if (abi_type == OM_NATIVE_ABI_CPOINTER
        || abi_type == OM_NATIVE_ABI_CSTRING) {
        return &ffi_type_pointer;
    }
    if (abi_type == OM_NATIVE_ABI_CBOOL) {
        return &ffi_type_uint8;
    }

    Om_Native_Abi_Type_Info info = {0};
    if (om_native_abi_type_info(abi_type, &info) != OM_STATUS_OK
        || info.is_floating_point || info.is_pointer || info.size == 0) {
        return NULL;
    }
    return ffi_integer_type(info.is_signed_integer, info.size);
}

static Om_Status native_record_layout(const Om_Omi_Module *module,
                                      size_t record_index,
                                      Om_Native_Record_Layout *layout)
{
    if (module == NULL || layout == NULL
        || record_index >= module->record_count || module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Record *record = &module->records[record_index];
    if (record->field_count == 0
        || record->field_start > module->record_field_count
        || record->field_count
            > module->record_field_count - record->field_start
        || module->record_fields == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t *offsets = om_alloc_array(record->field_count, sizeof(*offsets));
    if (offsets == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Status status = om_native_record_layout(
        &module->record_fields[record->field_start], record->field_count,
        offsets, record->field_count, layout);
    om_free(offsets);
    return status;
}

static Om_Status initialize_record_types(Om_Native_Call_Storage *storage)
{
    const Om_Omi_Module *module = storage->module;
    storage->record_count = module->record_count;
    if (storage->record_count == 0) {
        return OM_STATUS_OK;
    }
    if (module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    storage->record_types = om_alloc_array(
        storage->record_count, sizeof(*storage->record_types));
    if (storage->record_types == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(storage->record_types, 0,
                 storage->record_count * sizeof(*storage->record_types));

    for (size_t record_index = 0;
         record_index < storage->record_count; ++record_index) {
        const Om_Omi_Native_Record *record = &module->records[record_index];
        if (record->field_count == 0
            || record->field_start > module->record_field_count
            || record->field_count
                > module->record_field_count - record->field_start
            || module->record_fields == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Native_Ffi_Record *ffi_record =
            &storage->record_types[record_index];
        size_t element_count = 0;
        if (!om_checked_size_add(record->field_count, 1, &element_count)) {
            return OM_STATUS_OVERFLOW;
        }
        ffi_record->elements = om_alloc_array(
            element_count, sizeof(*ffi_record->elements));
        if (ffi_record->elements == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t field_index = 0;
             field_index < record->field_count; ++field_index) {
            const Om_Omi_Record_Field *field =
                &module->record_fields[record->field_start + field_index];
            ffi_record->elements[field_index] = ffi_type_for_abi(
                field->abi_type, false);
            if (ffi_record->elements[field_index] == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
        ffi_record->elements[record->field_count] = NULL;
        ffi_record->type = (ffi_type){
            .size = 0,
            .alignment = 0,
            .type = FFI_TYPE_STRUCT,
            .elements = ffi_record->elements
        };
        Om_Status status = native_record_layout(
            module, record_index, &ffi_record->layout);
        if (status != OM_STATUS_OK) {
            return status;
        }

        /* Have libffi resolve its platform layout before binding the call. */
        ffi_cif layout_cif;
        ffi_type *argument_types[] = {&ffi_record->type};
        if (ffi_prep_cif(&layout_cif, FFI_DEFAULT_ABI, 1,
                         &ffi_type_void, argument_types) != FFI_OK
            || ffi_record->type.size != ffi_record->layout.size
            || ffi_record->type.alignment != ffi_record->layout.alignment) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    return OM_STATUS_OK;
}

static bool callback_value_from_abi(Om_Native_Abi_Type abi_type,
                                    const void *argument,
                                    Om_Native_Value *value)
{
    if (argument == NULL || value == NULL) {
        return false;
    }
    *value = (Om_Native_Value){
        .kind = OM_NATIVE_VALUE_SCALAR,
        .abi_type = abi_type
    };
    if (abi_type == OM_NATIVE_ABI_CBOOL) {
        uint8_t native = 0;
        (void)memcpy(&native, argument, sizeof(native));
        value->as.boolean = native != 0;
        return true;
    }
    if (abi_type == OM_NATIVE_ABI_CFLOAT) {
        float native = 0;
        (void)memcpy(&native, argument, sizeof(native));
        value->as.floating_point = (double)native;
        return true;
    }
    if (abi_type == OM_NATIVE_ABI_CDOUBLE) {
        (void)memcpy(&value->as.floating_point, argument,
                     sizeof(value->as.floating_point));
        return true;
    }
    if (abi_type == OM_NATIVE_ABI_CPOINTER
        || abi_type == OM_NATIVE_ABI_CSTRING) {
        value->kind = OM_NATIVE_VALUE_POINTER;
        (void)memcpy(&value->as.pointer, argument,
                     sizeof(value->as.pointer));
        return true;
    }
    Om_Native_Abi_Type_Info info = {0};
    if (om_native_abi_type_info(abi_type, &info) != OM_STATUS_OK
        || info.size == 0 || info.size > sizeof(uint64_t)) {
        return false;
    }
    if (info.is_signed_integer) {
        switch (info.size) {
        case 1: {
            int8_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.signed_integer = native;
            break;
        }
        case 2: {
            int16_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.signed_integer = native;
            break;
        }
        case 4: {
            int32_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.signed_integer = native;
            break;
        }
        case 8: {
            int64_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.signed_integer = native;
            break;
        }
        default: return false;
        }
    } else {
        switch (info.size) {
        case 1: {
            uint8_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.unsigned_integer = native;
            break;
        }
        case 2: {
            uint16_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.unsigned_integer = native;
            break;
        }
        case 4: {
            uint32_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.unsigned_integer = native;
            break;
        }
        case 8: {
            uint64_t native = 0;
            (void)memcpy(&native, argument, sizeof(native));
            value->as.unsigned_integer = native;
            break;
        }
        default: return false;
        }
    }
    return true;
}

static void ffi_native_callback_dispatch(ffi_cif *cif, void *return_value,
                                         void **arguments, void *user_data)
{
    (void)cif;
    (void)return_value;
    Om_Native_Callback_Storage *storage = user_data;
    if (storage == NULL || storage->entry == NULL
        || (storage->parameter_count != 0 && arguments == NULL)) {
        return;
    }
    Om_Native_Value values[OM_NATIVE_MAX_CALLBACK_PARAMETERS] = {{0}};
    for (size_t index = 0; index < storage->parameter_count; ++index) {
        if (!callback_value_from_abi(storage->parameter_types[index],
                                     arguments[index], &values[index])) {
            return;
        }
    }
    (void)storage->entry(storage->context, values, storage->parameter_count);
}

static const Om_Omi_Native_Function *bound_function(
    const Om_Native_Call_Storage *storage)
{
    if (storage == NULL || storage->module == NULL
        || storage->function_index >= storage->module->function_count) {
        return NULL;
    }
    return &storage->module->functions[storage->function_index];
}

static Om_String_View release_symbol_for(
    const Om_Native_Call_Storage *storage,
    const Om_Omi_Native_Function *function)
{
    if (storage == NULL || storage->module == NULL || function == NULL) {
        return (Om_String_View){0};
    }
    for (size_t index = 0; index < storage->module->function_count; ++index) {
        const Om_Omi_Native_Function *candidate =
            &storage->module->functions[index];
        if (om_string_view_equal(candidate->name,
                                 function->release_function)) {
            return candidate->symbol;
        }
    }
    return (Om_String_View){0};
}

static Om_Status bind_integer_argument(Om_Native_Abi_Type abi_type,
                                      const Om_Native_Value *value,
                                      Om_Native_Abi_Storage *storage)
{
    Om_Native_Abi_Type_Info info = {0};
    if (value->kind != OM_NATIVE_VALUE_SCALAR
        || value->abi_type != abi_type
        || om_native_abi_type_info(abi_type, &info) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (info.is_signed_integer) {
        const int64_t number = value->as.signed_integer;
        switch (info.size) {
        case 1:
            if (number < INT8_MIN || number > INT8_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->signed_8 = (int8_t)number;
            break;
        case 2:
            if (number < INT16_MIN || number > INT16_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->signed_16 = (int16_t)number;
            break;
        case 4:
            if (number < INT32_MIN || number > INT32_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->signed_32 = (int32_t)number;
            break;
        case 8:
            storage->signed_64 = number;
            break;
        default:
            return OM_STATUS_INVALID_ARGUMENT;
        }
    } else {
        const uint64_t number = value->as.unsigned_integer;
        switch (info.size) {
        case 1:
            if (number > UINT8_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->unsigned_8 = (uint8_t)number;
            break;
        case 2:
            if (number > UINT16_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->unsigned_16 = (uint16_t)number;
            break;
        case 4:
            if (number > UINT32_MAX) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            storage->unsigned_32 = (uint32_t)number;
            break;
        case 8:
            storage->unsigned_64 = number;
            break;
        default:
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status bind_argument(const Om_Omi_Parameter *parameter,
                               Om_Native_Abi_Type expected_type,
                               size_t maximum_cstring_bytes,
                               const Om_Native_Value *value,
                               Om_Native_Abi_Storage *storage,
                               Om_Native_C_String *temporary_string)
{
    if (parameter == NULL || value == NULL || storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (value->abi_type != expected_type) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (parameter->is_callback) {
        if (value->kind != OM_NATIVE_VALUE_POINTER) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        storage->pointer = value->as.pointer;
        return OM_STATUS_OK;
    }
    if (expected_type == OM_NATIVE_ABI_CPOINTER) {
        if (value->kind != OM_NATIVE_VALUE_POINTER) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        storage->pointer = value->as.pointer;
        return OM_STATUS_OK;
    }
    if (expected_type == OM_NATIVE_ABI_CSTRING) {
        if (value->kind == OM_NATIVE_VALUE_POINTER) {
            storage->pointer = value->as.pointer;
            return OM_STATUS_OK;
        }
        if (value->kind != OM_NATIVE_VALUE_UTF8_STRING
            || parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Status status = om_native_cstring_from_utf8(
            value->as.utf8_string, maximum_cstring_bytes, temporary_string);
        if (status == OM_STATUS_OK) {
            storage->pointer = temporary_string->data;
        }
        return status;
    }
    if (expected_type == OM_NATIVE_ABI_CBOOL) {
        if (value->kind != OM_NATIVE_VALUE_SCALAR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        storage->boolean = value->as.boolean;
        return OM_STATUS_OK;
    }
    if (expected_type == OM_NATIVE_ABI_CFLOAT) {
        if (value->kind != OM_NATIVE_VALUE_SCALAR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const double number = value->as.floating_point;
        const float narrowed = (float)number;
        if (isfinite(number) && !isfinite((double)narrowed)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        storage->float_32 = narrowed;
        return OM_STATUS_OK;
    }
    if (expected_type == OM_NATIVE_ABI_CDOUBLE) {
        if (value->kind != OM_NATIVE_VALUE_SCALAR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        storage->float_64 = value->as.floating_point;
        return OM_STATUS_OK;
    }
    return bind_integer_argument(expected_type, value, storage);
}

static void make_call_result(const Om_Native_Call_Storage *storage,
                             Om_Native_Abi_Storage abi_result,
                             Om_Native_Call_Result *result)
{
    const Om_Omi_Native_Function *function = bound_function(storage);
    result->value = (Om_Native_Value){
        .kind = OM_NATIVE_VALUE_SCALAR,
        .abi_type = function->result_type
    };
    result->ownership = function->result_ownership;
    result->has_owner_parameter = storage->has_owner_parameter;
    result->owner_parameter_index = storage->owner_parameter_index;
    result->release_symbol = release_symbol_for(storage, function);

    if (function->result_type == OM_NATIVE_ABI_CVOID) {
        return;
    }
    if (function->result_type == OM_NATIVE_ABI_CPOINTER
        || function->result_type == OM_NATIVE_ABI_CSTRING) {
        result->value.kind = OM_NATIVE_VALUE_POINTER;
        result->value.as.pointer = abi_result.pointer;
        return;
    }
    if (function->result_type == OM_NATIVE_ABI_CBOOL) {
        result->value.as.boolean = abi_result.unsigned_8 != 0;
        return;
    }
    if (function->result_type == OM_NATIVE_ABI_CFLOAT) {
        result->value.as.floating_point = (double)abi_result.float_32;
        return;
    }
    if (function->result_type == OM_NATIVE_ABI_CDOUBLE) {
        result->value.as.floating_point = abi_result.float_64;
        return;
    }

    Om_Native_Abi_Type_Info info = {0};
    (void)om_native_abi_type_info(function->result_type, &info);
    if (info.is_signed_integer) {
        switch (info.size) {
        case 1: result->value.as.signed_integer = abi_result.signed_8; break;
        case 2: result->value.as.signed_integer = abi_result.signed_16; break;
        case 4: result->value.as.signed_integer = abi_result.signed_32; break;
        case 8: result->value.as.signed_integer = abi_result.signed_64; break;
        default: break;
        }
    } else {
        switch (info.size) {
        case 1: result->value.as.unsigned_integer = abi_result.unsigned_8; break;
        case 2: result->value.as.unsigned_integer = abi_result.unsigned_16; break;
        case 4: result->value.as.unsigned_integer = abi_result.unsigned_32; break;
        case 8: result->value.as.unsigned_integer = abi_result.unsigned_64; break;
        default: break;
        }
    }
}

static void make_record_call_result(const Om_Native_Call_Storage *storage,
                                    void *record_data,
                                    size_t record_size,
                                    Om_Native_Call_Result *result)
{
    const Om_Omi_Native_Function *function = bound_function(storage);
    result->value = (Om_Native_Value){
        .kind = OM_NATIVE_VALUE_RECORD,
        .abi_type = OM_NATIVE_ABI_RECORD,
        .record_index = function->result_record_index,
        .as.record = {.data = record_data, .size = record_size}
    };
    result->ownership = function->result_ownership;
    result->has_owner_parameter = storage->has_owner_parameter;
    result->owner_parameter_index = storage->owner_parameter_index;
    result->release_symbol = release_symbol_for(storage, function);
    result->owned_copy_storage = record_data;
}

Om_Status om_native_call_binding_init(Om_Native_Call_Binding *binding,
                                      const Om_Native_Library *library,
                                      const Om_Omi_Module *module,
                                      size_t function_index,
                                      size_t maximum_cstring_bytes)
{
    if (binding == NULL || binding->owned_storage != NULL || library == NULL
        || library->handle == NULL || module == NULL
        || module->diagnostic_count != 0 || module->functions == NULL
        || (module->parameter_count != 0 && module->parameters == NULL)
        || function_index >= module->function_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Function *function = &module->functions[function_index];
    if (function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || function->callback_binding_start > module->callback_binding_count
        || function->callback_binding_count
            > module->callback_binding_count - function->callback_binding_start
        || function->parameter_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Native_Call_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Native_Call_Storage){
        .library = *library,
        .module = module,
        .function_index = function_index,
        .maximum_cstring_bytes = maximum_cstring_bytes == 0
            ? OM_NATIVE_DEFAULT_MAX_CSTRING_BYTES : maximum_cstring_bytes,
        .owner_parameter_index = SIZE_MAX
    };
    Om_Status status = initialize_record_types(storage);
    if (status != OM_STATUS_OK) {
        om_native_call_binding_destroy(&(Om_Native_Call_Binding){storage});
        return status;
    }
    if (function->parameter_count != 0) {
        storage->argument_types = om_alloc_array(
            function->parameter_count, sizeof(*storage->argument_types));
        storage->parameter_types = om_alloc_array(
            function->parameter_count, sizeof(*storage->parameter_types));
        if (storage->argument_types == NULL
            || storage->parameter_types == NULL) {
            om_native_call_binding_destroy(
                &(Om_Native_Call_Binding){storage});
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }

    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            function->parameter_start + index];
        if (parameter->is_callback
            && (parameter->callback_parameter_start
                    > module->callback_parameter_count
                || parameter->callback_parameter_count
                    > module->callback_parameter_count
                        - parameter->callback_parameter_start
                || (parameter->callback_parameter_count != 0
                    && module->callback_parameters == NULL))) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Native_Abi_Type abi_type = parameter->is_callback
            ? OM_NATIVE_ABI_CPOINTER : parameter->abi_type;
        if (!parameter->is_callback && abi_type == OM_NATIVE_ABI_RECORD) {
            if (parameter->record_index >= storage->record_count) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            storage->argument_types[index] =
                &storage->record_types[parameter->record_index].type;
        } else {
            storage->argument_types[index] =
                ffi_type_for_abi(abi_type, false);
        }
        storage->parameter_types[index] = abi_type;
        if (storage->argument_types[index] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
    }
    ffi_type *return_type = NULL;
    if (function->result_type == OM_NATIVE_ABI_RECORD) {
        if (function->result_record_index >= storage->record_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            return_type =
                &storage->record_types[function->result_record_index].type;
        }
    } else {
        return_type = ffi_type_for_abi(function->result_type, true);
    }
    if (status == OM_STATUS_OK && return_type == NULL) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK
        && function->result_ownership == OM_NATIVE_OWNERSHIP_BORROWED) {
        bool owner_found = false;
        for (size_t index = 0; index < function->parameter_count; ++index) {
            const Om_Omi_Parameter *parameter = &module->parameters[
                function->parameter_start + index];
            if (om_string_view_equal(parameter->name,
                                     function->result_owner_parameter)) {
                storage->owner_parameter_index = index;
                storage->has_owner_parameter = true;
                owner_found = true;
                break;
            }
        }
        if (!owner_found) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == OM_STATUS_OK) {
        void *symbol_address = NULL;
        status = om_native_library_lookup(
            library, function->symbol, &symbol_address);
        if (status == OM_STATUS_OK) {
            if (symbol_address == NULL) {
                status = OM_STATUS_NOT_FOUND;
            } else if (sizeof(storage->function) != sizeof(symbol_address)) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                (void)memcpy(&storage->function, &symbol_address,
                             sizeof(storage->function));
            }
        }
    }
    if (status == OM_STATUS_OK
        && ffi_prep_cif(&storage->cif, FFI_DEFAULT_ABI,
                        (unsigned int)function->parameter_count,
                        return_type, storage->argument_types) != FFI_OK) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status != OM_STATUS_OK) {
        om_native_call_binding_destroy(&(Om_Native_Call_Binding){storage});
        return status;
    }
    binding->owned_storage = storage;
    return OM_STATUS_OK;
}

Om_Status om_native_call_binding_signature(
    const Om_Native_Call_Binding *binding,
    Om_Native_Call_Signature *signature)
{
    if (binding == NULL || binding->owned_storage == NULL
        || signature == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Call_Storage *storage = binding->owned_storage;
    const Om_Omi_Native_Function *function = bound_function(storage);
    if (function == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *signature = (Om_Native_Call_Signature){
        .result_type = function->result_type,
        .result_record_index = function->result_record_index,
        .result_ownership = function->result_ownership,
        .blocking = function->blocking,
        .parameter_types = storage->parameter_types,
        .parameters = function->parameter_count == 0 ? NULL
            : &storage->module->parameters[function->parameter_start],
        .callback_parameters = storage->module->callback_parameters,
        .callback_parameter_count =
            storage->module->callback_parameter_count,
        .parameter_count = function->parameter_count
    };
    return OM_STATUS_OK;
}

Om_Status om_native_callback_closure_init(
    const Om_Native_Call_Binding *binding, size_t parameter_index,
    Om_Native_Callback_Entry entry, void *context,
    Om_Native_Callback_Closure *closure)
{
    if (binding == NULL || binding->owned_storage == NULL || entry == NULL
        || closure == NULL || closure->owned_storage != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Call_Storage *call_storage = binding->owned_storage;
    const Om_Omi_Native_Function *function = bound_function(call_storage);
    if (function == NULL || parameter_index >= function->parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Parameter *parameter = &call_storage->module->parameters[
        function->parameter_start + parameter_index];
    if (!parameter->is_callback
        || parameter->callback_result_type != OM_NATIVE_ABI_CVOID
        || parameter->callback_parameter_count
            > OM_NATIVE_MAX_CALLBACK_PARAMETERS
        || (parameter->callback_parameter_count != 0
            && call_storage->module->callback_parameters == NULL)
        || parameter->callback_parameter_start
            > call_storage->module->callback_parameter_count
        || parameter->callback_parameter_count
            > call_storage->module->callback_parameter_count
                - parameter->callback_parameter_start) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Native_Callback_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Native_Callback_Storage){
        .parameter_count = parameter->callback_parameter_count,
        .entry = entry,
        .context = context
    };
    if (storage->parameter_count != 0) {
        storage->argument_types = om_alloc_array(
            storage->parameter_count, sizeof(*storage->argument_types));
        storage->parameter_types = om_alloc_array(
            storage->parameter_count, sizeof(*storage->parameter_types));
        if (storage->argument_types == NULL
            || storage->parameter_types == NULL) {
            om_native_callback_closure_destroy(
                &(Om_Native_Callback_Closure){storage});
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; index < storage->parameter_count; ++index) {
        const Om_Omi_Callback_Parameter *callback_parameter =
            &call_storage->module->callback_parameters[
                parameter->callback_parameter_start + index];
        storage->parameter_types[index] = callback_parameter->abi_type;
        storage->argument_types[index] = ffi_type_for_abi(
            callback_parameter->abi_type, false);
        if (storage->argument_types[index] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
    }
    if (status == OM_STATUS_OK
        && ffi_prep_cif(&storage->cif, FFI_DEFAULT_ABI,
                        (unsigned int)storage->parameter_count,
                        &ffi_type_void, storage->argument_types) != FFI_OK) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        storage->closure = ffi_closure_alloc(sizeof(*storage->closure),
                                             &storage->code);
        if (storage->closure == NULL || storage->code == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK
        && ffi_prep_closure_loc(storage->closure, &storage->cif,
                                ffi_native_callback_dispatch, storage,
                                storage->code) != FFI_OK) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status != OM_STATUS_OK) {
        om_native_callback_closure_destroy(
            &(Om_Native_Callback_Closure){storage});
        return status;
    }
    closure->owned_storage = storage;
    return OM_STATUS_OK;
}

void *om_native_callback_closure_code(
    const Om_Native_Callback_Closure *closure)
{
    if (closure == NULL || closure->owned_storage == NULL) {
        return NULL;
    }
    const Om_Native_Callback_Storage *storage = closure->owned_storage;
    return storage->code;
}

void om_native_callback_closure_destroy(
    Om_Native_Callback_Closure *closure)
{
    if (closure == NULL || closure->owned_storage == NULL) {
        return;
    }
    Om_Native_Callback_Storage *storage = closure->owned_storage;
    if (storage->closure != NULL) {
        ffi_closure_free(storage->closure);
    }
    om_free(storage->argument_types);
    om_free(storage->parameter_types);
    om_free(storage);
    closure->owned_storage = NULL;
}

void om_native_call_binding_destroy(Om_Native_Call_Binding *binding)
{
    if (binding == NULL || binding->owned_storage == NULL) {
        return;
    }
    Om_Native_Call_Storage *storage = binding->owned_storage;
    if (storage->record_types != NULL) {
        for (size_t index = 0; index < storage->record_count; ++index) {
            om_free(storage->record_types[index].elements);
        }
    }
    om_free(storage->record_types);
    om_free(storage->argument_types);
    om_free(storage->parameter_types);
    om_free(storage);
    binding->owned_storage = NULL;
}

Om_Status om_native_call_invoke(const Om_Native_Call_Binding *binding,
                               Om_Native_Value *arguments,
                               size_t argument_count,
                               Om_Native_Call_Result *result)
{
    if (binding == NULL || binding->owned_storage == NULL || result == NULL
        || (argument_count != 0 && arguments == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Call_Storage *storage = binding->owned_storage;
    const Om_Omi_Native_Function *function = bound_function(storage);
    if (function == NULL || argument_count != function->parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Native_Abi_Storage *argument_storage = argument_count == 0 ? NULL
        : om_alloc_array(argument_count, sizeof(*argument_storage));
    void **argument_values = argument_count == 0 ? NULL
        : om_alloc_array(argument_count, sizeof(*argument_values));
    void **record_argument_storage = argument_count == 0 ? NULL
        : om_alloc_array(argument_count, sizeof(*record_argument_storage));
    Om_Native_C_String *temporary_strings = argument_count == 0 ? NULL
        : om_alloc_array(argument_count, sizeof(*temporary_strings));
    if (argument_count != 0
        && (argument_storage == NULL || argument_values == NULL
            || record_argument_storage == NULL || temporary_strings == NULL)) {
        om_free(argument_storage);
        om_free(argument_values);
        om_free(record_argument_storage);
        om_free(temporary_strings);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (argument_count != 0) {
        (void)memset(argument_storage, 0,
                     argument_count * sizeof(*argument_storage));
        (void)memset(record_argument_storage, 0,
                     argument_count * sizeof(*record_argument_storage));
        (void)memset(temporary_strings, 0,
                     argument_count * sizeof(*temporary_strings));
    }

    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; index < argument_count; ++index) {
        const Om_Omi_Parameter *parameter = &storage->module->parameters[
            function->parameter_start + index];
        const Om_Native_Abi_Type expected_type = storage->parameter_types[index];
        if (!parameter->is_callback
            && expected_type == OM_NATIVE_ABI_RECORD) {
            if (parameter->record_index >= storage->record_count
                || arguments[index].kind != OM_NATIVE_VALUE_RECORD
                || arguments[index].abi_type != OM_NATIVE_ABI_RECORD
                || arguments[index].record_index != parameter->record_index
                || arguments[index].as.record.data == NULL
                || arguments[index].as.record.size
                    != storage->record_types[
                        parameter->record_index].layout.size) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            const size_t record_size = storage->record_types[
                parameter->record_index].layout.size;
            record_argument_storage[index] = om_alloc(record_size);
            if (record_argument_storage[index] == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            (void)memcpy(record_argument_storage[index],
                         arguments[index].as.record.data, record_size);
            argument_values[index] = record_argument_storage[index];
            continue;
        }
        status = bind_argument(
            parameter, expected_type, storage->maximum_cstring_bytes,
            &arguments[index], &argument_storage[index],
            &temporary_strings[index]);
        if (status != OM_STATUS_OK) {
            break;
        }
        argument_values[index] = &argument_storage[index];
    }
    void *record_result_storage = NULL;
    size_t record_result_size = 0;
    if (status == OM_STATUS_OK
        && function->result_type == OM_NATIVE_ABI_RECORD) {
        if (function->result_record_index >= storage->record_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            record_result_size = storage->record_types[
                function->result_record_index].layout.size;
            record_result_storage = om_alloc(record_result_size);
            if (record_result_storage == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        Om_Native_Abi_Storage native_result = {0};
        void *result_pointer = function->result_type == OM_NATIVE_ABI_CVOID
            ? NULL : function->result_type == OM_NATIVE_ABI_RECORD
                ? record_result_storage : &native_result;
        ffi_call((ffi_cif *)&storage->cif, storage->function,
                 result_pointer, argument_values);
        if (function->result_type == OM_NATIVE_ABI_RECORD) {
            make_record_call_result(storage, record_result_storage,
                                    record_result_size, result);
            record_result_storage = NULL;
        } else {
            make_call_result(storage, native_result, result);
        }
        if (function->result_type == OM_NATIVE_ABI_CSTRING
            && function->result_ownership == OM_NATIVE_OWNERSHIP_BORROWED
            && native_result.pointer != NULL) {
            Om_Native_String copy = {0};
            status = om_native_utf8_from_cstring(
                native_result.pointer, storage->maximum_cstring_bytes,
                &copy);
            if (status == OM_STATUS_OK) {
                result->value.kind = OM_NATIVE_VALUE_UTF8_STRING;
                result->value.as.utf8_string =
                    (Om_String_View){copy.data, copy.length};
                result->owned_copy_storage = copy.data;
            } else {
                *result = (Om_Native_Call_Result){0};
            }
        }
        for (size_t index = 0; index < argument_count; ++index) {
            const Om_Omi_Parameter *parameter = &storage->module->parameters[
                function->parameter_start + index];
            if (parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED
                && arguments[index].kind == OM_NATIVE_VALUE_POINTER) {
                arguments[index].as.pointer = NULL;
            }
        }
    }
    for (size_t index = 0; index < argument_count; ++index) {
        om_native_cstring_destroy(&temporary_strings[index]);
        om_free(record_argument_storage[index]);
    }
    om_free(record_result_storage);
    om_free(argument_storage);
    om_free(argument_values);
    om_free(record_argument_storage);
    om_free(temporary_strings);
    return status;
}

void om_native_call_result_destroy(Om_Native_Call_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->owned_copy_storage);
    *result = (Om_Native_Call_Result){0};
}

Om_Status om_native_call_result_release(const Om_Native_Library *library,
                                       Om_Native_Call_Result *result)
{
    if (library == NULL || result == NULL
        || result->ownership != OM_NATIVE_OWNERSHIP_OWNED
        || result->value.kind != OM_NATIVE_VALUE_POINTER
        || (result->value.abi_type != OM_NATIVE_ABI_CPOINTER
            && result->value.abi_type != OM_NATIVE_ABI_CSTRING)
        || result->release_symbol.data == NULL
        || result->release_symbol.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result->value.as.pointer == NULL) {
        return OM_STATUS_OK;
    }
    const Om_Status status = om_native_owned_pointer_release(
        library, result->release_symbol, result->value.as.pointer);
    if (status == OM_STATUS_OK) {
        result->value.as.pointer = NULL;
    }
    return status;
}

Om_Status om_native_call_result_release_bound(
    const Om_Native_Call_Binding *binding,
    Om_Native_Call_Result *result)
{
    if (binding == NULL || binding->owned_storage == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Call_Storage *storage = binding->owned_storage;
    return om_native_call_result_release(&storage->library, result);
}
