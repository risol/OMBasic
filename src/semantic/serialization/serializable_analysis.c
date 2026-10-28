#include "../semantic_model/semantic_analysis_internal.h"

#include "ombasic/semantic/type_store.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/syntax/ast.h"
#include "ombasic/syntax/name_rules.h"

static bool find_serializability(
    const Om_Semantic_Analysis_Context *context,
    Om_Type_Id type_id,
    bool *serializable)
{
    for (size_t index = 0; index < context->serializable_types.length; ++index) {
        const Om_Type_Serializability *entry = om_vector_at_const(
            &context->serializable_types, index);
        if (entry->type_id.value == type_id.value) {
            *serializable = entry->serializable;
            return true;
        }
    }
    return false;
}

static Om_Status record_serializability(
    Om_Semantic_Analysis_Context *context,
    Om_Type_Id type_id,
    bool serializable)
{
    for (size_t index = 0; index < context->serializable_types.length; ++index) {
        Om_Type_Serializability *entry = om_vector_at(
            &context->serializable_types, index);
        if (entry->type_id.value == type_id.value) {
            entry->serializable = serializable;
            return OM_STATUS_OK;
        }
    }
    const Om_Type_Serializability entry = {type_id, serializable};
    return om_vector_push(&context->serializable_types, &entry);
}

static bool type_is_active(const Om_Vector *active_types, Om_Type_Id type_id)
{
    for (size_t index = 0; index < active_types->length; ++index) {
        const Om_Type_Id *active = om_vector_at_const(active_types, index);
        if (active->value == type_id.value) {
            return true;
        }
    }
    return false;
}

static bool actor_down_symbol(const Om_Semantic_Analysis_Context *context,
                              Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = om_semantic_symbol(context, symbol_id);
    return symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
        && om_string_view_equal(symbol->name,
                                (Om_String_View){"Actor_Down", 10});
}

static bool system_message_symbol(const Om_Semantic_Analysis_Context *context,
                                  Om_Symbol_Id symbol_id)
{
    const Om_Symbol *symbol = om_semantic_symbol(context, symbol_id);
    if (symbol == NULL || symbol->kind != OM_SYMBOL_BUILTIN) {
        return false;
    }
    static const char *const names[] = {
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE,
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        size_t length = 0;
        while (names[index][length] != '\0') {
            ++length;
        }
        if (om_name_equal_case_insensitive(
                symbol->name, (Om_String_View){names[index], length})) {
            return true;
        }
    }
    return false;
}

static Om_Status is_serializable_type(Om_Semantic_Analysis_Context *context,
                                      Om_Type_Id type_id,
                                      Om_Vector *active_types,
                                      size_t depth,
                                      bool *serializable);

