#include "ombasic/semantic/semantic_analysis.h"

#include "ombasic/foundation/memory.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "semantic_analysis_internal.h"

const Om_Ast *om_semantic_module_ast(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id)
{
    if (context == NULL || module_id.value >= context->module_count) {
        return NULL;
    }
    return &context->parsed_modules[module_id.value].ast;
}

const Om_Ast_Node *om_semantic_node(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id)
{
    return om_ast_get_node(om_semantic_module_ast(context, module_id), node_id);
}

Om_Ast_Node_Id om_semantic_child(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    const Om_Ast_Node *node,
    size_t index)
{
    return om_ast_get_child(om_semantic_module_ast(context, module_id), node,
                            index);
}

const Om_Symbol *om_semantic_symbol(
    const Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id symbol_id)
{
    if (context == NULL || symbol_id.value >= context->resolution->symbol_count) {
        return NULL;
    }
    return &context->resolution->symbols[symbol_id.value];
}

Om_Symbol_Id om_semantic_resolved_symbol(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id)
{
    if (context == NULL) {
        return OM_SYMBOL_ID_INVALID;
    }
    return om_resolution_symbol_for_node(context->resolution, module_id, node_id);
}

Om_Status om_semantic_add_diagnostic(
    Om_Semantic_Analysis_Context *context,
    Om_Semantic_Analysis_Diagnostic_Code code,
    Om_Module_Id module_id,
    Om_Source_Span span,
    Om_Symbol_Id symbol_id)
{
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < context->diagnostics.length; ++index) {
        const Om_Semantic_Analysis_Diagnostic *existing = om_vector_at_const(
            &context->diagnostics, index);
        if (existing->code == code
            && existing->module_id.value == module_id.value
            && existing->span.source.value == span.source.value
            && existing->span.start == span.start
            && existing->span.end == span.end
            && existing->symbol_id.value == symbol_id.value) {
            return OM_STATUS_OK;
        }
    }
    const Om_Semantic_Analysis_Diagnostic diagnostic = {
        .code = code,
        .module_id = module_id,
        .span = span,
        .symbol_id = symbol_id,
    };
    return om_vector_push(&context->diagnostics, &diagnostic);
}

Om_Status om_semantic_set_node_effect(
    Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id,
    uint32_t effects)
{
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < context->node_effects.length; ++index) {
        Om_Node_Effect *existing = om_vector_at(&context->node_effects, index);
        if (existing->module_id.value == module_id.value
            && existing->node_id.value == node_id.value) {
            existing->effects |= effects;
            return OM_STATUS_OK;
        }
    }
    const Om_Node_Effect effect = {module_id, node_id, effects};
    return om_vector_push(&context->node_effects, &effect);
}

Om_Status om_semantic_set_symbol_effect(
    Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id symbol_id,
    uint32_t effects)
{
    if (context == NULL || symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < context->symbol_effects.length; ++index) {
        Om_Symbol_Effect *existing = om_vector_at(&context->symbol_effects, index);
        if (existing->symbol_id.value == symbol_id.value) {
            existing->effects |= effects;
            return OM_STATUS_OK;
        }
    }
    const Om_Symbol_Effect effect = {symbol_id, effects};
    return om_vector_push(&context->symbol_effects, &effect);
}

static void destroy_context(Om_Semantic_Analysis_Context *context)
{
    om_vector_destroy(&context->diagnostics);
    om_vector_destroy(&context->symbol_effects);
    om_vector_destroy(&context->node_effects);
    om_vector_destroy(&context->actor_protocol);
    om_vector_destroy(&context->serializable_types);
}

