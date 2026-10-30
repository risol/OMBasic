#include "ombasic/semantic/name_resolver.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/syntax/name_rules.h"

typedef struct Om_Resolver {
    const Om_Module_Project *project;
    const Om_Parser_Result *parsed_modules;
    const Om_Resolver_Config *config;
    size_t module_count;
    Om_Vector symbols;
    Om_Vector scopes;
    Om_Vector resolved_nodes;
    Om_Vector node_scopes;
    Om_Vector diagnostics;
    Om_Vector exports;
    Om_Scope_Id prelude_scope;
    Om_Scope_Id *module_scopes;
} Om_Resolver;

static const Om_Ast *module_ast(const Om_Resolver *resolver, Om_Module_Id module)
{
    if (module.value >= resolver->module_count) {
        return NULL;
    }
    return &resolver->parsed_modules[module.value].ast;
}

static const Om_Ast_Node *ast_node(const Om_Resolver *resolver,
                                   Om_Module_Id module,
                                   Om_Ast_Node_Id node_id)
{
    return om_ast_get_node(module_ast(resolver, module), node_id);
}

static Om_Ast_Node_Id ast_child(const Om_Resolver *resolver,
                                Om_Module_Id module,
                                const Om_Ast_Node *node,
                                size_t index)
{
    return om_ast_get_child(module_ast(resolver, module), node, index);
}

static const Om_Symbol *symbol_at(const Om_Resolver *resolver, Om_Symbol_Id id)
{
    if (id.value >= resolver->symbols.length) {
        return NULL;
    }
    return om_vector_at_const(&resolver->symbols, id.value);
}

static const Om_Scope *scope_at(const Om_Resolver *resolver, Om_Scope_Id id)
{
    if (id.value >= resolver->scopes.length) {
        return NULL;
    }
    return om_vector_at_const(&resolver->scopes, id.value);
}

static Om_Status add_diagnostic(Om_Resolver *resolver,
                                Om_Resolution_Diagnostic_Code code,
                                Om_Module_Id module,
                                Om_Source_Span span,
                                const Om_Source_Span *related)
{
    const Om_Resolution_Diagnostic diagnostic = {
        .code = code,
        .module_id = module,
        .span = span,
        .related_span = related != NULL ? *related :
                                         (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0},
        .has_related_span = related != NULL,
    };
    return om_vector_push(&resolver->diagnostics, &diagnostic);
}

