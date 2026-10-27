#include "ombasic/bytecode/bytecode.h"

#include <limits.h>
#include <string.h>

#include "../bytecode_internal.h"
#include "ombasic/foundation/fingerprint.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/system_actors.h"
#include "ombasic/foundation/utf8.h"

typedef struct Om_Emit_Local {
    Om_Symbol_Id symbol;
    uint32_t slot;
} Om_Emit_Local;

typedef struct Om_Emit_Function_Decl {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Symbol_Id symbol_id;
    uint32_t function_index;
    uint32_t actor_index;
    Om_Symbol_Id message_symbol;
    uint32_t flags;
} Om_Emit_Function_Decl;

typedef struct Om_Emit_Actor_Decl {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Symbol_Id symbol_id;
    uint32_t initializer_function;
    uint32_t state_count;
    Om_Vector state_symbols;
} Om_Emit_Actor_Decl;

typedef struct Om_Emit_Actor_Handler {
    uint32_t actor_index;
    Om_Symbol_Id message_symbol;
    uint32_t function_index;
} Om_Emit_Actor_Handler;

typedef struct Om_Emit_Loop_Jump {
    uint32_t instruction_index;
    bool is_continue;
} Om_Emit_Loop_Jump;

typedef struct Om_Emit_Loop {
    size_t jump_start;
    uint32_t continue_target;
} Om_Emit_Loop;

typedef struct Om_Emit_Context {
    const Om_Module_Project *project;
    const Om_Parser_Result *parsed_modules;
    size_t parsed_module_count;
    const Om_Resolution_Result *resolution;
    const Om_Type_Inference_Result *inference;
    const Om_Semantic_Analysis_Result *semantic;
    Om_Bytecode_Builder *builder;
    Om_Vector diagnostics;
    Om_Vector locals;
    Om_Vector actor_states;
    Om_Vector loop_jumps;
    Om_Emit_Loop loops[256];
    size_t loop_depth;
    uint32_t *function_by_symbol;
    uint32_t function_index;
    uint32_t next_local_slot;
    uint32_t parameter_count;
    Om_Module_Id module_id;
    const Om_Ast *ast;
    uint32_t actor_index;
    bool in_actor;
} Om_Emit_Context;

static const Om_Ast *emit_ast(const Om_Emit_Context *context,
                              Om_Module_Id module_id)
{
    if (module_id.value >= context->parsed_module_count) {
        return NULL;
    }
    return &context->parsed_modules[module_id.value].ast;
}

static const Om_Ast_Node *emit_node(const Om_Emit_Context *context,
                                    Om_Ast_Node_Id node_id)
{
    return om_ast_get_node(context->ast, node_id);
}

static Om_Ast_Node_Id emit_child_id(const Om_Emit_Context *context,
                                    const Om_Ast_Node *node,
                                    size_t index)
{
    return om_ast_get_child(context->ast, node, index);
}

static const Om_Ast_Node *emit_child(const Om_Emit_Context *context,
                                     const Om_Ast_Node *node,
                                     size_t index)
{
    return emit_node(context, emit_child_id(context, node, index));
}

static Om_Status add_diagnostic(Om_Emit_Context *context,
                                Om_Bytecode_Emit_Code code,
                                Om_Ast_Node_Id node_id)
{
    const Om_Ast_Node *node = emit_node(context, node_id);
    const Om_Bytecode_Emit_Diagnostic diagnostic = {
        .code = code,
        .module_id = context->module_id,
        .node_id = node_id,
        .span = node != NULL
            ? node->span
            : (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0}
    };
    return om_vector_push(&context->diagnostics, &diagnostic);
}

static Om_Status fail_node(Om_Emit_Context *context,
                           Om_Bytecode_Emit_Code code,
                           Om_Ast_Node_Id node_id)
{
    const Om_Status status = add_diagnostic(context, code, node_id);
    return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
}

static Om_Status emit_instruction(Om_Emit_Context *context,
                                  Om_Bytecode_Opcode opcode,
                                  uint32_t operand_a,
                                  uint32_t operand_b,
                                  int64_t immediate,
                                  Om_Source_Span span,
                                  uint32_t *pc)
{
    return om_bytecode_builder_emit(context->builder, opcode, operand_a,
                                    operand_b, immediate, span, pc);
}

static Om_Status emit_node_instruction(Om_Emit_Context *context,
                                      Om_Ast_Node_Id node_id,
                                      Om_Bytecode_Opcode opcode,
                                      uint32_t operand_a,
                                      uint32_t operand_b,
                                      int64_t immediate,
                                      uint32_t *pc)
{
    const Om_Ast_Node *node = emit_node(context, node_id);
    if (node == NULL) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    return emit_instruction(context, opcode, operand_a, operand_b, immediate,
                            node->span, pc);
}

static Om_Status add_constant(Om_Emit_Context *context,
                              Om_Bytecode_Constant constant,
                              uint32_t *constant_index)
{
    return om_bytecode_builder_add_constant(context->builder, &constant,
                                            constant_index);
}

static Om_Status add_string_constant(Om_Emit_Context *context,
                                     Om_String_View value,
                                     uint32_t *constant_index)
{
    const Om_Bytecode_Constant constant = {
        .kind = OM_BYTECODE_CONSTANT_STRING,
        .as.string_value = value
    };
    return add_constant(context, constant, constant_index);
}

static Om_Status emit_constant(Om_Emit_Context *context,
                               Om_Ast_Node_Id node_id,
                               Om_Bytecode_Constant constant)
{
    uint32_t constant_index = 0;
    Om_Status status = add_constant(context, constant, &constant_index);
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_CONST,
                                       constant_index, 0, 0, NULL);
    }
    return status;
}

static Om_Status append_utf8(uint32_t codepoint, char *buffer,
                             size_t capacity, size_t *length)
{
    uint8_t bytes[4] = {0};
    size_t width = 0;
    if (codepoint <= UINT32_C(0x7f)) {
        bytes[0] = (uint8_t)codepoint;
        width = 1;
    } else if (codepoint <= UINT32_C(0x7ff)) {
        bytes[0] = (uint8_t)(UINT32_C(0xc0) | (codepoint >> 6));
        bytes[1] = (uint8_t)(UINT32_C(0x80) | (codepoint & UINT32_C(0x3f)));
        width = 2;
    } else if (codepoint <= UINT32_C(0xffff)
               && !(codepoint >= UINT32_C(0xd800)
                    && codepoint <= UINT32_C(0xdfff))) {
        bytes[0] = (uint8_t)(UINT32_C(0xe0) | (codepoint >> 12));
        bytes[1] = (uint8_t)(UINT32_C(0x80)
                             | ((codepoint >> 6) & UINT32_C(0x3f)));
        bytes[2] = (uint8_t)(UINT32_C(0x80) | (codepoint & UINT32_C(0x3f)));
        width = 3;
    } else if (codepoint <= UINT32_C(0x10ffff)) {
        bytes[0] = (uint8_t)(UINT32_C(0xf0) | (codepoint >> 18));
        bytes[1] = (uint8_t)(UINT32_C(0x80)
                             | ((codepoint >> 12) & UINT32_C(0x3f)));
        bytes[2] = (uint8_t)(UINT32_C(0x80)
                             | ((codepoint >> 6) & UINT32_C(0x3f)));
        bytes[3] = (uint8_t)(UINT32_C(0x80) | (codepoint & UINT32_C(0x3f)));
        width = 4;
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (width > capacity - *length) {
        return OM_STATUS_OVERFLOW;
    }
    (void)memcpy(buffer + *length, bytes, width);
    *length += width;
    return OM_STATUS_OK;
}

static int hex_digit(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return (int)(byte - (uint8_t)'0');
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return 10 + (int)(byte - (uint8_t)'a');
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return 10 + (int)(byte - (uint8_t)'A');
    }
    return -1;
}

