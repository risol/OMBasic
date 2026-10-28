#ifndef OMBASIC_BACKEND_LLVM_BACKEND_H
#define OMBASIC_BACKEND_LLVM_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/ir/mir.h"
#include "ombasic/target/target.h"

typedef struct Om_Backend_Source_Location {
    Om_String_View path;
    uint32_t line;
    uint32_t column;
} Om_Backend_Source_Location;

typedef enum Om_Llvm_Artifact {
    OM_LLVM_ARTIFACT_OBJECT = 0,
    OM_LLVM_ARTIFACT_ASSEMBLY,
    OM_LLVM_ARTIFACT_LLVM_IR
} Om_Llvm_Artifact;

typedef enum Om_Llvm_Optimization_Level {
    OM_LLVM_OPTIMIZATION_DEFAULT = 0,
    OM_LLVM_OPTIMIZATION_NONE,
    OM_LLVM_OPTIMIZATION_SPEED
} Om_Llvm_Optimization_Level;

typedef struct Om_Llvm_Emit_Request {
    const Om_Mir_Project *project;
    Om_Symbol_Id entry_symbol;
    const Om_Target *target;
    Om_String_View source_root;
    const Om_Backend_Source_Location *source_locations;
    size_t source_location_count;
    Om_Llvm_Artifact artifact;
    const char *output_path;
    /* DEFAULT uses the build configuration's optimization level. */
    Om_Llvm_Optimization_Level optimization_level;
} Om_Llvm_Emit_Request;

/* Populated only after successful emission; contains the selected C ABI. */
typedef struct Om_Llvm_Emit_Result {
    Om_Target_C_Abi_Family c_abi;
    uint8_t pointer_size;
    uint8_t c_long_size;
} Om_Llvm_Emit_Result;

/* Emits one artifact for the selected entry using the requested target ABI. */
Om_Status om_llvm_emit(const Om_Llvm_Emit_Request *request,
                       Om_Llvm_Emit_Result *result);

#endif
