#include "../semantic_model/semantic_analysis_internal.h"

#include "ombasic/syntax/ast.h"
#include "ombasic/syntax/name_rules.h"

static bool is_actor_down(const Om_Semantic_Analysis_Context *context,
                          Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = om_semantic_symbol(context, symbol_id);
    return symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
        && symbol->builtin_id == OM_BUILTIN_ACTOR_DOWN;
}

static bool is_system_message(const Om_Semantic_Analysis_Context *context,
                              Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = om_semantic_symbol(context, symbol_id);
    return symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
        && om_builtin_id_is_system_message(symbol->builtin_id);
}

static Om_Symbol_Id find_builtin_symbol(
    const Om_Semantic_Analysis_Context *context,
    Om_Builtin_Id builtin_id)
{
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &context->resolution->symbols[index];
        if (symbol->kind == OM_SYMBOL_BUILTIN
            && symbol->builtin_id == builtin_id) {
            return symbol->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Status add_protocol_entry(Om_Semantic_Analysis_Context *context,
                                    Om_Symbol_Id actor_symbol,
                                    Om_Symbol_Id message_symbol,
                                    Om_Ast_Node_Id receive_node)
{
    for (size_t index = 0; index < context->actor_protocol.length; ++index) {
        const Om_Actor_Protocol_Entry *existing = om_vector_at_const(
            &context->actor_protocol, index);
        if (existing->actor_symbol.value == actor_symbol.value
            && existing->message_symbol.value == message_symbol.value) {
            return OM_STATUS_OK;
        }
    }
    const Om_Actor_Protocol_Entry entry = {
        .actor_symbol = actor_symbol,
        .message_symbol = message_symbol,
        .receive_node = receive_node,
    };
    return om_vector_push(&context->actor_protocol, &entry);
}

static Om_Status collect_actor_protocols(
    Om_Semantic_Analysis_Context *context)
{
    Om_Symbol_Id actor_down_symbol = OM_SYMBOL_ID_INVALID;
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *symbol = &context->resolution->symbols[index];
        if (is_actor_down(context, symbol->id)) {
            actor_down_symbol = symbol->id;
            break;
        }
    }

    static const struct {
        Om_Builtin_Id actor;
        Om_Builtin_Id message;
    } system_protocols[] = {
        {OM_BUILTIN_SYSTEM_ACTOR_CONSOLE,
         OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE},
        {OM_BUILTIN_SYSTEM_ACTOR_CONSOLE,
         OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_READ_LINE},
        {OM_BUILTIN_SYSTEM_ACTOR_TIMER,
         OM_BUILTIN_SYSTEM_MESSAGE_TIMER_AFTER},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_APPEND},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_EXISTS},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_DELETE},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_CREATE},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_LIST},
        {OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
         OM_BUILTIN_SYSTEM_MESSAGE_FILE_STAT},
        {OM_BUILTIN_SYSTEM_ACTOR_CLOCK,
         OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_MONOTONIC_NOW},
        {OM_BUILTIN_SYSTEM_ACTOR_CLOCK,
         OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_UTC_NOW},
        {OM_BUILTIN_SYSTEM_ACTOR_SECURE_RANDOM,
          OM_BUILTIN_SYSTEM_MESSAGE_SECURE_RANDOM_BYTES},
        {OM_BUILTIN_SYSTEM_ACTOR_LOGGER,
         OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_WRITE},
        {OM_BUILTIN_SYSTEM_ACTOR_LOGGER,
         OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_FLUSH},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_LISTEN},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_CONNECT},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_ACCEPT},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_SEND},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_RECEIVE},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_TCP_CLOSE},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_UDP_CONNECT},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_UDP_SEND},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_UDP_RECEIVE},
        {OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
         OM_BUILTIN_SYSTEM_MESSAGE_UDP_CLOSE},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_SPAWN},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WRITE_STDIN},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_CLOSE_STDIN},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDOUT},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDERR},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_TERMINATE},
        {OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
         OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WAIT}
    };
    for (size_t index = 0;
         index < sizeof(system_protocols) / sizeof(system_protocols[0]);
         ++index) {
        const Om_Symbol_Id actor = find_builtin_symbol(
            context, system_protocols[index].actor);
        const Om_Symbol_Id message = find_builtin_symbol(
            context, system_protocols[index].message);
        if (actor.value == UINT32_MAX || message.value == UINT32_MAX) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Status status = add_protocol_entry(
            context, actor, message, OM_AST_NODE_ID_INVALID);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }

    for (size_t module_index = 0; module_index < context->module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = om_semantic_module_ast(context, module);
        const Om_Ast_Node *program = ast != NULL
            ? om_ast_get_node(ast, ast->root) : NULL;
        if (program == NULL || program->kind != OM_AST_PROGRAM) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t index = 0; index < program->child_count; ++index) {
            const Om_Ast_Node_Id actor_node_id = om_semantic_child(
                context, module, program, index);
            const Om_Ast_Node *actor_node = om_semantic_node(
                context, module, actor_node_id);
            if (actor_node == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            if (actor_node->kind != OM_AST_ACTOR_DECLARATION) {
                continue;
            }
            const Om_Symbol_Id actor_symbol = om_semantic_resolved_symbol(
                context, module, actor_node_id);
            const Om_Symbol *actor = om_semantic_symbol(context, actor_symbol);
            if (actor == NULL || actor->kind != OM_SYMBOL_ACTOR) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            Om_Status status = OM_STATUS_OK;
            if (actor_down_symbol.value != UINT32_MAX) {
                status = add_protocol_entry(context, actor_symbol,
                                            actor_down_symbol,
                                            OM_AST_NODE_ID_INVALID);
            }
            for (size_t member_index = 0;
                 status == OM_STATUS_OK && member_index < actor_node->child_count;
                 ++member_index) {
                const Om_Ast_Node_Id member_id = om_semantic_child(
                    context, module, actor_node, member_index);
                const Om_Ast_Node *member = om_semantic_node(
                    context, module, member_id);
                if (member == NULL) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                if (member->kind != OM_AST_RECEIVE_DECLARATION) {
                    continue;
                }
                const Om_Symbol_Id message_symbol =
                    om_semantic_resolved_symbol(context, module, member_id);
                const Om_Symbol *message = om_semantic_symbol(
                    context, message_symbol);
                if (message == NULL
                    || (message->kind != OM_SYMBOL_MESSAGE
                        && !is_actor_down(context, message_symbol)
                        && !is_system_message(context, message_symbol))) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                status = add_protocol_entry(context, actor_symbol,
                                            message_symbol, member_id);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

static Om_Status actor_symbol_for_type(
    const Om_Type_Inference_Result *inference,
    Om_Type_Id type_id,
    Om_Symbol_Id *actor_symbol,
    bool *known)
{
    if (inference == NULL || actor_symbol == NULL || known == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *actor_symbol = OM_SYMBOL_ID_INVALID;
    *known = false;
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(&inference->type_store, type_id,
                                             &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Type_View view = {0};
    status = om_type_store_get(&inference->type_store, resolved, &view);
    if (status != OM_STATUS_OK || view.kind != OM_TYPE_ACTOR_REF
        || view.child_count != 1) {
        return status;
    }
    Om_Type_Id protocol_id = OM_TYPE_ID_INVALID;
    status = om_type_store_child_at(&inference->type_store, resolved, 0,
                                    &protocol_id);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_type_store_resolve(&inference->type_store, protocol_id,
                                   &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_type_store_get(&inference->type_store, resolved, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.kind == OM_TYPE_ACTOR_PROTOCOL
        && view.nominal_symbol.value != UINT32_MAX) {
        *actor_symbol = view.nominal_symbol;
        *known = true;
    }
    return OM_STATUS_OK;
}

static Om_Status message_symbol_for_type(
    const Om_Type_Inference_Result *inference,
    Om_Type_Id type_id,
    Om_Symbol_Id *message_symbol,
    bool *known)
{
    if (inference == NULL || message_symbol == NULL || known == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *message_symbol = OM_SYMBOL_ID_INVALID;
    *known = false;
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(&inference->type_store, type_id,
                                             &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Type_View view = {0};
    status = om_type_store_get(&inference->type_store, resolved, &view);
    if (status == OM_STATUS_OK && view.kind == OM_TYPE_MESSAGE
        && view.nominal_symbol.value != UINT32_MAX) {
        *message_symbol = view.nominal_symbol;
        *known = true;
    }
    return status;
}

static bool context_actor_accepts(
    const Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id actor_symbol,
    Om_Symbol_Id message_symbol)
{
    for (size_t index = 0; index < context->actor_protocol.length; ++index) {
        const Om_Actor_Protocol_Entry *entry = om_vector_at_const(
            &context->actor_protocol, index);
        if (entry->actor_symbol.value == actor_symbol.value
            && entry->message_symbol.value == message_symbol.value) {
            return true;
        }
    }
    return false;
}

static Om_Status check_send_protocols(Om_Semantic_Analysis_Context *context)
{
    for (size_t module_index = 0; module_index < context->module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = om_semantic_module_ast(context, module);
        if (ast == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t node_index = 0; node_index < ast->node_count; ++node_index) {
            const Om_Ast_Node_Id send_id = {(uint32_t)node_index};
            const Om_Ast_Node *send = om_ast_get_node(ast, send_id);
            if (send == NULL || send->kind != OM_AST_SEND) {
                continue;
            }
            if (send->child_count != 2) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            const Om_Ast_Node_Id receiver_id = om_semantic_child(
                context, module, send, 0);
            const Om_Ast_Node_Id message_id = om_semantic_child(
                context, module, send, 1);
            const Om_Type_Id receiver_type = om_type_inference_type_for_node(
                context->inference, module, receiver_id);
            const Om_Type_Id message_type = om_type_inference_type_for_node(
                context->inference, module, message_id);
            Om_Symbol_Id actor_symbol = OM_SYMBOL_ID_INVALID;
            Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
            bool actor_known = false;
            bool message_known = false;
            Om_Status status = actor_symbol_for_type(
                context->inference, receiver_type, &actor_symbol, &actor_known);
            if (status == OM_STATUS_OK) {
                status = message_symbol_for_type(context->inference,
                                                 message_type,
                                                 &message_symbol,
                                                 &message_known);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
            const bool universally_accepted = message_known
                && is_actor_down(context, message_symbol);
            if (!actor_known || !message_known
                || (!universally_accepted
                    && !context_actor_accepts(context, actor_symbol,
                                              message_symbol))) {
                status = om_semantic_add_diagnostic(
                    context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601,
                    module, send->span,
                    actor_known ? actor_symbol : OM_SYMBOL_ID_INVALID);
                if (status != OM_STATUS_OK) {
                    return status;
                }
            }
        }
    }
    return OM_STATUS_OK;
}

Om_Status om_actor_protocol_check_internal(
    Om_Semantic_Analysis_Context *context)
{
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = collect_actor_protocols(context);
    if (status == OM_STATUS_OK) {
        status = check_send_protocols(context);
    }
    return status;
}
