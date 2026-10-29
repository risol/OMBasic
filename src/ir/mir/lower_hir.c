#include "ombasic/ir/mir.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/semantic/type_store.h"

typedef struct Om_Mir_Env_Entry {
    Om_Symbol_Id symbol_id;
    Om_Mir_Value_Id value_id;
} Om_Mir_Env_Entry;

typedef struct Om_Mir_Lowered_Instance {
    Om_Function_Instance_Id instance_id;
    Om_Mir_Function_Id function_id;
    Om_Hir_Node_Id root_node_id;
} Om_Mir_Lowered_Instance;

typedef struct Om_Mir_Merge_Predecessor {
    Om_Mir_Block_Id block_id;
    Om_Mir_Env_Entry *environment;
    size_t environment_count;
    bool is_conditional_edge;
} Om_Mir_Merge_Predecessor;

typedef struct Om_Mir_Loop_Predecessor {
    Om_Mir_Block_Id block_id;
    Om_Mir_Value_Id *values;
    size_t value_count;
} Om_Mir_Loop_Predecessor;

typedef struct Om_Mir_Loop_Frame {
    Om_Symbol_Id *symbols;
    size_t exit_symbol_count;
    size_t continue_symbol_count;
    Om_Mir_Block_Id header_block_id;
    Om_Vector break_predecessors;
    Om_Vector continue_predecessors;
} Om_Mir_Loop_Frame;

typedef struct Om_Mir_Test_Block {
    Om_Mir_Block_Id block_id;
    Om_Mir_Block_Id true_block_id;
    Om_Mir_Block_Id false_block_id;
    Om_Mir_Value_Id condition_value_id;
} Om_Mir_Test_Block;

typedef struct Om_Mir_Lowering_Context {
    const Om_Hir_Project *hir;
    const Om_Hir_Specialization *specialization;
    Om_Mir_Project *mir;
    Om_Mir_Lowering_Result *result;
    Om_Mir_Function_Id function_id;
    Om_Function_Instance_Id instance_id;
    Om_Mir_Block_Id block_id;
    Om_Mir_Value_Id actor_state_value_id;
    Om_Vector lowered_instances;
    Om_Vector environment;
    Om_Vector loop_frames;
    size_t next_synthetic_symbol_id;
    bool terminated;
    bool unsupported;
} Om_Mir_Lowering_Context;

static Om_Status lower_expression(Om_Mir_Lowering_Context *context,
                                  Om_Hir_Node_Id node_id,
                                  Om_Mir_Value_Id *out_value);
static bool types_match(const Om_Type_Store *store,
                        Om_Type_Id left,
                        Om_Type_Id right);
static Om_Status set_branch(Om_Mir_Lowering_Context *context,
                            Om_Mir_Block_Id source_block_id,
                            Om_Mir_Block_Id target_block_id,
                            const Om_Mir_Value_Id *arguments,
                            size_t argument_count);
static Om_Status set_conditional_branch(Om_Mir_Lowering_Context *context,
                                        Om_Mir_Block_Id source_block,
                                        Om_Mir_Value_Id condition,
                                        Om_Mir_Block_Id true_block,
                                        Om_Mir_Block_Id false_block);

static Om_Vector *diagnostic_vector(Om_Mir_Lowering_Result *result)
{
    return result == NULL ? NULL : result->owned_diagnostics;
}

static const Om_Hir_Node *hir_node(const Om_Hir_Project *hir,
                                  Om_Hir_Node_Id node_id)
{
    return om_hir_get_node(hir, node_id);
}

static const Om_Hir_Node *hir_child(const Om_Hir_Project *hir,
                                    const Om_Hir_Node *node,
                                    size_t index)
{
    return om_hir_child_at(hir, node, index);
}

static bool symbol_is_actor_state(const Om_Mir_Lowering_Context *context,
                                  Om_Symbol_Id symbol_id)
{
    if (context == NULL || context->hir == NULL
        || context->hir->resolution == NULL) {
        return false;
    }
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->hir->resolution, symbol_id);
    return symbol != NULL
        && symbol->scope_id.value < context->hir->resolution->scope_count
        && context->hir->resolution->scopes[symbol->scope_id.value].kind
            == OM_SCOPE_ACTOR;
}

static Om_Status add_lowering_diagnostic(Om_Mir_Lowering_Context *context,
                                         Om_Source_Span span)
{
    Om_Vector *diagnostics = diagnostic_vector(context->result);
    if (diagnostics == NULL || diagnostics->length >= UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Mir_Diagnostic diagnostic = {
        .code = OM_MIR_DIAGNOSTIC_E1106,
        .function_id = context->function_id,
        .block_id = context->block_id,
        .value_id = OM_MIR_VALUE_ID_INVALID,
        .span = span,
    };
    const Om_Status status = om_vector_push(diagnostics, &diagnostic);
    if (status == OM_STATUS_OK) {
        context->unsupported = true;
    }
    return status;
}

static Om_Status specialized_type(Om_Mir_Lowering_Context *context,
                                  Om_Type_Id source_type,
                                  Om_Type_Id *out_type)
{
    return om_hir_specialization_resolve_type(context->hir,
        context->specialization, context->instance_id, source_type, out_type);
}

static Om_Status resolved_type_kind(const Om_Type_Store *store,
                                    Om_Type_Id type_id,
                                    Om_Type_Kind *kind)
{
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(store, type_id, &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved, &view);
    }
    if (status == OM_STATUS_OK) {
        *kind = view.kind;
    }
    return status;
}

static Om_Status mir_value_type(const Om_Mir_Project *mir,
                                Om_Mir_Value_Id value_id,
                                Om_Type_Id *type_id)
{
    if (value_id.value >= mir->value_count || type_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *type_id = mir->values[value_id.value].type_id;
    return OM_STATUS_OK;
}

static Om_Status emit_instruction(Om_Mir_Lowering_Context *context,
                                  const Om_Hir_Node *source,
                                  Om_Mir_Opcode opcode,
                                  Om_Type_Id type_id,
                                  Om_Hir_Operator operator_kind,
                                  Om_Function_Instance_Id callee_instance_id,
                                  const Om_Mir_Value_Id *operands,
                                  size_t operand_count,
                                  uint64_t integer_value,
                                  double float_value,
                                  Om_Mir_Value_Id *out_value)
{
    const Om_Mir_Instruction_Spec spec = {
        .opcode = opcode,
        .type_id = type_id,
        .operator_kind = operator_kind,
        .callee_instance_id = callee_instance_id,
        .symbol_id = source->symbol_id,
        .span = source->span,
        .effects = source->effects,
        .integer_value = integer_value,
        .float_value = float_value,
    };
    return om_mir_add_instruction(context->mir, context->block_id, &spec,
        operands, operand_count, out_value);
}

static Om_Status append_string_byte(Om_Vector *bytes, unsigned char byte)
{
    const char value = (char)byte;
    return om_vector_push(bytes, &value);
}

static Om_Status append_unicode_scalar(Om_Vector *bytes, uint32_t scalar)
{
    if (scalar <= UINT32_C(0x7f)) {
        return append_string_byte(bytes, (unsigned char)scalar);
    }
    if (scalar <= UINT32_C(0x7ff)) {
        Om_Status status = append_string_byte(bytes,
            (unsigned char)(UINT32_C(0xc0) | (scalar >> 6)));
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | (scalar & 0x3f)));
        }
        return status;
    }
    if (scalar >= UINT32_C(0xd800) && scalar <= UINT32_C(0xdfff)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (scalar <= UINT32_C(0xffff)) {
        Om_Status status = append_string_byte(bytes,
            (unsigned char)(UINT32_C(0xe0) | (scalar >> 12)));
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | ((scalar >> 6) & 0x3f)));
        }
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | (scalar & 0x3f)));
        }
        return status;
    }
    if (scalar <= UINT32_C(0x10ffff)) {
        Om_Status status = append_string_byte(bytes,
            (unsigned char)(UINT32_C(0xf0) | (scalar >> 18)));
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | ((scalar >> 12) & 0x3f)));
        }
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | ((scalar >> 6) & 0x3f)));
        }
        if (status == OM_STATUS_OK) {
            status = append_string_byte(bytes,
                (unsigned char)(UINT32_C(0x80) | (scalar & 0x3f)));
        }
        return status;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static int string_hex_value(unsigned char byte)
{
    if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
        return (int)(byte - (unsigned char)'0');
    }
    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'f') {
        return (int)(byte - (unsigned char)'a') + 10;
    }
    if (byte >= (unsigned char)'A' && byte <= (unsigned char)'F') {
        return (int)(byte - (unsigned char)'A') + 10;
    }
    return -1;
}

static Om_Status decode_string_contents(Om_String_View source,
                                       size_t start,
                                       size_t end,
                                       bool collapse_braces,
                                       Om_Vector *decoded)
{
    if (source.data == NULL || start > end || end > source.length
        || decoded == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = om_vector_init(decoded, sizeof(char));
    for (size_t index = start; status == OM_STATUS_OK && index < end; ++index) {
        const unsigned char byte = (unsigned char)source.data[index];
        if (collapse_braces && (byte == (unsigned char)'{'
                || byte == (unsigned char)'}')
            && index + 1 < end && source.data[index + 1] == (char)byte) {
            status = append_string_byte(decoded, byte);
            ++index;
            continue;
        }
        if (byte != (unsigned char)'\\') {
            status = append_string_byte(decoded, byte);
            continue;
        }
        if (++index >= end) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const unsigned char escape = (unsigned char)source.data[index];
        switch (escape) {
        case '\\': status = append_string_byte(decoded, (unsigned char)'\\'); break;
        case '"': status = append_string_byte(decoded, (unsigned char)'"'); break;
        case 'n': status = append_string_byte(decoded, (unsigned char)'\n'); break;
        case 'r': status = append_string_byte(decoded, (unsigned char)'\r'); break;
        case 't': status = append_string_byte(decoded, (unsigned char)'\t'); break;
        case '0': status = append_string_byte(decoded, (unsigned char)'\0'); break;
        case 'u': {
            if (index + 1 >= end || source.data[index + 1] != '{') {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            index += 2;
            uint32_t scalar = 0;
            size_t digits = 0;
            while (index < end && source.data[index] != '}') {
                const int digit = string_hex_value(
                    (unsigned char)source.data[index]);
                if (digit < 0 || digits == 6
                    || scalar > (UINT32_C(0x10ffff) - (uint32_t)digit) / 16) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                scalar = scalar * 16 + (uint32_t)digit;
                ++digits;
                ++index;
            }
            if (status == OM_STATUS_OK
                && (digits == 0 || index >= end
                    || source.data[index] != '}')) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                status = append_unicode_scalar(decoded, scalar);
            }
            break;
        }
        default: status = OM_STATUS_INVALID_ARGUMENT; break;
        }
    }
    if (status != OM_STATUS_OK) {
        om_vector_destroy(decoded);
    }
    return status;
}

static Om_Status decode_string_literal(Om_String_View source,
                                       Om_Vector *decoded)
{
    if (source.data == NULL || source.length < 2
        || source.data[0] != '"' || source.data[source.length - 1] != '"') {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return decode_string_contents(source, 1, source.length - 1, false, decoded);
}

static Om_Status decode_string_text(Om_String_View source,
                                    Om_Vector *decoded)
{
    return decode_string_contents(source, 0, source.length, true, decoded);
}

static Om_Status emit_string_literal(Om_Mir_Lowering_Context *context,
                                     const Om_Hir_Node *node,
                                     Om_Type_Id type_id,
                                     Om_String_View value,
                                     Om_Mir_Value_Id *out_value)
{
    const Om_Mir_Instruction_Spec spec = {
        .opcode = OM_MIR_RUNTIME_STRING_CREATE,
        .type_id = type_id,
        .symbol_id = node->symbol_id,
        .span = node->span,
        .effects = node->effects,
        .string_value = value,
    };
    return om_mir_add_instruction(context->mir, context->block_id, &spec,
        NULL, 0, out_value);
}

static Om_Status emit_string_operation(Om_Mir_Lowering_Context *context,
                                       const Om_Hir_Node *source,
                                       Om_Mir_Opcode opcode,
                                       Om_Type_Id type_id,
                                       const Om_Mir_Value_Id *operands,
                                       size_t operand_count,
                                       Om_Mir_Value_Id *out_value)
{
    const Om_Mir_Instruction_Spec spec = {
        .opcode = opcode,
        .type_id = type_id,
        .symbol_id = source->symbol_id,
        .span = source->span,
        .effects = source->effects,
    };
    return om_mir_add_instruction(context->mir, context->block_id, &spec,
        operands, operand_count, out_value);
}

static Om_Status lower_list_literal(Om_Mir_Lowering_Context *context,
                                    const Om_Hir_Node *node,
                                    Om_Type_Id type_id,
                                    Om_Mir_Value_Id *out_value)
{
    Om_Mir_Value_Id *operands = node->child_count == 0 ? NULL
        : om_alloc_array(node->child_count, sizeof(*operands));
    if (node->child_count != 0 && operands == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count;
         ++index) {
        const Om_Hir_Node *element = hir_child(context->hir, node, index);
        status = element == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, element->id, &operands[index]);
        if (context->unsupported) {
            break;
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = emit_instruction(context, node, OM_MIR_LIST_CREATE, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, operands,
            node->child_count, 0, 0.0, out_value);
    }
    om_free(operands);
    return status;
}

static Om_Status lower_map_literal(Om_Mir_Lowering_Context *context,
                                   const Om_Hir_Node *node,
                                   Om_Type_Id type_id,
                                   Om_Mir_Value_Id *out_value)
{
    size_t operand_count = 0;
    if (!om_checked_size_mul(node->child_count, 2, &operand_count)) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Mir_Value_Id *operands = operand_count == 0 ? NULL
        : om_alloc_array(operand_count, sizeof(*operands));
    if (operand_count != 0 && operands == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count;
         ++index) {
        const Om_Hir_Node *entry = hir_child(context->hir, node, index);
        if (entry == NULL || entry->kind != OM_HIR_MAP_ENTRY
            || entry->child_count != 2) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        for (size_t part_index = 0; status == OM_STATUS_OK
             && part_index < 2; ++part_index) {
            const Om_Hir_Node *part = hir_child(context->hir, entry, part_index);
            status = part == NULL ? OM_STATUS_INVALID_ARGUMENT
                : lower_expression(context, part->id,
                    &operands[index * 2 + part_index]);
            if (context->unsupported) {
                break;
            }
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = emit_instruction(context, node, OM_MIR_MAP_CREATE, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, operands,
            operand_count, 0, 0.0, out_value);
    }
    om_free(operands);
    return status;
}

static Om_Status lower_index(Om_Mir_Lowering_Context *context,
                             const Om_Hir_Node *node,
                             Om_Type_Id result_type,
                             Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *collection = hir_child(context->hir, node, 0);
    const Om_Hir_Node *key = hir_child(context->hir, node, 1);
    if (collection == NULL || key == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id operands[2] = {
        OM_MIR_VALUE_ID_INVALID, OM_MIR_VALUE_ID_INVALID,
    };
    Om_Status status = lower_expression(context, collection->id, &operands[0]);
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = lower_expression(context, key->id, &operands[1]);
    }
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Type_Id collection_type = OM_TYPE_ID_INVALID;
    Om_Type_Kind kind = OM_TYPE_VARIABLE;
    status = mir_value_type(context->mir, operands[0], &collection_type);
    if (status == OM_STATUS_OK) {
        status = resolved_type_kind(context->mir->type_store, collection_type,
                                    &kind);
    }
    Om_Mir_Opcode opcode = OM_MIR_LIST_GET;
    if (status == OM_STATUS_OK && kind == OM_TYPE_MAP) {
        opcode = OM_MIR_MAP_GET;
    } else if (status == OM_STATUS_OK && kind != OM_TYPE_LIST) {
        return add_lowering_diagnostic(context, node->span);
    }
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, opcode, result_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, operands,
            2, 0, 0.0, out_value);
    }
    return status;
}

static Om_Status lower_result_constructor(Om_Mir_Lowering_Context *context,
                                          const Om_Hir_Node *node,
                                          Om_Type_Id result_type,
                                          Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *payload_node = hir_child(context->hir, node, 1);
    if (payload_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id payload = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, payload_node->id, &payload);
    if (status == OM_STATUS_OK && !context->unsupported) {
        const Om_Mir_Opcode opcode = node->kind == OM_HIR_RESULT_OK
            ? OM_MIR_RESULT_MAKE_OK : OM_MIR_RESULT_MAKE_ERROR;
        status = emit_instruction(context, node, opcode, result_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, &payload, 1,
            0, 0.0, out_value);
    }
    return status;
}

static Om_Status lower_try(Om_Mir_Lowering_Context *context,
                           const Om_Hir_Node *node,
                           Om_Type_Id value_type,
                           Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *input_node = hir_child(context->hir, node, 0);
    if (input_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id input_value = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, input_node->id, &input_value);
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Type_Id input_type_id = OM_TYPE_ID_INVALID;
    Om_Type_View input_type = {0};
    status = mir_value_type(context->mir, input_value, &input_type_id);
    if (status == OM_STATUS_OK) {
        status = om_type_store_resolve(context->mir->type_store, input_type_id,
                                       &input_type_id);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(context->mir->type_store, input_type_id,
                                   &input_type);
    }
    Om_Type_Id success_type = OM_TYPE_ID_INVALID;
    Om_Type_Id error_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK && input_type.kind == OM_TYPE_RESULT
        && input_type.child_count == 2) {
        status = om_type_store_child_at(context->mir->type_store,
            input_type_id, 0, &success_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(context->mir->type_store,
                input_type_id, 1, &error_type);
        }
    } else if (status == OM_STATUS_OK) {
        status = add_lowering_diagnostic(context, node->span);
    }
    if (status == OM_STATUS_OK
        && !types_match(context->mir->type_store, value_type, success_type)) {
        status = add_lowering_diagnostic(context, node->span);
    }
    const Om_Mir_Function *function = context->function_id.value
            < context->mir->function_count
        ? &context->mir->functions[context->function_id.value] : NULL;
    const bool receive_handler = function != NULL
        && function->kind == OM_MIR_RECEIVE;
    Om_Type_Id function_type_id = OM_TYPE_ID_INVALID;
    Om_Type_View function_type = {0};
    Om_Type_Id function_result_type = OM_TYPE_ID_INVALID;
    Om_Type_View function_result_view = {0};
    if (status == OM_STATUS_OK && receive_handler) {
        /* A failed TRY in a Receive marks its Actor failed instead of
           propagating a Result through the Unit-returning handler ABI. */
    } else if (status == OM_STATUS_OK && function != NULL) {
        status = om_type_store_resolve(context->mir->type_store,
            function->signature_type_id, &function_type_id);
    } else if (status == OM_STATUS_OK) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK && !receive_handler) {
        status = om_type_store_get(context->mir->type_store, function_type_id,
                                   &function_type);
    }
    if (status == OM_STATUS_OK && !receive_handler
        && function_type.child_count != 0) {
        status = om_type_store_child_at(context->mir->type_store,
            function_type_id, function_type.child_count - 1,
            &function_result_type);
    } else if (status == OM_STATUS_OK && !receive_handler) {
        status = add_lowering_diagnostic(context, node->span);
    }
    if (status == OM_STATUS_OK && !receive_handler) {
        status = om_type_store_resolve(context->mir->type_store,
            function_result_type, &function_result_type);
    }
    if (status == OM_STATUS_OK && !receive_handler) {
        status = om_type_store_get(context->mir->type_store,
            function_result_type, &function_result_view);
    }
    if (status == OM_STATUS_OK && !receive_handler
        && (function_result_view.kind != OM_TYPE_RESULT
            || function_result_view.child_count != 2)) {
        status = add_lowering_diagnostic(context, node->span);
    }
    Om_Type_Id function_error_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK && !receive_handler) {
        status = om_type_store_child_at(context->mir->type_store,
            function_result_type, 1, &function_error_type);
    }
    if (status == OM_STATUS_OK && !receive_handler
        && !types_match(context->mir->type_store, error_type,
                        function_error_type)) {
        status = add_lowering_diagnostic(context, node->span);
    }
    Om_Type_Id bool_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_primitive(context->mir->type_store, OM_TYPE_BOOL,
                                         &bool_type);
    }
    Om_Mir_Value_Id is_ok = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_RESULT_TAG, bool_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            &input_value, 1, 0, 0.0, &is_ok);
    }
    const Om_Mir_Block_Id source_block = context->block_id;
    Om_Mir_Block_Id success_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &success_block);
    }
    context->block_id = success_block;
    context->terminated = false;
    Om_Mir_Value_Id success_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_RESULT_OK_PAYLOAD,
            value_type, OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            &input_value, 1, 0, 0.0, &success_value);
    }
    Om_Mir_Block_Id error_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &error_block);
    }
    context->block_id = error_block;
    context->terminated = false;
    Om_Mir_Value_Id error_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node,
            OM_MIR_RESULT_ERROR_PAYLOAD, error_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            &input_value, 1, 0, 0.0, &error_value);
    }
    Om_Mir_Value_Id propagated_error = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK && receive_handler) {
        Om_Type_Id unit_type = OM_TYPE_ID_INVALID;
        status = om_type_store_primitive(context->mir->type_store,
            OM_TYPE_UNIT, &unit_type);
        if (status == OM_STATUS_OK) {
            status = emit_instruction(context, node, OM_MIR_RUNTIME_FAULT,
                unit_type, OM_HIR_OPERATOR_NONE,
                OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
                OM_MIR_RUNTIME_FAULT_ACTOR_FAILED, 0.0,
                &propagated_error);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_terminator(context->mir, error_block,
                OM_MIR_UNREACHABLE, OM_MIR_VALUE_ID_INVALID, NULL, 0);
        }
    } else if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_RESULT_MAKE_ERROR,
            function_result_type, OM_HIR_OPERATOR_NONE,
            OM_FUNCTION_INSTANCE_ID_INVALID, &error_value, 1, 0, 0.0,
            &propagated_error);
    }
    if (status == OM_STATUS_OK && !receive_handler) {
        status = om_mir_set_terminator(context->mir, error_block,
            OM_MIR_RETURN, propagated_error, NULL, 0);
    }
    Om_Mir_Block_Id merge_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &merge_block);
    }
    Om_Mir_Value_Id merge_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block_parameter(context->mir, merge_block,
                                             value_type, &merge_value);
    }
    if (status == OM_STATUS_OK) {
        status = set_branch(context, success_block, merge_block,
                            &success_value, 1);
    }
    if (status == OM_STATUS_OK) {
        status = set_conditional_branch(context, source_block, is_ok,
            success_block, error_block);
    }
    if (status == OM_STATUS_OK) {
        context->block_id = merge_block;
        context->terminated = false;
        *out_value = merge_value;
    }
    return status;
}

