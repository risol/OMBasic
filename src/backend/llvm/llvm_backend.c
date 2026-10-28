#include "ombasic/backend/llvm_backend.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "llvm-c/Analysis.h"
#include "llvm-c/Core.h"
#include "llvm-c/DebugInfo.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"

#include "abi/native_abi.h"
#include "ombasic/foundation/fingerprint.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/target/target.h"

#ifndef OM_LLVM_TARGET_TRIPLE
#define OM_LLVM_TARGET_TRIPLE ""
#endif
#ifndef OM_LLVM_OPT_LEVEL
#define OM_LLVM_OPT_LEVEL 0
#endif
#if OM_LLVM_OPT_LEVEL != 0 && OM_LLVM_OPT_LEVEL != 2
#error "OM_LLVM_OPT_LEVEL must be 0 or 2"
#endif

typedef struct Om_Llvm_Type_Descriptor_Cache {
    Om_Type_Id type_id;
    LLVMValueRef global;
    uint64_t fingerprint;
} Om_Llvm_Type_Descriptor_Cache;

#define OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT 64u

typedef struct Om_Llvm_Native_Callback_Thunk_Set {
    Om_Native_Abi_Type *parameter_types;
    size_t parameter_count;
    LLVMTypeRef function_type;
    LLVMTypeRef function_pointer_type;
    LLVMValueRef candidates;
} Om_Llvm_Native_Callback_Thunk_Set;

typedef struct Om_Llvm_Native_Callback_Call {
    size_t callback_count;
    size_t source_parameter_count;
    size_t *binding_for_parameter;
    Om_Llvm_Native_Callback_Thunk_Set **thunk_sets;
    LLVMValueRef *message_descriptors;
} Om_Llvm_Native_Callback_Call;

typedef struct Om_Llvm_Backend {
    const Om_Mir_Project *project;
    Om_Target target;
    Om_String_View source_root;
    const Om_Backend_Source_Location *source_locations;
    size_t source_location_count;
    const Om_Mir_Instruction *current_instruction;
    const Om_Mir_Function *current_function;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTargetMachineRef target_machine;
    LLVMTargetDataRef target_data;
    LLVMDIBuilderRef debug_builder;
    LLVMMetadataRef debug_compile_unit;
    LLVMMetadataRef *debug_files;
    LLVMMetadataRef *debug_subprograms;
    bool debug_finalized;
    LLVMTypeRef i1_type;
    LLVMTypeRef i8_type;
    LLVMTypeRef i32_type;
    LLVMTypeRef i64_type;
    LLVMTypeRef double_type;
    LLVMTypeRef byte_pointer_type;
    LLVMTypeRef runtime_source_span_type;
    LLVMTypeRef runtime_type_descriptor_type;
    LLVMTypeRef runtime_field_descriptor_type;
    LLVMTypeRef runtime_actor_handler_descriptor_type;
    LLVMTypeRef runtime_actor_descriptor_type;
    LLVMTypeRef runtime_program_descriptor_type;
    LLVMValueRef *functions;
    LLVMBasicBlockRef *blocks;
    LLVMBasicBlockRef *block_tails;
    LLVMValueRef *values;
    LLVMValueRef runtime_context_create;
    LLVMValueRef runtime_context_destroy;
    LLVMValueRef runtime_set_source_spans;
    LLVMValueRef runtime_run;
    LLVMValueRef runtime_fault;
    LLVMValueRef runtime_roots_push;
    LLVMValueRef runtime_roots_pop;
    LLVMValueRef runtime_native_pointer_create;
    LLVMValueRef runtime_native_pointer_borrow;
    LLVMValueRef runtime_native_pointer_take;
    LLVMValueRef runtime_string_create;
    LLVMValueRef runtime_string_cstring_view;
    LLVMValueRef runtime_string_from_cstring;
    LLVMValueRef runtime_string_from_owned_cstring;
    LLVMValueRef runtime_string_format;
    LLVMValueRef runtime_string_concat;
    LLVMValueRef runtime_print;
    LLVMValueRef runtime_record_create;
    LLVMValueRef runtime_record_set;
    LLVMValueRef runtime_record_get;
    LLVMValueRef runtime_list_create;
    LLVMValueRef runtime_list_append;
    LLVMValueRef runtime_list_get;
    LLVMValueRef runtime_list_length;
    LLVMValueRef runtime_map_create;
    LLVMValueRef runtime_map_set;
    LLVMValueRef runtime_map_get;
    LLVMValueRef runtime_map_key_at;
    LLVMValueRef runtime_map_length;
    LLVMValueRef runtime_actor_system_start;
    LLVMValueRef runtime_actor_system_wait;
    LLVMValueRef runtime_actor_spawn_id;
    LLVMValueRef runtime_actor_spawn_at;
    LLVMValueRef runtime_distribution_create;
    LLVMValueRef runtime_distribution_destroy;
    LLVMValueRef runtime_actor_self_id;
    LLVMValueRef runtime_actor_state_object;
    LLVMValueRef runtime_actor_message_object;
    LLVMValueRef runtime_actor_send_id;
    LLVMValueRef runtime_actor_watch_id;
    LLVMValueRef runtime_actor_stop;
    LLVMValueRef runtime_native_callback_register_many;
    LLVMValueRef runtime_native_callback_unregister_many;
    LLVMValueRef runtime_native_callback_post;
    LLVMTypeRef native_callback_function_pointer_type;
    LLVMValueRef *root_frames;
    LLVMValueRef *root_arrays;
    uint32_t *root_slot_indices;
    uint32_t *root_slot_counts_by_value;
    size_t *root_counts;
    Om_Llvm_Type_Descriptor_Cache *descriptors;
    size_t descriptor_count;
    size_t descriptor_capacity;
    Om_Llvm_Native_Callback_Thunk_Set *callback_thunk_sets;
    size_t callback_thunk_set_count;
    size_t callback_thunk_set_capacity;
    LLVMValueRef *actor_descriptors;
    LLVMValueRef program_descriptor;
    uint32_t generated_block_count;
} Om_Llvm_Backend;

enum Om_Llvm_Runtime_Fault_Code {
    OM_LLVM_RUNTIME_FAULT_INTEGER_OVERFLOW = 1,
    OM_LLVM_RUNTIME_FAULT_DIVISION_BY_ZERO = 2,
    OM_LLVM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS = 3,
    OM_LLVM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND = 4,
    OM_LLVM_RUNTIME_FAULT_ZERO_RANGE_STEP = 5,
    OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED = 6,
    OM_LLVM_RUNTIME_FAULT_ACTOR_FAILED = 7
};

static Om_Status language_type(Om_Llvm_Backend *backend,
                               Om_Type_Id type_id,
                               unsigned depth,
                               LLVMTypeRef *llvm_type);
static Om_Status native_record_llvm_type(Om_Llvm_Backend *backend,
                                         const Om_Omi_Module *module,
                                         size_t record_index,
                                         LLVMTypeRef *llvm_type);
static LLVMValueRef context_for_function(const Om_Llvm_Backend *backend,
                                        const Om_Mir_Function *function);

static LLVMMetadataRef create_debug_file(Om_Llvm_Backend *backend,
                                         Om_String_View path)
{
    if (backend == NULL || backend->debug_builder == NULL
        || !om_string_view_is_valid(path) || path.length == 0
        || memchr(path.data, '\0', path.length) != NULL) {
        return NULL;
    }
    size_t filename_start = 0;
    for (size_t index = 0; index < path.length; ++index) {
        if (path.data[index] == '/' || path.data[index] == '\\') {
            filename_start = index + 1;
        }
    }
    if (filename_start == path.length) {
        return NULL;
    }
    const bool is_absolute = path.data[0] == '/' || path.data[0] == '\\'
        || (path.length >= 2 && path.data[1] == ':');
    if (!is_absolute && backend->source_root.length != 0
        && om_string_view_is_valid(backend->source_root)) {
        size_t root_start = 0;
        if (backend->source_root.length >= 4
            && backend->source_root.data[0] == '\\'
            && backend->source_root.data[1] == '\\'
            && backend->source_root.data[2] == '?'
            && backend->source_root.data[3] == '\\') {
            root_start = 4;
        }
        size_t root_length = backend->source_root.length - root_start;
        const size_t relative_directory_length = filename_start == 0
            ? 0 : filename_start - 1;
        char separator = '/';
        for (size_t index = root_start; index < backend->source_root.length;
             ++index) {
            if (backend->source_root.data[index] == '\\') {
                separator = '\\';
            } else if (backend->source_root.data[index] == '/') {
                separator = '/';
            }
        }
        const bool needs_separator = root_length != 0
            && backend->source_root.data[backend->source_root.length - 1] != '/'
            && backend->source_root.data[backend->source_root.length - 1] != '\\';
        size_t directory_length = root_length;
        if (!om_checked_size_add(directory_length,
                needs_separator ? 1 : 0, &directory_length)
            || !om_checked_size_add(directory_length,
                relative_directory_length, &directory_length)) {
            return NULL;
        }
        char *directory = om_alloc(directory_length == 0 ? 1 : directory_length);
        if (directory == NULL) {
            return NULL;
        }
        size_t offset = 0;
        for (size_t index = root_start;
             index < backend->source_root.length; ++index) {
            const char value = backend->source_root.data[index];
            directory[offset++] = value == '/' || value == '\\'
                ? separator : value;
        }
        if (needs_separator) {
            directory[offset++] = separator;
        }
        for (size_t index = 0; index < relative_directory_length; ++index) {
            const char value = path.data[index];
            directory[offset++] = value == '/' || value == '\\'
                ? separator : value;
        }
        LLVMMetadataRef file = LLVMDIBuilderCreateFile(
            backend->debug_builder, path.data + filename_start,
            path.length - filename_start, directory, directory_length);
        om_free(directory);
        return file;
    }
    const size_t directory_length = filename_start == 0 ? 0
        : (filename_start == 1 ? 1 : filename_start - 1);
    return LLVMDIBuilderCreateFile(backend->debug_builder,
        path.data + filename_start, path.length - filename_start,
        path.data, directory_length);
}

