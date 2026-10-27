#include "ombasic/semantic/type_inference.h"

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/system_actors.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/syntax/name_rules.h"

typedef enum Om_Infer_Symbol_State {
    OM_INFER_SYMBOL_UNINITIALIZED = 0,
    OM_INFER_SYMBOL_READY,
    OM_INFER_SYMBOL_IN_PROGRESS,
    OM_INFER_SYMBOL_DONE
} Om_Infer_Symbol_State;

typedef struct Om_Empty_Collection {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Type_Id type_id;
} Om_Empty_Collection;

typedef struct Om_Infer_Context {
    const Om_Module_Project *project;
    const Om_Parser_Result *parsed_modules;
    const Om_Resolution_Result *resolution;
    size_t module_count;
    Om_Type_Store type_store;
    Om_Type_Scheme *symbol_schemes;
    Om_Infer_Symbol_State *symbol_states;
    bool *recursive_callables;
    Om_Vector callable_stack;
    Om_Vector inferred_nodes;
    Om_Vector diagnostics;
    Om_Vector empty_collections;
    uint32_t level;
    Om_Type_Id return_type;
    Om_Symbol_Id current_actor;
    bool in_actor_receive;
} Om_Infer_Context;

static Om_Status infer_expression(Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id node_id,
                                  uint32_t level,
                                  Om_Type_Id *type_id);
static Om_Status infer_statement(Om_Infer_Context *context,
                                 Om_Module_Id module,
                                 Om_Ast_Node_Id statement_id,
                                 uint32_t level);
static Om_Status infer_callable(Om_Infer_Context *context,
                                Om_Symbol_Id symbol_id);
static Om_Status infer_actor(Om_Infer_Context *context,
                             Om_Module_Id module,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node,
                             uint32_t level);
static const Om_Ast_Node *infer_node(const Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id);
static Om_Ast_Node_Id infer_child(const Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  const Om_Ast_Node *node,
                                  size_t index);
static const Om_Symbol *infer_symbol(const Om_Infer_Context *context,
                                     Om_Symbol_Id symbol_id);
static Om_Symbol_Id resolved_symbol_for_node(const Om_Infer_Context *context,
                                             Om_Module_Id module,
                                             Om_Ast_Node_Id node);
static Om_Type_Id type_for_symbol_scheme(const Om_Infer_Context *context,
                                        Om_Symbol_Id symbol_id);
static Om_Status set_symbol_monotype(Om_Infer_Context *context,
                                     Om_Symbol_Id symbol_id,
                                     Om_Type_Id type_id);
static Om_Status generalize_symbol(Om_Infer_Context *context,
                                   Om_Symbol_Id symbol_id,
                                   Om_Type_Id type_id,
                                   uint32_t environment_level);
static Om_Status set_node_type(Om_Infer_Context *context,
                               Om_Module_Id module,
                               Om_Ast_Node_Id node,
                               Om_Type_Id type);
static Om_Status unify_at(Om_Infer_Context *context,
                          Om_Module_Id module,
                          Om_Source_Span span,
                          Om_Type_Id expected,
                          Om_Type_Id actual);
static Om_Status add_type_diagnostic(Om_Infer_Context *context,
                                     Om_Type_Inference_Diagnostic_Code code,
                                     Om_Module_Id module,
                                     Om_Source_Span span,
                                     Om_Type_Id expected,
                                     Om_Type_Id actual);

static Om_Status set_unit_node_type(Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id)
{
    const Om_Type_Id unit = context->type_store.primitive_types[OM_TYPE_UNIT];
    return set_node_type(context, module, node_id, unit);
}

static bool block_guarantees_return(const Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id block_id);

static bool statement_guarantees_return(const Om_Infer_Context *context,
                                        Om_Module_Id module,
                                        Om_Ast_Node_Id statement_id)
{
    const Om_Ast_Node *node = infer_node(context, module, statement_id);
    if (node == NULL) {
        return false;
    }
    if (node->kind == OM_AST_RETURN) {
        return true;
    }
    if (node->kind == OM_AST_IF) {
        const size_t arm_count = node->auxiliary;
        const bool has_else = (node->flags & OM_AST_FLAG_HAS_ELSE) != 0;
        if (arm_count == 0 || !has_else
            || node->child_count != arm_count + 1) {
            return false;
        }
        for (size_t index = 0; index < arm_count; ++index) {
            const Om_Ast_Node *arm = infer_node(
                context, module, infer_child(context, module, node, index));
            if (arm == NULL || arm->kind != OM_AST_IF_ARM
                || arm->child_count != 2
                || !block_guarantees_return(
                    context, module, infer_child(context, module, arm, 1))) {
                return false;
            }
        }
        return block_guarantees_return(
            context, module, infer_child(context, module, node, arm_count));
    }
    if (node->kind == OM_AST_SELECT && node->child_count > 1) {
        bool has_else = false;
        for (size_t index = 1; index < node->child_count; ++index) {
            const Om_Ast_Node *arm = infer_node(
                context, module, infer_child(context, module, node, index));
            if (arm == NULL || arm->kind != OM_AST_CASE_ARM) {
                return false;
            }
            const bool is_else = (arm->flags & OM_AST_FLAG_CASE_ELSE) != 0;
            has_else = has_else || is_else;
            const size_t body_index = is_else ? 0u : 1u;
            if (arm->child_count != body_index + 1
                || !block_guarantees_return(
                    context, module, infer_child(context, module, arm,
                                                  body_index))) {
                return false;
            }
        }
        return has_else;
    }
    return false;
}

static bool block_guarantees_return(const Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id block_id)
{
    const Om_Ast_Node *block = infer_node(context, module, block_id);
    if (block == NULL || block->kind != OM_AST_BLOCK) {
        return false;
    }
    for (size_t index = 0; index < block->child_count; ++index) {
        if (statement_guarantees_return(
                context, module, infer_child(context, module, block, index))) {
            return true;
        }
    }
    return false;
}

static Om_Status infer_block(Om_Infer_Context *context,
                             Om_Module_Id module,
                             Om_Ast_Node_Id block_id,
                             uint32_t level)
{
    const Om_Ast_Node *block = infer_node(context, module, block_id);
    if (block == NULL || block->kind != OM_AST_BLOCK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < block->child_count; ++index) {
        status = infer_statement(context, module,
                                 infer_child(context, module, block, index),
                                 level);
    }
    if (status == OM_STATUS_OK) {
        status = set_unit_node_type(context, module, block_id);
    }
    return status;
}

static Om_Status infer_binding(Om_Infer_Context *context,
                               Om_Module_Id module,
                               Om_Ast_Node_Id node_id,
                               const Om_Ast_Node *node,
                               uint32_t level)
{
    if (node->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id value_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module,
                                        infer_child(context, module, node, 0),
                                        level, &value_type);
    const Om_Symbol_Id symbol_id = resolved_symbol_for_node(context, module,
                                                            node_id);
    const Om_Symbol *symbol = infer_symbol(context, symbol_id);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (symbol == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Infer_Symbol_State state = context->symbol_states[symbol_id.value];
    if (state == OM_INFER_SYMBOL_UNINITIALIZED) {
        if (symbol->kind == OM_SYMBOL_MUTABLE_BINDING) {
            status = set_symbol_monotype(context, symbol_id, value_type);
        } else {
            status = generalize_symbol(context, symbol_id, value_type, level);
        }
    } else {
        Om_Type_Id current_type = type_for_symbol_scheme(context, symbol_id);
        status = unify_at(context, module, node->span, current_type, value_type);
    }
    if (status == OM_STATUS_OK) {
        status = set_node_type(context, module, node_id, value_type);
    }
    return status;
}

static Om_Status infer_condition(Om_Infer_Context *context,
                                 Om_Module_Id module,
                                 Om_Ast_Node_Id expression_id,
                                 uint32_t level)
{
    Om_Type_Id condition_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module, expression_id,
                                         level, &condition_type);
    if (status == OM_STATUS_OK) {
        const Om_Ast_Node *condition = infer_node(context, module, expression_id);
        status = unify_at(context, module,
                          condition != NULL ? condition->span
                                            : (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0},
                          context->type_store.primitive_types[OM_TYPE_BOOL],
                          condition_type);
    }
    return status;
}

static Om_Status infer_if(Om_Infer_Context *context,
                          Om_Module_Id module,
                          Om_Ast_Node_Id node_id,
                          const Om_Ast_Node *node,
                          uint32_t level)
{
    const size_t arm_count = node->auxiliary;
    const bool has_else = (node->flags & OM_AST_FLAG_HAS_ELSE) != 0;
    if (node->child_count != arm_count + (has_else ? 1u : 0u)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < arm_count; ++index) {
        const Om_Ast_Node_Id arm_id = infer_child(context, module, node, index);
        const Om_Ast_Node *arm = infer_node(context, module, arm_id);
        if (arm == NULL || arm->kind != OM_AST_IF_ARM || arm->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = infer_condition(context, module,
                                 infer_child(context, module, arm, 0), level);
        if (status == OM_STATUS_OK) {
            status = infer_block(context, module,
                                 infer_child(context, module, arm, 1), level);
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, arm_id);
        }
    }
    if (status == OM_STATUS_OK && has_else) {
        status = infer_block(context, module,
                             infer_child(context, module, node, arm_count), level);
    }
    if (status == OM_STATUS_OK) {
        status = set_unit_node_type(context, module, node_id);
    }
    return status;
}

static Om_Status infer_for(Om_Infer_Context *context,
                           Om_Module_Id module,
                           Om_Ast_Node_Id node_id,
                           const Om_Ast_Node *node,
                           uint32_t level)
{
    if (node->child_count < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t body_index = node->child_count - 1;
    Om_Status status = OM_STATUS_OK;
    Om_Type_Id variable_type = OM_TYPE_ID_INVALID;
    if ((node->flags & OM_AST_FLAG_FOR_NUMERIC) != 0) {
        const size_t header_count = body_index;
        if (header_count < 2 || header_count > 3) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        variable_type = context->type_store.primitive_types[OM_TYPE_INT];
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < header_count; ++index) {
            Om_Type_Id value_type = OM_TYPE_ID_INVALID;
            const Om_Ast_Node_Id value_id = infer_child(context, module,
                                                        node, index);
            status = infer_expression(context, module, value_id, level,
                                      &value_type);
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node *value = infer_node(context, module, value_id);
                status = unify_at(context, module,
                                  value != NULL ? value->span : node->span,
                                  variable_type, value_type);
            }
        }
    } else if ((node->flags & OM_AST_FLAG_FOR_EACH) != 0) {
        if (body_index != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id iterable_type = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &iterable_type);
        Om_Type_View iterable_view = {0};
        const Om_Type_Id original_iterable_type = iterable_type;
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(&context->type_store, iterable_type,
                                       &iterable_view);
        }
        if (status == OM_STATUS_OK && iterable_view.kind == OM_TYPE_LIST) {
            status = om_type_store_child_at(&context->type_store,
                                            iterable_type, 0, &variable_type);
        } else if (status == OM_STATUS_OK && iterable_view.kind == OM_TYPE_MAP) {
            status = om_type_store_child_at(&context->type_store,
                                            iterable_type, 0, &variable_type);
        } else if (status == OM_STATUS_OK
                   && iterable_view.kind == OM_TYPE_VARIABLE) {
            Om_Type_Id element_type = OM_TYPE_ID_INVALID;
            status = om_type_store_variable(&context->type_store, level,
                                            &element_type);
            if (status == OM_STATUS_OK) {
                status = om_type_store_list(&context->type_store,
                                            element_type, &iterable_type);
            }
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  original_iterable_type, iterable_type);
            }
            variable_type = element_type;
        } else if (status == OM_STATUS_OK) {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, node->span,
                                         OM_TYPE_ID_INVALID, iterable_type);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &variable_type);
            }
        }
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        const Om_Symbol_Id loop_symbol = resolved_symbol_for_node(context,
                                                                  module,
                                                                  node_id);
        status = set_symbol_monotype(context, loop_symbol, variable_type);
    }
    if (status == OM_STATUS_OK) {
        status = infer_block(context, module,
                             infer_child(context, module, node, body_index),
                             level);
    }
    if (status == OM_STATUS_OK) {
        status = set_unit_node_type(context, module, node_id);
    }
    return status;
}
static Om_Status infer_actor(Om_Infer_Context *context,
                             Om_Module_Id module,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node,
                             uint32_t level);

