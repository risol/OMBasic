#include "../semantic_model/semantic_analysis_internal.h"

#include "ombasic/foundation/memory.h"

typedef struct Om_Call_Edge {
    Om_Symbol_Id caller;
    Om_Symbol_Id callee;
    Om_Module_Id module_id;
    Om_Ast_Node_Id call_node;
} Om_Call_Edge;

typedef struct Om_Effect_Scan {
    Om_Semantic_Analysis_Context *context;
    Om_Vector edges;
    uint32_t *symbol_effects;
} Om_Effect_Scan;

static bool is_pure_builtin(Om_Builtin_Id id)
{
    return id == OM_BUILTIN_OK || id == OM_BUILTIN_ERROR
        || id == OM_BUILTIN_ACTOR_DOWN;
}

static uint32_t native_effect_for_builtin(const Om_Symbol *symbol)
{
    if (symbol == NULL || symbol->native_module == NULL
        || symbol->native_function_index
            >= symbol->native_module->function_count
        || symbol->native_module->functions == NULL) {
        return (uint32_t)OM_EFFECT_NATIVE;
    }
    const uint32_t native_effects = symbol->native_module->functions[
        symbol->native_function_index].effects;
    uint32_t effects = (uint32_t)OM_EFFECT_NONE;
    if ((native_effects & (uint32_t)OM_NATIVE_EFFECT_NATIVE) != 0) {
        effects |= (uint32_t)OM_EFFECT_NATIVE;
    }
    if ((native_effects & (uint32_t)OM_NATIVE_EFFECT_FILE_IO) != 0) {
        effects |= (uint32_t)OM_EFFECT_FILE_IO;
    }
    if ((native_effects & (uint32_t)OM_NATIVE_EFFECT_NETWORK_IO) != 0) {
        effects |= (uint32_t)OM_EFFECT_NETWORK_IO;
    }
    if ((native_effects & (uint32_t)OM_NATIVE_EFFECT_CLOCK) != 0) {
        effects |= (uint32_t)OM_EFFECT_CLOCK;
    }
    return effects;
}

static uint32_t builtin_effect(const Om_Symbol *symbol)
{
    if (symbol != NULL && symbol->native_module != NULL) {
        return native_effect_for_builtin(symbol);
    }
    return symbol != NULL && !is_pure_builtin(symbol->builtin_id)
        ? (uint32_t)OM_EFFECT_NATIVE : (uint32_t)OM_EFFECT_NONE;
}

static bool symbol_is_actor_state(const Om_Effect_Scan *scan,
                                  const Om_Symbol *symbol)
{
    if (symbol == NULL || symbol->kind != OM_SYMBOL_MUTABLE_BINDING
        || symbol->scope_id.value >= scan->context->resolution->scope_count) {
        return false;
    }
    const Om_Scope *scope = &scan->context->resolution->scopes[
        symbol->scope_id.value];
    return scope->kind == OM_SCOPE_ACTOR;
}

static Om_Status add_call_edge(Om_Effect_Scan *scan,
                               Om_Symbol_Id caller,
                               Om_Module_Id module,
                               Om_Ast_Node_Id call_node,
                               Om_Symbol_Id callee)
{
    const Om_Call_Edge edge = {
        .caller = caller,
        .callee = callee,
        .module_id = module,
        .call_node = call_node,
    };
    return om_vector_push(&scan->edges, &edge);
}

