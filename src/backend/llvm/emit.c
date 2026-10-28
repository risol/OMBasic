#include "backend_internal.h"

Om_Status run_optimization_pipeline(Om_Llvm_Backend_Context *backend)
{
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (options == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMPassBuilderOptionsSetVerifyEach(options, 1);
    const char *pipeline =
        backend->optimization_level == OM_LLVM_OPTIMIZATION_SPEED
            ? "default<O2>" : "default<O0>";
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

Om_Status om_llvm_backend_emit_file(Om_Llvm_Backend_Context *backend,
                                   Om_Llvm_Artifact artifact,
                                   const char *output_path)
{
    if (backend == NULL || backend->module == NULL || output_path == NULL
        || output_path[0] == '\0') {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    char *error_message = NULL;
    int emit_status = 0;
    if (artifact == OM_LLVM_ARTIFACT_LLVM_IR) {
        emit_status = LLVMPrintModuleToFile(backend->module, output_path,
                                             &error_message);
    } else {
        const LLVMCodeGenFileType file_type =
            artifact == OM_LLVM_ARTIFACT_ASSEMBLY
                ? LLVMAssemblyFile : LLVMObjectFile;
        emit_status = LLVMTargetMachineEmitToFile(backend->target_machine,
                backend->module, (char *)output_path, file_type,
                &error_message);
    }
    if (emit_status == 0) {
        if (error_message != NULL) {
            LLVMDisposeMessage(error_message);
        }
        return OM_STATUS_OK;
    }
    const char *failure = error_message == NULL
        ? "unknown error" : error_message;
    (void)fprintf(stderr, "ombasic: %s emission failed: %s\n",
        artifact == OM_LLVM_ARTIFACT_LLVM_IR ? "LLVM IR" : "native artifact",
        failure);
    if (error_message != NULL) {
        LLVMDisposeMessage(error_message);
    }
    return OM_STATUS_IO_ERROR;
}
