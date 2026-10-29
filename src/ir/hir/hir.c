#include "ombasic/ir/hir.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/syntax/name_rules.h"

typedef struct Om_Hir_Storage {
    Om_Vector modules;
    Om_Vector entities;
    Om_Vector nodes;
    Om_Vector children;
} Om_Hir_Storage;

typedef struct Om_Hir_Lower_Context {
    const Om_Module_Project *project;
    const Om_Parser_Result *parsed;
    const Om_Resolution_Result *resolution;
    const Om_Type_Inference_Result *inference;
    const Om_Semantic_Analysis_Result *semantic;
    Om_Hir_Storage *storage;
} Om_Hir_Lower_Context;

static Om_Hir_Storage *storage_of(const Om_Hir_Project *project)
{
    return project != NULL ? project->owned_storage : NULL;
}

static Om_Status init_storage(Om_Hir_Storage *storage)
{
    Om_Status status = om_vector_init(&storage->modules, sizeof(Om_Hir_Module));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->entities, sizeof(Om_Hir_Entity));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->nodes, sizeof(Om_Hir_Node));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->children, sizeof(Om_Hir_Node_Id));
    }
    return status;
}

static void destroy_storage(Om_Hir_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    om_vector_destroy(&storage->children);
    om_vector_destroy(&storage->nodes);
    om_vector_destroy(&storage->entities);
    om_vector_destroy(&storage->modules);
    om_free(storage);
}

static Om_Hir_Operator operator_for_token(Om_Token_Kind token,
                                          Om_Keyword keyword)
{
    switch (token) {
    case OM_TOKEN_PLUS: return OM_HIR_OPERATOR_ADD;
    case OM_TOKEN_MINUS: return OM_HIR_OPERATOR_SUBTRACT;
    case OM_TOKEN_STAR: return OM_HIR_OPERATOR_MULTIPLY;
    case OM_TOKEN_SLASH: return OM_HIR_OPERATOR_DIVIDE;
    case OM_TOKEN_PERCENT: return OM_HIR_OPERATOR_REMAINDER;
    case OM_TOKEN_EQUAL: return OM_HIR_OPERATOR_EQUAL;
    case OM_TOKEN_NOT_EQUAL: return OM_HIR_OPERATOR_NOT_EQUAL;
    case OM_TOKEN_LESS: return OM_HIR_OPERATOR_LESS;
    case OM_TOKEN_GREATER: return OM_HIR_OPERATOR_GREATER;
    case OM_TOKEN_LESS_EQUAL: return OM_HIR_OPERATOR_LESS_EQUAL;
    case OM_TOKEN_GREATER_EQUAL: return OM_HIR_OPERATOR_GREATER_EQUAL;
    case OM_TOKEN_KEYWORD:
        if (keyword == OM_KEYWORD_AND) return OM_HIR_OPERATOR_AND;
        if (keyword == OM_KEYWORD_OR) return OM_HIR_OPERATOR_OR;
        if (keyword == OM_KEYWORD_NOT) return OM_HIR_OPERATOR_NOT;
        if (keyword == OM_KEYWORD_TRY) return OM_HIR_OPERATOR_NONE;
        break;
    default:
        break;
    }
    return OM_HIR_OPERATOR_NONE;
}

static bool builtin_member_ordinal(Om_Builtin_Member_Id member_id,
                                  uint64_t *member_index)
{
    if (member_index == NULL) {
        return false;
    }
    switch (member_id) {
    case OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NORMAL:
        *member_index = 0;
        return true;
    case OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_FAILED:
        *member_index = 1;
        return true;
    case OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NODE_UNREACHABLE:
        *member_index = 2;
        return true;
    case OM_BUILTIN_MEMBER_NONE:
        return false;
    }
    return false;
}

static Om_Hir_Kind kind_for_call(const Om_Hir_Lower_Context *context,
                                 Om_Module_Id module_id,
                                 const Om_Ast *ast,
                                 const Om_Ast_Node *node)
{
    if (node->child_count == 0) {
        return OM_HIR_CALL_INDIRECT;
    }
    const Om_Ast_Node_Id callee_id = om_ast_get_child(ast, node, 0);
    const Om_Symbol_Id symbol_id = om_resolution_symbol_for_node(
        context->resolution, module_id, callee_id);
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->resolution, symbol_id);
    if (symbol == NULL) {
        return OM_HIR_CALL_INDIRECT;
    }
    switch (symbol->kind) {
    case OM_SYMBOL_RECORD: return OM_HIR_RECORD_CONSTRUCT;
    case OM_SYMBOL_MESSAGE: return OM_HIR_MESSAGE_CONSTRUCT;
    case OM_SYMBOL_FUNCTION:
        return OM_HIR_CALL_DIRECT;
    case OM_SYMBOL_BUILTIN:
        if (symbol->builtin_id == OM_BUILTIN_OK) {
            return OM_HIR_RESULT_OK;
        }
        if (symbol->builtin_id == OM_BUILTIN_ERROR) {
            return OM_HIR_RESULT_ERROR;
        }
        return om_builtin_id_is_system_message(symbol->builtin_id)
            ? OM_HIR_MESSAGE_CONSTRUCT : OM_HIR_CALL_DIRECT;
    default:
        return OM_HIR_CALL_INDIRECT;
    }
}

