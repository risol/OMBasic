#include "backend_internal.h"

static LLVMMetadataRef create_debug_file(Om_Llvm_Backend_Context *backend,
                                         Om_String_View path);

static LLVMMetadataRef create_debug_file(Om_Llvm_Backend_Context *backend,
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

Om_Status initialize_debug_info(Om_Llvm_Backend_Context *backend,
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
    if (backend->target.os == OM_TARGET_OS_WINDOWS) {
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

void set_instruction_debug_location(Om_Llvm_Backend_Context *backend,
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

Om_Status create_source_span_table(Om_Llvm_Backend_Context *backend,
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