static Om_Status scan_callable_tree(Om_Effect_Scan *scan,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id,
                                    Om_Symbol_Id caller,
                                    bool in_receive,
                                    size_t depth)
{
    if (depth >= 256) {
        return OM_STATUS_OVERFLOW;
    }
    const Om_Ast_Node *node = om_semantic_node(scan->context, module, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t direct_effect = (uint32_t)OM_EFFECT_NONE;
    if (node->kind == OM_AST_SEND) {
        direct_effect |= (uint32_t)OM_EFFECT_SEND_MESSAGE;
    } else if (node->kind == OM_AST_SPAWN) {
        direct_effect |= (uint32_t)OM_EFFECT_SPAWN_ACTOR;
    } else if (node->kind == OM_AST_WATCH) {
        direct_effect |= (uint32_t)OM_EFFECT_WATCH;
        if (!in_receive) {
            const Om_Status status = om_semantic_add_diagnostic(
                scan->context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                module, node->span, caller);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    } else if (node->kind == OM_AST_STOP) {
        direct_effect |= (uint32_t)OM_EFFECT_STOP;
        if (!in_receive) {
            const Om_Status status = om_semantic_add_diagnostic(
                scan->context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                module, node->span, caller);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    } else if (node->kind == OM_AST_NAME) {
        const Om_Symbol_Id binding_id = om_semantic_resolved_symbol(
            scan->context, module, node_id);
        const Om_Symbol *binding = om_semantic_symbol(scan->context, binding_id);
        if (symbol_is_actor_state(scan, binding)) {
            direct_effect |= (uint32_t)OM_EFFECT_READS_ACTOR_STATE;
        }
    } else if (node->kind == OM_AST_BINDING) {
        const Om_Symbol_Id binding_id = om_semantic_resolved_symbol(
            scan->context, module, node_id);
        const Om_Symbol *binding = om_semantic_symbol(scan->context, binding_id);
        if (binding != NULL && binding->kind == OM_SYMBOL_MUTABLE_BINDING) {
            if (symbol_is_actor_state(scan, binding)
                && binding->declaration_node.value != node_id.value) {
                direct_effect |= (uint32_t)OM_EFFECT_MUTATES_ACTOR_STATE;
            } else if (!symbol_is_actor_state(scan, binding)) {
                direct_effect |= (uint32_t)OM_EFFECT_MUTATES_LOCAL;
            }
        }
    } else if (node->kind == OM_AST_CALL && node->child_count != 0) {
        const Om_Ast_Node_Id callee_node = om_semantic_child(
            scan->context, module, node, 0);
        const Om_Symbol_Id callee_id = om_semantic_resolved_symbol(
            scan->context, module, callee_node);
        const Om_Symbol *callee = om_semantic_symbol(scan->context, callee_id);
        if (callee != NULL && (callee->kind == OM_SYMBOL_FUNCTION
                               || callee->kind == OM_SYMBOL_ACTION)) {
            Om_Status status = add_call_edge(scan, caller, module, node_id,
                                             callee_id);
            if (status != OM_STATUS_OK) {
                return status;
            }
            if (caller.value < scan->context->resolution->symbol_count
                && callee->kind == OM_SYMBOL_ACTION) {
                scan->symbol_effects[caller.value] |=
                    (uint32_t)OM_EFFECT_CALL_ACTION;
                direct_effect |= (uint32_t)OM_EFFECT_CALL_ACTION;
            }
        } else if (callee != NULL && callee->kind == OM_SYMBOL_BUILTIN) {
            const uint32_t native_effects = builtin_effect(callee);
            direct_effect |= native_effects;
            if (caller.value < scan->context->resolution->symbol_count) {
                scan->symbol_effects[caller.value] |= native_effects;
            }
        }
    }
    if (caller.value < scan->context->resolution->symbol_count) {
        scan->symbol_effects[caller.value] |= direct_effect;
    }
    Om_Status status = direct_effect != (uint32_t)OM_EFFECT_NONE
        ? om_semantic_set_node_effect(scan->context, module, node_id,
                                      direct_effect)
        : OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < node->child_count; ++index) {
        status = scan_callable_tree(
            scan, module, om_semantic_child(scan->context, module, node, index),
            caller, in_receive, depth + 1);
    }
    return status;
}

static uint32_t direct_node_effect(const Om_Effect_Scan *scan,
                                   Om_Module_Id module,
                                   Om_Ast_Node_Id node_id,
                                   const Om_Ast_Node *node,
                                   bool in_receive)
{
    uint32_t effects = (uint32_t)OM_EFFECT_NONE;
    if (node->kind == OM_AST_SEND) {
        effects |= (uint32_t)OM_EFFECT_SEND_MESSAGE;
    } else if (node->kind == OM_AST_SPAWN) {
        effects |= (uint32_t)OM_EFFECT_SPAWN_ACTOR;
    } else if (node->kind == OM_AST_WATCH) {
        effects |= (uint32_t)OM_EFFECT_WATCH;
    } else if (node->kind == OM_AST_STOP) {
        effects |= (uint32_t)OM_EFFECT_STOP;
    } else if (node->kind == OM_AST_NAME) {
        const Om_Symbol_Id binding_id = om_semantic_resolved_symbol(
            scan->context, module, node_id);
        const Om_Symbol *binding = om_semantic_symbol(scan->context, binding_id);
        if (symbol_is_actor_state(scan, binding)) {
            effects |= (uint32_t)OM_EFFECT_READS_ACTOR_STATE;
        }
    } else if (node->kind == OM_AST_BINDING) {
        const Om_Symbol_Id binding_id = om_semantic_resolved_symbol(
            scan->context, module, node_id);
        const Om_Symbol *binding = om_semantic_symbol(scan->context, binding_id);
        if (binding != NULL && binding->kind == OM_SYMBOL_MUTABLE_BINDING) {
            if (symbol_is_actor_state(scan, binding)
                && binding->declaration_node.value != node_id.value) {
                effects |= (uint32_t)OM_EFFECT_MUTATES_ACTOR_STATE;
            } else if (!symbol_is_actor_state(scan, binding)) {
                effects |= (uint32_t)OM_EFFECT_MUTATES_LOCAL;
            }
        }
    } else if (node->kind == OM_AST_CALL && node->child_count != 0) {
        const Om_Symbol_Id callee_id = om_semantic_resolved_symbol(
            scan->context, module,
            om_semantic_child(scan->context, module, node, 0));
        const Om_Symbol *callee = om_semantic_symbol(scan->context, callee_id);
        if (callee != NULL && (callee->kind == OM_SYMBOL_FUNCTION
                               || callee->kind == OM_SYMBOL_ACTION)) {
            effects |= scan->symbol_effects[callee_id.value];
            if (callee->kind == OM_SYMBOL_ACTION) {
                effects |= (uint32_t)OM_EFFECT_CALL_ACTION;
            }
        } else if (callee != NULL && callee->kind == OM_SYMBOL_BUILTIN) {
            effects |= builtin_effect(callee);
        }
    }
    if ((node->kind == OM_AST_WATCH || node->kind == OM_AST_STOP)
        && !in_receive) {
        effects |= (uint32_t)OM_EFFECT_CALL_ACTION;
    }
    return effects;
}

static Om_Status compute_tree_effects(Om_Effect_Scan *scan,
                                      Om_Module_Id module,
                                      Om_Ast_Node_Id node_id,
                                      bool in_receive,
                                      size_t depth,
                                      uint32_t *effects_out)
{
    if (depth >= 256 || effects_out == NULL) {
        return OM_STATUS_OVERFLOW;
    }
    const Om_Ast_Node *node = om_semantic_node(scan->context, module, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t effects = direct_node_effect(scan, module, node_id, node,
                                          in_receive);
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < node->child_count; ++index) {
        uint32_t child_effects = (uint32_t)OM_EFFECT_NONE;
        status = compute_tree_effects(
            scan, module, om_semantic_child(scan->context, module, node, index),
            in_receive, depth + 1, &child_effects);
        effects |= child_effects;
    }
    if (status == OM_STATUS_OK && effects != (uint32_t)OM_EFFECT_NONE) {
        status = om_semantic_set_node_effect(scan->context, module, node_id,
                                             effects);
    }
    if (status == OM_STATUS_OK) {
        *effects_out = effects;
    }
    return status;
}

static Om_Status scan_project_declarations(Om_Effect_Scan *scan)
{
    for (size_t module_index = 0; module_index < scan->context->module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = om_semantic_module_ast(scan->context, module);
        const Om_Ast_Node *program = ast != NULL
            ? om_ast_get_node(ast, ast->root) : NULL;
        if (program == NULL || program->kind != OM_AST_PROGRAM) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t index = 0; index < program->child_count; ++index) {
            const Om_Ast_Node_Id declaration_id = om_semantic_child(
                scan->context, module, program, index);
            const Om_Ast_Node *declaration = om_semantic_node(
                scan->context, module, declaration_id);
            if (declaration == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            if (declaration->kind == OM_AST_FUNCTION_DECLARATION
                || declaration->kind == OM_AST_ACTION_DECLARATION) {
                const Om_Symbol_Id symbol = om_semantic_resolved_symbol(
                    scan->context, module, declaration_id);
                const size_t body_index = declaration->auxiliary;
                if (declaration->child_count != body_index + 1) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                const Om_Status status = scan_callable_tree(
                    scan, module,
                    om_semantic_child(scan->context, module, declaration,
                                      body_index),
                    symbol, false, 0);
                if (status != OM_STATUS_OK) {
                    return status;
                }
            } else if (declaration->kind == OM_AST_ACTOR_DECLARATION) {
                for (size_t member_index = 0;
                     member_index < declaration->child_count; ++member_index) {
                    const Om_Ast_Node_Id member_id = om_semantic_child(
                        scan->context, module, declaration, member_index);
                    const Om_Ast_Node *member = om_semantic_node(
                        scan->context, module, member_id);
                    if (member == NULL) {
                        return OM_STATUS_INVALID_ARGUMENT;
                    }
                    Om_Status status = OM_STATUS_OK;
                    if (member->kind == OM_AST_BINDING
                        && member->child_count == 1) {
                        status = scan_callable_tree(
                            scan, module,
                            om_semantic_child(scan->context, module, member, 0),
                            OM_SYMBOL_ID_INVALID, false, 0);
                    } else if (member->kind == OM_AST_RECEIVE_DECLARATION) {
                        const size_t body_index = member->auxiliary;
                        if (member->child_count != body_index + 1) {
                            return OM_STATUS_INVALID_ARGUMENT;
                        }
                        status = scan_callable_tree(
                            scan, module,
                            om_semantic_child(scan->context, module, member,
                                              body_index),
                            OM_SYMBOL_ID_INVALID, true, 0);
                    }
                    if (status != OM_STATUS_OK) {
                        return status;
                    }
                }
            }
        }
    }
    return OM_STATUS_OK;
}

static Om_Status propagate_call_effects(Om_Effect_Scan *scan)
{
    bool changed = true;
    size_t iterations = 0;
    const size_t maximum_iterations = scan->context->resolution->symbol_count
                                     + 1;
    while (changed && iterations++ < maximum_iterations) {
        changed = false;
        for (size_t index = 0; index < scan->edges.length; ++index) {
            const Om_Call_Edge *edge = om_vector_at_const(&scan->edges, index);
            if (edge->caller.value == UINT32_MAX
                || edge->caller.value >= scan->context->resolution->symbol_count
                || edge->callee.value >= scan->context->resolution->symbol_count) {
                continue;
            }
            const Om_Symbol *callee = om_semantic_symbol(scan->context,
                                                         edge->callee);
            uint32_t callee_effects = scan->symbol_effects[edge->callee.value];
            if (callee != NULL && callee->kind == OM_SYMBOL_ACTION) {
                callee_effects |= (uint32_t)OM_EFFECT_CALL_ACTION;
            }
            const uint32_t old_effects = scan->symbol_effects[edge->caller.value];
            const uint32_t new_effects = old_effects | callee_effects;
            if (new_effects != old_effects) {
                scan->symbol_effects[edge->caller.value] = new_effects;
                changed = true;
            }
        }
    }
    if (changed) {
        return OM_STATUS_OVERFLOW;
    }
    return OM_STATUS_OK;
}

static Om_Status publish_effect_summaries(Om_Effect_Scan *scan)
{
    const uint32_t forbidden_function_effects =
        (uint32_t)OM_EFFECT_MUTATES_ACTOR_STATE
        | (uint32_t)OM_EFFECT_SEND_MESSAGE
        | (uint32_t)OM_EFFECT_SPAWN_ACTOR
        | (uint32_t)OM_EFFECT_NATIVE
        | (uint32_t)OM_EFFECT_FILE_IO
        | (uint32_t)OM_EFFECT_NETWORK_IO
        | (uint32_t)OM_EFFECT_CLOCK
        | (uint32_t)OM_EFFECT_WATCH
        | (uint32_t)OM_EFFECT_STOP
        | (uint32_t)OM_EFFECT_CALL_ACTION
        | (uint32_t)OM_EFFECT_READS_ACTOR_STATE;
    for (size_t index = 0; index < scan->context->resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &scan->context->resolution->symbols[index];
        if (symbol->kind != OM_SYMBOL_FUNCTION && symbol->kind != OM_SYMBOL_ACTION) {
            continue;
        }
        const uint32_t effects = scan->symbol_effects[index];
        Om_Status status = om_semantic_set_symbol_effect(
            scan->context, symbol->id, effects);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (symbol->kind == OM_SYMBOL_FUNCTION
            && (effects & forbidden_function_effects) != 0) {
            status = om_semantic_add_diagnostic(
                scan->context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                symbol->module_id, symbol->span, symbol->id);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
        const Om_Ast_Node *declaration = om_semantic_node(
            scan->context, symbol->module_id, symbol->declaration_node);
        if (declaration == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = om_semantic_set_node_effect(scan->context, symbol->module_id,
                                             symbol->declaration_node, effects);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    for (size_t index = 0; index < scan->edges.length; ++index) {
        const Om_Call_Edge *edge = om_vector_at_const(&scan->edges, index);
        if (edge->callee.value >= scan->context->resolution->symbol_count) {
            continue;
        }
        const Om_Symbol *callee = om_semantic_symbol(scan->context,
                                                     edge->callee);
        uint32_t effects = scan->symbol_effects[edge->callee.value];
        if (callee != NULL && callee->kind == OM_SYMBOL_ACTION) {
            effects |= (uint32_t)OM_EFFECT_CALL_ACTION;
        }
        const Om_Status status = om_semantic_set_node_effect(
            scan->context, edge->module_id, edge->call_node, effects);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status validate_state_initializers(Om_Effect_Scan *scan)
{
    for (size_t module_index = 0; module_index < scan->context->module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = om_semantic_module_ast(scan->context, module);
        const Om_Ast_Node *program = ast != NULL
            ? om_ast_get_node(ast, ast->root) : NULL;
        if (program == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t index = 0; index < program->child_count; ++index) {
            const Om_Ast_Node_Id actor_id = om_semantic_child(
                scan->context, module, program, index);
            const Om_Ast_Node *actor = om_semantic_node(scan->context, module,
                                                        actor_id);
            if (actor == NULL || actor->kind != OM_AST_ACTOR_DECLARATION) {
                continue;
            }
            for (size_t member_index = 0; member_index < actor->child_count;
                 ++member_index) {
                const Om_Ast_Node_Id state_id = om_semantic_child(
                    scan->context, module, actor, member_index);
                const Om_Ast_Node *state = om_semantic_node(scan->context, module,
                                                            state_id);
                if (state == NULL || state->kind != OM_AST_BINDING
                    || state->child_count != 1) {
                    continue;
                }
                uint32_t effects = (uint32_t)OM_EFFECT_NONE;
                const Om_Status status = compute_tree_effects(
                    scan, module, om_semantic_child(scan->context, module, state, 0),
                    false, 0, &effects);
                if (status != OM_STATUS_OK) {
                    return status;
                }
                const uint32_t forbidden = effects
                    & ~((uint32_t)OM_EFFECT_MUTATES_LOCAL
                        | (uint32_t)OM_EFFECT_READS_ACTOR_STATE);
                if (forbidden != (uint32_t)OM_EFFECT_NONE) {
                    const Om_Status diagnostic_status = om_semantic_add_diagnostic(
                        scan->context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501,
                        module, state->span,
                        om_semantic_resolved_symbol(scan->context, module,
                                                    state_id));
                    if (diagnostic_status != OM_STATUS_OK) {
                        return diagnostic_status;
                    }
                }
            }
        }
    }
    return OM_STATUS_OK;
}

Om_Status om_effect_analyze_internal(Om_Semantic_Analysis_Context *context)
{
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Effect_Scan scan = {.context = context};
    Om_Status status = om_vector_init(&scan.edges, sizeof(Om_Call_Edge));
    const size_t effect_slot_count = context->resolution->symbol_count != 0
        ? context->resolution->symbol_count : 1;
    scan.symbol_effects = om_alloc_array(effect_slot_count,
                                         sizeof(*scan.symbol_effects));
    if (status == OM_STATUS_OK && scan.symbol_effects == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (scan.symbol_effects != NULL) {
        for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
            scan.symbol_effects[index] = (uint32_t)OM_EFFECT_NONE;
        }
    }
    if (status == OM_STATUS_OK) {
        status = scan_project_declarations(&scan);
    }
    if (status == OM_STATUS_OK) {
        status = propagate_call_effects(&scan);
    }
    if (status == OM_STATUS_OK) {
        status = publish_effect_summaries(&scan);
    }
    if (status == OM_STATUS_OK) {
        status = validate_state_initializers(&scan);
    }
    om_free(scan.symbol_effects);
    om_vector_destroy(&scan.edges);
    return status;
}