static const Om_Ast *infer_module_ast(const Om_Infer_Context *context,
                                     Om_Module_Id module)
{
    if (module.value >= context->module_count) {
        return NULL;
    }
    return &context->parsed_modules[module.value].ast;
}

static const Om_Ast_Node *infer_node(const Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id)
{
    return om_ast_get_node(infer_module_ast(context, module), node_id);
}

static Om_Ast_Node_Id infer_child(const Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  const Om_Ast_Node *node,
                                  size_t index)
{
    return om_ast_get_child(infer_module_ast(context, module), node, index);
}

static const Om_Symbol *infer_symbol(const Om_Infer_Context *context,
                                     Om_Symbol_Id symbol_id)
{
    if (symbol_id.value >= context->resolution->symbol_count) {
        return NULL;
    }
    return &context->resolution->symbols[symbol_id.value];
}

static Om_Type_Scheme *scheme_for(Om_Infer_Context *context,
                                  Om_Symbol_Id symbol_id)
{
    if (symbol_id.value >= context->resolution->symbol_count) {
        return NULL;
    }
    return &context->symbol_schemes[symbol_id.value];
}

static Om_Status set_symbol_monotype(Om_Infer_Context *context,
                                     Om_Symbol_Id symbol_id,
                                     Om_Type_Id type_id)
{
    Om_Type_Scheme *scheme = scheme_for(context, symbol_id);
    if (scheme == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    om_type_scheme_destroy(scheme);
    scheme->type_id = type_id;
    context->symbol_states[symbol_id.value] = OM_INFER_SYMBOL_READY;
    return OM_STATUS_OK;
}

static Om_Status generalize_symbol(Om_Infer_Context *context,
                                   Om_Symbol_Id symbol_id,
                                   Om_Type_Id type_id,
                                   uint32_t environment_level)
{
    Om_Type_Scheme *scheme = scheme_for(context, symbol_id);
    if (scheme == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    om_type_scheme_destroy(scheme);
    const Om_Status status = om_type_store_generalize(
        &context->type_store, type_id, environment_level, scheme);
    if (status == OM_STATUS_OK) {
        context->symbol_states[symbol_id.value] = OM_INFER_SYMBOL_DONE;
    }
    return status;
}

static Om_Status set_node_type(Om_Infer_Context *context,
                               Om_Module_Id module,
                               Om_Ast_Node_Id node,
                               Om_Type_Id type)
{
    for (size_t index = 0; index < context->inferred_nodes.length; ++index) {
        Om_Inferred_Node *entry = om_vector_at(&context->inferred_nodes, index);
        if (entry->module_id.value == module.value
            && entry->node_id.value == node.value) {
            entry->type_id = type;
            return OM_STATUS_OK;
        }
    }
    const Om_Inferred_Node entry = {module, node, type};
    return om_vector_push(&context->inferred_nodes, &entry);
}

static bool has_diagnostic_at(const Om_Infer_Context *context,
                              Om_Type_Inference_Diagnostic_Code code,
                              Om_Module_Id module,
                              Om_Source_Span span)
{
    for (size_t index = 0; index < context->diagnostics.length; ++index) {
        const Om_Type_Inference_Diagnostic *diagnostic =
            om_vector_at_const(&context->diagnostics, index);
        if (diagnostic->code == code
            && diagnostic->module_id.value == module.value
            && diagnostic->span.source.value == span.source.value
            && diagnostic->span.start == span.start
            && diagnostic->span.end == span.end) {
            return true;
        }
    }
    return false;
}

static Om_Status add_type_diagnostic(Om_Infer_Context *context,
                                     Om_Type_Inference_Diagnostic_Code code,
                                     Om_Module_Id module,
                                     Om_Source_Span span,
                                     Om_Type_Id expected,
                                     Om_Type_Id actual)
{
    if (has_diagnostic_at(context, code, module, span)) {
        return OM_STATUS_OK;
    }
    const Om_Type_Inference_Diagnostic diagnostic = {
        .code = code,
        .module_id = module,
        .span = span,
        .expected_type = expected,
        .actual_type = actual,
    };
    return om_vector_push(&context->diagnostics, &diagnostic);
}

static Om_Status unify_at(Om_Infer_Context *context,
                          Om_Module_Id module,
                          Om_Source_Span span,
                          Om_Type_Id expected,
                          Om_Type_Id actual)
{
    bool unified = false;
    Om_Status status = om_type_store_unify(&context->type_store,
                                           expected, actual, &unified);
    if (status == OM_STATUS_OK && !unified) {
        status = add_type_diagnostic(
            context, OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
            module, span, expected, actual);
    }
    return status;
}

static Om_Status get_primitive_type(Om_Infer_Context *context,
                                    Om_Type_Kind kind,
                                    Om_Type_Id *type)
{
    return om_type_store_primitive(&context->type_store, kind, type);
}

static bool name_is(Om_String_View name, const char *expected, size_t length)
{
    return om_string_view_equal(name, (Om_String_View){expected, length});
}

static bool name_is_ci(Om_String_View name, const char *expected, size_t length)
{
    return om_name_equal_case_insensitive(name,
                                          (Om_String_View){expected, length});
}

static Om_Status set_builtin_scheme(Om_Infer_Context *context,
                                    Om_Symbol_Id symbol_id,
                                    Om_Type_Id type_id)
{
    const Om_Status status = set_symbol_monotype(context, symbol_id, type_id);
    if (status == OM_STATUS_OK) {
        context->symbol_states[symbol_id.value] = OM_INFER_SYMBOL_DONE;
    }
    return status;
}

static Om_Status native_abi_type_to_language_type(
    Om_Infer_Context *context, Om_Native_Abi_Type abi_type, bool is_result,
    Om_Type_Id *type_id)
{
    if (type_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Kind kind = OM_TYPE_VARIABLE;
    switch (abi_type) {
    case OM_NATIVE_ABI_CVOID:
        if (!is_result) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        kind = OM_TYPE_UNIT;
        break;
    case OM_NATIVE_ABI_CINT8:
    case OM_NATIVE_ABI_CINT16:
    case OM_NATIVE_ABI_CINT32:
    case OM_NATIVE_ABI_CINT64:
    case OM_NATIVE_ABI_CUINT8:
    case OM_NATIVE_ABI_CUINT16:
    case OM_NATIVE_ABI_CUINT32:
    case OM_NATIVE_ABI_CUINT64:
    case OM_NATIVE_ABI_CINT:
    case OM_NATIVE_ABI_CUINT:
    case OM_NATIVE_ABI_CLONG:
    case OM_NATIVE_ABI_CULONG:
    case OM_NATIVE_ABI_CLONGLONG:
    case OM_NATIVE_ABI_CULONGLONG:
    case OM_NATIVE_ABI_CSIZE:
        kind = OM_TYPE_INT;
        break;
    case OM_NATIVE_ABI_CFLOAT:
    case OM_NATIVE_ABI_CDOUBLE:
        kind = OM_TYPE_FLOAT;
        break;
    case OM_NATIVE_ABI_CBOOL:
        kind = OM_TYPE_BOOL;
        break;
    case OM_NATIVE_ABI_CSTRING:
        kind = OM_TYPE_STRING;
        break;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_CPOINTER:
    case OM_NATIVE_ABI_RECORD:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return get_primitive_type(context, kind, type_id);
}

static Om_Status initialize_native_builtin(Om_Infer_Context *context,
                                           const Om_Symbol *symbol)
{
    const Om_Omi_Module *module = symbol->native_module;
    if (module == NULL || symbol->native_function_index >= module->function_count
        || module->functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Function *function =
        &module->functions[symbol->native_function_index];
    if (function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || (function->parameter_count != 0 && module->parameters == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t language_parameter_count = 0;
    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter =
            &module->parameters[function->parameter_start + index];
        if (parameter->is_callback) {
            if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
                || parameter->callback_parameter_start
                    > module->callback_parameter_count
                || parameter->callback_parameter_count
                    > module->callback_parameter_count
                        - parameter->callback_parameter_start
                || (parameter->callback_parameter_count != 0
                    && module->callback_parameters == NULL)) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            for (size_t callback_index = 0;
                 callback_index < parameter->callback_parameter_count;
                 ++callback_index) {
                Om_Type_Id ignored = OM_TYPE_ID_INVALID;
                const Om_Status callback_status =
                    native_abi_type_to_language_type(
                        context,
                        module->callback_parameters[
                            parameter->callback_parameter_start
                                + callback_index].abi_type,
                        false, &ignored);
                if (callback_status != OM_STATUS_OK
                    || module->callback_parameters[
                        parameter->callback_parameter_start + callback_index]
                           .abi_type == OM_NATIVE_ABI_CSTRING) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
            }
        } else {
            language_parameter_count += 1;
        }
    }
    Om_Type_Id *parameter_types = language_parameter_count == 0 ? NULL
        : om_alloc_array(language_parameter_count, sizeof(*parameter_types));
    if (language_parameter_count != 0 && parameter_types == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t language_index = 0;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter =
            &module->parameters[function->parameter_start + index];
        if (parameter->is_callback) {
            continue;
        }
        status = native_abi_type_to_language_type(
            context, parameter->abi_type, false,
            &parameter_types[language_index++]);
    }
    Om_Type_Id result_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = native_abi_type_to_language_type(
            context, function->result_type, true, &result_type);
    }
    Om_Type_Id function_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_function(
            &context->type_store, OM_TYPE_FUNCTION, parameter_types,
            language_parameter_count, result_type, &function_type);
    }
    if (status == OM_STATUS_OK) {
        status = set_builtin_scheme(context, symbol->id, function_type);
    }
    om_free(parameter_types);
    return status;
}

static bool system_actor_name(Om_String_View name)
{
    return name_is_ci(name, OM_SYSTEM_ACTOR_NAME_CONSOLE,
                      sizeof(OM_SYSTEM_ACTOR_NAME_CONSOLE) - 1)
        || name_is_ci(name, OM_SYSTEM_ACTOR_NAME_TIMER,
                      sizeof(OM_SYSTEM_ACTOR_NAME_TIMER) - 1)
        || name_is_ci(name, OM_SYSTEM_ACTOR_NAME_FILE_SYSTEM,
                      sizeof(OM_SYSTEM_ACTOR_NAME_FILE_SYSTEM) - 1);
}

static bool system_message_name(Om_String_View name)
{
    return name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_FILE_READ,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_FILE_WRITE,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_WRITE) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED) - 1)
        || name_is_ci(name, OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER,
                      sizeof(OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER) - 1);
}

static Om_Status initialize_system_actor_builtin(
    Om_Infer_Context *context,
    const Om_Symbol *symbol)
{
    Om_Type_Id protocol = OM_TYPE_ID_INVALID;
    Om_Type_Id actor_ref = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_nominal(
        &context->type_store, OM_TYPE_ACTOR_PROTOCOL, symbol->id, &protocol);
    if (status == OM_STATUS_OK) {
        status = om_type_store_actor_ref_type(
            &context->type_store, protocol, &actor_ref);
    }
    if (status == OM_STATUS_OK) {
        status = set_builtin_scheme(context, symbol->id, actor_ref);
    }
    return status;
}

static Om_Status make_system_result_type(Om_Infer_Context *context,
                                         Om_Type_Id value_type,
                                         Om_Type_Id *result_type)
{
    Om_Type_Id error_type = OM_TYPE_ID_INVALID;
    Om_Status status = get_primitive_type(context, OM_TYPE_STRING,
                                          &error_type);
    if (status == OM_STATUS_OK) {
        status = om_type_store_result(&context->type_store, value_type,
                                      error_type, result_type);
    }
    return status;
}

