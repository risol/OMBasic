#include <stddef.h>
#include <stdint.h>

#include "ombasic/ir/mir.h"
#include "ombasic/semantic/type_store.h"

static uint8_t byte_at(const uint8_t *data, size_t size, size_t index)
{
    return index < size ? data[index] : 0;
}

static uint32_t word_at(const uint8_t *data, size_t size, size_t index)
{
    uint32_t value = 0;
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= (uint32_t)byte_at(data, size, index + byte) << (byte * 8);
    }
    return value;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    enum { MAX_INSTRUCTIONS = 8 };
    const Om_Type_Kind type_kinds[] = {
        OM_TYPE_INT, OM_TYPE_FLOAT, OM_TYPE_BOOL,
    };
    const Om_Mir_Opcode constant_opcodes[] = {
        OM_MIR_CONST_INT, OM_MIR_CONST_FLOAT, OM_MIR_CONST_BOOL,
    };
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id block_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Type_Id result_type = OM_TYPE_ID_INVALID;
    Om_Mir_Value_Id last_value = OM_MIR_VALUE_ID_INVALID;

    if (om_type_store_init(&types) != OM_STATUS_OK) return 0;
    const size_t kind_index = byte_at(data, size, 0)
        % (sizeof(type_kinds) / sizeof(type_kinds[0]));
    if (om_type_store_primitive(&types, type_kinds[kind_index], &result_type)
            != OM_STATUS_OK
        || om_mir_project_init(&project, &types) != OM_STATUS_OK) {
        goto cleanup;
    }
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    if (om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0, result_type,
            &signature) != OM_STATUS_OK
        || om_mir_add_function(&project, (Om_Function_Instance_Id){0},
            OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
            OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) != OM_STATUS_OK
        || om_mir_add_block(&project, function_id, &block_id) != OM_STATUS_OK) {
        goto cleanup;
    }

    const size_t instruction_count = 1u
        + (size_t)(byte_at(data, size, 1) % MAX_INSTRUCTIONS);
    for (size_t index = 0; index < instruction_count; ++index) {
        Om_Mir_Instruction_Spec constant = {
            .opcode = constant_opcodes[kind_index],
            .type_id = result_type,
            .integer_value = word_at(data, size, 2 + index * 4),
        };
        if (om_mir_add_instruction(&project, block_id, &constant, NULL, 0,
                &last_value) != OM_STATUS_OK) {
            goto cleanup;
        }
    }
    if (om_mir_set_terminator(&project, block_id, OM_MIR_RETURN,
            last_value, NULL, 0) != OM_STATUS_OK) {
        goto cleanup;
    }

    Om_Mir_Project candidate = project;
    Om_Mir_Function functions[1] = {project.functions[0]};
    Om_Mir_Block blocks[1] = {project.blocks[0]};
    Om_Mir_Value values[MAX_INSTRUCTIONS] = {0};
    Om_Mir_Instruction instructions[MAX_INSTRUCTIONS] = {0};
    for (size_t index = 0; index < project.value_count; ++index) {
        values[index] = project.values[index];
    }
    for (size_t index = 0; index < project.instruction_count; ++index) {
        instructions[index] = project.instructions[index];
    }
    candidate.functions = functions;
    candidate.blocks = blocks;
    candidate.values = values;
    candidate.instructions = instructions;

    switch (byte_at(data, size, 34) % 5u) {
    case 0:
        blocks[0].instruction_count = word_at(data, size, 35);
        break;
    case 1:
        blocks[0].terminator.kind =
            (Om_Mir_Terminator_Kind)byte_at(data, size, 35);
        break;
    case 2:
        functions[0].entry_block_id.value = word_at(data, size, 35);
        break;
    case 3:
        instructions[byte_at(data, size, 35) % project.instruction_count]
            .opcode = (Om_Mir_Opcode)byte_at(data, size, 36);
        break;
    case 4:
        values[byte_at(data, size, 35) % project.value_count]
            .type_id.value = word_at(data, size, 36);
        break;
    }

    (void)om_mir_verify_project(&candidate, &verification);

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return 0;
}