static Om_Status synthetic_symbol_id(Om_Mir_Lowering_Context *context,
                                     Om_Symbol_Id *symbol_id)
{
    if (context == NULL || symbol_id == NULL
        || context->next_synthetic_symbol_id >= UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *symbol_id = (Om_Symbol_Id){
        (uint32_t)context->next_synthetic_symbol_id,
    };
    ++context->next_synthetic_symbol_id;
    return OM_STATUS_OK;
}

static const Om_Hir_Entity *entity_for_symbol(
    const Om_Mir_Lowering_Context *context,
    Om_Symbol_Id symbol_id,
    Om_Hir_Entity_Kind kind)
{
    for (size_t index = 0; index < context->hir->entity_count; ++index) {
        const Om_Hir_Entity *entity = &context->hir->entities[index];
        if (entity->kind == kind
            && entity->symbol_id.value == symbol_id.value) {
            return entity;
        }
    }
    return NULL;
}

static const Om_Hir_Node *field_definition(
    const Om_Mir_Lowering_Context *context,
    Om_Type_Id nominal_type,
    Om_String_View field_name)
{
    Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
    Om_Type_View type_view = {0};
    if (om_type_store_resolve(context->mir->type_store, nominal_type,
                              &resolved_type) != OM_STATUS_OK
        || om_type_store_get(context->mir->type_store, resolved_type,
                             &type_view) != OM_STATUS_OK) {
        return NULL;
    }
    Om_Hir_Entity_Kind entity_kind = OM_HIR_ENTITY_RECORD;
    if (type_view.kind == OM_TYPE_MESSAGE) {
        entity_kind = OM_HIR_ENTITY_MESSAGE;
    } else if (type_view.kind != OM_TYPE_RECORD) {
        return NULL;
    }
    const Om_Hir_Entity *entity = entity_for_symbol(context,
        type_view.nominal_symbol, entity_kind);
    const Om_Hir_Node *declaration = entity == NULL ? NULL
        : hir_node(context->hir, entity->node_id);
    if (declaration == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < declaration->child_count; ++index) {
        const Om_Hir_Node *field = hir_child(context->hir, declaration, index);
        if (field != NULL && field->kind == OM_HIR_PARAMETER
            && om_string_view_equal(field->name, field_name)) {
            return field;
        }
    }
    return NULL;
}

static Om_Status emit_instruction_with_symbol(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *source,
    Om_Mir_Opcode opcode,
    Om_Type_Id type_id,
    Om_Symbol_Id symbol_id,
    const Om_Mir_Value_Id *operands,
    size_t operand_count,
    Om_Mir_Value_Id *out_value)
{
    const Om_Mir_Instruction_Spec spec = {
        .opcode = opcode,
        .type_id = type_id,
        .symbol_id = symbol_id,
        .span = source->span,
        .effects = source->effects,
    };
    return om_mir_add_instruction(context->mir, context->block_id, &spec,
        operands, operand_count, out_value);
}

static Om_Status lower_nominal_constructor(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *node,
    Om_Type_Id result_type,
    Om_Hir_Entity_Kind entity_kind,
    Om_Mir_Value_Id *out_value)
{
    const Om_Hir_Entity *entity = entity_for_symbol(context, node->symbol_id,
                                                    entity_kind);
    const Om_Hir_Node *declaration = entity == NULL ? NULL
        : hir_node(context->hir, entity->node_id);
    const Om_Type_Kind type_kind = entity_kind == OM_HIR_ENTITY_MESSAGE
        ? OM_TYPE_MESSAGE : OM_TYPE_RECORD;
    const Om_Mir_Nominal_Descriptor *nominal = NULL;
    for (size_t index = 0; index < context->mir->nominal_count; ++index) {
        const Om_Mir_Nominal_Descriptor *candidate =
            &context->mir->nominals[index];
        if (candidate->symbol_id.value == node->symbol_id.value
            && candidate->type_kind == type_kind) {
            nominal = candidate;
            break;
        }
    }
    if (node->child_count == 0 || nominal == NULL
        || nominal->field_count != node->child_count - 1
        || (declaration != NULL
            && declaration->child_count != node->child_count - 1)) {
        return add_lowering_diagnostic(context, node->span);
    }
    const size_t operand_count = node->child_count - 1;
    Om_Mir_Value_Id *operands = operand_count == 0 ? NULL
        : om_alloc_array(operand_count, sizeof(*operands));
    if (operand_count != 0 && operands == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < operand_count;
         ++index) {
        const Om_Hir_Node *argument = hir_child(context->hir, node, index + 1);
        status = argument == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, argument->id, &operands[index]);
        if (context->unsupported) {
            break;
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = emit_instruction(context, node, OM_MIR_ALLOC_OBJECT,
            result_type, OM_HIR_OPERATOR_NONE,
            OM_FUNCTION_INSTANCE_ID_INVALID, operands, operand_count, 0, 0.0,
            out_value);
    }
    om_free(operands);
    return status;
}

static Om_Status lower_field_read(Om_Mir_Lowering_Context *context,
                                  const Om_Hir_Node *node,
                                  Om_Type_Id result_type,
                                  Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 1 || node->symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *base_node = hir_child(context->hir, node, 0);
    if (base_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id base = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, base_node->id, &base);
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Type_Id base_type = OM_TYPE_ID_INVALID;
    status = mir_value_type(context->mir, base, &base_type);
    const Om_Hir_Node *field = status == OM_STATUS_OK
        ? field_definition(context, base_type, node->name) : NULL;
    if (status == OM_STATUS_OK
        && (field == NULL || field->symbol_id.value != node->symbol_id.value)) {
        return add_lowering_diagnostic(context, node->span);
    }
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_LOAD_FIELD, result_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, &base, 1,
            0, 0.0, out_value);
    }
    return status;
}

