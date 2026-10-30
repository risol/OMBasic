#include "ombasic/ir/mir.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/ir/hir.h"
#include "ombasic/ir/query_ir.h"
#include "ombasic/ir/specialize.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/native/metadata/omi.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_store.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/syntax/lexer.h"
#include "ombasic/syntax/parser.h"
#include "ombasic/foundation/memory.h"

#if !defined(OM_MIR_TEST_FIXTURE_ROOT)
#error OM_MIR_TEST_FIXTURE_ROOT must name the MIR fixture directory
#endif
#if !defined(OM_MIR_TEST_GOLDEN_ROOT)
#error OM_MIR_TEST_GOLDEN_ROOT must name the normalized MIR snapshot directory
#endif

#define MIR_ASSERT(expression) \
    do { \
        if (!(expression)) { \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #expression); \
            passed = false; \
            goto cleanup; \
        } \
    } while (false)

static bool has_diagnostic(const Om_Mir_Verification_Result *verification,
                           Om_Mir_Diagnostic_Code code)
{
    for (size_t index = 0; index < verification->diagnostic_count; ++index) {
        if (verification->diagnostics[index].code == code) {
            return true;
        }
    }
    return false;
}

static bool update_mir_goldens;

static bool read_golden(const char *path, char **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL || data == NULL || length == NULL) {
        if (file != NULL) (void)fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return false;
    }
    const long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    const size_t file_length = (size_t)end;
    char *buffer = om_alloc_array(file_length + 1, sizeof(*buffer));
    if (buffer == NULL) {
        (void)fclose(file);
        return false;
    }
    const bool read_all = fread(buffer, 1, file_length, file) == file_length;
    const bool closed = fclose(file) == 0;
    if (!read_all || !closed) {
        om_free(buffer);
        return false;
    }
    buffer[file_length] = '\0';
    *data = buffer;
    *length = file_length;
    return true;
}

static bool write_golden(const char *path, Om_String_View text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    const bool wrote_all = fwrite(text.data, 1, text.length, file)
        == text.length;
    const bool closed = fclose(file) == 0;
    return wrote_all && closed;
}

static bool compare_or_update_golden(const char *name, Om_String_View actual)
{
    char path[4096];
    const int path_length = snprintf(path, sizeof(path), "%s/%s",
        OM_MIR_TEST_GOLDEN_ROOT, name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) return false;
    if (update_mir_goldens) return write_golden(path, actual);
    char *expected = NULL;
    size_t expected_length = 0;
    const bool loaded = read_golden(path, &expected, &expected_length);
    const bool matches = loaded && expected_length == actual.length
        && memcmp(expected, actual.data, actual.length) == 0;
    om_free(expected);
    return matches;
}