static Om_Status initialize_system_message_builtin(
    Om_Infer_Context *context,
    const Om_Symbol *symbol)
{
    Om_Type_Id parameters[3] = {OM_TYPE_ID_INVALID, OM_TYPE_ID_INVALID,
                                OM_TYPE_ID_INVALID};
    size_t parameter_count = 0;
    Om_Type_Id string_type = OM_TYPE_ID_INVALID;
    Om_Type_Id unit_type = OM_TYPE_ID_INVALID;
    Om_Type_Id int_type = OM_TYPE_ID_INVALID;
    Om_Status status = get_primitive_type(context, OM_TYPE_STRING,
                                          &string_type);
    if (status == OM_STATUS_OK) {
        status = get_primitive_type(context, OM_TYPE_UNIT, &unit_type);
    }
    if (status == OM_STATUS_OK) {
        status = get_primitive_type(context, OM_TYPE_INT, &int_type);
    }

    const bool console_write = name_is_ci(
        symbol->name, OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE,
        sizeof(OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE) - 1);
    const bool file_read = name_is_ci(
        symbol->name, OM_SYSTEM_MESSAGE_NAME_FILE_READ,
        sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ) - 1);
    const bool file_write = name_is_ci(
        symbol->name, OM_SYSTEM_MESSAGE_NAME_FILE_WRITE,
        sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_WRITE) - 1);
    const bool timer_after = name_is_ci(
        symbol->name, OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER,
        sizeof(OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER) - 1);
    const bool file_read_completed = name_is_ci(
        symbol->name, OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED,
        sizeof(OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED) - 1);
    Om_Type_Id reply_protocol = OM_TYPE_ID_INVALID;
    Om_Type_Id reply_ref = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK
        && (console_write || file_read || file_write || timer_after)) {
        status = om_type_store_variable(&context->type_store, 1,
                                        &reply_protocol);
        if (status == OM_STATUS_OK) {
            status = om_type_store_actor_ref_type(&context->type_store,
                                                  reply_protocol, &reply_ref);
        }
    }
    if (status == OM_STATUS_OK && console_write) {
        parameters[0] = string_type;
        parameters[1] = reply_ref;
        parameter_count = 2;
    } else if (status == OM_STATUS_OK && file_read) {
        parameters[0] = string_type;
        parameters[1] = reply_ref;
        parameter_count = 2;
    } else if (status == OM_STATUS_OK && file_write) {
        parameters[0] = string_type;
        parameters[1] = string_type;
        parameters[2] = reply_ref;
        parameter_count = 3;
    } else if (status == OM_STATUS_OK && timer_after) {
        Om_Type_Id message_type = OM_TYPE_ID_INVALID;
        status = om_type_store_variable(&context->type_store, 1,
                                        &message_type);
        if (status == OM_STATUS_OK) {
            bool satisfied = false;
            status = om_type_store_require(&context->type_store, message_type,
                                           OM_TYPE_CONSTRAINT_MESSAGE,
                                           &satisfied);
            if (status == OM_STATUS_OK && !satisfied) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        parameters[0] = int_type;
        parameters[1] = message_type;
        parameters[2] = reply_ref;
        parameter_count = 3;
    } else if (status == OM_STATUS_OK) {
        Om_Type_Id result_type = OM_TYPE_ID_INVALID;
        Om_Type_Id result_value = file_read_completed
            ? string_type : unit_type;
        status = make_system_result_type(context, result_value, &result_type);
        parameters[0] = result_type;
        parameter_count = 1;
    }

    Om_Type_Id message_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_nominal(&context->type_store, OM_TYPE_MESSAGE,
                                       symbol->id, &message_type);
    }
    Om_Type_Id function_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_function(&context->type_store,
                                        OM_TYPE_FUNCTION, parameters,
                                        parameter_count, message_type,
                                        &function_type);
    }
    if (status == OM_STATUS_OK) {
        status = generalize_symbol(context, symbol->id, function_type, 0);
    }
    return status;
}

static Om_Status initialize_builtin(Om_Infer_Context *context,
                                    const Om_Symbol *symbol)
{
    if (symbol->native_module != NULL) {
        return initialize_native_builtin(context, symbol);
    }
    if (system_actor_name(symbol->name)) {
        return initialize_system_actor_builtin(context, symbol);
    }
    if (system_message_name(symbol->name)) {
        return initialize_system_message_builtin(context, symbol);
    }
    Om_Type_Kind primitive_kind = OM_TYPE_VARIABLE;
    if (name_is_ci(symbol->name, "Unit", 4)) {
        primitive_kind = OM_TYPE_UNIT;
    } else if (name_is_ci(symbol->name, "Node_Ref", 8)) {
        primitive_kind = OM_TYPE_NODE_REF;
    } else if (name_is_ci(symbol->name, "Actor_Id", 8)) {
        primitive_kind = OM_TYPE_ACTOR_ID;
    } else if (name_is_ci(symbol->name, "Down_Reason", 11)) {
        primitive_kind = OM_TYPE_DOWN_REASON;
    } else if (name_is_ci(symbol->name, "Actor_Down", 10)) {
        Om_Type_Id actor_id = OM_TYPE_ID_INVALID;
        Om_Type_Id reason = OM_TYPE_ID_INVALID;
        Om_Type_Id result = OM_TYPE_ID_INVALID;
        Om_Type_Id parameters[2] = {OM_TYPE_ID_INVALID, OM_TYPE_ID_INVALID};
        Om_Status status = get_primitive_type(context, OM_TYPE_ACTOR_ID,
                                              &actor_id);
        if (status == OM_STATUS_OK) {
            status = get_primitive_type(context, OM_TYPE_DOWN_REASON, &reason);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_nominal(&context->type_store, OM_TYPE_MESSAGE,
                                           symbol->id, &result);
        }
        parameters[0] = actor_id;
        parameters[1] = reason;
        Om_Type_Id function = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(&context->type_store,
                                            OM_TYPE_FUNCTION, parameters, 2,
                                            result, &function);
        }
        if (status == OM_STATUS_OK) {
            status = set_builtin_scheme(context, symbol->id, function);
        }
        return status;
    }
    if (primitive_kind != OM_TYPE_VARIABLE) {
        Om_Type_Id type_id = OM_TYPE_ID_INVALID;
        Om_Status status = get_primitive_type(context, primitive_kind, &type_id);
        if (status == OM_STATUS_OK) {
            status = set_builtin_scheme(context, symbol->id, type_id);
        }
        return status;
    }
    if (name_is(symbol->name, "Ok", 2) || name_is(symbol->name, "Error", 5)) {
        const bool is_ok = name_is(symbol->name, "Ok", 2);
        Om_Type_Id input = OM_TYPE_ID_INVALID;
        Om_Type_Id other = OM_TYPE_ID_INVALID;
        Om_Type_Id result = OM_TYPE_ID_INVALID;
        Om_Type_Id function = OM_TYPE_ID_INVALID;
        Om_Status status = om_type_store_variable(&context->type_store, 1, &input);
        if (status == OM_STATUS_OK) {
            status = om_type_store_variable(&context->type_store, 1, &other);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_result(
                &context->type_store,
                is_ok ? input : other,
                is_ok ? other : input,
                &result);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(
                &context->type_store, OM_TYPE_FUNCTION, &input, 1,
                result, &function);
        }
        if (status == OM_STATUS_OK) {
            status = generalize_symbol(context, symbol->id, function, 0);
        }
        return status;
    }
    if (name_is_ci(symbol->name, "Result", 6)) {
        return OM_STATUS_OK;
    }
    Om_Type_Id unknown_builtin = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_variable(&context->type_store, 1,
                                              &unknown_builtin);
    if (status == OM_STATUS_OK) {
        status = generalize_symbol(context, symbol->id, unknown_builtin, 0);
    }
    return status;
}

static Om_Status initialize_callable(Om_Infer_Context *context,
                                     const Om_Symbol *symbol)
{
    const Om_Ast_Node *declaration = infer_node(
        context, symbol->module_id, symbol->declaration_node);
    if (declaration == NULL
        || (declaration->kind != OM_AST_FUNCTION_DECLARATION
            && declaration->kind != OM_AST_ACTION_DECLARATION)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t parameter_count = declaration->auxiliary;
    if (declaration->child_count != parameter_count + 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id *parameters = om_alloc_array(parameter_count, sizeof(*parameters));
    if (parameter_count != 0 && parameters == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count; ++index) {
        status = om_type_store_variable(&context->type_store, 1, &parameters[index]);
    }
    Om_Type_Id return_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK
        && declaration->kind == OM_AST_ACTION_DECLARATION) {
        status = get_primitive_type(context, OM_TYPE_UNIT, &return_type);
    } else if (status == OM_STATUS_OK) {
        status = om_type_store_variable(&context->type_store, 1, &return_type);
    }
    Om_Type_Id function_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_function(
            &context->type_store,
            declaration->kind == OM_AST_ACTION_DECLARATION
                ? OM_TYPE_ACTION : OM_TYPE_FUNCTION,
            parameters, parameter_count, return_type, &function_type);
    }
    if (status == OM_STATUS_OK) {
        status = set_symbol_monotype(context, symbol->id, function_type);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < parameter_count; ++index) {
        const Om_Ast_Node_Id parameter_id = infer_child(
            context, symbol->module_id, declaration, index);
        const Om_Symbol_Id parameter_symbol = om_resolution_symbol_for_node(
            context->resolution, symbol->module_id, parameter_id);
        if (parameter_symbol.value == UINT32_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            status = set_symbol_monotype(context, parameter_symbol,
                                         parameters[index]);
        }
    }
    om_free(parameters);
    return status;
}

