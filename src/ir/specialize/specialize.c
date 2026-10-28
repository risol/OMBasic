#include "ombasic/ir/specialize.h"

#include <stdbool.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/semantic/type_store.h"

#define OM_SPECIALIZATION_MAX_INSTANCES ((size_t)4096)
#define OM_SPECIALIZATION_MAX_TYPE_DEPTH ((size_t)128)

typedef struct Om_Specialization_Storage {
    Om_Vector instances;
    Om_Vector type_arguments;
    Om_Vector descriptor_types;
    Om_Vector diagnostics;
    Om_Vector worklist;
    Om_Vector node_stack;
} Om_Specialization_Storage;

typedef struct Om_Specialization_Context {
    const Om_Hir_Project *project;
    Om_Type_Store *type_store;
    Om_Specialization_Storage *storage;
} Om_Specialization_Context;

static Om_Specialization_Storage *storage_of(
    const Om_Hir_Specialization *specialization)
{
    return specialization != NULL ? specialization->owned_storage : NULL;
}

static void destroy_storage(Om_Specialization_Storage *storage)
{
    if (storage == NULL) {
        return;
    }
    om_vector_destroy(&storage->node_stack);
    om_vector_destroy(&storage->worklist);
    om_vector_destroy(&storage->diagnostics);
    om_vector_destroy(&storage->descriptor_types);
    om_vector_destroy(&storage->type_arguments);
    om_vector_destroy(&storage->instances);
    om_free(storage);
}

static Om_Status init_storage(Om_Specialization_Storage *storage)
{
    Om_Status status = om_vector_init(&storage->instances,
                                      sizeof(Om_Function_Instance));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->type_arguments, sizeof(Om_Type_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->diagnostics,
                                sizeof(Om_Specialization_Diagnostic));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->descriptor_types, sizeof(Om_Type_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->worklist,
                                sizeof(Om_Function_Instance_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->node_stack, sizeof(Om_Hir_Node_Id));
    }
    return status;
}

static Om_Status add_diagnostic(Om_Specialization_Context *context,
                                Om_Specialization_Diagnostic_Code code,
                                Om_Module_Id module_id,
                                Om_Hir_Node_Id node_id,
                                Om_Symbol_Id symbol_id)
{
    const Om_Specialization_Diagnostic diagnostic = {
        .code = code,
        .module_id = module_id,
        .node_id = node_id,
        .symbol_id = symbol_id,
    };
    return om_vector_push(&context->storage->diagnostics, &diagnostic);
}