static Om_Status is_serializable_nominal(
    Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id nominal_symbol,
    Om_Vector *active_types,
    size_t depth,
    bool *serializable)
{
    const Om_Symbol *owner = om_semantic_symbol(context, nominal_symbol);
    if (owner == NULL
        || (owner->kind != OM_SYMBOL_RECORD
            && owner->kind != OM_SYMBOL_MESSAGE
            && !(owner->kind == OM_SYMBOL_BUILTIN
                 && system_message_symbol(context, nominal_symbol)))) {
        *serializable = false;
        return OM_STATUS_OK;
    }
    if (owner->native_module != NULL) {
        *serializable = false;
        return OM_STATUS_OK;
    }
    if (owner->kind == OM_SYMBOL_MESSAGE
        && actor_down_symbol(context, nominal_symbol)) {
        *serializable = true;
        return OM_STATUS_OK;
    }
    if (owner->kind == OM_SYMBOL_BUILTIN
        && system_message_symbol(context, nominal_symbol)) {
        *serializable = true;
        return OM_STATUS_OK;
    }
    const Om_Ast_Node *declaration = om_semantic_node(
        context, owner->module_id, owner->declaration_node);
    if (declaration == NULL
        || (declaration->kind != OM_AST_RECORD_DECLARATION
            && declaration->kind != OM_AST_MESSAGE_DECLARATION)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    bool result = true;
    for (size_t index = 0; result && index < declaration->child_count; ++index) {
        const Om_Ast_Node_Id field_node = om_semantic_child(
            context, owner->module_id, declaration, index);
        const Om_Symbol_Id field_symbol = om_semantic_resolved_symbol(
            context, owner->module_id, field_node);
        if (field_symbol.value >= context->inference->symbol_scheme_count) {
            result = false;
            break;
        }
        const Om_Type_Id field_type =
            context->inference->symbol_schemes[field_symbol.value].type_id;
        bool field_serializable = false;
        const Om_Status status = is_serializable_type(
            context, field_type, active_types, depth + 1,
            &field_serializable);
        if (status != OM_STATUS_OK) {
            return status;
        }
        result = field_serializable;
    }
    *serializable = result;
    return OM_STATUS_OK;
}

static Om_Status is_serializable_type(Om_Semantic_Analysis_Context *context,
                                      Om_Type_Id type_id,
                                      Om_Vector *active_types,
                                      size_t depth,
                                      bool *serializable)
{
    if (context == NULL || active_types == NULL || serializable == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *serializable = false;
    if (depth >= 256) {
        return OM_STATUS_OK;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_resolve(&context->inference->type_store,
                                             type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (find_serializability(context, resolved, serializable)) {
        return OM_STATUS_OK;
    }
    if (type_is_active(active_types, resolved)) {
        *serializable = true;
        return OM_STATUS_OK;
    }

    Om_Type_View view = {0};
    status = om_type_store_get(&context->inference->type_store, resolved,
                               &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_vector_push(active_types, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }

    bool result = false;
    switch (view.kind) {
    case OM_TYPE_INT:
    case OM_TYPE_FLOAT:
    case OM_TYPE_BOOL:
    case OM_TYPE_STRING:
    case OM_TYPE_NULL:
    case OM_TYPE_UNIT:
    case OM_TYPE_NODE_REF:
    case OM_TYPE_ACTOR_ID:
    case OM_TYPE_DOWN_REASON:
    case OM_TYPE_ACTOR_REF:
        result = true;
        break;
    case OM_TYPE_LIST: {
        Om_Type_Id element = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(&context->inference->type_store,
                                        resolved, 0, &element);
        if (status == OM_STATUS_OK) {
            status = is_serializable_type(context, element, active_types,
                                          depth + 1, &result);
        }
        break;
    }
    case OM_TYPE_MAP: {
        Om_Type_Id key = OM_TYPE_ID_INVALID;
        Om_Type_Id value = OM_TYPE_ID_INVALID;
        bool key_serializable = false;
        bool value_serializable = false;
        status = om_type_store_child_at(&context->inference->type_store,
                                        resolved, 0, &key);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(&context->inference->type_store,
                                            resolved, 1, &value);
        }
        if (status == OM_STATUS_OK) {
            status = is_serializable_type(context, key, active_types,
                                          depth + 1, &key_serializable);
        }
        if (status == OM_STATUS_OK) {
            status = is_serializable_type(context, value, active_types,
                                          depth + 1, &value_serializable);
        }
        result = key_serializable && value_serializable;
        break;
    }
    case OM_TYPE_RESULT: {
        Om_Type_Id value_type = OM_TYPE_ID_INVALID;
        Om_Type_Id error_type = OM_TYPE_ID_INVALID;
        bool value_serializable = false;
        bool error_serializable = false;
        status = om_type_store_child_at(&context->inference->type_store,
                                        resolved, 0, &value_type);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(&context->inference->type_store,
                                            resolved, 1, &error_type);
        }
        if (status == OM_STATUS_OK) {
            status = is_serializable_type(context, value_type, active_types,
                                          depth + 1, &value_serializable);
        }
        if (status == OM_STATUS_OK) {
            status = is_serializable_type(context, error_type, active_types,
                                          depth + 1, &error_serializable);
        }
        result = value_serializable && error_serializable;
        break;
    }
    case OM_TYPE_RECORD:
    case OM_TYPE_MESSAGE:
        status = is_serializable_nominal(context, view.nominal_symbol,
                                         active_types, depth + 1, &result);
        break;
    case OM_TYPE_ACTION:
    case OM_TYPE_FUNCTION:
    case OM_TYPE_NATIVE_POINTER:
    case OM_TYPE_VARIABLE:
    default:
        result = false;
        break;
    }
    --active_types->length;
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = record_serializability(context, resolved, result);
    if (status == OM_STATUS_OK && type_id.value != resolved.value) {
        status = record_serializability(context, type_id, result);
    }
    if (status == OM_STATUS_OK) {
        *serializable = result;
    }
    return status;
}

static Om_Status diagnose_unserializable_sends(
    Om_Semantic_Analysis_Context *context)
{
    Om_Vector active_types = {0};
    Om_Status status = om_vector_init(&active_types, sizeof(Om_Type_Id));
    for (size_t module_index = 0; status == OM_STATUS_OK
                                 && module_index < context->module_count;
         ++module_index) {
        const Om_Module_Id module = {(uint32_t)module_index};
        const Om_Ast *ast = om_semantic_module_ast(context, module);
        if (ast == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        for (size_t node_index = 0; status == OM_STATUS_OK
                                   && node_index < ast->node_count;
             ++node_index) {
            const Om_Ast_Node_Id send_id = {(uint32_t)node_index};
            const Om_Ast_Node *send = om_ast_get_node(ast, send_id);
            if (send == NULL || send->kind != OM_AST_SEND) {
                continue;
            }
            if (send->child_count != 2) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            const Om_Ast_Node_Id message_node_id = om_semantic_child(
                context, module, send, 1);
            const Om_Ast_Node *message_node = om_semantic_node(
                context, module, message_node_id);
            const Om_Type_Id message_type = om_type_inference_type_for_node(
                context->inference, module, message_node_id);
            if (message_node == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            bool serializable = false;
            Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
            active_types.length = 0;
            status = is_serializable_type(context, message_type,
                                          &active_types, 0, &serializable);
            if (status == OM_STATUS_OK && !serializable) {
                Om_Type_Id resolved_message_type = OM_TYPE_ID_INVALID;
                Om_Type_View message_view = {0};
                status = om_type_store_resolve(
                    &context->inference->type_store, message_type,
                    &resolved_message_type);
                if (status == OM_STATUS_OK) {
                    status = om_type_store_get(
                        &context->inference->type_store,
                        resolved_message_type, &message_view);
                }
                if (status == OM_STATUS_OK
                    && message_view.kind == OM_TYPE_MESSAGE) {
                    message_symbol = message_view.nominal_symbol;
                }
            }
            if (status == OM_STATUS_OK && !serializable) {
                const Om_Status diagnostic_status = om_semantic_add_diagnostic(
                    context, OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701,
                    module, message_node->span, message_symbol);
                if (diagnostic_status != OM_STATUS_OK) {
                    status = diagnostic_status;
                }
            }
        }
    }
    om_vector_destroy(&active_types);
    return status;
}

Om_Status om_serializable_check_internal(
    Om_Semantic_Analysis_Context *context)
{
    if (context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return diagnose_unserializable_sends(context);
}