static Om_Status initialize_symbols(Om_Infer_Context *context)
{
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &context->resolution->symbols[index];
        Om_Status status = OM_STATUS_OK;
        switch (symbol->kind) {
        case OM_SYMBOL_BUILTIN:
            status = initialize_builtin(context, symbol);
            break;
        case OM_SYMBOL_FUNCTION:
        case OM_SYMBOL_ACTION:
            status = initialize_callable(context, symbol);
            break;
        case OM_SYMBOL_RECORD:
        case OM_SYMBOL_MESSAGE:
        case OM_SYMBOL_ACTOR: {
            const Om_Type_Kind kind = symbol->kind == OM_SYMBOL_RECORD
                ? OM_TYPE_RECORD
                : symbol->kind == OM_SYMBOL_MESSAGE
                    ? OM_TYPE_MESSAGE : OM_TYPE_ACTOR_PROTOCOL;
            Om_Type_Id nominal = OM_TYPE_ID_INVALID;
            status = om_type_store_nominal(&context->type_store, kind,
                                           symbol->id, &nominal);
            if (status == OM_STATUS_OK) {
                status = set_symbol_monotype(context, symbol->id, nominal);
            }
            break;
        }
        case OM_SYMBOL_FIELD: {
            Om_Type_Id field_type = OM_TYPE_ID_INVALID;
            status = om_type_store_variable(&context->type_store, 0,
                                            &field_type);
            if (status == OM_STATUS_OK) {
                status = set_symbol_monotype(context, symbol->id, field_type);
            }
            break;
        }
        case OM_SYMBOL_MODULE_ALIAS:
        case OM_SYMBOL_PARAMETER:
        case OM_SYMBOL_IMMUTABLE_BINDING:
        case OM_SYMBOL_MUTABLE_BINDING:
        case OM_SYMBOL_LOOP_VARIABLE:
            break;
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Symbol_Id resolved_symbol_for_node(const Om_Infer_Context *context,
                                             Om_Module_Id module,
                                             Om_Ast_Node_Id node)
{
    return om_resolution_symbol_for_node(context->resolution, module, node);
}

static Om_Type_Id type_for_symbol_scheme(const Om_Infer_Context *context,
                                        Om_Symbol_Id symbol_id)
{
    if (symbol_id.value >= context->resolution->symbol_count) {
        return OM_TYPE_ID_INVALID;
    }
    return context->symbol_schemes[symbol_id.value].type_id;
}

static Om_Status infer_symbol_value(Om_Infer_Context *context,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id,
                                    Om_Symbol_Id symbol_id,
                                    uint32_t level,
                                    Om_Type_Id *type_id)
{
    const Om_Symbol *symbol = infer_symbol(context, symbol_id);
    Om_Type_Scheme *scheme = scheme_for(context, symbol_id);
    if (symbol == NULL || scheme == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (symbol->kind == OM_SYMBOL_FUNCTION || symbol->kind == OM_SYMBOL_ACTION) {
        const Om_Infer_Symbol_State state = context->symbol_states[symbol_id.value];
        if (state == OM_INFER_SYMBOL_IN_PROGRESS) {
            for (size_t index = context->callable_stack.length; index > 0; --index) {
                const Om_Symbol_Id *active = om_vector_at_const(
                    &context->callable_stack, index - 1);
                if (active->value == symbol_id.value) {
                    for (size_t recursive_index = index - 1;
                         recursive_index < context->callable_stack.length;
                         ++recursive_index) {
                        const Om_Symbol_Id *recursive_symbol = om_vector_at_const(
                            &context->callable_stack, recursive_index);
                        context->recursive_callables[recursive_symbol->value] = true;
                    }
                    break;
                }
            }
        } else if (state != OM_INFER_SYMBOL_DONE) {
            const Om_Status status = infer_callable(context, symbol_id);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    if (scheme->type_id.value == UINT32_MAX) {
        if (symbol->kind == OM_SYMBOL_BUILTIN
            && name_is(symbol->name, "Result", 6)) {
            Om_Status status = add_type_diagnostic(
                context, OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                module, infer_node(context, module, node_id)->span,
                OM_TYPE_ID_INVALID, OM_TYPE_ID_INVALID);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                type_id);
            }
            if (status == OM_STATUS_OK) {
                status = set_node_type(context, module, node_id, *type_id);
            }
            return status;
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status status = om_type_store_instantiate(
        &context->type_store, scheme, level, type_id);
    if (status == OM_STATUS_OK && node_id.value != UINT32_MAX) {
        return set_node_type(context, module, node_id, *type_id);
    }
    return status;
}

static Om_Status find_record_field(const Om_Infer_Context *context,
                                   Om_Symbol_Id record_symbol,
                                   Om_String_View field_name,
                                   Om_Symbol_Id *field_symbol)
{
    if (field_symbol == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *field_symbol = OM_SYMBOL_ID_INVALID;
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *candidate = &context->resolution->symbols[index];
        if (candidate->kind != OM_SYMBOL_FIELD
            || !om_string_view_equal(candidate->name, field_name)
            || candidate->scope_id.value >= context->resolution->scope_count) {
            continue;
        }
        const Om_Scope *scope = &context->resolution->scopes[
            candidate->scope_id.value];
        const Om_Symbol_Id owner = resolved_symbol_for_node(
            context, candidate->module_id, scope->owner_node);
        if (owner.value == record_symbol.value) {
            *field_symbol = candidate->id;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

static Om_Status find_unique_field_owner(const Om_Infer_Context *context,
                                         Om_String_View field_name,
                                         Om_Symbol_Id *record_symbol,
                                         Om_Symbol_Id *field_symbol)
{
    size_t matches = 0;
    if (record_symbol == NULL || field_symbol == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *record_symbol = OM_SYMBOL_ID_INVALID;
    *field_symbol = OM_SYMBOL_ID_INVALID;
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *candidate = &context->resolution->symbols[index];
        if (candidate->kind != OM_SYMBOL_FIELD
            || !om_string_view_equal(candidate->name, field_name)
            || candidate->scope_id.value >= context->resolution->scope_count) {
            continue;
        }
        const Om_Scope *scope = &context->resolution->scopes[
            candidate->scope_id.value];
        const Om_Symbol_Id owner = resolved_symbol_for_node(
            context, candidate->module_id, scope->owner_node);
        const Om_Symbol *owner_symbol = infer_symbol(context, owner);
        if (owner_symbol == NULL || owner_symbol->kind != OM_SYMBOL_RECORD) {
            continue;
        }
        ++matches;
        *record_symbol = owner;
        *field_symbol = candidate->id;
    }
    if (matches == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    if (matches != 1) {
        *record_symbol = OM_SYMBOL_ID_INVALID;
        *field_symbol = OM_SYMBOL_ID_INVALID;
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}

static Om_Status infer_record_constructor(Om_Infer_Context *context,
                                          Om_Module_Id module,
                                          Om_Ast_Node_Id call_id,
                                          const Om_Ast_Node *call,
                                          Om_Symbol_Id entity_symbol,
                                          uint32_t level,
                                          Om_Type_Id *type_id)
{
    const Om_Symbol *symbol = infer_symbol(context, entity_symbol);
    const Om_Ast_Node *declaration = symbol != NULL
        ? infer_node(context, symbol->module_id, symbol->declaration_node) : NULL;
    if (symbol == NULL || declaration == NULL
        || (symbol->kind != OM_SYMBOL_RECORD
            && symbol->kind != OM_SYMBOL_MESSAGE)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t argument_count = call->child_count - 1;
    if (declaration->child_count != argument_count) {
        const Om_Type_Id expected = type_for_symbol_scheme(context, entity_symbol);
        return add_type_diagnostic(context,
                                   OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                   module, call->span, expected,
                                   OM_TYPE_ID_INVALID);
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < argument_count; ++index) {
        const Om_Ast_Node_Id field_node = infer_child(
            context, symbol->module_id, declaration, index);
        const Om_Symbol_Id field_symbol = resolved_symbol_for_node(
            context, symbol->module_id, field_node);
        const Om_Type_Id field_type = type_for_symbol_scheme(context, field_symbol);
        Om_Type_Id argument_type = OM_TYPE_ID_INVALID;
        const Om_Ast_Node_Id argument_node = infer_child(context, module, call,
                                                         index + 1);
        status = infer_expression(context, module, argument_node,
                                  level, &argument_type);
        if (status == OM_STATUS_OK) {
            const Om_Ast_Node *argument = infer_node(context, module, argument_node);
            status = unify_at(context, module,
                              argument != NULL ? argument->span : call->span,
                              field_type, argument_type);
        }
    }
    if (status == OM_STATUS_OK) {
        status = infer_symbol_value(context, module,
                                    infer_child(context, module, call, 0),
                                    entity_symbol, level, type_id);
    }
    if (status == OM_STATUS_OK) {
        status = set_node_type(context, module, call_id, *type_id);
    }
    return status;
}

static Om_Status infer_call(Om_Infer_Context *context,
                            Om_Module_Id module,
                            Om_Ast_Node_Id node_id,
                            const Om_Ast_Node *node,
                            uint32_t level,
                            Om_Type_Id *type_id)
{
    if (node->child_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Ast_Node_Id callee_id = infer_child(context, module, node, 0);
    const Om_Symbol_Id callee_symbol_id = resolved_symbol_for_node(
        context, module, callee_id);
    const Om_Symbol *callee_symbol = infer_symbol(context, callee_symbol_id);
    if (callee_symbol != NULL
        && (callee_symbol->kind == OM_SYMBOL_RECORD
            || callee_symbol->kind == OM_SYMBOL_MESSAGE)) {
        return infer_record_constructor(context, module, node_id, node,
                                        callee_symbol_id, level, type_id);
    }
    Om_Type_Id callee_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module, callee_id,
                                        level, &callee_type);
    const size_t argument_count = node->child_count - 1;
    Om_Type_Id *argument_types = om_alloc_array(argument_count,
                                                sizeof(*argument_types));
    if (argument_count != 0 && argument_types == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < argument_count; ++index) {
        status = infer_expression(context, module,
                                  infer_child(context, module, node, index + 1),
                                  level, &argument_types[index]);
    }
    if (status != OM_STATUS_OK) {
        om_free(argument_types);
        return status;
    }
    Om_Type_View callee_view = {0};
    status = om_type_store_get(&context->type_store, callee_type, &callee_view);
    if (status != OM_STATUS_OK) {
        om_free(argument_types);
        return status;
    }
    if (callee_view.kind == OM_TYPE_VARIABLE) {
        Om_Type_Id return_type = OM_TYPE_ID_INVALID;
        status = om_type_store_variable(&context->type_store, level, &return_type);
        Om_Type_Id expected_function = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(&context->type_store,
                                            OM_TYPE_FUNCTION,
                                            argument_types, argument_count,
                                            return_type, &expected_function);
        }
        if (status == OM_STATUS_OK) {
            status = unify_at(context, module, node->span,
                              expected_function, callee_type);
        }
        if (status == OM_STATUS_OK) {
            *type_id = return_type;
        }
    } else if (callee_view.kind != OM_TYPE_FUNCTION
               && callee_view.kind != OM_TYPE_ACTION) {
        Om_Type_Id expected_function = OM_TYPE_ID_INVALID;
        Om_Type_Id expected_return = OM_TYPE_ID_INVALID;
        status = om_type_store_variable(&context->type_store, level,
                                        &expected_return);
        if (status == OM_STATUS_OK) {
            status = om_type_store_function(&context->type_store,
                                            OM_TYPE_FUNCTION,
                                            argument_types, argument_count,
                                            expected_return,
                                            &expected_function);
        }
        if (status == OM_STATUS_OK) {
            status = unify_at(context, module, node->span,
                              expected_function, callee_type);
        }
        if (status == OM_STATUS_OK) {
            *type_id = expected_return;
        }
    } else if (callee_view.child_count != argument_count + 1) {
        const size_t parameter_count = callee_view.child_count - 1;
        const size_t common_count = parameter_count < argument_count
            ? parameter_count : argument_count;
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < common_count; ++index) {
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(&context->type_store, callee_type,
                                            index, &parameter_type);
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node *argument = infer_node(
                    context, module, infer_child(context, module, node, index + 1));
                status = unify_at(context, module,
                                  argument != NULL ? argument->span : node->span,
                                  parameter_type, argument_types[index]);
            }
        }
        if (status == OM_STATUS_OK) {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, node->span,
                                         OM_TYPE_ID_INVALID, callee_type);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(&context->type_store, callee_type,
                                            callee_view.child_count - 1,
                                            type_id);
        }
    } else {
        for (size_t index = 0; status == OM_STATUS_OK && index < argument_count; ++index) {
            Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(&context->type_store, callee_type,
                                            index, &parameter_type);
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node *argument = infer_node(
                    context, module, infer_child(context, module, node, index + 1));
                status = unify_at(context, module,
                                  argument != NULL ? argument->span : node->span,
                                  parameter_type, argument_types[index]);
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(&context->type_store, callee_type,
                                            argument_count, type_id);
        }
    }
    om_free(argument_types);
    if (status == OM_STATUS_OK) {
        status = set_node_type(context, module, node_id, *type_id);
    }
    return status;
}

static Om_Status infer_member(Om_Infer_Context *context,
                              Om_Module_Id module,
                              Om_Ast_Node_Id node_id,
                              const Om_Ast_Node *node,
                              uint32_t level,
                              Om_Type_Id *type_id)
{
    const Om_Symbol_Id resolved = resolved_symbol_for_node(context, module,
                                                            node_id);
    const Om_Symbol *symbol = infer_symbol(context, resolved);
    if (symbol != NULL && symbol->kind != OM_SYMBOL_MODULE_ALIAS) {
        return infer_symbol_value(context, module, node_id, resolved,
                                  level, type_id);
    }
    if (node->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id receiver_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(
        context, module, infer_child(context, module, node, 0),
        level, &receiver_type);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Type_View receiver_view = {0};
    status = om_type_store_get(&context->type_store, receiver_type,
                               &receiver_view);
    Om_Symbol_Id record_symbol = OM_SYMBOL_ID_INVALID;
    if (status == OM_STATUS_OK && receiver_view.kind == OM_TYPE_RECORD) {
        record_symbol = receiver_view.nominal_symbol;
    } else if (status == OM_STATUS_OK
               && receiver_view.kind == OM_TYPE_VARIABLE) {
        Om_Symbol_Id field_symbol = OM_SYMBOL_ID_INVALID;
        status = find_unique_field_owner(context, node->name,
                                         &record_symbol, &field_symbol);
        if (status == OM_STATUS_OK) {
            Om_Type_Id record_type = type_for_symbol_scheme(context, record_symbol);
            status = unify_at(context, module, node->span,
                              receiver_type, record_type);
            if (status == OM_STATUS_OK) {
                *type_id = type_for_symbol_scheme(context, field_symbol);
                status = set_node_type(context, module, node_id, *type_id);
            }
            return status;
        }
    }
    if (status == OM_STATUS_OK && record_symbol.value != UINT32_MAX) {
        Om_Symbol_Id field_symbol = OM_SYMBOL_ID_INVALID;
        status = find_record_field(context, record_symbol, node->name,
                                   &field_symbol);
        if (status == OM_STATUS_OK) {
            *type_id = type_for_symbol_scheme(context, field_symbol);
            status = set_node_type(context, module, node_id, *type_id);
            return status;
        }
    }
    if (status == OM_STATUS_NOT_FOUND || status == OM_STATUS_INVALID_ARGUMENT) {
        status = add_type_diagnostic(context,
                                     OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                     module, node->span,
                                     OM_TYPE_ID_INVALID, receiver_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_variable(&context->type_store, level, type_id);
        }
        if (status == OM_STATUS_OK) {
            status = set_node_type(context, module, node_id, *type_id);
        }
    }
    return status;
}

static Om_Status require_constraint_at(Om_Infer_Context *context,
                                       Om_Module_Id module,
                                       Om_Source_Span span,
                                       Om_Type_Id type,
                                       uint32_t constraint)
{
    bool satisfied = false;
    Om_Status status = om_type_store_require(&context->type_store, type,
                                             constraint, &satisfied);
    if (status == OM_STATUS_OK && !satisfied) {
        status = add_type_diagnostic(context,
                                     OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                     module, span, OM_TYPE_ID_INVALID, type);
    }
    return status;
}

static Om_Status infer_binary(Om_Infer_Context *context,
                              Om_Module_Id module,
                              const Om_Ast_Node *node,
                              uint32_t level,
                              Om_Type_Id *type_id)
{
    if (node->child_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id left = OM_TYPE_ID_INVALID;
    Om_Type_Id right = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module,
                                        infer_child(context, module, node, 0),
                                        level, &left);
    if (status == OM_STATUS_OK) {
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 1),
                                  level, &right);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Type_Id bool_type = context->type_store.primitive_types[OM_TYPE_BOOL];
    if (node->keyword == OM_KEYWORD_AND || node->keyword == OM_KEYWORD_OR) {
        status = unify_at(context, module, node->span, bool_type, left);
        if (status == OM_STATUS_OK) {
            status = unify_at(context, module, node->span, bool_type, right);
        }
        *type_id = bool_type;
        return status;
    }
    const bool equality = node->token_kind == OM_TOKEN_EQUAL
                          || node->token_kind == OM_TOKEN_NOT_EQUAL;
    const bool comparison = equality
        || node->token_kind == OM_TOKEN_LESS
        || node->token_kind == OM_TOKEN_GREATER
        || node->token_kind == OM_TOKEN_LESS_EQUAL
        || node->token_kind == OM_TOKEN_GREATER_EQUAL;
    if (comparison) {
        status = unify_at(context, module, node->span, left, right);
        if (status == OM_STATUS_OK) {
            status = require_constraint_at(
                context, module, node->span, left,
                equality ? (uint32_t)OM_TYPE_CONSTRAINT_COMPARABLE
                         : (uint32_t)OM_TYPE_CONSTRAINT_ORDERED);
        }
        *type_id = bool_type;
        return status;
    }
    status = unify_at(context, module, node->span, left, right);
    const uint32_t constraint = node->token_kind == OM_TOKEN_PERCENT
        ? (uint32_t)OM_TYPE_CONSTRAINT_INTEGRAL
        : (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC;
    if (status == OM_STATUS_OK) {
        status = require_constraint_at(context, module, node->span, left,
                                       constraint);
    }
    *type_id = left;
    return status;
}

static Om_Status infer_with(Om_Infer_Context *context,
                            Om_Module_Id module,
                            const Om_Ast_Node *node,
                            uint32_t level,
                            Om_Type_Id *type_id)
{
    if (node->child_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id base_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module,
                                        infer_child(context, module, node, 0),
                                        level, &base_type);
    Om_Type_View base_view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(&context->type_store, base_type, &base_view);
    }
    Om_Symbol_Id record_symbol = OM_SYMBOL_ID_INVALID;
    if (status == OM_STATUS_OK && base_view.kind == OM_TYPE_RECORD) {
        record_symbol = base_view.nominal_symbol;
    } else if (status == OM_STATUS_OK && base_view.kind == OM_TYPE_VARIABLE
               && node->child_count > 1) {
        const Om_Ast_Node *update = infer_node(
            context, module, infer_child(context, module, node, 1));
        Om_Symbol_Id field_symbol = OM_SYMBOL_ID_INVALID;
        status = update == NULL ? OM_STATUS_INVALID_ARGUMENT
            : find_unique_field_owner(context, update->name,
                                      &record_symbol, &field_symbol);
        if (status == OM_STATUS_OK) {
            const Om_Type_Id record_type = type_for_symbol_scheme(context,
                                                                  record_symbol);
            status = unify_at(context, module, node->span,
                              base_type, record_type);
        }
    } else if (status == OM_STATUS_OK) {
        status = add_type_diagnostic(context,
                                     OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                     module, node->span, OM_TYPE_ID_INVALID,
                                     base_type);
    }
    for (size_t index = 1; status == OM_STATUS_OK && index < node->child_count; ++index) {
        const Om_Ast_Node_Id update_id = infer_child(context, module, node, index);
        const Om_Ast_Node *update = infer_node(context, module, update_id);
        if (update == NULL || update->kind != OM_AST_FIELD_UPDATE
            || update->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Symbol_Id field_symbol = OM_SYMBOL_ID_INVALID;
        status = find_record_field(context, record_symbol, update->name,
                                   &field_symbol);
        if (status == OM_STATUS_NOT_FOUND) {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, update->span,
                                         OM_TYPE_ID_INVALID, base_type);
        }
        Om_Type_Id field_value = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK) {
            status = infer_expression(context, module,
                                      infer_child(context, module, update, 0),
                                      level, &field_value);
        }
        if (status == OM_STATUS_OK) {
            status = unify_at(context, module, update->span,
                              type_for_symbol_scheme(context, field_symbol),
                              field_value);
        }
    }
    if (status == OM_STATUS_OK) {
        *type_id = base_type;
    }
    return status;
}

static Om_Status record_empty_collection(Om_Infer_Context *context,
                                         Om_Module_Id module,
                                         Om_Ast_Node_Id node,
                                         Om_Type_Id type)
{
    const Om_Empty_Collection entry = {module, node, type};
    return om_vector_push(&context->empty_collections, &entry);
}

static Om_Status infer_expression(Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id node_id,
                                  uint32_t level,
                                  Om_Type_Id *type_id)
{
    const Om_Ast_Node *node = infer_node(context, module, node_id);
    if (node == NULL || type_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id inferred = OM_TYPE_ID_INVALID;
    Om_Status status = OM_STATUS_OK;
    switch (node->kind) {
    case OM_AST_NAME: {
        const Om_Symbol_Id symbol = resolved_symbol_for_node(context, module, node_id);
        if (symbol.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = infer_symbol_value(context, module, node_id, symbol,
                                    level, &inferred);
        break;
    }
    case OM_AST_INTEGER_LITERAL:
        status = get_primitive_type(context, OM_TYPE_INT, &inferred);
        break;
    case OM_AST_FLOAT_LITERAL:
        status = get_primitive_type(context, OM_TYPE_FLOAT, &inferred);
        break;
    case OM_AST_BOOLEAN_LITERAL:
        status = get_primitive_type(context, OM_TYPE_BOOL, &inferred);
        break;
    case OM_AST_STRING_LITERAL:
    case OM_AST_STRING_TEXT:
    case OM_AST_INTERPOLATED_STRING: {
        status = get_primitive_type(context, OM_TYPE_STRING, &inferred);
        for (size_t index = 0; status == OM_STATUS_OK
                               && node->kind == OM_AST_INTERPOLATED_STRING
                               && index < node->child_count; ++index) {
            const Om_Ast_Node_Id part_id = infer_child(context, module, node, index);
            const Om_Ast_Node *part = infer_node(context, module, part_id);
            if (part == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (part->kind != OM_AST_STRING_TEXT) {
                Om_Type_Id part_type = OM_TYPE_ID_INVALID;
                status = infer_expression(context, module, part_id, level,
                                          &part_type);
                if (status == OM_STATUS_OK) {
                    status = require_constraint_at(
                        context, module, part->span, part_type,
                        (uint32_t)OM_TYPE_CONSTRAINT_PRINTABLE);
                }
            }
        }
        break;
    }
    case OM_AST_NULL_LITERAL:
        status = get_primitive_type(context, OM_TYPE_NULL, &inferred);
        break;
    case OM_AST_DOWN_REASON_LITERAL:
        status = get_primitive_type(context, OM_TYPE_DOWN_REASON, &inferred);
        break;
    case OM_AST_SELF:
        if (context->in_actor_receive
            && context->current_actor.value != UINT32_MAX) {
            status = om_type_store_actor_ref(&context->type_store,
                                             context->current_actor,
                                             &inferred);
        } else {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, node->span,
                                         OM_TYPE_ID_INVALID,
                                         OM_TYPE_ID_INVALID);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &inferred);
            }
        }
        break;
    case OM_AST_GROUP:
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &inferred);
        break;
    case OM_AST_MEMBER:
        return infer_member(context, module, node_id, node, level, type_id);
    case OM_AST_CALL:
        return infer_call(context, module, node_id, node, level, type_id);
    case OM_AST_BINARY:
        status = infer_binary(context, module, node, level, &inferred);
        break;
    case OM_AST_UNARY: {
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id operand = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &operand);
        if (status != OM_STATUS_OK) {
            break;
        }
        if (node->token_kind == OM_TOKEN_MINUS) {
            status = require_constraint_at(
                context, module, node->span, operand,
                (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC);
            inferred = operand;
        } else if (node->keyword == OM_KEYWORD_NOT) {
            status = get_primitive_type(context, OM_TYPE_BOOL, &inferred);
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  inferred, operand);
            }
        } else if (node->keyword == OM_KEYWORD_TRY) {
            Om_Type_Id value_type = OM_TYPE_ID_INVALID;
            Om_Type_Id error_type = OM_TYPE_ID_INVALID;
            status = om_type_store_variable(&context->type_store, level,
                                            &value_type);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &error_type);
            }
            Om_Type_Id result_type = OM_TYPE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = om_type_store_result(&context->type_store,
                                              value_type, error_type,
                                              &result_type);
            }
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  result_type, operand);
            }
            if (status == OM_STATUS_OK && !context->in_actor_receive
                && context->return_type.value != UINT32_MAX) {
                Om_Type_Id propagated = OM_TYPE_ID_INVALID;
                status = om_type_store_result(&context->type_store,
                                              value_type, error_type,
                                              &propagated);
                if (status == OM_STATUS_OK) {
                    status = unify_at(context, module, node->span,
                                      context->return_type, propagated);
                }
            }
            inferred = value_type;
        } else {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        break;
    }
    case OM_AST_LIST: {
        Om_Type_Id element_type = OM_TYPE_ID_INVALID;
        status = om_type_store_variable(&context->type_store, level + 1,
                                        &element_type);
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < node->child_count; ++index) {
            Om_Type_Id item_type = OM_TYPE_ID_INVALID;
            const Om_Ast_Node_Id item_id = infer_child(context, module, node, index);
            status = infer_expression(context, module, item_id, level, &item_type);
            if (status == OM_STATUS_OK) {
                const Om_Ast_Node *item = infer_node(context, module, item_id);
                status = unify_at(context, module,
                                  item != NULL ? item->span : node->span,
                                  element_type, item_type);
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_list(&context->type_store,
                                        element_type, &inferred);
        }
        if (status == OM_STATUS_OK && node->child_count == 0) {
            status = record_empty_collection(context, module, node_id,
                                             inferred);
        }
        break;
    }
    case OM_AST_MAP: {
        Om_Type_Id key_type = OM_TYPE_ID_INVALID;
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        status = om_type_store_variable(&context->type_store, level + 1,
                                        &key_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_variable(&context->type_store, level + 1,
                                            &value_type);
        }
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < node->child_count; ++index) {
            const Om_Ast_Node_Id entry_id = infer_child(context, module, node, index);
            const Om_Ast_Node *entry = infer_node(context, module, entry_id);
            if (entry == NULL || entry->kind != OM_AST_MAP_ENTRY
                || entry->child_count != 2) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            Om_Type_Id item_key = OM_TYPE_ID_INVALID;
            Om_Type_Id item_value = OM_TYPE_ID_INVALID;
            status = infer_expression(context, module,
                                      infer_child(context, module, entry, 0),
                                      level, &item_key);
            if (status == OM_STATUS_OK) {
                status = infer_expression(context, module,
                                          infer_child(context, module, entry, 1),
                                          level, &item_value);
            }
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, entry->span,
                                  key_type, item_key);
            }
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, entry->span,
                                  value_type, item_value);
            }
        }
        if (status == OM_STATUS_OK) {
            status = require_constraint_at(context, module, node->span,
                                           key_type,
                                           (uint32_t)OM_TYPE_CONSTRAINT_HASH_KEY);
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_map(&context->type_store, key_type,
                                       value_type, &inferred);
        }
        if (status == OM_STATUS_OK && node->child_count == 0) {
            status = record_empty_collection(context, module, node_id,
                                             inferred);
        }
        break;
    }
    case OM_AST_INDEX: {
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id container = OM_TYPE_ID_INVALID;
        Om_Type_Id index_type = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &container);
        if (status == OM_STATUS_OK) {
            status = infer_expression(context, module,
                                      infer_child(context, module, node, 1),
                                      level, &index_type);
        }
        Om_Type_View container_view = {0};
        if (status == OM_STATUS_OK) {
            status = om_type_store_get(&context->type_store, container,
                                       &container_view);
        }
        if (status == OM_STATUS_OK && container_view.kind == OM_TYPE_LIST) {
            status = unify_at(context, module, node->span,
                              context->type_store.primitive_types[OM_TYPE_INT],
                              index_type);
            if (status == OM_STATUS_OK) {
                status = om_type_store_child_at(&context->type_store,
                                                container, 0, &inferred);
            }
        } else if (status == OM_STATUS_OK && container_view.kind == OM_TYPE_MAP) {
            Om_Type_Id key_type = OM_TYPE_ID_INVALID;
            status = om_type_store_child_at(&context->type_store, container,
                                            0, &key_type);
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  key_type, index_type);
            }
            if (status == OM_STATUS_OK) {
                status = require_constraint_at(
                    context, module, node->span, key_type,
                    (uint32_t)OM_TYPE_CONSTRAINT_HASH_KEY);
            }
            if (status == OM_STATUS_OK) {
                status = om_type_store_child_at(&context->type_store,
                                                container, 1, &inferred);
            }
        } else if (status == OM_STATUS_OK
                   && container_view.kind == OM_TYPE_VARIABLE) {
            Om_Type_Id resolved_index = OM_TYPE_ID_INVALID;
            status = om_type_store_resolve(&context->type_store, index_type,
                                           &resolved_index);
            Om_Type_View index_view = {0};
            if (status == OM_STATUS_OK) {
                status = om_type_store_get(&context->type_store, resolved_index,
                                           &index_view);
            }
            const bool use_map = status == OM_STATUS_OK
                && (index_view.kind == OM_TYPE_STRING
                    || index_view.kind == OM_TYPE_BOOL);
            Om_Type_Id value_type = OM_TYPE_ID_INVALID;
            Om_Type_Id expected_container = OM_TYPE_ID_INVALID;
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &value_type);
            }
            if (status == OM_STATUS_OK && use_map) {
                status = require_constraint_at(
                    context, module, node->span, index_type,
                    (uint32_t)OM_TYPE_CONSTRAINT_HASH_KEY);
            }
            if (status == OM_STATUS_OK && use_map) {
                status = om_type_store_map(&context->type_store, index_type,
                                           value_type, &expected_container);
            } else if (status == OM_STATUS_OK) {
                status = om_type_store_list(&context->type_store, value_type,
                                            &expected_container);
                if (status == OM_STATUS_OK) {
                    status = unify_at(
                        context, module, node->span,
                        context->type_store.primitive_types[OM_TYPE_INT],
                        index_type);
                }
            }
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  expected_container, container);
            }
            inferred = value_type;
        } else if (status == OM_STATUS_OK) {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, node->span,
                                         OM_TYPE_ID_INVALID, container);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &inferred);
            }
        }
        break;
    }
    case OM_AST_WITH:
        status = infer_with(context, module, node, level, &inferred);
        break;
    case OM_AST_SPAWN: {
        const Om_Symbol_Id actor_id = resolved_symbol_for_node(context, module,
                                                               node_id);
        const Om_Symbol *actor = infer_symbol(context, actor_id);
        if (actor == NULL || actor->kind != OM_SYMBOL_ACTOR) {
            status = add_type_diagnostic(context,
                                         OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                         module, node->span,
                                         OM_TYPE_ID_INVALID,
                                         OM_TYPE_ID_INVALID);
            if (status == OM_STATUS_OK) {
                status = om_type_store_variable(&context->type_store, level,
                                                &inferred);
            }
            break;
        }
        if (node->child_count > 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (node->child_count == 1) {
            Om_Type_Id placement = OM_TYPE_ID_INVALID;
            status = infer_expression(context, module,
                                      infer_child(context, module, node, 0),
                                      level, &placement);
            if (status == OM_STATUS_OK) {
                status = unify_at(context, module, node->span,
                                  context->type_store.primitive_types[OM_TYPE_NODE_REF],
                                  placement);
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_type_store_actor_ref(&context->type_store, actor_id,
                                             &inferred);
        }
        break;
    }
    case OM_AST_FIELD_UPDATE:
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &inferred);
        break;
    case OM_AST_MAP_ENTRY:
        return OM_STATUS_INVALID_ARGUMENT;
    case OM_AST_PROGRAM:
    case OM_AST_IMPORT:
    case OM_AST_RECORD_DECLARATION:
    case OM_AST_MESSAGE_DECLARATION:
    case OM_AST_FUNCTION_DECLARATION:
    case OM_AST_ACTION_DECLARATION:
    case OM_AST_ACTOR_DECLARATION:
    case OM_AST_RECEIVE_DECLARATION:
    case OM_AST_PARAMETER:
    case OM_AST_BLOCK:
    case OM_AST_BINDING:
    case OM_AST_RETURN:
    case OM_AST_BREAK:
    case OM_AST_CONTINUE:
    case OM_AST_SEND:
    case OM_AST_WATCH:
    case OM_AST_STOP:
    case OM_AST_CALL_STATEMENT:
    case OM_AST_IF:
    case OM_AST_IF_ARM:
    case OM_AST_WHILE:
    case OM_AST_FOR:
    case OM_AST_SELECT:
    case OM_AST_CASE_ARM:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        *type_id = inferred;
        status = set_node_type(context, module, node_id, inferred);
    }
    return status;
}