Om_Status om_semantic_analyze_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    Om_Semantic_Analysis_Result *result)
{
    if (project == NULL || parsed_modules == NULL || resolution == NULL
        || inference == NULL || result == NULL
        || parsed_module_count != om_module_project_module_count(project)
        || resolution->diagnostic_count != 0
        || inference->diagnostic_count != 0
        || inference->symbol_scheme_count != resolution->symbol_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < parsed_module_count; ++index) {
        if (parsed_modules[index].diagnostic_count != 0
            || parsed_modules[index].ast.root.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    *result = (Om_Semantic_Analysis_Result){0};
    Om_Semantic_Analysis_Context context = {
        .project = project,
        .parsed_modules = parsed_modules,
        .module_count = parsed_module_count,
        .resolution = resolution,
        .inference = inference,
    };
    Om_Status status = om_vector_init(
        &context.diagnostics, sizeof(Om_Semantic_Analysis_Diagnostic));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.symbol_effects,
                                sizeof(Om_Symbol_Effect));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.node_effects, sizeof(Om_Node_Effect));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.actor_protocol,
                                sizeof(Om_Actor_Protocol_Entry));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&context.serializable_types,
                                sizeof(Om_Type_Serializability));
    }
    if (status == OM_STATUS_OK) {
        status = om_effect_analyze_internal(&context);
    }
    if (status == OM_STATUS_OK) {
        status = om_actor_protocol_check_internal(&context);
    }
    if (status == OM_STATUS_OK) {
        status = om_serializable_check_internal(&context);
    }
    if (status != OM_STATUS_OK) {
        destroy_context(&context);
        return status;
    }

    result->diagnostics = context.diagnostics.data;
    result->diagnostic_count = context.diagnostics.length;
    result->owned_diagnostics = context.diagnostics.data;
    context.diagnostics = (Om_Vector){0};
    result->symbol_effects = context.symbol_effects.data;
    result->symbol_effect_count = context.symbol_effects.length;
    result->owned_symbol_effects = context.symbol_effects.data;
    context.symbol_effects = (Om_Vector){0};
    result->node_effects = context.node_effects.data;
    result->node_effect_count = context.node_effects.length;
    result->owned_node_effects = context.node_effects.data;
    context.node_effects = (Om_Vector){0};
    result->actor_protocol = context.actor_protocol.data;
    result->actor_protocol_count = context.actor_protocol.length;
    result->owned_actor_protocol = context.actor_protocol.data;
    context.actor_protocol = (Om_Vector){0};
    result->serializable_types = context.serializable_types.data;
    result->serializable_type_count = context.serializable_types.length;
    result->owned_serializable_types = context.serializable_types.data;
    context.serializable_types = (Om_Vector){0};
    destroy_context(&context);
    return OM_STATUS_OK;
}

void om_semantic_analysis_result_destroy(Om_Semantic_Analysis_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->owned_diagnostics);
    om_free(result->owned_symbol_effects);
    om_free(result->owned_node_effects);
    om_free(result->owned_actor_protocol);
    om_free(result->owned_serializable_types);
    *result = (Om_Semantic_Analysis_Result){0};
}

uint32_t om_semantic_effect_for_symbol(
    const Om_Semantic_Analysis_Result *result,
    Om_Symbol_Id symbol_id)
{
    if (result == NULL) {
        return (uint32_t)OM_EFFECT_NONE;
    }
    for (size_t index = 0; index < result->symbol_effect_count; ++index) {
        if (result->symbol_effects[index].symbol_id.value == symbol_id.value) {
            return result->symbol_effects[index].effects;
        }
    }
    return (uint32_t)OM_EFFECT_NONE;
}

uint32_t om_semantic_effect_for_node(
    const Om_Semantic_Analysis_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id)
{
    if (result == NULL) {
        return (uint32_t)OM_EFFECT_NONE;
    }
    for (size_t index = 0; index < result->node_effect_count; ++index) {
        const Om_Node_Effect *effect = &result->node_effects[index];
        if (effect->module_id.value == module_id.value
            && effect->node_id.value == node_id.value) {
            return effect->effects;
        }
    }
    return (uint32_t)OM_EFFECT_NONE;
}

bool om_semantic_actor_accepts(
    const Om_Semantic_Analysis_Result *result,
    Om_Symbol_Id actor_symbol,
    Om_Symbol_Id message_symbol)
{
    if (result == NULL) {
        return false;
    }
    for (size_t index = 0; index < result->actor_protocol_count; ++index) {
        const Om_Actor_Protocol_Entry *entry = &result->actor_protocol[index];
        if (entry->actor_symbol.value == actor_symbol.value
            && entry->message_symbol.value == message_symbol.value) {
            return true;
        }
    }
    return false;
}

bool om_semantic_type_is_serializable(
    const Om_Semantic_Analysis_Result *result,
    Om_Type_Id type_id,
    bool *serializable)
{
    if (result == NULL || serializable == NULL) {
        return false;
    }
    for (size_t index = 0; index < result->serializable_type_count; ++index) {
        if (result->serializable_types[index].type_id.value == type_id.value) {
            *serializable = result->serializable_types[index].serializable;
            return true;
        }
    }
    return false;
}

const char *om_semantic_analysis_diagnostic_code_name(
    Om_Semantic_Analysis_Diagnostic_Code code)
{
    switch (code) {
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501: return "E0501";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601: return "E0601";
    case OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701: return "E0701";
    default: return "E0000";
    }
}