static Om_Symbol_Id inferred_record_field_symbol(
    const Om_Hir_Lower_Context *context,
    Om_Module_Id module_id,
    const Om_Ast *ast,
    const Om_Ast_Node *member)
{
    if (context == NULL || ast == NULL || member == NULL
        || member->kind != OM_AST_MEMBER || member->child_count != 1) {
        return OM_SYMBOL_ID_INVALID;
    }
    const Om_Ast_Node_Id receiver_id = om_ast_get_child(ast, member, 0);
    Om_Type_Id receiver_type = om_type_inference_type_for_node(
        context->inference, module_id, receiver_id);
    Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
    Om_Type_View type_view = {0};
    if (receiver_type.value == UINT32_MAX
        || om_type_store_resolve(&context->inference->type_store, receiver_type,
                                 &resolved_type) != OM_STATUS_OK
        || om_type_store_get(&context->inference->type_store, resolved_type,
                             &type_view) != OM_STATUS_OK
        || (type_view.kind != OM_TYPE_RECORD
            && type_view.kind != OM_TYPE_MESSAGE)) {
        return OM_SYMBOL_ID_INVALID;
    }
    const Om_Symbol *record = om_resolution_symbol_by_id(context->resolution,
        type_view.nominal_symbol);
    if (record == NULL
        || (record->kind != OM_SYMBOL_RECORD
            && record->kind != OM_SYMBOL_MESSAGE)) {
        return OM_SYMBOL_ID_INVALID;
    }
    Om_Scope_Id field_scope_id = OM_SCOPE_ID_INVALID;
    for (size_t index = 0; index < context->resolution->scope_count; ++index) {
        const Om_Scope *scope = &context->resolution->scopes[index];
        if ((scope->kind == OM_SCOPE_RECORD
             || scope->kind == OM_SCOPE_MESSAGE)
            && scope->module_id.value == record->module_id.value
            && scope->owner_node.value == record->declaration_node.value) {
            field_scope_id = scope->id;
            break;
        }
    }
    if (field_scope_id.value == UINT32_MAX) {
        return OM_SYMBOL_ID_INVALID;
    }
    for (size_t index = 0; index < context->resolution->symbol_count; ++index) {
        const Om_Symbol *field = &context->resolution->symbols[index];
        if (field->kind == OM_SYMBOL_FIELD
            && field->scope_id.value == field_scope_id.value
            && om_string_view_equal(field->name, member->name)) {
            return field->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Hir_Kind kind_for_ast(const Om_Hir_Lower_Context *context,
                                Om_Module_Id module_id,
                                const Om_Ast *ast,
                                const Om_Ast_Node *node,
                                Om_Ast_Kind parent_kind)
{
    switch (node->kind) {
    case OM_AST_PROGRAM: return OM_HIR_PROGRAM;
    case OM_AST_IMPORT: return OM_HIR_IMPORT;
    case OM_AST_RECORD_DECLARATION: return OM_HIR_RECORD;
    case OM_AST_MESSAGE_DECLARATION: return OM_HIR_MESSAGE;
    case OM_AST_FUNCTION_DECLARATION: return OM_HIR_FUNCTION;
    case OM_AST_ACTION_DECLARATION: return OM_HIR_ACTION;
    case OM_AST_ACTOR_DECLARATION: return OM_HIR_ACTOR;
    case OM_AST_RECEIVE_DECLARATION: return OM_HIR_RECEIVE;
    case OM_AST_PARAMETER: return OM_HIR_PARAMETER;
    case OM_AST_BLOCK: return OM_HIR_BLOCK;
    case OM_AST_BINDING: {
        const Om_Symbol_Id symbol_id = om_resolution_symbol_for_node(
            context->resolution, module_id,
            (Om_Ast_Node_Id){(uint32_t)(node - ast->nodes)});
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            context->resolution, symbol_id);
        if (symbol != NULL
            && symbol->declaration_node.value
                != (uint32_t)(node - ast->nodes)) {
            if (symbol->scope_id.value < context->resolution->scope_count
                && context->resolution->scopes[symbol->scope_id.value].kind
                    == OM_SCOPE_ACTOR) {
                return OM_HIR_ACTOR_STATE_ASSIGN;
            }
            return OM_HIR_ASSIGN_VAR;
        }
        if (parent_kind == OM_AST_ACTOR_DECLARATION) {
            return OM_HIR_ACTOR_STATE_INIT;
        }
        return (node->flags & OM_AST_FLAG_MUTABLE) != 0
            ? OM_HIR_DECLARE_VAR : OM_HIR_BIND;
    }
    case OM_AST_RETURN: return OM_HIR_RETURN;
    case OM_AST_BREAK: return OM_HIR_BREAK;
    case OM_AST_CONTINUE: return OM_HIR_CONTINUE;
    case OM_AST_SEND: return OM_HIR_SEND;
    case OM_AST_WATCH: return OM_HIR_WATCH;
    case OM_AST_STOP: return OM_HIR_STOP;
    case OM_AST_CALL_STATEMENT: return OM_HIR_CALL_STATEMENT;
    case OM_AST_IF: return OM_HIR_IF;
    case OM_AST_IF_ARM: return OM_HIR_IF_ARM;
    case OM_AST_WHILE: return OM_HIR_WHILE;
    case OM_AST_FOR:
        return (node->flags & OM_AST_FLAG_FOR_NUMERIC) != 0
            ? OM_HIR_FOR_RANGE : OM_HIR_FOR_EACH;
    case OM_AST_SELECT: return OM_HIR_SELECT;
    case OM_AST_CASE_ARM: return OM_HIR_CASE_ARM;
    case OM_AST_NAME: {
        const Om_Symbol_Id symbol_id = om_resolution_symbol_for_node(
            context->resolution, module_id,
            (Om_Ast_Node_Id){(uint32_t)(node - ast->nodes)});
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            context->resolution, symbol_id);
        if (symbol != NULL && symbol->kind == OM_SYMBOL_MODULE_ALIAS) {
            return OM_HIR_SYMBOL_REF;
        }
        if (symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
            && om_builtin_id_is_system_actor(symbol->builtin_id)) {
            return OM_HIR_SYSTEM_ACTOR_REF;
        }
        return OM_HIR_LOCAL;
    }
    case OM_AST_INTEGER_LITERAL: return OM_HIR_INTEGER;
    case OM_AST_FLOAT_LITERAL: return OM_HIR_FLOAT;
    case OM_AST_STRING_LITERAL: return OM_HIR_STRING;
    case OM_AST_STRING_TEXT: return OM_HIR_STRING_TEXT;
    case OM_AST_INTERPOLATED_STRING: return OM_HIR_INTERPOLATED_STRING;
    case OM_AST_BOOLEAN_LITERAL: return OM_HIR_BOOLEAN;
    case OM_AST_NULL_LITERAL: return OM_HIR_NULL;
    case OM_AST_SELF: return OM_HIR_SELF;
    case OM_AST_UNARY:
        return node->keyword == OM_KEYWORD_TRY ? OM_HIR_TRY : OM_HIR_UNARY;
    case OM_AST_BINARY: return OM_HIR_BINARY;
    case OM_AST_CALL: return kind_for_call(context, module_id, ast, node);
    case OM_AST_MEMBER: {
        const Om_Symbol_Id symbol_id = om_resolution_symbol_for_node(
            context->resolution, module_id,
            (Om_Ast_Node_Id){(uint32_t)(node - ast->nodes)});
        const Om_Symbol *symbol = om_resolution_symbol_by_id(
            context->resolution, symbol_id);
        const Om_Builtin_Member_Id member_id =
            om_resolution_builtin_member_for_node(context->resolution,
                                                   module_id,
                                                   (Om_Ast_Node_Id){
                                                       (uint32_t)(node - ast->nodes)});
        if (symbol != NULL && symbol->kind == OM_SYMBOL_BUILTIN
            && symbol->builtin_id == OM_BUILTIN_ACTOR_DOWN_REASON
            && member_id != OM_BUILTIN_MEMBER_NONE) {
            return OM_HIR_NOMINAL_VALUE;
        }
        return (symbol != NULL && symbol->kind == OM_SYMBOL_FIELD)
                || inferred_record_field_symbol(context, module_id, ast, node)
                    .value != UINT32_MAX
            ? OM_HIR_FIELD_READ : OM_HIR_SYMBOL_REF;
    }
    case OM_AST_INDEX: return OM_HIR_INDEX;
    case OM_AST_WITH: return OM_HIR_RECORD_WITH;
    case OM_AST_FIELD_UPDATE: return OM_HIR_FIELD_UPDATE;
    case OM_AST_SPAWN: return OM_HIR_SPAWN;
    case OM_AST_GROUP: return OM_HIR_GROUP;
    case OM_AST_LIST: return OM_HIR_LIST;
    case OM_AST_MAP: return OM_HIR_MAP;
    case OM_AST_MAP_ENTRY: return OM_HIR_MAP_ENTRY;
    }
    return OM_HIR_PROGRAM;
}

static uint32_t flags_for_ast(const Om_Ast_Node *node)
{
    uint32_t flags = OM_HIR_FLAG_NONE;
    if ((node->flags & OM_AST_FLAG_EXPORTED) != 0) {
        flags |= OM_HIR_FLAG_EXPORTED;
    }
    if ((node->flags & OM_AST_FLAG_HAS_ELSE) != 0) {
        flags |= OM_HIR_FLAG_HAS_ELSE;
    }
    if ((node->flags & OM_AST_FLAG_CASE_ELSE) != 0) {
        flags |= OM_HIR_FLAG_CASE_ELSE;
    }
    if ((node->flags & OM_AST_FLAG_MUTABLE) != 0) {
        flags |= OM_HIR_FLAG_MUTABLE;
    }
    if ((node->flags & OM_AST_FLAG_MIN_INTEGER) != 0) {
        flags |= OM_HIR_FLAG_MIN_INTEGER;
    }
    return flags;
}

static Om_Status lower_node(Om_Hir_Lower_Context *context,
                            Om_Module_Id module_id,
                            const Om_Ast *ast,
                            Om_Ast_Node_Id source_id,
                            Om_Ast_Kind parent_kind,
                            Om_Hir_Node_Id *out)
{
    const Om_Ast_Node *source = om_ast_get_node(ast, source_id);
    if (source == NULL || out == NULL
        || context->storage->nodes.length >= UINT32_MAX
        || context->storage->children.length > UINT32_MAX
        || source->child_count > UINT32_MAX - context->storage->children.length) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Hir_Kind kind = kind_for_ast(context, module_id, ast, source, parent_kind);
    Om_Symbol_Id symbol_id = om_resolution_symbol_for_node(
        context->resolution, module_id, source_id);
    if (kind == OM_HIR_FIELD_READ && symbol_id.value == UINT32_MAX) {
        symbol_id = inferred_record_field_symbol(context, module_id, ast,
                                                  source);
    }
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->resolution, symbol_id);
    if ((kind == OM_HIR_CALL_DIRECT || kind == OM_HIR_CALL_INDIRECT
         || kind == OM_HIR_RESULT_OK || kind == OM_HIR_RESULT_ERROR
         || kind == OM_HIR_RECORD_CONSTRUCT || kind == OM_HIR_MESSAGE_CONSTRUCT)
        && source->child_count != 0) {
        const Om_Ast_Node_Id callee_id = om_ast_get_child(ast, source, 0);
        symbol_id = om_resolution_symbol_for_node(context->resolution,
                                                  module_id, callee_id);
        symbol = om_resolution_symbol_by_id(context->resolution, symbol_id);
    }

    Om_Type_Id type_id = om_type_inference_type_for_node(
        context->inference, module_id, source_id);
    if (type_id.value == UINT32_MAX && kind == OM_HIR_STRING_TEXT) {
        (void)om_type_store_primitive(&context->inference->type_store,
                                      OM_TYPE_STRING, &type_id);
    }
    Om_Hir_Node node = {
        .id = {(uint32_t)context->storage->nodes.length},
        .kind = kind,
        .operator_kind = operator_for_token(source->token_kind, source->keyword),
        .module_id = module_id,
        .source_node = source_id,
        .span = source->span,
        .type_id = type_id,
        .symbol_id = symbol_id,
        .related_symbol_id = OM_SYMBOL_ID_INVALID,
        .builtin_id = symbol != NULL ? symbol->builtin_id : OM_BUILTIN_NONE,
        .builtin_member_id = om_resolution_builtin_member_for_node(
            context->resolution, module_id, source_id),
        .effects = om_semantic_effect_for_node(context->semantic, module_id,
                                               source_id),
        .flags = flags_for_ast(source),
        .child_start = (uint32_t)context->storage->children.length,
        .child_count = kind == OM_HIR_NOMINAL_VALUE ? 0 : source->child_count,
        .name = source->name,
        .integer_magnitude = source->integer_magnitude,
        .float_value = source->float_value,
    };
    if (kind == OM_HIR_NOMINAL_VALUE
        && !builtin_member_ordinal(node.builtin_member_id,
                                   &node.integer_magnitude)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (kind == OM_HIR_UNARY && source->keyword == OM_KEYWORD_NOT) {
        node.operator_kind = OM_HIR_OPERATOR_NOT;
    } else if (kind == OM_HIR_UNARY && source->token_kind == OM_TOKEN_MINUS) {
        node.operator_kind = OM_HIR_OPERATOR_NEGATE;
    } else if (kind == OM_HIR_TRY) {
        node.operator_kind = OM_HIR_OPERATOR_NONE;
    }
    if (symbol != NULL && node.name.length == 0) {
        node.name = symbol->name;
    }
    Om_Status status = om_vector_push(&context->storage->nodes, &node);
    if (status != OM_STATUS_OK) {
        return status;
    }
    *out = node.id;

    for (size_t index = 0; status == OM_STATUS_OK && index < node.child_count;
         ++index) {
        const Om_Hir_Node_Id invalid = OM_HIR_NODE_ID_INVALID;
        status = om_vector_push(&context->storage->children, &invalid);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < node.child_count;
         ++index) {
        const Om_Ast_Node_Id source_child = om_ast_get_child(ast, source, index);
        const Om_Ast_Node *child_node = om_ast_get_node(ast, source_child);
        if (child_node == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        Om_Hir_Node_Id child_id = OM_HIR_NODE_ID_INVALID;
        status = lower_node(context, module_id, ast, source_child, source->kind,
                            &child_id);
        if (status == OM_STATUS_OK) {
            Om_Hir_Node_Id *slot = om_vector_at(
                &context->storage->children,
                (size_t)node.child_start + index);
            if (slot == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                *slot = child_id;
            }
        }
    }
    Om_Hir_Node *stored = om_vector_at(&context->storage->nodes, out->value);
    if (status == OM_STATUS_OK && stored == NULL) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK && kind == OM_HIR_SEND) {
        const Om_Hir_Node *receiver = source->child_count > 0
            ? om_vector_at_const(&context->storage->nodes,
                ((Om_Hir_Node_Id *)context->storage->children.data)
                    [node.child_start].value)
            : NULL;
        const Om_Hir_Node *message = source->child_count > 1
            ? om_vector_at_const(&context->storage->nodes,
                ((Om_Hir_Node_Id *)context->storage->children.data)
                    [node.child_start + 1].value)
            : NULL;
        if (receiver != NULL && receiver->type_id.value != UINT32_MAX) {
            Om_Type_Id receiver_type_id = OM_TYPE_ID_INVALID;
            Om_Type_View receiver_type = {0};
            if (om_type_store_resolve(&context->inference->type_store,
                    receiver->type_id, &receiver_type_id) == OM_STATUS_OK
                && om_type_store_get(&context->inference->type_store,
                                     receiver_type_id, &receiver_type) == OM_STATUS_OK
                && receiver_type.kind == OM_TYPE_ACTOR_REF) {
                Om_Type_Id protocol_type = OM_TYPE_ID_INVALID;
                Om_Type_View protocol = {0};
                if (om_type_store_child_at(&context->inference->type_store,
                        receiver_type_id, 0, &protocol_type) == OM_STATUS_OK
                    && om_type_store_resolve(&context->inference->type_store,
                        protocol_type, &protocol_type) == OM_STATUS_OK
                    && om_type_store_get(&context->inference->type_store,
                                         protocol_type, &protocol) == OM_STATUS_OK
                    && protocol.kind == OM_TYPE_ACTOR_PROTOCOL) {
                    stored->related_symbol_id = protocol.nominal_symbol;
                }
            }
        }
        if (message != NULL && message->type_id.value != UINT32_MAX) {
            Om_Type_Id message_type_id = OM_TYPE_ID_INVALID;
            Om_Type_View message_type = {0};
            if (om_type_store_resolve(&context->inference->type_store,
                    message->type_id, &message_type_id) == OM_STATUS_OK
                && om_type_store_get(&context->inference->type_store,
                    message_type_id, &message_type) == OM_STATUS_OK
                && message_type.kind == OM_TYPE_MESSAGE) {
                stored->symbol_id = message_type.nominal_symbol;
            }
        }
    }
    return status;
}

static Om_Status push_entity(Om_Hir_Storage *storage,
                             Om_Hir_Entity_Kind kind,
                             Om_Module_Id module_id,
                             Om_Hir_Node_Id node_id,
                             Om_Symbol_Id symbol_id,
                             Om_Symbol_Id related_symbol_id,
                             Om_String_View name,
                             Om_Source_Span span)
{
    if (storage->entities.length >= UINT32_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Hir_Entity entity = {
        .id = {(uint32_t)storage->entities.length},
        .kind = kind,
        .module_id = module_id,
        .node_id = node_id,
        .symbol_id = symbol_id,
        .related_symbol_id = related_symbol_id,
        .name = name,
        .span = span,
    };
    return om_vector_push(&storage->entities, &entity);
}

static bool entity_kind_for_node(Om_Hir_Kind kind,
                                 Om_Hir_Entity_Kind *entity_kind)
{
    switch (kind) {
    case OM_HIR_FUNCTION: *entity_kind = OM_HIR_ENTITY_FUNCTION; return true;
    case OM_HIR_ACTION: *entity_kind = OM_HIR_ENTITY_ACTION; return true;
    case OM_HIR_RECORD: *entity_kind = OM_HIR_ENTITY_RECORD; return true;
    case OM_HIR_MESSAGE: *entity_kind = OM_HIR_ENTITY_MESSAGE; return true;
    case OM_HIR_ACTOR: *entity_kind = OM_HIR_ENTITY_ACTOR; return true;
    default: return false;
    }
}

static Om_Symbol_Id protocol_symbol_for_receive(
    const Om_Semantic_Analysis_Result *semantic,
    const Om_Hir_Node *receive,
    Om_Symbol_Id *actor_symbol)
{
    if (actor_symbol != NULL) {
        *actor_symbol = OM_SYMBOL_ID_INVALID;
    }
    for (size_t index = 0; index < semantic->actor_protocol_count; ++index) {
        const Om_Actor_Protocol_Entry *entry = &semantic->actor_protocol[index];
        if (entry->receive_node.value == receive->source_node.value
            && entry->receive_node.value != UINT32_MAX) {
            if (actor_symbol != NULL) {
                *actor_symbol = entry->actor_symbol;
            }
            return entry->message_symbol;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Status collect_entities(Om_Hir_Lower_Context *context,
                                  Om_Hir_Module *module)
{
    const Om_Hir_Node *root = om_vector_at_const(
        &context->storage->nodes, module->root.value);
    if (root == NULL || root->kind != OM_HIR_PROGRAM) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    module->entity_start = (uint32_t)context->storage->entities.length;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < root->child_count;
         ++index) {
        const Om_Hir_Node_Id id = ((const Om_Hir_Node_Id *)
            context->storage->children.data)[root->child_start + index];
        const Om_Hir_Node *node = om_vector_at_const(
            &context->storage->nodes, id.value);
        Om_Hir_Entity_Kind entity_kind = OM_HIR_ENTITY_FUNCTION;
        if (node == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (entity_kind_for_node(node->kind, &entity_kind)) {
            status = push_entity(context->storage, entity_kind, module->id,
                                 id, node->symbol_id,
                                 OM_SYMBOL_ID_INVALID, node->name, node->span);
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_ACTOR) {
            for (size_t child_index = 0; status == OM_STATUS_OK
                 && child_index < node->child_count; ++child_index) {
                const Om_Hir_Node_Id receive_id = ((const Om_Hir_Node_Id *)
                    context->storage->children.data)[node->child_start + child_index];
                const Om_Hir_Node *receive = om_vector_at_const(
                    &context->storage->nodes, receive_id.value);
                if (receive == NULL) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
                if (receive->kind == OM_HIR_RECEIVE) {
                    Om_Symbol_Id actor_symbol = OM_SYMBOL_ID_INVALID;
                    const Om_Symbol_Id message_symbol = protocol_symbol_for_receive(
                        context->semantic, receive, &actor_symbol);
                    const Om_Symbol *message = om_resolution_symbol_by_id(
                        context->resolution, message_symbol);
                    Om_Symbol_Id resolved_message_symbol = message_symbol;
                    if (message == NULL) {
                        resolved_message_symbol = receive->symbol_id;
                        actor_symbol = node->symbol_id;
                        message = om_resolution_symbol_by_id(
                            context->resolution, resolved_message_symbol);
                    }
                    status = push_entity(context->storage, OM_HIR_ENTITY_RECEIVE,
                        module->id, receive_id, resolved_message_symbol,
                        actor_symbol,
                        message != NULL ? message->name : receive->name,
                        receive->span);
                }
            }
        }
    }
    module->entity_count = (uint32_t)(context->storage->entities.length
                                      - module->entity_start);
    return status;
}

Om_Status om_hir_lower_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Hir_Project *result)
{
    if (project == NULL || parsed_modules == NULL || resolution == NULL
        || inference == NULL || semantic == NULL || result == NULL
        || result->owned_storage != NULL
        || parsed_module_count == 0
        || parsed_module_count != om_module_project_module_count(project)
        || resolution->diagnostic_count != 0 || inference->diagnostic_count != 0
        || semantic->diagnostic_count != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Hir_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Hir_Storage){0};
    Om_Status status = init_storage(storage);
    if (status != OM_STATUS_OK) {
        destroy_storage(storage);
        return status;
    }
    Om_Hir_Lower_Context context = {
        .project = project,
        .parsed = parsed_modules,
        .resolution = resolution,
        .inference = inference,
        .semantic = semantic,
        .storage = storage,
    };
    for (size_t index = 0; status == OM_STATUS_OK && index < parsed_module_count;
         ++index) {
        const Om_Module_Id module_id = {(uint32_t)index};
        const Om_Source_Text *source = om_module_project_source(project, module_id);
        const Om_Ast *ast = &parsed_modules[index].ast;
        const Om_Ast_Node *root = om_ast_get_node(ast, ast->root);
        if (source == NULL || root == NULL || root->kind != OM_AST_PROGRAM
            || source->bytes.length > UINT32_MAX
            || storage->modules.length >= UINT32_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        Om_Hir_Module module = {
            .id = module_id,
            .source_id = source->id,
            .canonical_path = source->canonical_path,
            .source_length = (uint32_t)source->bytes.length,
            .root = OM_HIR_NODE_ID_INVALID,
        };
        status = om_vector_push(&storage->modules, &module);
        if (status == OM_STATUS_OK) {
            status = lower_node(&context, module_id, ast, ast->root,
                                OM_AST_PROGRAM, &module.root);
        }
        if (status == OM_STATUS_OK) {
            status = collect_entities(&context, &module);
        }
        if (status == OM_STATUS_OK) {
            Om_Hir_Module *stored = om_vector_at(&storage->modules, index);
            if (stored == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                *stored = module;
            }
        }
    }
    if (status != OM_STATUS_OK) {
        destroy_storage(storage);
        return status;
    }
    result->modules = storage->modules.data;
    result->module_count = storage->modules.length;
    result->entities = storage->entities.data;
    result->entity_count = storage->entities.length;
    result->nodes = storage->nodes.data;
    result->node_count = storage->nodes.length;
    result->children = storage->children.data;
    result->child_count = storage->children.length;
    result->type_store = &inference->type_store;
    result->inference = inference;
    result->resolution = resolution;
    result->semantic = semantic;
    result->owned_storage = storage;
    return OM_STATUS_OK;
}

void om_hir_project_destroy(Om_Hir_Project *project)
{
    if (project == NULL) {
        return;
    }
    destroy_storage(storage_of(project));
    *project = (Om_Hir_Project){0};
}

const Om_Hir_Node *om_hir_get_node(const Om_Hir_Project *project,
                                    Om_Hir_Node_Id id)
{
    return project != NULL && id.value < project->node_count
        ? &project->nodes[id.value] : NULL;
}

const Om_Hir_Node *om_hir_child_at(const Om_Hir_Project *project,
                                    const Om_Hir_Node *node,
                                    size_t index)
{
    if (project == NULL || node == NULL || index >= node->child_count
        || node->child_start > project->child_count
        || index >= project->child_count - node->child_start) {
        return NULL;
    }
    return om_hir_get_node(project, project->children[node->child_start + index]);
}

static Om_Status push_diagnostic(Om_Vector *diagnostics,
                                 Om_Hir_Diagnostic_Code code,
                                 const Om_Hir_Node *node)
{
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Diagnostic diagnostic = {
        .code = code,
        .module_id = node->module_id,
        .node_id = node->id,
        .span = node->span,
    };
    return om_vector_push(diagnostics, &diagnostic);
}

static bool kind_requires_value_type(Om_Hir_Kind kind)
{
    switch (kind) {
    case OM_HIR_LOCAL:
    case OM_HIR_INTEGER:
    case OM_HIR_FLOAT:
    case OM_HIR_STRING:
    case OM_HIR_STRING_TEXT:
    case OM_HIR_INTERPOLATED_STRING:
    case OM_HIR_BOOLEAN:
    case OM_HIR_NULL:
    case OM_HIR_SELF:
    case OM_HIR_NOMINAL_VALUE:
    case OM_HIR_UNARY:
    case OM_HIR_TRY:
    case OM_HIR_BINARY:
    case OM_HIR_CALL_DIRECT:
    case OM_HIR_CALL_INDIRECT:
    case OM_HIR_RESULT_OK:
    case OM_HIR_RESULT_ERROR:
    case OM_HIR_RECORD_CONSTRUCT:
    case OM_HIR_MESSAGE_CONSTRUCT:
    case OM_HIR_FIELD_READ:
    case OM_HIR_SYSTEM_ACTOR_REF:
    case OM_HIR_INDEX:
    case OM_HIR_RECORD_WITH:
    case OM_HIR_SPAWN:
    case OM_HIR_GROUP:
    case OM_HIR_LIST:
    case OM_HIR_MAP:
        return true;
    default:
        return false;
    }
}

static bool kind_requires_symbol(Om_Hir_Kind kind)
{
    switch (kind) {
    case OM_HIR_RECORD:
    case OM_HIR_MESSAGE:
    case OM_HIR_FUNCTION:
    case OM_HIR_ACTION:
    case OM_HIR_ACTOR:
    case OM_HIR_RECEIVE:
    case OM_HIR_PARAMETER:
    case OM_HIR_BIND:
    case OM_HIR_DECLARE_VAR:
    case OM_HIR_ASSIGN_VAR:
    case OM_HIR_ACTOR_STATE_INIT:
    case OM_HIR_ACTOR_STATE_ASSIGN:
    case OM_HIR_LOCAL:
    case OM_HIR_SYMBOL_REF:
    case OM_HIR_SYSTEM_ACTOR_REF:
    case OM_HIR_CALL_DIRECT:
    case OM_HIR_CALL_INDIRECT:
    case OM_HIR_NOMINAL_VALUE:
    case OM_HIR_RESULT_OK:
    case OM_HIR_RESULT_ERROR:
    case OM_HIR_RECORD_CONSTRUCT:
    case OM_HIR_MESSAGE_CONSTRUCT:
        return true;
    default:
        return false;
    }
}

static bool hir_symbol_exists(const Om_Hir_Project *project,
                              Om_Symbol_Id symbol_id)
{
    return symbol_id.value != UINT32_MAX
        && om_resolution_symbol_by_id(project->resolution, symbol_id) != NULL;
}

static Om_Status resolved_type_view(const Om_Hir_Project *project,
                                    Om_Type_Id type_id,
                                    Om_Type_Id *resolved,
                                    Om_Type_View *view)
{
    Om_Status status = om_type_store_resolve(project->type_store, type_id,
                                             resolved);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(project->type_store, *resolved, view);
    }
    return status;
}

static bool node_within_actor(const Om_Hir_Project *project,
                              const Om_Hir_Node_Id *parents,
                              const Om_Hir_Node *node,
                              Om_Ast_Node_Id actor_declaration)
{
    Om_Hir_Node_Id parent_id = parents[node->id.value];
    size_t depth = 0;
    while (parent_id.value != UINT32_MAX && depth++ < project->node_count) {
        const Om_Hir_Node *parent = om_hir_get_node(project, parent_id);
        if (parent == NULL) {
            return false;
        }
        if (parent->kind == OM_HIR_ACTOR
            && parent->module_id.value == node->module_id.value
            && parent->source_node.value == actor_declaration.value) {
            return true;
        }
        parent_id = parents[parent->id.value];
    }
    return false;
}

static bool symbol_is_actor_state(const Om_Hir_Project *project,
                                  Om_Symbol_Id symbol_id,
                                  Om_Ast_Node_Id *actor_declaration)
{
    const Om_Symbol *symbol = om_resolution_symbol_by_id(project->resolution,
                                                         symbol_id);
    if (symbol == NULL || symbol->scope_id.value >= project->resolution->scope_count) {
        return false;
    }
    const Om_Scope *scope = &project->resolution->scopes[symbol->scope_id.value];
    if (scope->kind != OM_SCOPE_ACTOR) {
        return false;
    }
    if (actor_declaration != NULL) {
        *actor_declaration = scope->owner_node;
    }
    return true;
}

Om_Status om_hir_verify_project(const Om_Hir_Project *project,
                               Om_Hir_Verification_Result *result)
{
    if (project == NULL || result == NULL || result->owned_diagnostics != NULL
        || project->owned_storage == NULL || project->type_store == NULL
        || project->resolution == NULL || project->semantic == NULL
        || project->modules == NULL || project->module_count == 0
        || project->nodes == NULL || project->node_count == 0
        || project->resolution->symbols == NULL
        || project->resolution->scopes == NULL
        || (project->entity_count != 0 && project->entities == NULL)
        || (project->child_count != 0 && project->children == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector diagnostics = {0};
    Om_Status status = om_vector_init(&diagnostics, sizeof(Om_Hir_Diagnostic));
    Om_Hir_Node_Id *parents = NULL;
    uint8_t *module_roots = NULL;
    parents = om_alloc_array(project->node_count, sizeof(*parents));
    module_roots = om_alloc_array(project->node_count, sizeof(*module_roots));
    if (status == OM_STATUS_OK && (parents == NULL || module_roots == NULL)) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < project->node_count; ++index) {
            parents[index] = OM_HIR_NODE_ID_INVALID;
            module_roots[index] = 0;
        }
    }
    size_t expected_entity_start = 0;
    for (size_t index = 0; status == OM_STATUS_OK && index < project->module_count;
         ++index) {
        const Om_Hir_Module *module = &project->modules[index];
        const Om_Hir_Node *root = om_hir_get_node(project, module->root);
        if (module->id.value != index || module->source_id.value == UINT32_MAX
            || root == NULL || root->kind != OM_HIR_PROGRAM
            || root->module_id.value != module->id.value
            || module->entity_start != expected_entity_start
            || module->entity_start > project->entity_count
            || module->entity_count > project->entity_count - module->entity_start) {
            const Om_Hir_Node *diagnostic_node = root != NULL
                ? root : &project->nodes[0];
            status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901,
                                     diagnostic_node);
        } else if (module_roots[module->root.value] != 0) {
            status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901,
                                     root);
        } else {
            module_roots[module->root.value] = 1;
            expected_entity_start += module->entity_count;
        }
    }
    if (status == OM_STATUS_OK && expected_entity_start != project->entity_count) {
        status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901,
                                 &project->nodes[0]);
    }
    size_t expected_child_start = 0;
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
    for (size_t index = 0; status == OM_STATUS_OK && index < project->node_count;
         ++index) {
        const Om_Hir_Node *node = &project->nodes[index];
        if (node->id.value != index || node->module_id.value >= project->module_count
            || node->span.source.value
                != project->modules[node->module_id.value].source_id.value
            || !om_source_span_is_valid(node->span,
                project->modules[node->module_id.value].source_length)
            || node->child_start != expected_child_start
            || node->child_start > project->child_count
            || node->child_count > project->child_count - node->child_start) {
            status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901, node);
            continue;
        }
        expected_child_start += node->child_count;
        for (size_t child_index = 0; status == OM_STATUS_OK
             && child_index < node->child_count; ++child_index) {
            const Om_Hir_Node_Id child_id =
                project->children[node->child_start + child_index];
            const Om_Hir_Node *child = om_hir_get_node(project, child_id);
            if (child == NULL || child_id.value <= node->id.value
                || child->module_id.value != node->module_id.value) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0901, node);
            } else if (parents[child_id.value].value != UINT32_MAX) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0901, node);
            } else {
                parents[child_id.value] = node->id;
            }
        }
        if (status != OM_STATUS_OK) {
            continue;
        }
        if ((kind_requires_symbol(node->kind)
                && !hir_symbol_exists(project, node->symbol_id))
            || (node->symbol_id.value != UINT32_MAX
                && !hir_symbol_exists(project, node->symbol_id))
            || (node->related_symbol_id.value != UINT32_MAX
                && !hir_symbol_exists(project, node->related_symbol_id))) {
            status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0902, node);
        }
        if (status == OM_STATUS_OK && kind_requires_value_type(node->kind)) {
            Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
            Om_Type_View view = {0};
            if (node->type_id.value == UINT32_MAX) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0903, node);
            } else if (resolved_type_view(project, node->type_id,
                                          &resolved_type, &view) != OM_STATUS_OK) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0904, node);
            } else if (((node->kind == OM_HIR_RESULT_OK
                         || node->kind == OM_HIR_RESULT_ERROR)
                        && view.kind != OM_TYPE_RESULT)
                       || (node->kind == OM_HIR_RECORD_CONSTRUCT
                           && view.kind != OM_TYPE_RECORD)
                       || (node->kind == OM_HIR_MESSAGE_CONSTRUCT
                           && view.kind != OM_TYPE_MESSAGE)
                       || (node->kind == OM_HIR_SYSTEM_ACTOR_REF
                           && view.kind != OM_TYPE_ACTOR_REF)) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0904, node);
            }
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_FUNCTION
            && (node->effects & forbidden_function_effects) != 0) {
            status = push_diagnostic(&diagnostics,
                                     OM_HIR_DIAGNOSTIC_E0907, node);
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_NOMINAL_VALUE) {
            const Om_Symbol *type_symbol = om_resolution_symbol_by_id(
                project->resolution, node->symbol_id);
            Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
            Om_Type_View type_view = {0};
            uint64_t member_index = UINT64_MAX;
            if (type_symbol == NULL
                || type_symbol->kind != OM_SYMBOL_BUILTIN
                || type_symbol->builtin_id != OM_BUILTIN_ACTOR_DOWN_REASON
                || node->builtin_id != OM_BUILTIN_ACTOR_DOWN_REASON
                || !builtin_member_ordinal(node->builtin_member_id,
                                           &member_index)
                || member_index != node->integer_magnitude
                || resolved_type_view(project, node->type_id, &resolved_type,
                                      &type_view) != OM_STATUS_OK
                || type_view.kind != OM_TYPE_DOWN_REASON
                || type_view.nominal_symbol.value != node->symbol_id.value) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0904, node);
            }
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_TRY) {
            const Om_Hir_Node *input = node->child_count == 1
                ? om_hir_child_at(project, node, 0) : NULL;
            Om_Type_Id input_type = OM_TYPE_ID_INVALID;
            Om_Type_View input_view = {0};
            if (input == NULL
                || resolved_type_view(project, input->type_id, &input_type,
                                      &input_view) != OM_STATUS_OK
                || input_view.kind != OM_TYPE_RESULT) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0906, node);
            }
        }
        if (status == OM_STATUS_OK
            && (node->kind == OM_HIR_ACTOR_STATE_INIT
                || node->kind == OM_HIR_ACTOR_STATE_ASSIGN
                || (node->kind == OM_HIR_LOCAL
                    && symbol_is_actor_state(project, node->symbol_id, NULL)))) {
            Om_Ast_Node_Id actor_declaration = OM_AST_NODE_ID_INVALID;
            const Om_Hir_Node_Id parent_id = parents[node->id.value];
            const Om_Hir_Node *parent = om_hir_get_node(project, parent_id);
            const bool state_symbol = symbol_is_actor_state(
                project, node->symbol_id, &actor_declaration);
            const bool in_actor = state_symbol
                && node_within_actor(project, parents, node, actor_declaration);
            const bool direct_actor_init = node->kind != OM_HIR_ACTOR_STATE_INIT
                || (parent != NULL && parent->kind == OM_HIR_ACTOR
                    && parent->source_node.value == actor_declaration.value);
            if (!in_actor || !direct_actor_init) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0908, node);
            }
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_SEND) {
            const Om_Hir_Node *receiver = node->child_count == 2
                ? om_hir_child_at(project, node, 0) : NULL;
            const Om_Hir_Node *message = node->child_count == 2
                ? om_hir_child_at(project, node, 1) : NULL;
            Om_Symbol_Id actor_symbol = OM_SYMBOL_ID_INVALID;
            Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
            Om_Type_Id receiver_type = OM_TYPE_ID_INVALID;
            Om_Type_View receiver_view = {0};
            Om_Type_Id protocol_type = OM_TYPE_ID_INVALID;
            Om_Type_View protocol_view = {0};
            Om_Type_Id message_type = OM_TYPE_ID_INVALID;
            Om_Type_View message_view = {0};
            bool serializable = false;
            const bool send_types_valid = receiver != NULL && message != NULL
                && resolved_type_view(project, receiver->type_id,
                    &receiver_type, &receiver_view) == OM_STATUS_OK
                && receiver_view.kind == OM_TYPE_ACTOR_REF
                && receiver_view.child_count == 1
                && om_type_store_child_at(project->type_store, receiver_type, 0,
                    &protocol_type) == OM_STATUS_OK
                && resolved_type_view(project, protocol_type, &protocol_type,
                    &protocol_view) == OM_STATUS_OK
                && protocol_view.kind == OM_TYPE_ACTOR_PROTOCOL
                && protocol_view.nominal_symbol.value != UINT32_MAX
                && resolved_type_view(project, message->type_id, &message_type,
                    &message_view) == OM_STATUS_OK
                && message_view.kind == OM_TYPE_MESSAGE
                && message_view.nominal_symbol.value != UINT32_MAX;
            if (send_types_valid) {
                actor_symbol = protocol_view.nominal_symbol;
                message_symbol = message_view.nominal_symbol;
            }
            const bool serializability_known = send_types_valid
                && (om_semantic_type_is_serializable(project->semantic,
                        message->type_id, &serializable)
                    || om_semantic_type_is_serializable(project->semantic,
                        message_type, &serializable));
            if (node->child_count != 2 || !send_types_valid
                || node->related_symbol_id.value != actor_symbol.value
                || node->symbol_id.value != message_symbol.value
                || !om_semantic_actor_accepts(project->semantic, actor_symbol,
                                               message_symbol)
                || !serializability_known || !serializable) {
                status = push_diagnostic(&diagnostics,
                                         OM_HIR_DIAGNOSTIC_E0905, node);
            }
        }
    }
    if (status == OM_STATUS_OK && expected_child_start != project->child_count) {
        status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901,
                                 &project->nodes[0]);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < project->node_count;
         ++index) {
        const bool is_root = module_roots[index] != 0;
        if ((is_root && parents[index].value != UINT32_MAX)
            || (!is_root && parents[index].value == UINT32_MAX)) {
            status = push_diagnostic(&diagnostics, OM_HIR_DIAGNOSTIC_E0901,
                                     &project->nodes[index]);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->entity_count; ++index) {
        const Om_Hir_Entity *entity = &project->entities[index];
        const Om_Hir_Node *node = om_hir_get_node(project, entity->node_id);
        if (entity->id.value != index || node == NULL
            || entity->module_id.value >= project->module_count
            || entity->module_id.value != node->module_id.value
            || entity->symbol_id.value == UINT32_MAX
            || !hir_symbol_exists(project, entity->symbol_id)
            || (entity->related_symbol_id.value != UINT32_MAX
                && !hir_symbol_exists(project, entity->related_symbol_id))
            || (entity->kind == OM_HIR_ENTITY_FUNCTION
                && node->kind != OM_HIR_FUNCTION)
            || (entity->kind == OM_HIR_ENTITY_ACTION
                && node->kind != OM_HIR_ACTION)
            || (entity->kind == OM_HIR_ENTITY_RECORD
                && node->kind != OM_HIR_RECORD)
            || (entity->kind == OM_HIR_ENTITY_MESSAGE
                && node->kind != OM_HIR_MESSAGE)
            || (entity->kind == OM_HIR_ENTITY_ACTOR
                && node->kind != OM_HIR_ACTOR)
            || (entity->kind == OM_HIR_ENTITY_RECEIVE
                && (node->kind != OM_HIR_RECEIVE
                    || entity->related_symbol_id.value == UINT32_MAX
                    || !om_semantic_actor_accepts(project->semantic,
                        entity->related_symbol_id, entity->symbol_id)))
            || entity->kind > OM_HIR_ENTITY_RECEIVE) {
            status = push_diagnostic(&diagnostics,
                entity->symbol_id.value == UINT32_MAX
                    ? OM_HIR_DIAGNOSTIC_E0902 : OM_HIR_DIAGNOSTIC_E0901,
                node != NULL ? node : &project->nodes[0]);
        }
    }
    if (status == OM_STATUS_OK) {
        result->diagnostics = diagnostics.data;
        result->diagnostic_count = diagnostics.length;
        result->owned_diagnostics = diagnostics.data;
        diagnostics.data = NULL;
    }
    om_free(module_roots);
    om_free(parents);
    om_vector_destroy(&diagnostics);
    return status;
}

