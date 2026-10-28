#include "backend_internal.h"

Om_Status om_llvm_backend_initialize_module(Om_Llvm_Backend_Context *backend)
{
    if (backend == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (LLVMInitializeNativeTarget() != 0
        || LLVMInitializeNativeAsmPrinter() != 0) {
        return OM_STATUS_NOT_FOUND;
    }

    const char *triple = backend->target.triple;
    if (triple == NULL || triple[0] == '\0') {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Status status = OM_STATUS_OK;
    LLVMTargetRef target = NULL;
    char *error_message = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &error_message) != 0) {
        (void)fprintf(stderr, "ombasic: LLVM target lookup failed: %s\n",
            error_message == NULL ? "unknown error" : error_message);
        status = OM_STATUS_NOT_FOUND;
    }
    if (error_message != NULL) {
        LLVMDisposeMessage(error_message);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }

    const LLVMCodeGenOptLevel codegen_optimization_level =
        backend->optimization_level == OM_LLVM_OPTIMIZATION_SPEED
            ? LLVMCodeGenLevelDefault : LLVMCodeGenLevelNone;
    const LLVMRelocMode relocation_mode =
        backend->target.object_format == OM_TARGET_OBJECT_FORMAT_ELF
            ? LLVMRelocPIC : LLVMRelocDefault;
    backend->target_machine = LLVMCreateTargetMachine(target, triple,
        "generic", "", codegen_optimization_level, relocation_mode,
        LLVMCodeModelDefault);
    if (backend->target_machine == NULL) {
        return OM_STATUS_NOT_FOUND;
    }

    backend->context = LLVMContextCreate();
    if (backend->context == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    backend->module = LLVMModuleCreateWithNameInContext("ombasic.native",
                                                        backend->context);
    backend->builder = LLVMCreateBuilderInContext(backend->context);
    if (backend->module == NULL || backend->builder == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }

    LLVMSetTarget(backend->module, triple);
    backend->target_data = LLVMCreateTargetDataLayout(backend->target_machine);
    if (backend->target_data == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    char *layout = LLVMCopyStringRepOfTargetData(backend->target_data);
    if (layout == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    LLVMSetDataLayout(backend->module, layout);
    LLVMDisposeMessage(layout);
    return OM_STATUS_OK;
}

Om_Status om_llvm_backend_verify_module(Om_Llvm_Backend_Context *backend)
{
    if (backend == NULL || backend->module == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    char *error_message = NULL;
    if (LLVMVerifyModule(backend->module, LLVMReturnStatusAction,
                         &error_message) == 0) {
        if (error_message != NULL) {
            LLVMDisposeMessage(error_message);
        }
        return OM_STATUS_OK;
    }
    (void)fprintf(stderr, "ombasic: LLVM module verification failed: %s\n",
        error_message == NULL ? "invalid module" : error_message);
    if (error_message != NULL) {
        LLVMDisposeMessage(error_message);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}
