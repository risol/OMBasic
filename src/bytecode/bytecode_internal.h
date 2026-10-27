#ifndef OMBASIC_BYTECODE_INTERNAL_H
#define OMBASIC_BYTECODE_INTERNAL_H

#include "ombasic/bytecode/bytecode.h"
#include "ombasic/foundation/vector.h"

typedef struct Om_Bytecode_Storage {
    Om_Vector constants;
    Om_Vector functions;
    Om_Vector instructions;
    Om_Vector source_map;
    Om_Vector actors;
    Om_Vector handlers;
    Om_Vector schemas;
    Om_Vector schema_fields;
    uint32_t active_function;
} Om_Bytecode_Storage;

Om_Status om_bytecode_storage_create(Om_Bytecode_Storage **storage);
void om_bytecode_storage_destroy(Om_Bytecode_Storage *storage);
Om_Status om_bytecode_storage_clone_image(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Storage **storage
);
void om_bytecode_storage_bind_image(
    const Om_Bytecode_Storage *storage,
    Om_Bytecode_Image *image
);
Om_Status om_bytecode_prepare_image(
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result
);
void om_bytecode_set_verify_failure(
    Om_Bytecode_Verify_Result *result,
    Om_Bytecode_Verify_Code code,
    uint32_t function_index,
    uint32_t instruction_index
);

#endif