static Om_Status lower_record_with(Om_Mir_Lowering_Context *context,
                                   const Om_Hir_Node *node,
                                   Om_Type_Id result_type,
                                   Om_Mir_Value_Id *out_value)
{
    if (node->child_count < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *base_node = hir_child(context->hir, node, 0);
    if (base_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id current = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, base_node->id, &current);
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Type_Id base_type = OM_TYPE_ID_INVALID;
    status = mir_value_type(context->mir, current, &base_type);
    Om_Type_Kind base_kind = OM_TYPE_VARIABLE;
    if (status == OM_STATUS_OK) {
        status = resolved_type_kind(context->mir->type_store, base_type,
                                    &base_kind);
    }
    if (status == OM_STATUS_OK && base_kind != OM_TYPE_RECORD) {
        return add_lowering_diagnostic(context, node->span);
    }
    for (size_t index = 1; status == OM_STATUS_OK
         && !context->unsupported && index < node->child_count; ++index) {
        const Om_Hir_Node *update = hir_child(context->hir, node, index);
        if (update == NULL || update->kind != OM_HIR_FIELD_UPDATE
            || update->child_count != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Hir_Node *field = field_definition(context, base_type,
                                                    update->name);
        const Om_Hir_Node *value_node = hir_child(context->hir, update, 0);
        if (field == NULL || value_node == NULL
            || field->symbol_id.value == UINT32_MAX) {
            status = add_lowering_diagnostic(context, update->span);
            break;
        }
        Om_Type_Id field_type = OM_TYPE_ID_INVALID;
        status = specialized_type(context, field->type_id, &field_type);
        Om_Mir_Value_Id value = OM_MIR_VALUE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = lower_expression(context, value_node->id, &value);
        }
        Om_Type_Id actual_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = mir_value_type(context->mir, value, &actual_type);
        }
        if (status == OM_STATUS_OK && !context->unsupported
            && !types_match(context->mir->type_store, field_type, actual_type)) {
            status = add_lowering_diagnostic(context, update->span);
        }
        if (status == OM_STATUS_OK && !context->unsupported) {
            const Om_Mir_Value_Id operands[] = {current, value};
            status = emit_instruction_with_symbol(context, update,
                OM_MIR_COPY_MANAGED, result_type, field->symbol_id, operands,
                2, &current);
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        *out_value = current;
    }
    return status;
}

static Om_Status lower_interpolated_string(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *node,
    Om_Type_Id type_id,
    Om_Mir_Value_Id *out_value)
{
    Om_Mir_Value_Id result = OM_MIR_VALUE_ID_INVALID;
    if (node->child_count == 0) {
        const Om_String_View empty = {"", 0};
        return emit_string_literal(context, node, type_id, empty, out_value);
    }
    for (size_t index = 0; index < node->child_count; ++index) {
        const Om_Hir_Node *part = hir_child(context->hir, node, index);
        if (part == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Mir_Value_Id part_value = OM_MIR_VALUE_ID_INVALID;
        Om_Status status = OM_STATUS_OK;
        if (part->kind == OM_HIR_STRING_TEXT) {
            Om_Vector decoded = {0};
            status = decode_string_text(part->name, &decoded);
            if (status == OM_STATUS_OK) {
                const Om_String_View value = {decoded.data, decoded.length};
                status = emit_string_literal(context, part, type_id, value,
                                             &part_value);
            }
            om_vector_destroy(&decoded);
        } else {
            status = lower_expression(context, part->id, &part_value);
            if (status == OM_STATUS_OK && !context->unsupported) {
                const Om_Mir_Value_Id operand = part_value;
                status = emit_string_operation(context, part,
                    OM_MIR_RUNTIME_STRING_FORMAT, type_id, &operand, 1,
                    &part_value);
            }
        }
        if (status != OM_STATUS_OK || context->unsupported) {
            return status;
        }
        if (index == 0) {
            result = part_value;
        } else {
            const Om_Mir_Value_Id operands[] = {result, part_value};
            status = emit_string_operation(context, node,
                OM_MIR_RUNTIME_STRING_CONCAT, type_id, operands, 2, &result);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    *out_value = result;
    return OM_STATUS_OK;
}

static Om_Mir_Env_Entry *environment_entry(Om_Mir_Lowering_Context *context,
                                           Om_Symbol_Id symbol_id)
{
    for (size_t index = context->environment.length; index > 0; --index) {
        Om_Mir_Env_Entry *entry = om_vector_at(&context->environment, index - 1);
        if (entry != NULL && entry->symbol_id.value == symbol_id.value) {
            return entry;
        }
    }
    return NULL;
}

static Om_Status environment_set(Om_Mir_Lowering_Context *context,
                                 Om_Symbol_Id symbol_id,
                                 Om_Mir_Value_Id value_id,
                                 bool add_if_missing)
{
    Om_Mir_Env_Entry *existing = environment_entry(context, symbol_id);
    if (existing != NULL) {
        existing->value_id = value_id;
        return OM_STATUS_OK;
    }
    if (!add_if_missing) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Mir_Env_Entry entry = {symbol_id, value_id};
    return om_vector_push(&context->environment, &entry);
}

static Om_Status environment_snapshot(const Om_Mir_Lowering_Context *context,
                                      Om_Mir_Env_Entry **entries,
                                      size_t *entry_count)
{
    if (entries == NULL || entry_count == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *entries = NULL;
    *entry_count = context->environment.length;
    if (*entry_count == 0) {
        return OM_STATUS_OK;
    }
    *entries = om_alloc_array(*entry_count, sizeof(**entries));
    if (*entries == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memcpy(*entries, context->environment.data,
                 *entry_count * sizeof(**entries));
    return OM_STATUS_OK;
}

static Om_Status environment_restore(Om_Mir_Lowering_Context *context,
                                     const Om_Mir_Env_Entry *entries,
                                     size_t entry_count)
{
    om_vector_clear(&context->environment);
    for (size_t index = 0; index < entry_count; ++index) {
        const Om_Status status = om_vector_push(&context->environment,
                                               &entries[index]);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status environment_from_block_parameters(
    Om_Mir_Lowering_Context *context,
    const Om_Mir_Env_Entry *symbols,
    const Om_Mir_Value_Id *parameters,
    size_t count)
{
    om_vector_clear(&context->environment);
    for (size_t index = 0; index < count; ++index) {
        const Om_Mir_Env_Entry entry = {
            .symbol_id = symbols[index].symbol_id,
            .value_id = parameters[index],
        };
        const Om_Status status = om_vector_push(&context->environment, &entry);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status add_block_parameters_for_environment(
    Om_Mir_Lowering_Context *context,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Env_Entry *environment,
    size_t environment_count,
    Om_Mir_Value_Id *parameter_ids)
{
    for (size_t index = 0; index < environment_count; ++index) {
        Om_Type_Id type_id = OM_TYPE_ID_INVALID;
        Om_Status status = mir_value_type(context->mir,
            environment[index].value_id, &type_id);
        if (status == OM_STATUS_OK) {
            status = om_mir_add_block_parameter(context->mir, block_id,
                                                type_id, &parameter_ids[index]);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status values_for_symbols(Om_Mir_Lowering_Context *context,
                                    const Om_Symbol_Id *symbols,
                                    size_t symbol_count,
                                    Om_Mir_Value_Id *values)
{
    for (size_t index = 0; index < symbol_count; ++index) {
        const Om_Mir_Env_Entry *entry = environment_entry(context, symbols[index]);
        if (entry == NULL) {
            return OM_STATUS_NOT_FOUND;
        }
        values[index] = entry->value_id;
    }
    return OM_STATUS_OK;
}

static bool types_match_recursive(const Om_Type_Store *store,
                                  Om_Type_Id left,
                                  Om_Type_Id right,
                                  size_t depth)
{
    if (depth >= 128) {
        return false;
    }
    Om_Type_Id resolved_left = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_right = OM_TYPE_ID_INVALID;
    if (om_type_store_resolve(store, left, &resolved_left) != OM_STATUS_OK
        || om_type_store_resolve(store, right, &resolved_right) != OM_STATUS_OK) {
        return false;
    }
    if (resolved_left.value == resolved_right.value) {
        return true;
    }
    Om_Type_View left_view = {0};
    Om_Type_View right_view = {0};
    if (om_type_store_get(store, resolved_left, &left_view) != OM_STATUS_OK
        || om_type_store_get(store, resolved_right, &right_view) != OM_STATUS_OK
        || left_view.kind != right_view.kind
        || left_view.nominal_symbol.value != right_view.nominal_symbol.value
        || left_view.child_count != right_view.child_count) {
        return false;
    }
    for (size_t index = 0; index < left_view.child_count; ++index) {
        Om_Type_Id left_child = OM_TYPE_ID_INVALID;
        Om_Type_Id right_child = OM_TYPE_ID_INVALID;
        if (om_type_store_child_at(store, resolved_left, index,
                &left_child) != OM_STATUS_OK
            || om_type_store_child_at(store, resolved_right, index,
                &right_child) != OM_STATUS_OK
            || !types_match_recursive(store, left_child, right_child, depth + 1)) {
            return false;
        }
    }
    return true;
}

static bool types_match(const Om_Type_Store *store,
                        Om_Type_Id left,
                        Om_Type_Id right)
{
    return types_match_recursive(store, left, right, 0);
}

static const Om_Mir_Function *resolve_direct_callee(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *call,
    const Om_Mir_Value_Id *arguments,
    size_t argument_count,
    Om_Type_Id result_type)
{
    const Om_Mir_Function *match = NULL;
    for (size_t index = 0; index < context->mir->function_count; ++index) {
        const Om_Mir_Function *candidate = &context->mir->functions[index];
        if (candidate->symbol_id.value != call->symbol_id.value) {
            continue;
        }
        Om_Type_Id signature = OM_TYPE_ID_INVALID;
        Om_Type_View signature_view = {0};
        if (om_type_store_resolve(context->mir->type_store,
                candidate->signature_type_id, &signature) != OM_STATUS_OK
            || om_type_store_get(context->mir->type_store, signature,
                &signature_view) != OM_STATUS_OK
            || (signature_view.kind != OM_TYPE_FUNCTION
                && signature_view.kind != OM_TYPE_ACTION)
            || signature_view.child_count != argument_count + 1) {
            continue;
        }
        bool compatible = true;
        for (size_t parameter = 0; compatible && parameter < argument_count;
             ++parameter) {
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            Om_Type_Id argument_type = OM_TYPE_ID_INVALID;
            if (om_type_store_child_at(context->mir->type_store, signature,
                    parameter, &parameter_type) != OM_STATUS_OK
                || mir_value_type(context->mir, arguments[parameter],
                    &argument_type) != OM_STATUS_OK
                || !types_match(context->mir->type_store,
                    parameter_type, argument_type)) {
                compatible = false;
            }
        }
        Om_Type_Id return_type = OM_TYPE_ID_INVALID;
        if (compatible
            && (om_type_store_child_at(context->mir->type_store, signature,
                    signature_view.child_count - 1, &return_type) != OM_STATUS_OK
                || !types_match(context->mir->type_store, return_type,
                                result_type))) {
            compatible = false;
        }
        if (compatible) {
            if (match != NULL) {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
}

static const Om_Mir_Function *resolve_function_reference(
    const Om_Mir_Lowering_Context *context,
    Om_Symbol_Id symbol_id,
    Om_Type_Id function_type)
{
    const Om_Mir_Function *match = NULL;
    for (size_t index = 0; index < context->mir->function_count; ++index) {
        const Om_Mir_Function *candidate = &context->mir->functions[index];
        if (candidate->symbol_id.value == symbol_id.value
            && types_match(context->mir->type_store,
                           candidate->signature_type_id, function_type)) {
            if (match != NULL) {
                return NULL;
            }
            match = candidate;
        }
    }
    return match;
}

static Om_Status lower_function_reference(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *node,
    Om_Type_Id function_type,
    Om_Mir_Value_Id *out_value)
{
    const Om_Mir_Function *callee = resolve_function_reference(context,
        node->symbol_id, function_type);
    if (callee == NULL) {
        return add_lowering_diagnostic(context, node->span);
    }
    return emit_instruction(context, node, OM_MIR_FUNCTION_REF, function_type,
        OM_HIR_OPERATOR_NONE, callee->instance_id, NULL, 0, 0, 0.0,
        out_value);
}

static Om_Status lower_statement(Om_Mir_Lowering_Context *context,
                                 Om_Hir_Node_Id node_id);
static Om_Status lower_block(Om_Mir_Lowering_Context *context,
                             Om_Hir_Node_Id block_node_id);

static Om_Status lower_call(Om_Mir_Lowering_Context *context,
                            const Om_Hir_Node *node,
                            Om_Mir_Value_Id *out_value)
{
    const bool direct = node->kind == OM_HIR_CALL_DIRECT;
    if (node->child_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t argument_count = node->child_count - 1;
    const size_t callee_offset = direct ? 0 : 1;
    const size_t operand_count = argument_count + callee_offset;
    Om_Mir_Value_Id *operands = operand_count == 0 ? NULL
        : om_alloc_array(operand_count, sizeof(*operands));
    if (operand_count != 0 && operands == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    if (!direct) {
        const Om_Hir_Node *callee_node = hir_child(context->hir, node, 0);
        status = callee_node == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, callee_node->id, &operands[0]);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < argument_count;
         ++index) {
        const Om_Hir_Node *argument = hir_child(context->hir, node,
                                                index + 1);
        if (argument == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = lower_expression(context, argument->id,
                                      &operands[index + callee_offset]);
        }
    }
    Om_Type_Id result_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = specialized_type(context, node->type_id, &result_type);
    }
    if (status == OM_STATUS_OK && direct) {
        const Om_Symbol *callee_symbol = om_resolution_symbol_by_id(
            context->hir->resolution, node->symbol_id);
        if (callee_symbol != NULL && callee_symbol->native_module != NULL) {
            const Om_Omi_Module *native_module = callee_symbol->native_module;
            if (callee_symbol->native_function_index
                    >= native_module->function_count
                || native_module->functions == NULL) {
                status = add_lowering_diagnostic(context, node->span);
                om_free(operands);
                return status;
            }
            const Om_Omi_Native_Function *native_function =
                &native_module->functions[callee_symbol->native_function_index];
            if (native_function->parameter_start
                    > native_module->parameter_count
                || native_function->parameter_count
                    > native_module->parameter_count
                        - native_function->parameter_start
                || (native_function->parameter_count != 0
                    && native_module->parameters == NULL)) {
                status = add_lowering_diagnostic(context, node->span);
                om_free(operands);
                return status;
            }
            size_t source_parameter_count = 0;
            size_t callback_parameter_count = 0;
            bool has_callback_parameter = false;
            for (size_t index = 0; index < native_function->parameter_count;
                 ++index) {
                const Om_Omi_Parameter *parameter =
                    &native_module->parameters[
                        native_function->parameter_start + index];
                if (parameter->is_callback) {
                    has_callback_parameter = true;
                    ++callback_parameter_count;
                } else {
                    ++source_parameter_count;
                }
            }
            if ((has_callback_parameter
                    && native_function->callback_binding_count
                        != callback_parameter_count)
                || source_parameter_count != argument_count) {
                status = add_lowering_diagnostic(context, node->span);
            } else {
                const Om_Mir_Instruction_Spec spec = {
                    .opcode = OM_MIR_NATIVE_CALL,
                    .type_id = result_type,
                    .callee_instance_id = OM_FUNCTION_INSTANCE_ID_INVALID,
                    .symbol_id = node->symbol_id,
                    .span = node->span,
                    .effects = node->effects,
                    .flags = native_function->blocking
                            == OM_NATIVE_BLOCKING_YES
                        ? OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING
                        : OM_MIR_INSTRUCTION_FLAG_NONE,
                };
                status = om_mir_add_instruction(context->mir,
                    context->block_id, &spec, operands, argument_count,
                    out_value);
            }
            om_free(operands);
            return status;
        }
        if (callee_symbol != NULL
            && callee_symbol->kind == OM_SYMBOL_BUILTIN
            && om_string_view_equal(callee_symbol->name,
                                    (Om_String_View){"print", 5})) {
            if (argument_count != 1) {
                status = add_lowering_diagnostic(context, node->span);
            } else {
                status = emit_instruction(context, node, OM_MIR_RUNTIME_PRINT,
                    result_type, OM_HIR_OPERATOR_NONE,
                    OM_FUNCTION_INSTANCE_ID_INVALID, operands, 1, 0, 0.0,
                    out_value);
            }
            om_free(operands);
            return status;
        }
        const Om_Mir_Function *callee = resolve_direct_callee(context, node,
            operands, argument_count, result_type);
        if (callee == NULL) {
            status = add_lowering_diagnostic(context, node->span);
        } else {
            status = emit_instruction(context, node, OM_MIR_CALL_DIRECT,
                result_type, OM_HIR_OPERATOR_NONE, callee->instance_id,
                operands, argument_count, 0, 0.0, out_value);
        }
    } else if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_CALL_INDIRECT,
            result_type, OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            operands, operand_count, 0, 0.0, out_value);
    }
    om_free(operands);
    return status;
}

static Om_Status lower_unary(Om_Mir_Lowering_Context *context,
                             const Om_Hir_Node *node,
                             Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *operand_node = hir_child(context->hir, node, 0);
    if (operand_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id result_type = OM_TYPE_ID_INVALID;
    Om_Status status = specialized_type(context, node->type_id, &result_type);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (node->operator_kind == OM_HIR_OPERATOR_NEGATE
        && operand_node->kind == OM_HIR_INTEGER
        && (operand_node->flags & OM_HIR_FLAG_MIN_INTEGER) != 0) {
        return emit_instruction(context, node, OM_MIR_CONST_INT, result_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            UINT64_C(1) << 63, 0.0, out_value);
    }
    Om_Mir_Value_Id operand = OM_MIR_VALUE_ID_INVALID;
    status = lower_expression(context, operand_node->id, &operand);
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Mir_Opcode opcode = OM_MIR_BOOL_NOT;
    if (node->operator_kind == OM_HIR_OPERATOR_NEGATE) {
        Om_Type_Kind type_kind = OM_TYPE_VARIABLE;
        status = resolved_type_kind(context->mir->type_store, result_type,
                                    &type_kind);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (type_kind == OM_TYPE_INT) {
            opcode = OM_MIR_INT_NEGATE_CHECKED;
        } else if (type_kind == OM_TYPE_FLOAT) {
            opcode = OM_MIR_FLOAT_NEGATE;
        } else {
            return add_lowering_diagnostic(context, node->span);
        }
    } else if (node->operator_kind != OM_HIR_OPERATOR_NOT) {
        return add_lowering_diagnostic(context, node->span);
    }
    return emit_instruction(context, node, opcode, result_type,
        node->operator_kind, OM_FUNCTION_INSTANCE_ID_INVALID, &operand, 1,
        0, 0.0, out_value);
}

static Om_Status lower_short_circuit_binary(
    Om_Mir_Lowering_Context *context,
    const Om_Hir_Node *node,
    Om_Mir_Value_Id *out_value)
{
    const Om_Hir_Node *left_node = hir_child(context->hir, node, 0);
    const Om_Hir_Node *right_node = hir_child(context->hir, node, 1);
    if (left_node == NULL || right_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const bool is_and = node->operator_kind == OM_HIR_OPERATOR_AND;
    if (!is_and && node->operator_kind != OM_HIR_OPERATOR_OR) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id left_value = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, left_node->id, &left_value);
    if (status != OM_STATUS_OK || context->unsupported) return status;

    Om_Type_Id bool_type = OM_TYPE_ID_INVALID;
    status = specialized_type(context, node->type_id, &bool_type);
    if (status != OM_STATUS_OK) return status;
    const Om_Mir_Block_Id source_block = context->block_id;
    Om_Mir_Value_Id short_value = OM_MIR_VALUE_ID_INVALID;
    const Om_Mir_Instruction_Spec constant = {
        .opcode = OM_MIR_CONST_BOOL,
        .type_id = bool_type,
        .operator_kind = OM_HIR_OPERATOR_NONE,
        .callee_instance_id = OM_FUNCTION_INSTANCE_ID_INVALID,
        .symbol_id = node->symbol_id,
        .span = node->span,
        .effects = node->effects,
        .integer_value = is_and ? 0 : 1,
    };
    status = om_mir_add_instruction(context->mir, source_block, &constant,
                                    NULL, 0, &short_value);
    if (status != OM_STATUS_OK) return status;

    Om_Mir_Block_Id right_block = OM_MIR_BLOCK_ID_INVALID;
    status = om_mir_add_block(context->mir, context->function_id,
                              &right_block);
    if (status != OM_STATUS_OK) return status;

    context->block_id = right_block;
    context->terminated = false;
    Om_Mir_Value_Id right_value = OM_MIR_VALUE_ID_INVALID;
    status = lower_expression(context, right_node->id, &right_value);
    if (status != OM_STATUS_OK || context->unsupported) return status;
    const Om_Mir_Block_Id right_end_block = context->block_id;
    const bool right_terminated = context->terminated;

    Om_Mir_Block_Id merge_block = OM_MIR_BLOCK_ID_INVALID;
    status = om_mir_add_block(context->mir, context->function_id,
                              &merge_block);
    if (status != OM_STATUS_OK) return status;
    Om_Mir_Value_Id merge_value = OM_MIR_VALUE_ID_INVALID;
    status = om_mir_add_block_parameter(context->mir, merge_block,
                                        bool_type, &merge_value);
    if (status != OM_STATUS_OK) return status;

    const Om_Mir_Value_Id short_arguments[] = {short_value};
    const Om_Mir_Edge_Input and_edges[] = {
        {.target_block_id = right_block},
        {.target_block_id = merge_block, .arguments = short_arguments,
         .argument_count = 1},
    };
    const Om_Mir_Edge_Input or_edges[] = {
        {.target_block_id = merge_block, .arguments = short_arguments,
         .argument_count = 1},
        {.target_block_id = right_block},
    };
    status = om_mir_set_terminator(context->mir, source_block,
        OM_MIR_COND_BR, left_value, is_and ? and_edges : or_edges, 2);
    if (status != OM_STATUS_OK) return status;
    if (!right_terminated) {
        status = set_branch(context, right_end_block, merge_block,
                            &right_value, 1);
        if (status != OM_STATUS_OK) return status;
    }
    context->block_id = merge_block;
    context->terminated = false;
    *out_value = merge_value;
    return OM_STATUS_OK;
}

static Om_Status lower_binary(Om_Mir_Lowering_Context *context,
                              const Om_Hir_Node *node,
                              Om_Mir_Value_Id *out_value)
{
    if (node->child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (node->operator_kind == OM_HIR_OPERATOR_AND
        || node->operator_kind == OM_HIR_OPERATOR_OR) {
        return lower_short_circuit_binary(context, node, out_value);
    }
    Om_Mir_Value_Id operands[2] = {
        OM_MIR_VALUE_ID_INVALID, OM_MIR_VALUE_ID_INVALID,
    };
    Om_Status status = lower_expression(context,
        hir_child(context->hir, node, 0)->id, &operands[0]);
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = lower_expression(context,
            hir_child(context->hir, node, 1)->id, &operands[1]);
    }
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Type_Id result_type = OM_TYPE_ID_INVALID;
    status = specialized_type(context, node->type_id, &result_type);
    Om_Type_Kind result_kind = OM_TYPE_VARIABLE;
    if (status == OM_STATUS_OK) {
        status = resolved_type_kind(context->mir->type_store, result_type,
                                    &result_kind);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Mir_Opcode opcode = OM_MIR_COMPARE;
    switch (node->operator_kind) {
    case OM_HIR_OPERATOR_ADD:
        opcode = result_kind == OM_TYPE_INT ? OM_MIR_INT_ADD_CHECKED
            : result_kind == OM_TYPE_FLOAT ? OM_MIR_FLOAT_ADD : 0;
        break;
    case OM_HIR_OPERATOR_SUBTRACT:
        opcode = result_kind == OM_TYPE_INT ? OM_MIR_INT_SUB_CHECKED
            : result_kind == OM_TYPE_FLOAT ? OM_MIR_FLOAT_SUB : 0;
        break;
    case OM_HIR_OPERATOR_MULTIPLY:
        opcode = result_kind == OM_TYPE_INT ? OM_MIR_INT_MUL_CHECKED
            : result_kind == OM_TYPE_FLOAT ? OM_MIR_FLOAT_MUL : 0;
        break;
    case OM_HIR_OPERATOR_DIVIDE:
        opcode = result_kind == OM_TYPE_INT ? OM_MIR_INT_DIV_CHECKED
            : result_kind == OM_TYPE_FLOAT ? OM_MIR_FLOAT_DIV : 0;
        break;
    case OM_HIR_OPERATOR_REMAINDER:
        opcode = result_kind == OM_TYPE_INT ? OM_MIR_INT_REM_CHECKED : 0;
        break;
    case OM_HIR_OPERATOR_AND: opcode = OM_MIR_BOOL_AND; break;
    case OM_HIR_OPERATOR_OR: opcode = OM_MIR_BOOL_OR; break;
    case OM_HIR_OPERATOR_EQUAL:
    case OM_HIR_OPERATOR_NOT_EQUAL:
    case OM_HIR_OPERATOR_LESS:
    case OM_HIR_OPERATOR_GREATER:
    case OM_HIR_OPERATOR_LESS_EQUAL:
    case OM_HIR_OPERATOR_GREATER_EQUAL:
        opcode = OM_MIR_COMPARE;
        break;
    default:
        opcode = 0;
        break;
    }
    if ((int)opcode == 0) {
        return add_lowering_diagnostic(context, node->span);
    }
    return emit_instruction(context, node, opcode, result_type,
        node->operator_kind, OM_FUNCTION_INSTANCE_ID_INVALID, operands, 2,
        0, 0.0, out_value);
}

static Om_Status lower_expression(Om_Mir_Lowering_Context *context,
                                  Om_Hir_Node_Id node_id,
                                  Om_Mir_Value_Id *out_value)
{
    if (context == NULL || out_value == NULL || context->terminated) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *node = hir_node(context->hir, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id type_id = OM_TYPE_ID_INVALID;
    Om_Status status = specialized_type(context, node->type_id, &type_id);
    if (status != OM_STATUS_OK) {
        return status;
    }
    switch (node->kind) {
    case OM_HIR_INTEGER:
        return emit_instruction(context, node, OM_MIR_CONST_INT, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            node->integer_magnitude, 0.0, out_value);
    case OM_HIR_FLOAT:
        return emit_instruction(context, node, OM_MIR_CONST_FLOAT, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            0, node->float_value, out_value);
    case OM_HIR_STRING: {
        Om_Vector decoded = {0};
        status = decode_string_literal(node->name, &decoded);
        if (status == OM_STATUS_OK) {
            const Om_String_View value = {decoded.data, decoded.length};
            status = emit_string_literal(context, node, type_id, value,
                                         out_value);
        }
        om_vector_destroy(&decoded);
        return status;
    }
    case OM_HIR_INTERPOLATED_STRING:
        return lower_interpolated_string(context, node, type_id, out_value);
    case OM_HIR_TRY:
        return lower_try(context, node, type_id, out_value);
    case OM_HIR_RESULT_OK:
    case OM_HIR_RESULT_ERROR:
        return lower_result_constructor(context, node, type_id, out_value);
    case OM_HIR_RECORD_CONSTRUCT:
        return lower_nominal_constructor(context, node, type_id,
            OM_HIR_ENTITY_RECORD, out_value);
    case OM_HIR_MESSAGE_CONSTRUCT:
        return lower_nominal_constructor(context, node, type_id,
            OM_HIR_ENTITY_MESSAGE, out_value);
    case OM_HIR_FIELD_READ:
        return lower_field_read(context, node, type_id, out_value);
    case OM_HIR_RECORD_WITH:
        return lower_record_with(context, node, type_id, out_value);
    case OM_HIR_LIST:
        return lower_list_literal(context, node, type_id, out_value);
    case OM_HIR_MAP:
        return lower_map_literal(context, node, type_id, out_value);
    case OM_HIR_INDEX:
        return lower_index(context, node, type_id, out_value);
    case OM_HIR_NULL:
        return emit_instruction(context, node, OM_MIR_CONST_NULL, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            0, 0.0, out_value);
    case OM_HIR_BOOLEAN: {
        const uint64_t value = node->name.length != 0
            && (node->name.data[0] == 'T' || node->name.data[0] == 't') ? 1 : 0;
        return emit_instruction(context, node, OM_MIR_CONST_BOOL, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            value, 0.0, out_value);
    }
    case OM_HIR_NOMINAL_VALUE: {
        Om_Type_View nominal_type = {0};
        if (node->integer_magnitude > 2
            || om_type_store_get(context->specialization->type_store, type_id,
                                 &nominal_type) != OM_STATUS_OK
            || nominal_type.kind != OM_TYPE_DOWN_REASON
            || nominal_type.nominal_symbol.value == UINT32_MAX) {
            return add_lowering_diagnostic(context, node->span);
        }
        return emit_instruction(context, node, OM_MIR_CONST_INT, type_id,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            node->integer_magnitude, 0.0, out_value);
    }
    case OM_HIR_SELF:
        return emit_instruction(context, node, OM_MIR_RUNTIME_ACTOR_SELF,
            type_id, OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            NULL, 0, 0, 0.0, out_value);
    case OM_HIR_SPAWN: {
        if (node->child_count > 1) {
            return add_lowering_diagnostic(context, node->span);
        }
        Om_Mir_Value_Id node_ref = OM_MIR_VALUE_ID_INVALID;
        if (node->child_count == 1) {
            const Om_Hir_Node *placement = hir_child(context->hir, node, 0);
            if (placement == NULL) {
                return add_lowering_diagnostic(context, node->span);
            }
            status = lower_expression(context, placement->id, &node_ref);
            if (status != OM_STATUS_OK || context->unsupported) return status;
        }
        return emit_instruction_with_symbol(context, node,
            OM_MIR_RUNTIME_ACTOR_SPAWN, type_id, node->symbol_id,
            node->child_count == 0 ? NULL : &node_ref, node->child_count,
            out_value);
    }
    case OM_HIR_SYSTEM_ACTOR_REF: {
        uint64_t actor_id = UINT64_MAX;
        if (node->builtin_id == OM_BUILTIN_SYSTEM_ACTOR_CONSOLE) {
            actor_id = OM_SYSTEM_ACTOR_REF_CONSOLE;
        } else if (node->builtin_id == OM_BUILTIN_SYSTEM_ACTOR_TIMER) {
            actor_id = OM_SYSTEM_ACTOR_REF_TIMER;
        } else if (node->builtin_id
                   == OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM) {
            actor_id = OM_SYSTEM_ACTOR_REF_FILE_SYSTEM;
        }
        if (actor_id == UINT64_MAX) {
            return add_lowering_diagnostic(context, node->span);
        }
        return emit_instruction(context, node, OM_MIR_RUNTIME_SYSTEM_ACTOR,
            type_id, OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            NULL, 0, actor_id, 0.0, out_value);
    }
    case OM_HIR_LOCAL: {
        const Om_Mir_Env_Entry *entry = environment_entry(context, node->symbol_id);
        if (entry != NULL) {
            *out_value = entry->value_id;
            return OM_STATUS_OK;
        }
        if (symbol_is_actor_state(context, node->symbol_id)) {
            if (context->actor_state_value_id.value == UINT32_MAX) {
                return add_lowering_diagnostic(context, node->span);
            }
            return emit_instruction_with_symbol(context, node,
                OM_MIR_LOAD_FIELD, type_id, node->symbol_id,
                &context->actor_state_value_id, 1, out_value);
        }
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            context->hir->resolution, node->symbol_id);
        if (symbol != NULL
            && (symbol->kind == OM_SYMBOL_FUNCTION
                || symbol->kind == OM_SYMBOL_ACTION)) {
            return lower_function_reference(context, node, type_id, out_value);
        }
        return add_lowering_diagnostic(context, node->span);
    }
    case OM_HIR_SYMBOL_REF: {
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            context->hir->resolution, node->symbol_id);
        if (symbol != NULL
            && (symbol->kind == OM_SYMBOL_FUNCTION
                || symbol->kind == OM_SYMBOL_ACTION)) {
            return lower_function_reference(context, node, type_id, out_value);
        }
        return add_lowering_diagnostic(context, node->span);
    }
    case OM_HIR_GROUP: {
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Hir_Node *child = hir_child(context->hir, node, 0);
        return child == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, child->id, out_value);
    }
    case OM_HIR_UNARY:
        return lower_unary(context, node, out_value);
    case OM_HIR_BINARY:
        return lower_binary(context, node, out_value);
    case OM_HIR_CALL_DIRECT:
    case OM_HIR_CALL_INDIRECT:
        return lower_call(context, node, out_value);
    default:
        return add_lowering_diagnostic(context, node->span);
    }
}

static Om_Status set_branch(Om_Mir_Lowering_Context *context,
                            Om_Mir_Block_Id source_block_id,
                            Om_Mir_Block_Id target_block_id,
                            const Om_Mir_Value_Id *arguments,
                            size_t argument_count)
{
    const Om_Mir_Edge_Input edge = {
        .target_block_id = target_block_id,
        .arguments = arguments,
        .argument_count = argument_count,
    };
    return om_mir_set_terminator(context->mir, source_block_id, OM_MIR_BR,
        OM_MIR_VALUE_ID_INVALID, &edge, 1);
}

static Om_Status add_merge_predecessor(Om_Vector *predecessors,
                                       Om_Mir_Block_Id block_id,
                                       const Om_Mir_Env_Entry *environment,
                                       size_t environment_count,
                                       bool is_conditional_edge)
{
    Om_Mir_Env_Entry *copy = NULL;
    if (environment_count != 0) {
        copy = om_alloc_array(environment_count, sizeof(*copy));
        if (copy == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(copy, environment, environment_count * sizeof(*copy));
    }
    const Om_Mir_Merge_Predecessor predecessor = {
        block_id, copy, environment_count, is_conditional_edge,
    };
    const Om_Status status = om_vector_push(predecessors, &predecessor);
    if (status != OM_STATUS_OK) {
        om_free(copy);
    }
    return status;
}

static void destroy_merge_predecessors(Om_Vector *predecessors)
{
    for (size_t index = 0; index < predecessors->length; ++index) {
        const Om_Mir_Merge_Predecessor *predecessor = om_vector_at_const(
            predecessors, index);
        if (predecessor != NULL) {
            om_free(predecessor->environment);
        }
    }
    om_vector_destroy(predecessors);
}

static Om_Status add_loop_predecessor(Om_Vector *predecessors,
                                      Om_Mir_Block_Id block_id,
                                      const Om_Mir_Value_Id *values,
                                      size_t value_count)
{
    Om_Mir_Value_Id *copy = NULL;
    if (value_count != 0) {
        copy = om_alloc_array(value_count, sizeof(*copy));
        if (copy == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(copy, values, value_count * sizeof(*copy));
    }
    const Om_Mir_Loop_Predecessor predecessor = {
        block_id, copy, value_count,
    };
    const Om_Status status = om_vector_push(predecessors, &predecessor);
    if (status != OM_STATUS_OK) {
        om_free(copy);
    }
    return status;
}

static void destroy_loop_frame(Om_Mir_Loop_Frame *frame)
{
    if (frame == NULL) {
        return;
    }
    for (size_t index = 0; index < frame->break_predecessors.length; ++index) {
        const Om_Mir_Loop_Predecessor *predecessor = om_vector_at_const(
            &frame->break_predecessors, index);
        if (predecessor != NULL) om_free(predecessor->values);
    }
    for (size_t index = 0; index < frame->continue_predecessors.length; ++index) {
        const Om_Mir_Loop_Predecessor *predecessor = om_vector_at_const(
            &frame->continue_predecessors, index);
        if (predecessor != NULL) om_free(predecessor->values);
    }
    om_vector_destroy(&frame->continue_predecessors);
    om_vector_destroy(&frame->break_predecessors);
    om_free(frame->symbols);
    *frame = (Om_Mir_Loop_Frame){0};
}

static Om_Mir_Loop_Frame *current_loop_frame(Om_Mir_Lowering_Context *context)
{
    if (context->loop_frames.length == 0) {
        return NULL;
    }
    return om_vector_at(&context->loop_frames, context->loop_frames.length - 1);
}

static Om_Status push_loop_frame(Om_Mir_Lowering_Context *context,
                                 const Om_Mir_Env_Entry *environment,
                                 size_t exit_symbol_count,
                                 size_t continue_symbol_count,
                                 Om_Mir_Block_Id header_block_id)
{
    Om_Mir_Loop_Frame frame = {
        .exit_symbol_count = exit_symbol_count,
        .continue_symbol_count = continue_symbol_count,
        .header_block_id = header_block_id,
    };
    if (continue_symbol_count != 0) {
        frame.symbols = om_alloc_array(continue_symbol_count,
                                       sizeof(*frame.symbols));
        if (frame.symbols == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t index = 0; index < continue_symbol_count; ++index) {
            frame.symbols[index] = environment[index].symbol_id;
        }
    }
    Om_Status status = om_vector_init(&frame.break_predecessors,
                                      sizeof(Om_Mir_Loop_Predecessor));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&frame.continue_predecessors,
                                sizeof(Om_Mir_Loop_Predecessor));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_push(&context->loop_frames, &frame);
    }
    if (status != OM_STATUS_OK) {
        destroy_loop_frame(&frame);
    }
    return status;
}

static Om_Status record_loop_transfer(Om_Mir_Lowering_Context *context,
                                      bool is_break,
                                      Om_Source_Span span)
{
    Om_Mir_Loop_Frame *frame = current_loop_frame(context);
    if (frame == NULL) {
        return add_lowering_diagnostic(context, span);
    }
    const size_t count = is_break ? frame->exit_symbol_count
                                  : frame->continue_symbol_count;
    Om_Mir_Value_Id *values = count == 0 ? NULL
        : om_alloc_array(count, sizeof(*values));
    if (count != 0 && values == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = values_for_symbols(context, frame->symbols, count, values);
    if (status == OM_STATUS_OK) {
        status = add_loop_predecessor(is_break ? &frame->break_predecessors
            : &frame->continue_predecessors, context->block_id, values, count);
    }
    om_free(values);
    if (status == OM_STATUS_OK) {
        context->terminated = true;
    }
    return status;
}

static Om_Status finish_loop_predecessors(Om_Mir_Lowering_Context *context,
                                          const Om_Vector *predecessors,
                                          Om_Mir_Block_Id target_block_id,
                                          size_t expected_value_count)
{
    for (size_t index = 0; index < predecessors->length; ++index) {
        const Om_Mir_Loop_Predecessor *predecessor = om_vector_at_const(
            predecessors, index);
        if (predecessor == NULL
            || predecessor->value_count != expected_value_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Status status = set_branch(context, predecessor->block_id,
            target_block_id, predecessor->values, predecessor->value_count);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status lower_if(Om_Mir_Lowering_Context *context,
                          const Om_Hir_Node *node)
{
    Om_Vector tests = {0};
    Om_Vector predecessors = {0};
    Om_Status status = om_vector_init(&tests, sizeof(Om_Mir_Test_Block));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&predecessors,
                                sizeof(Om_Mir_Merge_Predecessor));
    }
    Om_Mir_Env_Entry *base_environment = NULL;
    size_t base_count = 0;
    if (status == OM_STATUS_OK) {
        status = environment_snapshot(context, &base_environment, &base_count);
    }
    size_t arm_count = 0;
    const bool has_else = (node->flags & OM_HIR_FLAG_HAS_ELSE) != 0;
    if (status == OM_STATUS_OK) {
        arm_count = node->child_count - (has_else ? 1u : 0u);
        if (arm_count == 0) status = OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Block_Id current_test_block = context->block_id;
    for (size_t arm_index = 0; status == OM_STATUS_OK
         && !context->unsupported && arm_index < arm_count; ++arm_index) {
        const Om_Hir_Node *arm = hir_child(context->hir, node, arm_index);
        const Om_Hir_Node *condition = arm != NULL
            ? hir_child(context->hir, arm, 0) : NULL;
        const Om_Hir_Node *body = arm != NULL
            ? hir_child(context->hir, arm, 1) : NULL;
        if (arm == NULL || arm->kind != OM_HIR_IF_ARM
            || condition == NULL || body == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = environment_restore(context, base_environment, base_count);
        if (status != OM_STATUS_OK) break;
        context->block_id = current_test_block;
        context->terminated = false;
        Om_Mir_Value_Id condition_value = OM_MIR_VALUE_ID_INVALID;
        status = lower_expression(context, condition->id, &condition_value);
        if (status != OM_STATUS_OK || context->unsupported) break;
        const Om_Mir_Block_Id condition_block = context->block_id;
        Om_Mir_Block_Id body_block = OM_MIR_BLOCK_ID_INVALID;
        status = om_mir_add_block(context->mir, context->function_id,
                                  &body_block);
        if (status != OM_STATUS_OK) break;
        Om_Mir_Test_Block test = {
            .block_id = condition_block,
            .true_block_id = body_block,
            .false_block_id = OM_MIR_BLOCK_ID_INVALID,
            .condition_value_id = condition_value,
        };
        status = om_vector_push(&tests, &test);
        if (status != OM_STATUS_OK) break;
        current_test_block = condition_block;
        context->block_id = body_block;
        context->terminated = false;
        status = lower_block(context, body->id);
        if (status != OM_STATUS_OK || context->unsupported) break;
        if (!context->terminated) {
            status = add_merge_predecessor(&predecessors, context->block_id,
                context->environment.data, context->environment.length, false);
            if (status != OM_STATUS_OK) break;
        }
        if (arm_index + 1 < arm_count) {
            Om_Mir_Block_Id next_test_block = OM_MIR_BLOCK_ID_INVALID;
            status = om_mir_add_block(context->mir, context->function_id,
                                      &next_test_block);
            if (status != OM_STATUS_OK) break;
            Om_Mir_Test_Block *stored_test = om_vector_at(&tests,
                                                           tests.length - 1);
            if (stored_test == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            stored_test->false_block_id = next_test_block;
            current_test_block = next_test_block;
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported && has_else) {
        const Om_Hir_Node *else_block = hir_child(context->hir, node,
                                                  node->child_count - 1);
        Om_Mir_Block_Id else_block_id = OM_MIR_BLOCK_ID_INVALID;
        status = om_mir_add_block(context->mir, context->function_id,
                                  &else_block_id);
        if (status == OM_STATUS_OK) {
            Om_Mir_Test_Block *stored_test = om_vector_at(&tests,
                                                           tests.length - 1);
            if (stored_test == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                stored_test->false_block_id = else_block_id;
                context->block_id = else_block_id;
                context->terminated = false;
                status = lower_block(context, else_block->id);
                if (status == OM_STATUS_OK && !context->unsupported
                    && !context->terminated) {
                    status = add_merge_predecessor(&predecessors,
                        context->block_id, context->environment.data,
                        context->environment.length, false);
                }
            }
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported && !has_else) {
        status = add_merge_predecessor(&predecessors, current_test_block,
            base_environment, base_count, true);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        if (predecessors.length == 0) {
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < tests.length; ++index) {
                const Om_Mir_Test_Block *test = om_vector_at_const(&tests, index);
                if (test == NULL || test->false_block_id.value == UINT32_MAX) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                const Om_Mir_Edge_Input edges[] = {
                    {.target_block_id = test->true_block_id},
                    {.target_block_id = test->false_block_id},
                };
                status = om_mir_set_terminator(context->mir, test->block_id,
                    OM_MIR_COND_BR, test->condition_value_id, edges, 2);
            }
            if (status == OM_STATUS_OK) context->terminated = true;
        } else {
            Om_Mir_Block_Id merge_block = OM_MIR_BLOCK_ID_INVALID;
            status = om_mir_add_block(context->mir, context->function_id,
                                      &merge_block);
            Om_Mir_Value_Id *merge_parameters = NULL;
            if (status == OM_STATUS_OK && predecessors.length > 1
                && base_count != 0) {
                merge_parameters = om_alloc_array(base_count,
                                                   sizeof(*merge_parameters));
                if (merge_parameters == NULL) status = OM_STATUS_OUT_OF_MEMORY;
                if (status == OM_STATUS_OK) {
                    status = add_block_parameters_for_environment(context,
                        merge_block, base_environment, base_count,
                        merge_parameters);
                }
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < predecessors.length; ++index) {
                const Om_Mir_Merge_Predecessor *predecessor = om_vector_at_const(
                    &predecessors, index);
                if (predecessor == NULL
                    || predecessor->environment_count != base_count) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                Om_Mir_Value_Id *arguments = NULL;
                size_t argument_count = 0;
                if (predecessors.length > 1) {
                    argument_count = base_count;
                    arguments = base_count == 0 ? NULL
                        : om_alloc_array(base_count, sizeof(*arguments));
                    if (base_count != 0 && arguments == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                        break;
                    }
                    for (size_t value_index = 0; value_index < base_count;
                         ++value_index) {
                        if (predecessor->environment[value_index].symbol_id.value
                            != base_environment[value_index].symbol_id.value) {
                            status = OM_STATUS_INVALID_ARGUMENT;
                            break;
                        }
                        arguments[value_index] =
                            predecessor->environment[value_index].value_id;
                    }
                }
                if (status == OM_STATUS_OK
                    && !predecessor->is_conditional_edge) {
                    status = set_branch(context, predecessor->block_id,
                        merge_block, arguments, argument_count);
                }
                om_free(arguments);
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < tests.length; ++index) {
                const Om_Mir_Test_Block *test = om_vector_at_const(&tests, index);
                const bool implicit_false_edge = !has_else
                    && index + 1 == tests.length;
                if (test == NULL || (!implicit_false_edge
                    && test->false_block_id.value == UINT32_MAX)) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                const Om_Mir_Block_Id false_target = implicit_false_edge
                    ? merge_block : test->false_block_id;
                Om_Mir_Value_Id *false_arguments = NULL;
                if (implicit_false_edge && predecessors.length > 1
                    && base_count != 0) {
                    false_arguments = om_alloc_array(base_count,
                        sizeof(*false_arguments));
                    if (false_arguments == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                        break;
                    }
                    for (size_t value_index = 0; value_index < base_count;
                         ++value_index) {
                        false_arguments[value_index] =
                            base_environment[value_index].value_id;
                    }
                }
                const Om_Mir_Edge_Input edges[] = {
                    {.target_block_id = test->true_block_id},
                    {.target_block_id = false_target,
                     .arguments = false_arguments,
                     .argument_count = implicit_false_edge
                            && predecessors.length > 1
                        ? base_count : 0},
                };
                if (status == OM_STATUS_OK) {
                    status = om_mir_set_terminator(context->mir, test->block_id,
                        OM_MIR_COND_BR, test->condition_value_id, edges, 2);
                }
                om_free(false_arguments);
            }
            if (status == OM_STATUS_OK) {
                context->block_id = merge_block;
                context->terminated = false;
                if (predecessors.length > 1) {
                    status = environment_from_block_parameters(context,
                        base_environment, merge_parameters, base_count);
                } else {
                    const Om_Mir_Merge_Predecessor *predecessor =
                        om_vector_at_const(&predecessors, 0);
                    if (predecessor == NULL) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                    } else {
                        status = environment_restore(context,
                            predecessor->environment,
                            predecessor->environment_count);
                    }
                }
            }
            om_free(merge_parameters);
        }
    }
    om_free(base_environment);
    destroy_merge_predecessors(&predecessors);
    om_vector_destroy(&tests);
    return status;
}

static Om_Status lower_select(Om_Mir_Lowering_Context *context,
                              const Om_Hir_Node *node)
{
    if (node->child_count < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *scrutinee_node = hir_child(context->hir, node, 0);
    if (scrutinee_node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id scrutinee = OM_MIR_VALUE_ID_INVALID;
    Om_Status status = lower_expression(context, scrutinee_node->id,
                                        &scrutinee);
    if (status != OM_STATUS_OK || context->unsupported) {
        return status;
    }
    Om_Mir_Env_Entry *base_environment = NULL;
    size_t base_count = 0;
    status = environment_snapshot(context, &base_environment, &base_count);
    Om_Vector tests = {0};
    Om_Vector predecessors = {0};
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&tests, sizeof(Om_Mir_Test_Block));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&predecessors,
                                sizeof(Om_Mir_Merge_Predecessor));
    }
    const size_t arm_count = node->child_count - 1;
    const Om_Hir_Node *last_arm = hir_child(context->hir, node,
                                            node->child_count - 1);
    const bool has_else = last_arm != NULL
        && (last_arm->flags & OM_HIR_FLAG_CASE_ELSE) != 0;
    const size_t conditional_arm_count = arm_count - (has_else ? 1u : 0u);
    Om_Type_Id bool_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_primitive(context->mir->type_store, OM_TYPE_BOOL,
                                         &bool_type);
    }
    Om_Mir_Block_Id current_test_block = context->block_id;
    for (size_t arm_index = 0; status == OM_STATUS_OK
         && !context->unsupported && arm_index < conditional_arm_count;
         ++arm_index) {
        const Om_Hir_Node *arm = hir_child(context->hir, node, arm_index + 1);
        const Om_Hir_Node *pattern = arm == NULL ? NULL
            : hir_child(context->hir, arm, 0);
        const Om_Hir_Node *body = arm == NULL ? NULL
            : hir_child(context->hir, arm, 1);
        if (arm == NULL || arm->kind != OM_HIR_CASE_ARM
            || (arm->flags & OM_HIR_FLAG_CASE_ELSE) != 0
            || arm->child_count != 2 || pattern == NULL || body == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = environment_restore(context, base_environment, base_count);
        if (status != OM_STATUS_OK) break;
        context->block_id = current_test_block;
        context->terminated = false;
        Om_Mir_Value_Id condition_value = OM_MIR_VALUE_ID_INVALID;
        if (pattern->kind == OM_HIR_RESULT_OK
            || pattern->kind == OM_HIR_RESULT_ERROR) {
            Om_Mir_Value_Id is_ok = OM_MIR_VALUE_ID_INVALID;
            status = emit_instruction(context, pattern, OM_MIR_RESULT_TAG,
                bool_type, OM_HIR_OPERATOR_NONE,
                OM_FUNCTION_INSTANCE_ID_INVALID, &scrutinee, 1, 0, 0.0,
                &is_ok);
            if (status == OM_STATUS_OK
                && pattern->kind == OM_HIR_RESULT_ERROR) {
                status = emit_instruction(context, pattern, OM_MIR_BOOL_NOT,
                    bool_type, OM_HIR_OPERATOR_NOT,
                    OM_FUNCTION_INSTANCE_ID_INVALID, &is_ok, 1, 0, 0.0,
                    &condition_value);
            } else if (status == OM_STATUS_OK) {
                condition_value = is_ok;
            }
        } else {
            Om_Mir_Value_Id pattern_value = OM_MIR_VALUE_ID_INVALID;
            status = lower_expression(context, pattern->id, &pattern_value);
            if (status == OM_STATUS_OK && !context->unsupported) {
                const Om_Mir_Value_Id operands[] = {scrutinee, pattern_value};
                status = emit_instruction(context, pattern, OM_MIR_COMPARE,
                    bool_type, OM_HIR_OPERATOR_EQUAL,
                    OM_FUNCTION_INSTANCE_ID_INVALID, operands, 2, 0, 0.0,
                    &condition_value);
            }
        }
        if (status != OM_STATUS_OK || context->unsupported) break;
        const Om_Mir_Block_Id condition_block = context->block_id;
        Om_Mir_Block_Id body_block = OM_MIR_BLOCK_ID_INVALID;
        status = om_mir_add_block(context->mir, context->function_id,
                                  &body_block);
        if (status != OM_STATUS_OK) break;
        const Om_Mir_Test_Block test = {
            .block_id = condition_block,
            .true_block_id = body_block,
            .false_block_id = OM_MIR_BLOCK_ID_INVALID,
            .condition_value_id = condition_value,
        };
        status = om_vector_push(&tests, &test);
        if (status != OM_STATUS_OK) break;
        context->block_id = body_block;
        context->terminated = false;
        status = environment_restore(context, base_environment, base_count);
        if (status == OM_STATUS_OK
            && (pattern->kind == OM_HIR_RESULT_OK
                || pattern->kind == OM_HIR_RESULT_ERROR)) {
            const Om_Hir_Node *binding = pattern->child_count == 2
                ? hir_child(context->hir, pattern, 1) : NULL;
            if (binding == NULL || binding->kind != OM_HIR_LOCAL
                || binding->symbol_id.value == UINT32_MAX) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                Om_Type_Id binding_type = OM_TYPE_ID_INVALID;
                status = specialized_type(context, binding->type_id,
                                          &binding_type);
                Om_Mir_Value_Id payload = OM_MIR_VALUE_ID_INVALID;
                if (status == OM_STATUS_OK) {
                    const Om_Mir_Opcode opcode =
                        pattern->kind == OM_HIR_RESULT_OK
                            ? OM_MIR_RESULT_OK_PAYLOAD
                            : OM_MIR_RESULT_ERROR_PAYLOAD;
                    status = emit_instruction(context, pattern, opcode,
                        binding_type, OM_HIR_OPERATOR_NONE,
                        OM_FUNCTION_INSTANCE_ID_INVALID, &scrutinee, 1, 0,
                        0.0, &payload);
                }
                if (status == OM_STATUS_OK) {
                    status = environment_set(context, binding->symbol_id,
                                             payload, true);
                }
            }
        }
        if (status == OM_STATUS_OK) {
            status = lower_block(context, body->id);
        }
        if (status != OM_STATUS_OK || context->unsupported) break;
        if (!context->terminated) {
            status = add_merge_predecessor(&predecessors, context->block_id,
                context->environment.data, base_count, false);
        }
        if (status == OM_STATUS_OK && arm_index + 1 < arm_count) {
            Om_Mir_Block_Id next_test_block = OM_MIR_BLOCK_ID_INVALID;
            status = om_mir_add_block(context->mir, context->function_id,
                                      &next_test_block);
            if (status == OM_STATUS_OK) {
                Om_Mir_Test_Block *stored_test = om_vector_at(&tests,
                                                               tests.length - 1);
                if (stored_test == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                } else {
                    stored_test->false_block_id = next_test_block;
                    current_test_block = next_test_block;
                }
            }
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported && has_else) {
        const Om_Hir_Node *else_body = hir_child(context->hir, last_arm, 0);
        status = environment_restore(context, base_environment, base_count);
        context->block_id = current_test_block;
        context->terminated = false;
        if (status == OM_STATUS_OK && else_body != NULL) {
            status = lower_block(context, else_body->id);
        } else if (status == OM_STATUS_OK) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK && !context->unsupported
            && !context->terminated) {
            status = add_merge_predecessor(&predecessors, context->block_id,
                context->environment.data, base_count, false);
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported && !has_else) {
        status = add_merge_predecessor(&predecessors, current_test_block,
            base_environment, base_count, true);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        if (predecessors.length == 0) {
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < tests.length; ++index) {
                const Om_Mir_Test_Block *test = om_vector_at_const(&tests, index);
                if (test == NULL || test->false_block_id.value == UINT32_MAX) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                const Om_Mir_Edge_Input edges[] = {
                    {.target_block_id = test->true_block_id},
                    {.target_block_id = test->false_block_id},
                };
                status = om_mir_set_terminator(context->mir, test->block_id,
                    OM_MIR_COND_BR, test->condition_value_id, edges, 2);
            }
            if (status == OM_STATUS_OK) context->terminated = true;
        } else {
            Om_Mir_Block_Id merge_block = OM_MIR_BLOCK_ID_INVALID;
            status = om_mir_add_block(context->mir, context->function_id,
                                      &merge_block);
            Om_Mir_Value_Id *merge_parameters = NULL;
            if (status == OM_STATUS_OK && predecessors.length > 1
                && base_count != 0) {
                merge_parameters = om_alloc_array(base_count,
                                                   sizeof(*merge_parameters));
                if (merge_parameters == NULL) status = OM_STATUS_OUT_OF_MEMORY;
                if (status == OM_STATUS_OK) {
                    status = add_block_parameters_for_environment(context,
                        merge_block, base_environment, base_count,
                        merge_parameters);
                }
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < predecessors.length; ++index) {
                const Om_Mir_Merge_Predecessor *predecessor = om_vector_at_const(
                    &predecessors, index);
                if (predecessor == NULL
                    || predecessor->environment_count != base_count) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                Om_Mir_Value_Id *arguments = NULL;
                size_t argument_count = 0;
                if (predecessors.length > 1) {
                    argument_count = base_count;
                    arguments = base_count == 0 ? NULL
                        : om_alloc_array(base_count, sizeof(*arguments));
                    if (base_count != 0 && arguments == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                        break;
                    }
                    for (size_t value_index = 0; value_index < base_count;
                         ++value_index) {
                        if (predecessor->environment[value_index].symbol_id.value
                            != base_environment[value_index].symbol_id.value) {
                            status = OM_STATUS_INVALID_ARGUMENT;
                            break;
                        }
                        arguments[value_index] =
                            predecessor->environment[value_index].value_id;
                    }
                }
                if (status == OM_STATUS_OK
                    && !predecessor->is_conditional_edge) {
                    status = set_branch(context, predecessor->block_id,
                        merge_block, arguments, argument_count);
                }
                om_free(arguments);
            }
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < tests.length; ++index) {
                const Om_Mir_Test_Block *test = om_vector_at_const(&tests, index);
                const bool implicit_false_edge = !has_else
                    && index + 1 == tests.length;
                if (test == NULL || (!implicit_false_edge
                    && test->false_block_id.value == UINT32_MAX)) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                const Om_Mir_Block_Id false_target = implicit_false_edge
                    ? merge_block : test->false_block_id;
                Om_Mir_Value_Id *false_arguments = NULL;
                if (implicit_false_edge && predecessors.length > 1
                    && base_count != 0) {
                    false_arguments = om_alloc_array(base_count,
                        sizeof(*false_arguments));
                    if (false_arguments == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                        break;
                    }
                    for (size_t value_index = 0; value_index < base_count;
                         ++value_index) {
                        false_arguments[value_index] =
                            base_environment[value_index].value_id;
                    }
                }
                const Om_Mir_Edge_Input edges[] = {
                    {.target_block_id = test->true_block_id},
                    {.target_block_id = false_target,
                     .arguments = false_arguments,
                     .argument_count = implicit_false_edge
                            && predecessors.length > 1
                        ? base_count : 0},
                };
                if (status == OM_STATUS_OK) {
                    status = om_mir_set_terminator(context->mir, test->block_id,
                        OM_MIR_COND_BR, test->condition_value_id, edges, 2);
                }
                om_free(false_arguments);
            }
            if (status == OM_STATUS_OK) {
                context->block_id = merge_block;
                context->terminated = false;
                if (predecessors.length > 1) {
                    status = environment_from_block_parameters(context,
                        base_environment, merge_parameters, base_count);
                } else {
                    const Om_Mir_Merge_Predecessor *predecessor =
                        om_vector_at_const(&predecessors, 0);
                    if (predecessor == NULL) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                    } else {
                        status = environment_restore(context,
                            predecessor->environment,
                            predecessor->environment_count);
                    }
                }
            }
            om_free(merge_parameters);
        }
    }
    om_free(base_environment);
    destroy_merge_predecessors(&predecessors);
    om_vector_destroy(&tests);
    return status;
}

static Om_Status lower_while(Om_Mir_Lowering_Context *context,
                             const Om_Hir_Node *node)
{
    if (node->child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Env_Entry *base_environment = NULL;
    size_t base_count = 0;
    Om_Status status = environment_snapshot(context, &base_environment,
                                            &base_count);
    Om_Mir_Value_Id *header_parameters = base_count == 0 ? NULL
        : om_alloc_array(base_count, sizeof(*header_parameters));
    Om_Mir_Value_Id *exit_parameters = base_count == 0 ? NULL
        : om_alloc_array(base_count, sizeof(*exit_parameters));
    if (status == OM_STATUS_OK && base_count != 0
        && (header_parameters == NULL || exit_parameters == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Mir_Block_Id header_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &header_block);
    }
    if (status == OM_STATUS_OK) {
        status = add_block_parameters_for_environment(context, header_block,
            base_environment, base_count, header_parameters);
    }
    const Om_Mir_Block_Id preheader = context->block_id;
    if (status == OM_STATUS_OK) {
        Om_Mir_Value_Id *incoming = base_count == 0 ? NULL
            : om_alloc_array(base_count, sizeof(*incoming));
        if (base_count != 0 && incoming == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            for (size_t index = 0; index < base_count; ++index) {
                incoming[index] = base_environment[index].value_id;
            }
            if (status == OM_STATUS_OK) {
                status = set_branch(context, preheader, header_block, incoming,
                                    base_count);
            }
        }
        om_free(incoming);
    }
    if (status == OM_STATUS_OK) {
        status = environment_from_block_parameters(context, base_environment,
            header_parameters, base_count);
    }
    context->block_id = header_block;
    context->terminated = false;
    const Om_Hir_Node *condition = hir_child(context->hir, node, 0);
    const Om_Hir_Node *body = hir_child(context->hir, node, 1);
    if (condition == NULL || body == NULL) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id condition_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = lower_expression(context, condition->id, &condition_value);
    }
    Om_Mir_Block_Id body_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &body_block);
    }
    bool loop_frame_pushed = false;
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = push_loop_frame(context, base_environment, base_count,
                                 base_count, header_block);
        loop_frame_pushed = status == OM_STATUS_OK;
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        context->block_id = body_block;
        context->terminated = false;
        if (status == OM_STATUS_OK) {
            status = lower_block(context, body->id);
        }
        if (status == OM_STATUS_OK && !context->unsupported
            && !context->terminated) {
            Om_Mir_Loop_Frame *frame = current_loop_frame(context);
            Om_Mir_Value_Id *backedge_values = base_count == 0 ? NULL
                : om_alloc_array(base_count, sizeof(*backedge_values));
            if (base_count != 0 && backedge_values == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else if (frame == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                status = values_for_symbols(context, frame->symbols,
                    base_count, backedge_values);
                if (status == OM_STATUS_OK) {
                    status = add_loop_predecessor(&frame->continue_predecessors,
                        context->block_id, backedge_values, base_count);
                }
            }
            om_free(backedge_values);
        }
        if (status == OM_STATUS_OK && !context->unsupported) {
            Om_Mir_Block_Id exit_block = OM_MIR_BLOCK_ID_INVALID;
            status = om_mir_add_block(context->mir, context->function_id,
                                      &exit_block);
            if (status == OM_STATUS_OK) {
                status = add_block_parameters_for_environment(context, exit_block,
                    base_environment, base_count, exit_parameters);
            }
            if (status == OM_STATUS_OK) {
                const Om_Mir_Edge_Input edges[] = {
                    {.target_block_id = body_block},
                    {.target_block_id = exit_block,
                     .arguments = header_parameters,
                     .argument_count = base_count},
                };
                status = om_mir_set_terminator(context->mir, header_block,
                    OM_MIR_COND_BR, condition_value, edges, 2);
            }
            Om_Mir_Loop_Frame *frame = current_loop_frame(context);
            if (status == OM_STATUS_OK && frame != NULL) {
                status = finish_loop_predecessors(context,
                    &frame->continue_predecessors, header_block, base_count);
                if (status == OM_STATUS_OK) {
                    status = finish_loop_predecessors(context,
                        &frame->break_predecessors, exit_block, base_count);
                }
            }
            context->block_id = exit_block;
            context->terminated = false;
            if (status == OM_STATUS_OK) {
                status = environment_from_block_parameters(context,
                    base_environment, exit_parameters, base_count);
            }
        }
        if (loop_frame_pushed) {
            Om_Mir_Loop_Frame *frame = current_loop_frame(context);
            destroy_loop_frame(frame);
            --context->loop_frames.length;
        }
    }
    om_free(exit_parameters);
    om_free(header_parameters);
    om_free(base_environment);
    return status;
}

static Om_Status set_conditional_branch(Om_Mir_Lowering_Context *context,
                                        Om_Mir_Block_Id source_block,
                                        Om_Mir_Value_Id condition,
                                        Om_Mir_Block_Id true_block,
                                        Om_Mir_Block_Id false_block)
{
    const Om_Mir_Edge_Input edges[] = {
        {.target_block_id = true_block},
        {.target_block_id = false_block},
    };
    return om_mir_set_terminator(context->mir, source_block, OM_MIR_COND_BR,
                                  condition, edges, 2);
}

static Om_Status lower_for_range(Om_Mir_Lowering_Context *context,
                                 const Om_Hir_Node *node)
{
    if ((node->child_count != 3 && node->child_count != 4)
        || node->symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t outer_count = 0;
    Om_Mir_Env_Entry *outer_environment = NULL;
    Om_Status status = environment_snapshot(context, &outer_environment,
                                            &outer_count);
    const Om_Hir_Node *start_node = hir_child(context->hir, node, 0);
    const Om_Hir_Node *end_node = hir_child(context->hir, node, 1);
    const bool has_explicit_step = node->child_count == 4;
    const Om_Hir_Node *step_node = has_explicit_step
        ? hir_child(context->hir, node, 2) : NULL;
    const Om_Hir_Node *body_node = hir_child(context->hir, node,
                                             node->child_count - 1);
    if (start_node == NULL || end_node == NULL || body_node == NULL
        || (has_explicit_step && step_node == NULL)) {
        om_free(outer_environment);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id start_value = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id end_value = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id step_value = OM_MIR_VALUE_ID_INVALID;
    status = lower_expression(context, start_node->id, &start_value);
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = lower_expression(context, end_node->id, &end_value);
    }
    if (status == OM_STATUS_OK && !context->unsupported && has_explicit_step) {
        status = lower_expression(context, step_node->id, &step_value);
    }
    Om_Type_Id int_type = context->mir->type_store->primitive_types[OM_TYPE_INT];
    Om_Type_Id bool_type = context->mir->type_store->primitive_types[OM_TYPE_BOOL];
    Om_Type_Id unit_type = context->mir->type_store->primitive_types[OM_TYPE_UNIT];
    if (status == OM_STATUS_OK && !context->unsupported
        && !has_explicit_step) {
        status = emit_instruction(context, node, OM_MIR_CONST_INT, int_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            UINT64_C(1), 0.0, &step_value);
    }
    if (status != OM_STATUS_OK || context->unsupported) {
        om_free(outer_environment);
        return status;
    }

    const size_t transfer_count = outer_count + 1;
    if (transfer_count < outer_count) {
        om_free(outer_environment);
        return OM_STATUS_OVERFLOW;
    }
    Om_Mir_Value_Id *header_parameters = om_alloc_array(transfer_count,
        sizeof(*header_parameters));
    Om_Mir_Value_Id *exit_parameters = outer_count == 0 ? NULL
        : om_alloc_array(outer_count, sizeof(*exit_parameters));
    Om_Mir_Env_Entry *transfer_environment = om_alloc_array(transfer_count,
        sizeof(*transfer_environment));
    Om_Mir_Value_Id *preheader_arguments = om_alloc_array(transfer_count,
        sizeof(*preheader_arguments));
    if (header_parameters == NULL || transfer_environment == NULL
        || preheader_arguments == NULL
        || (outer_count != 0 && exit_parameters == NULL)) {
        om_free(preheader_arguments);
        om_free(transfer_environment);
        om_free(exit_parameters);
        om_free(header_parameters);
        om_free(outer_environment);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < outer_count; ++index) {
        transfer_environment[index] = outer_environment[index];
        preheader_arguments[index] = outer_environment[index].value_id;
    }
    preheader_arguments[outer_count] = start_value;
    const Om_Mir_Block_Id preheader = context->block_id;
    Om_Mir_Block_Id header_block = OM_MIR_BLOCK_ID_INVALID;
    status = om_mir_add_block(context->mir, context->function_id, &header_block);
    if (status == OM_STATUS_OK) {
        status = add_block_parameters_for_environment(context, header_block,
            outer_environment, outer_count, header_parameters);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block_parameter(context->mir, header_block, int_type,
            &header_parameters[outer_count]);
    }
    if (status == OM_STATUS_OK) {
        status = set_branch(context, preheader, header_block,
                            preheader_arguments, transfer_count);
    }
    transfer_environment[outer_count] = (Om_Mir_Env_Entry){
        .symbol_id = node->symbol_id,
        .value_id = header_parameters[outer_count],
    };
    if (status == OM_STATUS_OK) {
        status = environment_from_block_parameters(context,
            transfer_environment, header_parameters, transfer_count);
    }
    context->block_id = header_block;
    context->terminated = false;

    Om_Mir_Value_Id zero_value = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id zero_condition = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id positive_condition = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id positive_range_condition = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id negative_range_condition = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id comparison_operands[2] = {
        step_value, OM_MIR_VALUE_ID_INVALID,
    };
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_CONST_INT, int_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            0, 0.0, &zero_value);
    }
    comparison_operands[1] = zero_value;
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_COMPARE, bool_type,
            OM_HIR_OPERATOR_EQUAL, OM_FUNCTION_INSTANCE_ID_INVALID,
            comparison_operands, 2, 0, 0.0, &zero_condition);
    }
    Om_Mir_Block_Id fault_block = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id sign_block = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id positive_test_block = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id negative_test_block = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Block_Id body_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &fault_block);
    }
    if (status == OM_STATUS_OK) {
        context->block_id = fault_block;
        Om_Mir_Value_Id ignored = OM_MIR_VALUE_ID_INVALID;
        status = emit_instruction(context, node, OM_MIR_RUNTIME_FAULT, unit_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            (uint64_t)OM_MIR_RUNTIME_FAULT_ZERO_RANGE_STEP, 0.0, &ignored);
        if (status == OM_STATUS_OK) {
            status = om_mir_set_terminator(context->mir, fault_block,
                OM_MIR_UNREACHABLE, OM_MIR_VALUE_ID_INVALID, NULL, 0);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &sign_block);
    }
    if (status == OM_STATUS_OK) {
        context->block_id = sign_block;
        comparison_operands[1] = zero_value;
        status = emit_instruction(context, node, OM_MIR_COMPARE, bool_type,
            OM_HIR_OPERATOR_GREATER, OM_FUNCTION_INSTANCE_ID_INVALID,
            comparison_operands, 2, 0, 0.0, &positive_condition);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &positive_test_block);
    }
    if (status == OM_STATUS_OK) {
        context->block_id = positive_test_block;
        comparison_operands[0] = header_parameters[outer_count];
        comparison_operands[1] = end_value;
        status = emit_instruction(context, node, OM_MIR_COMPARE, bool_type,
            OM_HIR_OPERATOR_LESS_EQUAL, OM_FUNCTION_INSTANCE_ID_INVALID,
            comparison_operands, 2, 0, 0.0, &positive_range_condition);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &negative_test_block);
    }
    if (status == OM_STATUS_OK) {
        context->block_id = negative_test_block;
        comparison_operands[0] = header_parameters[outer_count];
        comparison_operands[1] = end_value;
        status = emit_instruction(context, node, OM_MIR_COMPARE, bool_type,
            OM_HIR_OPERATOR_GREATER_EQUAL, OM_FUNCTION_INSTANCE_ID_INVALID,
            comparison_operands, 2, 0, 0.0, &negative_range_condition);
    }
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &body_block);
    }
    bool loop_frame_pushed = false;
    if (status == OM_STATUS_OK) {
        status = push_loop_frame(context, transfer_environment, outer_count,
                                 transfer_count, header_block);
        loop_frame_pushed = status == OM_STATUS_OK;
    }
    if (status == OM_STATUS_OK) {
        context->block_id = body_block;
        context->terminated = false;
        status = environment_set(context, node->symbol_id,
            header_parameters[outer_count], true);
        if (status == OM_STATUS_OK) {
            status = lower_block(context, body_node->id);
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported
        && !context->terminated) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        Om_Mir_Value_Id *continue_values = om_alloc_array(transfer_count,
            sizeof(*continue_values));
        if (continue_values == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = values_for_symbols(context, frame->symbols, transfer_count,
                                        continue_values);
            if (status == OM_STATUS_OK) {
                status = add_loop_predecessor(&frame->continue_predecessors,
                    context->block_id, continue_values, transfer_count);
            }
        }
        om_free(continue_values);
    }

    Om_Mir_Block_Id increment_block = OM_MIR_BLOCK_ID_INVALID;
    Om_Mir_Value_Id *increment_parameters = NULL;
    if (status == OM_STATUS_OK && !context->unsupported) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else if (frame->continue_predecessors.length != 0) {
            increment_parameters = om_alloc_array(transfer_count,
                sizeof(*increment_parameters));
            if (increment_parameters == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_add_block(context->mir, context->function_id,
                                          &increment_block);
            }
            if (status == OM_STATUS_OK) {
                status = add_block_parameters_for_environment(context,
                    increment_block, transfer_environment, transfer_count,
                    increment_parameters);
            }
            if (status == OM_STATUS_OK) {
                status = finish_loop_predecessors(context,
                    &frame->continue_predecessors, increment_block,
                    transfer_count);
            }
            if (status == OM_STATUS_OK) {
                status = environment_from_block_parameters(context,
                    transfer_environment, increment_parameters, transfer_count);
            }
            context->block_id = increment_block;
            context->terminated = false;
            Om_Mir_Value_Id add_operands[2] = {
                increment_parameters[outer_count], step_value,
            };
            Om_Mir_Value_Id next_index = OM_MIR_VALUE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = emit_instruction(context, node,
                    OM_MIR_INT_ADD_CHECKED, int_type, OM_HIR_OPERATOR_ADD,
                    OM_FUNCTION_INSTANCE_ID_INVALID, add_operands, 2, 0, 0.0,
                    &next_index);
            }
            if (status == OM_STATUS_OK) {
                increment_parameters[outer_count] = next_index;
                status = set_branch(context, increment_block, header_block,
                    increment_parameters, transfer_count);
            }
        }
    }

    Om_Mir_Block_Id exit_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &exit_block);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = add_block_parameters_for_environment(context, exit_block,
            outer_environment, outer_count, exit_parameters);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = set_conditional_branch(context, header_block, zero_condition,
                                        fault_block, sign_block);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        Om_Mir_Block_Id positive_next = positive_test_block;
        Om_Mir_Block_Id negative_next = negative_test_block;
        status = set_conditional_branch(context, sign_block, positive_condition,
                                        positive_next, negative_next);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        const Om_Mir_Edge_Input positive_edges[] = {
            {.target_block_id = body_block},
            {.target_block_id = exit_block,
             .arguments = header_parameters,
             .argument_count = outer_count},
        };
        status = om_mir_set_terminator(context->mir, positive_test_block,
            OM_MIR_COND_BR, positive_range_condition, positive_edges, 2);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        const Om_Mir_Edge_Input negative_edges[] = {
            {.target_block_id = body_block},
            {.target_block_id = exit_block,
             .arguments = header_parameters,
             .argument_count = outer_count},
        };
        status = om_mir_set_terminator(context->mir, negative_test_block,
            OM_MIR_COND_BR, negative_range_condition, negative_edges, 2);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = finish_loop_predecessors(context,
                &frame->break_predecessors, exit_block, outer_count);
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        context->block_id = exit_block;
        context->terminated = false;
        status = environment_from_block_parameters(context, outer_environment,
            exit_parameters, outer_count);
    }
    if (loop_frame_pushed) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        destroy_loop_frame(frame);
        --context->loop_frames.length;
    }
    om_free(increment_parameters);
    om_free(preheader_arguments);
    om_free(transfer_environment);
    om_free(exit_parameters);
    om_free(header_parameters);
    om_free(outer_environment);
    return status;
}