static Om_Status initialize_debug_info(Om_Llvm_Backend *backend,
                                       const char *target_triple)
{
    if (backend == NULL || target_triple == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t first_location = SIZE_MAX;
    for (size_t index = 0; index < backend->source_location_count; ++index) {
        const Om_Backend_Source_Location *location =
            &backend->source_locations[index];
        if (location->line != 0 && location->path.length != 0) {
            first_location = index;
            break;
        }
    }
    if (first_location == SIZE_MAX) {
        return OM_STATUS_OK;
    }
    backend->debug_builder = LLVMCreateDIBuilder(backend->module);
    if (backend->debug_builder == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < backend->source_location_count; ++index) {
        const Om_Backend_Source_Location *location =
            &backend->source_locations[index];
        if (location->line == 0 || location->path.length == 0) {
            continue;
        }
        if (!om_string_view_is_valid(location->path)
            || memchr(location->path.data, '\0', location->path.length) != NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        backend->debug_files[index] = create_debug_file(backend,
                                                         location->path);
        if (backend->debug_files[index] == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMMetadataRef compile_unit_file = backend->debug_files[first_location];
    if (compile_unit_file == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const char producer[] = "OMBasic Native AOT";
    backend->debug_compile_unit = LLVMDIBuilderCreateCompileUnit(
        backend->debug_builder,
            LLVMDWARFSourceLanguageC, compile_unit_file, producer,
            sizeof(producer) - 1, 0, "", 0, 0, "", 0,
            LLVMDWARFEmissionFull, 0, 0, 0, "", 0, "", 0);
    if (backend->debug_compile_unit == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMAddModuleFlag(backend->module, LLVMModuleFlagBehaviorWarning,
        "Debug Info Version", sizeof("Debug Info Version") - 1,
        LLVMValueAsMetadata(LLVMConstInt(
            LLVMInt32TypeInContext(backend->context),
            LLVMDebugMetadataVersion(), 0)));
    if (strstr(target_triple, "windows") != NULL
        || strstr(target_triple, "win32") != NULL) {
        LLVMAddModuleFlag(backend->module, LLVMModuleFlagBehaviorWarning,
            "CodeView", sizeof("CodeView") - 1,
            LLVMValueAsMetadata(LLVMConstInt(
                LLVMInt32TypeInContext(backend->context), 1, 0)));
    }
    LLVMMetadataRef return_type = LLVMDIBuilderCreateUnspecifiedType(
        backend->debug_builder, "OMBasic value", sizeof("OMBasic value") - 1);
    if (return_type == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMMetadataRef subroutine_types[1] = {return_type};
    for (size_t function_index = 0;
         function_index < backend->project->function_count; ++function_index) {
        const Om_Mir_Function *function =
            &backend->project->functions[function_index];
        const Om_Backend_Source_Location *first_function_location = NULL;
        LLVMMetadataRef function_file = NULL;
        for (size_t block_index = 0;
             block_index < backend->project->block_count
                 && first_function_location == NULL; ++block_index) {
            const Om_Mir_Block *block =
                &backend->project->blocks[block_index];
            if (block->function_id.value != function->id.value) {
                continue;
            }
            for (size_t offset = 0; offset < block->instruction_count; ++offset) {
                const Om_Mir_Instruction *instruction =
                    &backend->project->instructions[
                        block->instruction_start + offset];
                if (instruction->id.value >= backend->source_location_count) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                const Om_Backend_Source_Location *location =
                    &backend->source_locations[instruction->id.value];
                if (location->line != 0
                    && backend->debug_files[instruction->id.value] != NULL) {
                    first_function_location = location;
                    function_file =
                        backend->debug_files[instruction->id.value];
                    break;
                }
            }
        }
        if (first_function_location == NULL) {
            continue;
        }
        LLVMValueRef llvm_function = backend->functions[function_index];
        size_t linkage_name_length = 0;
        const char *linkage_name = LLVMGetValueName2(llvm_function,
                                                     &linkage_name_length);
        const char *debug_name = function->debug_name.data;
        size_t debug_name_length = function->debug_name.length;
        if (!om_string_view_is_valid(function->debug_name)
            || debug_name_length == 0) {
            debug_name = linkage_name;
            debug_name_length = linkage_name_length;
        }
        LLVMMetadataRef subroutine_type = LLVMDIBuilderCreateSubroutineType(
            backend->debug_builder, function_file, subroutine_types, 1,
            LLVMDIFlagZero);
        if (debug_name == NULL || linkage_name == NULL
            || subroutine_type == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        LLVMMetadataRef subprogram = LLVMDIBuilderCreateFunction(
            backend->debug_builder, backend->debug_compile_unit, debug_name,
            debug_name_length, linkage_name, linkage_name_length,
            function_file, first_function_location->line, subroutine_type,
            LLVMGetLinkage(llvm_function) != LLVMExternalLinkage, 1,
            first_function_location->line, LLVMDIFlagZero, 0);
        if (subprogram == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        backend->debug_subprograms[function_index] = subprogram;
        LLVMSetSubprogram(llvm_function, subprogram);
    }
    return OM_STATUS_OK;
}

static void set_instruction_debug_location(Om_Llvm_Backend *backend,
                                           const Om_Mir_Function *function,
                                           const Om_Mir_Instruction *instruction)
{
    LLVMMetadataRef location_metadata = NULL;
    if (backend->debug_builder != NULL
        && function->id.value < backend->project->function_count
        && backend->debug_subprograms[function->id.value] != NULL
        && instruction->id.value < backend->source_location_count) {
        const Om_Backend_Source_Location *location =
            &backend->source_locations[instruction->id.value];
        if (location->line != 0
            && backend->debug_files[instruction->id.value] != NULL) {
            location_metadata = LLVMDIBuilderCreateDebugLocation(
                backend->context, location->line, location->column,
                backend->debug_subprograms[function->id.value], NULL);
        }
    }
    LLVMSetCurrentDebugLocation2(backend->builder, location_metadata);
}

static Om_Status resolved_type(const Om_Type_Store *store,
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

static Om_Status language_function_type(Om_Llvm_Backend *backend,
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

static Om_Status language_type(Om_Llvm_Backend *backend,
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

static Om_Status type_kind(Om_Llvm_Backend *backend,
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

static LLVMValueRef value_at(const Om_Llvm_Backend *backend,
                             Om_Mir_Value_Id value_id)
{
    if (value_id.value >= backend->project->value_count) {
        return NULL;
    }
    return backend->values[value_id.value];
}

static LLVMValueRef entry_alloca(Om_Llvm_Backend *backend,
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

static LLVMValueRef function_for_instance(const Om_Llvm_Backend *backend,
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

static Om_Status initialize_types(Om_Llvm_Backend *backend)
{
    backend->i1_type = LLVMInt1TypeInContext(backend->context);
    backend->i8_type = LLVMInt8TypeInContext(backend->context);
    backend->i32_type = LLVMInt32TypeInContext(backend->context);
    backend->i64_type = LLVMInt64TypeInContext(backend->context);
    backend->double_type = LLVMDoubleTypeInContext(backend->context);
    backend->byte_pointer_type = LLVMPointerType(backend->i8_type, 0);

    backend->runtime_field_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Field_Descriptor");
    LLVMTypeRef field_members[2] = {
        backend->i64_type, backend->byte_pointer_type,
    };
    LLVMStructSetBody(backend->runtime_field_descriptor_type, field_members,
                      2, 0);
    backend->runtime_type_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Type_Descriptor");
    LLVMTypeRef descriptor_members[15] = {
        backend->i64_type,
        backend->i32_type,
        backend->i64_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->i32_type,
    };
    LLVMStructSetBody(backend->runtime_type_descriptor_type,
                      descriptor_members, 15, 0);

    LLVMTypeRef source_span_members[3] = {
        backend->byte_pointer_type, backend->i32_type, backend->i32_type,
    };
    backend->runtime_source_span_type = LLVMStructTypeInContext(
        backend->context, source_span_members, 3, 0);

    backend->runtime_actor_handler_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Actor_Handler_Descriptor");
    LLVMTypeRef handler_members[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMStructSetBody(backend->runtime_actor_handler_descriptor_type,
                      handler_members, 2, 0);
    backend->runtime_actor_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Actor_Descriptor");
    LLVMTypeRef actor_members[6] = {
        backend->i64_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMStructSetBody(backend->runtime_actor_descriptor_type,
                      actor_members, 6, 0);
    backend->runtime_program_descriptor_type = LLVMStructCreateNamed(
        backend->context, "Om_Runtime_Program_Descriptor");
    LLVMTypeRef program_members[13] = {
        backend->i64_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
        backend->i64_type,
        LLVMPointerType(backend->runtime_source_span_type, 0),
        backend->i64_type,
        backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMStructSetBody(backend->runtime_program_descriptor_type,
                      program_members, 13, 0);

    LLVMTypeRef context_create_parameters[1] = {backend->i64_type};
    LLVMTypeRef context_create_type = LLVMFunctionType(
        backend->byte_pointer_type, context_create_parameters, 1, 0);
    backend->runtime_context_create = LLVMAddFunction(backend->module,
        "om_runtime_context_create", context_create_type);
    LLVMTypeRef context_destroy_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef context_destroy_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), context_destroy_parameters,
        1, 0);
    backend->runtime_context_destroy = LLVMAddFunction(backend->module,
        "om_runtime_context_destroy", context_destroy_type);
    LLVMTypeRef set_source_spans_parameters[3] = {
        backend->byte_pointer_type,
        LLVMPointerType(backend->runtime_source_span_type, 0),
        backend->i64_type,
    };
    LLVMTypeRef set_source_spans_type = LLVMFunctionType(backend->i32_type,
        set_source_spans_parameters, 3, 0);
    backend->runtime_set_source_spans = LLVMAddFunction(backend->module,
        "om_runtime_set_source_spans", set_source_spans_type);
    LLVMTypeRef entry_function_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef entry_function_type = LLVMFunctionType(backend->i32_type,
        entry_function_parameters, 1, 0);
    LLVMTypeRef run_parameters[2] = {
        backend->byte_pointer_type, LLVMPointerType(entry_function_type, 0),
    };
    LLVMTypeRef run_type = LLVMFunctionType(backend->i32_type,
        run_parameters, 2, 0);
    backend->runtime_run = LLVMAddFunction(backend->module,
        "om_runtime_run", run_type);
    LLVMTypeRef fault_parameters[3] = {
        backend->byte_pointer_type, backend->i32_type, backend->i32_type,
    };
    LLVMTypeRef fault_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), fault_parameters, 3, 0);
    backend->runtime_fault = LLVMAddFunction(backend->module,
        "om_runtime_fault", fault_type);
    LLVMTypeRef roots_push_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMTypeRef roots_push_type = LLVMFunctionType(backend->byte_pointer_type,
        roots_push_parameters, 3, 0);
    backend->runtime_roots_push = LLVMAddFunction(backend->module,
        "om_runtime_roots_push", roots_push_type);
    LLVMTypeRef roots_pop_parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef roots_pop_type = LLVMFunctionType(backend->i32_type,
        roots_pop_parameters, 2, 0);
    backend->runtime_roots_pop = LLVMAddFunction(backend->module,
        "om_runtime_roots_pop", roots_pop_type);
    LLVMTypeRef native_pointer_release_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef native_pointer_release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        native_pointer_release_parameters, 1, 0);
    LLVMTypeRef native_pointer_release_pointer_type =
        LLVMPointerType(native_pointer_release_type, 0);
    LLVMTypeRef native_pointer_create_parameters[5] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        native_pointer_release_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef native_pointer_create_type = LLVMFunctionType(
        backend->i32_type, native_pointer_create_parameters, 5, 0);
    backend->runtime_native_pointer_create = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_create", native_pointer_create_type);
    LLVMTypeRef native_pointer_access_parameters[3] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef native_pointer_access_type = LLVMFunctionType(
        backend->i32_type, native_pointer_access_parameters, 3, 0);
    backend->runtime_native_pointer_borrow = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_borrow", native_pointer_access_type);
    backend->runtime_native_pointer_take = LLVMAddFunction(backend->module,
        "om_runtime_native_pointer_take", native_pointer_access_type);
    LLVMTypeRef string_create_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_create_type = LLVMFunctionType(backend->i32_type,
        string_create_parameters, 4, 0);
    backend->runtime_string_create = LLVMAddFunction(backend->module,
        "om_runtime_string_create_utf8_default", string_create_type);
    LLVMTypeRef runtime_string_cstring_view_parameters[4] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
        LLVMPointerType(LLVMIntPtrTypeForASInContext(backend->context,
            backend->target_data, 0), 0),
    };
    LLVMTypeRef runtime_string_cstring_view_type = LLVMFunctionType(
        backend->i32_type, runtime_string_cstring_view_parameters, 4, 0);
    backend->runtime_string_cstring_view = LLVMAddFunction(backend->module,
        "om_runtime_string_cstring_view",
        runtime_string_cstring_view_type);
    LLVMTypeRef runtime_string_from_cstring_parameters[3] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef runtime_string_from_cstring_type = LLVMFunctionType(
        backend->i32_type, runtime_string_from_cstring_parameters, 3, 0);
    backend->runtime_string_from_cstring = LLVMAddFunction(backend->module,
        "om_runtime_string_create_from_cstring",
        runtime_string_from_cstring_type);
    LLVMTypeRef cstring_release_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef cstring_release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        cstring_release_parameters, 1, 0);
    LLVMTypeRef runtime_string_from_owned_cstring_parameters[4] = {
        backend->byte_pointer_type,
        backend->byte_pointer_type,
        LLVMPointerType(cstring_release_type, 0),
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef runtime_string_from_owned_cstring_type = LLVMFunctionType(
        backend->i32_type, runtime_string_from_owned_cstring_parameters, 4, 0);
    backend->runtime_string_from_owned_cstring = LLVMAddFunction(
        backend->module,
        "om_runtime_string_create_from_owned_cstring",
        runtime_string_from_owned_cstring_type);
    LLVMTypeRef string_format_parameters[4] = {
        backend->byte_pointer_type, backend->i32_type,
        backend->byte_pointer_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_format_type = LLVMFunctionType(backend->i32_type,
        string_format_parameters, 4, 0);
    backend->runtime_string_format = LLVMAddFunction(backend->module,
        "om_runtime_string_format_value", string_format_type);
    LLVMTypeRef string_concat_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type, LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef string_concat_type = LLVMFunctionType(backend->i32_type,
        string_concat_parameters, 4, 0);
    backend->runtime_string_concat = LLVMAddFunction(backend->module,
        "om_runtime_string_concat", string_concat_type);
    LLVMTypeRef print_parameters[2] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
    };
    LLVMTypeRef print_type = LLVMFunctionType(backend->i32_type,
        print_parameters, 2, 0);
    backend->runtime_print = LLVMAddFunction(backend->module,
        "om_runtime_print", print_type);

    LLVMTypeRef descriptor_pointer_type = backend->byte_pointer_type;
    LLVMTypeRef value_pointer_type = backend->byte_pointer_type;
    LLVMTypeRef size_pointer_type = LLVMPointerType(backend->i64_type, 0);
    LLVMTypeRef object_create_parameters[3] = {
        backend->byte_pointer_type, descriptor_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef object_create_type = LLVMFunctionType(backend->i32_type,
        object_create_parameters, 3, 0);
    backend->runtime_record_create = LLVMAddFunction(backend->module,
        "om_runtime_record_create", object_create_type);
    backend->runtime_list_create = LLVMAddFunction(backend->module,
        "om_runtime_list_create", object_create_type);
    backend->runtime_map_create = LLVMAddFunction(backend->module,
        "om_runtime_map_create", object_create_type);
    LLVMTypeRef record_set_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef record_set_type = LLVMFunctionType(backend->i32_type,
        record_set_parameters, 4, 0);
    backend->runtime_record_set = LLVMAddFunction(backend->module,
        "om_runtime_record_set_value", record_set_type);
    LLVMTypeRef record_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef record_get_type = LLVMFunctionType(backend->i32_type,
        record_get_parameters, 4, 0);
    backend->runtime_record_get = LLVMAddFunction(backend->module,
        "om_runtime_record_get", record_get_type);
    LLVMTypeRef list_append_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type,
    };
    LLVMTypeRef list_append_type = LLVMFunctionType(backend->i32_type,
        list_append_parameters, 3, 0);
    backend->runtime_list_append = LLVMAddFunction(backend->module,
        "om_runtime_list_append_value", list_append_type);
    LLVMTypeRef list_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef list_get_type = LLVMFunctionType(backend->i32_type,
        list_get_parameters, 4, 0);
    backend->runtime_list_get = LLVMAddFunction(backend->module,
        "om_runtime_list_get", list_get_type);
    LLVMTypeRef list_length_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        size_pointer_type,
    };
    LLVMTypeRef list_length_type = LLVMFunctionType(backend->i32_type,
        list_length_parameters, 3, 0);
    backend->runtime_list_length = LLVMAddFunction(backend->module,
        "om_runtime_list_length", list_length_type);
    LLVMTypeRef map_set_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type, value_pointer_type,
    };
    LLVMTypeRef map_set_type = LLVMFunctionType(backend->i32_type,
        map_set_parameters, 4, 0);
    backend->runtime_map_set = LLVMAddFunction(backend->module,
        "om_runtime_map_set_values", map_set_type);
    LLVMTypeRef map_get_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        value_pointer_type, value_pointer_type,
    };
    LLVMTypeRef map_get_type = LLVMFunctionType(backend->i32_type,
        map_get_parameters, 4, 0);
    backend->runtime_map_get = LLVMAddFunction(backend->module,
        "om_runtime_map_get_value", map_get_type);
    LLVMTypeRef map_key_at_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, value_pointer_type,
    };
    LLVMTypeRef map_key_at_type = LLVMFunctionType(backend->i32_type,
        map_key_at_parameters, 4, 0);
    backend->runtime_map_key_at = LLVMAddFunction(backend->module,
        "om_runtime_map_key_at", map_key_at_type);
    LLVMTypeRef map_length_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        size_pointer_type,
    };
    LLVMTypeRef map_length_type = LLVMFunctionType(backend->i32_type,
        map_length_parameters, 3, 0);
    backend->runtime_map_length = LLVMAddFunction(backend->module,
        "om_runtime_map_length", map_length_type);
    LLVMTypeRef actor_start_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type,
    };
    LLVMTypeRef actor_start_type = LLVMFunctionType(backend->i32_type,
        actor_start_parameters, 3, 0);
    backend->runtime_actor_system_start = LLVMAddFunction(backend->module,
        "om_runtime_actor_system_start", actor_start_type);
    LLVMTypeRef actor_context_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef actor_context_type = LLVMFunctionType(backend->i32_type,
        actor_context_parameters, 1, 0);
    backend->runtime_actor_system_wait = LLVMAddFunction(backend->module,
        "om_runtime_actor_system_wait", actor_context_type);
    LLVMTypeRef actor_spawn_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
    };
    LLVMTypeRef actor_spawn_type = LLVMFunctionType(backend->i32_type,
        actor_spawn_parameters, 3, 0);
    backend->runtime_actor_spawn_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_spawn_id", actor_spawn_type);
    LLVMTypeRef actor_spawn_at_parameters[4] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->i64_type, LLVMPointerType(backend->i64_type, 0),
    };
    LLVMTypeRef actor_spawn_at_type = LLVMFunctionType(backend->i32_type,
        actor_spawn_at_parameters, 4, 0);
    backend->runtime_actor_spawn_at = LLVMAddFunction(backend->module,
        "om_runtime_actor_spawn_at", actor_spawn_at_type);
    LLVMTypeRef distribution_create_parameters[3] = {
        backend->byte_pointer_type, backend->byte_pointer_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef distribution_create_type = LLVMFunctionType(
        backend->i32_type, distribution_create_parameters, 3, 0);
    backend->runtime_distribution_create = LLVMAddFunction(backend->module,
        "om_runtime_distribution_create", distribution_create_type);
    LLVMTypeRef distribution_destroy_parameters[1] = {
        backend->byte_pointer_type,
    };
    LLVMTypeRef distribution_destroy_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context),
        distribution_destroy_parameters, 1, 0);
    backend->runtime_distribution_destroy = LLVMAddFunction(backend->module,
        "om_runtime_distribution_destroy", distribution_destroy_type);
    LLVMTypeRef actor_id_out_parameters[2] = {
        backend->byte_pointer_type, LLVMPointerType(backend->i64_type, 0),
    };
    LLVMTypeRef actor_id_out_type = LLVMFunctionType(backend->i32_type,
        actor_id_out_parameters, 2, 0);
    backend->runtime_actor_self_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_self_id", actor_id_out_type);
    LLVMTypeRef actor_object_out_parameters[2] = {
        backend->byte_pointer_type,
        LLVMPointerType(backend->byte_pointer_type, 0),
    };
    LLVMTypeRef actor_object_out_type = LLVMFunctionType(backend->i32_type,
        actor_object_out_parameters, 2, 0);
    backend->runtime_actor_state_object = LLVMAddFunction(backend->module,
        "om_runtime_actor_state_object", actor_object_out_type);
    backend->runtime_actor_message_object = LLVMAddFunction(backend->module,
        "om_runtime_actor_message_object", actor_object_out_type);
    LLVMTypeRef actor_send_parameters[3] = {
        backend->byte_pointer_type, backend->i64_type,
        backend->byte_pointer_type,
    };
    LLVMTypeRef actor_send_type = LLVMFunctionType(backend->i32_type,
        actor_send_parameters, 3, 0);
    backend->runtime_actor_send_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_send_id", actor_send_type);
    LLVMTypeRef actor_watch_parameters[2] = {
        backend->byte_pointer_type, backend->i64_type,
    };
    LLVMTypeRef actor_watch_type = LLVMFunctionType(backend->i32_type,
        actor_watch_parameters, 2, 0);
    backend->runtime_actor_watch_id = LLVMAddFunction(backend->module,
        "om_runtime_actor_watch_id", actor_watch_type);
    backend->runtime_actor_stop = LLVMAddFunction(backend->module,
        "om_runtime_actor_stop", actor_context_type);
    LLVMTypeRef empty_callback_signature = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), NULL, 0, 0);
    backend->native_callback_function_pointer_type =
        empty_callback_signature == NULL ? NULL
            : LLVMPointerType(empty_callback_signature, 0);
    LLVMTypeRef callback_set_pointer_type =
        backend->native_callback_function_pointer_type == NULL ? NULL
        : LLVMPointerType(backend->native_callback_function_pointer_type, 0);
    LLVMTypeRef callback_sets_pointer_type = callback_set_pointer_type == NULL
        ? NULL : LLVMPointerType(callback_set_pointer_type, 0);
    LLVMTypeRef callback_count_parameters[6] = {
        backend->byte_pointer_type,
        backend->i64_type,
        callback_sets_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
        LLVMPointerType(backend->byte_pointer_type, 0),
        callback_set_pointer_type,
    };
    LLVMTypeRef callback_register_type = LLVMFunctionType(backend->i32_type,
        callback_count_parameters, 6, 0);
    backend->runtime_native_callback_register_many = callback_register_type
        == NULL ? NULL : LLVMAddFunction(backend->module,
            "om_runtime_native_callback_register_many",
            callback_register_type);
    LLVMTypeRef callback_unregister_parameters[3] = {
        backend->byte_pointer_type, backend->i64_type,
        callback_set_pointer_type,
    };
    LLVMTypeRef callback_unregister_type = LLVMFunctionType(
        backend->i32_type, callback_unregister_parameters, 3, 0);
    backend->runtime_native_callback_unregister_many =
        callback_unregister_type == NULL ? NULL : LLVMAddFunction(
            backend->module,
            "om_runtime_native_callback_unregister_many",
            callback_unregister_type);
    LLVMTypeRef callback_post_parameters[3] = {
        backend->native_callback_function_pointer_type,
        LLVMPointerType(backend->i64_type, 0),
        backend->i64_type,
    };
    LLVMTypeRef callback_post_type = LLVMFunctionType(backend->i32_type,
        callback_post_parameters, 3, 0);
    backend->runtime_native_callback_post = callback_post_type == NULL
        ? NULL : LLVMAddFunction(backend->module,
            "om_runtime_native_callback_post", callback_post_type);
    return backend->runtime_context_create == NULL
        || backend->runtime_context_destroy == NULL
        || backend->runtime_set_source_spans == NULL
        || backend->runtime_run == NULL
        || backend->runtime_fault == NULL
        || backend->runtime_roots_push == NULL
        || backend->runtime_roots_pop == NULL
        || backend->runtime_native_pointer_create == NULL
        || backend->runtime_native_pointer_borrow == NULL
        || backend->runtime_native_pointer_take == NULL
        || backend->runtime_string_create == NULL
        || backend->runtime_string_cstring_view == NULL
        || backend->runtime_string_from_cstring == NULL
        || backend->runtime_string_from_owned_cstring == NULL
        || backend->runtime_string_format == NULL
        || backend->runtime_string_concat == NULL
        || backend->runtime_print == NULL
        || backend->runtime_record_create == NULL
        || backend->runtime_record_set == NULL
        || backend->runtime_record_get == NULL
        || backend->runtime_list_create == NULL
        || backend->runtime_list_append == NULL
        || backend->runtime_list_get == NULL
        || backend->runtime_list_length == NULL
        || backend->runtime_map_create == NULL
        || backend->runtime_map_set == NULL
        || backend->runtime_map_get == NULL
        || backend->runtime_map_key_at == NULL
        || backend->runtime_map_length == NULL
        || backend->runtime_actor_system_start == NULL
        || backend->runtime_actor_system_wait == NULL
        || backend->runtime_actor_spawn_id == NULL
        || backend->runtime_actor_spawn_at == NULL
        || backend->runtime_distribution_create == NULL
        || backend->runtime_distribution_destroy == NULL
        || backend->runtime_actor_self_id == NULL
        || backend->runtime_actor_state_object == NULL
        || backend->runtime_actor_message_object == NULL
        || backend->runtime_actor_send_id == NULL
        || backend->runtime_actor_watch_id == NULL
        || backend->runtime_actor_stop == NULL
        || backend->runtime_native_callback_register_many == NULL
        || backend->runtime_native_callback_unregister_many == NULL
        || backend->runtime_native_callback_post == NULL
        || backend->native_callback_function_pointer_type == NULL
        || backend->runtime_type_descriptor_type == NULL
        || backend->runtime_field_descriptor_type == NULL
        || backend->runtime_source_span_type == NULL
        || backend->runtime_actor_handler_descriptor_type == NULL
        || backend->runtime_actor_descriptor_type == NULL
        || backend->runtime_program_descriptor_type == NULL
        ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status create_function_declarations(Om_Llvm_Backend *backend)
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

static Om_Status create_basic_blocks(Om_Llvm_Backend *backend)
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

static Om_Status create_block_parameters(Om_Llvm_Backend *backend)
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

static LLVMValueRef create_string_constant(Om_Llvm_Backend *backend,
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

static Om_Status create_source_span_table(Om_Llvm_Backend *backend,
                                          LLVMValueRef *table)
{
    if (backend == NULL || table == NULL
        || backend->source_location_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *table = LLVMConstNull(
        LLVMPointerType(backend->runtime_source_span_type, 0));
    if (backend->source_location_count == 0) {
        return OM_STATUS_OK;
    }
    LLVMValueRef *initializers = om_alloc_array(
        backend->source_location_count, sizeof(*initializers));
    if (initializers == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < backend->source_location_count; ++index) {
        const Om_Backend_Source_Location *location =
            &backend->source_locations[index];
        if (!om_string_view_is_valid(location->path)
            || location->path.length > UINT_MAX
            || (location->path.length != 0
                && memchr(location->path.data, '\0',
                          location->path.length) != NULL)) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const char *path_bytes = location->path.length == 0
            ? "" : location->path.data;
        LLVMTypeRef path_type = LLVMArrayType2(backend->i8_type,
            (uint64_t)location->path.length + 1);
        LLVMValueRef path_initializer = LLVMConstStringInContext(
            backend->context, path_bytes, (unsigned)location->path.length, 0);
        char path_name[56];
        const int path_name_length = snprintf(path_name, sizeof(path_name),
            ".om_source_path_%" PRIu64, (uint64_t)index);
        if (path_name_length < 0
            || (size_t)path_name_length >= sizeof(path_name)) {
            status = OM_STATUS_OUT_OF_RANGE;
            break;
        }
        LLVMValueRef path_global = LLVMAddGlobal(backend->module, path_type,
                                                  path_name);
        if (path_global == NULL || path_initializer == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMSetInitializer(path_global, path_initializer);
        LLVMSetGlobalConstant(path_global, 1);
        LLVMSetLinkage(path_global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(path_global, LLVMGlobalUnnamedAddr);
        LLVMValueRef path_indices[2] = {
            LLVMConstInt(backend->i32_type, 0, 0),
            LLVMConstInt(backend->i32_type, 0, 0),
        };
        LLVMValueRef path_pointer = LLVMConstInBoundsGEP2(path_type,
            path_global, path_indices, 2);
        LLVMValueRef members[3] = {
            path_pointer,
            LLVMConstInt(backend->i32_type, location->line, 0),
            LLVMConstInt(backend->i32_type, location->column, 0),
        };
        initializers[index] = LLVMConstStructInContext(backend->context,
            members, 3, 0);
        if (path_pointer == NULL || initializers[index] == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        LLVMTypeRef array_type = LLVMArrayType(
            backend->runtime_source_span_type,
            (unsigned)backend->source_location_count);
        LLVMValueRef initializer = LLVMConstArray(
            backend->runtime_source_span_type, initializers,
            (unsigned)backend->source_location_count);
        LLVMValueRef global = LLVMAddGlobal(backend->module, array_type,
                                             ".om_source_spans");
        if (global == NULL || initializer == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            LLVMSetInitializer(global, initializer);
            LLVMSetGlobalConstant(global, 1);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            *table = LLVMConstBitCast(global,
                LLVMPointerType(backend->runtime_source_span_type, 0));
        }
    }
    om_free(initializers);
    return status;
}

static Om_Status add_checked_continuation(Om_Llvm_Backend *backend,
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

static Om_Status emit_checked_integer(Om_Llvm_Backend *backend,
                                      Om_Mir_Block_Id block_id,
                                      const Om_Mir_Instruction *instruction,
                                      LLVMValueRef left,
                                      LLVMValueRef right,
                                      const char *intrinsic_name,
                                      bool negate,
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
    const unsigned argument_count = negate ? 1u : 2u;
    LLVMValueRef pair = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(intrinsic), intrinsic, arguments,
        argument_count, "checked_pair");
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

static Om_Status emit_integer_division(Om_Llvm_Backend *backend,
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

static LLVMValueRef context_for_function(const Om_Llvm_Backend *backend,
                                         const Om_Mir_Function *function)
{
    if (backend == NULL || function == NULL
        || function->id.value >= backend->project->function_count) {
        return NULL;
    }
    return LLVMGetParam(backend->functions[function->id.value], 0);
}

static Om_Status emit_runtime_status_check(Om_Llvm_Backend *backend,
                                           Om_Mir_Block_Id block_id,
                                           LLVMValueRef status_value)
{
    if (status_value == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef zero = LLVMConstInt(backend->i32_type, 0, 0);
    LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntNE,
        status_value, zero, "runtime_call_failed");
    if (failed == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = backend->current_instruction == NULL
        ? UINT32_MAX : backend->current_instruction->id.value;
    return add_checked_continuation(backend, block_id, failed,
        OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED, span_id);
}

static Om_Status emit_runtime_status_check_as(
    Om_Llvm_Backend *backend,
    Om_Mir_Block_Id block_id,
    LLVMValueRef status_value,
    uint32_t fault_code
)
{
    if (status_value == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef zero = LLVMConstInt(backend->i32_type, 0, 0);
    LLVMValueRef failed = LLVMBuildICmp(backend->builder, LLVMIntNE,
        status_value, zero, "runtime_call_failed");
    if (failed == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const uint32_t span_id = backend->current_instruction == NULL
        ? UINT32_MAX : backend->current_instruction->id.value;
    return add_checked_continuation(backend, block_id, failed, fault_code,
                                    span_id);
}

static Om_Status emit_runtime_object_call(Om_Llvm_Backend *backend,
                                          const Om_Mir_Function *function,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef callee,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count,
                                          LLVMValueRef *result)
{
    LLVMValueRef result_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "runtime_object_result_slot");
    if (result_slot == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), result_slot);
    LLVMValueRef context = context_for_function(backend, function);
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    arguments[0] = context;
    arguments[argument_count - 1] = result_slot;
    LLVMValueRef call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(callee), callee, arguments, argument_count,
        "runtime_object_status");
    Om_Status status = emit_runtime_status_check(backend, block_id, call);
    if (status == OM_STATUS_OK) {
        *result = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, result_slot, "runtime_object_result");
        if (*result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

static Om_Status runtime_value_kind(Om_Type_Kind kind, uint32_t *runtime_kind)
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

static Om_Status descriptor_fingerprint(Om_Llvm_Backend *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        uint64_t *fingerprint);
static Om_Status actor_protocol_fingerprint(
    Om_Llvm_Backend *backend,
    const Om_Mir_Actor_Descriptor *actor,
    uint64_t *fingerprint);

static Om_Status descriptor_cache_add(Om_Llvm_Backend *backend,
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

static const Om_Mir_Nominal_Descriptor *nominal_for_type(
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
    case OM_TYPE_DOWN_REASON: return (Om_String_View){"DownReason", 10};
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

typedef struct Om_Llvm_Fingerprint_Stack {
    Om_Type_Id types[65];
    size_t count;
} Om_Llvm_Fingerprint_Stack;

static Om_Status compute_descriptor_fingerprint(
    Om_Llvm_Backend *backend,
    Om_Type_Id type_id,
    unsigned depth,
    Om_Llvm_Fingerprint_Stack *stack,
    uint64_t *fingerprint)
{
    static const char general_domain[] = "OMBASIC.Type.v1";
    static const char record_domain[] = "OMBASIC.Record.v1";
    static const char message_domain[] = "OMBASIC.Message.v1";
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
        if (fields != NULL) {
            child_type = fields[index].type_id;
        } else {
            status = om_type_store_child_at(backend->project->type_store,
                resolved_id, index, &child_type);
        }
        if (status == OM_STATUS_OK) {
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

static Om_Status descriptor_fingerprint(Om_Llvm_Backend *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        uint64_t *fingerprint)
{
    Om_Llvm_Fingerprint_Stack stack = {0};
    return compute_descriptor_fingerprint(backend, type_id, depth, &stack,
                                         fingerprint);
}

static Om_Status nominal_descriptor_fingerprint(
    Om_Llvm_Backend *backend,
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
    Om_Llvm_Backend *backend,
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

static Om_Status runtime_descriptor(Om_Llvm_Backend *backend,
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

static Om_Status callback_status_check(Om_Llvm_Backend *backend,
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

static const Om_Mir_Function *mir_function_for_instance(
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

static Om_Status create_actor_state_initializer(
    Om_Llvm_Backend *backend,
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

static Om_Status create_actor_handler_wrapper(
    Om_Llvm_Backend *backend,
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

static LLVMValueRef create_private_string(Om_Llvm_Backend *backend,
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

static Om_Status actor_canonical_name_text(
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

static Om_Status create_actor_runtime_descriptors(Om_Llvm_Backend *backend)
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

static Om_Status actor_down_layout(Om_Llvm_Backend *backend,
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

static Om_Status program_fingerprints(Om_Llvm_Backend *backend,
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

static Om_Status create_actor_program_descriptor(
    Om_Llvm_Backend *backend,
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

static bool is_directly_managed_type(const Om_Llvm_Backend *backend,
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

static Om_Status managed_root_slot_count(Om_Llvm_Backend *backend,
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

static LLVMValueRef root_slot_address(Om_Llvm_Backend *backend,
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
    Om_Llvm_Backend *backend,
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

static Om_Status store_value_root(Om_Llvm_Backend *backend,
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

static Om_Status initialize_function_roots(Om_Llvm_Backend *backend)
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

static Om_Status create_runtime_value_storage(Om_Llvm_Backend *backend,
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

static Om_Status allocate_runtime_value_storage(Om_Llvm_Backend *backend,
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

static Om_Status load_runtime_value_storage(Om_Llvm_Backend *backend,
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

static Om_Status get_mir_value_storage(Om_Llvm_Backend *backend,
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

static Om_Status emit_runtime_status_call(Om_Llvm_Backend *backend,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef function,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count)
{
    LLVMValueRef call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(function), function, arguments, argument_count,
        "runtime_operation_status");
    return emit_runtime_status_check(backend, block_id, call);
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

typedef struct Om_Llvm_Native_Abi_Type {
    LLVMTypeRef llvm_type;
    unsigned bit_width;
    bool is_integer;
    bool is_signed;
    bool is_float;
    bool is_boolean;
    bool is_pointer;
} Om_Llvm_Native_Abi_Type;

static bool native_target_uses_windows_long(const Om_Llvm_Backend *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_WIN64_X64;
}

static Om_Status native_abi_type(Om_Llvm_Backend *backend,
                                 Om_Native_Abi_Type abi_type,
                                 bool is_result,
                                 Om_Llvm_Native_Abi_Type *result)
{
    if (backend == NULL || backend->target_data == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned pointer_bytes = LLVMPointerSize(backend->target_data);
    if (pointer_bytes == 0 || pointer_bytes > UINT_MAX / CHAR_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned pointer_bits = pointer_bytes * CHAR_BIT;
    unsigned bits = 0;
    bool is_integer = true;
    bool is_signed = false;
    bool is_float = false;
    bool is_boolean = false;
    switch (abi_type) {
    case OM_NATIVE_ABI_CVOID:
        if (!is_result) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *result = (Om_Llvm_Native_Abi_Type){
            .llvm_type = LLVMVoidTypeInContext(backend->context),
        };
        return OM_STATUS_OK;
    case OM_NATIVE_ABI_CINT8: bits = 8; is_signed = true; break;
    case OM_NATIVE_ABI_CINT16: bits = 16; is_signed = true; break;
    case OM_NATIVE_ABI_CINT32: bits = 32; is_signed = true; break;
    case OM_NATIVE_ABI_CINT64: bits = 64; is_signed = true; break;
    case OM_NATIVE_ABI_CUINT8: bits = 8; break;
    case OM_NATIVE_ABI_CUINT16: bits = 16; break;
    case OM_NATIVE_ABI_CUINT32: bits = 32; break;
    case OM_NATIVE_ABI_CUINT64: bits = 64; break;
    case OM_NATIVE_ABI_CINT: bits = 32; is_signed = true; break;
    case OM_NATIVE_ABI_CUINT: bits = 32; break;
    case OM_NATIVE_ABI_CLONG:
        bits = native_target_uses_windows_long(backend) ? 32 : pointer_bits;
        is_signed = true;
        break;
    case OM_NATIVE_ABI_CULONG:
        bits = native_target_uses_windows_long(backend) ? 32 : pointer_bits;
        break;
    case OM_NATIVE_ABI_CLONGLONG: bits = 64; is_signed = true; break;
    case OM_NATIVE_ABI_CULONGLONG: bits = 64; break;
    case OM_NATIVE_ABI_CSIZE: bits = pointer_bits; break;
    case OM_NATIVE_ABI_CBOOL:
        bits = 1;
        is_boolean = true;
        break;
    case OM_NATIVE_ABI_CFLOAT:
        bits = 32;
        is_integer = false;
        is_float = true;
        break;
    case OM_NATIVE_ABI_CDOUBLE:
        bits = 64;
        is_integer = false;
        is_float = true;
        break;
    case OM_NATIVE_ABI_CSTRING:
    case OM_NATIVE_ABI_CPOINTER:
        *result = (Om_Llvm_Native_Abi_Type){
            .llvm_type = backend->byte_pointer_type,
            .bit_width = pointer_bits,
            .is_pointer = true,
        };
        return OM_STATUS_OK;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_RECORD:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef llvm_type = NULL;
    if (is_float) {
        llvm_type = bits == 32
            ? LLVMFloatTypeInContext(backend->context)
            : LLVMDoubleTypeInContext(backend->context);
    } else if (bits == 1) {
        llvm_type = backend->i1_type;
    } else if (bits == 8) {
        llvm_type = backend->i8_type;
    } else if (bits == 32) {
        llvm_type = backend->i32_type;
    } else if (bits == 64) {
        llvm_type = backend->i64_type;
    } else if (bits != 0) {
        llvm_type = LLVMIntTypeInContext(backend->context, bits);
    }
    if (llvm_type == NULL || bits == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Llvm_Native_Abi_Type){
        .llvm_type = llvm_type,
        .bit_width = bits,
        .is_integer = is_integer,
        .is_signed = is_signed,
        .is_float = is_float,
        .is_boolean = is_boolean,
    };
    return OM_STATUS_OK;
}

static Om_Status native_record_llvm_type(Om_Llvm_Backend *backend,
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

static bool native_target_is_windows_x64(const Om_Llvm_Backend *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_WIN64_X64;
}

static bool native_target_is_sysv_amd64(const Om_Llvm_Backend *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_SYSV_AMD64;
}

static bool native_target_is_darwin_arm64(const Om_Llvm_Backend *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_DARWIN_ARM64;
}

static bool native_target_is_aapcs64(const Om_Llvm_Backend *backend)
{
    return backend != NULL
        && backend->target.c_abi == OM_TARGET_C_ABI_AAPCS64;
}

static Om_Status native_record_abi(Om_Llvm_Backend *backend,
                                   const Om_Omi_Module *module,
                                   size_t record_index,
                                   Om_Llvm_Native_Record_Abi *abi)
{
    if (backend == NULL || module == NULL || abi == NULL
        || record_index >= module->record_count || module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef record_type = NULL;
    Om_Status status = native_record_llvm_type(backend, module, record_index,
                                                &record_type);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const uint64_t size = LLVMABISizeOfType(backend->target_data, record_type);
    const uint64_t alignment = LLVMABIAlignmentOfType(backend->target_data,
                                                       record_type);
    if (size == 0 || alignment == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (native_target_is_windows_x64(backend)) {
        return om_llvm_native_record_classify_win64(backend->context,
            record_type, size, alignment, abi);
    }
    const bool is_sysv_amd64 = native_target_is_sysv_amd64(backend);
    const bool is_aapcs64 = native_target_is_aapcs64(backend);
    const bool is_darwin_arm64 = native_target_is_darwin_arm64(backend);
    if (!is_sysv_amd64 && !is_aapcs64 && !is_darwin_arm64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Record *record = &module->records[record_index];
    if (record->field_start > module->record_field_count
        || record->field_count
            > module->record_field_count - record->field_start
        || record->field_count == 0 || module->record_fields == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Llvm_Native_Record_Field_Layout *fields = om_alloc_array(
        record->field_count, sizeof(*fields));
    if (fields == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < record->field_count; ++index) {
        const Om_Omi_Record_Field *field = &module->record_fields[
            record->field_start + index];
        Om_Llvm_Native_Abi_Type field_abi = {0};
        if (field->abi_type == OM_NATIVE_ABI_CBOOL) {
            field_abi.llvm_type = backend->i8_type;
            field_abi.bit_width = 8;
            field_abi.is_integer = true;
        } else {
            status = native_abi_type(backend, field->abi_type, false,
                                     &field_abi);
        }
        if (status == OM_STATUS_OK) {
            fields[index] = (Om_Llvm_Native_Record_Field_Layout){
                .llvm_type = field_abi.llvm_type,
                .offset = LLVMOffsetOfElement(backend->target_data,
                    record_type, (unsigned)index),
                .size = LLVMABISizeOfType(backend->target_data,
                                           field_abi.llvm_type),
                .alignment = LLVMABIAlignmentOfType(backend->target_data,
                                                      field_abi.llvm_type),
                .bit_width = field_abi.bit_width,
                .is_integer = field_abi.is_integer,
                .is_signed = field_abi.is_signed,
                .is_float = field_abi.is_float,
                .is_pointer = field_abi.is_pointer,
            };
        }
    }
    if (status == OM_STATUS_OK) {
        if (is_aapcs64) {
            status = om_llvm_native_record_classify_aapcs64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        } else if (is_darwin_arm64) {
            status = om_llvm_native_record_classify_darwin_arm64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        } else {
            status = om_llvm_native_record_classify_sysv_amd64(
                backend->context, record_type, size, alignment, fields,
                record->field_count, abi);
        }
    }
    om_free(fields);
    return status;
}

static Om_Status native_record_return_type(Om_Llvm_Backend *backend,
                                           const Om_Llvm_Native_Record_Abi *abi,
                                           LLVMTypeRef *type)
{
    if (backend == NULL || abi == NULL || type == NULL
        || abi->indirect || abi->part_count == 0 || abi->part_count > 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi->result_type != NULL) {
        *type = abi->result_type;
        return OM_STATUS_OK;
    }
    if (abi->result_is_record) {
        *type = abi->llvm_type;
        return OM_STATUS_OK;
    }
    if (abi->part_count == 1) {
        *type = abi->parts[0].llvm_type;
        return OM_STATUS_OK;
    }
    LLVMTypeRef parts[2] = {
        abi->parts[0].llvm_type,
        abi->parts[1].llvm_type,
    };
    *type = LLVMStructTypeInContext(backend->context, parts, 2, 0);
    return *type == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status add_native_enum_attribute(Om_Llvm_Backend *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           uint64_t value)
{
    if (backend == NULL || name == NULL
        || (function == NULL && call == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(backend->context,
                                                          kind, value);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status add_native_type_attribute(Om_Llvm_Backend *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           LLVMTypeRef type)
{
    if (backend == NULL || name == NULL || type == NULL
        || (function == NULL && call == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateTypeAttribute(backend->context,
                                                          kind, type);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status native_record_part_address(Om_Llvm_Backend *backend,
                                           LLVMValueRef slot,
                                           const Om_Llvm_Native_Record_Part *part,
                                           LLVMValueRef *address)
{
    if (backend == NULL || slot == NULL || part == NULL
        || (part->memory_type == NULL && part->llvm_type == NULL)
        || address == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef base = LLVMBuildBitCast(backend->builder, slot,
        backend->byte_pointer_type, "native_record_bytes");
    LLVMTypeRef size_type = LLVMIntPtrTypeInContext(backend->context,
                                                     backend->target_data);
    LLVMValueRef offset = LLVMConstInt(size_type, part->offset, 0);
    LLVMValueRef bytes = base == NULL || size_type == NULL ? NULL
        : LLVMBuildGEP2(backend->builder, backend->i8_type, base, &offset, 1,
                        "native_record_part_bytes");
    LLVMTypeRef memory_type = part->memory_type != NULL
        ? part->memory_type : part->llvm_type;
    LLVMTypeRef pointer_type = LLVMPointerType(memory_type, 0);
    *address = bytes == NULL ? NULL : LLVMBuildBitCast(backend->builder,
        bytes, pointer_type, "native_record_part_address");
    return *address == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

static Om_Status load_native_record_part(Om_Llvm_Backend *backend,
                                         LLVMValueRef slot,
                                         const Om_Llvm_Native_Record_Part *part,
                                         LLVMValueRef *value)
{
    if (backend == NULL || slot == NULL || part == NULL || value == NULL
        || part->llvm_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (part->mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_ARRAY) {
        if (LLVMGetTypeKind(part->llvm_type) != LLVMArrayTypeKind
            || part->memory_size == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMTypeRef element_type = LLVMGetElementType(part->llvm_type);
        if (element_type == NULL || LLVMGetTypeKind(element_type)
                != LLVMIntegerTypeKind) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const unsigned element_bits = LLVMGetIntTypeWidth(element_type);
        const uint64_t element_bytes = element_bits / CHAR_BIT;
        const unsigned element_count = LLVMGetArrayLength(part->llvm_type);
        if (element_bytes == 0 || element_count == 0
            || part->memory_size > element_bytes * element_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        LLVMValueRef aggregate = LLVMConstNull(part->llvm_type);
        if (aggregate == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        uint64_t consumed = 0;
        for (unsigned index = 0; index < element_count; ++index) {
            const uint64_t remaining = part->memory_size - consumed;
            if (remaining == 0) {
                break;
            }
            const uint64_t chunk_size = remaining < element_bytes
                ? remaining : element_bytes;
            if (chunk_size > UINT_MAX / CHAR_BIT) {
                return OM_STATUS_OUT_OF_RANGE;
            }
            LLVMTypeRef chunk_type = LLVMIntTypeInContext(backend->context,
                (unsigned)(chunk_size * CHAR_BIT));
            if (chunk_type == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            Om_Llvm_Native_Record_Part chunk = *part;
            chunk.llvm_type = chunk_type;
            chunk.memory_type = chunk_type;
            chunk.offset = part->offset + consumed;
            chunk.memory_size = chunk_size;
            chunk.alignment = part->alignment;
            chunk.mode = OM_LLVM_NATIVE_RECORD_PART_DIRECT;
            LLVMValueRef chunk_value = NULL;
            Om_Status status = load_native_record_part(backend, slot, &chunk,
                                                        &chunk_value);
            if (status != OM_STATUS_OK) {
                return status;
            }
            const unsigned chunk_bits =
                LLVMGetIntTypeWidth(LLVMTypeOf(chunk_value));
            LLVMValueRef widened = chunk_value;
            if (chunk_bits < element_bits) {
                widened = LLVMBuildZExt(backend->builder, chunk_value,
                    element_type, "native_record_chunk_extend");
            } else if (chunk_bits > element_bits) {
                widened = LLVMBuildTrunc(backend->builder, chunk_value,
                    element_type, "native_record_chunk_truncate");
            }
            if (widened == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            aggregate = LLVMBuildInsertValue(backend->builder, aggregate,
                widened, index, "native_record_array_part");
            if (aggregate == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            consumed += chunk_size;
        }
        if (consumed != part->memory_size) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *value = aggregate;
        return OM_STATUS_OK;
    }

    LLVMValueRef address = NULL;
    Om_Status status = native_record_part_address(backend, slot, part,
                                                   &address);
    if (status == OM_STATUS_OK) {
        LLVMTypeRef memory_type = part->memory_type != NULL
            ? part->memory_type : part->llvm_type;
        LLVMValueRef loaded = LLVMBuildLoad2(backend->builder, memory_type,
            address,
                                "native_record_abi_part");
        if (loaded == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (part->alignment != 0) {
            LLVMSetAlignment(loaded, part->alignment);
        }
        if (status == OM_STATUS_OK
            && part->mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND
            && LLVMTypeOf(loaded) != part->llvm_type) {
            if (LLVMGetTypeKind(LLVMTypeOf(loaded)) != LLVMIntegerTypeKind
                || LLVMGetTypeKind(part->llvm_type) != LLVMIntegerTypeKind) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                const unsigned source_bits =
                    LLVMGetIntTypeWidth(LLVMTypeOf(loaded));
                const unsigned target_bits = LLVMGetIntTypeWidth(
                    part->llvm_type);
                *value = source_bits < target_bits
                    ? LLVMBuildZExt(backend->builder, loaded,
                        part->llvm_type, "native_record_abi_extend")
                    : LLVMBuildTrunc(backend->builder, loaded,
                        part->llvm_type, "native_record_abi_truncate");
                if (*value == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
        } else if (status == OM_STATUS_OK) {
            if (LLVMTypeOf(loaded) != part->llvm_type) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                *value = loaded;
            }
        }
    }
    return status;
}

static Om_Status store_native_record_part(Om_Llvm_Backend *backend,
                                          LLVMValueRef slot,
                                          const Om_Llvm_Native_Record_Part *part,
                                          LLVMValueRef value)
{
    if (backend == NULL || slot == NULL || part == NULL
        || part->llvm_type == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef memory_type = part->memory_type != NULL
        ? part->memory_type : part->llvm_type;
    if (memory_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (LLVMTypeOf(value) != memory_type) {
        if (part->mode != OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(memory_type) != LLVMIntegerTypeKind) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(value));
        const unsigned target_bits = LLVMGetIntTypeWidth(memory_type);
        value = source_bits > target_bits
            ? LLVMBuildTrunc(backend->builder, value, memory_type,
                             "native_record_result_truncate")
            : LLVMBuildZExt(backend->builder, value, memory_type,
                            "native_record_result_extend");
        if (value == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef address = NULL;
    Om_Status status = native_record_part_address(backend, slot, part,
                                                   &address);
    if (status == OM_STATUS_OK) {
        LLVMValueRef store = LLVMBuildStore(backend->builder, value, address);
        if (store == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (part->alignment != 0) {
            LLVMSetAlignment(store, part->alignment);
        }
    }
    return status;
}

static Om_Status add_native_integer_extension_attribute(
    Om_Llvm_Backend *backend,
    LLVMValueRef function,
    LLVMValueRef call,
    LLVMAttributeIndex index,
    const Om_Llvm_Native_Abi_Type *abi_type)
{
    if (abi_type == NULL || !abi_type->is_integer
        || abi_type->bit_width >= 32) {
        return OM_STATUS_OK;
    }
    const char *name = abi_type->is_signed ? "signext" : "zeroext";
    const unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    if (kind == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(backend->context,
                                                          kind, 0);
    if (attribute == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (function != NULL) {
        LLVMAddAttributeAtIndex(function, index, attribute);
    }
    if (call != NULL) {
        LLVMAddCallSiteAttribute(call, index, attribute);
    }
    return OM_STATUS_OK;
}

static Om_Status native_callback_emit_trampoline(
    Om_Llvm_Backend *backend,
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
    Om_Llvm_Backend *backend,
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

static bool native_metadata_name_equal(Om_String_View left,
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
    Om_Llvm_Backend *backend,
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
    Om_Llvm_Backend *backend,
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

static Om_Status emit_native_callback_registration(
    Om_Llvm_Backend *backend,
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

static void native_callback_call_destroy(Om_Llvm_Native_Callback_Call *call)
{
    if (call == NULL) return;
    om_free(call->binding_for_parameter);
    om_free(call->thunk_sets);
    om_free(call->message_descriptors);
    *call = (Om_Llvm_Native_Callback_Call){0};
}

static Om_Status native_callback_call_prepare(
    Om_Llvm_Backend *backend,
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

static Om_Status declare_native_external(Om_Llvm_Backend *backend,
                                         Om_String_View symbol,
                                         LLVMTypeRef signature,
                                         LLVMValueRef *function_out)
{
    if (backend == NULL || !om_string_view_is_valid(symbol)
        || symbol.length == 0 || symbol.length == SIZE_MAX
        || memchr(symbol.data, '\0', symbol.length) != NULL
        || signature == NULL || function_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *function_out = NULL;
    char *symbol_name = om_alloc(symbol.length + 1);
    if (symbol_name == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(symbol_name, symbol.data, symbol.length);
    symbol_name[symbol.length] = '\0';
    LLVMValueRef function = LLVMGetNamedFunctionWithLength(backend->module,
        symbol_name, symbol.length);
    Om_Status status = OM_STATUS_OK;
    if (function != NULL
        && LLVMGlobalGetValueType(function) != signature) {
        status = OM_STATUS_INVALID_ARGUMENT;
    } else if (function == NULL) {
        function = LLVMAddFunction(backend->module, symbol_name, signature);
        if (function == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        *function_out = function;
    }
    om_free(symbol_name);
    return status;
}

static Om_Status native_release_function(
    Om_Llvm_Backend *backend,
    const Om_Omi_Module *module,
    Om_String_View release_name,
    LLVMValueRef *release_function_out)
{
    if (backend == NULL || module == NULL || release_function_out == NULL
        || module->functions == NULL
        || !om_string_view_is_valid(release_name)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *release_function_out = NULL;
    const Om_Omi_Native_Function *release = NULL;
    for (size_t index = 0; index < module->function_count; ++index) {
        if (native_metadata_name_equal(module->functions[index].name,
                                       release_name)) {
            release = &module->functions[index];
            break;
        }
    }
    if (release == NULL || release->result_type != OM_NATIVE_ABI_CVOID
        || release->parameter_count != 1
        || release->parameter_start > module->parameter_count
        || release->parameter_count
            > module->parameter_count - release->parameter_start
        || module->parameters == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Parameter *parameter =
        &module->parameters[release->parameter_start];
    if (parameter->is_callback
        || (parameter->abi_type != OM_NATIVE_ABI_CPOINTER
            && parameter->abi_type != OM_NATIVE_ABI_CSTRING)
        || parameter->ownership != OM_NATIVE_OWNERSHIP_OWNED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), parameters, 1, 0);
    return declare_native_external(backend, release->symbol, signature,
                                   release_function_out);
}

static Om_Status native_pointer_result(
    Om_Llvm_Backend *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *native_function,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef native_result,
    LLVMValueRef *managed_result)
{
    if (backend == NULL || function == NULL || module == NULL
        || native_function == NULL || native_result == NULL
        || managed_result == NULL
        || native_function->result_type != OM_NATIVE_ABI_CPOINTER
        || native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && (module->parameters == NULL || operands == NULL))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Ownership ownership = native_function->result_ownership;
    if (ownership != OM_NATIVE_OWNERSHIP_BORROWED
        && ownership != OM_NATIVE_OWNERSHIP_STATIC
        && ownership != OM_NATIVE_OWNERSHIP_OWNED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    LLVMTypeRef release_parameters[1] = {backend->byte_pointer_type};
    LLVMTypeRef release_type = LLVMFunctionType(
        LLVMVoidTypeInContext(backend->context), release_parameters, 1, 0);
    LLVMTypeRef release_pointer_type = LLVMPointerType(release_type, 0);
    LLVMValueRef release_function = LLVMConstNull(release_pointer_type);
    if (ownership == OM_NATIVE_OWNERSHIP_OWNED) {
        Om_Status status = native_release_function(backend, module,
            native_function->release_function, &release_function);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }

    LLVMValueRef owner = LLVMConstNull(backend->byte_pointer_type);
    if (ownership == OM_NATIVE_OWNERSHIP_BORROWED) {
        size_t owner_index = SIZE_MAX;
        for (size_t index = 0; index < native_function->parameter_count;
             ++index) {
            const Om_Omi_Parameter *parameter = &module->parameters[
                native_function->parameter_start + index];
            if (native_metadata_name_equal(parameter->name,
                    native_function->result_owner_parameter)) {
                owner_index = index;
                break;
            }
        }
        if (owner_index == SIZE_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Omi_Parameter *owner_parameter = &module->parameters[
            native_function->parameter_start + owner_index];
        if (owner_parameter->is_callback
            || owner_parameter->abi_type != OM_NATIVE_ABI_CPOINTER
            || owner_parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        size_t source_owner_index = 0;
        for (size_t index = 0; index < owner_index; ++index) {
            if (!module->parameters[native_function->parameter_start + index]
                    .is_callback) {
                ++source_owner_index;
            }
        }
        owner = operands == NULL ? NULL
            : value_at(backend, operands[source_owner_index]);
        if (owner == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    LLVMValueRef context = context_for_function(backend, function);
    LLVMValueRef result_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_pointer_result_slot");
    if (context == NULL || result_slot == NULL) {
        return context == NULL ? OM_STATUS_INVALID_ARGUMENT
                               : OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), result_slot);
    LLVMValueRef arguments[5] = {
        context, native_result, release_function, owner, result_slot,
    };
    LLVMValueRef create_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_native_pointer_create),
        backend->runtime_native_pointer_create, arguments, 5,
        "native_pointer_create_status");
    Om_Status status = emit_runtime_status_check(backend, block_id,
                                                 create_status);
    if (status == OM_STATUS_OK) {
        *managed_result = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, result_slot,
            "native_managed_pointer_result");
        if (*managed_result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

static Om_Status native_cstring_call_argument(
    Om_Llvm_Backend *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || function == NULL || parameter == NULL
        || value == NULL || argument == NULL
        || parameter->abi_type != OM_NATIVE_ABI_CSTRING
        || parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef size_type = LLVMIntPtrTypeInContext(backend->context,
                                                     backend->target_data);
    LLVMValueRef bytes_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_cstring_bytes_slot");
    LLVMValueRef length_slot = entry_alloca(backend, function,
        size_type, "native_cstring_length_slot");
    if (bytes_slot == NULL || length_slot == NULL || size_type == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), bytes_slot);
    LLVMBuildStore(backend->builder, LLVMConstInt(size_type, 0, 0),
                   length_slot);
    LLVMValueRef view_arguments[4] = {
        context_for_function(backend, function),
        value,
        bytes_slot,
        length_slot,
    };
    if (view_arguments[0] == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef view_call = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(backend->runtime_string_cstring_view),
        backend->runtime_string_cstring_view, view_arguments, 4,
        "native_cstring_view_status");
    Om_Status status = emit_runtime_status_check(backend, block_id, view_call);
    if (status != OM_STATUS_OK) {
        return status;
    }
    LLVMValueRef bytes = LLVMBuildLoad2(backend->builder,
        backend->byte_pointer_type, bytes_slot, "native_cstring_source_bytes");
    LLVMValueRef length = LLVMBuildLoad2(backend->builder, size_type,
        length_slot, "native_cstring_source_length");
    LLVMValueRef capacity = LLVMBuildAdd(backend->builder, length,
        LLVMConstInt(size_type, 1, 0), "native_cstring_capacity");
    LLVMValueRef stack_cstring = LLVMBuildArrayAlloca(backend->builder,
        backend->i8_type, capacity, "native_cstring_argument");
    if (bytes == NULL || length == NULL || capacity == NULL
        || stack_cstring == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef copy = LLVMBuildMemCpy(backend->builder,
        stack_cstring, 1, bytes, 1, length);
    LLVMValueRef terminator = LLVMBuildGEP2(backend->builder,
        backend->i8_type, stack_cstring, &length, 1,
        "native_cstring_terminator");
    if (copy == NULL || terminator == NULL
        || LLVMBuildStore(backend->builder,
            LLVMConstInt(backend->i8_type, 0, 0), terminator) == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *argument = stack_cstring;
    return OM_STATUS_OK;
}

static Om_Status native_pointer_call_argument(
    Om_Llvm_Backend *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || function == NULL || parameter == NULL
        || value == NULL || argument == NULL
        || parameter->abi_type != OM_NATIVE_ABI_CPOINTER
        || (parameter->ownership != OM_NATIVE_OWNERSHIP_BORROWED
            && parameter->ownership != OM_NATIVE_OWNERSHIP_STATIC
            && parameter->ownership != OM_NATIVE_OWNERSHIP_OWNED)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMValueRef raw_pointer_slot = entry_alloca(backend, function,
        backend->byte_pointer_type, "native_pointer_argument_slot");
    LLVMValueRef context = context_for_function(backend, function);
    if (raw_pointer_slot == NULL || context == NULL) {
        return raw_pointer_slot == NULL ? OM_STATUS_OUT_OF_MEMORY
                                        : OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMBuildStore(backend->builder,
        LLVMConstNull(backend->byte_pointer_type), raw_pointer_slot);
    LLVMValueRef arguments[3] = {context, value, raw_pointer_slot};
    LLVMValueRef access_function =
        parameter->ownership == OM_NATIVE_OWNERSHIP_OWNED
            ? backend->runtime_native_pointer_take
            : backend->runtime_native_pointer_borrow;
    LLVMValueRef access_status = LLVMBuildCall2(backend->builder,
        LLVMGlobalGetValueType(access_function), access_function,
        arguments, 3, "native_pointer_access_status");
    Om_Status status = emit_runtime_status_check(backend, block_id,
                                                 access_status);
    if (status == OM_STATUS_OK) {
        *argument = LLVMBuildLoad2(backend->builder,
            backend->byte_pointer_type, raw_pointer_slot,
            "native_pointer_argument");
        if (*argument == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return status;
}

static Om_Status native_scalar_call_argument(
    Om_Llvm_Backend *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Omi_Parameter *parameter,
    const Om_Llvm_Native_Abi_Type *abi_type,
    LLVMValueRef value,
    LLVMValueRef *argument)
{
    if (backend == NULL || parameter == NULL || abi_type == NULL
        || value == NULL || argument == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi_type->is_pointer) {
        if (parameter->abi_type == OM_NATIVE_ABI_CSTRING) {
            return native_cstring_call_argument(backend, function, block_id,
                                                parameter, value, argument);
        }
        if (parameter->abi_type == OM_NATIVE_ABI_CPOINTER) {
            return native_pointer_call_argument(backend, function, block_id,
                                                parameter, value, argument);
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (abi_type->is_float && abi_type->bit_width == 32) {
        *argument = LLVMBuildFPTrunc(backend->builder, value,
            abi_type->llvm_type, "native_float_argument");
    } else if (abi_type->is_integer && !abi_type->is_boolean
               && abi_type->bit_width < 64) {
        *argument = LLVMBuildTrunc(backend->builder, value,
            abi_type->llvm_type, "native_integer_argument");
    } else {
        *argument = value;
    }
    return *argument == NULL ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
}

typedef struct Om_Llvm_Native_Call_Parameter {
    Om_Llvm_Native_Abi_Type scalar;
    Om_Llvm_Native_Record_Abi record;
    size_t abi_start;
    size_t abi_count;
    bool is_record;
} Om_Llvm_Native_Call_Parameter;

static Om_Status emit_native_record_call(
    Om_Llvm_Backend *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Instruction *instruction,
    const Om_Mir_Value_Id *operands,
    LLVMValueRef *result,
    const Om_Symbol *symbol,
    const Om_Omi_Native_Function *native_function)
{
    const Om_Omi_Module *module = symbol->native_module;
    if (module == NULL || native_function == NULL || result == NULL
        || native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && module->parameters == NULL)
        || native_function->parameter_count != instruction->operand_count
        || (native_function->parameter_count != 0 && operands == NULL)
        || native_function->parameter_count > UINT_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        if (module->parameters[native_function->parameter_start + index]
                .is_callback) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    const bool result_is_record =
        native_function->result_type == OM_NATIVE_ABI_RECORD;
    Om_Llvm_Native_Record_Abi result_record = {0};
    Om_Llvm_Native_Abi_Type result_scalar = {0};
    Om_Status status = result_is_record
        ? native_record_abi(backend, module,
              native_function->result_record_index, &result_record)
        : native_abi_type(backend, native_function->result_type, true,
                          &result_scalar);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Llvm_Native_Call_Parameter *parameters =
        native_function->parameter_count == 0 ? NULL
        : om_alloc_array(native_function->parameter_count,
                          sizeof(*parameters));
    if (native_function->parameter_count != 0 && parameters == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t abi_parameter_count = result_is_record && result_record.indirect
        ? 1 : 0;
    bool needs_stack_restore =
        native_function->result_type == OM_NATIVE_ABI_CSTRING;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        plan->is_record = parameter->abi_type == OM_NATIVE_ABI_RECORD;
        if (plan->is_record) {
            status = native_record_abi(backend, module,
                parameter->record_index, &plan->record);
            plan->abi_count = plan->record.indirect
                ? 1 : plan->record.part_count;
        } else {
            status = native_abi_type(backend, parameter->abi_type, false,
                                     &plan->scalar);
            plan->abi_count = 1;
            needs_stack_restore = needs_stack_restore
                || parameter->abi_type == OM_NATIVE_ABI_CSTRING;
        }
        plan->abi_start = abi_parameter_count;
        if (status == OM_STATUS_OK
            && !om_checked_size_add(abi_parameter_count, plan->abi_count,
                                    &abi_parameter_count)) {
            status = OM_STATUS_OVERFLOW;
        }
    }
    if (status == OM_STATUS_OK && abi_parameter_count > UINT_MAX) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    LLVMTypeRef return_type = NULL;
    if (status == OM_STATUS_OK) {
        if (result_is_record && result_record.indirect) {
            return_type = LLVMVoidTypeInContext(backend->context);
        } else if (result_is_record) {
            status = native_record_return_type(backend, &result_record,
                                               &return_type);
        } else {
            return_type = result_scalar.llvm_type;
        }
    }
    LLVMTypeRef *parameter_types = abi_parameter_count == 0 ? NULL
        : om_alloc_array(abi_parameter_count, sizeof(*parameter_types));
    LLVMValueRef *arguments = abi_parameter_count == 0 ? NULL
        : om_alloc_array(abi_parameter_count, sizeof(*arguments));
    if (status == OM_STATUS_OK && abi_parameter_count != 0
        && (parameter_types == NULL || arguments == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    size_t next_type = 0;
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        parameter_types[next_type++] = backend->byte_pointer_type;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        if (plan->is_record && plan->record.indirect) {
            parameter_types[next_type++] = backend->byte_pointer_type;
        } else if (plan->is_record) {
            for (size_t part = 0; part < plan->record.part_count; ++part) {
                parameter_types[next_type++] =
                    plan->record.parts[part].llvm_type;
            }
        } else {
            parameter_types[next_type++] = plan->scalar.llvm_type;
        }
    }
    if (status == OM_STATUS_OK && next_type != abi_parameter_count) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    LLVMTypeRef signature = status == OM_STATUS_OK
        ? LLVMFunctionType(return_type, parameter_types,
            (unsigned)abi_parameter_count, 0) : NULL;
    if (status == OM_STATUS_OK && signature == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMValueRef native_function_value = NULL;
    if (status == OM_STATUS_OK) {
        status = declare_native_external(backend, native_function->symbol,
            signature, &native_function_value);
    }
    if (status == OM_STATUS_OK && !result_is_record) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, LLVMAttributeReturnIndex,
            &result_scalar);
    }
    LLVMValueRef result_slot = NULL;
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        result_slot = entry_alloca(backend, function, result_record.llvm_type,
                                   "native_record_sret_slot");
        if (result_slot == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            arguments[0] = result_slot;
            status = add_native_type_attribute(backend,
                native_function_value, NULL, (LLVMAttributeIndex)1,
                "sret", result_record.llvm_type);
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend,
                    native_function_value, NULL, (LLVMAttributeIndex)1,
                    "align", result_record.alignment);
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        const LLVMAttributeIndex first_index = (LLVMAttributeIndex)(
            plan->abi_start + 1);
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (plan->is_record && plan->record.indirect) {
            if (plan->record.byval) {
                status = add_native_type_attribute(backend,
                    native_function_value, NULL, first_index, "byval",
                    plan->record.llvm_type);
            }
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend,
                    native_function_value, NULL, first_index, "align",
                    plan->record.alignment);
            }
        } else if (!plan->is_record) {
            status = add_native_integer_extension_attribute(backend,
                native_function_value, NULL, first_index, &plan->scalar);
        } else {
            for (size_t part = 0; status == OM_STATUS_OK
                 && part < plan->record.part_count; ++part) {
                const Om_Llvm_Native_Record_Part *record_part =
                    &plan->record.parts[part];
                if (record_part->abi_alignment != 0) {
                    status = add_native_enum_attribute(backend,
                        native_function_value, NULL,
                        (LLVMAttributeIndex)(plan->abi_start + part + 1),
                        "alignstack", record_part->abi_alignment);
                }
            }
        }
        (void)parameter;
    }
    LLVMValueRef saved_stack = NULL;
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_save_name[] = "llvm.stacksave";
        const unsigned stack_save_id = LLVMLookupIntrinsicID(
            stack_save_name, sizeof(stack_save_name) - 1);
        LLVMTypeRef stack_save_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_save_function = stack_save_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_save_id,
                stack_save_overload_types, 1);
        if (stack_save_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            saved_stack = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(stack_save_function),
                stack_save_function, NULL, 0, "native_stack_save");
            if (saved_stack == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        LLVMValueRef value = value_at(backend, operands[index]);
        if (value == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const size_t argument_index = plan->abi_start;
        if (plan->is_record) {
            LLVMValueRef slot = entry_alloca(backend, function,
                plan->record.llvm_type, "native_record_argument_slot");
            if (slot == NULL || LLVMBuildStore(backend->builder, value,
                                               slot) == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            if (plan->record.indirect) {
                arguments[argument_index] = slot;
            } else {
                for (size_t part = 0; status == OM_STATUS_OK
                     && part < plan->record.part_count; ++part) {
                    status = load_native_record_part(backend, slot,
                        &plan->record.parts[part],
                        &arguments[argument_index + part]);
                }
            }
        } else {
            const Om_Omi_Parameter *parameter = &module->parameters[
                native_function->parameter_start + index];
            status = native_scalar_call_argument(backend, function, block_id,
                parameter, &plan->scalar, value,
                &arguments[argument_index]);
        }
    }
    LLVMValueRef call = NULL;
    if (status == OM_STATUS_OK) {
        call = LLVMBuildCall2(backend->builder, signature,
            native_function_value, arguments, (unsigned)abi_parameter_count,
            native_function->result_type == OM_NATIVE_ABI_CVOID
                || (result_is_record && result_record.indirect)
                ? "" : "native_call_result");
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && !result_is_record) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            LLVMAttributeReturnIndex, &result_scalar);
    }
    if (status == OM_STATUS_OK && result_is_record && result_record.indirect) {
        status = add_native_type_attribute(backend, NULL, call,
            (LLVMAttributeIndex)1, "sret", result_record.llvm_type);
        if (status == OM_STATUS_OK) {
            status = add_native_enum_attribute(backend, NULL, call,
                (LLVMAttributeIndex)1, "align", result_record.alignment);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        Om_Llvm_Native_Call_Parameter *plan = &parameters[index];
        const LLVMAttributeIndex first_index = (LLVMAttributeIndex)(
            plan->abi_start + 1);
        if (plan->is_record && plan->record.indirect) {
            if (plan->record.byval) {
                status = add_native_type_attribute(backend, NULL, call,
                    first_index, "byval", plan->record.llvm_type);
            }
            if (status == OM_STATUS_OK) {
                status = add_native_enum_attribute(backend, NULL, call,
                    first_index, "align", plan->record.alignment);
            }
        } else if (!plan->is_record) {
            status = add_native_integer_extension_attribute(backend, NULL,
                call, first_index, &plan->scalar);
        } else {
            for (size_t part = 0; status == OM_STATUS_OK
                 && part < plan->record.part_count; ++part) {
                const Om_Llvm_Native_Record_Part *record_part =
                    &plan->record.parts[part];
                if (record_part->abi_alignment != 0) {
                    status = add_native_enum_attribute(backend, NULL, call,
                        (LLVMAttributeIndex)(plan->abi_start + part + 1),
                        "alignstack", record_part->abi_alignment);
                }
            }
        }
    }

    if (status == OM_STATUS_OK && result_is_record) {
        if (result_record.indirect) {
            *result = LLVMBuildLoad2(backend->builder, result_record.llvm_type,
                result_slot, "native_record_sret_result");
        } else if (result_record.result_is_record) {
            *result = call;
        } else {
            result_slot = entry_alloca(backend, function,
                result_record.llvm_type, "native_record_result_slot");
            if (result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Llvm_Native_Record_Part *result_parts =
                    result_record.result_part_count != 0
                        ? result_record.result_parts : result_record.parts;
                const size_t result_part_count =
                    result_record.result_part_count != 0
                        ? result_record.result_part_count
                        : result_record.part_count;
                if (result_part_count == 1) {
                    status = store_native_record_part(backend, result_slot,
                        &result_parts[0], call);
                } else {
                    for (size_t part = 0; status == OM_STATUS_OK
                         && part < result_part_count; ++part) {
                        LLVMValueRef component = LLVMBuildExtractValue(
                            backend->builder, call, (unsigned)part,
                            "native_record_result_part");
                        if (component == NULL) {
                            status = OM_STATUS_OUT_OF_MEMORY;
                        } else {
                            status = store_native_record_part(backend,
                                result_slot, &result_parts[part], component);
                        }
                    }
                }
                if (status == OM_STATUS_OK) {
                    *result = LLVMBuildLoad2(backend->builder,
                        result_record.llvm_type, result_slot,
                        "native_record_result");
                }
            }
        }
        if (status == OM_STATUS_OK && *result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    } else if (status == OM_STATUS_OK) {
        if (native_function->result_type == OM_NATIVE_ABI_CVOID) {
            *result = LLVMConstNull(backend->i8_type);
        } else if (native_function->result_type == OM_NATIVE_ABI_CSTRING) {
            if (native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_BORROWED
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_STATIC
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_OWNED) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                LLVMValueRef managed_result_slot = entry_alloca(backend,
                    function, backend->byte_pointer_type,
                    "native_string_result_slot");
                if (managed_result_slot == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                } else {
                    LLVMBuildStore(backend->builder,
                        LLVMConstNull(backend->byte_pointer_type),
                        managed_result_slot);
                    const bool owned_result = native_function->result_ownership
                        == OM_NATIVE_OWNERSHIP_OWNED;
                    LLVMValueRef string_arguments[4] = {
                        context_for_function(backend, function),
                        call,
                        managed_result_slot,
                        NULL,
                    };
                    LLVMValueRef string_function = NULL;
                    unsigned string_argument_count = 3;
                    if (string_arguments[0] == NULL) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                    } else if (owned_result) {
                        LLVMValueRef release_function_value = NULL;
                        status = native_release_function(backend, module,
                            native_function->release_function,
                            &release_function_value);
                        if (status == OM_STATUS_OK) {
                            string_arguments[2] = release_function_value;
                            string_function =
                                backend->runtime_string_from_owned_cstring;
                            string_argument_count = 4;
                        }
                    } else {
                        string_function =
                            backend->runtime_string_from_cstring;
                    }
                    if (status == OM_STATUS_OK) {
                        LLVMValueRef string_status = LLVMBuildCall2(
                            backend->builder,
                            LLVMGlobalGetValueType(string_function),
                            string_function, string_arguments,
                            string_argument_count,
                            "native_string_conversion_status");
                        status = emit_runtime_status_check(backend, block_id,
                                                           string_status);
                    }
                    if (status == OM_STATUS_OK) {
                        *result = LLVMBuildLoad2(backend->builder,
                            backend->byte_pointer_type, managed_result_slot,
                            "native_managed_string_result");
                    }
                }
            }
        } else if (native_function->result_type == OM_NATIVE_ABI_CPOINTER) {
            status = native_pointer_result(backend, function, block_id,
                module, native_function, operands, call, result);
        } else if (result_scalar.is_float && result_scalar.bit_width == 32) {
            *result = LLVMBuildFPExt(backend->builder, call,
                backend->double_type, "native_float_result");
        } else if (result_scalar.is_integer && !result_scalar.is_boolean
                   && result_scalar.bit_width < 64) {
            *result = result_scalar.is_signed
                ? LLVMBuildSExt(backend->builder, call, backend->i64_type,
                    "native_signed_result")
                : LLVMBuildZExt(backend->builder, call, backend->i64_type,
                    "native_unsigned_result");
        } else {
            *result = call;
        }
        if (status == OM_STATUS_OK && *result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_restore_name[] = "llvm.stackrestore";
        const unsigned stack_restore_id = LLVMLookupIntrinsicID(
            stack_restore_name, sizeof(stack_restore_name) - 1);
        LLVMTypeRef stack_restore_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_restore_function = stack_restore_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_restore_id,
                stack_restore_overload_types, 1);
        if (stack_restore_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            LLVMValueRef restore_arguments[1] = {saved_stack};
            if (LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(stack_restore_function),
                    stack_restore_function, restore_arguments, 1, "") == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    om_free(parameters);
    om_free(parameter_types);
    om_free(arguments);
    return status;
}

static Om_Status emit_native_call(Om_Llvm_Backend *backend,
                                  const Om_Mir_Function *function,
                                  Om_Mir_Block_Id block_id,
                                  const Om_Mir_Instruction *instruction,
                                  const Om_Mir_Value_Id *operands,
                                  LLVMValueRef *result)
{
    if (backend == NULL || instruction == NULL || result == NULL
        || function == NULL
        || backend->project->resolution == NULL
        || instruction->symbol_id.value
            >= backend->project->resolution->symbol_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        backend->project->resolution, instruction->symbol_id);
    if (symbol == NULL || symbol->native_module == NULL
        || symbol->native_function_index
            >= symbol->native_module->function_count
        || symbol->native_module->functions == NULL) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Omi_Module *module = symbol->native_module;
    const Om_Omi_Native_Function *native_function =
        &module->functions[symbol->native_function_index];
    if (native_function->parameter_start > module->parameter_count
        || native_function->parameter_count
            > module->parameter_count - native_function->parameter_start
        || (native_function->parameter_count != 0
            && module->parameters == NULL)
        || native_function->callback_binding_start
            > module->callback_binding_count
        || native_function->callback_binding_count
            > module->callback_binding_count
                - native_function->callback_binding_start
        || (native_function->callback_binding_count != 0
            && module->callback_bindings == NULL)
        || native_function->parameter_count > UINT_MAX
        || (instruction->operand_count != 0 && operands == NULL)
        || native_function->symbol.data == NULL
        || native_function->symbol.length == 0
        || memchr(native_function->symbol.data, '\0',
                  native_function->symbol.length) != NULL
        || native_function->symbol.length == SIZE_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    size_t source_parameter_count = 0;
    size_t declared_callback_count = 0;
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) ++declared_callback_count;
        else ++source_parameter_count;
    }
    if (declared_callback_count != native_function->callback_binding_count
        || source_parameter_count != instruction->operand_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    bool has_native_record = native_function->result_type
        == OM_NATIVE_ABI_RECORD;
    for (size_t index = 0; !has_native_record
         && index < native_function->parameter_count; ++index) {
        has_native_record = module->parameters[
            native_function->parameter_start + index].abi_type
            == OM_NATIVE_ABI_RECORD;
    }
    if (has_native_record) {
        if (declared_callback_count != 0) return OM_STATUS_INVALID_ARGUMENT;
        return emit_native_record_call(backend, function, block_id,
            instruction, operands, result, symbol, native_function);
    }

    Om_Llvm_Native_Callback_Call callback_call = {0};
    Om_Status status = native_callback_call_prepare(backend, module,
        native_function, &callback_call);
    if (status != OM_STATUS_OK) return status;

    bool needs_stack_restore =
        native_function->result_type == OM_NATIVE_ABI_CSTRING;
    for (size_t index = 0; index < native_function->parameter_count; ++index) {
        if (module->parameters[native_function->parameter_start + index]
                .abi_type == OM_NATIVE_ABI_CSTRING) {
            needs_stack_restore = true;
            break;
        }
    }
    LLVMValueRef saved_stack = NULL;
    if (needs_stack_restore) {
        static const char stack_save_name[] = "llvm.stacksave";
        const unsigned stack_save_id = LLVMLookupIntrinsicID(
            stack_save_name, sizeof(stack_save_name) - 1);
        LLVMTypeRef stack_save_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_save_function = stack_save_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_save_id,
                stack_save_overload_types, 1);
        if (stack_save_function == NULL) {
            native_callback_call_destroy(&callback_call);
            return OM_STATUS_NOT_FOUND;
        }
        saved_stack = LLVMBuildCall2(backend->builder,
            LLVMGlobalGetValueType(stack_save_function),
            stack_save_function, NULL, 0, "native_stack_save");
        if (saved_stack == NULL) {
            native_callback_call_destroy(&callback_call);
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }

    Om_Llvm_Native_Abi_Type return_abi = {0};
    status = native_abi_type(backend, native_function->result_type,
                             true, &return_abi);
    Om_Llvm_Native_Abi_Type *parameter_abi = native_function->parameter_count
        == 0 ? NULL : om_alloc_array(native_function->parameter_count,
                                     sizeof(*parameter_abi));
    LLVMTypeRef *parameter_types = native_function->parameter_count == 0
        ? NULL : om_alloc_array(native_function->parameter_count,
                                sizeof(*parameter_types));
    LLVMValueRef *arguments = native_function->parameter_count == 0
        ? NULL : om_alloc_array(native_function->parameter_count,
                                sizeof(*arguments));
    if (status == OM_STATUS_OK && native_function->parameter_count != 0
        && (parameter_abi == NULL || parameter_types == NULL
            || arguments == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) {
            const size_t binding_index =
                callback_call.binding_for_parameter == NULL ? SIZE_MAX
                    : callback_call.binding_for_parameter[index];
            if (binding_index >= callback_call.callback_count
                || callback_call.thunk_sets[binding_index] == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                parameter_abi[index] = (Om_Llvm_Native_Abi_Type){
                    .llvm_type = callback_call.thunk_sets[binding_index]
                        ->function_pointer_type,
                    .bit_width = LLVMPointerSize(backend->target_data)
                        * CHAR_BIT,
                    .is_pointer = true,
                };
                parameter_types[index] = parameter_abi[index].llvm_type;
            }
            continue;
        }
        status = native_abi_type(backend, parameter->abi_type, false,
                                 &parameter_abi[index]);
        if (status == OM_STATUS_OK) {
            parameter_types[index] = parameter_abi[index].llvm_type;
        }
    }
    LLVMTypeRef signature = NULL;
    if (status == OM_STATUS_OK) {
        signature = LLVMFunctionType(return_abi.llvm_type, parameter_types,
            (unsigned)native_function->parameter_count, 0);
        if (signature == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef native_function_value = NULL;
    if (status == OM_STATUS_OK) {
        status = declare_native_external(backend, native_function->symbol,
            signature, &native_function_value);
    }
    if (status == OM_STATUS_OK) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, LLVMAttributeReturnIndex, &return_abi);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        status = add_native_integer_extension_attribute(backend,
            native_function_value, NULL, (LLVMAttributeIndex)(index + 1),
            &parameter_abi[index]);
    }
    size_t source_index = 0;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (parameter->is_callback) continue;
        LLVMValueRef argument = value_at(backend, operands[source_index]);
        if (argument == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Llvm_Native_Abi_Type *abi_type = &parameter_abi[index];
        status = native_scalar_call_argument(backend, function, block_id,
            parameter, abi_type, argument, &arguments[index]);
        ++source_index;
    }
    LLVMValueRef registered_callback_slot = NULL;
    LLVMTypeRef registered_callback_array_type = NULL;
    if (status == OM_STATUS_OK && callback_call.callback_count != 0) {
        status = emit_native_callback_registration(backend, function, block_id,
            instruction, callback_call.callback_count,
            callback_call.thunk_sets, callback_call.message_descriptors,
            &registered_callback_slot, &registered_callback_array_type);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &module->parameters[
            native_function->parameter_start + index];
        if (!parameter->is_callback) continue;
        const size_t binding_index =
            callback_call.binding_for_parameter[index];
        LLVMValueRef indices[2] = {
            LLVMConstInt(backend->i32_type, 0, 0),
            LLVMConstInt(backend->i32_type, binding_index, 0),
        };
        LLVMValueRef slot_address = LLVMBuildGEP2(backend->builder,
            registered_callback_array_type, registered_callback_slot,
            indices, 2, "native_callback_slot_address");
        LLVMValueRef selected = slot_address == NULL ? NULL
            : LLVMBuildLoad2(backend->builder,
                backend->native_callback_function_pointer_type,
                slot_address, "native_callback_selected");
        if (selected == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            arguments[index] = LLVMBuildBitCast(backend->builder, selected,
                parameter_abi[index].llvm_type, "native_callback_argument");
            if (arguments[index] == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef call = NULL;
    if (status == OM_STATUS_OK) {
        call = LLVMBuildCall2(backend->builder, signature,
            native_function_value, arguments,
            (unsigned)native_function->parameter_count,
            native_function->result_type == OM_NATIVE_ABI_CVOID
                ? "" : "native_call_result");
        if (call == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    LLVMValueRef callback_unregister_status = NULL;
    if (status == OM_STATUS_OK && callback_call.callback_count != 0) {
        LLVMValueRef unregister_arguments[3] = {
            context_for_function(backend, function),
            LLVMConstInt(backend->i64_type, callback_call.callback_count, 0),
            LLVMBuildBitCast(backend->builder, registered_callback_slot,
                backend->byte_pointer_type,
                "native_callback_unregister_pointer"),
        };
        if (unregister_arguments[0] == NULL || unregister_arguments[2] == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            callback_unregister_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(
                    backend->runtime_native_callback_unregister_many),
                backend->runtime_native_callback_unregister_many,
                unregister_arguments, 3,
                "native_callback_unregister_status");
            if (callback_unregister_status == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            LLVMAttributeReturnIndex, &return_abi);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < native_function->parameter_count; ++index) {
        status = add_native_integer_extension_attribute(backend, NULL, call,
            (LLVMAttributeIndex)(index + 1), &parameter_abi[index]);
    }
    if (status == OM_STATUS_OK) {
        if (native_function->result_type == OM_NATIVE_ABI_CVOID) {
            *result = LLVMConstNull(backend->i8_type);
        } else if (return_abi.is_pointer
                   && native_function->result_type == OM_NATIVE_ABI_CSTRING) {
            if (native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_BORROWED
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_STATIC
                && native_function->result_ownership
                    != OM_NATIVE_OWNERSHIP_OWNED) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto native_call_cleanup;
            }
            LLVMValueRef managed_result_slot = entry_alloca(backend, function,
                backend->byte_pointer_type,
                "native_string_result_slot");
            if (managed_result_slot == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                goto native_call_cleanup;
            }
            LLVMBuildStore(backend->builder,
                LLVMConstNull(backend->byte_pointer_type),
                managed_result_slot);
            const bool owned_result = native_function->result_ownership
                == OM_NATIVE_OWNERSHIP_OWNED;
            LLVMValueRef string_arguments[4] = {
                context_for_function(backend, function),
                call,
                managed_result_slot,
                NULL,
            };
            LLVMValueRef string_function = NULL;
            unsigned string_argument_count = 3;
            if (string_arguments[0] == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                goto native_call_cleanup;
            }
            if (owned_result) {
                LLVMValueRef release_function_value = NULL;
                status = native_release_function(backend, module,
                    native_function->release_function,
                    &release_function_value);
                if (status != OM_STATUS_OK) {
                    goto native_call_cleanup;
                }
                string_arguments[2] = release_function_value;
                string_arguments[3] = managed_result_slot;
                string_function = backend->runtime_string_from_owned_cstring;
                string_argument_count = 4;
            } else {
                string_function = backend->runtime_string_from_cstring;
            }
            LLVMValueRef string_status = LLVMBuildCall2(backend->builder,
                LLVMGlobalGetValueType(string_function), string_function,
                string_arguments, string_argument_count,
                "native_string_conversion_status");
            status = emit_runtime_status_check(backend, block_id,
                                               string_status);
            if (status != OM_STATUS_OK) {
                goto native_call_cleanup;
            }
            *result = LLVMBuildLoad2(backend->builder,
                backend->byte_pointer_type, managed_result_slot,
                "native_managed_string_result");
        } else if (return_abi.is_pointer
                   && native_function->result_type == OM_NATIVE_ABI_CPOINTER) {
            status = native_pointer_result(backend, function, block_id,
                module, native_function, operands, call, result);
            if (status != OM_STATUS_OK) {
                goto native_call_cleanup;
            }
        } else if (return_abi.is_float && return_abi.bit_width == 32) {
            *result = LLVMBuildFPExt(backend->builder, call,
                backend->double_type, "native_float_result");
        } else if (return_abi.is_integer && !return_abi.is_boolean
                   && return_abi.bit_width < 64) {
            *result = return_abi.is_signed
                ? LLVMBuildSExt(backend->builder, call, backend->i64_type,
                    "native_signed_result")
                : LLVMBuildZExt(backend->builder, call, backend->i64_type,
                    "native_unsigned_result");
        } else {
            *result = call;
        }
        if (*result == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK && needs_stack_restore) {
        static const char stack_restore_name[] = "llvm.stackrestore";
        const unsigned stack_restore_id = LLVMLookupIntrinsicID(
            stack_restore_name, sizeof(stack_restore_name) - 1);
        LLVMTypeRef stack_restore_overload_types[1] = {
            backend->byte_pointer_type,
        };
        LLVMValueRef stack_restore_function = stack_restore_id == 0 ? NULL
            : LLVMGetIntrinsicDeclaration(backend->module, stack_restore_id,
                stack_restore_overload_types, 1);
        if (stack_restore_function == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            LLVMValueRef restore_arguments[1] = {saved_stack};
            if (LLVMBuildCall2(backend->builder,
                    LLVMGlobalGetValueType(stack_restore_function),
                    stack_restore_function, restore_arguments, 1, "") == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
    }
    if (status == OM_STATUS_OK && callback_unregister_status != NULL) {
        status = emit_runtime_status_check(backend, block_id,
                                           callback_unregister_status);
    }
native_call_cleanup:
    native_callback_call_destroy(&callback_call);
    om_free(parameter_abi);
    om_free(parameter_types);
    om_free(arguments);
    return status;
}

static Om_Status emit_instruction(Om_Llvm_Backend *backend,
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
            left, right, intrinsic_name, false, &result);
        break;
    }
    case OM_MIR_INT_NEGATE_CHECKED: {
        LLVMValueRef zero = LLVMConstInt(backend->i64_type, 0, 0);
        status = emit_checked_integer(backend, block_id, instruction,
            zero, left, "llvm.ssub.with.overflow.i64", true, &result);
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
        LLVMValueRef call = LLVMBuildCall2(backend->builder, callee_type,
            callee, arguments, (unsigned)instruction->operand_count + 1,
            "direct_call");
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
        LLVMValueRef call = LLVMBuildCall2(backend->builder, signature, left,
            arguments, (unsigned)argument_count, "indirect_call");
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
    case OM_MIR_RUNTIME_ACTOR_SPAWN: {
        if (instruction->operand_count > 1
            || backend->actor_descriptors == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
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
            status = OM_STATUS_NOT_FOUND;
            break;
        }
        LLVMValueRef actor_id_slot = entry_alloca(backend, function,
            backend->i64_type, "actor_id_slot");
        if (actor_id_slot == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMValueRef actor_descriptor = LLVMConstBitCast(
            backend->actor_descriptors[actor_index],
            backend->byte_pointer_type);
        if (instruction->operand_count == 0) {
            LLVMValueRef arguments[3] = {
                context_for_function(backend, function), actor_descriptor,
                actor_id_slot,
            };
            status = emit_runtime_status_call(backend, block_id,
                backend->runtime_actor_spawn_id, arguments, 3);
        } else {
            LLVMValueRef arguments[4] = {
                context_for_function(backend, function), actor_descriptor,
                left, actor_id_slot,
            };
            status = emit_runtime_status_call(backend, block_id,
                backend->runtime_actor_spawn_at, arguments, 4);
        }
        if (status == OM_STATUS_OK) {
            result = LLVMBuildLoad2(backend->builder, backend->i64_type,
                actor_id_slot, "spawned_actor_id");
            if (result == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_SELF: {
        if (instruction->operand_count != 0) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef actor_id_slot = entry_alloca(backend, function,
            backend->i64_type, "self_actor_id_slot");
        if (actor_id_slot == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        LLVMValueRef arguments[2] = {
            context_for_function(backend, function), actor_id_slot,
        };
        status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_self_id, arguments, 2);
        if (status == OM_STATUS_OK) {
            result = LLVMBuildLoad2(backend->builder, backend->i64_type,
                actor_id_slot, "self_actor_id");
            if (result == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_SEND: {
        if (instruction->operand_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef arguments[3] = {
            context_for_function(backend, function), left, right,
        };
        status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_send_id, arguments, 3);
        if (status == OM_STATUS_OK) result = LLVMConstNull(backend->i8_type);
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_WATCH: {
        if (instruction->operand_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef arguments[2] = {
            context_for_function(backend, function), left,
        };
        status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_watch_id, arguments, 2);
        if (status == OM_STATUS_OK) result = LLVMConstNull(backend->i8_type);
        break;
    }
    case OM_MIR_RUNTIME_ACTOR_STOP: {
        if (instruction->operand_count != 0) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        LLVMValueRef arguments[1] = {
            context_for_function(backend, function),
        };
        status = emit_runtime_status_call(backend, block_id,
            backend->runtime_actor_stop, arguments, 1);
        if (status == OM_STATUS_OK) result = LLVMConstNull(backend->i8_type);
        break;
    }
    case OM_MIR_RUNTIME_SYSTEM_ACTOR:
        if (instruction->operand_count != 0
            || (instruction->integer_value != OM_SYSTEM_ACTOR_REF_CONSOLE
                && instruction->integer_value != OM_SYSTEM_ACTOR_REF_TIMER
                && instruction->integer_value
                    != OM_SYSTEM_ACTOR_REF_FILE_SYSTEM)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            result = LLVMConstInt(backend->i64_type,
                                  instruction->integer_value, 0);
        }
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

static Om_Status emit_terminator(Om_Llvm_Backend *backend,
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

static Om_Status emit_function_bodies(Om_Llvm_Backend *backend)
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

static Om_Status add_wrapper_failure_check(Om_Llvm_Backend *backend,
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

static Om_Status add_entry_thunk(Om_Llvm_Backend *backend,
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

static Om_Status add_entry_wrapper(Om_Llvm_Backend *backend,
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

static void dispose_backend(Om_Llvm_Backend *backend)
{
    if (backend->builder != NULL) LLVMDisposeBuilder(backend->builder);
    if (backend->debug_builder != NULL) {
        if (!backend->debug_finalized) {
            LLVMDIBuilderFinalize(backend->debug_builder);
            backend->debug_finalized = true;
        }
        LLVMDisposeDIBuilder(backend->debug_builder);
    }
    if (backend->module != NULL) LLVMDisposeModule(backend->module);
    if (backend->context != NULL) LLVMContextDispose(backend->context);
    if (backend->target_machine != NULL) {
        LLVMDisposeTargetMachine(backend->target_machine);
    }
    if (backend->target_data != NULL) {
        LLVMDisposeTargetData(backend->target_data);
    }
    om_free(backend->functions);
    om_free(backend->blocks);
    om_free(backend->block_tails);
    om_free(backend->values);
    om_free(backend->root_frames);
    om_free(backend->root_arrays);
    om_free(backend->root_slot_indices);
    om_free(backend->root_slot_counts_by_value);
    om_free(backend->root_counts);
    for (size_t index = 0; index < backend->callback_thunk_set_count; ++index) {
        om_free(backend->callback_thunk_sets[index].parameter_types);
    }
    om_free(backend->callback_thunk_sets);
    om_free(backend->descriptors);
    om_free(backend->actor_descriptors);
    om_free(backend->debug_files);
    om_free(backend->debug_subprograms);
    *backend = (Om_Llvm_Backend){0};
}

static Om_Status run_optimization_pipeline(Om_Llvm_Backend *backend)
{
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (options == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMPassBuilderOptionsSetVerifyEach(options, 1);
#if OM_LLVM_OPT_LEVEL == 2
    const char *pipeline = "default<O2>";
#else
    const char *pipeline = "default<O0>";
#endif
    LLVMErrorRef error = LLVMRunPasses(backend->module, pipeline,
        backend->target_machine, options);
    LLVMDisposePassBuilderOptions(options);
    if (error == NULL) {
        return OM_STATUS_OK;
    }
    char *message = LLVMGetErrorMessage(error);
    (void)fprintf(stderr, "ombasic: LLVM optimization failed: %s\n",
        message == NULL ? "unknown error" : message);
    if (message != NULL) {
        LLVMDisposeErrorMessage(message);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

Om_Status om_llvm_emit_artifact(const Om_Mir_Project *project,
                                Om_Symbol_Id entry_symbol,
                                Om_String_View source_root,
                                const Om_Backend_Source_Location *locations,
                                size_t location_count,
                                Om_Llvm_Artifact artifact,
                                const char *output_path)
{
    if (project == NULL || project->owned_storage == NULL
        || project->type_store == NULL || output_path == NULL
        || output_path[0] == '\0' || entry_symbol.value == UINT32_MAX
        || artifact < OM_LLVM_ARTIFACT_OBJECT
        || artifact > OM_LLVM_ARTIFACT_LLVM_IR
        || (project->function_count != 0 && project->functions == NULL)
        || (project->block_count != 0 && project->blocks == NULL)
        || (project->value_count != 0 && project->values == NULL)
        || (project->instruction_count != 0 && project->instructions == NULL)
        || (project->actor_count != 0 && project->actors == NULL)
        || !om_string_view_is_valid(source_root)
        || location_count != project->instruction_count
        || (location_count != 0 && locations == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Llvm_Backend backend = {
        .project = project,
        .source_root = source_root,
        .source_locations = locations,
        .source_location_count = location_count,
    };
    Om_Status status = OM_STATUS_OK;
    backend.functions = project->function_count == 0 ? NULL
        : om_alloc_array(project->function_count, sizeof(*backend.functions));
    backend.blocks = project->block_count == 0 ? NULL
        : om_alloc_array(project->block_count, sizeof(*backend.blocks));
    backend.block_tails = project->block_count == 0 ? NULL
        : om_alloc_array(project->block_count, sizeof(*backend.block_tails));
    backend.values = project->value_count == 0 ? NULL
        : om_alloc_array(project->value_count, sizeof(*backend.values));
    backend.root_frames = project->function_count == 0 ? NULL
        : om_alloc_array(project->function_count, sizeof(*backend.root_frames));
    backend.root_arrays = project->function_count == 0 ? NULL
        : om_alloc_array(project->function_count, sizeof(*backend.root_arrays));
    backend.root_counts = project->function_count == 0 ? NULL
        : om_alloc_array(project->function_count, sizeof(*backend.root_counts));
    backend.root_slot_indices = project->value_count == 0 ? NULL
        : om_alloc_array(project->value_count,
                         sizeof(*backend.root_slot_indices));
    backend.root_slot_counts_by_value = project->value_count == 0 ? NULL
        : om_alloc_array(project->value_count,
                         sizeof(*backend.root_slot_counts_by_value));
    backend.actor_descriptors = project->actor_count == 0 ? NULL
        : om_alloc_array(project->actor_count,
                         sizeof(*backend.actor_descriptors));
    backend.debug_files = location_count == 0 ? NULL
        : om_alloc_array(location_count, sizeof(*backend.debug_files));
    backend.debug_subprograms = project->function_count == 0 ? NULL
        : om_alloc_array(project->function_count,
                         sizeof(*backend.debug_subprograms));
    if ((project->function_count != 0 && backend.functions == NULL)
        || (project->block_count != 0
            && (backend.blocks == NULL || backend.block_tails == NULL))
        || (project->value_count != 0 && (backend.values == NULL
            || backend.root_slot_indices == NULL
            || backend.root_slot_counts_by_value == NULL))
        || (project->function_count != 0 && (backend.root_frames == NULL
            || backend.root_arrays == NULL || backend.root_counts == NULL))
        || (project->actor_count != 0 && backend.actor_descriptors == NULL)
        || (location_count != 0 && backend.debug_files == NULL)
        || (project->function_count != 0
            && backend.debug_subprograms == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status != OM_STATUS_OK) {
        dispose_backend(&backend);
        return status;
    }
    if (backend.functions != NULL) {
        (void)memset(backend.functions, 0,
            project->function_count * sizeof(*backend.functions));
    }
    if (backend.blocks != NULL) {
        (void)memset(backend.blocks, 0,
            project->block_count * sizeof(*backend.blocks));
        (void)memset(backend.block_tails, 0,
            project->block_count * sizeof(*backend.block_tails));
    }
    if (backend.values != NULL) {
        (void)memset(backend.values, 0,
            project->value_count * sizeof(*backend.values));
        (void)memset(backend.root_slot_indices, 0xff,
            project->value_count * sizeof(*backend.root_slot_indices));
        (void)memset(backend.root_slot_counts_by_value, 0,
            project->value_count * sizeof(*backend.root_slot_counts_by_value));
    }
    if (backend.root_frames != NULL) {
        (void)memset(backend.root_frames, 0,
            project->function_count * sizeof(*backend.root_frames));
        (void)memset(backend.root_arrays, 0,
            project->function_count * sizeof(*backend.root_arrays));
        (void)memset(backend.root_counts, 0,
            project->function_count * sizeof(*backend.root_counts));
    }
    if (backend.actor_descriptors != NULL) {
        (void)memset(backend.actor_descriptors, 0,
            project->actor_count * sizeof(*backend.actor_descriptors));
    }
    if (backend.debug_files != NULL) {
        (void)memset(backend.debug_files, 0,
            location_count * sizeof(*backend.debug_files));
    }
    if (backend.debug_subprograms != NULL) {
        (void)memset(backend.debug_subprograms, 0,
            project->function_count * sizeof(*backend.debug_subprograms));
    }
    if (LLVMInitializeNativeTarget() != 0
        || LLVMInitializeNativeAsmPrinter() != 0) {
        status = OM_STATUS_NOT_FOUND;
    }
    const char *triple = OM_LLVM_TARGET_TRIPLE;
    char *default_triple = NULL;
    if (status == OM_STATUS_OK && triple[0] == '\0') {
        default_triple = LLVMGetDefaultTargetTriple();
        if (default_triple == NULL) {
            status = OM_STATUS_NOT_FOUND;
        } else {
            triple = default_triple;
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_target_describe(triple, &backend.target);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: unsupported target ABI triple '%s'\n", triple);
        }
    }
    LLVMTargetRef target = NULL;
    char *error_message = NULL;
    if (status == OM_STATUS_OK
        && LLVMGetTargetFromTriple(triple, &target, &error_message) != 0) {
        (void)fprintf(stderr, "ombasic: LLVM target lookup failed: %s\n",
            error_message == NULL ? "unknown target" : error_message);
        status = OM_STATUS_NOT_FOUND;
    }
    if (error_message != NULL) LLVMDisposeMessage(error_message);
    if (status == OM_STATUS_OK) {
#if OM_LLVM_OPT_LEVEL == 2
        const LLVMCodeGenOptLevel codegen_optimization_level =
            LLVMCodeGenLevelDefault;
#else
        const LLVMCodeGenOptLevel codegen_optimization_level =
            LLVMCodeGenLevelNone;
#endif
        const LLVMRelocMode relocation_mode =
            strstr(triple, "linux") != NULL ? LLVMRelocPIC : LLVMRelocDefault;
        backend.target_machine = LLVMCreateTargetMachine(target, triple,
            "generic", "", codegen_optimization_level, relocation_mode,
            LLVMCodeModelDefault);
        if (backend.target_machine == NULL) {
            status = OM_STATUS_NOT_FOUND;
        }
    }
    if (status == OM_STATUS_OK) {
        backend.context = LLVMContextCreate();
        if (backend.context == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        backend.module = LLVMModuleCreateWithNameInContext("ombasic.native",
                                                          backend.context);
        backend.builder = LLVMCreateBuilderInContext(backend.context);
        if (backend.module == NULL || backend.builder == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        LLVMSetTarget(backend.module, triple);
        backend.target_data = LLVMCreateTargetDataLayout(
            backend.target_machine);
        if (backend.target_data == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            char *layout = LLVMCopyStringRepOfTargetData(backend.target_data);
            if (layout == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                LLVMSetDataLayout(backend.module, layout);
                LLVMDisposeMessage(layout);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = initialize_types(&backend);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: LLVM type setup failed (%s)\n",
                          om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = create_function_declarations(&backend);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: LLVM function declaration failed (%s)\n",
                om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = initialize_debug_info(&backend, triple);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: LLVM debug info setup failed (%s)\n",
                om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = create_basic_blocks(&backend);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: LLVM block setup failed (%s)\n",
                          om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = create_actor_runtime_descriptors(&backend);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr,
                "ombasic: LLVM actor descriptor setup failed (%s)\n",
                om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = emit_function_bodies(&backend);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: LLVM body emission failed (%s)\n",
                          om_status_name(status));
        }
    }
    if (status == OM_STATUS_OK) {
        status = add_entry_wrapper(&backend, entry_symbol);
        if (status != OM_STATUS_OK) {
            (void)fprintf(stderr, "ombasic: LLVM entry setup failed (%s)\n",
                          om_status_name(status));
        }
    }
    if (backend.debug_builder != NULL && !backend.debug_finalized) {
        LLVMDIBuilderFinalize(backend.debug_builder);
        backend.debug_finalized = true;
    }
    if (status == OM_STATUS_OK) {
        status = run_optimization_pipeline(&backend);
    }
    if (status == OM_STATUS_OK) {
        error_message = NULL;
        if (LLVMVerifyModule(backend.module, LLVMReturnStatusAction,
                             &error_message) != 0) {
            (void)fprintf(stderr, "ombasic: LLVM module verification failed: %s\n",
                error_message == NULL ? "invalid module" : error_message);
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (error_message != NULL) LLVMDisposeMessage(error_message);
    }
    if (status == OM_STATUS_OK) {
        error_message = NULL;
        if (artifact == OM_LLVM_ARTIFACT_LLVM_IR) {
            if (LLVMPrintModuleToFile(backend.module, output_path,
                                      &error_message) != 0) {
                (void)fprintf(stderr, "ombasic: LLVM IR emission failed: %s\n",
                    error_message == NULL ? "unknown error" : error_message);
                status = OM_STATUS_IO_ERROR;
            }
        } else {
            const LLVMCodeGenFileType file_type =
                artifact == OM_LLVM_ARTIFACT_ASSEMBLY
                    ? LLVMAssemblyFile : LLVMObjectFile;
            if (LLVMTargetMachineEmitToFile(backend.target_machine,
                    backend.module, (char *)output_path, file_type,
                    &error_message) != 0) {
                (void)fprintf(stderr, "ombasic: native artifact emission failed: %s\n",
                    error_message == NULL ? "unknown error" : error_message);
                status = OM_STATUS_IO_ERROR;
            }
        }
        if (error_message != NULL) LLVMDisposeMessage(error_message);
    }
    if (default_triple != NULL) LLVMDisposeMessage(default_triple);
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: LLVM backend failed (%s)\n",
                      om_status_name(status));
    }
    dispose_backend(&backend);
    return status;
}

Om_Status om_llvm_emit_object(const Om_Mir_Project *project,
                              Om_Symbol_Id entry_symbol,
                              Om_String_View source_root,
                              const Om_Backend_Source_Location *locations,
                              size_t location_count,
                              const char *output_path)
{
    return om_llvm_emit_artifact(project, entry_symbol, source_root, locations,
        location_count, OM_LLVM_ARTIFACT_OBJECT, output_path);
}
