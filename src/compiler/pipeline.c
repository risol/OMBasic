#include "ombasic/compiler/pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/backend/llvm_backend.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/source/source_text.h"

static const Om_Source_Text *source_for_source_id(
    const Om_Module_Project *project,
    size_t module_count,
    Om_Source_Id source_id
)
{
    for (size_t index = 0; index < module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(project,
            (Om_Module_Id){(uint32_t)index});
        if (source != NULL && source->id.value == source_id.value) {
            return source;
        }
    }
    return NULL;
}

static Om_Status build_source_locations(
    const Om_Module_Project *source_project,
    size_t module_count,
    const Om_Mir_Project *mir,
    Om_Backend_Source_Location **locations_out
)
{
    if (source_project == NULL || mir == NULL || locations_out == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *locations_out = NULL;
    if (mir->instruction_count == 0) {
        return OM_STATUS_OK;
    }
    Om_Backend_Source_Location *locations = om_alloc_array(
        mir->instruction_count, sizeof(*locations));
    if (locations == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(locations, 0,
        mir->instruction_count * sizeof(*locations));
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < mir->instruction_count; ++index) {
        const Om_Mir_Instruction *instruction = &mir->instructions[index];
        if (instruction->span.source.value == UINT32_MAX) {
            continue;
        }
        const Om_Source_Text *source = source_for_source_id(source_project,
            module_count, instruction->span.source);
        if (source == NULL) {
            status = OM_STATUS_NOT_FOUND;
            break;
        }
        Om_Source_Location location = {0};
        status = om_source_text_location(source, instruction->span.start,
                                         &location);
        if (status == OM_STATUS_OK) {
            locations[index] = (Om_Backend_Source_Location){
                .path = source->canonical_path,
                .line = location.line,
                .column = location.byte_column,
            };
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(locations);
        return status;
    }
    *locations_out = locations;
    return OM_STATUS_OK;
}

static Om_Status write_text_artifact(const char *path, const char *text,
                                     size_t length)
{
    if (path == NULL || path[0] == '\0' || text == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    FILE *file = NULL;
#ifdef _MSC_VER
    if (fopen_s(&file, path, "wb") != 0) {
        return OM_STATUS_IO_ERROR;
    }
#else
    file = fopen(path, "wb");
    if (file == NULL) {
        return OM_STATUS_IO_ERROR;
    }
#endif
    const bool wrote_all = fwrite(text, 1, length, file) == length;
    return fclose(file) == 0 && wrote_all
        ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

void om_compiler_emit_result_destroy(Om_Compiler_Emit_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_mir_verification_result_destroy(&result->mir_verification);
    om_mir_lowering_result_destroy(&result->mir_lowering);
    om_mir_project_destroy(&result->mir);
    om_hir_specialization_destroy(&result->specialization);
    om_hir_verification_result_destroy(&result->hir_verification);
    om_hir_project_destroy(&result->hir);
    (void)memset(result, 0, sizeof(*result));
}

Om_Status om_compiler_emit(const Om_Compiler_Emit_Request *request,
                           Om_Compiler_Emit_Result *result)
{
    if (result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    Om_Status status = OM_STATUS_INVALID_ARGUMENT;
    if (request == NULL || request->target == NULL
        || request->target->triple == NULL
        || request->target->triple[0] == '\0'
        || request->source_project == NULL
        || request->parsed_modules == NULL || request->parsed_module_count == 0
        || request->resolution == NULL || request->inference == NULL
        || request->semantic == NULL || request->entry_symbol.value == UINT32_MAX
        || request->artifact < OM_COMPILER_ARTIFACT_HIR
        || request->artifact > OM_COMPILER_ARTIFACT_OBJECT
        || request->output_path == NULL || request->output_path[0] == '\0'
        || !om_string_view_is_valid(request->source_root)) {
        result->failed_stage = OM_COMPILER_STAGE_VALIDATE;
        result->status = status;
        return status;
    }

    result->failed_stage = OM_COMPILER_STAGE_HIR_LOWER;
    status = om_hir_lower_project(request->source_project,
        request->parsed_modules, request->parsed_module_count,
        request->resolution, request->inference, request->semantic,
        &result->hir);
    if (status != OM_STATUS_OK) goto done;

    result->failed_stage = OM_COMPILER_STAGE_HIR_VERIFY;
    status = om_hir_verify_project(&result->hir,
                                   &result->hir_verification);
    if (status != OM_STATUS_OK
        || result->hir_verification.diagnostic_count != 0) {
        if (status == OM_STATUS_OK) status = OM_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    if (request->artifact == OM_COMPILER_ARTIFACT_HIR) {
        result->failed_stage = OM_COMPILER_STAGE_HIR_EMIT;
        Om_Hir_Text text = {0};
        status = om_hir_dump(&result->hir, &text);
        if (status == OM_STATUS_OK) {
            status = write_text_artifact(request->output_path, text.data,
                                         text.length);
        }
        om_hir_text_destroy(&text);
        goto done;
    }

    result->failed_stage = OM_COMPILER_STAGE_SPECIALIZE;
    status = om_hir_specialize_project(&result->hir,
        &request->inference->type_store, &result->specialization);
    if (status != OM_STATUS_OK
        || result->specialization.diagnostic_count != 0) {
        if (status == OM_STATUS_OK) status = OM_STATUS_INVALID_ARGUMENT;
        goto done;
    }

    result->failed_stage = OM_COMPILER_STAGE_MIR_LOWER;
    status = om_mir_lower_project(&result->hir, &result->specialization,
        &result->mir, &result->mir_lowering);
    if (status != OM_STATUS_OK || result->mir_lowering.diagnostic_count != 0) {
        if (status == OM_STATUS_OK) status = OM_STATUS_INVALID_ARGUMENT;
        goto done;
    }

    result->failed_stage = OM_COMPILER_STAGE_MIR_VERIFY;
    status = om_mir_verify_project(&result->mir, &result->mir_verification);
    if (status != OM_STATUS_OK
        || result->mir_verification.diagnostic_count != 0) {
        if (status == OM_STATUS_OK) status = OM_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    if (request->artifact == OM_COMPILER_ARTIFACT_MIR) {
        result->failed_stage = OM_COMPILER_STAGE_MIR_EMIT;
        Om_Mir_Text text = {0};
        status = om_mir_dump(&result->mir, &text);
        if (status == OM_STATUS_OK) {
            status = write_text_artifact(request->output_path, text.data,
                                         text.length);
        }
        om_mir_text_destroy(&text);
        goto done;
    }

    result->failed_stage = OM_COMPILER_STAGE_SOURCE_LOCATIONS;
    Om_Backend_Source_Location *locations = NULL;
    status = build_source_locations(request->source_project,
        request->parsed_module_count, &result->mir, &locations);
    if (status == OM_STATUS_OK) {
        result->failed_stage = OM_COMPILER_STAGE_BACKEND_EMIT;
        Om_Llvm_Artifact artifact = OM_LLVM_ARTIFACT_OBJECT;
        if (request->artifact == OM_COMPILER_ARTIFACT_LLVM_IR) {
            artifact = OM_LLVM_ARTIFACT_LLVM_IR;
        } else if (request->artifact == OM_COMPILER_ARTIFACT_ASSEMBLY) {
            artifact = OM_LLVM_ARTIFACT_ASSEMBLY;
        }
        const Om_Llvm_Emit_Request emit_request = {
            .project = &result->mir,
            .entry_symbol = request->entry_symbol,
            .target = request->target,
            .source_root = request->source_root,
            .source_locations = locations,
            .source_location_count = result->mir.instruction_count,
            .artifact = artifact,
            .output_path = request->output_path,
            .optimization_level = OM_LLVM_OPTIMIZATION_DEFAULT,
        };
        status = om_llvm_emit(&emit_request, NULL);
    }
    om_free(locations);

done:
    result->status = status;
    return status;
}