static Om_Status decode_string_text(Om_String_View raw,
                                    bool quoted,
                                    bool interpolation,
                                    char **decoded,
                                    size_t *decoded_length)
{
    if (decoded == NULL || decoded_length == NULL
        || (raw.data == NULL && raw.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *decoded = NULL;
    *decoded_length = 0;
    size_t begin = 0;
    size_t end = raw.length;
    if (quoted) {
        if (raw.length < 2 || raw.data[0] != '"'
            || raw.data[raw.length - 1] != '"') {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        begin = 1;
        end = raw.length - 1;
    }
    char *buffer = raw.length > 0 ? om_alloc(raw.length) : NULL;
    if (raw.length > 0 && buffer == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t length = 0;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = begin; index < end && status == OM_STATUS_OK;) {
        const uint8_t byte = (uint8_t)raw.data[index];
        if (byte == (uint8_t)'\\') {
            if (++index >= end) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            const uint8_t escape = (uint8_t)raw.data[index++];
            switch (escape) {
            case (uint8_t)'\\': buffer[length++] = '\\'; break;
            case (uint8_t)'"': buffer[length++] = '"'; break;
            case (uint8_t)'n': buffer[length++] = '\n'; break;
            case (uint8_t)'r': buffer[length++] = '\r'; break;
            case (uint8_t)'t': buffer[length++] = '\t'; break;
            case (uint8_t)'0': buffer[length++] = '\0'; break;
            case (uint8_t)'u': {
                if (index >= end || raw.data[index] != '{') {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                ++index;
                uint32_t scalar = 0;
                size_t digits = 0;
                while (index < end && raw.data[index] != '}') {
                    const int digit = hex_digit((uint8_t)raw.data[index]);
                    if (digit < 0 || digits == 6
                        || scalar > (UINT32_C(0x10ffff)
                                     - (uint32_t)digit) / 16) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                        break;
                    }
                    scalar = scalar * 16 + (uint32_t)digit;
                    ++digits;
                    ++index;
                }
                if (status != OM_STATUS_OK) {
                    break;
                }
                if (index >= end || raw.data[index] != '}' || digits == 0) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                ++index;
                status = append_utf8(scalar, buffer, raw.length, &length);
                break;
            }
            default:
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
        } else if (interpolation && byte == (uint8_t)'{'
                   && index + 1 < end && raw.data[index + 1] == '{') {
            buffer[length++] = '{';
            index += 2;
        } else if (interpolation && byte == (uint8_t)'}'
                   && index + 1 < end && raw.data[index + 1] == '}') {
            buffer[length++] = '}';
            index += 2;
        } else {
            buffer[length++] = (char)byte;
            ++index;
        }
    }
    if (status == OM_STATUS_OK
        && om_utf8_validate((Om_Const_Byte_Slice){
               (const uint8_t *)buffer, length}) != OM_STATUS_OK) {
        status = OM_STATUS_INVALID_UTF8;
    }
    if (status != OM_STATUS_OK) {
        om_free(buffer);
        return status;
    }
    *decoded = buffer;
    *decoded_length = length;
    return OM_STATUS_OK;
}

static Om_Symbol_Id symbol_for_node(const Om_Emit_Context *context,
                                    Om_Ast_Node_Id node_id)
{
    return om_resolution_symbol_for_node(context->resolution,
                                         context->module_id, node_id);
}

static Om_Status add_local(Om_Emit_Context *context, Om_Symbol_Id symbol,
                           uint32_t *slot)
{
    if (symbol.value == UINT32_MAX || slot == NULL
        || context->next_local_slot == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < context->locals.length; ++index) {
        const Om_Emit_Local *local = om_vector_at_const(&context->locals, index);
        if (local->symbol.value == symbol.value) {
            *slot = local->slot;
            return OM_STATUS_OK;
        }
    }
    const Om_Emit_Local local = {
        .symbol = symbol, .slot = context->next_local_slot++
    };
    const Om_Status status = om_vector_push(&context->locals, &local);
    if (status == OM_STATUS_OK) {
        *slot = local.slot;
    }
    return status;
}

static bool find_local(const Om_Emit_Context *context, Om_Symbol_Id symbol,
                       uint32_t *slot)
{
    for (size_t index = 0; index < context->locals.length; ++index) {
        const Om_Emit_Local *local = om_vector_at_const(&context->locals, index);
        if (local->symbol.value == symbol.value) {
            *slot = local->slot;
            return true;
        }
    }
    return false;
}

static bool find_actor_state(const Om_Emit_Context *context,
                             Om_Symbol_Id symbol, uint32_t *slot)
{
    for (size_t index = 0; index < context->actor_states.length; ++index) {
        const Om_Emit_Local *state =
            om_vector_at_const(&context->actor_states, index);
        if (state->symbol.value == symbol.value) {
            *slot = state->slot;
            return true;
        }
    }
    return false;
}

static Om_Status allocate_temp(Om_Emit_Context *context, uint32_t *slot)
{
    if (slot == NULL || context->next_local_slot == UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    *slot = context->next_local_slot++;
    return OM_STATUS_OK;
}

static Om_Type_Kind inferred_kind(const Om_Emit_Context *context,
                                  Om_Ast_Node_Id node_id)
{
    Om_Type_Id type = om_type_inference_type_for_node(
        context->inference, context->module_id, node_id);
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Type_View view = {0};
    if (type.value != UINT32_MAX
        && om_type_store_resolve(&context->inference->type_store,
                                type, &resolved) == OM_STATUS_OK
        && om_type_store_get(&context->inference->type_store,
                             resolved, &view) == OM_STATUS_OK) {
        return view.kind;
    }
    return OM_TYPE_VARIABLE;
}

static Om_Status emit_expression(Om_Emit_Context *context,
                                 Om_Ast_Node_Id node_id);
static Om_Status emit_statement(Om_Emit_Context *context,
                                Om_Ast_Node_Id node_id);
static Om_Status emit_block(Om_Emit_Context *context,
                            Om_Ast_Node_Id block_id);

static Om_Status emit_jump(Om_Emit_Context *context,
                           Om_Ast_Node_Id node_id,
                           Om_Bytecode_Opcode opcode,
                           uint32_t *jump_pc)
{
    return emit_node_instruction(context, node_id, opcode, 0, 0, 0, jump_pc);
}

static Om_Status patch_jump(Om_Emit_Context *context,
                            uint32_t jump_pc,
                            uint32_t target_pc)
{
    return om_bytecode_builder_patch_jump(context->builder, jump_pc, target_pc);
}

static Om_Status emit_short_circuit(Om_Emit_Context *context,
                                    Om_Ast_Node_Id node_id,
                                    const Om_Ast_Node *node,
                                    bool is_and)
{
    const Om_Ast_Node_Id left_id = emit_child_id(context, node, 0);
    const Om_Ast_Node_Id right_id = emit_child_id(context, node, 1);
    Om_Status status = emit_expression(context, left_id);
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_DUP, 0, 0, 0,
                                       NULL);
    }
    uint32_t branch_pc = 0;
    if (status == OM_STATUS_OK) {
        status = emit_jump(context, node_id,
                           is_and ? OM_BC_JUMP_IF_FALSE : OM_BC_JUMP_IF_TRUE,
                           &branch_pc);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_POP, 0, 0, 0,
                                       NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, right_id);
    }
    if (status == OM_STATUS_OK) {
        const size_t length = ((Om_Bytecode_Storage *)
            context->builder->owned_storage)->instructions.length;
        if (length > UINT32_MAX) {
            return OM_STATUS_OVERFLOW;
        }
        status = patch_jump(context, branch_pc, (uint32_t)length);
    }
    return status;
}

static Om_Status emit_binary(Om_Emit_Context *context,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node)
{
    if (node->child_count != 2) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    if (node->keyword == OM_KEYWORD_AND || node->keyword == OM_KEYWORD_OR) {
        return emit_short_circuit(context, node_id, node,
                                  node->keyword == OM_KEYWORD_AND);
    }
    Om_Status status = emit_expression(context, emit_child_id(context, node, 0));
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, emit_child_id(context, node, 1));
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Bytecode_Opcode opcode = OM_BC_NOP;
    const bool floating = inferred_kind(context, node_id) == OM_TYPE_FLOAT;
    switch (node->token_kind) {
    case OM_TOKEN_PLUS: opcode = floating ? OM_BC_ADD_FLOAT : OM_BC_ADD_INT; break;
    case OM_TOKEN_MINUS: opcode = floating ? OM_BC_SUB_FLOAT : OM_BC_SUB_INT; break;
    case OM_TOKEN_STAR: opcode = floating ? OM_BC_MUL_FLOAT : OM_BC_MUL_INT; break;
    case OM_TOKEN_SLASH: opcode = floating ? OM_BC_DIV_FLOAT : OM_BC_DIV_INT; break;
    case OM_TOKEN_PERCENT: opcode = OM_BC_MOD_INT; break;
    case OM_TOKEN_EQUAL: opcode = OM_BC_EQ; break;
    case OM_TOKEN_NOT_EQUAL: opcode = OM_BC_NE; break;
    case OM_TOKEN_LESS: opcode = OM_BC_LT; break;
    case OM_TOKEN_LESS_EQUAL: opcode = OM_BC_LE; break;
    case OM_TOKEN_GREATER: opcode = OM_BC_GT; break;
    case OM_TOKEN_GREATER_EQUAL: opcode = OM_BC_GE; break;
    default:
        return fail_node(context, OM_BYTECODE_EMIT_UNSUPPORTED_NODE, node_id);
    }
    return emit_node_instruction(context, node_id, opcode, 0, 0, 0, NULL);
}

static bool name_is(Om_String_View name, const char *literal, size_t length)
{
    return name.length == length && name.data != NULL
        && memcmp(name.data, literal, length) == 0;
}

static bool system_actor_operand(Om_String_View name, uint32_t *operand)
{
    if (name_is(name, OM_SYSTEM_ACTOR_NAME_CONSOLE,
                sizeof(OM_SYSTEM_ACTOR_NAME_CONSOLE) - 1)) {
        *operand = OM_BYTECODE_SYSTEM_ACTOR_CONSOLE;
        return true;
    }
    if (name_is(name, OM_SYSTEM_ACTOR_NAME_TIMER,
                sizeof(OM_SYSTEM_ACTOR_NAME_TIMER) - 1)) {
        *operand = OM_BYTECODE_SYSTEM_ACTOR_TIMER;
        return true;
    }
    if (name_is(name, OM_SYSTEM_ACTOR_NAME_FILE_SYSTEM,
                sizeof(OM_SYSTEM_ACTOR_NAME_FILE_SYSTEM) - 1)) {
        *operand = OM_BYTECODE_SYSTEM_ACTOR_FILE_SYSTEM;
        return true;
    }
    return false;
}

static Om_Symbol_Id system_message_schema_symbol(Om_String_View name)
{
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE,
                sizeof(OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_CONSOLE_WRITE};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED,
                sizeof(OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_FILE_READ,
                sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_FILE_READ};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED,
                sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_FILE_READ_COMPLETED};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_FILE_WRITE,
                sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_WRITE) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_FILE_WRITE};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED,
                sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED};
    }
    if (name_is(name, OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER,
                sizeof(OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER) - 1)) {
        return (Om_Symbol_Id){OM_SYSTEM_MESSAGE_TIMER_AFTER};
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Status emit_name(Om_Emit_Context *context, Om_Ast_Node_Id node_id,
                           const Om_Ast_Node *node)
{
    const Om_Symbol_Id symbol_id = symbol_for_node(context, node_id);
    const Om_Symbol *symbol =
        om_resolution_symbol_by_id(context->resolution, symbol_id);
    if (symbol == NULL) {
        return fail_node(context, OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL, node_id);
    }
    uint32_t slot = 0;
    if (find_local(context, symbol_id, &slot)) {
        return emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                     slot, 0, 0, NULL);
    }
    if (find_actor_state(context, symbol_id, &slot)) {
        return emit_node_instruction(context, node_id, OM_BC_LOAD_STATE,
                                     slot, 0, 0, NULL);
    }
    uint32_t system_actor = 0;
    if (symbol->kind == OM_SYMBOL_BUILTIN
        && system_actor_operand(symbol->name, &system_actor)) {
        return emit_node_instruction(context, node_id,
                                     OM_BC_LOAD_SYSTEM_ACTOR,
                                     system_actor, 0, 0, NULL);
    }
    if (symbol_id.value < context->resolution->symbol_count
        && context->function_by_symbol[symbol_id.value] != UINT32_MAX) {
        return emit_node_instruction(
            context, node_id, OM_BC_LOAD_FUNCTION,
            context->function_by_symbol[symbol_id.value], 0, 0, NULL);
    }
    (void)node;
    return fail_node(context, OM_BYTECODE_EMIT_UNSUPPORTED_NODE, node_id);
}

