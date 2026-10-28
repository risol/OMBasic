#ifndef OMBASIC_COMPILER_PIPELINE_H
#define OMBASIC_COMPILER_PIPELINE_H

#include <stddef.h>

#include "ombasic/ir/hir.h"
#include "ombasic/ir/mir.h"
#include "ombasic/ir/specialize.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/syntax/parser.h"

typedef enum Om_Compiler_Artifact {
    OM_COMPILER_ARTIFACT_HIR = 0,
    OM_COMPILER_ARTIFACT_MIR,
    OM_COMPILER_ARTIFACT_LLVM_IR,
    OM_COMPILER_ARTIFACT_ASSEMBLY,
    OM_COMPILER_ARTIFACT_OBJECT
} Om_Compiler_Artifact;

typedef enum Om_Compiler_Stage {
    OM_COMPILER_STAGE_NONE = 0,
    OM_COMPILER_STAGE_VALIDATE,
    OM_COMPILER_STAGE_HIR_LOWER,
    OM_COMPILER_STAGE_HIR_VERIFY,
    OM_COMPILER_STAGE_HIR_EMIT,
    OM_COMPILER_STAGE_SPECIALIZE,
    OM_COMPILER_STAGE_MIR_LOWER,
    OM_COMPILER_STAGE_MIR_VERIFY,
    OM_COMPILER_STAGE_MIR_EMIT,
    OM_COMPILER_STAGE_SOURCE_LOCATIONS,
    OM_COMPILER_STAGE_BACKEND_EMIT
} Om_Compiler_Stage;

typedef struct Om_Compiler_Emit_Request {
    const Om_Module_Project *source_project;
    Om_String_View source_root;
    const Om_Parser_Result *parsed_modules;
    size_t parsed_module_count;
    const Om_Resolution_Result *resolution;
    Om_Type_Inference_Result *inference;
    const Om_Semantic_Analysis_Result *semantic;
    Om_Symbol_Id entry_symbol;
    Om_Compiler_Artifact artifact;
    const char *output_path;
} Om_Compiler_Emit_Request;

typedef struct Om_Compiler_Emit_Result {
    Om_Status status;
    Om_Compiler_Stage failed_stage;
    Om_Hir_Project hir;
    Om_Hir_Verification_Result hir_verification;
    Om_Hir_Specialization specialization;
    Om_Mir_Project mir;
    Om_Mir_Lowering_Result mir_lowering;
    Om_Mir_Verification_Result mir_verification;
} Om_Compiler_Emit_Result;

/* Lowers analyzed source through verified HIR/MIR and emits one native artifact. */
Om_Status om_compiler_emit(const Om_Compiler_Emit_Request *request,
                           Om_Compiler_Emit_Result *result);
void om_compiler_emit_result_destroy(Om_Compiler_Emit_Result *result);

#endif