static Om_Status lower_for_each(Om_Mir_Lowering_Context *context,
                                const Om_Hir_Node *node)
{
    if (node->child_count != 2 || node->symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Env_Entry *outer_environment = NULL;
    size_t outer_count = 0;
    Om_Status status = environment_snapshot(context, &outer_environment,
                                            &outer_count);
    const Om_Hir_Node *collection_node = hir_child(context->hir, node, 0);
    const Om_Hir_Node *body_node = hir_child(context->hir, node, 1);
    if (collection_node == NULL || body_node == NULL) {
        om_free(outer_environment);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Mir_Value_Id collection_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = lower_expression(context, collection_node->id,
                                  &collection_value);
    }
    if (status != OM_STATUS_OK || context->unsupported) {
        om_free(outer_environment);
        return status;
    }
    Om_Type_Id collection_type = OM_TYPE_ID_INVALID;
    Om_Type_Kind collection_kind = OM_TYPE_VARIABLE;
    status = mir_value_type(context->mir, collection_value, &collection_type);
    if (status == OM_STATUS_OK) {
        status = resolved_type_kind(context->mir->type_store, collection_type,
                                    &collection_kind);
    }
    const bool list_iteration = collection_kind == OM_TYPE_LIST;
    const bool map_iteration = collection_kind == OM_TYPE_MAP;
    if (status != OM_STATUS_OK || (!list_iteration && !map_iteration)
        || outer_count > SIZE_MAX - 2) {
        om_free(outer_environment);
        return status != OM_STATUS_OK ? status
            : (!list_iteration && !map_iteration)
                ? add_lowering_diagnostic(context, node->span)
                : OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved_collection_type = OM_TYPE_ID_INVALID;
    Om_Type_Id element_type = OM_TYPE_ID_INVALID;
    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    Om_Type_Id bool_type = OM_TYPE_ID_INVALID;
    status = om_type_store_resolve(context->mir->type_store, collection_type,
                                   &resolved_collection_type);
    if (status == OM_STATUS_OK) {
        status = om_type_store_child_at(context->mir->type_store,
            resolved_collection_type, 0, &element_type);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_primitive(context->mir->type_store, OM_TYPE_INT,
                                         &int_type);
    }
    if (status == OM_STATUS_OK) {
        status = om_type_store_primitive(context->mir->type_store, OM_TYPE_BOOL,
                                         &bool_type);
    }
    const size_t transfer_count = outer_count + 2;
    Om_Mir_Env_Entry *transfer_environment = status == OM_STATUS_OK
        ? om_alloc_array(transfer_count, sizeof(*transfer_environment)) : NULL;
    Om_Mir_Value_Id *initial_values = status == OM_STATUS_OK
        ? om_alloc_array(transfer_count, sizeof(*initial_values)) : NULL;
    Om_Mir_Value_Id *header_parameters = status == OM_STATUS_OK
        ? om_alloc_array(transfer_count, sizeof(*header_parameters)) : NULL;
    Om_Mir_Value_Id *increment_parameters = NULL;
    Om_Mir_Value_Id *exit_parameters = outer_count == 0 || status != OM_STATUS_OK
        ? NULL : om_alloc_array(outer_count, sizeof(*exit_parameters));
    if (status == OM_STATUS_OK
        && (transfer_environment == NULL || initial_values == NULL
            || header_parameters == NULL
            || (outer_count != 0 && exit_parameters == NULL))) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Symbol_Id collection_symbol = OM_SYMBOL_ID_INVALID;
    Om_Symbol_Id index_symbol = OM_SYMBOL_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = synthetic_symbol_id(context, &collection_symbol);
    }
    if (status == OM_STATUS_OK) {
        status = synthetic_symbol_id(context, &index_symbol);
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < outer_count; ++index) {
            transfer_environment[index] = outer_environment[index];
            initial_values[index] = outer_environment[index].value_id;
        }
        transfer_environment[outer_count] = (Om_Mir_Env_Entry){
            collection_symbol, collection_value,
        };
        initial_values[outer_count] = collection_value;
    }
    Om_Mir_Value_Id zero_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = emit_instruction(context, node, OM_MIR_CONST_INT, int_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0,
            0, 0.0, &zero_value);
    }
    if (status == OM_STATUS_OK) {
        transfer_environment[outer_count + 1] = (Om_Mir_Env_Entry){
            index_symbol, zero_value,
        };
        initial_values[outer_count + 1] = zero_value;
    }
    Om_Mir_Block_Id header_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &header_block);
    }
    if (status == OM_STATUS_OK) {
        status = add_block_parameters_for_environment(context, header_block,
            transfer_environment, transfer_count, header_parameters);
    }
    const Om_Mir_Block_Id preheader = context->block_id;
    if (status == OM_STATUS_OK) {
        status = set_branch(context, preheader, header_block, initial_values,
                            transfer_count);
    }
    if (status == OM_STATUS_OK) {
        status = environment_from_block_parameters(context,
            transfer_environment, header_parameters, transfer_count);
    }
    context->block_id = header_block;
    context->terminated = false;
    Om_Mir_Value_Id length_value = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id condition_value = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Value_Id length_operand = header_parameters[outer_count];
    if (status == OM_STATUS_OK) {
        const Om_Mir_Opcode length_opcode = list_iteration
            ? OM_MIR_LIST_LENGTH : OM_MIR_MAP_LENGTH;
        status = emit_instruction(context, node, length_opcode, int_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            &length_operand, 1, 0, 0.0, &length_value);
    }
    if (status == OM_STATUS_OK) {
        const Om_Mir_Value_Id compare_operands[] = {
            header_parameters[outer_count + 1], length_value,
        };
        status = emit_instruction(context, node, OM_MIR_COMPARE, bool_type,
            OM_HIR_OPERATOR_LESS, OM_FUNCTION_INSTANCE_ID_INVALID,
            compare_operands, 2, 0, 0.0, &condition_value);
    }
    Om_Mir_Block_Id body_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &body_block);
    }
    bool loop_frame_pushed = false;
    if (status == OM_STATUS_OK) {
        status = push_loop_frame(context, transfer_environment, outer_count,
                                 transfer_count, header_block);
        loop_frame_pushed = status == OM_STATUS_OK;
    }
    context->block_id = body_block;
    context->terminated = false;
    if (status == OM_STATUS_OK) {
        status = environment_from_block_parameters(context,
            transfer_environment, header_parameters, transfer_count);
    }
    Om_Mir_Value_Id element_value = OM_MIR_VALUE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        const Om_Mir_Value_Id get_operands[] = {
            header_parameters[outer_count],
            header_parameters[outer_count + 1],
        };
        const Om_Mir_Opcode get_opcode = list_iteration
            ? OM_MIR_LIST_GET : OM_MIR_MAP_KEY_AT;
        status = emit_instruction(context, node, get_opcode, element_type,
            OM_HIR_OPERATOR_NONE, OM_FUNCTION_INSTANCE_ID_INVALID,
            get_operands, 2, 0, 0.0, &element_value);
    }
    if (status == OM_STATUS_OK) {
        status = environment_set(context, node->symbol_id, element_value, true);
    }
    if (status == OM_STATUS_OK) {
        status = lower_block(context, body_node->id);
    }
    if (status == OM_STATUS_OK && !context->unsupported
        && !context->terminated) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        Om_Mir_Value_Id *values = om_alloc_array(transfer_count,
                                                 sizeof(*values));
        if (values == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = values_for_symbols(context, frame->symbols, transfer_count,
                                        values);
            if (status == OM_STATUS_OK) {
                status = add_loop_predecessor(&frame->continue_predecessors,
                    context->block_id, values, transfer_count);
            }
        }
        om_free(values);
    }
    Om_Mir_Block_Id increment_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK && !context->unsupported) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else if (frame->continue_predecessors.length != 0) {
            increment_parameters = om_alloc_array(transfer_count,
                sizeof(*increment_parameters));
            if (increment_parameters == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_add_block(context->mir, context->function_id,
                                          &increment_block);
            }
            if (status == OM_STATUS_OK) {
                status = add_block_parameters_for_environment(context,
                    increment_block, transfer_environment, transfer_count,
                    increment_parameters);
            }
            if (status == OM_STATUS_OK) {
                status = finish_loop_predecessors(context,
                    &frame->continue_predecessors, increment_block,
                    transfer_count);
            }
            if (status == OM_STATUS_OK) {
                status = environment_from_block_parameters(context,
                    transfer_environment, increment_parameters, transfer_count);
            }
            context->block_id = increment_block;
            context->terminated = false;
            Om_Mir_Value_Id one_value = OM_MIR_VALUE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = emit_instruction(context, node, OM_MIR_CONST_INT,
                    int_type, OM_HIR_OPERATOR_NONE,
                    OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0, 1, 0.0,
                    &one_value);
            }
            Om_Mir_Value_Id next_index = OM_MIR_VALUE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                const Om_Mir_Value_Id add_operands[] = {
                    increment_parameters[outer_count + 1], one_value,
                };
                status = emit_instruction(context, node,
                    OM_MIR_INT_ADD_CHECKED, int_type, OM_HIR_OPERATOR_ADD,
                    OM_FUNCTION_INSTANCE_ID_INVALID, add_operands, 2, 0, 0.0,
                    &next_index);
            }
            if (status == OM_STATUS_OK) {
                Om_Mir_Value_Id *backedge_values = om_alloc_array(
                    transfer_count, sizeof(*backedge_values));
                if (backedge_values == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                } else {
                    (void)memcpy(backedge_values, increment_parameters,
                        transfer_count * sizeof(*backedge_values));
                    backedge_values[outer_count + 1] = next_index;
                    status = set_branch(context, increment_block, header_block,
                        backedge_values, transfer_count);
                    om_free(backedge_values);
                }
            }
        }
    }
    Om_Mir_Block_Id exit_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = om_mir_add_block(context->mir, context->function_id,
                                  &exit_block);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        status = add_block_parameters_for_environment(context, exit_block,
            outer_environment, outer_count, exit_parameters);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        const Om_Mir_Edge_Input edges[] = {
            {.target_block_id = body_block},
            {.target_block_id = exit_block,
             .arguments = header_parameters,
             .argument_count = outer_count},
        };
        status = om_mir_set_terminator(context->mir, header_block,
            OM_MIR_COND_BR, condition_value, edges, 2);
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        if (frame == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = finish_loop_predecessors(context,
                &frame->break_predecessors, exit_block, outer_count);
        }
    }
    if (status == OM_STATUS_OK && !context->unsupported) {
        context->block_id = exit_block;
        context->terminated = false;
        status = environment_from_block_parameters(context, outer_environment,
            exit_parameters, outer_count);
    }
    if (loop_frame_pushed) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(context);
        destroy_loop_frame(frame);
        --context->loop_frames.length;
    }
    om_free(exit_parameters);
    om_free(increment_parameters);
    om_free(header_parameters);
    om_free(initial_values);
    om_free(transfer_environment);
    om_free(outer_environment);
    return status;
}