static Om_Status emit_call(Om_Emit_Context *context, Om_Ast_Node_Id node_id,
                           const Om_Ast_Node *node)
{
    if (node->child_count == 0) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    const Om_Ast_Node_Id callee_id = emit_child_id(context, node, 0);
    const Om_Ast_Node *callee = emit_node(context, callee_id);
    if (callee == NULL) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    const Om_Symbol_Id symbol_id = symbol_for_node(context, callee_id);
    const Om_Symbol *symbol =
        om_resolution_symbol_by_id(context->resolution, symbol_id);
    const size_t argument_count = node->child_count - 1;
    if (argument_count > UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const bool record_constructor = symbol != NULL
        && (symbol->kind == OM_SYMBOL_RECORD
            || symbol->kind == OM_SYMBOL_MESSAGE
            || name_is(symbol->name, "Actor_Down", 10)
            || system_message_schema_symbol(symbol->name).value
                != UINT32_MAX);
    const bool result_constructor = symbol != NULL
        && (name_is(symbol->name, "Ok", 2)
            || name_is(symbol->name, "Error", 5));
    const bool direct_function = symbol != NULL
        && symbol_id.value < context->resolution->symbol_count
        && context->function_by_symbol[symbol_id.value] != UINT32_MAX;
    Om_Status status = OM_STATUS_OK;
    if (record_constructor || result_constructor || direct_function) {
        for (size_t index = 1; index < node->child_count; ++index) {
            status = emit_expression(
                context, emit_child_id(context, node, index));
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    } else {
        status = emit_expression(context, callee_id);
        for (size_t index = 1; index < node->child_count
                               && status == OM_STATUS_OK; ++index) {
            status = emit_expression(
                context, emit_child_id(context, node, index));
        }
        if (status == OM_STATUS_OK) {
            return emit_node_instruction(context, node_id, OM_BC_CALL_INDIRECT,
                                         0, (uint32_t)argument_count, 0, NULL);
        }
        return status;
    }
    if (symbol != NULL && (symbol->kind == OM_SYMBOL_RECORD
                           || symbol->kind == OM_SYMBOL_MESSAGE)) {
        return emit_node_instruction(
            context, node_id,
            symbol->kind == OM_SYMBOL_RECORD
                ? OM_BC_MAKE_RECORD : OM_BC_MAKE_MESSAGE,
            symbol_id.value, (uint32_t)argument_count, 0, NULL);
    }
    if (symbol != NULL
        && system_message_schema_symbol(symbol->name).value != UINT32_MAX) {
        const Om_Symbol_Id schema_symbol =
            system_message_schema_symbol(symbol->name);
        return emit_node_instruction(context, node_id, OM_BC_MAKE_MESSAGE,
                                     schema_symbol.value,
                                     (uint32_t)argument_count, 0, NULL);
    }
    if (symbol != NULL && name_is(symbol->name, "Ok", 2)) {
        if (argument_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        return emit_node_instruction(context, node_id, OM_BC_MAKE_OK, 0, 0,
                                     0, NULL);
    }
    if (symbol != NULL && name_is(symbol->name, "Error", 5)) {
        if (argument_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        return emit_node_instruction(context, node_id, OM_BC_MAKE_ERROR, 0, 0,
                                     0, NULL);
    }
    if (symbol != NULL && name_is(symbol->name, "Actor_Down", 10)) {
        return emit_node_instruction(context, node_id, OM_BC_MAKE_MESSAGE,
                                     symbol_id.value,
                                     (uint32_t)argument_count, 0, NULL);
    }
    if (direct_function) {
        return emit_node_instruction(
            context, node_id, OM_BC_CALL,
            context->function_by_symbol[symbol_id.value],
            (uint32_t)argument_count, 0, NULL);
    }
    return fail_node(context, OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL, node_id);
}

static Om_Status emit_expression(Om_Emit_Context *context,
                                 Om_Ast_Node_Id node_id)
{
    const Om_Ast_Node *node = emit_node(context, node_id);
    if (node == NULL) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    switch (node->kind) {
    case OM_AST_NAME:
        return emit_name(context, node_id, node);
    case OM_AST_INTEGER_LITERAL: {
        int64_t value = 0;
        if ((node->flags & OM_AST_FLAG_MIN_INTEGER) != 0) {
            value = INT64_MIN;
        } else if (node->integer_magnitude <= INT64_MAX) {
            value = (int64_t)node->integer_magnitude;
        } else {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        return emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT,
            .as.integer_value = value
        });
    }
    case OM_AST_FLOAT_LITERAL:
        return emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_FLOAT,
            .as.float_value = node->float_value
        });
    case OM_AST_STRING_LITERAL: {
        char *decoded = NULL;
        size_t decoded_length = 0;
        Om_Status status = decode_string_text(node->name, true, false,
                                              &decoded, &decoded_length);
        if (status == OM_STATUS_OK) {
            status = emit_constant(context, node_id, (Om_Bytecode_Constant){
                .kind = OM_BYTECODE_CONSTANT_STRING,
                .as.string_value = {decoded, decoded_length}
            });
        }
        om_free(decoded);
        return status;
    }
    case OM_AST_STRING_TEXT: {
        char *decoded = NULL;
        size_t decoded_length = 0;
        Om_Status status = decode_string_text(node->name, false, true,
                                              &decoded, &decoded_length);
        if (status == OM_STATUS_OK) {
            status = emit_constant(context, node_id, (Om_Bytecode_Constant){
                .kind = OM_BYTECODE_CONSTANT_STRING,
                .as.string_value = {decoded, decoded_length}
            });
        }
        om_free(decoded);
        return status;
    }
    case OM_AST_BOOLEAN_LITERAL:
        return emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_BOOL,
            .as.boolean_value = node->keyword == OM_KEYWORD_TRUE
        });
    case OM_AST_NULL_LITERAL:
        return emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_NULL
        });
    case OM_AST_DOWN_REASON_LITERAL: {
        uint32_t value = UINT32_MAX;
        if (name_is(node->name, "NORMAL", 6)) {
            value = 0;
        } else if (name_is(node->name, "FAILED", 6)) {
            value = 1;
        } else if (name_is(node->name, "NODE_UNREACHABLE", 16)) {
            value = 2;
        }
        if (value == UINT32_MAX) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        return emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_DOWN_REASON,
            .as.down_reason_value = value
        });
    }
    case OM_AST_SELF:
        return emit_node_instruction(context, node_id, OM_BC_SELF, 0, 0, 0,
                                     NULL);
    case OM_AST_UNARY: {
        if (node->child_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        const Om_Ast_Node_Id operand_id = emit_child_id(context, node, 0);
        const Om_Ast_Node *operand = emit_node(context, operand_id);
        if (node->keyword == OM_KEYWORD_TRY) {
            Om_Status status = emit_expression(context, operand_id);
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id,
                                               OM_BC_CHECK_RESULT, 0, 0, 0,
                                               NULL);
            }
            uint32_t error_branch = 0;
            if (status == OM_STATUS_OK) {
                status = emit_jump(context, node_id, OM_BC_JUMP_IF_FALSE,
                                   &error_branch);
            }
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id,
                                               OM_BC_UNWRAP_OK, 0, 0, 0,
                                               NULL);
            }
            uint32_t end_jump = 0;
            if (status == OM_STATUS_OK) {
                status = emit_jump(context, node_id, OM_BC_JUMP, &end_jump);
            }
            if (status == OM_STATUS_OK) {
                const size_t length = ((Om_Bytecode_Storage *)
                    context->builder->owned_storage)->instructions.length;
                status = patch_jump(context, error_branch, (uint32_t)length);
            }
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id,
                                               OM_BC_RETURN_ERROR, 0, 0, 0,
                                               NULL);
            }
            if (status == OM_STATUS_OK) {
                const size_t length = ((Om_Bytecode_Storage *)
                    context->builder->owned_storage)->instructions.length;
                status = patch_jump(context, end_jump, (uint32_t)length);
            }
            return status;
        }
        if (node->token_kind == OM_TOKEN_MINUS && operand != NULL
            && operand->kind == OM_AST_INTEGER_LITERAL
            && (operand->flags & OM_AST_FLAG_MIN_INTEGER) != 0) {
            return emit_constant(context, operand_id, (Om_Bytecode_Constant){
                .kind = OM_BYTECODE_CONSTANT_INT,
                .as.integer_value = INT64_MIN
            });
        }
        Om_Status status = emit_expression(context, operand_id);
        if (status != OM_STATUS_OK) {
            return status;
        }
        Om_Bytecode_Opcode opcode = OM_BC_NOP;
        if (node->keyword == OM_KEYWORD_NOT) {
            opcode = OM_BC_NOT_BOOL;
        } else if (node->token_kind == OM_TOKEN_MINUS) {
            opcode = inferred_kind(context, node_id) == OM_TYPE_FLOAT
                ? OM_BC_NEG_FLOAT : OM_BC_NEG_INT;
        } else {
            return fail_node(context, OM_BYTECODE_EMIT_UNSUPPORTED_NODE,
                             node_id);
        }
        return emit_node_instruction(context, node_id, opcode, 0, 0, 0, NULL);
    }
    case OM_AST_BINARY:
        return emit_binary(context, node_id, node);
    case OM_AST_CALL:
        return emit_call(context, node_id, node);
    case OM_AST_MEMBER: {
        if (node->child_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        Om_Status status = emit_expression(
            context, emit_child_id(context, node, 0));
        uint32_t field_constant = 0;
        if (status == OM_STATUS_OK) {
            status = add_string_constant(context, node->name, &field_constant);
        }
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(context, node_id, OM_BC_GET_FIELD,
                                           field_constant, 0, 0, NULL);
        }
        return status;
    }
    case OM_AST_INDEX:
        if (node->child_count != 2) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        } else {
            Om_Status status = emit_expression(
                context, emit_child_id(context, node, 0));
            if (status == OM_STATUS_OK) {
                status = emit_expression(
                    context, emit_child_id(context, node, 1));
            }
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id,
                                               OM_BC_GET_INDEX, 0, 0, 0,
                                               NULL);
            }
            return status;
        }
    case OM_AST_WITH: {
        if (node->child_count == 0) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        Om_Status status = emit_expression(
            context, emit_child_id(context, node, 0));
        for (size_t index = 1; status == OM_STATUS_OK
                               && index < node->child_count; ++index) {
            const Om_Ast_Node_Id update_id = emit_child_id(context, node, index);
            const Om_Ast_Node *update = emit_node(context, update_id);
            if (update == NULL || update->kind != OM_AST_FIELD_UPDATE
                || update->child_count != 1) {
                return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT,
                                 update_id);
            }
            status = emit_expression(
                context, emit_child_id(context, update, 0));
            uint32_t field_constant = 0;
            if (status == OM_STATUS_OK) {
                status = add_string_constant(context, update->name,
                                             &field_constant);
            }
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, update_id,
                                               OM_BC_WITH_FIELD,
                                               field_constant, 0, 0, NULL);
            }
        }
        return status;
    }
    case OM_AST_SPAWN: {
        const Om_Symbol_Id actor_symbol = symbol_for_node(context, node_id);
        const Om_Symbol *actor =
            om_resolution_symbol_by_id(context->resolution, actor_symbol);
        if (actor == NULL || actor->kind != OM_SYMBOL_ACTOR) {
            return fail_node(context, OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL,
                             node_id);
        }
        if (node->child_count > 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        Om_Status status = OM_STATUS_OK;
        if (node->child_count == 1) {
            status = emit_expression(context, emit_child_id(context, node, 0));
        }
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(
                context, node_id, OM_BC_SPAWN, actor_symbol.value,
                (uint32_t)node->child_count, 0, NULL);
        }
        return status;
    }
    case OM_AST_GROUP:
        return node->child_count == 1
            ? emit_expression(context, emit_child_id(context, node, 0))
            : fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    case OM_AST_LIST:
        for (size_t index = 0; index < node->child_count; ++index) {
            const Om_Status status = emit_expression(
                context, emit_child_id(context, node, index));
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return emit_node_instruction(context, node_id, OM_BC_MAKE_LIST,
                                     (uint32_t)node->child_count, 0, 0, NULL);
    case OM_AST_MAP:
        for (size_t index = 0; index < node->child_count; ++index) {
            const Om_Ast_Node *entry =
                emit_child(context, node, index);
            if (entry == NULL || entry->kind != OM_AST_MAP_ENTRY
                || entry->child_count != 2) {
                return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT,
                                 emit_child_id(context, node, index));
            }
            Om_Status status = emit_expression(
                context, emit_child_id(context, entry, 0));
            if (status == OM_STATUS_OK) {
                status = emit_expression(
                    context, emit_child_id(context, entry, 1));
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return emit_node_instruction(context, node_id, OM_BC_MAKE_MAP,
                                     (uint32_t)node->child_count, 0, 0, NULL);
    case OM_AST_INTERPOLATED_STRING:
        for (size_t index = 0; index < node->child_count; ++index) {
            const Om_Ast_Node_Id part_id = emit_child_id(context, node, index);
            const Om_Ast_Node *part = emit_node(context, part_id);
            if (part == NULL) {
                return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT,
                                 part_id);
            }
            Om_Status status = emit_expression(context, part_id);
            if (status == OM_STATUS_OK && part->kind != OM_AST_STRING_TEXT) {
                status = emit_node_instruction(context, part_id, OM_BC_TO_STRING,
                                               0, 0, 0, NULL);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        return emit_node_instruction(context, node_id, OM_BC_BUILD_STRING,
                                     (uint32_t)node->child_count, 0, 0, NULL);
    default:
        return fail_node(context, OM_BYTECODE_EMIT_UNSUPPORTED_NODE, node_id);
    }
}

static Om_Status begin_loop(Om_Emit_Context *context,
                            uint32_t continue_target)
{
    if (context->loop_depth >= 256) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    context->loops[context->loop_depth++] = (Om_Emit_Loop){
        .jump_start = context->loop_jumps.length,
        .continue_target = continue_target
    };
    return OM_STATUS_OK;
}

static Om_Status emit_loop_jump(Om_Emit_Context *context,
                                Om_Ast_Node_Id node_id,
                                bool is_continue)
{
    if (context->loop_depth == 0) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    uint32_t jump_pc = 0;
    Om_Status status = emit_jump(context, node_id, OM_BC_JUMP, &jump_pc);
    if (status == OM_STATUS_OK) {
        const Om_Emit_Loop_Jump jump = {
            .instruction_index = jump_pc, .is_continue = is_continue
        };
        status = om_vector_push(&context->loop_jumps, &jump);
    }
    return status;
}

static Om_Status finish_loop(Om_Emit_Context *context, uint32_t end_pc)
{
    if (context->loop_depth == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Emit_Loop loop = context->loops[context->loop_depth - 1];
    Om_Status status = OM_STATUS_OK;
    for (size_t index = loop.jump_start;
         index < context->loop_jumps.length && status == OM_STATUS_OK; ++index) {
        const Om_Emit_Loop_Jump *jump =
            om_vector_at_const(&context->loop_jumps, index);
        status = patch_jump(context, jump->instruction_index,
                            jump->is_continue ? loop.continue_target : end_pc);
    }
    context->loop_jumps.length = loop.jump_start;
    --context->loop_depth;
    return status;
}

static Om_Status emit_if(Om_Emit_Context *context, Om_Ast_Node_Id node_id,
                         const Om_Ast_Node *node)
{
    const size_t arm_count = node->auxiliary;
    const bool has_else = (node->flags & OM_AST_FLAG_HAS_ELSE) != 0;
    if (node->child_count != arm_count + (has_else ? 1u : 0u)) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    Om_Vector end_jumps = {0};
    Om_Status status = om_vector_init(&end_jumps, sizeof(uint32_t));
    for (size_t index = 0; index < arm_count && status == OM_STATUS_OK; ++index) {
        const Om_Ast_Node_Id arm_id = emit_child_id(context, node, index);
        const Om_Ast_Node *arm = emit_node(context, arm_id);
        if (arm == NULL || arm->kind != OM_AST_IF_ARM || arm->child_count != 2) {
            status = fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, arm_id);
            break;
        }
        status = emit_expression(context, emit_child_id(context, arm, 0));
        uint32_t next_arm_jump = 0;
        if (status == OM_STATUS_OK) {
            status = emit_jump(context, arm_id, OM_BC_JUMP_IF_FALSE,
                               &next_arm_jump);
        }
        if (status == OM_STATUS_OK) {
            status = emit_block(context, emit_child_id(context, arm, 1));
        }
        uint32_t end_jump = 0;
        if (status == OM_STATUS_OK) {
            status = emit_jump(context, node_id, OM_BC_JUMP, &end_jump);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&end_jumps, &end_jump);
        }
        if (status == OM_STATUS_OK) {
            const size_t length = ((Om_Bytecode_Storage *)
                context->builder->owned_storage)->instructions.length;
            status = patch_jump(context, next_arm_jump, (uint32_t)length);
        }
    }
    if (status == OM_STATUS_OK && has_else) {
        status = emit_block(context, emit_child_id(context, node, arm_count));
    }
    if (status == OM_STATUS_OK) {
        const size_t length = ((Om_Bytecode_Storage *)
            context->builder->owned_storage)->instructions.length;
        for (size_t index = 0; index < end_jumps.length && status == OM_STATUS_OK;
             ++index) {
            const uint32_t *jump = om_vector_at_const(&end_jumps, index);
            status = patch_jump(context, *jump, (uint32_t)length);
        }
    }
    om_vector_destroy(&end_jumps);
    return status;
}