static bool test_scalar_function_and_deterministic_dump(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Mir_Text first_dump = {0};
    Om_Mir_Text second_dump = {0};
    bool passed = true;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id constant_id = OM_MIR_VALUE_ID_INVALID;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_INT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec constant = {
        .opcode = OM_MIR_CONST_INT,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .integer_value = 42,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, entry_id, &constant, NULL, 0,
        &constant_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_RETURN,
        constant_id, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(verification.diagnostic_count == 0);
    MIR_ASSERT(om_mir_dump(&project, &first_dump) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_dump(&project, &second_dump) == OM_STATUS_OK);
    MIR_ASSERT(first_dump.length == second_dump.length
        && memcmp(first_dump.data, second_dump.data, first_dump.length) == 0);
    MIR_ASSERT(strstr(first_dump.data, "const-int value=42") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "return v0") != NULL);

cleanup:
    om_mir_text_destroy(&second_dump);
    om_mir_text_destroy(&first_dump);
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_block_parameters_and_dominance(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id then_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id else_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id join_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id condition_id = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id then_value_id = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id else_value_id = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id join_parameter_id = OM_MIR_VALUE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_INT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec condition = {
        .opcode = OM_MIR_CONST_BOOL,
        .type_id = types.primitive_types[OM_TYPE_BOOL],
        .integer_value = 1,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, entry_id, &condition, NULL, 0,
        &condition_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &then_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec then_constant = {
        .opcode = OM_MIR_CONST_INT,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .integer_value = 1,
    };
    const Om_Mir_Instruction_Spec else_constant = {
        .opcode = OM_MIR_CONST_INT,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .integer_value = 2,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, then_id, &then_constant,
        NULL, 0, &then_value_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &else_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_instruction(&project, else_id, &else_constant,
        NULL, 0, &else_value_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &join_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block_parameter(&project, join_id,
        types.primitive_types[OM_TYPE_INT], &join_parameter_id) == OM_STATUS_OK);
    const Om_Mir_Edge_Input condition_edges[] = {
        {.target_block_id = then_id},
        {.target_block_id = else_id},
    };
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_COND_BR,
        condition_id, condition_edges, 2) == OM_STATUS_OK);
    const Om_Mir_Value_Id then_arguments[] = {then_value_id};
    const Om_Mir_Edge_Input then_edge = {
        .target_block_id = join_id,
        .arguments = then_arguments,
        .argument_count = 1,
    };
    MIR_ASSERT(om_mir_set_terminator(&project, then_id, OM_MIR_BR,
        OM_MIR_VALUE_ID_INVALID, &then_edge, 1) == OM_STATUS_OK);
    const Om_Mir_Value_Id else_arguments[] = {else_value_id};
    const Om_Mir_Edge_Input else_edge = {
        .target_block_id = join_id,
        .arguments = else_arguments,
        .argument_count = 1,
    };
    MIR_ASSERT(om_mir_set_terminator(&project, else_id, OM_MIR_BR,
        OM_MIR_VALUE_ID_INVALID, &else_edge, 1) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, join_id, OM_MIR_RETURN,
        join_parameter_id, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(verification.diagnostic_count == 0);

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_bad_edge_type(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id target_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id bool_value_id = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id target_parameter_id = OM_MIR_VALUE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_UNIT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec condition = {
        .opcode = OM_MIR_CONST_BOOL,
        .type_id = types.primitive_types[OM_TYPE_BOOL],
        .integer_value = 1,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, entry_id, &condition, NULL, 0,
        &bool_value_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &target_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block_parameter(&project, target_id,
        types.primitive_types[OM_TYPE_INT], &target_parameter_id) == OM_STATUS_OK);
    const Om_Mir_Value_Id wrong_argument[] = {bool_value_id};
    const Om_Mir_Edge_Input branch = {
        .target_block_id = target_id,
        .arguments = wrong_argument,
        .argument_count = 1,
    };
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_BR,
        OM_MIR_VALUE_ID_INVALID, &branch, 1) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, target_id, OM_MIR_RETURN,
        OM_MIR_VALUE_ID_INVALID, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1102));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_non_dominating_value(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id left_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id right_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id left_value_id = OM_MIR_VALUE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_INT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec condition = {
        .opcode = OM_MIR_CONST_BOOL,
        .type_id = types.primitive_types[OM_TYPE_BOOL],
        .integer_value = 1,
    };
    Om_Mir_Value_Id condition_id = OM_MIR_VALUE_ID_INVALID;
    MIR_ASSERT(om_mir_add_instruction(&project, entry_id, &condition, NULL, 0,
        &condition_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &left_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec constant = {
        .opcode = OM_MIR_CONST_INT,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .integer_value = 5,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, left_id, &constant, NULL, 0,
        &left_value_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &right_id)
        == OM_STATUS_OK);
    const Om_Mir_Edge_Input branches[] = {
        {.target_block_id = left_id},
        {.target_block_id = right_id},
    };
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_COND_BR,
        condition_id, branches, 2) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, left_id, OM_MIR_RETURN,
        left_value_id, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, right_id, OM_MIR_RETURN,
        left_value_id, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1103));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_direct_call_signature(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id integer_parameters[] = {
        {0}, {0},
    };
    Om_Type_Id callee_signature = OM_TYPE_ID_INVALID;
    Om_Type_Id caller_signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id callee_function = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Function_Id caller_function = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id callee_entry = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id caller_entry = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id parameter_left = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id parameter_right = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id sum = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id constant_left = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id constant_right = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id call_result = OM_MIR_VALUE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    integer_parameters[0] = types.primitive_types[OM_TYPE_INT];
    integer_parameters[1] = types.primitive_types[OM_TYPE_INT];
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION,
        integer_parameters, 2, types.primitive_types[OM_TYPE_INT],
        &callee_signature) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_INT], &caller_signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){1},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){1}, callee_signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &callee_function) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, callee_function, &callee_entry)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block_parameter(&project, callee_entry,
        types.primitive_types[OM_TYPE_INT], &parameter_left) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block_parameter(&project, callee_entry,
        types.primitive_types[OM_TYPE_INT], &parameter_right) == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec add = {
        .opcode = OM_MIR_INT_ADD_CHECKED,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .operator_kind = OM_HIR_OPERATOR_ADD,
    };
    const Om_Mir_Value_Id add_operands[] = {parameter_left, parameter_right};
    MIR_ASSERT(om_mir_add_instruction(&project, callee_entry, &add,
        add_operands, 2, &sum) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, callee_entry, OM_MIR_RETURN,
        sum, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, caller_signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &caller_function) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, caller_function, &caller_entry)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec constant = {
        .opcode = OM_MIR_CONST_INT,
        .type_id = types.primitive_types[OM_TYPE_INT],
    };
    MIR_ASSERT(om_mir_add_instruction(&project, caller_entry, &constant,
        NULL, 0, &constant_left) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_instruction(&project, caller_entry, &constant,
        NULL, 0, &constant_right) == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec call = {
        .opcode = OM_MIR_CALL_DIRECT,
        .type_id = types.primitive_types[OM_TYPE_INT],
        .callee_instance_id = {1},
    };
    const Om_Mir_Value_Id call_operands[] = {constant_left, constant_right};
    MIR_ASSERT(om_mir_add_instruction(&project, caller_entry, &call,
        call_operands, 2, &call_result) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, caller_entry, OM_MIR_RETURN,
        call_result, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(verification.diagnostic_count == 0);

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_missing_terminator(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_UNIT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1101));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_effect_in_pure_function(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_UNIT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_SEND_MESSAGE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_RETURN,
        OM_MIR_VALUE_ID_INVALID, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1104));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_blocking_native_handler(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id native_result = OM_MIR_VALUE_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        types.primitive_types[OM_TYPE_UNIT], &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_RECEIVE, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NATIVE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    const Om_Mir_Instruction_Spec native_call = {
        .opcode = OM_MIR_NATIVE_CALL,
        .type_id = types.primitive_types[OM_TYPE_UNIT],
        .effects = OM_EFFECT_NATIVE,
        .flags = OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING,
    };
    MIR_ASSERT(om_mir_add_instruction(&project, entry_id, &native_call,
        NULL, 0, &native_result) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_RETURN,
        native_result, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1104));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_verifier_rejects_unresolved_function_type(void)
{
    Om_Type_Store types = {0};
    Om_Mir_Project project = {0};
    Om_Mir_Verification_Result verification = {0};
    bool passed = true;
    Om_Type_Id variable = OM_TYPE_ID_INVALID;
    Om_Type_Id signature = OM_TYPE_ID_INVALID;
    Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
    Om_Mir_Block_Id entry_id = OM_MIR_BLOCK_ID_INVALID;

    MIR_ASSERT(om_type_store_init(&types) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_variable(&types, 1, &variable) == OM_STATUS_OK);
    MIR_ASSERT(om_type_store_function(&types, OM_TYPE_FUNCTION, NULL, 0,
        variable, &signature) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_project_init(&project, &types) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_function(&project, (Om_Function_Instance_Id){0},
        OM_MIR_FUNCTION, (Om_Module_Id){0}, (Om_Symbol_Id){0}, signature,
        OM_EFFECT_NONE, (Om_Source_Span){0}, &function_id) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_add_block(&project, function_id, &entry_id)
        == OM_STATUS_OK);
    MIR_ASSERT(om_mir_set_terminator(&project, entry_id, OM_MIR_UNREACHABLE,
        OM_MIR_VALUE_ID_INVALID, NULL, 0) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_verify_project(&project, &verification) == OM_STATUS_OK);
    MIR_ASSERT(has_diagnostic(&verification, OM_MIR_DIAGNOSTIC_E1105));

cleanup:
    om_mir_verification_result_destroy(&verification);
    om_mir_project_destroy(&project);
    om_type_store_destroy(&types);
    return passed;
}