static Om_Status resolve_type(const Om_Type_Store *store,
                              Om_Type_Id type_id,
                              Om_Type_Id *resolved)
{
    if (store == NULL || resolved == NULL || type_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_type_store_resolve(store, type_id, resolved);
}

static Om_Status type_equal(const Om_Type_Store *store,
                            Om_Type_Id left,
                            Om_Type_Id right,
                            size_t depth,
                            bool *equal)
{
    if (depth >= OM_SPECIALIZATION_MAX_TYPE_DEPTH || equal == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved_left = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_right = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, left, &resolved_left);
    if (status == OM_STATUS_OK) {
        status = resolve_type(store, right, &resolved_right);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (resolved_left.value == resolved_right.value) {
        *equal = true;
        return OM_STATUS_OK;
    }
    Om_Type_View left_view = {0};
    Om_Type_View right_view = {0};
    status = om_type_store_get(store, resolved_left, &left_view);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved_right, &right_view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (left_view.kind != right_view.kind
        || left_view.nominal_symbol.value != right_view.nominal_symbol.value
        || left_view.child_count != right_view.child_count) {
        *equal = false;
        return OM_STATUS_OK;
    }
    *equal = true;
    for (size_t index = 0; *equal && index < left_view.child_count; ++index) {
        Om_Type_Id left_child = OM_TYPE_ID_INVALID;
        Om_Type_Id right_child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved_left, index, &left_child);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(store, resolved_right, index,
                                            &right_child);
        }
        if (status == OM_STATUS_OK) {
            status = type_equal(store, left_child, right_child, depth + 1, equal);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status type_is_concrete(const Om_Type_Store *store,
                                  Om_Type_Id type_id,
                                  size_t depth,
                                  bool *concrete)
{
    if (depth >= OM_SPECIALIZATION_MAX_TYPE_DEPTH || concrete == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, type_id, &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved, &view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.kind == OM_TYPE_VARIABLE) {
        *concrete = false;
        return OM_STATUS_OK;
    }
    *concrete = true;
    for (size_t index = 0; *concrete && index < view.child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved, index, &child);
        if (status == OM_STATUS_OK) {
            status = type_is_concrete(store, child, depth + 1, concrete);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static size_t quantified_variable_index(const Om_Type_Store *store,
                                        const Om_Type_Scheme *scheme,
                                        Om_Type_Id type_id)
{
    if (scheme == NULL) {
        return SIZE_MAX;
    }
    Om_Type_Id resolved_type = OM_TYPE_ID_INVALID;
    if (resolve_type(store, type_id, &resolved_type) != OM_STATUS_OK) {
        return SIZE_MAX;
    }
    for (size_t index = 0; index < scheme->quantified_variable_count; ++index) {
        Om_Type_Id quantified = OM_TYPE_ID_INVALID;
        if (resolve_type(store, scheme->quantified_variables[index], &quantified)
                == OM_STATUS_OK
            && quantified.value == resolved_type.value) {
            return index;
        }
    }
    return SIZE_MAX;
}

static Om_Status specialized_type(Om_Type_Store *store,
                                  Om_Type_Id source_type,
                                  const Om_Type_Scheme *caller_scheme,
                                  const Om_Type_Id *caller_arguments,
                                  size_t depth,
                                  Om_Type_Id *result)
{
    if (depth >= OM_SPECIALIZATION_MAX_TYPE_DEPTH || result == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, source_type, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const size_t variable_index = quantified_variable_index(
        store, caller_scheme, resolved);
    if (variable_index != SIZE_MAX) {
        if (caller_arguments == NULL
            || variable_index >= caller_scheme->quantified_variable_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *result = caller_arguments[variable_index];
        return OM_STATUS_OK;
    }

    Om_Type_View view = {0};
    status = om_type_store_get(store, resolved, &view);
    if (status != OM_STATUS_OK || view.kind == OM_TYPE_VARIABLE) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    Om_Type_Id *children = view.child_count != 0
        ? om_alloc_array(view.child_count, sizeof(*children)) : NULL;
    if (view.child_count != 0 && children == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < view.child_count;
         ++index) {
        Om_Type_Id source_child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved, index, &source_child);
        if (status == OM_STATUS_OK) {
            status = specialized_type(store, source_child, caller_scheme,
                caller_arguments, depth + 1, &children[index]);
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(children);
        return status;
    }

    switch (view.kind) {
    case OM_TYPE_INT:
    case OM_TYPE_FLOAT:
    case OM_TYPE_BOOL:
    case OM_TYPE_STRING:
    case OM_TYPE_NULL:
    case OM_TYPE_UNIT:
    case OM_TYPE_NODE_REF:
    case OM_TYPE_ACTOR_ID:
    case OM_TYPE_NATIVE_POINTER:
        status = om_type_store_primitive(store, view.kind, result);
        break;
    case OM_TYPE_DOWN_REASON:
        status = view.nominal_symbol.value == UINT32_MAX
            ? om_type_store_primitive(store, view.kind, result)
            : om_type_store_nominal(store, view.kind, view.nominal_symbol,
                                    result);
        break;
    case OM_TYPE_LIST:
        status = view.child_count == 1
            ? om_type_store_list(store, children[0], result)
            : OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_TYPE_MAP:
        status = view.child_count == 2
            ? om_type_store_map(store, children[0], children[1], result)
            : OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_TYPE_RECORD:
    case OM_TYPE_MESSAGE:
    case OM_TYPE_ACTOR_PROTOCOL:
        status = om_type_store_nominal(store, view.kind, view.nominal_symbol,
                                       result);
        break;
    case OM_TYPE_ACTOR_REF:
        status = view.child_count == 1
            ? om_type_store_actor_ref_type(store, children[0], result)
            : OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_TYPE_RESULT:
        status = view.child_count == 2
            ? om_type_store_result(store, children[0], children[1], result)
            : OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_TYPE_FUNCTION:
    case OM_TYPE_ACTION:
        status = view.child_count != 0
            ? om_type_store_function(store, view.kind, children,
                                     view.child_count - 1,
                                     children[view.child_count - 1], result)
            : OM_STATUS_INVALID_ARGUMENT;
        break;
    case OM_TYPE_VARIABLE:
        status = OM_STATUS_INVALID_ARGUMENT;
        break;
    }
    om_free(children);
    return status;
}

static Om_Status collect_descriptor_type(Om_Specialization_Context *context,
                                         Om_Type_Id type_id,
                                         size_t depth)
{
    if (depth >= OM_SPECIALIZATION_MAX_TYPE_DEPTH) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(context->type_store, type_id, &resolved);
    Om_Type_View view = {0};
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(context->type_store, resolved, &view);
    }
    bool concrete = false;
    if (status == OM_STATUS_OK) {
        status = type_is_concrete(context->type_store, resolved, 0, &concrete);
    }
    if (status != OM_STATUS_OK || !concrete) {
        return status;
    }
    if (view.kind == OM_TYPE_LIST || view.kind == OM_TYPE_MAP
        || view.kind == OM_TYPE_RESULT) {
        bool found = false;
        for (size_t index = 0; index < context->storage->descriptor_types.length;
             ++index) {
            const Om_Type_Id *existing = om_vector_at_const(
                &context->storage->descriptor_types, index);
            if (existing == NULL) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            bool equal = false;
            status = type_equal(context->type_store, *existing, resolved, 0,
                                &equal);
            if (status != OM_STATUS_OK) {
                return status;
            }
            if (equal) {
                found = true;
                break;
            }
        }
        if (!found) {
            status = om_vector_push(&context->storage->descriptor_types,
                                    &resolved);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < view.child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(context->type_store, resolved, index,
                                        &child);
        if (status == OM_STATUS_OK) {
            status = collect_descriptor_type(context, child, depth + 1);
        }
    }
    return status;
}

static Om_Status collect_instance_node_type(
    Om_Specialization_Context *context,
    const Om_Function_Instance *instance,
    const Om_Hir_Node *node)
{
    if (node->type_id.value == UINT32_MAX) {
        return OM_STATUS_OK;
    }
    const Om_Type_Scheme *caller_scheme = om_type_inference_scheme_for_symbol(
        context->project->inference, instance->symbol_id);
    const Om_Type_Id *caller_arguments = NULL;
    if (instance->type_argument_count != 0) {
        if (caller_scheme == NULL
            || caller_scheme->quantified_variable_count
                != instance->type_argument_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Type_Id *stored_arguments =
            context->storage->type_arguments.data;
        caller_arguments = stored_arguments + instance->type_argument_start;
    }
    Om_Type_Id concrete_type = OM_TYPE_ID_INVALID;
    const Om_Status status = specialized_type(context->type_store,
        node->type_id, caller_scheme, caller_arguments, 0, &concrete_type);
    if (status == OM_STATUS_INVALID_ARGUMENT) {
        return OM_STATUS_OK;
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    return collect_descriptor_type(context, concrete_type, 0);
}

static Om_Status match_instance_type(const Om_Type_Store *store,
                                     Om_Type_Id template_type,
                                     Om_Type_Id actual_type,
                                     const Om_Type_Scheme *target_scheme,
                                     Om_Type_Id *arguments,
                                     size_t depth)
{
    if (depth >= OM_SPECIALIZATION_MAX_TYPE_DEPTH) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Type_Id resolved_template = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_actual = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_type(store, template_type, &resolved_template);
    if (status == OM_STATUS_OK) {
        status = resolve_type(store, actual_type, &resolved_actual);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    const size_t variable_index = quantified_variable_index(
        store, target_scheme, resolved_template);
    if (variable_index != SIZE_MAX) {
        bool concrete = false;
        status = type_is_concrete(store, resolved_actual, 0, &concrete);
        if (status != OM_STATUS_OK || !concrete) {
            return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
        }
        if (arguments[variable_index].value == UINT32_MAX) {
            arguments[variable_index] = resolved_actual;
            return OM_STATUS_OK;
        }
        bool equal = false;
        status = type_equal(store, arguments[variable_index], resolved_actual,
                            0, &equal);
        return status == OM_STATUS_OK && !equal
            ? OM_STATUS_INVALID_ARGUMENT : status;
    }

    Om_Type_View template_view = {0};
    Om_Type_View actual_view = {0};
    status = om_type_store_get(store, resolved_template, &template_view);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(store, resolved_actual, &actual_view);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (template_view.kind != actual_view.kind
        || template_view.nominal_symbol.value != actual_view.nominal_symbol.value
        || template_view.child_count != actual_view.child_count
        || template_view.kind == OM_TYPE_VARIABLE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < template_view.child_count; ++index) {
        Om_Type_Id template_child = OM_TYPE_ID_INVALID;
        Om_Type_Id actual_child = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(store, resolved_template, index,
                                        &template_child);
        if (status == OM_STATUS_OK) {
            status = om_type_store_child_at(store, resolved_actual, index,
                                            &actual_child);
        }
        if (status == OM_STATUS_OK) {
            status = match_instance_type(store, template_child, actual_child,
                target_scheme, arguments, depth + 1);
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

static bool same_argument_list(const Om_Specialization_Context *context,
                               const Om_Function_Instance *left,
                               const Om_Type_Id *right,
                               size_t right_count)
{
    if (left->type_argument_count != right_count) {
        return false;
    }
    const Om_Type_Id *left_arguments = context->storage->type_arguments.data;
    for (size_t index = 0; index < right_count; ++index) {
        bool equal = false;
        if (type_equal(context->type_store,
                left_arguments[left->type_argument_start + index], right[index],
                0, &equal) != OM_STATUS_OK || !equal) {
            return false;
        }
    }
    return true;
}

static Om_Status find_instance(const Om_Specialization_Context *context,
                               Om_Function_Instance_Kind kind,
                               Om_Hir_Entity_Id entity_id,
                               Om_Hir_Node_Id root_node,
                               Om_Symbol_Id symbol_id,
                               const Om_Type_Id *type_arguments,
                               size_t type_argument_count,
                               Om_Function_Instance_Id *instance_id)
{
    for (size_t index = 0; index < context->storage->instances.length; ++index) {
        const Om_Function_Instance *existing = om_vector_at_const(
            &context->storage->instances, index);
        if (existing == NULL || existing->kind != kind) {
            continue;
        }
        const bool same_identity = kind == OM_FUNCTION_INSTANCE_RECEIVE
            ? existing->entity_id.value == entity_id.value
            : kind == OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT
                ? existing->root_node.value == root_node.value
                : existing->symbol_id.value == symbol_id.value;
        if (same_identity && same_argument_list(context, existing,
                type_arguments, type_argument_count)) {
            *instance_id = existing->id;
            return OM_STATUS_OK;
        }
    }
    *instance_id = OM_FUNCTION_INSTANCE_ID_INVALID;
    return OM_STATUS_OK;
}

static Om_Status add_instance(Om_Specialization_Context *context,
                              Om_Function_Instance_Kind kind,
                              Om_Hir_Entity_Id entity_id,
                              Om_Hir_Node_Id root_node,
                              Om_Module_Id module_id,
                              Om_Symbol_Id symbol_id,
                              Om_Function_Instance_Id parent_id,
                              const Om_Type_Id *type_arguments,
                              size_t type_argument_count,
                              Om_Function_Instance_Id *instance_id)
{
    Om_Status status = find_instance(context, kind, entity_id, root_node,
        symbol_id, type_arguments, type_argument_count, instance_id);
    if (status != OM_STATUS_OK || instance_id->value != UINT32_MAX) {
        return status;
    }

    for (Om_Function_Instance_Id ancestor_id = parent_id;
         ancestor_id.value != UINT32_MAX;) {
        const Om_Function_Instance *ancestor = om_vector_at_const(
            &context->storage->instances, ancestor_id.value);
        if (ancestor == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if ((kind == OM_FUNCTION_INSTANCE_FUNCTION
                || kind == OM_FUNCTION_INSTANCE_ACTION)
            && ancestor->symbol_id.value == symbol_id.value
            && !same_argument_list(context, ancestor, type_arguments,
                                   type_argument_count)) {
            return add_diagnostic(context,
                OM_SPECIALIZATION_DIAGNOSTIC_E1001, module_id, root_node,
                symbol_id);
        }
        ancestor_id = ancestor->parent_instance_id;
    }
    if (context->storage->instances.length >= OM_SPECIALIZATION_MAX_INSTANCES
        || context->storage->instances.length >= UINT32_MAX
        || context->storage->type_arguments.length > UINT32_MAX
        || type_argument_count > UINT32_MAX
            - context->storage->type_arguments.length) {
        return add_diagnostic(context, OM_SPECIALIZATION_DIAGNOSTIC_E1003,
                              module_id, root_node, symbol_id);
    }
    const uint32_t argument_start =
        (uint32_t)context->storage->type_arguments.length;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < type_argument_count; ++index) {
        status = om_vector_push(&context->storage->type_arguments,
                                &type_arguments[index]);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Function_Instance new_instance = {
        .id = {(uint32_t)context->storage->instances.length},
        .kind = kind,
        .entity_id = entity_id,
        .module_id = module_id,
        .root_node = root_node,
        .symbol_id = symbol_id,
        .parent_instance_id = parent_id,
        .type_argument_start = argument_start,
        .type_argument_count = (uint32_t)type_argument_count,
    };
    status = om_vector_push(&context->storage->instances, &new_instance);
    if (status != OM_STATUS_OK) {
        return status;
    }
    *instance_id = new_instance.id;
    return om_vector_push(&context->storage->worklist, instance_id);
}

static const Om_Hir_Entity *entity_for_symbol(const Om_Hir_Project *project,
                                              Om_Symbol_Id symbol_id,
                                              Om_Hir_Entity_Kind kind)
{
    for (size_t index = 0; index < project->entity_count; ++index) {
        const Om_Hir_Entity *entity = &project->entities[index];
        if (entity->kind == kind && entity->symbol_id.value == symbol_id.value) {
            return entity;
        }
    }
    return NULL;
}

static Om_Status derive_type_arguments(Om_Specialization_Context *context,
                                      const Om_Function_Instance *caller,
                                      const Om_Hir_Node *call,
                                      Om_Symbol_Id target_symbol,
                                      Om_Type_Id **arguments,
                                      size_t *argument_count)
{
    const Om_Type_Scheme *target_scheme = om_type_inference_scheme_for_symbol(
        context->project->inference, target_symbol);
    if (target_scheme == NULL) {
        *arguments = NULL;
        *argument_count = 0;
        return OM_STATUS_OK;
    }
    *argument_count = target_scheme->quantified_variable_count;
    *arguments = NULL;
    if (*argument_count == 0) {
        return OM_STATUS_OK;
    }
    Om_Type_Id *instance_arguments = om_alloc_array(*argument_count,
                                                    sizeof(*instance_arguments));
    if (instance_arguments == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < *argument_count; ++index) {
        instance_arguments[index] = OM_TYPE_ID_INVALID;
    }

    Om_Type_Id target_function_type = OM_TYPE_ID_INVALID;
    Om_Type_View target_function_view = {0};
    Om_Status status = resolve_type(context->type_store, target_scheme->type_id,
                                    &target_function_type);
    if (status == OM_STATUS_OK) {
        status = om_type_store_get(context->type_store, target_function_type,
                                   &target_function_view);
    }
    if (status != OM_STATUS_OK
        || (target_function_view.kind != OM_TYPE_FUNCTION
            && target_function_view.kind != OM_TYPE_ACTION)
        || target_function_view.child_count == 0
        || call->child_count != target_function_view.child_count) {
        om_free(instance_arguments);
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    const Om_Type_Scheme *caller_scheme = om_type_inference_scheme_for_symbol(
        context->project->inference, caller->symbol_id);
    const Om_Type_Id *caller_arguments = context->storage->type_arguments.data;
    if (caller->type_argument_count == 0) {
        caller_arguments = NULL;
    } else if (caller_scheme == NULL
               || caller_scheme->quantified_variable_count
                    != caller->type_argument_count) {
        om_free(instance_arguments);
        return OM_STATUS_INVALID_ARGUMENT;
    } else {
        caller_arguments += caller->type_argument_start;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index + 1 < target_function_view.child_count; ++index) {
        Om_Type_Id formal_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(context->type_store,
            target_function_type, index, &formal_type);
        const Om_Hir_Node *actual_argument = om_hir_child_at(
            context->project, call, index + 1);
        Om_Type_Id specialized_argument = OM_TYPE_ID_INVALID;
        if (status == OM_STATUS_OK
            && (actual_argument == NULL
                || actual_argument->type_id.value == UINT32_MAX)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = specialized_type(context->type_store,
                actual_argument->type_id, caller_scheme, caller_arguments, 0,
                &specialized_argument);
        }
        if (status == OM_STATUS_OK) {
            status = match_instance_type(context->type_store, formal_type,
                specialized_argument, target_scheme, instance_arguments, 0);
        }
    }
    bool has_missing_argument = false;
    for (size_t index = 0; index < *argument_count; ++index) {
        has_missing_argument = has_missing_argument
            || instance_arguments[index].value == UINT32_MAX;
    }
    if (status == OM_STATUS_OK && has_missing_argument
        && call->type_id.value != UINT32_MAX) {
        Om_Type_Id result_type = OM_TYPE_ID_INVALID;
        Om_Type_Id specialized_result_type = OM_TYPE_ID_INVALID;
        status = om_type_store_child_at(context->type_store,
            target_function_type, target_function_view.child_count - 1,
            &result_type);
        if (status == OM_STATUS_OK) {
            status = specialized_type(context->type_store, call->type_id,
                caller_scheme, caller_arguments, 0, &specialized_result_type);
        }
        if (status == OM_STATUS_OK) {
            status = match_instance_type(context->type_store, result_type,
                specialized_result_type, target_scheme, instance_arguments, 0);
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < *argument_count;
         ++index) {
        if (instance_arguments[index].value == UINT32_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (target_scheme->quantified_constraints != NULL) {
            bool satisfied = false;
            status = om_type_store_require(context->type_store,
                instance_arguments[index], target_scheme->quantified_constraints[index],
                &satisfied);
            if (status == OM_STATUS_OK && !satisfied) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        if (status == OM_STATUS_OK) {
            status = resolve_type(context->type_store, instance_arguments[index],
                                  &instance_arguments[index]);
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(instance_arguments);
        return status;
    }
    *arguments = instance_arguments;
    return OM_STATUS_OK;
}

static Om_Status enqueue_call(Om_Specialization_Context *context,
                              const Om_Function_Instance *caller,
                              Om_Function_Instance_Id caller_id,
                              const Om_Hir_Node *call)
{
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->project->resolution, call->symbol_id);
    if (symbol == NULL
        || (symbol->kind != OM_SYMBOL_FUNCTION
            && symbol->kind != OM_SYMBOL_ACTION)) {
        return OM_STATUS_OK;
    }
    const Om_Hir_Entity_Kind entity_kind = symbol->kind == OM_SYMBOL_FUNCTION
        ? OM_HIR_ENTITY_FUNCTION : OM_HIR_ENTITY_ACTION;
    const Om_Hir_Entity *entity = entity_for_symbol(context->project,
                                                    symbol->id, entity_kind);
    if (entity == NULL) {
        return add_diagnostic(context, OM_SPECIALIZATION_DIAGNOSTIC_E1002,
                              call->module_id, call->id, call->symbol_id);
    }
    Om_Type_Id *type_arguments = NULL;
    size_t type_argument_count = 0;
    Om_Status status = derive_type_arguments(context, caller, call,
        symbol->id, &type_arguments, &type_argument_count);
    if (status == OM_STATUS_INVALID_ARGUMENT || status == OM_STATUS_OUT_OF_RANGE) {
        om_free(type_arguments);
        return add_diagnostic(context, OM_SPECIALIZATION_DIAGNOSTIC_E1002,
            call->module_id, call->id, call->symbol_id);
    }
    if (status == OM_STATUS_OK) {
        const Om_Function_Instance_Kind instance_kind =
            symbol->kind == OM_SYMBOL_FUNCTION
                ? OM_FUNCTION_INSTANCE_FUNCTION : OM_FUNCTION_INSTANCE_ACTION;
        Om_Function_Instance_Id ignored = OM_FUNCTION_INSTANCE_ID_INVALID;
        status = add_instance(context, instance_kind, entity->id, entity->node_id,
            entity->module_id, entity->symbol_id, caller_id, type_arguments,
            type_argument_count, &ignored);
    }
    om_free(type_arguments);
    return status;
}

static Om_Status derive_reference_type_arguments(
    Om_Specialization_Context *context,
    const Om_Function_Instance *caller,
    const Om_Hir_Node *reference,
    Om_Symbol_Id target_symbol,
    Om_Type_Id **arguments,
    size_t *argument_count)
{
    const Om_Type_Scheme *target_scheme = om_type_inference_scheme_for_symbol(
        context->project->inference, target_symbol);
    if (target_scheme == NULL) {
        *arguments = NULL;
        *argument_count = 0;
        return OM_STATUS_OK;
    }
    *argument_count = target_scheme->quantified_variable_count;
    *arguments = NULL;
    if (*argument_count == 0) {
        return OM_STATUS_OK;
    }
    Om_Type_Id *instance_arguments = om_alloc_array(*argument_count,
                                                    sizeof(*instance_arguments));
    if (instance_arguments == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < *argument_count; ++index) {
        instance_arguments[index] = OM_TYPE_ID_INVALID;
    }
    const Om_Type_Scheme *caller_scheme = om_type_inference_scheme_for_symbol(
        context->project->inference, caller->symbol_id);
    const Om_Type_Id *caller_arguments = context->storage->type_arguments.data;
    if (caller->type_argument_count == 0) {
        caller_arguments = NULL;
    } else if (caller_scheme == NULL
               || caller_scheme->quantified_variable_count
                    != caller->type_argument_count) {
        om_free(instance_arguments);
        return OM_STATUS_INVALID_ARGUMENT;
    } else {
        caller_arguments += caller->type_argument_start;
    }
    Om_Type_Id reference_type = OM_TYPE_ID_INVALID;
    Om_Status status = specialized_type(context->type_store,
        reference->type_id, caller_scheme, caller_arguments, 0,
        &reference_type);
    if (status == OM_STATUS_OK) {
        status = match_instance_type(context->type_store, target_scheme->type_id,
            reference_type, target_scheme, instance_arguments, 0);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < *argument_count;
         ++index) {
        if (instance_arguments[index].value == UINT32_MAX) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (target_scheme->quantified_constraints != NULL) {
            bool satisfied = false;
            status = om_type_store_require(context->type_store,
                instance_arguments[index],
                target_scheme->quantified_constraints[index], &satisfied);
            if (status == OM_STATUS_OK && !satisfied) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        if (status == OM_STATUS_OK) {
            status = resolve_type(context->type_store, instance_arguments[index],
                                  &instance_arguments[index]);
        }
    }
    if (status != OM_STATUS_OK) {
        om_free(instance_arguments);
        return status;
    }
    *arguments = instance_arguments;
    return OM_STATUS_OK;
}

static Om_Status enqueue_reference(Om_Specialization_Context *context,
                                   const Om_Function_Instance *caller,
                                   Om_Function_Instance_Id caller_id,
                                   const Om_Hir_Node *reference)
{
    const Om_Symbol *symbol = om_resolution_symbol_by_id(
        context->project->resolution, reference->symbol_id);
    if (symbol == NULL
        || (symbol->kind != OM_SYMBOL_FUNCTION
            && symbol->kind != OM_SYMBOL_ACTION)) {
        return OM_STATUS_OK;
    }
    const Om_Hir_Entity_Kind entity_kind = symbol->kind == OM_SYMBOL_FUNCTION
        ? OM_HIR_ENTITY_FUNCTION : OM_HIR_ENTITY_ACTION;
    const Om_Hir_Entity *entity = entity_for_symbol(context->project,
                                                    symbol->id, entity_kind);
    if (entity == NULL) {
        return add_diagnostic(context, OM_SPECIALIZATION_DIAGNOSTIC_E1002,
                              reference->module_id, reference->id,
                              reference->symbol_id);
    }
    Om_Type_Id *type_arguments = NULL;
    size_t type_argument_count = 0;
    Om_Status status = derive_reference_type_arguments(context, caller,
        reference, symbol->id, &type_arguments, &type_argument_count);
    if (status == OM_STATUS_INVALID_ARGUMENT || status == OM_STATUS_OUT_OF_RANGE) {
        om_free(type_arguments);
        return add_diagnostic(context, OM_SPECIALIZATION_DIAGNOSTIC_E1002,
            reference->module_id, reference->id, reference->symbol_id);
    }
    if (status == OM_STATUS_OK) {
        const Om_Function_Instance_Kind instance_kind =
            symbol->kind == OM_SYMBOL_FUNCTION
                ? OM_FUNCTION_INSTANCE_FUNCTION : OM_FUNCTION_INSTANCE_ACTION;
        Om_Function_Instance_Id ignored = OM_FUNCTION_INSTANCE_ID_INVALID;
        status = add_instance(context, instance_kind, entity->id,
            entity->node_id, entity->module_id, entity->symbol_id, caller_id,
            type_arguments, type_argument_count, &ignored);
    }
    om_free(type_arguments);
    return status;
}

static Om_Status scan_instance(Om_Specialization_Context *context,
                               Om_Function_Instance_Id instance_id)
{
    const Om_Function_Instance *stored = om_vector_at_const(
        &context->storage->instances, instance_id.value);
    if (stored == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Function_Instance instance = *stored;
    om_vector_clear(&context->storage->node_stack);
    Om_Status status = om_vector_push(&context->storage->node_stack,
                                      &instance.root_node);
    while (status == OM_STATUS_OK && context->storage->node_stack.length != 0) {
        const size_t last = context->storage->node_stack.length - 1;
        const Om_Hir_Node_Id *node_id = om_vector_at_const(
            &context->storage->node_stack, last);
        if (node_id == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Hir_Node_Id current_id = *node_id;
        context->storage->node_stack.length = last;
        const Om_Hir_Node *node = om_hir_get_node(context->project, current_id);
        if (node == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = collect_instance_node_type(context, &instance, node);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (current_id.value != instance.root_node.value
            && (node->kind == OM_HIR_FUNCTION || node->kind == OM_HIR_ACTION
                || node->kind == OM_HIR_ACTOR || node->kind == OM_HIR_RECEIVE)) {
            continue;
        }
        if (node->kind == OM_HIR_CALL_DIRECT) {
            status = enqueue_call(context, &instance, instance_id, node);
        } else if (node->kind == OM_HIR_LOCAL
                   || node->kind == OM_HIR_SYMBOL_REF) {
            status = enqueue_reference(context, &instance, instance_id, node);
        }
        const size_t first_child = node->kind == OM_HIR_CALL_DIRECT ? 1 : 0;
        for (size_t index = node->child_count; status == OM_STATUS_OK
             && index > first_child; --index) {
            const Om_Hir_Node_Id child_id =
                context->project->children[node->child_start + index - 1];
            status = om_vector_push(&context->storage->node_stack, &child_id);
        }
    }
    return status;
}

static Om_Status add_roots(Om_Specialization_Context *context)
{
    for (size_t index = 0; index < context->project->entity_count; ++index) {
        const Om_Hir_Entity *entity = &context->project->entities[index];
        Om_Function_Instance_Kind instance_kind;
        if (entity->kind == OM_HIR_ENTITY_ACTION) {
            instance_kind = OM_FUNCTION_INSTANCE_ACTION;
        } else if (entity->kind == OM_HIR_ENTITY_RECEIVE) {
            instance_kind = OM_FUNCTION_INSTANCE_RECEIVE;
        } else {
            continue;
        }
        Om_Function_Instance_Id ignored = OM_FUNCTION_INSTANCE_ID_INVALID;
        const Om_Status status = add_instance(context, instance_kind,
            entity->id, entity->node_id, entity->module_id, entity->symbol_id,
            OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0, &ignored);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    for (size_t index = 0; index < context->project->node_count; ++index) {
        const Om_Hir_Node *node = &context->project->nodes[index];
        if (node->kind != OM_HIR_ACTOR_STATE_INIT) {
            continue;
        }
        Om_Function_Instance_Id ignored = OM_FUNCTION_INSTANCE_ID_INVALID;
        const Om_Status status = add_instance(context,
            OM_FUNCTION_INSTANCE_ACTOR_STATE_INIT, OM_HIR_ENTITY_ID_INVALID,
            node->id, node->module_id, node->symbol_id,
            OM_FUNCTION_INSTANCE_ID_INVALID, NULL, 0, &ignored);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return OM_STATUS_OK;
}

Om_Status om_hir_specialize_project(const Om_Hir_Project *project,
                                    Om_Type_Store *type_store,
                                    Om_Hir_Specialization *result)
{
    if (project == NULL || type_store == NULL || result == NULL
        || result->owned_storage != NULL || project->owned_storage == NULL
        || project->inference == NULL || project->resolution == NULL
        || project->entities == NULL || project->nodes == NULL
        || project->children == NULL || project->type_store != type_store
        ) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Specialization_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Specialization_Storage){0};
    Om_Status status = init_storage(storage);
    if (status != OM_STATUS_OK) {
        destroy_storage(storage);
        return status;
    }
    Om_Specialization_Context context = {
        .project = project,
        .type_store = type_store,
        .storage = storage,
    };
    status = add_roots(&context);
    size_t work_index = 0;
    while (status == OM_STATUS_OK && work_index < storage->worklist.length) {
        const Om_Function_Instance_Id *item = om_vector_at_const(
            &storage->worklist, work_index++);
        if (item == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Function_Instance_Id current_instance = *item;
        status = scan_instance(&context, current_instance);
    }
    if (status != OM_STATUS_OK) {
        destroy_storage(storage);
        return status;
    }
    result->instances = storage->instances.data;
    result->instance_count = storage->instances.length;
    result->type_arguments = storage->type_arguments.data;
    result->type_argument_count = storage->type_arguments.length;
    result->descriptor_types = storage->descriptor_types.data;
    result->descriptor_type_count = storage->descriptor_types.length;
    result->diagnostics = storage->diagnostics.data;
    result->diagnostic_count = storage->diagnostics.length;
    result->type_store = type_store;
    result->owned_storage = storage;
    return OM_STATUS_OK;
}

Om_Status om_hir_specialization_resolve_type(
    const Om_Hir_Project *project,
    const Om_Hir_Specialization *specialization,
    Om_Function_Instance_Id instance_id,
    Om_Type_Id source_type,
    Om_Type_Id *specialized_type_id)
{
    if (project == NULL || specialization == NULL || specialized_type_id == NULL
        || project->inference == NULL || project->type_store == NULL
        || specialization->owned_storage == NULL
        || specialization->type_store != project->type_store
        || project->type_store != &project->inference->type_store) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Function_Instance *instance = NULL;
    for (size_t index = 0; index < specialization->instance_count; ++index) {
        if (specialization->instances[index].id.value == instance_id.value) {
            instance = &specialization->instances[index];
            break;
        }
    }
    if (instance == NULL
        || instance->type_argument_start > specialization->type_argument_count
        || instance->type_argument_count
            > specialization->type_argument_count
                - instance->type_argument_start) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Type_Scheme *scheme = om_type_inference_scheme_for_symbol(
        project->inference, instance->symbol_id);
    if (instance->type_argument_count == 0) {
        if (scheme != NULL && scheme->quantified_variable_count != 0) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return specialized_type(specialization->type_store, source_type,
            scheme, NULL, 0, specialized_type_id);
    }
    if (scheme == NULL
        || scheme->quantified_variable_count != instance->type_argument_count
        || specialization->type_arguments == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Type_Id *type_arguments = specialization->type_arguments
        + instance->type_argument_start;
    return specialized_type(specialization->type_store, source_type, scheme,
        type_arguments, 0, specialized_type_id);
}

void om_hir_specialization_destroy(Om_Hir_Specialization *specialization)
{
    if (specialization == NULL) {
        return;
    }
    destroy_storage(storage_of(specialization));
    *specialization = (Om_Hir_Specialization){0};
}

const char *om_specialization_diagnostic_code_name(
    Om_Specialization_Diagnostic_Code code)
{
    switch (code) {
    case OM_SPECIALIZATION_DIAGNOSTIC_E1001: return "E1001";
    case OM_SPECIALIZATION_DIAGNOSTIC_E1002: return "E1002";
    case OM_SPECIALIZATION_DIAGNOSTIC_E1003: return "E1003";
    default: return "E0000";
    }
}