static Om_Status emit_while(Om_Emit_Context *context, Om_Ast_Node_Id node_id,
                            const Om_Ast_Node *node)
{
    if (node->child_count != 2) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    Om_Bytecode_Storage *storage = context->builder->owned_storage;
    if (storage->instructions.length > UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const uint32_t start_pc = (uint32_t)storage->instructions.length;
    Om_Status status = begin_loop(context, start_pc);
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, emit_child_id(context, node, 0));
    }
    uint32_t end_jump = 0;
    if (status == OM_STATUS_OK) {
        status = emit_jump(context, node_id, OM_BC_JUMP_IF_FALSE, &end_jump);
    }
    if (status == OM_STATUS_OK) {
        status = emit_block(context, emit_child_id(context, node, 1));
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_JUMP,
                                       start_pc, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        const size_t length = storage->instructions.length;
        status = patch_jump(context, end_jump, (uint32_t)length);
        if (status == OM_STATUS_OK) {
            status = finish_loop(context, (uint32_t)length);
        }
    } else if (context->loop_depth > 0) {
        (void)finish_loop(context, 0);
    }
    return status;
}

static Om_Status emit_numeric_for(Om_Emit_Context *context,
                                  Om_Ast_Node_Id node_id,
                                  const Om_Ast_Node *node)
{
    const size_t child_count = node->child_count;
    const bool has_step = child_count == 4;
    if (child_count != 3 && child_count != 4) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    const Om_Ast_Node_Id start_id = emit_child_id(context, node, 0);
    const Om_Ast_Node_Id end_id = emit_child_id(context, node, 1);
    const Om_Ast_Node_Id step_id = has_step
        ? emit_child_id(context, node, 2) : OM_AST_NODE_ID_INVALID;
    const Om_Ast_Node_Id body_id =
        emit_child_id(context, node, child_count - 1);
    const Om_Symbol_Id loop_symbol = symbol_for_node(context, node_id);
    uint32_t loop_slot = 0;
    Om_Status status = add_local(context, loop_symbol, &loop_slot);
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, start_id);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       loop_slot, 0, 0, NULL);
    }
    uint32_t end_slot = 0;
    uint32_t step_slot = 0;
    if (status == OM_STATUS_OK) {
        status = allocate_temp(context, &end_slot);
    }
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, end_id);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       end_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = allocate_temp(context, &step_slot);
    }
    if (status == OM_STATUS_OK && has_step) {
        status = emit_expression(context, step_id);
    } else if (status == OM_STATUS_OK) {
        status = emit_constant(context, node_id, (Om_Bytecode_Constant){
            .kind = OM_BYTECODE_CONSTANT_INT, .as.integer_value = 1
        });
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       step_slot, 0, 0, NULL);
    }
    Om_Bytecode_Storage *storage = context->builder->owned_storage;
    if (status == OM_STATUS_OK && storage->instructions.length > UINT32_MAX) {
        status = OM_STATUS_OVERFLOW;
    }
    const uint32_t start_pc = status == OM_STATUS_OK
        ? (uint32_t)storage->instructions.length : 0;
    if (status == OM_STATUS_OK) {
        status = begin_loop(context, 0);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                       loop_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                       end_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                       step_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_FOR_RANGE_TEST,
                                       0, 0, 0, NULL);
    }
    uint32_t end_jump = 0;
    if (status == OM_STATUS_OK) {
        status = emit_jump(context, node_id, OM_BC_JUMP_IF_FALSE, &end_jump);
    }
    if (status == OM_STATUS_OK) {
        status = emit_block(context, body_id);
    }
    if (status == OM_STATUS_OK && storage->instructions.length > UINT32_MAX) {
        status = OM_STATUS_OVERFLOW;
    }
    const uint32_t continue_pc = status == OM_STATUS_OK
        ? (uint32_t)storage->instructions.length : 0;
    if (status == OM_STATUS_OK) {
        context->loops[context->loop_depth - 1].continue_target = continue_pc;
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                       loop_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_LOAD_LOCAL,
                                       step_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_ADD_INT,
                                       0, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       loop_slot, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_JUMP,
                                       start_pc, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        const size_t length = storage->instructions.length;
        status = patch_jump(context, end_jump, (uint32_t)length);
        if (status == OM_STATUS_OK) {
            status = finish_loop(context, (uint32_t)length);
        }
    } else if (context->loop_depth > 0) {
        (void)finish_loop(context, 0);
    }
    return status;
}