static bool test_hir_lowering_to_verified_mir(void)
{
    static const char fixture_root[] = OM_MIR_TEST_FIXTURE_ROOT;
    static const char entry_path[] = "simple.bas";
    static const char metadata_path[] =
        OM_MIR_TEST_FIXTURE_ROOT "/native_math.omi";
    Om_Source_Root root = {0};
    Om_Module_Project source_project = {0};
    Om_Source_Text metadata_source = {0};
    Om_Omi_Module native_metadata = {0};
    char *metadata_bytes = NULL;
    size_t metadata_length = 0;
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Hir_Project hir = {0};
    Om_Hir_Specialization specialization = {0};
    Om_Hir_Verification_Result hir_verification = {0};
    Om_Mir_Project mir = {0};
    Om_Mir_Lowering_Result lowering = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Mir_Text first_dump = {0};
    Om_Mir_Text second_dump = {0};
    bool passed = true;

    MIR_ASSERT(read_golden(metadata_path, &metadata_bytes, &metadata_length));
    MIR_ASSERT(om_source_text_init(&metadata_source, (Om_Source_Id){91},
        (Om_String_View){metadata_path, sizeof(metadata_path) - 1},
        (Om_Const_Byte_Slice){(const uint8_t *)metadata_bytes,
                              metadata_length},
        65536) == OM_STATUS_OK);
    MIR_ASSERT(om_omi_parse(&metadata_source, &native_metadata)
        == OM_STATUS_OK);
    MIR_ASSERT(native_metadata.diagnostic_count == 0);
    const Om_Resolver_Config resolver_config = {
        .native_modules = &native_metadata,
        .native_module_count = 1,
    };
    MIR_ASSERT(om_source_root_init(&root,
        (Om_String_View){fixture_root, sizeof(fixture_root) - 1})
        == OM_STATUS_OK);
    MIR_ASSERT(om_module_project_load(&root,
        (Om_String_View){entry_path, sizeof(entry_path) - 1}, 65536,
        &source_project) == OM_STATUS_OK);
    MIR_ASSERT(om_module_project_module_count(&source_project) == 1);
    const Om_Source_Text *source = om_module_project_source(&source_project,
        (Om_Module_Id){0});
    MIR_ASSERT(source != NULL);
    MIR_ASSERT(om_lexer_scan(source, &lexed) == OM_STATUS_OK);
    MIR_ASSERT(lexed.diagnostic_count == 0);
    MIR_ASSERT(om_parser_parse(&lexed, &parsed) == OM_STATUS_OK);
    MIR_ASSERT(parsed.diagnostic_count == 0);
    MIR_ASSERT(om_name_resolve_project(&source_project, &parsed, 1,
        &resolver_config, &resolution) == OM_STATUS_OK);
    for (size_t index = 0; index < resolution.diagnostic_count; ++index) {
        (void)fprintf(stderr, "MIR resolution diagnostic %s at %u-%u\n",
            om_resolution_diagnostic_code_name(
                resolution.diagnostics[index].code),
            resolution.diagnostics[index].span.start,
            resolution.diagnostics[index].span.end);
    }
    MIR_ASSERT(resolution.diagnostic_count == 0);
    MIR_ASSERT(om_type_infer_project(&source_project, &parsed, 1,
        &resolution, &inference) == OM_STATUS_OK);
    MIR_ASSERT(inference.diagnostic_count == 0);
    MIR_ASSERT(om_semantic_analyze_project(&source_project, &parsed, 1,
        &resolution, &inference, &semantic) == OM_STATUS_OK);
    MIR_ASSERT(semantic.diagnostic_count == 0);
    MIR_ASSERT(om_hir_lower_project(&source_project, &parsed, 1, &resolution,
        &inference, &semantic, &hir) == OM_STATUS_OK);
    MIR_ASSERT(om_hir_verify_project(&hir, &hir_verification) == OM_STATUS_OK);
    if (hir_verification.diagnostic_count != 0) {
        for (size_t index = 0; index < hir_verification.diagnostic_count;
             ++index) {
            const Om_Hir_Diagnostic *diagnostic =
                &hir_verification.diagnostics[index];
            const Om_Hir_Node *node = om_hir_get_node(&hir,
                                                       diagnostic->node_id);
            (void)fprintf(stderr,
                "HIR verifier diagnostic %s at node %u kind=%s symbol=%u\n",
                om_hir_diagnostic_code_name(diagnostic->code),
                diagnostic->node_id.value,
                node == NULL ? "invalid" : om_hir_kind_name(node->kind),
                node == NULL ? UINT32_MAX : node->symbol_id.value);
        }
    }
    MIR_ASSERT(hir_verification.diagnostic_count == 0);
    MIR_ASSERT(om_hir_specialize_project(&hir, &inference.type_store,
        &specialization) == OM_STATUS_OK);
    MIR_ASSERT(specialization.diagnostic_count == 0);
    const Om_Status lowering_status = om_mir_lower_project(&hir,
        &specialization, &mir, &lowering);
    if (lowering_status != OM_STATUS_OK) {
        (void)fprintf(stderr, "HIR-to-MIR returned %s\n",
                      om_status_name(lowering_status));
    }
    MIR_ASSERT(lowering_status == OM_STATUS_OK);
    if (lowering.diagnostic_count != 0) {
        for (size_t index = 0; index < lowering.diagnostic_count; ++index) {
            (void)fprintf(stderr, "HIR-to-MIR diagnostic %s at %u-%u\n",
                om_mir_diagnostic_code_name(lowering.diagnostics[index].code),
                lowering.diagnostics[index].span.start,
                lowering.diagnostics[index].span.end);
        }
    }
    MIR_ASSERT(lowering.diagnostic_count == 0);
    MIR_ASSERT(mir.function_count == 13);
    MIR_ASSERT(om_mir_verify_project(&mir, &verification) == OM_STATUS_OK);
    if (verification.diagnostic_count != 0) {
        for (size_t index = 0; index < verification.diagnostic_count; ++index) {
            const Om_Mir_Diagnostic *diagnostic = &verification.diagnostics[index];
            const Om_Mir_Value *value = diagnostic->value_id.value
                    < mir.value_count
                ? &mir.values[diagnostic->value_id.value] : NULL;
            const Om_Mir_Instruction *instruction = value != NULL
                    && !value->is_block_parameter
                    && value->defining_instruction_id.value
                        < mir.instruction_count
                ? &mir.instructions[value->defining_instruction_id.value] : NULL;
            (void)fprintf(stderr,
                "MIR verifier diagnostic %s in f%u bb%u v%u opcode=%s symbol=%u operands=%u operand_start=%u string=%u+%u total=%zu\n",
                om_mir_diagnostic_code_name(diagnostic->code),
                diagnostic->function_id.value, diagnostic->block_id.value,
                diagnostic->value_id.value,
                instruction == NULL ? "none"
                    : om_mir_opcode_name(instruction->opcode),
                instruction == NULL ? UINT32_MAX : instruction->symbol_id.value,
                instruction == NULL ? 0 : instruction->operand_count,
                instruction == NULL ? 0 : instruction->operand_start,
                instruction == NULL ? 0 : instruction->string_start,
                instruction == NULL ? 0 : instruction->string_length,
                mir.string_data_length);
        }
    }
    MIR_ASSERT(verification.diagnostic_count == 0);
    MIR_ASSERT(om_mir_dump(&mir, &first_dump) == OM_STATUS_OK);
    MIR_ASSERT(om_mir_dump(&mir, &second_dump) == OM_STATUS_OK);
    MIR_ASSERT(first_dump.length == second_dump.length
        && memcmp(first_dump.data, second_dump.data, first_dump.length) == 0);
    MIR_ASSERT(compare_or_update_golden("simple.mir",
        (Om_String_View){first_dump.data, first_dump.length}));
    MIR_ASSERT(strstr(first_dump.data, "int-add-checked") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "bool-and") == NULL);
    MIR_ASSERT(strstr(first_dump.data, "bool-or") == NULL);
    MIR_ASSERT(strstr(first_dump.data, "native-call") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "call-direct") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "function-ref") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "call-indirect") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "cond-br") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "bb") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "runtime-fault") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "runtime-string-create") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "runtime-string-format") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "runtime-string-concat") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "const-null") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "list-create") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "list-length") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "list-get") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "map-create") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "map-length") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "map-key-at") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "map-get") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "result-make-ok") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "result-make-error") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "result-tag") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "result-ok-payload") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "result-error-payload") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "value=\"Hello, OMBasic\"") != NULL);
    MIR_ASSERT(strstr(first_dump.data,
        "value=\"line\\nfeed \xF0\x9F\x98\x80\"") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "value=\"Hello {world}: ") != NULL);

