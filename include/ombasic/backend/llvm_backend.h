#ifndef OMBASIC_BACKEND_LLVM_BACKEND_H
#define OMBASIC_BACKEND_LLVM_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/ir/mir.h"

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

/* Emits one host-native object and a C main wrapper for the selected entry. */
Om_Status om_llvm_emit_object(const Om_Mir_Project *project,
                              Om_Symbol_Id entry_symbol,
                              Om_String_View source_root,
                              const Om_Backend_Source_Location *locations,
                              size_t location_count,
                              const char *output_path);
Om_Status om_llvm_emit_artifact(const Om_Mir_Project *project,
                                Om_Symbol_Id entry_symbol,
                                Om_String_View source_root,
                                const Om_Backend_Source_Location *locations,
                                size_t location_count,
                                Om_Llvm_Artifact artifact,
                                const char *output_path);

#endif