static Om_Status emit_each_for(Om_Emit_Context *context,
                               Om_Ast_Node_Id node_id,
                               const Om_Ast_Node *node)
{
    if (node->child_count != 2) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    const Om_Symbol_Id loop_symbol = symbol_for_node(context, node_id);
    uint32_t loop_slot = 0;
    uint32_t iterator_slot = 0;
    Om_Status status = add_local(context, loop_symbol, &loop_slot);
    if (status == OM_STATUS_OK) {
        status = allocate_temp(context, &iterator_slot);
    }
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, emit_child_id(context, node, 0));
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_ITER_INIT,
                                       0, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       iterator_slot, 0, 0, NULL);
    }
    Om_Bytecode_Storage *storage = context->builder->owned_storage;
    if (status == OM_STATUS_OK && storage->instructions.length > UINT32_MAX) {
        status = OM_STATUS_OVERFLOW;
    }
    const uint32_t start_pc = status == OM_STATUS_OK
        ? (uint32_t)storage->instructions.length : 0;
    if (status == OM_STATUS_OK) {
        status = begin_loop(context, start_pc);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_ITER_NEXT,
                                       iterator_slot, loop_slot, 0, NULL);
    }
    uint32_t end_jump = 0;
    if (status == OM_STATUS_OK) {
        status = emit_jump(context, node_id, OM_BC_JUMP_IF_FALSE, &end_jump);
    }
    if (status == OM_STATUS_OK) {
        status = emit_block(context, emit_child_id(context, node, 1));
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_JUMP,
                                       start_pc, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        const size_t length = storage->instructions.length;
        status = patch_jump(context, end_jump, (uint32_t)length);
        if (status == OM_STATUS_OK) {
            status = finish_loop(context, (uint32_t)length);
        }
    } else if (context->loop_depth > 0) {
        (void)finish_loop(context, 0);
    }
    return status;
}

static bool is_result_pattern(Om_Emit_Context *context,
                              const Om_Ast_Node *pattern,
                              bool *is_ok,
                              Om_Ast_Node_Id *binding_id)
{
    if (pattern == NULL || pattern->kind != OM_AST_CALL
        || pattern->child_count != 2) {
        return false;
    }
    const Om_Ast_Node_Id constructor_id = emit_child_id(context, pattern, 0);
    const Om_Ast_Node *constructor = emit_node(context, constructor_id);
    const Om_Ast_Node_Id argument_id = emit_child_id(context, pattern, 1);
    const Om_Ast_Node *argument = emit_node(context, argument_id);
    if (constructor == NULL || constructor->kind != OM_AST_NAME
        || argument == NULL || argument->kind != OM_AST_NAME) {
        return false;
    }
    if (name_is(constructor->name, "Ok", 2)) {
        *is_ok = true;
    } else if (name_is(constructor->name, "Error", 5)) {
        *is_ok = false;
    } else {
        return false;
    }
    *binding_id = argument_id;
    return true;
}

static Om_Status emit_select(Om_Emit_Context *context,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node)
{
    if (node->child_count == 0) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    uint32_t scrutinee_slot = 0;
    Om_Status status = allocate_temp(context, &scrutinee_slot);
    if (status == OM_STATUS_OK) {
        status = emit_expression(context, emit_child_id(context, node, 0));
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                       scrutinee_slot, 0, 0, NULL);
    }
    Om_Vector end_jumps = {0};
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&end_jumps, sizeof(uint32_t));
    }
    for (size_t index = 1; index < node->child_count && status == OM_STATUS_OK;
         ++index) {
        const Om_Ast_Node_Id arm_id = emit_child_id(context, node, index);
        const Om_Ast_Node *arm = emit_node(context, arm_id);
        if (arm == NULL || arm->kind != OM_AST_CASE_ARM) {
            status = fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, arm_id);
            break;
        }
        uint32_t next_arm_jump = UINT32_MAX;
        if ((arm->flags & OM_AST_FLAG_CASE_ELSE) == 0) {
            if (arm->child_count != 2) {
                status = fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT,
                                   arm_id);
                break;
            }
            const Om_Ast_Node_Id pattern_id = emit_child_id(context, arm, 0);
            const Om_Ast_Node *pattern = emit_node(context, pattern_id);
            bool is_ok = false;
            Om_Ast_Node_Id binding_id = OM_AST_NODE_ID_INVALID;
            if (is_result_pattern(context, pattern, &is_ok, &binding_id)) {
                status = emit_node_instruction(context, pattern_id,
                                               OM_BC_LOAD_LOCAL,
                                               scrutinee_slot, 0, 0, NULL);
                if (status == OM_STATUS_OK) {
                    status = emit_node_instruction(context, pattern_id,
                                                   OM_BC_IS_OK, 0, 0, 0,
                                                   NULL);
                }
                if (status == OM_STATUS_OK && !is_ok) {
                    status = emit_node_instruction(context, pattern_id,
                                                   OM_BC_NOT_BOOL, 0, 0, 0,
                                                   NULL);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_jump(context, pattern_id,
                                       OM_BC_JUMP_IF_FALSE, &next_arm_jump);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_node_instruction(context, pattern_id,
                                                   OM_BC_LOAD_LOCAL,
                                                   scrutinee_slot, 0, 0, NULL);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_node_instruction(
                        context, pattern_id,
                        is_ok ? OM_BC_UNWRAP_OK : OM_BC_UNWRAP_ERROR,
                        0, 0, 0, NULL);
                }
                const Om_Symbol_Id binding_symbol =
                    symbol_for_node(context, binding_id);
                uint32_t binding_slot = 0;
                if (status == OM_STATUS_OK) {
                    status = add_local(context, binding_symbol, &binding_slot);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_node_instruction(context, binding_id,
                                                   OM_BC_STORE_LOCAL,
                                                   binding_slot, 0, 0, NULL);
                }
            } else {
                status = emit_node_instruction(context, pattern_id,
                                               OM_BC_LOAD_LOCAL,
                                               scrutinee_slot, 0, 0, NULL);
                if (status == OM_STATUS_OK) {
                    status = emit_expression(context, pattern_id);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_node_instruction(context, arm_id, OM_BC_EQ,
                                                   0, 0, 0, NULL);
                }
                if (status == OM_STATUS_OK) {
                    status = emit_jump(context, arm_id,
                                       OM_BC_JUMP_IF_FALSE, &next_arm_jump);
                }
            }
            if (status == OM_STATUS_OK) {
                status = emit_block(context, emit_child_id(context, arm, 1));
            }
        } else {
            if (arm->child_count != 1) {
                status = fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT,
                                   arm_id);
            } else {
                status = emit_block(context, emit_child_id(context, arm, 0));
            }
        }
        uint32_t end_jump = 0;
        if (status == OM_STATUS_OK) {
            status = emit_jump(context, node_id, OM_BC_JUMP, &end_jump);
        }
        if (status == OM_STATUS_OK) {
            status = om_vector_push(&end_jumps, &end_jump);
        }
        if (status == OM_STATUS_OK && next_arm_jump != UINT32_MAX) {
            const size_t length = ((Om_Bytecode_Storage *)
                context->builder->owned_storage)->instructions.length;
            status = patch_jump(context, next_arm_jump, (uint32_t)length);
        }
    }
    if (status == OM_STATUS_OK) {
        const size_t length = ((Om_Bytecode_Storage *)
            context->builder->owned_storage)->instructions.length;
        for (size_t index = 0; index < end_jumps.length && status == OM_STATUS_OK;
             ++index) {
            const uint32_t *jump = om_vector_at_const(&end_jumps, index);
            status = patch_jump(context, *jump, (uint32_t)length);
        }
    }
    om_vector_destroy(&end_jumps);
    return status;
}

static Om_Status emit_binding(Om_Emit_Context *context,
                              Om_Ast_Node_Id node_id,
                              const Om_Ast_Node *node)
{
    if (node->child_count != 1) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    Om_Status status = emit_expression(context, emit_child_id(context, node, 0));
    const Om_Symbol_Id symbol = symbol_for_node(context, node_id);
    uint32_t slot = 0;
    if (status == OM_STATUS_OK && find_actor_state(context, symbol, &slot)) {
        status = emit_node_instruction(context, node_id, OM_BC_STORE_STATE,
                                       slot, 0, 0, NULL);
    } else if (status == OM_STATUS_OK) {
        status = add_local(context, symbol, &slot);
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(context, node_id, OM_BC_STORE_LOCAL,
                                           slot, 0, 0, NULL);
        }
    }
    return status;
}

static Om_Status emit_statement(Om_Emit_Context *context,
                                Om_Ast_Node_Id node_id)
{
    const Om_Ast_Node *node = emit_node(context, node_id);
    if (node == NULL) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    }
    switch (node->kind) {
    case OM_AST_BINDING:
        return emit_binding(context, node_id, node);
    case OM_AST_RETURN: {
        Om_Status status = OM_STATUS_OK;
        if (node->child_count > 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        if (node->child_count == 1) {
            status = emit_expression(context, emit_child_id(context, node, 0));
        } else {
            status = emit_node_instruction(context, node_id, OM_BC_LOAD_UNIT,
                                           0, 0, 0, NULL);
        }
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(context, node_id, OM_BC_RETURN,
                                           0, 0, 0, NULL);
        }
        return status;
    }
    case OM_AST_BREAK:
        return emit_loop_jump(context, node_id, false);
    case OM_AST_CONTINUE:
        return emit_loop_jump(context, node_id, true);
    case OM_AST_CALL_STATEMENT: {
        if (node->child_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        }
        Om_Status status = emit_expression(context, emit_child_id(context, node, 0));
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(context, node_id, OM_BC_POP,
                                           0, 0, 0, NULL);
        }
        return status;
    }
    case OM_AST_IF:
        return emit_if(context, node_id, node);
    case OM_AST_WHILE:
        return emit_while(context, node_id, node);
    case OM_AST_FOR:
        if ((node->flags & OM_AST_FLAG_FOR_NUMERIC) != 0) {
            return emit_numeric_for(context, node_id, node);
        }
        if ((node->flags & OM_AST_FLAG_FOR_EACH) != 0) {
            return emit_each_for(context, node_id, node);
        }
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
    case OM_AST_SELECT:
        return emit_select(context, node_id, node);
    case OM_AST_SEND:
        if (node->child_count != 2) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        } else {
            Om_Status status = emit_expression(
                context, emit_child_id(context, node, 0));
            if (status == OM_STATUS_OK) {
                status = emit_expression(
                    context, emit_child_id(context, node, 1));
            }
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id, OM_BC_SEND,
                                               0, 0, 0, NULL);
            }
            return status;
        }
    case OM_AST_WATCH:
        if (node->child_count != 1) {
            return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, node_id);
        } else {
            Om_Status status = emit_expression(
                context, emit_child_id(context, node, 0));
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(context, node_id, OM_BC_WATCH,
                                               0, 0, 0, NULL);
            }
            return status;
        }
    case OM_AST_STOP: {
        Om_Status status = emit_node_instruction(context, node_id, OM_BC_SELF,
                                                 0, 0, 0, NULL);
        if (status == OM_STATUS_OK) {
            status = emit_node_instruction(context, node_id, OM_BC_STOP,
                                           0, 0, 0, NULL);
        }
        return status;
    }
    default:
        return fail_node(context, OM_BYTECODE_EMIT_UNSUPPORTED_NODE, node_id);
    }
}