cleanup:
    om_hir_verification_result_destroy(&hir_verification);
    om_mir_text_destroy(&second_dump);
    om_mir_text_destroy(&first_dump);
    om_mir_verification_result_destroy(&verification);
    om_mir_lowering_result_destroy(&lowering);
    om_mir_project_destroy(&mir);
    om_hir_specialization_destroy(&specialization);
    om_hir_project_destroy(&hir);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_module_project_destroy(&source_project);
    om_source_root_destroy(&root);
    om_omi_module_destroy(&native_metadata);
    om_source_text_destroy(&metadata_source);
    om_free(metadata_bytes);
    return passed;
}

static bool test_actor_hir_lowering_to_verified_mir(void)
{
    static const char fixture_root[] = OM_MIR_TEST_FIXTURE_ROOT;
    static const char entry_path[] = "actors.bas";
    Om_Source_Root root = {0};
    Om_Module_Project source_project = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Hir_Project hir = {0};
    Om_Hir_Specialization specialization = {0};
    Om_Hir_Verification_Result hir_verification = {0};
    Om_Mir_Project mir = {0};
    Om_Mir_Lowering_Result lowering = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Mir_Text dump = {0};
    bool passed = true;

    MIR_ASSERT(om_source_root_init(&root,
        (Om_String_View){fixture_root, sizeof(fixture_root) - 1})
        == OM_STATUS_OK);
    MIR_ASSERT(om_module_project_load(&root,
        (Om_String_View){entry_path, sizeof(entry_path) - 1}, 65536,
        &source_project) == OM_STATUS_OK);
    const Om_Source_Text *source = om_module_project_source(&source_project,
        (Om_Module_Id){0});
    MIR_ASSERT(source != NULL);
    MIR_ASSERT(om_lexer_scan(source, &lexed) == OM_STATUS_OK);
    MIR_ASSERT(lexed.diagnostic_count == 0);
    MIR_ASSERT(om_parser_parse(&lexed, &parsed) == OM_STATUS_OK);
    MIR_ASSERT(parsed.diagnostic_count == 0);
    MIR_ASSERT(om_name_resolve_project(&source_project, &parsed, 1, NULL,
        &resolution) == OM_STATUS_OK);
    MIR_ASSERT(resolution.diagnostic_count == 0);
    MIR_ASSERT(om_type_infer_project(&source_project, &parsed, 1,
        &resolution, &inference) == OM_STATUS_OK);
    MIR_ASSERT(inference.diagnostic_count == 0);
    MIR_ASSERT(om_semantic_analyze_project(&source_project, &parsed, 1,
        &resolution, &inference, &semantic) == OM_STATUS_OK);
    MIR_ASSERT(semantic.diagnostic_count == 0);
    MIR_ASSERT(om_hir_lower_project(&source_project, &parsed, 1, &resolution,
        &inference, &semantic, &hir) == OM_STATUS_OK);
    MIR_ASSERT(om_hir_verify_project(&hir, &hir_verification) == OM_STATUS_OK);
    MIR_ASSERT(hir_verification.diagnostic_count == 0);
    MIR_ASSERT(om_hir_specialize_project(&hir, &inference.type_store,
        &specialization) == OM_STATUS_OK);
    MIR_ASSERT(specialization.diagnostic_count == 0);
    const Om_Status lowering_status = om_mir_lower_project(&hir,
        &specialization, &mir, &lowering);
    if (lowering_status != OM_STATUS_OK || lowering.diagnostic_count != 0) {
        (void)fprintf(stderr, "actor HIR-to-MIR failed: %s, diagnostics=%zu\n",
            om_status_name(lowering_status), lowering.diagnostic_count);
    }
    MIR_ASSERT(lowering_status == OM_STATUS_OK);
    MIR_ASSERT(lowering.diagnostic_count == 0);
    MIR_ASSERT(mir.actor_count == 1);
    MIR_ASSERT(mir.actor_state_initializer_count == 1);
    MIR_ASSERT(mir.actor_handler_count == 1);
    MIR_ASSERT(mir.nominal_count == 3);
    MIR_ASSERT(om_mir_verify_project(&mir, &verification) == OM_STATUS_OK);
    if (verification.diagnostic_count != 0) {
        for (size_t index = 0; index < mir.function_count; ++index) {
            (void)fprintf(stderr, "actor MIR function f%zu kind=%u symbol=%u\n",
                index, (unsigned)mir.functions[index].kind,
                mir.functions[index].symbol_id.value);
            const Om_Mir_Function *function = &mir.functions[index];
            Om_Type_View signature_view = {0};
            (void)om_type_store_get(mir.type_store,
                function->signature_type_id, &signature_view);
            for (size_t parameter = 0; parameter + 1 < signature_view.child_count;
                 ++parameter) {
                Om_Type_Id type_id = OM_TYPE_ID_INVALID;
                Om_Type_View type_view = {0};
                (void)om_type_store_child_at(mir.type_store,
                    function->signature_type_id, parameter, &type_id);
                (void)om_type_store_get(mir.type_store, type_id, &type_view);
                (void)fprintf(stderr, "  signature param %zu type=%u kind=%u nominal=%u\n",
                    parameter, type_id.value, (unsigned)type_view.kind,
                    type_view.nominal_symbol.value);
            }
            const Om_Mir_Block *entry = &mir.blocks[function->entry_block_id.value];
            for (size_t parameter = 0; parameter < entry->parameter_count;
                 ++parameter) {
                const Om_Mir_Value *value = &mir.values[
                    entry->parameter_start + parameter];
                Om_Type_View type_view = {0};
                (void)om_type_store_get(mir.type_store, value->type_id,
                                         &type_view);
                (void)fprintf(stderr, "  block param %zu type=%u kind=%u nominal=%u\n",
                    parameter, value->type_id.value, (unsigned)type_view.kind,
                    type_view.nominal_symbol.value);
            }
        }
        for (size_t index = 0; index < mir.instruction_count; ++index) {
            const Om_Mir_Instruction *instruction = &mir.instructions[index];
            (void)fprintf(stderr,
                "actor MIR instruction i%zu f%u bb%u v%u %s operands=%u sym=%u\n",
                index, mir.blocks[instruction->block_id.value].function_id.value,
                instruction->block_id.value, instruction->result_id.value,
                om_mir_opcode_name(instruction->opcode),
                instruction->operand_count, instruction->symbol_id.value);
        }
        for (size_t index = 0; index < verification.diagnostic_count; ++index) {
            const Om_Mir_Diagnostic *diagnostic =
                &verification.diagnostics[index];
            (void)fprintf(stderr,
                "actor MIR diagnostic %s f%u bb%u v%u\n",
                om_mir_diagnostic_code_name(diagnostic->code),
                diagnostic->function_id.value, diagnostic->block_id.value,
                diagnostic->value_id.value);
        }
    }
    MIR_ASSERT(verification.diagnostic_count == 0);
    bool saw_spawn = false;
    bool saw_remote_spawn = false;
    bool saw_send = false;
    bool saw_stop = false;
    bool saw_load_field = false;
    bool saw_store_field = false;
    for (size_t index = 0; index < mir.instruction_count; ++index) {
        const Om_Mir_Opcode opcode = mir.instructions[index].opcode;
        saw_spawn |= opcode == OM_MIR_RUNTIME_ACTOR_SPAWN;
        saw_remote_spawn |= opcode == OM_MIR_RUNTIME_ACTOR_SPAWN
            && mir.instructions[index].operand_count == 1;
        saw_send |= opcode == OM_MIR_RUNTIME_ACTOR_SEND;
        saw_stop |= opcode == OM_MIR_RUNTIME_ACTOR_STOP;
        saw_load_field |= opcode == OM_MIR_LOAD_FIELD;
        saw_store_field |= opcode == OM_MIR_STORE_INIT_FIELD;
    }
    MIR_ASSERT(saw_spawn && saw_send && saw_stop);
    MIR_ASSERT(saw_remote_spawn);
    MIR_ASSERT(saw_load_field && saw_store_field);
    MIR_ASSERT(om_mir_dump(&mir, &dump) == OM_STATUS_OK);
    MIR_ASSERT(compare_or_update_golden("actors.mir",
        (Om_String_View){dump.data, dump.length}));

cleanup:
    om_mir_text_destroy(&dump);
    om_mir_verification_result_destroy(&verification);
    om_mir_lowering_result_destroy(&lowering);
    om_mir_project_destroy(&mir);
    om_hir_verification_result_destroy(&hir_verification);
    om_hir_specialization_destroy(&specialization);
    om_hir_project_destroy(&hir);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_module_project_destroy(&source_project);
    om_source_root_destroy(&root);
    return passed;
}

