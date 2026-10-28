#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "ombasic/backend/llvm_backend.h"
#include "ombasic/ir/mir.h"
#include "ombasic/semantic/type_store.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { MAX_CONSTANTS = 8 };
    static const char output_template[] = "/tmp/ombasic-llvm-fuzz-XXXXXX";
    static const char source_root[] = "/fuzz";
    const size_t maximum_input_size = 256;
    if (size > maximum_input_size) return 0;

    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id block_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_init(&types);
    if (status != OM_STATUS_OK) goto cleanup;

    const bool is_action = size != 0 && (data[0] & 1u) != 0;
    const Om_Type_Kind signature_kind = is_action
        ? OM_TYPE_ACTION : OM_TYPE_FUNCTION;
    const Om_Mir_Function_Kind function_kind = is_action
        ? OM_MIR_ACTION : OM_MIR_FUNCTION;
    status = om_type_store_function(&types, signature_kind, NULL, 0,
        types.primitive_types[OM_TYPE_UNIT], &signature);
    if (status == OM_STATUS_OK) {
        status = om_mir_project_init(&project, &types);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_function(&project, (Om_Function_Instance_Id){0},
            function_kind, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
            OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(&project, function_id, &block_id);
    }
    const size_t constant_count = size == 0 ? 0
        : (size_t)(data[0] % (MAX_CONSTANTS + 1));
    for (size_t index = 0; status == OM_STATUS_OK && index < constant_count;
         ++index) {
        const Om_Mir_Instruction_Spec constant = {
            .opcode = OM_MIR_CONST_INT,
            .type_id = types.primitive_types[OM_TYPE_INT],
            .integer_value = index + 1 < size ? data[index + 1] : 0,
        };
        Om_Mir_Value_Id ignored = OM_MIR_VALUE_ID_INVALID;
        status = om_mir_add_instruction(&project, block_id, &constant, NULL, 0,
                                        &ignored);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_set_terminator(&project, block_id, OM_MIR_RETURN,
            OM_MIR_VALUE_ID_INVALID, NULL, 0);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_verify_project(&project, &verification);
    }
    if (status == OM_STATUS_OK && verification.diagnostic_count == 0) {
        char output_path[sizeof(output_template)];
        for (size_t index = 0; index < sizeof(output_template); ++index) {
            output_path[index] = output_template[index];
        }
        const int output_fd = mkstemp(output_path);
        if (output_fd >= 0) {
            (void)close(output_fd);
            (void)om_llvm_emit_object(&project, (Om_Symbol_Id){0},
                (Om_String_View){source_root, sizeof(source_root) - 1},
                NULL, 0, output_path);
            (void)unlink(output_path);
        }
    }

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return 0;
}