void om_hir_verification_result_destroy(Om_Hir_Verification_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->owned_diagnostics);
    *result = (Om_Hir_Verification_Result){0};
}

typedef struct Om_Hir_Text_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Hir_Text_Buffer;

static Om_Status text_append(Om_Hir_Text_Buffer *buffer,
                             const char *format,
                             ...)
{
    char temporary[512];
    va_list args;
    va_start(args, format);
    const int required_int = vsnprintf(temporary, sizeof(temporary), format, args);
    va_end(args);
    if (required_int < 0 || (size_t)required_int >= sizeof(temporary)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t required = (size_t)required_int;
    size_t with_terminator = 0;
    if (!om_checked_size_add(buffer->length, required, &with_terminator)
        || !om_checked_size_add(with_terminator, 1, &with_terminator)) {
        return OM_STATUS_OVERFLOW;
    }
    if (with_terminator > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
        while (capacity < with_terminator) {
            if (capacity > SIZE_MAX / 2) {
                capacity = with_terminator;
                break;
            }
            capacity *= 2;
        }
        char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
        if (data == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    (void)memcpy(buffer->data + buffer->length, temporary, required);
    buffer->length += required;
    buffer->data[buffer->length] = '\0';
    return OM_STATUS_OK;
}

static Om_Status text_append_escaped(Om_Hir_Text_Buffer *buffer,
                                    Om_String_View text)
{
    if (text.length != 0 && text.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = text_append(buffer, "\"");
    for (size_t index = 0; status == OM_STATUS_OK && index < text.length;
         ++index) {
        const unsigned char character = (unsigned char)text.data[index];
        if (character == (unsigned char)'\\') {
            status = text_append(buffer, "\\\\");
        } else if (character == (unsigned char)'\"') {
            status = text_append(buffer, "\\\"");
        } else if (character == (unsigned char)'\n') {
            status = text_append(buffer, "\\n");
        } else if (character == (unsigned char)'\r') {
            status = text_append(buffer, "\\r");
        } else if (character == (unsigned char)'\t') {
            status = text_append(buffer, "\\t");
        } else if (character < 0x20u || character == 0x7fu) {
            status = text_append(buffer, "\\x%02X", (unsigned int)character);
        } else {
            status = text_append(buffer, "%c", (int)character);
        }
    }
    if (status == OM_STATUS_OK) {
        status = text_append(buffer, "\"");
    }
    return status;
}

static const char *hir_operator_name(Om_Hir_Operator operator_kind)
{
    static const char *const names[] = {
        "none", "add", "subtract", "multiply", "divide", "remainder",
        "equal", "not-equal", "less", "greater", "less-equal",
        "greater-equal", "and", "or", "not", "negate",
    };
    return (size_t)operator_kind < sizeof(names) / sizeof(names[0])
        ? names[operator_kind] : "invalid-operator";
}

const char *om_hir_kind_name(Om_Hir_Kind kind)
{
    static const char *const names[] = {
        "program", "import", "record", "message", "function", "action",
        "actor", "receive", "parameter", "block", "bind", "declare-var",
        "assign-var", "actor-state-init", "actor-state-assign", "return",
        "break", "continue", "send", "watch", "stop", "call-statement",
        "if", "if-arm", "while", "for-range", "for-each", "select",
        "case-arm", "local", "integer", "float", "string", "string-text",
        "interpolated-string", "boolean", "null", "self", "nominal-value",
        "unary", "try", "binary", "call-direct", "call-indirect",
        "result-ok", "result-error", "record-construct", "message-construct",
        "field-read", "symbol-ref", "system-actor-ref", "index",
        "record-with", "field-update", "spawn", "group", "list", "map",
        "map-entry",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid-hir-kind";
}

const char *om_hir_diagnostic_code_name(Om_Hir_Diagnostic_Code code)
{
    switch (code) {
    case OM_HIR_DIAGNOSTIC_E0901: return "E0901";
    case OM_HIR_DIAGNOSTIC_E0902: return "E0902";
    case OM_HIR_DIAGNOSTIC_E0903: return "E0903";
    case OM_HIR_DIAGNOSTIC_E0904: return "E0904";
    case OM_HIR_DIAGNOSTIC_E0905: return "E0905";
    case OM_HIR_DIAGNOSTIC_E0906: return "E0906";
    case OM_HIR_DIAGNOSTIC_E0907: return "E0907";
    case OM_HIR_DIAGNOSTIC_E0908: return "E0908";
    default: return "E0000";
    }
}

Om_Status om_hir_dump(const Om_Hir_Project *project, Om_Hir_Text *result)
{
    if (project == NULL || result == NULL || project->owned_storage == NULL
        || result->data != NULL || result->length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Hir_Text_Buffer buffer = {0};
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < project->module_count; ++index) {
        const Om_Hir_Module *module = &project->modules[index];
        status = text_append(&buffer, "module %" PRIu32 " path=",
                            module->id.value);
        if (status == OM_STATUS_OK) {
            status = text_append_escaped(&buffer, module->canonical_path);
        }
        if (status == OM_STATUS_OK) {
            status = text_append(&buffer, "\n");
        }
        for (size_t entity_index = 0; status == OM_STATUS_OK
             && entity_index < module->entity_count; ++entity_index) {
            const Om_Hir_Entity *entity =
                &project->entities[module->entity_start + entity_index];
            status = text_append(&buffer,
                "  entity %" PRIu32 " kind=%u symbol=%" PRIu32 " related=%" PRIu32 " name=",
                entity->id.value, (unsigned)entity->kind, entity->symbol_id.value,
                entity->related_symbol_id.value);
            if (status == OM_STATUS_OK) {
                status = text_append_escaped(&buffer, entity->name);
            }
            if (status == OM_STATUS_OK) {
                status = text_append(&buffer, " span=%" PRIu32 ":%" PRIu32
                    "-%" PRIu32 "\n", entity->span.source.value,
                    entity->span.start, entity->span.end);
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < project->node_count;
         ++index) {
        const Om_Hir_Node *node = &project->nodes[index];
        status = text_append(&buffer,
            "node %" PRIu32 " module=%" PRIu32 " kind=%s op=%s"
            " type=%" PRIu32 " symbol=%" PRIu32 " related=%" PRIu32
            " effects=%" PRIu32 " flags=%" PRIu32 " span=%" PRIu32
            ":%" PRIu32 "-%" PRIu32 " children=%" PRIu32 ":%" PRIu32,
            node->id.value, node->module_id.value, om_hir_kind_name(node->kind),
            hir_operator_name(node->operator_kind), node->type_id.value,
            node->symbol_id.value, node->related_symbol_id.value, node->effects,
            node->flags, node->span.source.value, node->span.start,
            node->span.end, node->child_start, node->child_count);
        if (status == OM_STATUS_OK
            && node->kind == OM_HIR_NOMINAL_VALUE) {
            status = text_append(&buffer,
                " value=builtin::Actor_Down_Reason.%.*s : Actor_Down_Reason",
                (int)node->name.length, node->name.data);
        } else if (status == OM_STATUS_OK && node->name.length != 0) {
            status = text_append(&buffer, " name=");
            if (status == OM_STATUS_OK) {
                status = text_append_escaped(&buffer, node->name);
            }
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_INTEGER) {
            status = text_append(&buffer, " magnitude=%" PRIu64,
                                 node->integer_magnitude);
        }
        if (status == OM_STATUS_OK && node->kind == OM_HIR_FLOAT) {
            status = text_append(&buffer, " value=%.17g", node->float_value);
        }
        if (status == OM_STATUS_OK) {
            status = text_append(&buffer, "\n");
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(buffer.data);
        return status;
    }
    result->data = buffer.data;
    result->length = buffer.length;
    return OM_STATUS_OK;
}

void om_hir_text_destroy(Om_Hir_Text *text)
{
    if (text == NULL) {
        return;
    }
    om_free(text->data);
    *text = (Om_Hir_Text){0};
}