static Om_Status add_scope(Om_Resolver *resolver,
                           Om_Scope_Id parent,
                           Om_Scope_Kind kind,
                           Om_Module_Id module,
                           Om_Ast_Node_Id owner,
                           Om_Scope_Id *out)
{
    if (resolver->scopes.length >= UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const Om_Scope scope = {
        .id = {(uint32_t)resolver->scopes.length},
        .parent_id = parent,
        .kind = kind,
        .module_id = module,
        .owner_node = owner,
    };
    const Om_Status status = om_vector_push(&resolver->scopes, &scope);
    if (status == OM_STATUS_OK) {
        *out = scope.id;
    }
    return status;
}

static Om_Status set_node_scope(Om_Resolver *resolver, Om_Module_Id module,
                                Om_Ast_Node_Id node, Om_Scope_Id scope)
{
    for (size_t index = 0; index < resolver->node_scopes.length; ++index) {
        Om_Node_Scope *existing = om_vector_at(&resolver->node_scopes, index);
        if (existing->module_id.value == module.value
            && existing->node_id.value == node.value) {
            existing->scope_id = scope;
            return OM_STATUS_OK;
        }
    }
    const Om_Node_Scope entry = {module, node, scope};
    return om_vector_push(&resolver->node_scopes, &entry);
}

static Om_Status set_node_symbol(Om_Resolver *resolver, Om_Module_Id module,
                                 Om_Ast_Node_Id node, Om_Symbol_Id symbol)
{
    if (symbol.value == UINT32_MAX) {
        return OM_STATUS_OK;
    }
    for (size_t index = 0; index < resolver->resolved_nodes.length; ++index) {
        Om_Resolved_Node *existing = om_vector_at(&resolver->resolved_nodes, index);
        if (existing->module_id.value == module.value
            && existing->node_id.value == node.value) {
            existing->symbol_id = symbol;
            return OM_STATUS_OK;
        }
    }
    const Om_Resolved_Node entry = {
        .module_id = module,
        .node_id = node,
        .symbol_id = symbol,
        .builtin_member_id = OM_BUILTIN_MEMBER_NONE,
    };
    return om_vector_push(&resolver->resolved_nodes, &entry);
}

static Om_Status set_node_builtin_member(Om_Resolver *resolver,
                                         Om_Module_Id module,
                                         Om_Ast_Node_Id node,
                                         Om_Symbol_Id symbol,
                                         Om_Builtin_Member_Id member)
{
    Om_Status status = set_node_symbol(resolver, module, node, symbol);
    if (status != OM_STATUS_OK) {
        return status;
    }
    for (size_t index = 0; index < resolver->resolved_nodes.length; ++index) {
        Om_Resolved_Node *entry = om_vector_at(&resolver->resolved_nodes, index);
        if (entry->module_id.value == module.value
            && entry->node_id.value == node.value) {
            entry->builtin_member_id = member;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Symbol_Id resolved_symbol(const Om_Resolver *resolver,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node)
{
    for (size_t index = 0; index < resolver->resolved_nodes.length; ++index) {
        const Om_Resolved_Node *entry = om_vector_at_const(
            &resolver->resolved_nodes, index);
        if (entry->module_id.value == module.value
            && entry->node_id.value == node.value) {
            return entry->symbol_id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Symbol_Id find_in_scope(const Om_Resolver *resolver,
                                  Om_Scope_Id scope,
                                  Om_String_View name,
                                  bool case_insensitive)
{
    for (size_t index = 0; index < resolver->symbols.length; ++index) {
        const Om_Symbol *symbol = om_vector_at_const(&resolver->symbols, index);
        if (symbol->scope_id.value != scope.value) {
            continue;
        }
        const bool same = case_insensitive
            ? om_name_equal_case_insensitive(symbol->name, name)
            : om_string_view_equal(symbol->name, name);
        if (same) {
            return symbol->id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Symbol_Id lookup_visible(const Om_Resolver *resolver,
                                   Om_Scope_Id start,
                                   Om_String_View name)
{
    Om_Scope_Id scope_id = start;
    while (scope_id.value != UINT32_MAX) {
        const Om_Symbol_Id found = find_in_scope(resolver, scope_id, name, false);
        if (found.value != UINT32_MAX) {
            return found;
        }
        const Om_Scope *scope = scope_at(resolver, scope_id);
        if (scope == NULL) {
            return OM_SYMBOL_ID_INVALID;
        }
        scope_id = scope->parent_id;
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Symbol_Id lookup_module_symbol(const Om_Resolver *resolver,
                                         Om_Module_Id module,
                                         Om_String_View name)
{
    if (module.value >= resolver->module_count) {
        return OM_SYMBOL_ID_INVALID;
    }
    return find_in_scope(resolver, resolver->module_scopes[module.value], name, false);
}

static Om_Symbol_Id lookup_export(const Om_Resolver *resolver,
                                  Om_Module_Id module, Om_String_View name)
{
    for (size_t index = 0; index < resolver->exports.length; ++index) {
        const Om_Module_Export *entry = om_vector_at_const(&resolver->exports, index);
        if (entry->module_id.value == module.value
            && om_string_view_equal(entry->name, name)) return entry->symbol_id;
    }
    return OM_SYMBOL_ID_INVALID;
}

static Om_Status add_module_export(Om_Resolver *resolver, Om_Module_Id module,
                                   Om_Symbol_Id symbol_id, Om_Source_Span via)
{
    const Om_Symbol *symbol = symbol_at(resolver, symbol_id);
    if (symbol == NULL || !symbol->exported) return OM_STATUS_OK;
    const Om_Symbol_Id previous_id = lookup_export(resolver, module, symbol->name);
    if (previous_id.value != UINT32_MAX) {
        if (previous_id.value == symbol_id.value) return OM_STATUS_OK;
        const Om_Symbol *previous = symbol_at(resolver, previous_id);
        return add_diagnostic(resolver, OM_RESOLUTION_DIAGNOSTIC_E0308, module,
                              via, previous != NULL ? &previous->span : NULL);
    }
    const Om_Module_Export entry = {module, symbol->name, symbol_id};
    return om_vector_push(&resolver->exports, &entry);
}

static Om_Status build_module_exports(Om_Resolver *resolver)
{
    Om_Status status = OM_STATUS_OK;
    /* Seed every module with its direct public declarations. */
    for (size_t index = 0; status == OM_STATUS_OK && index < resolver->symbols.length; ++index) {
        const Om_Symbol *symbol = om_vector_at_const(&resolver->symbols, index);
        if (symbol->kind != OM_SYMBOL_MODULE_ALIAS && symbol->exported)
            status = add_module_export(resolver, symbol->module_id, symbol->id, symbol->span);
    }
    /* Graph order places dependencies before importers, so one pass composes
       arbitrarily deep re-export chains without synthetic symbols. */
    for (size_t order = 0; status == OM_STATUS_OK && order < resolver->project->order.length; ++order) {
        const Om_Module_Id module = resolver->project->order.items[order];
        const Om_Ast *ast = module_ast(resolver, module);
        const Om_Ast_Node *program = om_ast_get_node(ast, ast->root);
        size_t import_index = 0;
        for (size_t child_index = 0; status == OM_STATUS_OK && child_index < program->child_count; ++child_index) {
            const Om_Ast_Node *child = om_ast_get_node(ast, ast_child(resolver, module, program, child_index));
            if (child == NULL || child->kind != OM_AST_IMPORT) continue;
            Om_Module_Import_Info info = {0};
            status = om_module_project_import_at(resolver->project, module, import_index++, &info);
            if (status != OM_STATUS_OK || !info.reexport) continue;
            for (size_t e = 0; status == OM_STATUS_OK && e < resolver->exports.length; ++e) {
                const Om_Module_Export source = *(const Om_Module_Export *)om_vector_at_const(&resolver->exports, e);
                if (source.module_id.value == info.target_module.value)
                    status = add_module_export(resolver, module, source.symbol_id, info.span);
            }
        }
    }
    return status;
}

static Om_Status create_symbol(Om_Resolver *resolver,
                               Om_Module_Id module,
                               Om_Scope_Id scope,
                               Om_String_View name,
                               Om_Symbol_Kind kind,
                               Om_Source_Span span,
                               Om_Ast_Node_Id declaration_node,
                               bool exported,
                               bool mutable_binding,
                               Om_Module_Id target_module,
                               Om_Symbol_Id *out)
{
    if (resolver->symbols.length >= UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const Om_Symbol symbol = {
        .id = {(uint32_t)resolver->symbols.length},
        .name = name,
        .kind = kind,
        .span = span,
        .scope_id = scope,
        .module_id = module,
        .target_module = target_module,
        .declaration_node = declaration_node,
        .exported = exported,
        .mutable_binding = mutable_binding,
    };
    const Om_Status status = om_vector_push(&resolver->symbols, &symbol);
    if (status == OM_STATUS_OK) {
        *out = symbol.id;
    }
    return status;
}

static Om_Status declare_symbol(Om_Resolver *resolver,
                                Om_Module_Id module,
                                Om_Scope_Id scope,
                                Om_String_View name,
                                Om_Symbol_Kind kind,
                                Om_Source_Span span,
                                Om_Ast_Node_Id declaration_node,
                                bool exported,
                                bool mutable_binding,
                                Om_Module_Id target_module,
                                Om_Symbol_Id *out)
{
    const Om_Symbol_Id local_exact = find_in_scope(resolver, scope, name, false);
    if (local_exact.value != UINT32_MAX) {
        const Om_Symbol *previous = symbol_at(resolver, local_exact);
        if (kind == OM_SYMBOL_MODULE_ALIAS && previous != NULL
            && previous->kind == OM_SYMBOL_MODULE_ALIAS
            && previous->target_module.value == target_module.value) {
            *out = local_exact;
            return set_node_symbol(resolver, module, declaration_node, local_exact);
        }
        const Om_Resolution_Diagnostic_Code code = kind == OM_SYMBOL_MODULE_ALIAS
            || (previous != NULL && previous->kind == OM_SYMBOL_MODULE_ALIAS)
                ? OM_RESOLUTION_DIAGNOSTIC_E0306
                : OM_RESOLUTION_DIAGNOSTIC_E0303;
        const Om_Status status = add_diagnostic(
            resolver, code, module, span,
            previous != NULL ? &previous->span : NULL);
        if (status != OM_STATUS_OK) {
            return status;
        }
        *out = local_exact;
        return set_node_symbol(resolver, module, declaration_node, local_exact);
    }
    const Om_Symbol_Id local_folded = find_in_scope(resolver, scope, name, true);
    if (local_folded.value != UINT32_MAX) {
        const Om_Symbol *previous = symbol_at(resolver, local_folded);
        const Om_Resolution_Diagnostic_Code code = kind == OM_SYMBOL_MODULE_ALIAS
            || (previous != NULL && previous->kind == OM_SYMBOL_MODULE_ALIAS)
                ? OM_RESOLUTION_DIAGNOSTIC_E0306
                : OM_RESOLUTION_DIAGNOSTIC_E0304;
        const Om_Status status = add_diagnostic(
            resolver, code, module, span,
            previous != NULL ? &previous->span : NULL);
        if (status != OM_STATUS_OK) {
            return status;
        }
        *out = local_folded;
        return set_node_symbol(resolver, module, declaration_node, local_folded);
    }

    const Om_Scope *current_scope = scope_at(resolver, scope);
    Om_Scope_Id ancestor = current_scope != NULL ? current_scope->parent_id
                                                 : OM_SCOPE_ID_INVALID;
    while (ancestor.value != UINT32_MAX) {
        const Om_Symbol_Id visible = find_in_scope(resolver, ancestor, name, false);
        const Om_Symbol_Id folded = visible.value != UINT32_MAX
            ? visible : find_in_scope(resolver, ancestor, name, true);
        if (folded.value != UINT32_MAX) {
            const Om_Symbol *previous = symbol_at(resolver, folded);
            const bool case_only = previous != NULL
                && !om_string_view_equal(previous->name, name);
            const Om_Resolution_Diagnostic_Code code = case_only
                ? OM_RESOLUTION_DIAGNOSTIC_E0304
                : OM_RESOLUTION_DIAGNOSTIC_E0305;
            const Om_Status status = add_diagnostic(
                resolver, code, module, span,
                previous != NULL ? &previous->span : NULL);
            if (status != OM_STATUS_OK) {
                return status;
            }
            *out = folded;
            return set_node_symbol(resolver, module, declaration_node, folded);
        }
        const Om_Scope *parent = scope_at(resolver, ancestor);
        if (parent == NULL) {
            break;
        }
        ancestor = parent->parent_id;
    }

    Om_Status status = create_symbol(resolver, module, scope, name, kind, span,
                                     declaration_node, exported, mutable_binding,
                                     target_module, out);
    if (status == OM_STATUS_OK) {
        status = set_node_symbol(resolver, module, declaration_node, *out);
    }
    return status;
}

static const Om_Builtin_Descriptor builtin_descriptors[] = {
#define OM_BUILTIN(id, spelling, category, type_rule, lowering, rule) \
    {id, {spelling, sizeof(spelling) - 1}, category, \
     {type_rule, sizeof(type_rule) - 1}, \
     {lowering, sizeof(lowering) - 1}, {rule, sizeof(rule) - 1}},
#include "ombasic/semantic/builtin_registry.def"
#undef OM_BUILTIN
};

static const Om_Builtin_Member_Descriptor builtin_member_descriptors[] = {
#define OM_BUILTIN_MEMBER(id, owner, spelling, rule, semantic_value) \
    {id, owner, {spelling, sizeof(spelling) - 1}, \
     {rule, sizeof(rule) - 1}, \
     {semantic_value, sizeof(semantic_value) - 1}},
#include "ombasic/semantic/builtin_member_registry.def"
#undef OM_BUILTIN_MEMBER
};

size_t om_builtin_descriptor_count(void)
{
    return sizeof(builtin_descriptors) / sizeof(builtin_descriptors[0]);
}

const Om_Builtin_Descriptor *om_builtin_descriptor_at(size_t index)
{
    return index < om_builtin_descriptor_count()
        ? &builtin_descriptors[index] : NULL;
}

size_t om_builtin_member_descriptor_count(void)
{
    return sizeof(builtin_member_descriptors)
        / sizeof(builtin_member_descriptors[0]);
}

const Om_Builtin_Member_Descriptor *om_builtin_member_descriptor_at(size_t index)
{
    return index < om_builtin_member_descriptor_count()
        ? &builtin_member_descriptors[index] : NULL;
}

static Om_Builtin_Id builtin_id_for_name(Om_String_View name)
{
    for (size_t index = 0; index < om_builtin_descriptor_count(); ++index) {
        if (om_string_view_equal(name, builtin_descriptors[index].spelling)) {
            return builtin_descriptors[index].id;
        }
    }
    return OM_BUILTIN_NONE;
}

bool om_builtin_id_is_system_actor(Om_Builtin_Id id)
{
    return id == OM_BUILTIN_SYSTEM_ACTOR_CONSOLE
        || id == OM_BUILTIN_SYSTEM_ACTOR_TIMER
        || id == OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM
        || id == OM_BUILTIN_SYSTEM_ACTOR_CLOCK
        || id == OM_BUILTIN_SYSTEM_ACTOR_SECURE_RANDOM
        || id == OM_BUILTIN_SYSTEM_ACTOR_LOGGER
        || id == OM_BUILTIN_SYSTEM_ACTOR_NETWORK
        || id == OM_BUILTIN_SYSTEM_ACTOR_PROCESS
        || id == OM_BUILTIN_SYSTEM_ACTOR_ENVIRONMENT;
}

bool om_builtin_id_is_system_message(Om_Builtin_Id id)
{
    return id >= OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE
        && id <= OM_BUILTIN_SYSTEM_MESSAGE_TLS_CLOSE_COMPLETED;
}

static Om_Status add_builtin(Om_Resolver *resolver, Om_String_View name)
{
    Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
    Om_Status status = create_symbol(
        resolver, OM_MODULE_ID_INVALID, resolver->prelude_scope, name,
        OM_SYMBOL_BUILTIN,
        (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0},
        OM_AST_NODE_ID_INVALID, true, false, OM_MODULE_ID_INVALID, &symbol);
    if (status == OM_STATUS_OK) {
        Om_Symbol *entry = om_vector_at(&resolver->symbols, symbol.value);
        if (entry == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        entry->builtin_id = builtin_id_for_name(name);
    }
    return status;
}

static bool source_native_abi_type_supported(Om_Native_Abi_Type type,
                                             bool is_result)
{
    if (type == OM_NATIVE_ABI_CVOID) {
        return is_result;
    }
    switch (type) {
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
    case OM_NATIVE_ABI_CFLOAT:
    case OM_NATIVE_ABI_CDOUBLE:
    case OM_NATIVE_ABI_CSIZE:
    case OM_NATIVE_ABI_CBOOL:
    case OM_NATIVE_ABI_CPOINTER:
    case OM_NATIVE_ABI_CSTRING:
    case OM_NATIVE_ABI_CUTF8_SLICE:
    case OM_NATIVE_ABI_RECORD:
        return true;
    case OM_NATIVE_ABI_INVALID:
    case OM_NATIVE_ABI_CVOID:
        return false;
    }
    return false;
}

static bool native_function_source_callable(const Om_Omi_Module *module,
                                            size_t function_index)
{
    if (module == NULL || function_index >= module->function_count
        || module->functions == NULL) {
        return false;
    }
    const Om_Omi_Native_Function *function =
        &module->functions[function_index];
    if (function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || (function->parameter_count != 0 && module->parameters == NULL)
        || (function->result_type == OM_NATIVE_ABI_RECORD
            && (function->result_record_index >= module->record_count
                || module->records == NULL))
        || !source_native_abi_type_supported(function->result_type, true)) {
        return false;
    }
    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter =
            &module->parameters[function->parameter_start + index];
        if (!parameter->is_callback) {
            if (!source_native_abi_type_supported(parameter->abi_type, false)
                || (parameter->abi_type == OM_NATIVE_ABI_RECORD
                    && (parameter->record_index >= module->record_count
                        || module->records == NULL))) {
                return false;
            }
            continue;
        }
        if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
            || parameter->callback_parameter_start
                > module->callback_parameter_count
            || parameter->callback_parameter_count
                > module->callback_parameter_count
                    - parameter->callback_parameter_start
            || (parameter->callback_parameter_count != 0
                && module->callback_parameters == NULL)) {
            return false;
        }
        for (size_t callback_index = 0;
             callback_index < parameter->callback_parameter_count;
             ++callback_index) {
            const Om_Native_Abi_Type callback_type =
                module->callback_parameters[
                    parameter->callback_parameter_start + callback_index]
                    .abi_type;
            if (!source_native_abi_type_supported(callback_type, false)
                || callback_type == OM_NATIVE_ABI_CSTRING
                || callback_type == OM_NATIVE_ABI_CUTF8_SLICE
                || callback_type == OM_NATIVE_ABI_RECORD) {
                return false;
            }
        }
    }
    return true;
}

static Om_Status add_native_builtin(Om_Resolver *resolver,
                                   const Om_Omi_Module *module,
                                   size_t function_index)
{
    if (module == NULL || function_index >= module->function_count
        || module->functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (!native_function_source_callable(module, function_index)) {
        return OM_STATUS_OK;
    }
    const Om_Omi_Native_Function *function =
        &module->functions[function_index];
    if (function->name.data == NULL || function->name.length == 0
        || om_name_classify(function->name) == OM_NAME_CLASS_INVALID
        || find_in_scope(resolver, resolver->prelude_scope, function->name,
                         true).value != UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Symbol_Id symbol_id = OM_SYMBOL_ID_INVALID;
    Om_Status status = create_symbol(
        resolver, OM_MODULE_ID_INVALID, resolver->prelude_scope,
        function->name, OM_SYMBOL_BUILTIN,
        (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0},
        OM_AST_NODE_ID_INVALID, true, false, OM_MODULE_ID_INVALID,
        &symbol_id);
    if (status == OM_STATUS_OK) {
        Om_Symbol *symbol = om_vector_at(&resolver->symbols, symbol_id.value);
        if (symbol == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        symbol->native_module = module;
        symbol->native_function_index = function_index;
    }
    return status;
}

static Om_Status add_native_record(Om_Resolver *resolver,
                                   const Om_Omi_Module *module,
                                   size_t record_index)
{
    if (resolver == NULL || module == NULL
        || record_index >= module->record_count || module->records == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Native_Record *record = &module->records[record_index];
    if (record->name.data == NULL || record->name.length == 0
        || om_name_classify(record->name) == OM_NAME_CLASS_INVALID
        || find_in_scope(resolver, resolver->prelude_scope, record->name,
                         true).value != UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Symbol_Id symbol_id = OM_SYMBOL_ID_INVALID;
    Om_Status status = create_symbol(
        resolver, OM_MODULE_ID_INVALID, resolver->prelude_scope,
        record->name, OM_SYMBOL_RECORD,
        (Om_Source_Span){OM_SOURCE_ID_INVALID, 0, 0},
        OM_AST_NODE_ID_INVALID, true, false, OM_MODULE_ID_INVALID,
        &symbol_id);
    if (status == OM_STATUS_OK) {
        Om_Symbol *symbol = om_vector_at(&resolver->symbols, symbol_id.value);
        if (symbol == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        symbol->native_module = module;
        symbol->native_record_index = record_index;
    }
    return status;
}

static Om_Status setup_prelude(Om_Resolver *resolver,
                               const Om_Resolver_Config *config)
{
    static const char *const builtin_names[] = {
        "Unit", "Result", "Node_Ref", "Actor_Id", "Actor_Down_Reason",
        "Ok", "Error", "Actor_Down", "print",
        OM_SYSTEM_ACTOR_NAME_CONSOLE, OM_SYSTEM_ACTOR_NAME_TIMER,
        OM_SYSTEM_ACTOR_NAME_FILE_SYSTEM,
        OM_SYSTEM_ACTOR_NAME_CLOCK,
        OM_SYSTEM_ACTOR_NAME_SECURE_RANDOM,
        OM_SYSTEM_ACTOR_NAME_LOGGER,
        OM_SYSTEM_ACTOR_NAME_NETWORK,
        OM_SYSTEM_ACTOR_NAME_PROCESS,
        OM_SYSTEM_ACTOR_NAME_ENVIRONMENT,
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE,
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_WRITE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TIMER_AFTER,
        OM_SYSTEM_MESSAGE_NAME_FILE_APPEND,
        OM_SYSTEM_MESSAGE_NAME_FILE_APPEND_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_EXISTS,
        OM_SYSTEM_MESSAGE_NAME_FILE_EXISTS_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_DELETE,
        OM_SYSTEM_MESSAGE_NAME_FILE_DELETE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_DIRECTORY_CREATE,
        OM_SYSTEM_MESSAGE_NAME_DIRECTORY_CREATE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_DIRECTORY_LIST,
        OM_SYSTEM_MESSAGE_NAME_DIRECTORY_LIST_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_STAT,
        OM_SYSTEM_MESSAGE_NAME_FILE_STAT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ_BYTES,
        OM_SYSTEM_MESSAGE_NAME_FILE_READ_BYTES_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_BYTES,
        OM_SYSTEM_MESSAGE_NAME_FILE_WRITE_BYTES_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_FILE_APPEND_BYTES,
        OM_SYSTEM_MESSAGE_NAME_FILE_APPEND_BYTES_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_CLOCK_MONOTONIC_NOW,
        OM_SYSTEM_MESSAGE_NAME_CLOCK_MONOTONIC_NOW_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_CLOCK_UTC_NOW,
        OM_SYSTEM_MESSAGE_NAME_CLOCK_UTC_NOW_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_READ_LINE,
        OM_SYSTEM_MESSAGE_NAME_CONSOLE_READ_LINE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_SECURE_RANDOM_BYTES,
        OM_SYSTEM_MESSAGE_NAME_SECURE_RANDOM_BYTES_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_LOGGER_WRITE,
        OM_SYSTEM_MESSAGE_NAME_LOGGER_WRITE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_LOGGER_FLUSH,
        OM_SYSTEM_MESSAGE_NAME_LOGGER_FLUSH_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_LISTEN,
        OM_SYSTEM_MESSAGE_NAME_TCP_LISTEN_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_CONNECT,
        OM_SYSTEM_MESSAGE_NAME_TCP_CONNECT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_ACCEPT,
        OM_SYSTEM_MESSAGE_NAME_TCP_ACCEPT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_SEND,
        OM_SYSTEM_MESSAGE_NAME_TCP_SEND_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_RECEIVE,
        OM_SYSTEM_MESSAGE_NAME_TCP_RECEIVE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TCP_CLOSE,
        OM_SYSTEM_MESSAGE_NAME_TCP_CLOSE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_SPAWN,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_SPAWN_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_WRITE_STDIN,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_WRITE_STDIN_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_CLOSE_STDIN,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_CLOSE_STDIN_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_READ_STDOUT,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_READ_STDOUT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_READ_STDERR,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_READ_STDERR_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_TERMINATE,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_TERMINATE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_WAIT,
        OM_SYSTEM_MESSAGE_NAME_PROCESS_WAIT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_UDP_CONNECT,
        OM_SYSTEM_MESSAGE_NAME_UDP_CONNECT_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_UDP_SEND,
        OM_SYSTEM_MESSAGE_NAME_UDP_SEND_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_UDP_RECEIVE,
        OM_SYSTEM_MESSAGE_NAME_UDP_RECEIVE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_UDP_CLOSE,
        OM_SYSTEM_MESSAGE_NAME_UDP_CLOSE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_ARGUMENTS,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_ARGUMENTS_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_GET,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_GET_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_CURRENT_DIRECTORY,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_CURRENT_DIRECTORY_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_EXECUTABLE_PATH,
        OM_SYSTEM_MESSAGE_NAME_ENVIRONMENT_EXECUTABLE_PATH_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_DNS_RESOLVE,
        OM_SYSTEM_MESSAGE_NAME_DNS_RESOLVE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TLS_CLIENT_START,
        OM_SYSTEM_MESSAGE_NAME_TLS_CLIENT_START_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TLS_SEND,
        OM_SYSTEM_MESSAGE_NAME_TLS_SEND_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TLS_RECEIVE,
        OM_SYSTEM_MESSAGE_NAME_TLS_RECEIVE_COMPLETED,
        OM_SYSTEM_MESSAGE_NAME_TLS_CLOSE,
        OM_SYSTEM_MESSAGE_NAME_TLS_CLOSE_COMPLETED
    };
    Om_Status status = add_scope(resolver, OM_SCOPE_ID_INVALID, OM_SCOPE_PRELUDE,
                                 OM_MODULE_ID_INVALID, OM_AST_NODE_ID_INVALID,
                                 &resolver->prelude_scope);
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < sizeof(builtin_names) / sizeof(builtin_names[0]);
         ++index) {
        size_t length = 0;
        while (builtin_names[index][length] != '\0') {
            ++length;
        }
        status = add_builtin(resolver, (Om_String_View){builtin_names[index], length});
    }
    if (status == OM_STATUS_OK && config != NULL
        && config->native_module_count != 0
        && config->native_modules == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t module_index = 0; status == OM_STATUS_OK && config != NULL
                                  && module_index < config->native_module_count;
         ++module_index) {
        const Om_Omi_Module *module = &config->native_modules[module_index];
        if (module->diagnostic_count != 0
            || (module->function_count != 0 && module->functions == NULL)
            || (module->record_count != 0 && module->records == NULL)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t record_index = 0;
             status == OM_STATUS_OK && record_index < module->record_count;
             ++record_index) {
            status = add_native_record(resolver, module, record_index);
        }
        for (size_t function_index = 0;
             status == OM_STATUS_OK && function_index < module->function_count;
             ++function_index) {
            status = add_native_builtin(resolver, module, function_index);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && config != NULL
                           && index < config->prelude_name_count; ++index) {
        const Om_String_View name = config->prelude_names[index];
        if (om_name_classify(name) == OM_NAME_CLASS_INVALID) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Symbol_Id duplicate = find_in_scope(
            resolver, resolver->prelude_scope, name, true);
        if (duplicate.value != UINT32_MAX) {
            continue;
        }
        status = add_builtin(resolver, name);
    }
    return status;
}

static Om_String_View module_alias_name(const Om_Module_Project *project,
                                        Om_Module_Id module)
{
    Om_String_View path = om_module_graph_path(&project->graph, module);
    size_t start = 0;
    size_t end = path.length;
    for (size_t index = 0; index < path.length; ++index) {
        if (path.data[index] == '/') {
            start = index + 1;
        }
    }
    if (end >= start + 4 && path.data[end - 4] == '.'
        && path.data[end - 3] == 'b' && path.data[end - 2] == 'a'
        && path.data[end - 1] == 's') {
        end -= 4;
    }
    return (Om_String_View){path.data != NULL ? path.data + start : NULL,
                            end >= start ? end - start : 0};
}

static Om_Symbol_Kind symbol_kind_for_declaration(Om_Ast_Kind kind)
{
    switch (kind) {
    case OM_AST_FUNCTION_DECLARATION: return OM_SYMBOL_FUNCTION;
    case OM_AST_ACTION_DECLARATION: return OM_SYMBOL_ACTION;
    case OM_AST_RECORD_DECLARATION: return OM_SYMBOL_RECORD;
    case OM_AST_MESSAGE_DECLARATION: return OM_SYMBOL_MESSAGE;
    case OM_AST_ACTOR_DECLARATION: return OM_SYMBOL_ACTOR;
    default: return OM_SYMBOL_BUILTIN;
    }
}

static Om_Status predeclare_module(Om_Resolver *resolver, Om_Module_Id module)
{
    const Om_Ast *ast = module_ast(resolver, module);
    const Om_Ast_Node *program = om_ast_get_node(ast, ast->root);
    size_t import_index = 0;
    Om_Status status = OM_STATUS_OK;
    if (program == NULL || program->kind != OM_AST_PROGRAM) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < program->child_count; ++index) {
        const Om_Ast_Node_Id child_id = ast_child(resolver, module, program, index);
        const Om_Ast_Node *child = om_ast_get_node(ast, child_id);
        if (child == NULL || child->kind != OM_AST_IMPORT) {
            continue;
        }
        Om_Module_Import_Info import_info = {0};
        status = om_module_project_import_at(resolver->project, module,
                                             import_index, &import_info);
        if (status != OM_STATUS_OK) {
            return status;
        }
        ++import_index;
        const Om_String_View alias = module_alias_name(resolver->project,
                                                       import_info.target_module);
        if (om_name_classify(alias) != OM_NAME_CLASS_ORDINARY) {
            status = add_diagnostic(resolver, OM_RESOLUTION_DIAGNOSTIC_E0306,
                                    module, child->span, NULL);
            continue;
        }
        Om_Symbol_Id alias_symbol = OM_SYMBOL_ID_INVALID;
        status = declare_symbol(resolver, module,
                                resolver->module_scopes[module.value], alias,
                                OM_SYMBOL_MODULE_ALIAS, child->span, child_id,
                                false, false, import_info.target_module,
                                &alias_symbol);
    }
    if (status == OM_STATUS_OK
        && import_index != om_module_project_import_count(resolver->project, module)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t native_index = 0; status == OM_STATUS_OK
                                  && resolver->config != NULL
                                  && native_index < resolver->config->module_native_import_count;
         ++native_index) {
        const Om_Module_Native_Import *native_import =
            &resolver->config->module_native_imports[native_index];
        if (native_import->module_id.value != module.value
            || native_import->native_module == NULL) {
            continue;
        }
        const Om_Omi_Module *native_module = native_import->native_module;
        const Om_Scope_Id scope = resolver->module_scopes[module.value];
        for (size_t record_index = 0; status == OM_STATUS_OK
                                      && record_index < native_module->record_count;
             ++record_index) {
            const Om_Omi_Native_Record *record = &native_module->records[record_index];
            if (record->name.data == NULL
                || om_name_classify(record->name) == OM_NAME_CLASS_INVALID) {
                continue;
            }
            const bool has_existing =
                find_in_scope(resolver, scope, record->name, false).value
                    != UINT32_MAX
                || find_in_scope(resolver, scope, record->name, true).value
                    != UINT32_MAX;
            Om_Symbol_Id symbol_id = OM_SYMBOL_ID_INVALID;
            status = declare_symbol(resolver, module, scope, record->name,
                OM_SYMBOL_RECORD, native_import->import_span,
                OM_AST_NODE_ID_INVALID, false, false, OM_MODULE_ID_INVALID,
                &symbol_id);
            if (status == OM_STATUS_OK && !has_existing
                && symbol_id.value < resolver->symbols.length) {
                Om_Symbol *symbol = om_vector_at(&resolver->symbols, symbol_id.value);
                symbol->native_module = native_module;
                symbol->native_record_index = record_index;
            }
        }
        for (size_t function_index = 0; status == OM_STATUS_OK
                                        && function_index < native_module->function_count;
             ++function_index) {
            if (!native_function_source_callable(native_module, function_index)) {
                continue;
            }
            const Om_Omi_Native_Function *function =
                &native_module->functions[function_index];
            if (function->name.data == NULL
                || om_name_classify(function->name) == OM_NAME_CLASS_INVALID) {
                continue;
            }
            const bool has_existing =
                find_in_scope(resolver, scope, function->name, false).value
                    != UINT32_MAX
                || find_in_scope(resolver, scope, function->name, true).value
                    != UINT32_MAX;
            Om_Symbol_Id symbol_id = OM_SYMBOL_ID_INVALID;
            status = declare_symbol(resolver, module, scope, function->name,
                OM_SYMBOL_BUILTIN, native_import->import_span,
                OM_AST_NODE_ID_INVALID, false, false, OM_MODULE_ID_INVALID,
                &symbol_id);
            if (status == OM_STATUS_OK && !has_existing
                && symbol_id.value < resolver->symbols.length) {
                Om_Symbol *symbol = om_vector_at(&resolver->symbols, symbol_id.value);
                symbol->native_module = native_module;
                symbol->native_function_index = function_index;
            }
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < program->child_count; ++index) {
        const Om_Ast_Node_Id child_id = ast_child(resolver, module, program, index);
        const Om_Ast_Node *child = om_ast_get_node(ast, child_id);
        if (child == NULL || child->kind == OM_AST_IMPORT
            || child->kind == OM_AST_NATIVE_IMPORT) {
            continue;
        }
        const Om_Symbol_Kind kind = symbol_kind_for_declaration(child->kind);
        if (kind == OM_SYMBOL_BUILTIN || child->name.data == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
        status = declare_symbol(
            resolver, module, resolver->module_scopes[module.value], child->name,
            kind, child->span, child_id,
            (child->flags & OM_AST_FLAG_EXPORTED) != 0,
            false, OM_MODULE_ID_INVALID, &symbol);
    }
    return status;
}

static Om_Status resolve_expression(Om_Resolver *resolver,
                                   Om_Module_Id module,
                                   Om_Ast_Node_Id node_id,
                                   Om_Scope_Id scope);
static Om_Status resolve_block(Om_Resolver *resolver,
                               Om_Module_Id module,
                               Om_Ast_Node_Id block_id,
                               Om_Scope_Id parent_scope);
static Om_Status resolve_statement(Om_Resolver *resolver,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id statement_id,
                                  Om_Scope_Id scope);

static Om_Status report_undefined(Om_Resolver *resolver, Om_Module_Id module,
                                  Om_Source_Span span)
{
    return add_diagnostic(resolver, OM_RESOLUTION_DIAGNOSTIC_E0301,
                          module, span, NULL);
}

static Om_Source_Span member_name_span(const Om_Ast_Node *member)
{
    Om_Source_Span span = member->span;
    const size_t span_length = (size_t)(span.end - span.start);
    if (member->name.length <= span_length) {
        span.start = span.end - (uint32_t)member->name.length;
    }
    return span;
}

static Om_Status resolve_name(Om_Resolver *resolver,
                              Om_Module_Id module,
                              Om_Ast_Node_Id node_id,
                              Om_Scope_Id scope)
{
    const Om_Ast_Node *node = ast_node(resolver, module, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Symbol_Id symbol = lookup_visible(resolver, scope, node->name);
    if (symbol.value == UINT32_MAX) {
        return report_undefined(resolver, module, node->span);
    }
    return set_node_symbol(resolver, module, node_id, symbol);
}

static Om_Status resolve_module_member(Om_Resolver *resolver,
                                       Om_Module_Id module,
                                       Om_Ast_Node_Id member_id,
                                       const Om_Ast_Node *member,
                                       Om_Scope_Id scope)
{
    const Om_Ast_Node_Id receiver_id = ast_child(resolver, module, member, 0);
    const Om_Symbol_Id receiver_symbol = resolved_symbol(resolver, module, receiver_id);
    const Om_Symbol *receiver = symbol_at(resolver, receiver_symbol);
    if (receiver == NULL) {
        return OM_STATUS_OK;
    }
    if (receiver->kind == OM_SYMBOL_BUILTIN
        && receiver->builtin_id == OM_BUILTIN_ACTOR_DOWN_REASON) {
        for (size_t index = 0;
             index < om_builtin_member_descriptor_count(); ++index) {
            const Om_Builtin_Member_Descriptor *builtin_member =
                &builtin_member_descriptors[index];
            if (builtin_member->owner == receiver->builtin_id
                && om_string_view_equal(member->name,
                                        builtin_member->spelling)) {
                return set_node_builtin_member(
                    resolver, module, member_id, receiver_symbol,
                    builtin_member->id);
            }
        }
        return report_undefined(resolver, module, member_name_span(member));
    }
    if (receiver->kind != OM_SYMBOL_MODULE_ALIAS) {
        return OM_STATUS_OK;
    }
    Om_Symbol_Id target = lookup_export(
        resolver, receiver->target_module, member->name);
    if (target.value == UINT32_MAX) target = lookup_module_symbol(
        resolver, receiver->target_module, member->name);
    if (target.value == UINT32_MAX) {
        return report_undefined(resolver, module, member_name_span(member));
    }
    const Om_Symbol *symbol = symbol_at(resolver, target);
    if (symbol == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (!symbol->exported && symbol->kind != OM_SYMBOL_BUILTIN) {
        const Om_Status status = add_diagnostic(
            resolver, OM_RESOLUTION_DIAGNOSTIC_E0302, module,
            member_name_span(member), &symbol->span);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    (void)scope;
    return set_node_symbol(resolver, module, member_id, target);
}

static Om_Status resolve_actor_spawn(Om_Resolver *resolver,
                                     Om_Module_Id module,
                                     Om_Ast_Node_Id node_id,
                                     const Om_Ast_Node *node,
                                     Om_Scope_Id scope)
{
    Om_Symbol_Id symbol_id = OM_SYMBOL_ID_INVALID;
    const size_t placement_start = node->child_count == 0 ? 0 : 1;
    Om_Status status = OM_STATUS_OK;
    if (node->child_count != 0) {
        const Om_Ast_Node_Id actor_id = ast_child(resolver, module, node, 0);
        status = resolve_expression(resolver, module, actor_id, scope);
        if (status != OM_STATUS_OK) {
            return status;
        }
        symbol_id = resolved_symbol(resolver, module, actor_id);
    } else {
        symbol_id = lookup_visible(resolver, scope, node->name);
    }
    const Om_Symbol *symbol = symbol_at(resolver, symbol_id);
    if (symbol == NULL || symbol->kind != OM_SYMBOL_ACTOR) {
        status = report_undefined(resolver, module, node->span);
    } else {
        status = set_node_symbol(resolver, module, node_id, symbol_id);
    }
    for (size_t index = placement_start;
         status == OM_STATUS_OK && index < node->child_count; ++index) {
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, index), scope);
    }
    return status;
}

static Om_Status resolve_expression(Om_Resolver *resolver,
                                    Om_Module_Id module,
                                    Om_Ast_Node_Id node_id,
                                    Om_Scope_Id scope)
{
    const Om_Ast_Node *node = ast_node(resolver, module, node_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = set_node_scope(resolver, module, node_id, scope);
    if (status != OM_STATUS_OK) {
        return status;
    }
    switch (node->kind) {
    case OM_AST_NAME:
        return resolve_name(resolver, module, node_id, scope);
    case OM_AST_MEMBER:
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
        if (status == OM_STATUS_OK) {
            status = resolve_module_member(resolver, module, node_id, node, scope);
        }
        return status;
    case OM_AST_SPAWN:
        return resolve_actor_spawn(resolver, module, node_id, node, scope);
    case OM_AST_FIELD_UPDATE:
        if (node->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return resolve_expression(resolver, module,
                                 ast_child(resolver, module, node, 0), scope);
    case OM_AST_MAP_ENTRY:
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
        if (status == OM_STATUS_OK) {
            status = resolve_expression(resolver, module,
                                        ast_child(resolver, module, node, 1), scope);
        }
        return status;
    case OM_AST_PROGRAM:
    case OM_AST_IMPORT:
    case OM_AST_NATIVE_IMPORT:
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
    default:
        break;
    }
    if (node->kind == OM_AST_WITH) {
        if (node->child_count == 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
        for (size_t index = 1; status == OM_STATUS_OK && index < node->child_count; ++index) {
            const Om_Ast_Node_Id update_id = ast_child(resolver, module, node, index);
            status = resolve_expression(resolver, module, update_id, scope);
        }
        return status;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count; ++index) {
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, index), scope);
    }
    return status;
}

static Om_Status resolve_binding(Om_Resolver *resolver,
                                 Om_Module_Id module,
                                 Om_Ast_Node_Id node_id,
                                 const Om_Ast_Node *node,
                                 Om_Scope_Id scope)
{
    if (node->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Ast_Node_Id value_id = ast_child(resolver, module, node, 0);
    Om_Status status = resolve_expression(resolver, module, value_id, scope);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const bool is_mutable = (node->flags & OM_AST_FLAG_MUTABLE) != 0;
    const Om_Symbol_Id existing = lookup_visible(resolver, scope, node->name);
    if (!is_mutable && existing.value != UINT32_MAX) {
        const Om_Symbol *symbol = symbol_at(resolver, existing);
        if (symbol == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (!symbol->mutable_binding) {
            status = add_diagnostic(resolver, OM_RESOLUTION_DIAGNOSTIC_E0307,
                                    module, node->span, &symbol->span);
        }
        if (status == OM_STATUS_OK) {
            status = set_node_symbol(resolver, module, node_id, existing);
        }
        return status;
    }
    Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
    return declare_symbol(
        resolver, module, scope, node->name,
        is_mutable ? OM_SYMBOL_MUTABLE_BINDING : OM_SYMBOL_IMMUTABLE_BINDING,
        node->span, node_id, false, is_mutable, OM_MODULE_ID_INVALID, &symbol);
}

static Om_Status resolve_block(Om_Resolver *resolver,
                               Om_Module_Id module,
                               Om_Ast_Node_Id block_id,
                               Om_Scope_Id parent_scope)
{
    const Om_Ast_Node *block = ast_node(resolver, module, block_id);
    if (block == NULL || block->kind != OM_AST_BLOCK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Scope_Id scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, parent_scope, OM_SCOPE_BLOCK, module,
                                 block_id, &scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, block_id, scope);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < block->child_count; ++index) {
        status = resolve_statement(resolver, module,
                                  ast_child(resolver, module, block, index), scope);
    }
    return status;
}

static Om_Status resolve_if(Om_Resolver *resolver,
                            Om_Module_Id module,
                            Om_Ast_Node_Id node_id,
                            const Om_Ast_Node *node,
                            Om_Scope_Id scope)
{
    const size_t arm_count = node->auxiliary;
    const bool has_else = (node->flags & OM_AST_FLAG_HAS_ELSE) != 0;
    if (node->child_count != arm_count + (has_else ? 1u : 0u)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = set_node_scope(resolver, module, node_id, scope);
    for (size_t index = 0; status == OM_STATUS_OK && index < arm_count; ++index) {
        const Om_Ast_Node_Id arm_id = ast_child(resolver, module, node, index);
        const Om_Ast_Node *arm = ast_node(resolver, module, arm_id);
        if (arm == NULL || arm->kind != OM_AST_IF_ARM || arm->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = set_node_scope(resolver, module, arm_id, scope);
        if (status == OM_STATUS_OK) {
            status = resolve_expression(resolver, module,
                                        ast_child(resolver, module, arm, 0), scope);
        }
        if (status == OM_STATUS_OK) {
            status = resolve_block(resolver, module,
                                   ast_child(resolver, module, arm, 1), scope);
        }
    }
    if (status == OM_STATUS_OK && has_else) {
        status = resolve_block(resolver, module,
                               ast_child(resolver, module, node, arm_count), scope);
    }
    return status;
}

static Om_Status resolve_for(Om_Resolver *resolver,
                             Om_Module_Id module,
                             Om_Ast_Node_Id node_id,
                             const Om_Ast_Node *node,
                             Om_Scope_Id scope)
{
    if (node->child_count < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t header_count = node->child_count - 1;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < header_count; ++index) {
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, index), scope);
    }
    Om_Scope_Id loop_scope = OM_SCOPE_ID_INVALID;
    if (status == OM_STATUS_OK) {
        status = add_scope(resolver, scope, OM_SCOPE_LOOP, module, node_id, &loop_scope);
    }
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, node_id, loop_scope);
    }
    if (status == OM_STATUS_OK) {
        Om_Symbol_Id loop_symbol = OM_SYMBOL_ID_INVALID;
        status = declare_symbol(resolver, module, loop_scope, node->name,
                                OM_SYMBOL_LOOP_VARIABLE, node->span, node_id,
                                false, false, OM_MODULE_ID_INVALID, &loop_symbol);
    }
    if (status == OM_STATUS_OK) {
        status = resolve_block(resolver, module,
                               ast_child(resolver, module, node, node->child_count - 1),
                               loop_scope);
    }
    return status;
}

static bool is_result_binding_pattern(const Om_Resolver *resolver,
                                      Om_Module_Id module,
                                      Om_Ast_Node_Id pattern_id,
                                      Om_Ast_Node_Id *constructor_id,
                                      Om_Ast_Node_Id *binding_id)
{
    const Om_Ast_Node *call = ast_node(resolver, module, pattern_id);
    if (call == NULL || call->kind != OM_AST_CALL || call->child_count != 2) {
        return false;
    }
    const Om_Ast_Node_Id callee_id = ast_child(resolver, module, call, 0);
    const Om_Ast_Node *callee = ast_node(resolver, module, callee_id);
    const Om_Ast_Node_Id argument_id = ast_child(resolver, module, call, 1);
    const Om_Ast_Node *argument = ast_node(resolver, module, argument_id);
    if (callee == NULL || callee->kind != OM_AST_NAME || argument == NULL
        || argument->kind != OM_AST_NAME
        || ((callee->name.length != 2
             || memcmp(callee->name.data, "Ok", 2) != 0)
            && (callee->name.length != 5
                || memcmp(callee->name.data, "Error", 5) != 0))) {
        return false;
    }
    *constructor_id = callee_id;
    *binding_id = argument_id;
    return true;
}

static Om_Status resolve_case_arm(Om_Resolver *resolver,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id arm_id,
                                  const Om_Ast_Node *arm,
                                  Om_Scope_Id parent_scope)
{
    Om_Scope_Id arm_scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, parent_scope, OM_SCOPE_SELECT_ARM,
                                 module, arm_id, &arm_scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, arm_id, arm_scope);
    }
    const bool is_else = (arm->flags & OM_AST_FLAG_CASE_ELSE) != 0;
    if (status == OM_STATUS_OK && !is_else) {
        if (arm->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Ast_Node_Id pattern_id = ast_child(resolver, module, arm, 0);
        Om_Ast_Node_Id constructor = OM_AST_NODE_ID_INVALID;
        Om_Ast_Node_Id binding = OM_AST_NODE_ID_INVALID;
        if (is_result_binding_pattern(resolver, module, pattern_id,
                                      &constructor, &binding)) {
            status = resolve_expression(resolver, module, constructor, arm_scope);
            const Om_Ast_Node *binding_node = ast_node(resolver, module, binding);
            if (status == OM_STATUS_OK && binding_node != NULL) {
                Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
                status = declare_symbol(resolver, module, arm_scope,
                                        binding_node->name,
                                        OM_SYMBOL_IMMUTABLE_BINDING,
                                        binding_node->span, binding, false,
                                        false, OM_MODULE_ID_INVALID, &symbol);
                if (status == OM_STATUS_OK) {
                    status = set_node_scope(resolver, module, binding, arm_scope);
                }
            }
        } else {
            status = resolve_expression(resolver, module, pattern_id, arm_scope);
        }
        if (status == OM_STATUS_OK) {
            status = resolve_block(resolver, module,
                                   ast_child(resolver, module, arm, 1), arm_scope);
        }
    } else if (status == OM_STATUS_OK) {
        if (arm->child_count != 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_block(resolver, module,
                               ast_child(resolver, module, arm, 0), arm_scope);
    }
    return status;
}

static Om_Status resolve_select(Om_Resolver *resolver,
                                Om_Module_Id module,
                                Om_Ast_Node_Id node_id,
                                const Om_Ast_Node *node,
                                Om_Scope_Id scope)
{
    if (node->child_count == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = set_node_scope(resolver, module, node_id, scope);
    if (status == OM_STATUS_OK) {
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
    }
    for (size_t index = 1; status == OM_STATUS_OK && index < node->child_count; ++index) {
        const Om_Ast_Node_Id arm_id = ast_child(resolver, module, node, index);
        const Om_Ast_Node *arm = ast_node(resolver, module, arm_id);
        if (arm == NULL || arm->kind != OM_AST_CASE_ARM) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_case_arm(resolver, module, arm_id, arm, scope);
    }
    return status;
}

static Om_Status resolve_statement(Om_Resolver *resolver,
                                   Om_Module_Id module,
                                   Om_Ast_Node_Id statement_id,
                                   Om_Scope_Id scope)
{
    const Om_Ast_Node *node = ast_node(resolver, module, statement_id);
    if (node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = set_node_scope(resolver, module, statement_id, scope);
    if (status != OM_STATUS_OK) {
        return status;
    }
    switch (node->kind) {
    case OM_AST_BINDING:
        return resolve_binding(resolver, module, statement_id, node, scope);
    case OM_AST_RETURN:
    case OM_AST_WATCH:
    case OM_AST_CALL_STATEMENT:
        if (node->child_count > 1) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return node->child_count == 0 ? OM_STATUS_OK
            : resolve_expression(resolver, module,
                                 ast_child(resolver, module, node, 0), scope);
    case OM_AST_SEND:
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
        if (status == OM_STATUS_OK) {
            status = resolve_expression(resolver, module,
                                        ast_child(resolver, module, node, 1), scope);
        }
        return status;
    case OM_AST_IF:
        return resolve_if(resolver, module, statement_id, node, scope);
    case OM_AST_WHILE:
        if (node->child_count != 2) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = resolve_expression(resolver, module,
                                    ast_child(resolver, module, node, 0), scope);
        if (status == OM_STATUS_OK) {
            status = resolve_block(resolver, module,
                                   ast_child(resolver, module, node, 1), scope);
        }
        return status;
    case OM_AST_FOR:
        return resolve_for(resolver, module, statement_id, node, scope);
    case OM_AST_SELECT:
        return resolve_select(resolver, module, statement_id, node, scope);
    case OM_AST_BREAK:
    case OM_AST_CONTINUE:
    case OM_AST_STOP:
        return OM_STATUS_OK;
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status resolve_parameters(Om_Resolver *resolver,
                                    Om_Module_Id module,
                                    const Om_Ast_Node *declaration,
                                    Om_Scope_Id scope,
                                    size_t parameter_count)
{
    if (declaration->child_count < parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < parameter_count; ++index) {
        const Om_Ast_Node_Id parameter_id = ast_child(
            resolver, module, declaration, index);
        const Om_Ast_Node *parameter = ast_node(resolver, module, parameter_id);
        if (parameter == NULL || parameter->kind != OM_AST_PARAMETER) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
        const Om_Status status = declare_symbol(
            resolver, module, scope, parameter->name, OM_SYMBOL_PARAMETER,
            parameter->span, parameter_id, false, false,
            OM_MODULE_ID_INVALID, &symbol);
        if (status != OM_STATUS_OK) {
            return status;
        }
        const Om_Status scope_status = set_node_scope(resolver, module,
                                                      parameter_id, scope);
        if (scope_status != OM_STATUS_OK) {
            return scope_status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status resolve_callable(Om_Resolver *resolver,
                                  Om_Module_Id module,
                                  Om_Ast_Node_Id declaration_id,
                                  const Om_Ast_Node *declaration,
                                  Om_Scope_Id module_scope,
                                  Om_Scope_Kind kind)
{
    const size_t parameter_count = declaration->auxiliary;
    if (declaration->child_count != parameter_count + 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Scope_Id callable_scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, module_scope, kind, module,
                                 declaration_id, &callable_scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, declaration_id, callable_scope);
    }
    if (status == OM_STATUS_OK) {
        status = resolve_parameters(resolver, module, declaration,
                                    callable_scope, parameter_count);
    }
    if (status == OM_STATUS_OK) {
        status = resolve_block(resolver, module,
                               ast_child(resolver, module, declaration,
                                         parameter_count), callable_scope);
    }
    return status;
}

static Om_Status resolve_fields(Om_Resolver *resolver,
                                Om_Module_Id module,
                                Om_Ast_Node_Id declaration_id,
                                const Om_Ast_Node *declaration,
                                Om_Scope_Kind kind)
{
    Om_Scope_Id field_scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, OM_SCOPE_ID_INVALID, kind, module,
                                 declaration_id, &field_scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, declaration_id, field_scope);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < declaration->child_count; ++index) {
        const Om_Ast_Node_Id field_id = ast_child(resolver, module, declaration, index);
        const Om_Ast_Node *field = ast_node(resolver, module, field_id);
        if (field == NULL || field->kind != OM_AST_PARAMETER) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
        status = declare_symbol(resolver, module, field_scope, field->name,
                                OM_SYMBOL_FIELD, field->span, field_id, false,
                                false, OM_MODULE_ID_INVALID, &symbol);
        if (status == OM_STATUS_OK) {
            status = set_node_scope(resolver, module, field_id, field_scope);
        }
    }
    return status;
}

static Om_Status resolve_actor_state(Om_Resolver *resolver,
                                     Om_Module_Id module,
                                     Om_Ast_Node_Id state_id,
                                     const Om_Ast_Node *state,
                                     Om_Scope_Id actor_scope)
{
    if (state->child_count != 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = resolve_expression(resolver, module,
                                          ast_child(resolver, module, state, 0),
                                          actor_scope);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Symbol_Id symbol = OM_SYMBOL_ID_INVALID;
    status = declare_symbol(resolver, module, actor_scope, state->name,
                            OM_SYMBOL_MUTABLE_BINDING, state->span, state_id,
                            false, true, OM_MODULE_ID_INVALID, &symbol);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, state_id, actor_scope);
    }
    return status;
}

static Om_Status resolve_receive(Om_Resolver *resolver,
                                 Om_Module_Id module,
                                 Om_Ast_Node_Id receive_id,
                                 const Om_Ast_Node *receive,
                                 Om_Scope_Id actor_scope)
{
    Om_Symbol_Id message_id = OM_SYMBOL_ID_INVALID;
    const char *separator = receive->name.data != NULL
        ? memchr(receive->name.data, '.', receive->name.length) : NULL;
    if (separator == NULL) {
        message_id = lookup_visible(resolver, actor_scope, receive->name);
    } else {
        const size_t alias_length = (size_t)(separator - receive->name.data);
        const size_t member_offset = alias_length + 1;
        if (alias_length != 0 && member_offset < receive->name.length) {
            const Om_String_View alias_name = {
                receive->name.data, alias_length,
            };
            const Om_String_View member_name = {
                receive->name.data + member_offset,
                receive->name.length - member_offset,
            };
            const Om_Symbol_Id alias_id = lookup_visible(
                resolver, actor_scope, alias_name);
            const Om_Symbol *alias = symbol_at(resolver, alias_id);
            if (alias != NULL && alias->kind == OM_SYMBOL_MODULE_ALIAS) {
                message_id = lookup_export(resolver, alias->target_module,
                                           member_name);
            }
        }
    }
    const Om_Symbol *message = symbol_at(resolver, message_id);
    const bool system_message = message != NULL
        && message->kind == OM_SYMBOL_BUILTIN
        && (message->builtin_id == OM_BUILTIN_ACTOR_DOWN
            || om_builtin_id_is_system_message(message->builtin_id));
    if (message == NULL
        || (message->kind != OM_SYMBOL_MESSAGE && !system_message)) {
        Om_Status status = report_undefined(resolver, module, receive->span);
        if (status != OM_STATUS_OK) {
            return status;
        }
    } else {
        Om_Status status = set_node_symbol(resolver, module, receive_id, message_id);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    const size_t parameter_count = receive->auxiliary;
    if (receive->child_count != parameter_count + 1) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Scope_Id receive_scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, actor_scope, OM_SCOPE_RECEIVE,
                                 module, receive_id, &receive_scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, receive_id, receive_scope);
    }
    if (status == OM_STATUS_OK) {
        status = resolve_parameters(resolver, module, receive,
                                    receive_scope, parameter_count);
    }
    if (status == OM_STATUS_OK) {
        status = resolve_block(resolver, module,
                               ast_child(resolver, module, receive,
                                         parameter_count), receive_scope);
    }
    return status;
}

static Om_Status resolve_actor(Om_Resolver *resolver,
                               Om_Module_Id module,
                               Om_Ast_Node_Id declaration_id,
                               const Om_Ast_Node *declaration,
                               Om_Scope_Id module_scope)
{
    Om_Scope_Id actor_scope = OM_SCOPE_ID_INVALID;
    Om_Status status = add_scope(resolver, module_scope, OM_SCOPE_ACTOR,
                                 module, declaration_id, &actor_scope);
    if (status == OM_STATUS_OK) {
        status = set_node_scope(resolver, module, declaration_id, actor_scope);
    }
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < declaration->child_count; ++index) {
        const Om_Ast_Node_Id member_id = ast_child(
            resolver, module, declaration, index);
        const Om_Ast_Node *member = ast_node(resolver, module, member_id);
        if (member == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (member->kind == OM_AST_BINDING) {
            status = resolve_actor_state(resolver, module, member_id,
                                         member, actor_scope);
        } else if (member->kind == OM_AST_RECEIVE_DECLARATION) {
            status = resolve_receive(resolver, module, member_id,
                                     member, actor_scope);
        } else {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    return status;
}

static Om_Status resolve_declaration(Om_Resolver *resolver,
                                     Om_Module_Id module,
                                     Om_Ast_Node_Id declaration_id,
                                     Om_Scope_Id module_scope)
{
    const Om_Ast_Node *declaration = ast_node(resolver, module, declaration_id);
    if (declaration == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    switch (declaration->kind) {
    case OM_AST_FUNCTION_DECLARATION:
        return resolve_callable(resolver, module, declaration_id,
                                declaration, module_scope, OM_SCOPE_FUNCTION);
    case OM_AST_ACTION_DECLARATION:
        return resolve_callable(resolver, module, declaration_id,
                                declaration, module_scope, OM_SCOPE_ACTION);
    case OM_AST_RECORD_DECLARATION:
        return resolve_fields(resolver, module, declaration_id,
                              declaration, OM_SCOPE_RECORD);
    case OM_AST_MESSAGE_DECLARATION:
        return resolve_fields(resolver, module, declaration_id,
                              declaration, OM_SCOPE_MESSAGE);
    case OM_AST_ACTOR_DECLARATION:
        return resolve_actor(resolver, module, declaration_id,
                             declaration, module_scope);
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status resolve_module(Om_Resolver *resolver, Om_Module_Id module)
{
    const Om_Ast *ast = module_ast(resolver, module);
    const Om_Ast_Node *program = om_ast_get_node(ast, ast->root);
    if (program == NULL || program->kind != OM_AST_PROGRAM) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < program->child_count; ++index) {
        const Om_Ast_Node_Id declaration_id = ast_child(resolver, module, program, index);
        const Om_Ast_Node *declaration = om_ast_get_node(ast, declaration_id);
        if (declaration == NULL || declaration->kind == OM_AST_IMPORT
            || declaration->kind == OM_AST_NATIVE_IMPORT) {
            continue;
        }
        const Om_Status status = resolve_declaration(
            resolver, module, declaration_id, resolver->module_scopes[module.value]);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

void om_resolution_result_destroy(Om_Resolution_Result *result)
{
    if (result == NULL) {
        return;
    }
    om_free(result->owned_symbols);
    om_free(result->owned_scopes);
    om_free(result->owned_resolved_nodes);
    om_free(result->owned_node_scopes);
    om_free(result->owned_diagnostics);
    om_free(result->owned_exports);
    *result = (Om_Resolution_Result){0};
}

Om_Status om_name_resolve_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolver_Config *config,
    Om_Resolution_Result *result
)
{
    Om_Resolver resolver = {0};
    Om_Status status = OM_STATUS_OK;
    if (project == NULL || project->owned_storage == NULL || parsed_modules == NULL
        || result == NULL || result->symbols != NULL || result->scopes != NULL
        || result->resolved_nodes != NULL || result->node_scopes != NULL
        || result->diagnostics != NULL || result->owned_symbols != NULL
        || result->owned_scopes != NULL || result->owned_resolved_nodes != NULL
        || result->owned_node_scopes != NULL || result->owned_diagnostics != NULL
        || result->exports != NULL || result->owned_exports != NULL
        || (config != NULL && config->prelude_name_count != 0
            && config->prelude_names == NULL)
        || (config != NULL && config->module_native_import_count != 0
            && config->module_native_imports == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    resolver.project = project;
    resolver.parsed_modules = parsed_modules;
    resolver.config = config;
    resolver.module_count = om_module_project_module_count(project);
    if (resolver.module_count == 0 || parsed_module_count != resolver.module_count
        || resolver.module_count > UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < resolver.module_count; ++index) {
        const Om_Source_Text *source = om_module_project_source(
            project, (Om_Module_Id){(uint32_t)index});
        const Om_Ast_Node *root = om_ast_get_node(&parsed_modules[index].ast,
                                                  parsed_modules[index].ast.root);
        if (source == NULL || root == NULL
            || root->span.source.value != (uint32_t)index
            || parsed_modules[index].diagnostic_count != 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    status = om_vector_init(&resolver.symbols, sizeof(Om_Symbol));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&resolver.scopes, sizeof(Om_Scope));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&resolver.resolved_nodes, sizeof(Om_Resolved_Node));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&resolver.node_scopes, sizeof(Om_Node_Scope));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&resolver.diagnostics,
                                sizeof(Om_Resolution_Diagnostic));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&resolver.exports, sizeof(Om_Module_Export));
    }
    if (status == OM_STATUS_OK) {
        resolver.module_scopes = om_alloc_array(resolver.module_count,
                                                sizeof(*resolver.module_scopes));
        if (resolver.module_scopes == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < resolver.module_count; ++index) {
        resolver.module_scopes[index] = OM_SCOPE_ID_INVALID;
    }
    if (status == OM_STATUS_OK) {
        status = setup_prelude(&resolver, config);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < resolver.module_count; ++index) {
        Om_Scope_Id module_scope = OM_SCOPE_ID_INVALID;
        status = add_scope(&resolver, resolver.prelude_scope, OM_SCOPE_MODULE,
                           (Om_Module_Id){(uint32_t)index},
                           parsed_modules[index].ast.root, &module_scope);
        if (status == OM_STATUS_OK) {
            resolver.module_scopes[index] = module_scope;
            status = set_node_scope(&resolver, (Om_Module_Id){(uint32_t)index},
                                    parsed_modules[index].ast.root, module_scope);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < resolver.module_count; ++index) {
        status = predeclare_module(&resolver, (Om_Module_Id){(uint32_t)index});
    }
    if (status == OM_STATUS_OK) status = build_module_exports(&resolver);
    for (size_t index = 0; status == OM_STATUS_OK && index < resolver.module_count; ++index) {
        status = resolve_module(&resolver, (Om_Module_Id){(uint32_t)index});
    }
    om_free(resolver.module_scopes);
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&resolver.symbols);
        om_vector_destroy(&resolver.scopes);
        om_vector_destroy(&resolver.resolved_nodes);
        om_vector_destroy(&resolver.node_scopes);
        om_vector_destroy(&resolver.diagnostics);
        om_vector_destroy(&resolver.exports);
        return status;
    }
    result->symbols = resolver.symbols.data;
    result->symbol_count = resolver.symbols.length;
    result->scopes = resolver.scopes.data;
    result->scope_count = resolver.scopes.length;
    result->resolved_nodes = resolver.resolved_nodes.data;
    result->resolved_node_count = resolver.resolved_nodes.length;
    result->node_scopes = resolver.node_scopes.data;
    result->node_scope_count = resolver.node_scopes.length;
    result->diagnostics = resolver.diagnostics.data;
    result->diagnostic_count = resolver.diagnostics.length;
    result->exports = resolver.exports.data;
    result->export_count = resolver.exports.length;
    result->owned_symbols = resolver.symbols.data;
    result->owned_scopes = resolver.scopes.data;
    result->owned_resolved_nodes = resolver.resolved_nodes.data;
    result->owned_node_scopes = resolver.node_scopes.data;
    result->owned_diagnostics = resolver.diagnostics.data;
    result->owned_exports = resolver.exports.data;
    return OM_STATUS_OK;
}

Om_Symbol_Id om_resolution_symbol_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
)
{
    if (result == NULL) {
        return OM_SYMBOL_ID_INVALID;
    }
    for (size_t index = 0; index < result->resolved_node_count; ++index) {
        const Om_Resolved_Node *entry = &result->resolved_nodes[index];
        if (entry->module_id.value == module_id.value
            && entry->node_id.value == node_id.value) {
            return entry->symbol_id;
        }
    }
    return OM_SYMBOL_ID_INVALID;
}

Om_Builtin_Member_Id om_resolution_builtin_member_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id)
{
    if (result == NULL) {
        return OM_BUILTIN_MEMBER_NONE;
    }
    for (size_t index = 0; index < result->resolved_node_count; ++index) {
        const Om_Resolved_Node *entry = &result->resolved_nodes[index];
        if (entry->module_id.value == module_id.value
            && entry->node_id.value == node_id.value) {
            return entry->builtin_member_id;
        }
    }
    return OM_BUILTIN_MEMBER_NONE;
}

Om_Scope_Id om_resolution_scope_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
)
{
    if (result == NULL) {
        return OM_SCOPE_ID_INVALID;
    }
    for (size_t index = 0; index < result->node_scope_count; ++index) {
        const Om_Node_Scope *entry = &result->node_scopes[index];
        if (entry->module_id.value == module_id.value
            && entry->node_id.value == node_id.value) {
            return entry->scope_id;
        }
    }
    return OM_SCOPE_ID_INVALID;
}

const Om_Symbol *om_resolution_symbol_by_id(
    const Om_Resolution_Result *result,
    Om_Symbol_Id symbol_id
)
{
    if (result == NULL || symbol_id.value >= result->symbol_count) {
        return NULL;
    }
    return &result->symbols[symbol_id.value];
}

size_t om_resolution_module_export_count(const Om_Resolution_Result *result,
                                         Om_Module_Id module_id)
{
    size_t count = 0;
    if (result == NULL) return 0;
    for (size_t index = 0; index < result->export_count; ++index) {
        if (result->exports[index].module_id.value == module_id.value) ++count;
    }
    return count;
}

const Om_Module_Export *om_resolution_module_export_at(
    const Om_Resolution_Result *result, Om_Module_Id module_id, size_t requested)
{
    if (result == NULL) return NULL;
    for (size_t index = 0; index < result->export_count; ++index) {
        const Om_Module_Export *entry = &result->exports[index];
        if (entry->module_id.value == module_id.value) {
            if (requested == 0) return entry;
            --requested;
        }
    }
    return NULL;
}

const char *om_resolution_diagnostic_code_name(Om_Resolution_Diagnostic_Code code)
{
    switch (code) {
    case OM_RESOLUTION_DIAGNOSTIC_E0301: return "E0301";
    case OM_RESOLUTION_DIAGNOSTIC_E0302: return "E0302";
    case OM_RESOLUTION_DIAGNOSTIC_E0303: return "E0303";
    case OM_RESOLUTION_DIAGNOSTIC_E0304: return "E0304";
    case OM_RESOLUTION_DIAGNOSTIC_E0305: return "E0305";
    case OM_RESOLUTION_DIAGNOSTIC_E0306: return "E0306";
    case OM_RESOLUTION_DIAGNOSTIC_E0307: return "E0307";
    case OM_RESOLUTION_DIAGNOSTIC_E0308: return "E0308";
    default: return "E0000";
    }
}
