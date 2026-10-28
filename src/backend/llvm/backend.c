#include "backend_internal.h"

static void dispose_backend(Om_Llvm_Backend_Context *backend);

static void dispose_backend(Om_Llvm_Backend_Context *backend)
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
    if (backend->owned_target_triple != NULL) {
        LLVMDisposeMessage(backend->owned_target_triple);
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
    *backend = (Om_Llvm_Backend_Context){0};
}

Om_Status om_llvm_emit(const Om_Llvm_Emit_Request *request,
                       Om_Llvm_Emit_Result *result)
{
    if (result != NULL) {
        *result = (Om_Llvm_Emit_Result){0};
    }
    if (request == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Mir_Project *project = request->project;
    const Om_Symbol_Id entry_symbol = request->entry_symbol;
    const Om_String_View source_root = request->source_root;
    const Om_Backend_Source_Location *locations = request->source_locations;
    const size_t location_count = request->source_location_count;
    const Om_Llvm_Artifact artifact = request->artifact;
    const char *const output_path = request->output_path;
    if (project == NULL || project->owned_storage == NULL
        || project->type_store == NULL || output_path == NULL
        || output_path[0] == '\0' || entry_symbol.value == UINT32_MAX
        || artifact < OM_LLVM_ARTIFACT_OBJECT
        || artifact > OM_LLVM_ARTIFACT_LLVM_IR
        || (request->optimization_level != OM_LLVM_OPTIMIZATION_DEFAULT
            && request->optimization_level != OM_LLVM_OPTIMIZATION_NONE
            && request->optimization_level != OM_LLVM_OPTIMIZATION_SPEED)
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
#if OM_LLVM_OPT_LEVEL == 2
    const Om_Llvm_Optimization_Level build_optimization_level =
        OM_LLVM_OPTIMIZATION_SPEED;
#else
    const Om_Llvm_Optimization_Level build_optimization_level =
        OM_LLVM_OPTIMIZATION_NONE;
#endif
    Om_Llvm_Backend_Context backend = {
        .project = project,
        .requested_target_triple = request->target_triple,
        .optimization_level = request->optimization_level
            == OM_LLVM_OPTIMIZATION_DEFAULT
                ? build_optimization_level : request->optimization_level,
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
    if (status == OM_STATUS_OK) {
        status = om_llvm_backend_initialize_module(&backend);
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
        status = initialize_debug_info(&backend, backend.target_triple);
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
        status = om_llvm_backend_verify_module(&backend);
    }
    if (status == OM_STATUS_OK) {
        status = om_llvm_backend_emit_file(&backend, artifact, output_path);
    }
    if (status != OM_STATUS_OK) {
        (void)fprintf(stderr, "ombasic: LLVM backend failed (%s)\n",
                      om_status_name(status));
    } else if (result != NULL) {
        result->c_abi = backend.target.c_abi;
        result->pointer_size = backend.target.pointer_size;
        result->c_long_size = backend.target.c_long_size;
    }
    dispose_backend(&backend);
    return status;
}