static Om_Status infer_expression(Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id node_id,
                                  uint32_t level,
                                  Om_Type_Id *type_id);
static Om_Status infer_select(Om_Infer_Context *context,
                              Om_Module_Id module,
                              Om_Ast_Node_Id node_id,
                              const Om_Ast_Node *node,
                              uint32_t level)
{
    if (node->child_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Ast_Node_Id scrutinee_id = infer_child(context, module, node, 0);
    Om_Type_Id scrutinee_type = OM_TYPE_ID_INVALID;
    Om_Status status = infer_expression(context, module, scrutinee_id, level,
                                         &scrutinee_type);
    for (size_t index = 1; status == OM_STATUS_OK
                           && index < node->child_count; ++index) {
        const Om_Ast_Node_Id arm_id = infer_child(context, module, node, index);
        const Om_Ast_Node *arm = infer_node(context, module, arm_id);
        if (arm == NULL || arm->kind != OM_AST_CASE_ARM) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const bool is_else = (arm->flags & OM_AST_FLAG_CASE_ELSE) != 0;
        if (is_else) {
            if (arm->child_count != 1) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            status = infer_block(context, module,
                                 infer_child(context, module, arm, 0), level);
        } else {
            if (arm->child_count != 2) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            const Om_Ast_Node_Id pattern_id = infer_child(context, module, arm, 0);
            const Om_Ast_Node *pattern = infer_node(context, module, pattern_id);
            if (pattern == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            bool result_pattern = false;
            bool is_ok_pattern = false;
            Om_Ast_Node_Id constructor_id = OM_AST_NODE_ID_INVALID;
            Om_Ast_Node_Id binding_id = OM_AST_NODE_ID_INVALID;
            if (pattern != NULL && pattern->kind == OM_AST_CALL
                && pattern->child_count == 2) {
                constructor_id = infer_child(context, module, pattern, 0);
                binding_id = infer_child(context, module, pattern, 1);
                const Om_Ast_Node *constructor = infer_node(
                    context, module, constructor_id);
                const Om_Ast_Node *binding = infer_node(context, module, binding_id);
                if (constructor != NULL && constructor->kind == OM_AST_NAME
                    && binding != NULL && binding->kind == OM_AST_NAME
                    && (name_is(constructor->name, "Ok", 2)
                        || name_is(constructor->name, "Error", 5))) {
                    result_pattern = true;
                    is_ok_pattern = name_is(constructor->name, "Ok", 2);
                }
            }
            if (result_pattern) {
                Om_Type_Id value_type = OM_TYPE_ID_INVALID;
                Om_Type_Id error_type = OM_TYPE_ID_INVALID;
                Om_Type_Id result_type = OM_TYPE_ID_INVALID;
                status = om_type_store_variable(&context->type_store, level,
                                                &value_type);
                if (status == OM_STATUS_OK) {
                    status = om_type_store_variable(&context->type_store, level,
                                                    &error_type);
                }
                if (status == OM_STATUS_OK) {
                    status = om_type_store_result(&context->type_store,
                                                  value_type, error_type,
                                                  &result_type);
                }
                if (status == OM_STATUS_OK) {
                    status = unify_at(context, module, pattern->span,
                                      scrutinee_type, result_type);
                }
                Om_Type_Id payload_type = is_ok_pattern ? value_type : error_type;
                const Om_Symbol_Id binding_symbol = resolved_symbol_for_node(
                    context, module, binding_id);
                if (status == OM_STATUS_OK) {
                    status = set_symbol_monotype(context, binding_symbol,
                                                 payload_type);
                }
                Om_Type_Id constructor_function = OM_TYPE_ID_INVALID;
                if (status == OM_STATUS_OK) {
                    status = om_type_store_function(
                        &context->type_store, OM_TYPE_FUNCTION, &payload_type, 1,
                        result_type, &constructor_function);
                }
                if (status == OM_STATUS_OK) {
                    status = set_node_type(context, module, constructor_id,
                                           constructor_function);
                }
                if (status == OM_STATUS_OK) {
                    status = set_node_type(context, module, binding_id,
                                           payload_type);
                }
                if (status == OM_STATUS_OK) {
                    status = set_node_type(context, module, pattern_id,
                                           result_type);
                }
            } else {
                Om_Type_Id pattern_type = OM_TYPE_ID_INVALID;
                status = infer_expression(context, module, pattern_id, level,
                                          &pattern_type);
                if (status == OM_STATUS_OK) {
                    status = unify_at(context, module, pattern->span,
                                      scrutinee_type, pattern_type);
                }
            }
            if (status == OM_STATUS_OK) {
                status = infer_block(context, module,
                                     infer_child(context, module, arm, 1), level);
            }
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, arm_id);
        }
    }
    if (status == OM_STATUS_OK) {
        status = set_unit_node_type(context, module, node_id);
    }
    return status;
}

static Om_Status check_known_kind(Om_Infer_Context *context,
                                  Om_Module_Id module,
                                  Om_Source_Span span,
                                  Om_Type_Id type_id,
                                  Om_Type_Kind expected_kind)
{
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(&context->type_store, type_id,
                                             &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(&context->type_store, resolved, &view);
    }
    if (status == OM_STATUS_OK && view.kind != OM_TYPE_VARIABLE
        && view.kind != expected_kind) {
        status = add_type_diagnostic(context,
                                     OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                                     module, span, OM_TYPE_ID_INVALID,
                                     resolved);
    } else if (status == OM_STATUS_OK && view.kind == OM_TYPE_VARIABLE) {
        const uint32_t constraint = expected_kind == OM_TYPE_ACTOR_REF
            ? (uint32_t)OM_TYPE_CONSTRAINT_ACTOR_REF
            : expected_kind == OM_TYPE_MESSAGE
                ? (uint32_t)OM_TYPE_CONSTRAINT_MESSAGE : 0;
        if (constraint == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = require_constraint_at(context, module, span, type_id,
                                       constraint);
    }
    return status;
}

static Om_Status infer_statement(Om_Infer_Context *context,
                                 Om_Module_Id module,
                                 Om_Ast_Node_Id statement_id,
                                 uint32_t level)
{
    const Om_Ast_Node *node = infer_node(context, module, statement_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = OM_STATUS_OK;
    switch (node->kind) {
    case OM_AST_BINDING:
        return infer_binding(context, module, statement_id, node, level);
    case OM_AST_RETURN: {
        if (node->child_count > 1
            || context->return_type.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id value_type = context->type_store.primitive_types[OM_TYPE_UNIT];
        if (node->child_count == 1) {
            status = infer_expression(context, module,
                                      infer_child(context, module, node, 0),
                                      level, &value_type);
        }
        if (status == OM_STATUS_OK) {
            status = unify_at(context, module, node->span,
                              context->return_type, value_type);
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, statement_id);
        }
        return status;
    }
    case OM_AST_CALL_STATEMENT: {
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id ignored = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &ignored);
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, statement_id);
        }
        return status;
    }
    case OM_AST_SEND: {
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id receiver_type = OM_TYPE_ID_INVALID;
        Om_Type_Id message_type = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &receiver_type);
        if (status == OM_STATUS_OK) {
            status = infer_expression(context, module,
                                      infer_child(context, module, node, 1),
                                      level, &message_type);
        }
        if (status == OM_STATUS_OK) {
            const Om_Ast_Node *receiver = infer_node(
                context, module, infer_child(context, module, node, 0));
            const Om_Ast_Node *message = infer_node(
                context, module, infer_child(context, module, node, 1));
            status = check_known_kind(context, module,
                                      receiver != NULL ? receiver->span : node->span,
                                      receiver_type, OM_TYPE_ACTOR_REF);
            if (status == OM_STATUS_OK) {
                status = check_known_kind(context, module,
                                          message != NULL ? message->span : node->span,
                                          message_type, OM_TYPE_MESSAGE);
            }
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, statement_id);
        }
        return status;
    }
    case OM_AST_WATCH: {
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Type_Id actor_type = OM_TYPE_ID_INVALID;
        status = infer_expression(context, module,
                                  infer_child(context, module, node, 0),
                                  level, &actor_type);
        if (status == OM_STATUS_OK) {
            const Om_Ast_Node *actor = infer_node(
                context, module, infer_child(context, module, node, 0));
            status = check_known_kind(context, module,
                                      actor != NULL ? actor->span : node->span,
                                      actor_type, OM_TYPE_ACTOR_REF);
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, statement_id);
        }
        return status;
    }
    case OM_AST_IF:
        return infer_if(context, module, statement_id, node, level);
    case OM_AST_WHILE:
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = infer_condition(context, module,
                                 infer_child(context, module, node, 0), level);
        if (status == OM_STATUS_OK) {
            status = infer_block(context, module,
                                 infer_child(context, module, node, 1), level);
        }
        if (status == OM_STATUS_OK) {
            status = set_unit_node_type(context, module, statement_id);
        }
        return status;
    case OM_AST_FOR:
        return infer_for(context, module, statement_id, node, level);
    case OM_AST_SELECT:
        return infer_select(context, module, statement_id, node, level);
    case OM_AST_BREAK:
    case OM_AST_CONTINUE:
    case OM_AST_STOP:
        return set_unit_node_type(context, module, statement_id);
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status infer_callable(Om_Infer_Context *context,
                                Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = infer_symbol(context, symbol_id);
    if (symbol == NULL || (symbol->kind != OM_SYMBOL_FUNCTION
                           && symbol->kind != OM_SYMBOL_ACTION)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Infer_Symbol_State *state = &context->symbol_states[symbol_id.value];
    if (*state == OM_INFER_SYMBOL_IN_PROGRESS || *state == OM_INFER_SYMBOL_DONE) {
        return OM_STATUS_OK;
    }
    const Om_Type_Id function_type = type_for_symbol_scheme(context, symbol_id);
    if (function_type.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Ast_Node *declaration = infer_node(context, symbol->module_id,
                                                symbol->declaration_node);
    if (declaration == NULL
        || (declaration->kind != OM_AST_FUNCTION_DECLARATION
            && declaration->kind != OM_AST_ACTION_DECLARATION)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t parameter_count = declaration->auxiliary;
    if (declaration->child_count != parameter_count + 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_View signature = {0};
    Om_Status status = om_type_store_get(&context->type_store, function_type,
                                         &signature);
    if (status != OM_STATUS_OK
        || (signature.kind != OM_TYPE_FUNCTION && signature.kind != OM_TYPE_ACTION)
        || signature.child_count != parameter_count + 1) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < parameter_count; ++index) {
        const Om_Ast_Node_Id parameter_id = infer_child(context, symbol->module_id,
                                                        declaration, index);
        Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(&context->type_store, function_type,
                                        index, &parameter_type);
        if (status == OM_STATUS_OK) {
            status = set_node_type(context, symbol->module_id, parameter_id,
                                   parameter_type);
        }
    }
    Om_Type_Id return_type = OM_TYPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = om_type_store_child_at(&context->type_store, function_type,
                                        parameter_count, &return_type);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (context->callable_stack.length >= 256) {
        return OM_STATUS_OVERFLOW;
    }

    const Om_Type_Id previous_return_type = context->return_type;
    const bool previous_receive = context->in_actor_receive;
    const Om_Symbol_Id previous_actor = context->current_actor;
    context->return_type = return_type;
    context->in_actor_receive = false;
    context->current_actor = OM_SYMBOL_ID_INVALID;
    *state = OM_INFER_SYMBOL_IN_PROGRESS;
    status = om_vector_push(&context->callable_stack, &symbol_id);
    const bool stack_pushed = status == OM_STATUS_OK;
    const Om_Ast_Node_Id body_id = infer_child(context, symbol->module_id,
                                               declaration, parameter_count);
    if (status == OM_STATUS_OK) {
        status = infer_block(context, symbol->module_id, body_id, 1);
    }
    if (status == OM_STATUS_OK
        && !block_guarantees_return(context, symbol->module_id, body_id)) {
        status = unify_at(context, symbol->module_id, declaration->span,
                          return_type,
                          context->type_store.primitive_types[OM_TYPE_UNIT]);
    }
    context->return_type = previous_return_type;
    context->in_actor_receive = previous_receive;
    context->current_actor = previous_actor;
    if (status == OM_STATUS_OK) {
        *state = OM_INFER_SYMBOL_DONE;
        if (!context->recursive_callables[symbol_id.value]) {
            status = generalize_symbol(context, symbol_id, function_type, 0);
        }
    } else {
        *state = OM_INFER_SYMBOL_READY;
    }
    if (stack_pushed && context->callable_stack.length == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (stack_pushed) {
        const Om_Symbol_Id *active = om_vector_at_const(
            &context->callable_stack, context->callable_stack.length - 1);
        if (active == NULL || active->value != symbol_id.value) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        --context->callable_stack.length;
    }
    if (stack_pushed && status == OM_STATUS_OK
        && context->callable_stack.length == 0) {
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < context->resolution->symbol_count; ++index) {
            if (!context->recursive_callables[index]
                || context->symbol_states[index] != OM_INFER_SYMBOL_DONE) {
                continue;
            }
            const Om_Type_Id recursive_type = context->symbol_schemes[index].type_id;
            status = generalize_symbol(context, (Om_Symbol_Id){(uint32_t)index},
                                       recursive_type, 0);
            if (status == OM_STATUS_OK) {
                context->recursive_callables[index] = false;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = set_node_type(context, symbol->module_id,
                               symbol->declaration_node, function_type);
    } else {
        *state = OM_INFER_SYMBOL_READY;
    }
    return status;
}

static Om_Status infer_receive(Om_Infer_Context *context,
                               Om_Module_Id module,
                               Om_Ast_Node_Id receive_id,
                               const Om_Ast_Node *receive,
                               Om_Symbol_Id actor_symbol,
                               uint32_t level)
{
    const size_t parameter_count = receive->auxiliary;
    if (receive->child_count != parameter_count + 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Symbol_Id message_symbol = resolved_symbol_for_node(
        context, module, receive_id);
    const Om_Symbol *message = infer_symbol(context, message_symbol);
    if (message == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const bool actor_down = message->kind == OM_SYMBOL_BUILTIN
        && name_is_ci(message->name, "Actor_Down", 10);
    const bool system_message = message->kind == OM_SYMBOL_BUILTIN
        && system_message_name(message->name);
    size_t field_count = 0;
    const Om_Ast_Node *message_declaration = NULL;
    Om_Type_Id system_constructor_type = OM_TYPE_ID_INVALID;
    if (actor_down) {
        field_count = 2;
    } else if (system_message) {
        system_constructor_type = type_for_symbol_scheme(context, message_symbol);
        Om_Type_View constructor_view = {0};
        Om_Status status = om_type_store_get(&context->type_store,
                                             system_constructor_type,
                                             &constructor_view);
        if (status != OM_STATUS_OK
            || constructor_view.kind != OM_TYPE_FUNCTION
            || constructor_view.child_count == 0) {
            return status == OM_STATUS_OK
                ? OM_STATUS_INVALID_ARGUMENT : status;
        }
        field_count = constructor_view.child_count - 1;
    } else if (message->kind == OM_SYMBOL_MESSAGE) {
        message_declaration = infer_node(context, message->module_id,
                                         message->declaration_node);
        if (message_declaration == NULL
            || message_declaration->kind != OM_AST_MESSAGE_DECLARATION) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        field_count = message_declaration->child_count;
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (field_count != parameter_count) {
        Om_Status status = add_type_diagnostic(
            context, OM_TYPE_INFERENCE_DIAGNOSTIC_E0401, module,
            receive->span, type_for_symbol_scheme(context, message_symbol),
            OM_TYPE_ID_INVALID);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < parameter_count; ++index) {
        const Om_Ast_Node_Id parameter_id = infer_child(context, module,
                                                        receive, index);
        const Om_Symbol_Id parameter_symbol = resolved_symbol_for_node(
            context, module, parameter_id);
        Om_Type_Id parameter_type = OM_TYPE_ID_INVALID;
        if (index >= field_count) {
            status = om_type_store_variable(&context->type_store, level + 1,
                                            &parameter_type);
        } else if (actor_down) {
            const Om_Type_Kind builtin_kind = index == 0
                ? OM_TYPE_ACTOR_ID : OM_TYPE_DOWN_REASON;
            status = get_primitive_type(context, builtin_kind, &parameter_type);
        } else if (system_message) {
            status = om_type_store_child_at(&context->type_store,
                                            system_constructor_type,
                                            index, &parameter_type);
        } else {
            const Om_Ast_Node_Id field_id = infer_child(
                context, message->module_id, message_declaration, index);
            const Om_Symbol_Id field_symbol = resolved_symbol_for_node(
                context, message->module_id, field_id);
            parameter_type = type_for_symbol_scheme(context, field_symbol);
        }
        if (status == OM_STATUS_OK) {
            status = set_symbol_monotype(context, parameter_symbol,
                                         parameter_type);
        }
        if (status == OM_STATUS_OK) {
            status = set_node_type(context, module, parameter_id, parameter_type);
        }
    }
    Om_Type_Id message_type = type_for_symbol_scheme(context, message_symbol);
    if (actor_down || system_message) {
        status = om_type_store_nominal(&context->type_store, OM_TYPE_MESSAGE,
                                       message_symbol, &message_type);
    }
    if (status == OM_STATUS_OK) {
        status = set_node_type(context, module, receive_id, message_type);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }

    const Om_Type_Id previous_return_type = context->return_type;
    const bool previous_receive = context->in_actor_receive;
    const Om_Symbol_Id previous_actor = context->current_actor;
    context->return_type = context->type_store.primitive_types[OM_TYPE_UNIT];
    context->in_actor_receive = true;
    context->current_actor = actor_symbol;
    const Om_Ast_Node_Id body_id = infer_child(context, module, receive,
                                               parameter_count);
    status = infer_block(context, module, body_id, level + 1);
    context->return_type = previous_return_type;
    context->in_actor_receive = previous_receive;
    context->current_actor = previous_actor;
    if (status == OM_STATUS_OK) {
        status = set_unit_node_type(context, module, receive_id);
    }
    return status;
}

static Om_Status infer_actor(Om_Infer_Context *context,
                             Om_Module_Id module,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node,
                             uint32_t level)
{
    const Om_Symbol_Id actor_symbol = resolved_symbol_for_node(context, module,
                                                               node_id);
    const Om_Symbol *symbol = infer_symbol(context, actor_symbol);
    if (symbol == NULL || symbol->kind != OM_SYMBOL_ACTOR) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = set_node_type(context, module, node_id,
                                     type_for_symbol_scheme(context,
                                                            actor_symbol));
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count; ++index) {
        const Om_Ast_Node_Id member_id = infer_child(context, module, node, index);
        const Om_Ast_Node *member = infer_node(context, module, member_id);
        if (member == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (member->kind == OM_AST_BINDING) {
            status = infer_binding(context, module, member_id, member, level + 1);
        } else if (member->kind == OM_AST_RECEIVE_DECLARATION) {
            status = infer_receive(context, module, member_id, member,
                                   actor_symbol, level);
        } else {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    return status;
}

static bool type_contains_unresolved_variable(const Om_Infer_Context *context,
                                              Om_Type_Id type_id,
                                              size_t depth,
                                              bool *contains)
{
    if (depth >= 256 || contains == NULL) {
        return false;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    if (om_type_store_resolve(&context->type_store, type_id, &resolved)
        != OM_STATUS_OK) {
        return false;
    }
    Om_Type_View view = {0};
    if (om_type_store_get(&context->type_store, resolved, &view) != OM_STATUS_OK) {
        return false;
    }
    if (view.kind == OM_TYPE_VARIABLE) {
        *contains = view.type_variable_unbound;
        return true;
    }
    for (size_t index = 0; index < view.child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        bool child_contains = false;
        if (om_type_store_child_at(&context->type_store, resolved, index, &child)
                != OM_STATUS_OK
            || !type_contains_unresolved_variable(context, child, depth + 1,
                                                  &child_contains)) {
            return false;
        }
        if (child_contains) {
            *contains = true;
            return true;
        }
    }
    *contains = false;
    return true;
}

static Om_Status diagnose_unresolved_empty_collections(Om_Infer_Context *context)
{
    for (size_t index = 0; index < context->empty_collections.length; ++index) {
        const Om_Empty_Collection *entry = om_vector_at_const(
            &context->empty_collections, index);
        bool unresolved = false;
        if (!type_contains_unresolved_variable(context, entry->type_id, 0,
                                               &unresolved)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (unresolved) {
            const Om_Ast_Node *node = infer_node(context, entry->module_id,
                                                 entry->node_id);
            if (node == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            const Om_Status status = add_type_diagnostic(
                context, OM_TYPE_INFERENCE_DIAGNOSTIC_E0402,
                entry->module_id, node->span,
                OM_TYPE_ID_INVALID, entry->type_id);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

static Om_Status diagnose_unstable_exported_fields(Om_Infer_Context *context)
{
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *field = &context->resolution->symbols[index];
        if (field->kind != OM_SYMBOL_FIELD
            || field->scope_id.value >= context->resolution->scope_count) {
            continue;
        }
        const Om_Scope *scope = &context->resolution->scopes[field->scope_id.value];
        const Om_Symbol_Id owner_id = resolved_symbol_for_node(
            context, scope->module_id, scope->owner_node);
        const Om_Symbol *owner = infer_symbol(context, owner_id);
        if (owner == NULL || !owner->exported
            || (owner->kind != OM_SYMBOL_RECORD
                && owner->kind != OM_SYMBOL_MESSAGE)) {
            continue;
        }
        bool unresolved = false;
        if (!type_contains_unresolved_variable(
                context, type_for_symbol_scheme(context, field->id), 0,
                &unresolved)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (unresolved) {
            const Om_Status status = add_type_diagnostic(
                context, OM_TYPE_INFERENCE_DIAGNOSTIC_E0401,
                field->module_id, field->span,
                OM_TYPE_ID_INVALID, type_for_symbol_scheme(context, field->id));
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

static void destroy_infer_context(Om_Infer_Context *context)
{
    if (context->symbol_schemes != NULL) {
        for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
            om_type_scheme_destroy(&context->symbol_schemes[index]);
        }
    }
    om_free(context->symbol_schemes);
    om_free(context->symbol_states);
    om_free(context->recursive_callables);
    om_vector_destroy(&context->inferred_nodes);
    om_vector_destroy(&context->diagnostics);
    om_vector_destroy(&context->empty_collections);
    om_vector_destroy(&context->callable_stack);
    om_type_store_destroy(&context->type_store);
}

static void reset_type_inference_result(Om_Type_Inference_Result *result)
{
    for (size_t index = 0; index < OM_TYPE_PRIMITIVE_COUNT; ++index) {
        result->type_store.primitive_types[index] = OM_TYPE_ID_INVALID;
    }
    result->type_store.owned_storage = NULL;
    result->inferred_nodes = NULL;
    result->inferred_node_count = 0;
    result->symbol_schemes = NULL;
    result->symbol_scheme_count = 0;
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->owned_inferred_nodes = NULL;
    result->owned_symbol_schemes = NULL;
    result->owned_diagnostics = NULL;
}

Om_Status om_type_infer_project(const Om_Module_Project *project,
                                const Om_Parser_Result *parsed_modules,
                                size_t parsed_module_count,
                                const Om_Resolution_Result *resolution,
                                Om_Type_Inference_Result *result)
{
    if (project == NULL || parsed_modules == NULL || resolution == NULL
        || result == NULL
        || parsed_module_count != om_module_project_module_count(project)
        || resolution->diagnostic_count != 0
        || (resolution->symbol_count != 0 && resolution->symbols == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    reset_type_inference_result(result);
    for (size_t index = 0; index < parsed_module_count; ++index) {
        if (parsed_modules[index].diagnostic_count != 0
            || parsed_modules[index].ast.root.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    Om_Infer_Context context = {
        .project = project,
        .parsed_modules = parsed_modules,
        .resolution = resolution,
        .module_count = parsed_module_count,
        .return_type = OM_TYPE_ID_INVALID,
        .current_actor = OM_SYMBOL_ID_INVALID,
    };
    Om_Status status = om_type_store_init(&context.type_store);
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.inferred_nodes,
                                sizeof(Om_Inferred_Node));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.diagnostics,
                                sizeof(Om_Type_Inference_Diagnostic));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.empty_collections,
                                sizeof(Om_Empty_Collection));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.callable_stack, sizeof(Om_Symbol_Id));
    }
    context.symbol_schemes = om_alloc_array(resolution->symbol_count,
                                            sizeof(*context.symbol_schemes));
    context.symbol_states = om_alloc_array(resolution->symbol_count,
                                           sizeof(*context.symbol_states));
    context.recursive_callables = om_alloc_array(
        resolution->symbol_count, sizeof(*context.recursive_callables));
    if (context.symbol_schemes != NULL) {
        for (size_t index = 0; index < resolution->symbol_count; ++index) {
            context.symbol_schemes[index] = (Om_Type_Scheme){
                .type_id = OM_TYPE_ID_INVALID,
            };
        }
    }
    if (context.symbol_states != NULL) {
        for (size_t index = 0; index < resolution->symbol_count; ++index) {
            context.symbol_states[index] = OM_INFER_SYMBOL_UNINITIALIZED;
        }
    }
    if (context.recursive_callables != NULL) {
        for (size_t index = 0; index < resolution->symbol_count; ++index) {
            context.recursive_callables[index] = false;
        }
    }
    if (status == OM_STATUS_OK && resolution->symbol_count != 0
        && (context.symbol_schemes == NULL || context.symbol_states == NULL
            || context.recursive_callables == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        status = initialize_symbols(&context);
    }
    for (size_t module_index = 0; status == OM_STATUS_OK
                                 && module_index < parsed_module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = infer_module_ast(&context, module);
        const Om_Ast_Node *program = ast != NULL
            ? om_ast_get_node(ast, ast->root) : NULL;
        if (program == NULL || program->kind != OM_AST_PROGRAM) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        for (size_t child_index = 0; status == OM_STATUS_OK
                                   && child_index < program->child_count;
             ++child_index) {
            const Om_Ast_Node_Id child_id = infer_child(&context, module,
                                                       program, child_index);
            const Om_Ast_Node *declaration = infer_node(&context, module,
                                                        child_id);
            if (declaration == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (declaration->kind == OM_AST_FUNCTION_DECLARATION
                       || declaration->kind == OM_AST_ACTION_DECLARATION) {
                const Om_Symbol_Id symbol = resolved_symbol_for_node(
                    &context, module, child_id);
                status = infer_callable(&context, symbol);
            } else if (declaration->kind == OM_AST_ACTOR_DECLARATION) {
                status = infer_actor(&context, module, child_id, declaration, 0);
            } else if (declaration->kind == OM_AST_RECORD_DECLARATION
                       || declaration->kind == OM_AST_MESSAGE_DECLARATION) {
                const Om_Symbol_Id symbol = resolved_symbol_for_node(
                    &context, module, child_id);
                status = set_node_type(&context, module, child_id,
                                       type_for_symbol_scheme(&context, symbol));
                for (size_t field_index = 0; status == OM_STATUS_OK
                                          && field_index < declaration->child_count;
                     ++field_index) {
                    const Om_Ast_Node_Id field_id = infer_child(
                        &context, module, declaration, field_index);
                    const Om_Symbol_Id field_symbol = resolved_symbol_for_node(
                        &context, module, field_id);
                    status = set_node_type(&context, module, field_id,
                                           type_for_symbol_scheme(&context,
                                                                  field_symbol));
                }
            } else if (declaration->kind != OM_AST_IMPORT) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = diagnose_unresolved_empty_collections(&context);
    }
    if (status == OM_STATUS_OK) {
        status = diagnose_unstable_exported_fields(&context);
    }
    if (status != OM_STATUS_OK) {
        destroy_infer_context(&context);
        return status;
    }

    result->type_store = context.type_store;
    context.type_store = (Om_Type_Store){0};
    result->inferred_nodes = context.inferred_nodes.data;
    result->inferred_node_count = context.inferred_nodes.length;
    result->owned_inferred_nodes = context.inferred_nodes.data;
    context.inferred_nodes = (Om_Vector){0};
    result->symbol_schemes = context.symbol_schemes;
    result->symbol_scheme_count = resolution->symbol_count;
    result->owned_symbol_schemes = context.symbol_schemes;
    context.symbol_schemes = NULL;
    result->diagnostics = context.diagnostics.data;
    result->diagnostic_count = context.diagnostics.length;
    result->owned_diagnostics = context.diagnostics.data;
    context.diagnostics = (Om_Vector){0};
    om_free(context.symbol_states);
    context.symbol_states = NULL;
    om_free(context.recursive_callables);
    context.recursive_callables = NULL;
    om_vector_destroy(&context.empty_collections);
    om_vector_destroy(&context.callable_stack);
    return OM_STATUS_OK;
}

void om_type_inference_result_destroy(Om_Type_Inference_Result *result)
{
    if (result == NULL) {
        return;
    }
    Om_Type_Scheme *schemes = result->owned_symbol_schemes;
    for (size_t index = 0; schemes != NULL
                           && index < result->symbol_scheme_count; ++index) {
        om_type_scheme_destroy(&schemes[index]);
    }
    om_free(result->owned_inferred_nodes);
    om_free(result->owned_symbol_schemes);
    om_free(result->owned_diagnostics);
    om_type_store_destroy(&result->type_store);
    reset_type_inference_result(result);
}

Om_Type_Id om_type_inference_type_for_node(const Om_Type_Inference_Result *result,
                                           Om_Module_Id module_id,
                                           Om_Ast_Node_Id node_id)
{
    if (result == NULL) {
        return OM_TYPE_ID_INVALID;
    }
    for (size_t index = 0; index < result->inferred_node_count; ++index) {
        const Om_Inferred_Node *node = &result->inferred_nodes[index];
        if (node->module_id.value == module_id.value
            && node->node_id.value == node_id.value) {
            return node->type_id;
        }
    }
    return OM_TYPE_ID_INVALID;
}

const Om_Type_Scheme *om_type_inference_scheme_for_symbol(
    const Om_Type_Inference_Result *result,
    Om_Symbol_Id symbol_id)
{
    if (result == NULL || symbol_id.value >= result->symbol_scheme_count
        || result->symbol_schemes == NULL
        || result->symbol_schemes[symbol_id.value].type_id.value == UINT32_MAX) {
        return NULL;
    }
    return &result->symbol_schemes[symbol_id.value];
}

const char *om_type_inference_diagnostic_code_name(
    Om_Type_Inference_Diagnostic_Code code)
{
    switch (code) {
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0401: return "E0401";
    case OM_TYPE_INFERENCE_DIAGNOSTIC_E0402: return "E0402";
    default: return "E0000";
    }
}