static Om_Status emit_block(Om_Emit_Context *context,
                            Om_Ast_Node_Id block_id)
{
    const Om_Ast_Node *block = emit_node(context, block_id);
    if (block == NULL || block->kind != OM_AST_BLOCK) {
        return fail_node(context, OM_BYTECODE_EMIT_INVALID_INPUT, block_id);
    }
    for (size_t index = 0; index < block->child_count; ++index) {
        const Om_Status status = emit_statement(
            context, emit_child_id(context, block, index));
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status append_name(Om_String_View prefix, const char *suffix,
                             Om_String_View middle, char **owned,
                             Om_String_View *combined)
{
    size_t length = 0;
    if (prefix.length > SIZE_MAX - middle.length
        || prefix.length + middle.length > SIZE_MAX - strlen(suffix) - 2) {
        return OM_STATUS_OVERFLOW;
    }
    length = prefix.length + 2 + middle.length + strlen(suffix);
    char *text = om_alloc(length);
    if (text == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t offset = 0;
    if (prefix.length > 0) {
        (void)memcpy(text + offset, prefix.data, prefix.length);
        offset += prefix.length;
    }
    (void)memcpy(text + offset, "::", 2);
    offset += 2;
    if (middle.length > 0) {
        (void)memcpy(text + offset, middle.data, middle.length);
        offset += middle.length;
    }
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > 0) {
        (void)memcpy(text + offset, suffix, suffix_length);
    }
    *owned = text;
    *combined = (Om_String_View){text, length};
    return OM_STATUS_OK;
}

static Om_Status add_named_function(
    Om_Emit_Context *context,
    Om_String_View name,
    uint32_t parameter_count,
    uint32_t flags,
    bool external,
    uint32_t *function_index
)
{
    uint32_t name_constant = 0;
    Om_Status status = add_string_constant(context, name, &name_constant);
    if (status == OM_STATUS_OK) {
        status = external
            ? om_bytecode_builder_add_external_function(
                  context->builder, name_constant, parameter_count, flags,
                  function_index)
            : om_bytecode_builder_reserve_function(
                  context->builder, name_constant, parameter_count, flags,
                  function_index);
    }
    return status;
}

static Om_Status fingerprint_type(
    const Om_Emit_Context *context,
    Om_Type_Id type_id,
    size_t depth,
    uint64_t *fingerprint
)
{
    if (context == NULL || fingerprint == NULL || depth >= 256) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(
        &context->inference->type_store, type_id, &resolved_type);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(&context->inference->type_store,
                                   resolved_type, &view);
    }
    if (status != OM_STATUS_OK || view.kind == OM_TYPE_VARIABLE
        || view.kind == OM_TYPE_FUNCTION || view.kind == OM_TYPE_ACTION) {
        return status == OM_STATUS_OK
            ? OM_STATUS_INVALID_ARGUMENT : status;
    }

    const char *kind_name = om_type_kind_name(view.kind);
    if (kind_name == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t kind_name_length = strlen(kind_name);
    const Om_String_View kind_view = {kind_name, kind_name_length};
    Om_String_View module_path = {0};
    Om_String_View entity_name = kind_view;
    Om_String_View domain = {"OMBASIC-TYPE-V1", 15};
    if (view.kind == OM_TYPE_RECORD || view.kind == OM_TYPE_MESSAGE
        || view.kind == OM_TYPE_ACTOR_PROTOCOL) {
        const Om_Symbol *nominal = om_resolution_symbol_by_id(
            context->resolution, view.nominal_symbol);
        if (nominal == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        module_path = om_module_graph_path(&context->project->graph,
                                           nominal->module_id);
        if (module_path.data == NULL || module_path.length == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        entity_name = nominal->name;
        domain = kind_view;
    }
    uint64_t *child_fingerprints = NULL;
    if (view.child_count != 0) {
        child_fingerprints = om_alloc_array(view.child_count,
                                             sizeof(*child_fingerprints));
        if (child_fingerprints == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < view.child_count; ++index) {
        Om_Type_Id child_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(&context->inference->type_store,
                                        resolved_type, index, &child_type);
        if (status == OM_STATUS_OK) {
            status = fingerprint_type(context, child_type, depth + 1,
                                      &child_fingerprints[index]);
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_fingerprint64_schema(
            domain, module_path, entity_name, child_fingerprints,
            view.child_count, fingerprint);
    }
    om_free(child_fingerprints);
    return status;
}

static Om_Status compute_schema_fingerprint(
    const Om_Emit_Context *context,
    const Om_Symbol *symbol,
    const Om_Ast_Node *declaration,
    uint64_t *fingerprint
)
{
    if (context == NULL || symbol == NULL || declaration == NULL
        || fingerprint == NULL || declaration->child_count > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    uint64_t *field_type_fingerprints = NULL;
    if (declaration->child_count != 0) {
        field_type_fingerprints = om_alloc_array(
            declaration->child_count, sizeof(*field_type_fingerprints));
        if (field_type_fingerprints == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < declaration->child_count; ++index) {
        const Om_Ast_Node_Id field_id = emit_child_id(context, declaration,
                                                     index);
        const Om_Type_Id type_id = om_type_inference_type_for_node(
            context->inference, context->module_id, field_id);
        status = fingerprint_type(context, type_id, 0,
                                  &field_type_fingerprints[index]);
    }
    const Om_String_View module_path = om_module_graph_path(
        &context->project->graph, context->module_id);
    if (status == OM_STATUS_OK) {
        status = om_fingerprint64_schema(
            (Om_String_View){"OMBASIC-SCHEMA-V1", 17}, module_path,
            symbol->name, field_type_fingerprints, declaration->child_count,
            fingerprint);
    }
    om_free(field_type_fingerprints);
    return status;
}

static Om_Status compute_builtin_schema_fingerprint(
    Om_String_View schema_name,
    const char *const *field_type_names,
    size_t field_count,
    uint64_t *fingerprint
)
{
    static const char module_path[] = "__ombasic__";
    static const char type_domain[] = "OMBASIC-TYPE-V1";
    static const char schema_domain[] = "OMBASIC-SCHEMA-V1";
    if (schema_name.data == NULL || schema_name.length == 0
        || fingerprint == NULL
        || (field_count != 0 && field_type_names == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint64_t field_fingerprints[2] = {0};
    if (field_count > sizeof(field_fingerprints)
                            / sizeof(field_fingerprints[0])) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    for (size_t index = 0; index < field_count; ++index) {
        const char *type_name = field_type_names[index];
        if (type_name == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Status status = om_fingerprint64_schema(
            (Om_String_View){type_domain, sizeof(type_domain) - 1},
            (Om_String_View){NULL, 0},
            (Om_String_View){type_name, strlen(type_name)}, NULL, 0,
            &field_fingerprints[index]);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return om_fingerprint64_schema(
        (Om_String_View){schema_domain, sizeof(schema_domain) - 1},
        (Om_String_View){module_path, sizeof(module_path) - 1}, schema_name,
        field_fingerprints, field_count, fingerprint);
}

static Om_Status register_schema(
    Om_Emit_Context *context,
    Om_Symbol_Id symbol_id,
    Om_Bytecode_Schema_Kind kind,
    const Om_Ast_Node *declaration
)
{
    if (declaration == NULL || declaration->child_count > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t *field_names = om_alloc_array(declaration->child_count,
                                           sizeof(*field_names));
    if (declaration->child_count > 0 && field_names == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; index < declaration->child_count
                           && status == OM_STATUS_OK; ++index) {
        const Om_Ast_Node *field = emit_child(context, declaration, index);
        if (field == NULL || field->kind != OM_AST_PARAMETER) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = add_string_constant(context, field->name, &field_names[index]);
    }
    uint32_t schema_index = 0;
    uint64_t fingerprint = 0;
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->resolution, symbol_id);
    if (status == OM_STATUS_OK && symbol == NULL) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        status = compute_schema_fingerprint(context, symbol, declaration,
                                            &fingerprint);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_add_schema(
            context->builder, symbol_id, kind, field_names,
            declaration->child_count, fingerprint, &schema_index);
    }
    om_free(field_names);
    return status;
}

static uint32_t external_function_arity(const Om_Emit_Context *context,
                                        const Om_Symbol *symbol,
                                        uint32_t *flags)
{
    *flags = 0;
    if (name_is(symbol->name, "Ok", 2)
        || name_is(symbol->name, "Error", 5)) {
        return 1;
    }
    if (name_is(symbol->name, "Actor_Down", 10)) {
        return 2;
    }
    const Om_Type_Scheme *scheme = om_type_inference_scheme_for_symbol(
        context->inference, symbol->id);
    if (scheme != NULL && scheme->type_id.value != UINT32_MAX) {
        Om_Type_Id type = OM_TYPE_ID_INVALID;
        Om_Type_View view = {0};
        if (om_type_store_resolve(&context->inference->type_store,
                                  scheme->type_id, &type) == OM_STATUS_OK
            && om_type_store_get(&context->inference->type_store,
                                 type, &view) == OM_STATUS_OK
            && (view.kind == OM_TYPE_FUNCTION || view.kind == OM_TYPE_ACTION)
            && view.child_count > 0
            && view.child_count - 1 <= UINT32_MAX) {
            return (uint32_t)(view.child_count - 1);
        }
    }
    *flags |= OM_BYTECODE_FUNCTION_VARIADIC;
    return UINT32_MAX;
}

static Om_Status append_function_decl(Om_Vector *declarations,
                                      Om_Emit_Function_Decl declaration)
{
    return om_vector_push(declarations, &declaration);
}

static Om_Status register_project_functions(
    Om_Emit_Context *context,
    Om_Vector *declarations,
    Om_Vector *actors,
    Om_Vector *handlers
)
{
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &context->resolution->symbols[index];
        if (symbol->kind != OM_SYMBOL_BUILTIN) {
            continue;
        }
        uint32_t ignored_actor_operand = 0;
        if (system_actor_operand(symbol->name, &ignored_actor_operand)
            || system_message_schema_symbol(symbol->name).value
                != UINT32_MAX) {
            continue;
        }
        uint32_t flags = 0;
        const uint32_t arity = external_function_arity(context, symbol, &flags);
        uint32_t function_index = 0;
        Om_Status status = add_named_function(
            context, symbol->name, arity, flags, true, &function_index);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (symbol->id.value >= context->resolution->symbol_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        context->function_by_symbol[symbol->id.value] = function_index;
        if (name_is(symbol->name, "Actor_Down", 10)) {
            uint32_t field_constants[2] = {0, 0};
            Om_Status schema_status = add_string_constant(
                context, (Om_String_View){"actor_id", 8},
                &field_constants[0]);
            if (schema_status == OM_STATUS_OK) {
                schema_status = add_string_constant(
                    context, (Om_String_View){"reason", 6},
                    &field_constants[1]);
            }
            uint32_t schema_index = 0;
            if (schema_status == OM_STATUS_OK) {
                static const char *const actor_down_types[] = {
                    "ActorId", "DownReason"
                };
                uint64_t schema_fingerprint = 0;
                schema_status = compute_builtin_schema_fingerprint(
                    symbol->name, actor_down_types,
                    sizeof(actor_down_types) / sizeof(actor_down_types[0]),
                    &schema_fingerprint);
                if (schema_status == OM_STATUS_OK) {
                    schema_status = om_bytecode_builder_add_schema(
                        context->builder, symbol->id,
                        OM_BYTECODE_SCHEMA_MESSAGE, field_constants, 2,
                        schema_fingerprint, &schema_index);
                }
            }
            if (schema_status != OM_STATUS_OK) {
                return schema_status;
            }
        }
    }
    for (size_t module_index = 0; module_index < context->parsed_module_count;
         ++module_index) {
        context->module_id = (Om_Module_Id){(uint32_t)module_index};
        context->ast = emit_ast(context, context->module_id);
        const Om_Ast_Node *root = om_ast_get_node(context->ast,
            context->ast != NULL ? context->ast->root : OM_AST_NODE_ID_INVALID);
        if (root == NULL || root->kind != OM_AST_PROGRAM) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t child_index = 0; child_index < root->child_count;
             ++child_index) {
            const Om_Ast_Node_Id declaration_id =
                emit_child_id(context, root, child_index);
            const Om_Ast_Node *declaration = emit_node(context, declaration_id);
            if (declaration == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            if (declaration->kind == OM_AST_FUNCTION_DECLARATION
                || declaration->kind == OM_AST_ACTION_DECLARATION) {
                const Om_Symbol_Id symbol_id =
                    symbol_for_node(context, declaration_id);
                const Om_Symbol *symbol =
                    om_resolution_symbol_by_id(context->resolution, symbol_id);
                if (symbol == NULL
                    || declaration->auxiliary > declaration->child_count) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                uint32_t function_index = 0;
                const uint32_t flags =
                    declaration->kind == OM_AST_ACTION_DECLARATION
                        ? OM_BYTECODE_FUNCTION_ACTION
                        : OM_BYTECODE_FUNCTION_NONE;
                Om_Status status = add_named_function(
                    context, symbol->name, declaration->auxiliary, flags,
                    false, &function_index);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                context->function_by_symbol[symbol_id.value] = function_index;
                status = append_function_decl(declarations,
                    (Om_Emit_Function_Decl){
                        .module_id = context->module_id,
                        .node_id = declaration_id,
                        .symbol_id = symbol_id,
                        .function_index = function_index,
                        .actor_index = UINT32_MAX,
                        .message_symbol = OM_SYMBOL_ID_INVALID,
                        .flags = flags
                    });
                if (status != OM_STATUS_OK) {
                    return status;
                }
                continue;
            }
            if (declaration->kind == OM_AST_RECORD_DECLARATION
                || declaration->kind == OM_AST_MESSAGE_DECLARATION) {
                const Om_Symbol_Id symbol_id =
                    symbol_for_node(context, declaration_id);
                if (om_resolution_symbol_by_id(
                        context->resolution, symbol_id) == NULL) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                const Om_Status status = register_schema(
                    context, symbol_id,
                    declaration->kind == OM_AST_RECORD_DECLARATION
                        ? OM_BYTECODE_SCHEMA_RECORD
                        : OM_BYTECODE_SCHEMA_MESSAGE,
                    declaration);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                continue;
            }
            if (declaration->kind != OM_AST_ACTOR_DECLARATION) {
                continue;
            }
            const Om_Symbol_Id actor_symbol =
                symbol_for_node(context, declaration_id);
            const Om_Symbol *actor =
                om_resolution_symbol_by_id(context->resolution, actor_symbol);
            if (actor == NULL || actor->kind != OM_SYMBOL_ACTOR) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            Om_Emit_Actor_Decl actor_decl = {
                .module_id = context->module_id,
                .node_id = declaration_id,
                .symbol_id = actor_symbol,
                .initializer_function = UINT32_MAX,
                .state_count = 0
            };
            Om_Status status = om_vector_init(&actor_decl.state_symbols,
                                              sizeof(Om_Emit_Local));
            for (size_t member_index = 0;
                 status == OM_STATUS_OK && member_index < declaration->child_count;
                 ++member_index) {
                const Om_Ast_Node_Id member_id =
                    emit_child_id(context, declaration, member_index);
                const Om_Ast_Node *member = emit_node(context, member_id);
                if (member == NULL) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
                if (member->kind == OM_AST_BINDING) {
                    const Om_Symbol_Id state_symbol =
                        symbol_for_node(context, member_id);
                    if (state_symbol.value == UINT32_MAX
                        || actor_decl.state_count == UINT32_MAX) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                        break;
                    }
                    const Om_Emit_Local state = {
                        .symbol = state_symbol,
                        .slot = actor_decl.state_count++
                    };
                    status = om_vector_push(&actor_decl.state_symbols, &state);
                }
            }
            char *owned_name = NULL;
            Om_String_View initializer_name = {0};
            if (status == OM_STATUS_OK) {
                status = append_name(actor->name, "init", (Om_String_View){0},
                                     &owned_name, &initializer_name);
            }
            if (status == OM_STATUS_OK) {
                status = add_named_function(context, initializer_name, 0,
                                            OM_BYTECODE_FUNCTION_ACTION, false,
                                            &actor_decl.initializer_function);
            }
            om_free(owned_name);
            if (status == OM_STATUS_OK) {
                status = om_vector_push(actors, &actor_decl);
            }
            if (status != OM_STATUS_OK) {
                om_vector_destroy(&actor_decl.state_symbols);
                return status;
            }
            const uint32_t actor_index = (uint32_t)(actors->length - 1);
            for (size_t member_index = 0;
                 member_index < declaration->child_count; ++member_index) {
                const Om_Ast_Node_Id member_id =
                    emit_child_id(context, declaration, member_index);
                const Om_Ast_Node *member = emit_node(context, member_id);
                if (member == NULL
                    || member->kind != OM_AST_RECEIVE_DECLARATION) {
                    continue;
                }
                const Om_Symbol_Id message_symbol =
                    symbol_for_node(context, member_id);
                const Om_Symbol *message = om_resolution_symbol_by_id(
                    context->resolution, message_symbol);
                if (message == NULL) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                char *owned_handler_name = NULL;
                Om_String_View handler_name = {0};
                status = append_name(actor->name, "::receive",
                                     message->name, &owned_handler_name,
                                     &handler_name);
                uint32_t handler_function = 0;
                if (status == OM_STATUS_OK) {
                    status = add_named_function(
                        context, handler_name, member->auxiliary,
                        OM_BYTECODE_FUNCTION_RECEIVE, false,
                        &handler_function);
                }
                om_free(owned_handler_name);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                status = append_function_decl(declarations,
                    (Om_Emit_Function_Decl){
                        .module_id = context->module_id,
                        .node_id = member_id,
                        .symbol_id = OM_SYMBOL_ID_INVALID,
                        .function_index = handler_function,
                        .actor_index = actor_index,
                        .message_symbol = message_symbol,
                        .flags = OM_BYTECODE_FUNCTION_RECEIVE
                    });
                if (status == OM_STATUS_OK) {
                    status = om_vector_push(handlers,
                        &(Om_Emit_Actor_Handler){
                            .actor_index = actor_index,
                            .message_symbol = message_symbol,
                            .function_index = handler_function
                        });
                }
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
            status = append_function_decl(declarations,
                (Om_Emit_Function_Decl){
                    .module_id = context->module_id,
                    .node_id = declaration_id,
                    .symbol_id = actor_symbol,
                    .function_index = actor_decl.initializer_function,
                    .actor_index = actor_index,
                    .message_symbol = OM_SYMBOL_ID_INVALID,
                    .flags = OM_BYTECODE_FUNCTION_ACTION
                });
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

static Om_Status init_function_context(Om_Emit_Context *context,
                                       const Om_Emit_Function_Decl *decl,
                                       const Om_Emit_Actor_Decl *actor)
{
    Om_Status status = om_vector_init(&context->locals, sizeof(Om_Emit_Local));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context->actor_states, sizeof(Om_Emit_Local));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context->loop_jumps, sizeof(Om_Emit_Loop_Jump));
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    context->module_id = decl->module_id;
    context->ast = emit_ast(context, decl->module_id);
    context->function_index = decl->function_index;
    context->actor_index = decl->actor_index;
    context->in_actor = actor != NULL;
    context->next_local_slot = 0;
    context->parameter_count = 0;
    if (actor != NULL) {
        for (size_t index = 0; index < actor->state_symbols.length; ++index) {
            const Om_Emit_Local *state =
                om_vector_at_const(&actor->state_symbols, index);
            status = om_vector_push(&context->actor_states, state);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return status;
}

static void destroy_function_context(Om_Emit_Context *context)
{
    om_vector_destroy(&context->locals);
    om_vector_destroy(&context->actor_states);
    om_vector_destroy(&context->loop_jumps);
    context->loop_depth = 0;
}

static Om_Status bind_parameters(Om_Emit_Context *context,
                                 const Om_Ast_Node *declaration,
                                 uint32_t parameter_count)
{
    if (declaration == NULL || declaration->child_count < parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0; index < parameter_count; ++index) {
        const Om_Ast_Node_Id parameter_id =
            emit_child_id(context, declaration, index);
        const Om_Ast_Node *parameter = emit_node(context, parameter_id);
        if (parameter == NULL || parameter->kind != OM_AST_PARAMETER) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Symbol_Id symbol = symbol_for_node(context, parameter_id);
        uint32_t slot = 0;
        const Om_Status status = add_local(context, symbol, &slot);
        if (status != OM_STATUS_OK || slot != index) {
            return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
        }
    }
    context->parameter_count = parameter_count;
    return OM_STATUS_OK;
}

static Om_Status emit_function_declaration(
    Om_Emit_Context *base,
    const Om_Emit_Function_Decl *decl,
    const Om_Emit_Actor_Decl *actors
)
{
    const Om_Emit_Actor_Decl *actor = decl->actor_index != UINT32_MAX
        ? &actors[decl->actor_index] : NULL;
    Om_Emit_Context context = *base;
    context.locals = (Om_Vector){0};
    context.actor_states = (Om_Vector){0};
    context.loop_jumps = (Om_Vector){0};
    context.loop_depth = 0;
    Om_Status status = init_function_context(&context, decl, actor);
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_begin_function_body(
            context.builder, decl->function_index);
    }
    const Om_Ast_Node *declaration = emit_node(&context, decl->node_id);
    const bool is_initializer = actor != NULL
        && decl->node_id.value == actor->node_id.value
        && decl->symbol_id.value == actor->symbol_id.value;
    if (status == OM_STATUS_OK && is_initializer) {
        for (size_t state_index = 0;
             state_index < actor->state_symbols.length && status == OM_STATUS_OK;
             ++state_index) {
            const Om_Ast_Node_Id state_id =
                emit_child_id(&context, declaration, state_index);
            const Om_Ast_Node *state = emit_node(&context, state_id);
            if (state == NULL || state->kind != OM_AST_BINDING
                || state->child_count != 1) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            status = emit_expression(
                &context, emit_child_id(&context, state, 0));
            if (status == OM_STATUS_OK) {
                status = emit_node_instruction(
                    &context, state_id, OM_BC_STORE_STATE,
                    (uint32_t)state_index, 0, 0, NULL);
            }
        }
    } else if (status == OM_STATUS_OK) {
        const uint32_t parameter_count = declaration != NULL
            ? declaration->auxiliary : 0;
        status = bind_parameters(&context, declaration, parameter_count);
        if (status == OM_STATUS_OK) {
            const Om_Ast_Node_Id body_id = emit_child_id(
                &context, declaration, parameter_count);
            status = emit_block(&context, body_id);
        }
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(
            &context, decl->node_id, OM_BC_LOAD_UNIT, 0, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = emit_node_instruction(
            &context, decl->node_id, OM_BC_RETURN, 0, 0, 0, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_set_function_layout(
            context.builder, decl->function_index, context.parameter_count,
            context.next_local_slot);
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_end_function(
            context.builder, decl->function_index);
    }
    destroy_function_context(&context);
    return status;
}

void om_bytecode_emit_result_destroy(Om_Bytecode_Emit_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_bytecode_image_destroy(&result->image);
    om_free(result->owned_diagnostics);
    *result = (Om_Bytecode_Emit_Result){0};
}

static Om_Status compute_program_fingerprint(
    const Om_Module_Project *project,
    size_t module_count,
    uint64_t *fingerprint
)
{
    static const char module_domain[] = "OMBASIC-MODULE-V1";
    static const char program_domain[] = "OMBASIC-PROGRAM-V1";
    if (project == NULL || fingerprint == NULL || module_count == 0
        || module_count > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    uint64_t *module_fingerprints = om_alloc_array(
        module_count, sizeof(*module_fingerprints));
    if (module_fingerprints == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < module_count;
         ++index) {
        const Om_Module_Id module_id = {(uint32_t)index};
        const Om_Source_Text *source = om_module_project_source(
            project, module_id);
        const Om_String_View path = om_module_graph_path(
            &project->graph, module_id);
        if (source == NULL || path.data == NULL || path.length == 0) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = om_fingerprint64_schema(
            (Om_String_View){module_domain, sizeof(module_domain) - 1}, path,
            (Om_String_View){"Source", 6}, &source->content_hash, 1,
            &module_fingerprints[index]);
    }
    if (status == OM_STATUS_OK) {
        status = om_fingerprint64_schema(
            (Om_String_View){program_domain, sizeof(program_domain) - 1},
            (Om_String_View){NULL, 0}, (Om_String_View){"Program", 7},
            module_fingerprints, module_count, fingerprint);
    }
    om_free(module_fingerprints);
    return status;
}

static Om_Status compute_interface_fingerprint(
    const Om_Bytecode_Image *image,
    uint64_t *fingerprint
)
{
    static const char interface_domain[] = "OMBASIC-INTERFACE-V1";
    if (image == NULL || fingerprint == NULL
        || (image->schema_count != 0 && image->schemas == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *fingerprint = 0;
    uint64_t *schema_fingerprints = NULL;
    if (image->schema_count != 0) {
        schema_fingerprints = om_alloc_array(
            image->schema_count, sizeof(*schema_fingerprints));
        if (schema_fingerprints == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t index = 0; index < image->schema_count; ++index) {
            schema_fingerprints[index] = image->schemas[index].fingerprint;
        }
    }
    const Om_Status status = om_fingerprint64_schema(
        (Om_String_View){interface_domain, sizeof(interface_domain) - 1},
        (Om_String_View){NULL, 0}, (Om_String_View){"Interface", 9},
        schema_fingerprints, image->schema_count, fingerprint);
    om_free(schema_fingerprints);
    return status;
}

Om_Status om_bytecode_emit_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Bytecode_Emit_Result *result
)
{
    if (project == NULL || parsed_modules == NULL || resolution == NULL
        || inference == NULL || semantic == NULL || result == NULL
        || result->image.owned_storage != NULL || result->owned_diagnostics != NULL
        || parsed_module_count != om_module_project_module_count(project)
        || parsed_module_count == 0 || resolution->diagnostic_count != 0
        || inference->diagnostic_count != 0 || semantic->diagnostic_count != 0
        || (resolution->symbol_count > 0 && resolution->symbols == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Emit_Context context = {
        .project = project,
        .parsed_modules = parsed_modules,
        .parsed_module_count = parsed_module_count,
        .resolution = resolution,
        .inference = inference,
        .semantic = semantic,
        .function_index = UINT32_MAX,
        .actor_index = UINT32_MAX
    };
    Om_Bytecode_Builder builder = {0};
    Om_Vector declarations = {0};
    Om_Vector actors = {0};
    Om_Vector handlers = {0};
    uint64_t program_fingerprint = 0;
    Om_Status status = om_bytecode_builder_init(&builder);
    if (status == OM_STATUS_OK) {
        status = compute_program_fingerprint(project, parsed_module_count,
                                             &program_fingerprint);
    }
    if (status == OM_STATUS_OK) {
        context.builder = &builder;
        status = om_bytecode_builder_add_system_actor_schemas(&builder);
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.diagnostics,
                                sizeof(Om_Bytecode_Emit_Diagnostic));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&declarations, sizeof(Om_Emit_Function_Decl));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&actors, sizeof(Om_Emit_Actor_Decl));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&handlers, sizeof(Om_Emit_Actor_Handler));
    }
    if (status == OM_STATUS_OK && resolution->symbol_count > 0) {
        context.function_by_symbol = om_alloc_array(
            resolution->symbol_count, sizeof(*context.function_by_symbol));
        if (context.function_by_symbol == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            for (size_t index = 0; index < resolution->symbol_count; ++index) {
                context.function_by_symbol[index] = UINT32_MAX;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = register_project_functions(
            &context, &declarations, &actors, &handlers);
    }
    const Om_Emit_Actor_Decl *actor_data = actors.data;
    for (size_t index = 0; index < declarations.length
         && status == OM_STATUS_OK; ++index) {
        const Om_Emit_Function_Decl *declaration =
            om_vector_at_const(&declarations, index);
        status = emit_function_declaration(&context, declaration, actor_data);
    }
    for (size_t actor_index = 0; actor_index < actors.length
         && status == OM_STATUS_OK; ++actor_index) {
        const Om_Emit_Actor_Decl *actor = om_vector_at_const(&actors, actor_index);
        uint32_t bytecode_actor_index = 0;
        status = om_bytecode_builder_add_actor(
            &builder, actor->symbol_id, actor->initializer_function,
            actor->state_count, &bytecode_actor_index);
        for (size_t handler_index = 0;
             handler_index < handlers.length && status == OM_STATUS_OK;
             ++handler_index) {
            const Om_Emit_Actor_Handler *handler =
                om_vector_at_const(&handlers, handler_index);
            if (handler->actor_index == actor_index) {
                Om_Symbol_Id message_symbol = handler->message_symbol;
                const Om_Symbol *message = om_resolution_symbol_by_id(
                    resolution, handler->message_symbol);
                if (message != NULL
                    && message->kind == OM_SYMBOL_BUILTIN) {
                    const Om_Symbol_Id system_schema =
                        system_message_schema_symbol(message->name);
                    if (system_schema.value != UINT32_MAX) {
                        message_symbol = system_schema;
                    }
                }
                status = om_bytecode_builder_add_actor_handler(
                    &builder, bytecode_actor_index, message_symbol,
                    handler->function_index);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_bytecode_builder_finish(
            &builder, &result->image, NULL);
    }
    if (status == OM_STATUS_OK) {
        status = compute_interface_fingerprint(
            &result->image, &result->image.interface_fingerprint);
        result->image.program_fingerprint = program_fingerprint;
    }
    if (context.diagnostics.data != NULL) {
        result->diagnostics = context.diagnostics.data;
        result->diagnostic_count = context.diagnostics.length;
        result->owned_diagnostics = context.diagnostics.data;
        context.diagnostics.data = NULL;
        context.diagnostics.length = 0;
        context.diagnostics.capacity = 0;
        context.diagnostics.item_size = 0;
    }
    for (size_t index = 0; index < actors.length; ++index) {
        Om_Emit_Actor_Decl *actor = om_vector_at(&actors, index);
        om_vector_destroy(&actor->state_symbols);
    }
    om_vector_destroy(&context.diagnostics);
    om_vector_destroy(&declarations);
    om_vector_destroy(&actors);
    om_vector_destroy(&handlers);
    om_free(context.function_by_symbol);
    om_bytecode_builder_destroy(&builder);
    if (status != OM_STATUS_OK) {
        om_bytecode_image_destroy(&result->image);
    }
    return status;
}

