#ifndef OMBASIC_COMPILER_NATIVE_BINDING_H
#define OMBASIC_COMPILER_NATIVE_BINDING_H

#include <stddef.h>

#include "ombasic/bytecode/bytecode.h"
#include "ombasic/ffi/native_call.h"
#include "ombasic/ffi/omi.h"
#include "ombasic/foundation/status.h"
#include "ombasic/runtime/vm.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/type_inference.h"

/*
 * Validates `.omi` callback bindings against resolved Message declarations and
 * their inferred field types, then asks the Runtime to install typed bindings.
 * The image must be the image loaded by vm; all borrowed inputs must outlive
 * this call, while library and module must outlive the VM.
 */
Om_Status om_compiler_bind_native_module(
    Om_VM *vm,
    const Om_Bytecode_Image *image,
    const Om_Native_Library *library,
    const Om_Omi_Module *module,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    size_t maximum_cstring_bytes
);

#endif