static Om_Status lower_statement(Om_Mir_Lowering_Context *context,
                                 Om_Hir_Node_Id node_id)
{
    if (context->terminated || context->unsupported) {
        return OM_STATUS_OK;
    }
    const Om_Hir_Node *node = hir_node(context->hir, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    switch (node->kind) {
    case OM_HIR_BLOCK:
        return lower_block(context, node_id);
    case OM_HIR_BIND:
    case OM_HIR_DECLARE_VAR: {
        if (node->child_count != 1) return OM_STATUS_INVALID_ARGUMENT;
        const Om_Hir_Node *value_node = hir_child(context->hir, node, 0);
        Om_Mir_Value_Id value_id = OM_MIR_VALUE_ID_INVALID;
        Om_Status status = value_node == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, value_node->id, &value_id);
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = environment_set(context, node->symbol_id, value_id, true);
        }
        return status;
    }
    case OM_HIR_ASSIGN_VAR: {
        if (node->child_count != 1) return OM_STATUS_INVALID_ARGUMENT;
        const Om_Hir_Node *value_node = hir_child(context->hir, node, 0);
        Om_Mir_Value_Id value_id = OM_MIR_VALUE_ID_INVALID;
        Om_Status status = value_node == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, value_node->id, &value_id);
        if (status == OM_STATUS_OK && !context->unsupported
            && environment_entry(context, node->symbol_id) == NULL) {
            status = add_lowering_diagnostic(context, node->span);
        } else if (status == OM_STATUS_OK && !context->unsupported) {
            status = environment_set(context, node->symbol_id, value_id, false);
        }
        return status;
    }
    case OM_HIR_ACTOR_STATE_ASSIGN: {
        if (node->child_count != 1
            || context->actor_state_value_id.value == UINT32_MAX
            || !symbol_is_actor_state(context, node->symbol_id)) {
            return add_lowering_diagnostic(context, node->span);
        }
        const Om_Hir_Node *value_node = hir_child(context->hir, node, 0);
        Om_Mir_Value_Id value_id = OM_MIR_VALUE_ID_INVALID;
        Om_Status status = value_node == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, value_node->id, &value_id);
        Om_Type_Id unit_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = om_type_store_primitive(context->mir->type_store,
                OM_TYPE_UNIT, &unit_type);
        }
        const Om_Mir_Value_Id operands[2] = {
            context->actor_state_value_id, value_id,
        };
        Om_Mir_Value_Id ignored = OM_MIR_VALUE_ID_INVALID;
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = emit_instruction_with_symbol(context, node,
                OM_MIR_STORE_INIT_FIELD, unit_type, node->symbol_id,
                operands, 2, &ignored);
        }
        return status;
    }
    case OM_HIR_RETURN: {
        Om_Mir_Value_Id value_id = OM_MIR_VALUE_ID_INVALID;
        if (node->child_count > 1) return OM_STATUS_INVALID_ARGUMENT;
        Om_Status status = OM_STATUS_OK;
        if (node->child_count == 1) {
            const Om_Hir_Node *value_node = hir_child(context->hir, node, 0);
            status = value_node == NULL ? OM_STATUS_INVALID_ARGUMENT
                : lower_expression(context, value_node->id, &value_id);
        }
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = om_mir_set_terminator(context->mir, context->block_id,
                OM_MIR_RETURN, value_id, NULL, 0);
            if (status == OM_STATUS_OK) context->terminated = true;
        }
        return status;
    }
    case OM_HIR_CALL_STATEMENT: {
        if (node->child_count != 1) return OM_STATUS_INVALID_ARGUMENT;
        const Om_Hir_Node *call = hir_child(context->hir, node, 0);
        Om_Mir_Value_Id ignored = OM_MIR_VALUE_ID_INVALID;
        return call == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_expression(context, call->id, &ignored);
    }
    case OM_HIR_SEND:
    case OM_HIR_WATCH:
    case OM_HIR_STOP: {
        const size_t expected_children = node->kind == OM_HIR_SEND ? 2
            : node->kind == OM_HIR_WATCH ? 1 : 0;
        if (node->child_count != expected_children) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Mir_Value_Id operands[2] = {
            OM_MIR_VALUE_ID_INVALID, OM_MIR_VALUE_ID_INVALID,
        };
        Om_Status status = OM_STATUS_OK;
        for (size_t index = 0; status == OM_STATUS_OK
             && index < expected_children; ++index) {
            const Om_Hir_Node *child = hir_child(context->hir, node, index);
            status = child == NULL ? OM_STATUS_INVALID_ARGUMENT
                : lower_expression(context, child->id, &operands[index]);
        }
        Om_Type_Id unit_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = om_type_store_primitive(context->mir->type_store,
                OM_TYPE_UNIT, &unit_type);
        }
        Om_Mir_Opcode opcode = node->kind == OM_HIR_SEND
            ? OM_MIR_RUNTIME_ACTOR_SEND
            : node->kind == OM_HIR_WATCH ? OM_MIR_RUNTIME_ACTOR_WATCH
                                         : OM_MIR_RUNTIME_ACTOR_STOP;
        Om_Mir_Value_Id ignored = OM_MIR_VALUE_ID_INVALID;
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = emit_instruction_with_symbol(context, node, opcode,
                unit_type, node->symbol_id, operands, expected_children,
                &ignored);
        }
        return status;
    }
    case OM_HIR_IF:
        return lower_if(context, node);
    case OM_HIR_WHILE:
        return lower_while(context, node);
    case OM_HIR_FOR_RANGE:
        return lower_for_range(context, node);
    case OM_HIR_FOR_EACH:
        return lower_for_each(context, node);
    case OM_HIR_SELECT:
        return lower_select(context, node);
    case OM_HIR_BREAK:
        return record_loop_transfer(context, true, node->span);
    case OM_HIR_CONTINUE:
        return record_loop_transfer(context, false, node->span);
    default:
        return add_lowering_diagnostic(context, node->span);
    }
}