static bool test_query_plan_dump_is_deterministic(void)
{
    static const char fixture_root[] = OM_MIR_TEST_FIXTURE_ROOT;
    static const char entry_path[] = "query.bas";
    Om_Source_Root root = {0};
    Om_Module_Project source_project = {0};
    Om_Lexer_Result lexed = {0};
    Om_Parser_Result parsed = {0};
    Om_Resolution_Result resolution = {0};
    Om_Type_Inference_Result inference = {0};
    Om_Semantic_Analysis_Result semantic = {0};
    Om_Hir_Project hir = {0};
    Om_Hir_Specialization specialization = {0};
    Om_Mir_Project mir = {0};
    Om_Mir_Lowering_Result lowering = {0};
    Om_Mir_Verification_Result verification = {0};
    Om_Query_Plan plan = {0};
    Om_Query_Text first_dump = {0};
    Om_Query_Text second_dump = {0};
    Om_Mir_Text mir_dump = {0};
    bool passed = true;

    MIR_ASSERT(om_source_root_init(&root,
        (Om_String_View){fixture_root, sizeof(fixture_root) - 1})
        == OM_STATUS_OK);
    MIR_ASSERT(om_module_project_load(&root,
        (Om_String_View){entry_path, sizeof(entry_path) - 1}, 65536,
        &source_project) == OM_STATUS_OK);
    MIR_ASSERT(om_lexer_scan(om_module_project_source(&source_project,
        (Om_Module_Id){0}), &lexed) == OM_STATUS_OK);
    MIR_ASSERT(lexed.diagnostic_count == 0);
    MIR_ASSERT(om_parser_parse(&lexed, &parsed) == OM_STATUS_OK);
    MIR_ASSERT(parsed.diagnostic_count == 0);
    MIR_ASSERT(om_name_resolve_project(&source_project, &parsed, 1, NULL,
        &resolution) == OM_STATUS_OK);
    MIR_ASSERT(resolution.diagnostic_count == 0);
    MIR_ASSERT(om_type_infer_project(&source_project, &parsed, 1, &resolution,
        &inference) == OM_STATUS_OK);
    MIR_ASSERT(inference.diagnostic_count == 0);
    MIR_ASSERT(om_semantic_analyze_project(&source_project, &parsed, 1,
        &resolution, &inference, &semantic) == OM_STATUS_OK);
    MIR_ASSERT(semantic.diagnostic_count == 0);
    MIR_ASSERT(om_hir_lower_project(&source_project, &parsed, 1, &resolution,
        &inference, &semantic, &hir) == OM_STATUS_OK);

    const Om_Hir_Node *query = NULL;
    for (size_t index = 0; index < hir.node_count; ++index) {
        if (hir.nodes[index].kind == OM_HIR_QUERY) {
            query = &hir.nodes[index];
            break;
        }
    }
    MIR_ASSERT(query != NULL);
    MIR_ASSERT(om_query_plan_build(&hir, query->id, &plan) == OM_STATUS_OK);
    MIR_ASSERT(plan.physical_kind == OM_QUERY_PHYSICAL_STABLE_TOP_K);
    MIR_ASSERT(om_query_plan_verify(&hir, &plan) == OM_STATUS_OK);
    MIR_ASSERT(om_query_plan_dump(&hir, &plan, &first_dump) == OM_STATUS_OK);
    MIR_ASSERT(om_query_plan_dump(&hir, &plan, &second_dump) == OM_STATUS_OK);
    MIR_ASSERT(first_dump.length == second_dump.length
        && memcmp(first_dump.data, second_dump.data, first_dump.length) == 0);
    MIR_ASSERT(strstr(first_dump.data, "physical=stable-top-k") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op0 scan") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op1 filter") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op2 filter") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op3 order-key") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op4 take") != NULL);
    MIR_ASSERT(strstr(first_dump.data, "op5 project") != NULL);

    MIR_ASSERT(om_hir_specialize_project(&hir, &inference.type_store,
        &specialization) == OM_STATUS_OK);
    MIR_ASSERT(specialization.diagnostic_count == 0);
    MIR_ASSERT(om_mir_lower_project(&hir, &specialization, &mir, &lowering)
               == OM_STATUS_OK);
    MIR_ASSERT(lowering.diagnostic_count == 0);
    MIR_ASSERT(om_mir_verify_project(&mir, &verification) == OM_STATUS_OK);
    MIR_ASSERT(verification.diagnostic_count == 0);
    MIR_ASSERT(om_mir_dump(&mir, &mir_dump) == OM_STATUS_OK);
    size_t list_create_count = 0;
    size_t stable_order_count = 0;
    size_t direct_call_count = 0;
    for (size_t index = 0; index < mir.instruction_count; ++index) {
        list_create_count += mir.instructions[index].opcode == OM_MIR_LIST_CREATE;
        stable_order_count += mir.instructions[index].opcode
            == OM_MIR_LIST_STABLE_ORDER;
        direct_call_count += mir.instructions[index].opcode == OM_MIR_CALL_DIRECT;
    }
    MIR_ASSERT(list_create_count == 4);
    MIR_ASSERT(stable_order_count == 1);
    MIR_ASSERT(direct_call_count == 1);
    MIR_ASSERT(strstr(mir_dump.data, "list-stable-order") != NULL);

cleanup:
    om_mir_text_destroy(&mir_dump);
    om_query_text_destroy(&second_dump);
    om_query_text_destroy(&first_dump);
    om_query_plan_destroy(&plan);
    om_mir_verification_result_destroy(&verification);
    om_mir_lowering_result_destroy(&lowering);
    om_mir_project_destroy(&mir);
    om_hir_specialization_destroy(&specialization);
    om_hir_project_destroy(&hir);
    om_semantic_analysis_result_destroy(&semantic);
    om_type_inference_result_destroy(&inference);
    om_resolution_result_destroy(&resolution);
    om_parser_result_destroy(&parsed);
    om_lexer_result_destroy(&lexed);
    om_module_project_destroy(&source_project);
    om_source_root_destroy(&root);
    return passed;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--update-goldens") == 0) {
        update_mir_goldens = true;
    } else if (argc != 1) {
        (void)fprintf(stderr, "Usage: ombasic_mir_tests [--update-goldens]\n");
        return 2;
    }
    const bool passed = test_scalar_function_and_deterministic_dump()
        && test_block_parameters_and_dominance()
        && test_verifier_rejects_bad_edge_type()
        && test_verifier_rejects_non_dominating_value()
        && test_direct_call_signature()
        && test_verifier_rejects_missing_terminator()
        && test_verifier_rejects_effect_in_pure_function()
        && test_verifier_rejects_blocking_native_handler()
        && test_verifier_rejects_unresolved_function_type()
        && test_hir_lowering_to_verified_mir()
        && test_actor_hir_lowering_to_verified_mir()
        && test_query_plan_dump_is_deterministic();
    (void)fprintf(stdout, "MIR construction and verifier tests: %s\n",
                  passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