static Om_Status lower_block(Om_Mir_Lowering_Context *context,
                             Om_Hir_Node_Id block_node_id)
{
    const Om_Hir_Node *block = hir_node(context->hir, block_node_id);
    if (block == NULL || block->kind != OM_HIR_BLOCK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t environment_base = context->environment.length;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && !context->terminated
         && !context->unsupported && index < block->child_count; ++index) {
        const Om_Hir_Node *statement = hir_child(context->hir, block, index);
        status = statement == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_statement(context, statement->id);
    }
    if (context->environment.length >= environment_base) {
        context->environment.length = environment_base;
    }
    return status;
}

static Om_Status actor_symbol_for_state_symbol(
    const Om_Hir_Project *hir,
    Om_Symbol_Id state_symbol_id,
    Om_Symbol_Id *actor_symbol_id)
{
    if (hir == NULL || hir->resolution == NULL || actor_symbol_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Symbol *state_symbol = om_resolution_symbol_by_id(
        hir->resolution, state_symbol_id);
    if (state_symbol == NULL
        || state_symbol->scope_id.value >= hir->resolution->scope_count) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Scope *scope = &hir->resolution->scopes[state_symbol->scope_id.value];
    if (scope->kind != OM_SCOPE_ACTOR) {
        return OM_STATUS_NOT_FOUND;
    }
    for (size_t index = 0; index < hir->resolution->symbol_count; ++index) {
        const Om_Symbol *candidate = &hir->resolution->symbols[index];
        if (candidate->kind == OM_SYMBOL_ACTOR
            && candidate->module_id.value == scope->module_id.value
            && candidate->declaration_node.value == scope->owner_node.value) {
            *actor_symbol_id = candidate->id;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

static Om_Status actor_state_type(Om_Mir_Lowering_Context *context,
                                  Om_Symbol_Id actor_symbol_id,
                                  Om_Type_Id *state_type_id)
{
    if (context == NULL || state_type_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_type_store_nominal(context->specialization->type_store,
        OM_TYPE_RECORD, actor_symbol_id, state_type_id);
}

static Om_Status instance_signature(Om_Mir_Lowering_Context *context,
                                    const Om_Function_Instance *instance,
                                    const Om_Hir_Node *root,
                                    Om_Type_Id *signature_type_id,
                                    Om_Mir_Function_Kind *function_kind)
{
    Om_Status status = OM_STATUS_OK;
    if (instance->kind == OM_FUNCTION_INSTANCE_FUNCTION
        || instance->kind == OM_FUNCTION_INSTANCE_ACTION) {
        status = specialized_type(context, root->type_id, signature_type_id);
        *function_kind = instance->kind == OM_FUNCTION_INSTANCE_FUNCTION
            ? OM_MIR_FUNCTION : OM_MIR_ACTION;
        return status;
    }
    Om_Symbol_Id actor_symbol_id = OM_SYMBOL_ID_INVALID;
    Om_Type_Id state_type_id = OM_TYPE_ID_INVALID;
    if (instance->kind == OM_FUNCTION_INSTANCE_RECEIVE) {
        if (instance->entity_id.value >= context->hir->entity_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Hir_Entity *entity =
            &context->hir->entities[instance->entity_id.value];
        if (entity->kind != OM_HIR_ENTITY_RECEIVE
            || entity->node_id.value != root->id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        actor_symbol_id = entity->related_symbol_id;
        status = actor_state_type(context, actor_symbol_id, &state_type_id);
        Om_Type_Id message_type_id = OM_TYPE_ID_INVALID;
        Om_Type_Id unit_type_id = OM_TYPE_ID_INVALID;
        Om_Type_Id parameters[2] = {state_type_id, OM_TYPE_ID_INVALID};
        if (status == OM_STATUS_OK) {
            status = om_type_store_nominal(context->specialization->type_store,
                OM_TYPE_MESSAGE, entity->symbol_id, &message_type_id);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_primitive(context->specialization->type_store,
                OM_TYPE_UNIT, &unit_type_id);
        }
        parameters[1] = message_type_id;
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(context->specialization->type_store,
                OM_TYPE_ACTION, parameters, 2, unit_type_id,
                signature_type_id);
        }
        *function_kind = OM_MIR_RECEIVE;
        return status;
    }
    if (instance->kind == OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT) {
        status = actor_symbol_for_state_symbol(context->hir, instance->symbol_id,
                                               &actor_symbol_id);
        if (status == OM_STATUS_OK) {
            status = actor_state_type(context, actor_symbol_id, &state_type_id);
        }
        const Om_Hir_Node *value = root->child_count == 1
            ? hir_child(context->hir, root, 0) : NULL;
        Om_Type_Id value_type_id = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK && value == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = specialized_type(context, value->type_id, &value_type_id);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(context->specialization->type_store,
                OM_TYPE_FUNCTION, &state_type_id, 1, value_type_id,
                signature_type_id);
        }
        *function_kind = OM_MIR_ACTOR_STATE_INIT;
        return status;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status collect_subtree_effects(const Om_Hir_Project *hir,
                                         const Om_Hir_Node *node,
                                         size_t depth,
                                         uint32_t *effects)
{
    if (hir == NULL || node == NULL || effects == NULL
        || depth >= hir->node_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *effects |= node->effects;
    for (size_t index = 0; index < node->child_count; ++index) {
        const Om_Hir_Node *child = hir_child(hir, node, index);
        const Om_Status status = collect_subtree_effects(hir, child,
            depth + 1, effects);
        if (status != OM_STATUS_OK) return status;
    }
    return OM_STATUS_OK;
}

static Om_Status prepare_functions(Om_Mir_Lowering_Context *context)
{
    for (size_t index = 0; index < context->specialization->instance_count;
         ++index) {
        const Om_Function_Instance *instance =
            &context->specialization->instances[index];
        const Om_Hir_Node *root = hir_node(context->hir, instance->root_node);
        if (root == NULL
            || (instance->kind == OM_FUNCTION_INSTANCE_FUNCTION
                && root->kind != OM_HIR_FUNCTION)
            || (instance->kind == OM_FUNCTION_INSTANCE_ACTION
                && root->kind != OM_HIR_ACTION)
            || (instance->kind == OM_FUNCTION_INSTANCE_RECEIVE
                && root->kind != OM_HIR_RECEIVE)
            || (instance->kind == OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT
                && root->kind != OM_HIR_ACTOR_STATE_INIT)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        context->instance_id = instance->id;
        Om_Type_Id signature_type = OM_TYPE_ID_INVALID;
        Om_Mir_Function_Kind function_kind = OM_MIR_FUNCTION;
        Om_Status status = instance_signature(context, instance, root,
                                               &signature_type,
                                               &function_kind);
        if (status != OM_STATUS_OK) return status;
        uint32_t function_effects = root->effects;
        if (function_kind == OM_MIR_RECEIVE
            || function_kind == OM_MIR_ACTOR_STATE_INIT) {
            status = collect_subtree_effects(context->hir, root, 0,
                                             &function_effects);
            if (status != OM_STATUS_OK) return status;
        }
        Om_Mir_Function_Id function_id = OM_MIR_FUNCTION_ID_INVALID;
        status = om_mir_add_function(context->mir, instance->id,
            function_kind,
            root->module_id, root->symbol_id, signature_type, function_effects,
            root->span, &function_id);
        if (status == OM_STATUS_OK) {
            const Om_Symbol *symbol = om_resolution_symbol_by_id(
                context->hir->resolution, root->symbol_id);
            if (symbol != NULL && symbol->name.length != 0) {
                status = om_mir_set_function_debug_name(context->mir,
                    function_id, symbol->name);
            }
        }
        if (status != OM_STATUS_OK) return status;
        const Om_Mir_Lowered_Instance lowered = {
            instance->id, function_id, instance->root_node,
        };
        status = om_vector_push(&context->lowered_instances, &lowered);
        if (status != OM_STATUS_OK) return status;
    }
    return OM_STATUS_OK;
}

static Om_Status lower_function_body(Om_Mir_Lowering_Context *context,
                                     const Om_Mir_Lowered_Instance *lowered)
{
    const Om_Hir_Node *root = hir_node(context->hir, lowered->root_node_id);
    if (root == NULL) return OM_STATUS_INVALID_ARGUMENT;
    const Om_Mir_Function *function =
        &context->mir->functions[lowered->function_id.value];
    Om_Type_Id signature_type = OM_TYPE_ID_INVALID;
    Om_Type_View signature_view = {0};
    Om_Status status = om_type_store_resolve(context->mir->type_store,
        function->signature_type_id, &signature_type);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(context->mir->type_store, signature_type,
                                   &signature_view);
    }
    context->function_id = function->id;
    context->instance_id = function->instance_id;
    context->actor_state_value_id = OM_MIR_VALUE_ID_INVALID;
    Om_Mir_Block_Id entry_block = OM_MIR_BLOCK_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_mir_add_block(context->mir, function->id, &entry_block);
    }
    context->block_id = entry_block;
    context->terminated = false;
    context->unsupported = false;
    om_vector_clear(&context->environment);
    if (status == OM_STATUS_OK && function->kind == OM_MIR_ACTOR_STATE_INIT) {
        Om_Type_Id state_type = OM_TYPE_ID_INVALID;
        Om_Mir_Value_Id state_parameter = OM_MIR_VALUE_ID_INVALID;
        status = om_type_store_child_at(context->mir->type_store,
            signature_type, 0, &state_type);
        if (status == OM_STATUS_OK) {
            status = om_mir_add_block_parameter(context->mir, entry_block,
                state_type, &state_parameter);
        }
        context->actor_state_value_id = state_parameter;
        const Om_Hir_Node *initializer = root->child_count == 1
            ? hir_child(context->hir, root, 0) : NULL;
        Om_Mir_Value_Id value_id = OM_MIR_VALUE_ID_INVALID;
        if (status == OM_STATUS_OK && initializer == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = lower_expression(context, initializer->id, &value_id);
        }
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = om_mir_set_terminator(context->mir, context->block_id,
                OM_MIR_RETURN, value_id, NULL, 0);
            if (status == OM_STATUS_OK) context->terminated = true;
        }
    } else if (status == OM_STATUS_OK && function->kind == OM_MIR_RECEIVE) {
        if (signature_view.kind != OM_TYPE_ACTION
            || signature_view.child_count != 3 || root->child_count == 0) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id state_type = OM_TYPE_ID_INVALID;
        Om_Type_Id message_type = OM_TYPE_ID_INVALID;
        Om_Mir_Value_Id message_value = OM_MIR_VALUE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(context->mir->type_store,
                signature_type, 0, &state_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(context->mir->type_store,
                signature_type, 1, &message_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_add_block_parameter(context->mir, entry_block,
                state_type, &context->actor_state_value_id);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_add_block_parameter(context->mir, entry_block,
                message_type, &message_value);
        }
        const size_t receive_parameter_count = root->child_count - 1;
        const Om_Mir_Nominal_Descriptor *message_nominal = NULL;
        for (size_t nominal_index = 0;
             nominal_index < context->mir->nominal_count; ++nominal_index) {
            const Om_Mir_Nominal_Descriptor *nominal =
                &context->mir->nominals[nominal_index];
            if (nominal->type_kind == OM_TYPE_MESSAGE
                && nominal->symbol_id.value == root->symbol_id.value) {
                message_nominal = nominal;
                break;
            }
        }
        if (status == OM_STATUS_OK
            && (message_nominal == NULL
                || message_nominal->field_count != receive_parameter_count)) {
            status = add_lowering_diagnostic(context, root->span);
        }
        for (size_t index = 0; status == OM_STATUS_OK && !context->unsupported
             && index < receive_parameter_count; ++index) {
            const Om_Hir_Node *parameter = hir_child(context->hir, root, index);
            const Om_Mir_Field_Descriptor *field = &context->mir->fields[
                message_nominal->field_start + index];
            Om_Type_Id field_type = OM_TYPE_ID_INVALID;
            Om_Mir_Value_Id field_value = OM_MIR_VALUE_ID_INVALID;
            if (parameter == NULL || parameter->kind != OM_HIR_PARAMETER
                || field->symbol_id.value == UINT32_MAX) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            status = specialized_type(context, parameter->type_id,
                                      &field_type);
            if (status == OM_STATUS_OK
                && !types_match(context->mir->type_store, field_type,
                                field->type_id)) {
                status = add_lowering_diagnostic(context, parameter->span);
            }
            if (status == OM_STATUS_OK && !context->unsupported) {
                status = emit_instruction_with_symbol(context, parameter,
                    OM_MIR_LOAD_FIELD, field_type, field->symbol_id,
                    &message_value, 1, &field_value);
            }
            if (status == OM_STATUS_OK) {
                const Om_Mir_Env_Entry entry_value = {
                    .symbol_id = parameter->symbol_id,
                    .value_id = field_value,
                };
                status = om_vector_push(&context->environment, &entry_value);
            }
        }
        const Om_Hir_Node *body = hir_child(context->hir, root,
                                            root->child_count - 1);
        if (status == OM_STATUS_OK && !context->unsupported && body == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK && !context->unsupported) {
            status = lower_block(context, body->id);
        }
        if (status == OM_STATUS_OK && !context->unsupported
            && !context->terminated) {
            status = om_mir_set_terminator(context->mir, context->block_id,
                OM_MIR_RETURN, OM_MIR_VALUE_ID_INVALID, NULL, 0);
            if (status == OM_STATUS_OK) context->terminated = true;
        }
    } else if (status == OM_STATUS_OK) {
        if (signature_view.child_count == 0
            || root->child_count != signature_view.child_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        const size_t parameter_count = signature_view.child_count - 1;
        for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count;
             ++index) {
            const Om_Hir_Node *parameter = hir_child(context->hir, root, index);
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            Om_Type_Id signature_parameter_type = OM_TYPE_ID_INVALID;
            Om_Mir_Value_Id parameter_value = OM_MIR_VALUE_ID_INVALID;
            if (parameter == NULL || parameter->kind != OM_HIR_PARAMETER) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            status = specialized_type(context, parameter->type_id,
                                      &parameter_type);
            if (status == OM_STATUS_OK) {
                status = om_type_store_child_at(context->mir->type_store,
                    signature_type, index, &signature_parameter_type);
            }
            if (status == OM_STATUS_OK
                && !types_match(context->mir->type_store, parameter_type,
                                signature_parameter_type)) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_add_block_parameter(context->mir, entry_block,
                    parameter_type, &parameter_value);
            }
            if (status != OM_STATUS_OK) break;
            const Om_Mir_Env_Entry entry_value = {
                .symbol_id = parameter->symbol_id,
                .value_id = parameter_value,
            };
            status = om_vector_push(&context->environment, &entry_value);
        }
        const Om_Hir_Node *body = hir_child(context->hir, root,
                                            root->child_count - 1);
        if (status == OM_STATUS_OK && body == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = lower_block(context, body->id);
        }
        if (status == OM_STATUS_OK && !context->unsupported
            && !context->terminated) {
            Om_Type_Id return_type = OM_TYPE_ID_INVALID;
            Om_Type_Kind return_kind = OM_TYPE_VARIABLE;
            status = om_type_store_child_at(context->mir->type_store,
                signature_type, signature_view.child_count - 1, &return_type);
            if (status == OM_STATUS_OK) {
                status = resolved_type_kind(context->mir->type_store,
                    return_type, &return_kind);
            }
            if (status == OM_STATUS_OK && return_kind != OM_TYPE_UNIT) {
                status = add_lowering_diagnostic(context, root->span);
            } else if (status == OM_STATUS_OK) {
                status = om_mir_set_terminator(context->mir, context->block_id,
                    OM_MIR_RETURN, OM_MIR_VALUE_ID_INVALID, NULL, 0);
                if (status == OM_STATUS_OK) context->terminated = true;
            }
        }
    }
    if (status == OM_STATUS_OK && context->unsupported
        && context->block_id.value < context->mir->block_count
        && !context->mir->blocks[context->block_id.value].has_terminator) {
        status = om_mir_set_terminator(context->mir, context->block_id,
            OM_MIR_UNREACHABLE, OM_MIR_VALUE_ID_INVALID, NULL, 0);
    }
    return status;
}

static Om_System_Message_Id system_message_id_for_builtin(Om_Builtin_Id id)
{
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE) {
        return OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED) {
        return OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE_COMPLETED;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ) {
        return OM_SYSTEM_MESSAGE_ID_FILE_READ;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ_COMPLETED) {
        return OM_SYSTEM_MESSAGE_ID_FILE_READ_COMPLETED;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE) {
        return OM_SYSTEM_MESSAGE_ID_FILE_WRITE;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED) {
        return OM_SYSTEM_MESSAGE_ID_FILE_WRITE_COMPLETED;
    }
    if (id == OM_BUILTIN_SYSTEM_MESSAGE_TIMER_AFTER) {
        return OM_SYSTEM_MESSAGE_ID_TIMER_AFTER;
    }
    return OM_SYSTEM_MESSAGE_ID_NONE;
}

static Om_String_View canonical_module_path_for(
    const Om_Hir_Project *hir,
    Om_Module_Id module_id)
{
    if (hir == NULL) return (Om_String_View){0};
    for (size_t index = 0; index < hir->module_count; ++index) {
        if (hir->modules[index].id.value == module_id.value) {
            return hir->modules[index].canonical_path;
        }
    }
    return (Om_String_View){0};
}

static size_t system_message_field_count(Om_System_Message_Id message_id)
{
    switch (message_id) {
    case OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE:
    case OM_SYSTEM_MESSAGE_ID_FILE_READ:
        return 2;
    case OM_SYSTEM_MESSAGE_ID_FILE_WRITE:
    case OM_SYSTEM_MESSAGE_ID_TIMER_AFTER:
        return 3;
    case OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE_COMPLETED:
    case OM_SYSTEM_MESSAGE_ID_FILE_READ_COMPLETED:
    case OM_SYSTEM_MESSAGE_ID_FILE_WRITE_COMPLETED:
        return 1;
    case OM_SYSTEM_MESSAGE_ID_NONE:
    case OM_SYSTEM_MESSAGE_ID_ANY:
        break;
    }
    return 0;
}

static Om_Status collect_system_message_descriptors(
    Om_Mir_Lowering_Context *context)
{
    const Om_Hir_Project *hir = context->hir;
    Om_Mir_Project *mir = context->mir;
    bool timer_after_used = false;
    for (size_t node_index = 0; node_index < hir->node_count; ++node_index) {
        const Om_Hir_Node *node = &hir->nodes[node_index];
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            hir->resolution, node->symbol_id);
        if (node->kind == OM_HIR_MESSAGE_CONSTRUCT
            && symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
            && system_message_id_for_builtin(symbol->builtin_id)
                == OM_SYSTEM_MESSAGE_ID_TIMER_AFTER) {
            timer_after_used = true;
            break;
        }
    }

    Om_Symbol_Id any_message_symbol = OM_SYMBOL_ID_INVALID;
    Om_Type_Id any_message_type = OM_TYPE_ID_INVALID;
    Om_Status status = OM_STATUS_OK;
    if (timer_after_used) {
        status = synthetic_symbol_id(context, &any_message_symbol);
        if (status == OM_STATUS_OK) {
            status = om_type_store_nominal(context->specialization->type_store,
                OM_TYPE_MESSAGE, any_message_symbol, &any_message_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_add_nominal_descriptor(mir, any_message_symbol,
                OM_TYPE_MESSAGE, NULL, 0);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_nominal_identity(mir, any_message_symbol,
                (Om_String_View){"<ombasic-runtime>", 17},
                (Om_String_View){"Any", 3});
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_system_message_id(mir, any_message_symbol,
                OM_SYSTEM_MESSAGE_ID_ANY);
        }
    }

    for (size_t symbol_index = 0; status == OM_STATUS_OK
         && symbol_index < hir->resolution->symbol_count; ++symbol_index) {
        const Om_Symbol *symbol = &hir->resolution->symbols[symbol_index];
        if (symbol->kind != OM_SYMBOL_BUILTIN) continue;
        const Om_System_Message_Id message_id =
            system_message_id_for_builtin(symbol->builtin_id);
        const size_t field_count = system_message_field_count(message_id);
        if (field_count == 0) continue;

        Om_Type_Id *field_types = om_alloc_array(field_count,
                                                  sizeof(*field_types));
        Om_Mir_Field_Descriptor *fields = om_alloc_array(field_count,
                                                          sizeof(*fields));
        if (field_types == NULL || fields == NULL) {
            om_free(field_types);
            om_free(fields);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        bool found_layout = false;
        for (size_t node_index = 0; status == OM_STATUS_OK
             && !found_layout && node_index < hir->node_count; ++node_index) {
            const Om_Hir_Node *node = &hir->nodes[node_index];
            if (node->symbol_id.value != symbol->id.value) continue;
            const bool is_constructor = node->kind == OM_HIR_MESSAGE_CONSTRUCT;
            const bool is_receive = node->kind == OM_HIR_RECEIVE;
            if (!is_constructor && !is_receive) continue;
            if (node->child_count == 0
                || node->child_count - 1 != field_count) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            for (size_t field_index = 0; status == OM_STATUS_OK
                 && field_index < field_count; ++field_index) {
                if (message_id == OM_SYSTEM_MESSAGE_ID_TIMER_AFTER
                    && field_index == 1) {
                    field_types[field_index] = any_message_type;
                    continue;
                }
                const size_t child_index = is_constructor
                    ? field_index + 1 : field_index;
                const Om_Hir_Node *field = hir_child(hir, node, child_index);
                if (field == NULL
                    || (is_receive && field->kind != OM_HIR_PARAMETER)) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                status = om_type_store_resolve(mir->type_store,
                    field->type_id, &field_types[field_index]);
            }
            if (status == OM_STATUS_OK) found_layout = true;
        }
        if (status == OM_STATUS_OK && found_layout) {
            for (size_t field_index = 0; status == OM_STATUS_OK
                 && field_index < field_count; ++field_index) {
                Om_Symbol_Id field_symbol = OM_SYMBOL_ID_INVALID;
                status = synthetic_symbol_id(context, &field_symbol);
                if (status == OM_STATUS_OK) {
                    fields[field_index] = (Om_Mir_Field_Descriptor){
                        .symbol_id = field_symbol,
                        .type_id = field_types[field_index],
                    };
                }
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_add_nominal_descriptor(mir, symbol->id,
                    OM_TYPE_MESSAGE, fields, field_count);
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_set_nominal_identity(mir, symbol->id,
                    (Om_String_View){"<ombasic-runtime>", 17}, symbol->name);
            }
            if (status == OM_STATUS_OK) {
                status = om_mir_set_system_message_id(mir, symbol->id,
                                                       message_id);
            }
        }
        om_free(fields);
        om_free(field_types);
    }
    return status;
}

static Om_Status collect_nominal_descriptors(
    Om_Mir_Lowering_Context *context)
{
    const Om_Hir_Project *hir = context->hir;
    Om_Mir_Project *mir = context->mir;
    for (size_t entity_index = 0; entity_index < hir->entity_count;
         ++entity_index) {
        const Om_Hir_Entity *entity = &hir->entities[entity_index];
        if (entity->kind != OM_HIR_ENTITY_RECORD
            && entity->kind != OM_HIR_ENTITY_MESSAGE
            && entity->kind != OM_HIR_ENTITY_ACTOR) {
            continue;
        }
        const Om_Hir_Node *declaration = hir_node(hir, entity->node_id);
        if (declaration == NULL
            || (entity->kind == OM_HIR_ENTITY_RECORD
                && declaration->kind != OM_HIR_RECORD)
            || (entity->kind == OM_HIR_ENTITY_MESSAGE
                && declaration->kind != OM_HIR_MESSAGE)
            || (entity->kind == OM_HIR_ENTITY_ACTOR
                && declaration->kind != OM_HIR_ACTOR)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        size_t field_count = declaration->child_count;
        if (entity->kind == OM_HIR_ENTITY_ACTOR) {
            field_count = 0;
            for (size_t child_index = 0;
                 child_index < declaration->child_count; ++child_index) {
                const Om_Hir_Node *child = hir_child(hir, declaration,
                                                     child_index);
                if (child == NULL) return OM_STATUS_INVALID_ARGUMENT;
                if (child->kind == OM_HIR_ACTOR_STATE_INIT) ++field_count;
                else if (child->kind != OM_HIR_RECEIVE) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
            }
        }
        Om_Mir_Field_Descriptor *fields = field_count == 0
            ? NULL : om_alloc_array(field_count, sizeof(*fields));
        if (field_count != 0 && fields == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        Om_Status status = OM_STATUS_OK;
        size_t field_index = 0;
        for (size_t child_index = 0;
             status == OM_STATUS_OK && child_index < declaration->child_count;
             ++child_index) {
            const Om_Hir_Node *field = hir_child(hir, declaration, child_index);
            if (field == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (entity->kind == OM_HIR_ENTITY_ACTOR) {
                if (field->kind != OM_HIR_ACTOR_STATE_INIT) continue;
                const Om_Hir_Node *value = field->child_count == 1
                    ? hir_child(hir, field, 0) : NULL;
                if (value == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                Om_Type_Id field_type = OM_TYPE_ID_INVALID;
                status = om_type_store_resolve(hir->type_store, value->type_id,
                                               &field_type);
                if (status == OM_STATUS_OK) {
                    fields[field_index++] = (Om_Mir_Field_Descriptor){
                        .symbol_id = field->symbol_id,
                        .type_id = field_type,
                    };
                }
            } else {
                if (field->kind != OM_HIR_PARAMETER) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                Om_Type_Id field_type = OM_TYPE_ID_INVALID;
                status = om_type_store_resolve(hir->type_store, field->type_id,
                                               &field_type);
                if (status == OM_STATUS_OK) {
                    fields[field_index++] = (Om_Mir_Field_Descriptor){
                        .symbol_id = field->symbol_id,
                        .type_id = field_type,
                    };
                }
            }
        }
        if (status == OM_STATUS_OK) {
            const Om_Type_Kind kind = entity->kind == OM_HIR_ENTITY_MESSAGE
                ? OM_TYPE_MESSAGE : OM_TYPE_RECORD;
            status = om_mir_add_nominal_descriptor(mir, entity->symbol_id,
                kind, fields, field_count);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_nominal_identity(mir, entity->symbol_id,
                canonical_module_path_for(hir, entity->module_id),
                entity->name);
        }
        om_free(fields);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    Om_Status system_message_status = collect_system_message_descriptors(
        context);
    if (system_message_status != OM_STATUS_OK) {
        return system_message_status;
    }
    bool has_actor = false;
    for (size_t entity_index = 0; entity_index < hir->entity_count;
         ++entity_index) {
        has_actor = has_actor
            || hir->entities[entity_index].kind == OM_HIR_ENTITY_ACTOR;
    }
    if (has_actor) {
        const Om_Symbol *actor_down_symbol = NULL;
        for (size_t symbol_index = 0;
             symbol_index < hir->resolution->symbol_count; ++symbol_index) {
            const Om_Symbol *symbol = &hir->resolution->symbols[symbol_index];
            if (symbol->kind == OM_SYMBOL_BUILTIN
                && symbol->builtin_id == OM_BUILTIN_ACTOR_DOWN) {
                actor_down_symbol = symbol;
                break;
            }
        }
        if (actor_down_symbol == NULL) return OM_STATUS_NOT_FOUND;
        Om_Type_Id actor_id_type = OM_TYPE_ID_INVALID;
        Om_Type_Id reason_type = OM_TYPE_ID_INVALID;
        Om_Status status = om_type_store_primitive(context->specialization->type_store,
            OM_TYPE_ACTOR_ID, &actor_id_type);
        if (status == OM_STATUS_OK) {
            Om_Symbol_Id reason_type_symbol = OM_SYMBOL_ID_INVALID;
            for (size_t symbol_index = 0;
                 symbol_index < hir->resolution->symbol_count;
                 ++symbol_index) {
                const Om_Symbol *candidate =
                    &hir->resolution->symbols[symbol_index];
                if (candidate->kind == OM_SYMBOL_BUILTIN
                    && candidate->builtin_id
                        == OM_BUILTIN_ACTOR_DOWN_REASON) {
                    reason_type_symbol = candidate->id;
                    break;
                }
            }
            status = reason_type_symbol.value == UINT32_MAX
                ? OM_STATUS_NOT_FOUND
                : om_type_store_nominal(
                    context->specialization->type_store,
                    OM_TYPE_DOWN_REASON, reason_type_symbol, &reason_type);
        }
        Om_Symbol_Id actor_id_field = OM_SYMBOL_ID_INVALID;
        Om_Symbol_Id reason_field = OM_SYMBOL_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = synthetic_symbol_id(context, &actor_id_field);
        }
        if (status == OM_STATUS_OK) {
            status = synthetic_symbol_id(context, &reason_field);
        }
        if (status == OM_STATUS_OK) {
            const Om_Mir_Field_Descriptor fields[2] = {
                {.symbol_id = actor_id_field, .type_id = actor_id_type},
                {.symbol_id = reason_field, .type_id = reason_type},
            };
            status = om_mir_add_nominal_descriptor(mir,
                actor_down_symbol->id, OM_TYPE_MESSAGE, fields, 2);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_nominal_identity(mir,
                actor_down_symbol->id,
                (Om_String_View){"<ombasic-runtime>", 17},
                actor_down_symbol->name);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_nominal(context->specialization->type_store,
                OM_TYPE_MESSAGE, actor_down_symbol->id,
                &mir->actor_down_message_type_id);
        }
        if (status != OM_STATUS_OK) return status;
        mir->actor_down_message_symbol_id = actor_down_symbol->id;
    }
    return OM_STATUS_OK;
}

static const Om_Function_Instance *instance_for_root(
    const Om_Hir_Specialization *specialization,
    Om_Function_Instance_Kind kind,
    Om_Hir_Node_Id root_node)
{
    for (size_t index = 0; index < specialization->instance_count; ++index) {
        const Om_Function_Instance *instance = &specialization->instances[index];
        if (instance->kind == kind
            && instance->root_node.value == root_node.value) {
            return instance;
        }
    }
    return NULL;
}

static Om_Status collect_actor_descriptors(
    const Om_Hir_Project *hir,
    const Om_Hir_Specialization *specialization,
    Om_Mir_Project *mir)
{
    for (size_t entity_index = 0; entity_index < hir->entity_count;
         ++entity_index) {
        const Om_Hir_Entity *entity = &hir->entities[entity_index];
        if (entity->kind != OM_HIR_ENTITY_ACTOR) continue;
        const Om_Hir_Node *actor = hir_node(hir, entity->node_id);
        if (actor == NULL || actor->kind != OM_HIR_ACTOR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        size_t state_count = 0;
        size_t handler_count = 0;
        for (size_t child_index = 0; child_index < actor->child_count;
             ++child_index) {
            const Om_Hir_Node *child = hir_child(hir, actor, child_index);
            if (child == NULL) return OM_STATUS_INVALID_ARGUMENT;
            if (child->kind == OM_HIR_ACTOR_STATE_INIT) ++state_count;
            else if (child->kind == OM_HIR_RECEIVE) ++handler_count;
        }
        Om_Mir_Actor_State_Initializer *initializers = state_count == 0
            ? NULL : om_alloc_array(state_count, sizeof(*initializers));
        Om_Mir_Actor_Handler *handlers = handler_count == 0
            ? NULL : om_alloc_array(handler_count, sizeof(*handlers));
        if ((state_count != 0 && initializers == NULL)
            || (handler_count != 0 && handlers == NULL)) {
            om_free(initializers);
            om_free(handlers);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        size_t state_index = 0;
        size_t handler_index = 0;
        Om_Status status = OM_STATUS_OK;
        for (size_t child_index = 0; status == OM_STATUS_OK
             && child_index < actor->child_count; ++child_index) {
            const Om_Hir_Node *child = hir_child(hir, actor, child_index);
            if (child == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (child->kind == OM_HIR_ACTOR_STATE_INIT) {
                const Om_Function_Instance *instance = instance_for_root(
                    specialization, OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT,
                    child->id);
                if (instance == NULL) {
                    status = OM_STATUS_NOT_FOUND;
                    break;
                }
                initializers[state_index++] = (Om_Mir_Actor_State_Initializer){
                    .field_symbol_id = child->symbol_id,
                    .function_instance_id = instance->id,
                };
            } else if (child->kind == OM_HIR_RECEIVE) {
                const Om_Hir_Entity *receive_entity = NULL;
                for (size_t receive_index = 0;
                     receive_index < hir->entity_count; ++receive_index) {
                    const Om_Hir_Entity *candidate =
                        &hir->entities[receive_index];
                    if (candidate->kind == OM_HIR_ENTITY_RECEIVE
                        && candidate->node_id.value == child->id.value
                        && candidate->related_symbol_id.value
                            == entity->symbol_id.value) {
                        receive_entity = candidate;
                        break;
                    }
                }
                const Om_Function_Instance *instance = receive_entity == NULL
                    ? NULL : instance_for_root(specialization,
                        OM_FUNCTION_INSTANCE_RECEIVE, child->id);
                if (receive_entity == NULL || instance == NULL) {
                    status = OM_STATUS_NOT_FOUND;
                    break;
                }
                Om_Type_Id message_type_id = OM_TYPE_ID_INVALID;
                status = om_type_store_nominal(specialization->type_store,
                    OM_TYPE_MESSAGE, receive_entity->symbol_id,
                    &message_type_id);
                if (status != OM_STATUS_OK) break;
                handlers[handler_index++] = (Om_Mir_Actor_Handler){
                    .message_symbol_id = receive_entity->symbol_id,
                    .message_type_id = message_type_id,
                    .function_instance_id = instance->id,
                };
            }
        }
        Om_Type_Id state_type = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_nominal(specialization->type_store,
                OM_TYPE_RECORD, entity->symbol_id, &state_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_add_actor_descriptor(mir, entity->symbol_id,
                state_type, initializers, state_count, handlers, handler_count);
        }
        if (status == OM_STATUS_OK) {
            status = om_mir_set_actor_identity(mir, entity->symbol_id,
                canonical_module_path_for(hir, entity->module_id),
                entity->name);
        }
        om_free(initializers);
        om_free(handlers);
        if (status != OM_STATUS_OK) return status;
    }
    return OM_STATUS_OK;
}

Om_Status om_mir_lower_project(const Om_Hir_Project *hir,
                               const Om_Hir_Specialization *specialization,
                               Om_Mir_Project *result,
                               Om_Mir_Lowering_Result *lowering_result)
{
    if (hir == NULL || specialization == NULL || result == NULL
        || lowering_result == NULL || hir->owned_storage == NULL
        || specialization->owned_storage == NULL || hir->type_store == NULL
        || specialization->type_store != hir->type_store
        || result->owned_storage != NULL || result->functions != NULL
        || result->blocks != NULL || result->values != NULL
        || lowering_result->owned_diagnostics != NULL
        || lowering_result->diagnostics != NULL
        || lowering_result->diagnostic_count != 0
        || hir->resolution == NULL
        || hir->resolution->symbol_count >= UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector *diagnostics = om_alloc(sizeof(*diagnostics));
    if (diagnostics == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *diagnostics = (Om_Vector){0};
    Om_Status status = om_vector_init(diagnostics, sizeof(Om_Mir_Diagnostic));
    if (status != OM_STATUS_OK) {
        om_free(diagnostics);
        return status;
    }
    lowering_result->owned_diagnostics = diagnostics;
    status = om_mir_project_init(result, hir->type_store);
    if (status != OM_STATUS_OK) {
        om_mir_lowering_result_destroy(lowering_result);
        return status;
    }
    result->resolution = hir->resolution;
    Om_Mir_Lowering_Context context = {
        .hir = hir,
        .specialization = specialization,
        .mir = result,
        .result = lowering_result,
        .next_synthetic_symbol_id = hir->resolution == NULL
            ? 0 : hir->resolution->symbol_count,
    };
    status = collect_nominal_descriptors(&context);
    if (status == OM_STATUS_OK) {
        status = collect_actor_descriptors(hir, specialization, result);
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.lowered_instances,
                                sizeof(Om_Mir_Lowered_Instance));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.environment, sizeof(Om_Mir_Env_Entry));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.loop_frames, sizeof(Om_Mir_Loop_Frame));
    }
    if (status == OM_STATUS_OK) status = prepare_functions(&context);
    for (size_t index = 0; status == OM_STATUS_OK
         && index < context.lowered_instances.length; ++index) {
        const Om_Mir_Lowered_Instance *instance = om_vector_at_const(
            &context.lowered_instances, index);
        status = instance == NULL ? OM_STATUS_INVALID_ARGUMENT
            : lower_function_body(&context, instance);
    }
    while (context.loop_frames.length != 0) {
        Om_Mir_Loop_Frame *frame = current_loop_frame(&context);
        destroy_loop_frame(frame);
        --context.loop_frames.length;
    }
    om_vector_destroy(&context.loop_frames);
    om_vector_destroy(&context.environment);
    om_vector_destroy(&context.lowered_instances);
    if (status != OM_STATUS_OK) {
        om_mir_project_destroy(result);
        om_mir_lowering_result_destroy(lowering_result);
        return status;
    }
    lowering_result->diagnostics = diagnostics->data;
    lowering_result->diagnostic_count = diagnostics->length;
    return OM_STATUS_OK;
}

void om_mir_lowering_result_destroy(Om_Mir_Lowering_Result *result)
{
    if (result == NULL) return;
    Om_Vector *diagnostics = diagnostic_vector(result);
    if (diagnostics != NULL) {
        om_vector_destroy(diagnostics);
        om_free(diagnostics);
    }
    *result = (Om_Mir_Lowering_Result){0};
}
