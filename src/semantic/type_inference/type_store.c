#include "ombasic/semantic/type_store.h"

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"

#define OM_TYPE_MAX_RECURSION_DEPTH 512u

typedef struct Om_Type_Node {
    Om_Type_Kind kind;
    uint32_t first_child;
    uint32_t child_count;
    Om_Symbol_Id nominal_symbol;
    Om_Type_Id instance;
    uint32_t level;
    uint32_t constraints;
} Om_Type_Node;

typedef struct Om_Type_Change {
    Om_Type_Id type_id;
    Om_Type_Id old_instance;
    uint32_t old_level;
    uint32_t old_constraints;
} Om_Type_Change;

typedef struct Om_Type_Store_Storage {
    Om_Vector nodes;
    Om_Vector children;
    Om_Vector changes;
} Om_Type_Store_Storage;

typedef struct Om_Type_Replacement {
    Om_Type_Id original;
    Om_Type_Id replacement;
} Om_Type_Replacement;

static Om_Status record_change(Om_Type_Store_Storage *storage,
                               Om_Type_Id type_id);
static void rollback_changes(Om_Type_Store_Storage *storage, size_t mark);

static Om_Type_Store_Storage *storage_for(const Om_Type_Store *store)
{
    return store == NULL ? NULL : store->owned_storage;
}

static Om_Type_Node *node_at(Om_Type_Store_Storage *storage, Om_Type_Id type_id)
{
    if (storage == NULL) {
        return NULL;
    }
    return om_vector_at(&storage->nodes, type_id.value);
}

static const Om_Type_Node *node_at_const(const Om_Type_Store_Storage *storage,
                                         Om_Type_Id type_id)
{
    if (storage == NULL) {
        return NULL;
    }
    return om_vector_at_const(&storage->nodes, type_id.value);
}

static Om_Status resolve_internal(const Om_Type_Store_Storage *storage,
                                  Om_Type_Id type_id,
                                  Om_Type_Id *resolved_type)
{
    if (storage == NULL || resolved_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id current = type_id;
    for (size_t depth = 0; depth < OM_TYPE_MAX_RECURSION_DEPTH; ++depth) {
        const Om_Type_Node *node = node_at_const(storage, current);
        if (node == NULL) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        if (node->kind != OM_TYPE_VARIABLE
            || node->instance.value == UINT32_MAX) {
            *resolved_type = current;
            return OM_STATUS_OK;
        }
        current = node->instance;
    }
    return OM_STATUS_OVERFLOW;
}

static Om_Status child_at_internal(const Om_Type_Store_Storage *storage,
                                   const Om_Type_Node *node,
                                   size_t index,
                                   Om_Type_Id *child_type)
{
    if (storage == NULL || node == NULL || child_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (index >= node->child_count) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t child_index = (size_t)node->first_child + index;
    const Om_Type_Id *child = om_vector_at_const(&storage->children, child_index);
    if (child == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *child_type = *child;
    return OM_STATUS_OK;
}

static Om_Status require_recursive(Om_Type_Store_Storage *storage,
                                   Om_Type_Id type_id,
                                   uint32_t constraints,
                                   size_t depth,
                                   bool *satisfied)
{
    const uint32_t all_constraints =
        (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC
        | (uint32_t)OM_TYPE_CONSTRAINT_INTEGRAL
        | (uint32_t)OM_TYPE_CONSTRAINT_COMPARABLE
        | (uint32_t)OM_TYPE_CONSTRAINT_ORDERED
        | (uint32_t)OM_TYPE_CONSTRAINT_PRINTABLE
        | (uint32_t)OM_TYPE_CONSTRAINT_HASH_KEY
        | (uint32_t)OM_TYPE_CONSTRAINT_ACTOR_REF
        | (uint32_t)OM_TYPE_CONSTRAINT_MESSAGE;
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH
        || (constraints & ~all_constraints) != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Type_Node *node = node_at(storage, resolved);
    if (node == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (node->kind == OM_TYPE_VARIABLE) {
        if ((node->constraints & constraints) != constraints) {
            status = record_change(storage, resolved);
            if (status == OM_STATUS_OK) {
                node->constraints |= constraints;
            }
        }
        *satisfied = status == OM_STATUS_OK;
        return status;
    }
    *satisfied = true;
    for (uint32_t bit = UINT32_C(1); bit <= (uint32_t)OM_TYPE_CONSTRAINT_MESSAGE;
         bit <<= 1) {
        if ((constraints & bit) == 0) {
            continue;
        }
        bool bit_satisfied = false;
        if (bit == (uint32_t)OM_TYPE_CONSTRAINT_NUMERIC) {
            bit_satisfied = node->kind == OM_TYPE_INT || node->kind == OM_TYPE_FLOAT;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_INTEGRAL) {
            bit_satisfied = node->kind == OM_TYPE_INT;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_ORDERED) {
            bit_satisfied = node->kind == OM_TYPE_INT || node->kind == OM_TYPE_FLOAT
                            || node->kind == OM_TYPE_STRING;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_PRINTABLE) {
            bit_satisfied = node->kind == OM_TYPE_INT || node->kind == OM_TYPE_FLOAT
                            || node->kind == OM_TYPE_BOOL
                            || node->kind == OM_TYPE_STRING
                            || node->kind == OM_TYPE_NULL;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_HASH_KEY) {
            bit_satisfied = node->kind == OM_TYPE_INT || node->kind == OM_TYPE_BOOL
                            || node->kind == OM_TYPE_STRING;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_COMPARABLE) {
            bit_satisfied = node->kind == OM_TYPE_INT || node->kind == OM_TYPE_FLOAT
                            || node->kind == OM_TYPE_BOOL
                            || node->kind == OM_TYPE_STRING
                            || node->kind == OM_TYPE_NULL
                            || node->kind == OM_TYPE_ACTOR_REF
                            || node->kind == OM_TYPE_ACTOR_ID
                            || node->kind == OM_TYPE_DOWN_REASON;
            if (node->kind == OM_TYPE_RESULT) {
                bit_satisfied = true;
                for (size_t index = 0; status == OM_STATUS_OK
                                       && bit_satisfied
                                       && index < node->child_count; ++index) {
                    Om_Type_Id child = OM_TYPE_ID_INVALID;
                    status = child_at_internal(storage, node, index, &child);
                    if (status == OM_STATUS_OK) {
                        status = require_recursive(storage, child, bit,
                                                   depth + 1, &bit_satisfied);
                    }
                }
            }
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_ACTOR_REF) {
            bit_satisfied = node->kind == OM_TYPE_ACTOR_REF;
        } else if (bit == (uint32_t)OM_TYPE_CONSTRAINT_MESSAGE) {
            bit_satisfied = node->kind == OM_TYPE_MESSAGE;
        }
        if (!bit_satisfied) {
            *satisfied = false;
            return status;
        }
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    return status;
}

static bool node_children_equal(const Om_Type_Store_Storage *storage,
                                const Om_Type_Node *node,
                                const Om_Type_Id *children,
                                size_t child_count)
{
    if (node->child_count != child_count) {
        return false;
    }
    for (size_t index = 0; index < child_count; ++index) {
        Om_Type_Id stored = OM_TYPE_ID_INVALID;
        if (child_at_internal(storage, node, index, &stored) != OM_STATUS_OK
            || stored.value != children[index].value) {
            return false;
        }
    }
    return true;
}

static Om_Status intern_type(Om_Type_Store *store,
                             Om_Type_Kind kind,
                             Om_Symbol_Id nominal_symbol,
                             const Om_Type_Id *children,
                             size_t child_count,
                             Om_Type_Id *type_id)
{
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || type_id == NULL
        || (children == NULL && child_count != 0)
        || child_count > UINT32_MAX
        || storage->children.length > UINT32_MAX - child_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < child_count; ++index) {
        if (node_at_const(storage, children[index]) == NULL) {
            return OM_STATUS_OUT_OF_RANGE;
        }
    }
    for (size_t index = OM_TYPE_PRIMITIVE_COUNT;
         index < storage->nodes.length; ++index) {
        const Om_Type_Node *existing = om_vector_at_const(&storage->nodes, index);
        if (existing->kind == kind
            && existing->nominal_symbol.value == nominal_symbol.value
            && node_children_equal(storage, existing, children, child_count)) {
            *type_id = (Om_Type_Id){(uint32_t)index};
            return OM_STATUS_OK;
        }
    }
    if (storage->nodes.length >= UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const size_t old_child_count = storage->children.length;
    for (size_t index = 0; index < child_count; ++index) {
        const Om_Status status = om_vector_push(&storage->children, &children[index]);
        if (status != OM_STATUS_OK) {
            storage->children.length = old_child_count;
            return status;
        }
    }
    const Om_Type_Node node = {
        .kind = kind,
        .first_child = (uint32_t)old_child_count,
        .child_count = (uint32_t)child_count,
        .nominal_symbol = nominal_symbol,
        .instance = OM_TYPE_ID_INVALID,
        .level = 0,
        .constraints = 0,
    };
    const Om_Status status = om_vector_push(&storage->nodes, &node);
    if (status != OM_STATUS_OK) {
        storage->children.length = old_child_count;
        return status;
    }
    *type_id = (Om_Type_Id){(uint32_t)(storage->nodes.length - 1)};
    return OM_STATUS_OK;
}

static Om_Status append_primitive(Om_Type_Store_Storage *storage,
                                  Om_Type_Kind kind,
                                  Om_Type_Id *type_id)
{
    const Om_Type_Node node = {
        .kind = kind,
        .first_child = 0,
        .child_count = 0,
        .nominal_symbol = OM_SYMBOL_ID_INVALID,
        .instance = OM_TYPE_ID_INVALID,
        .level = 0,
        .constraints = 0,
    };
    const Om_Status status = om_vector_push(&storage->nodes, &node);
    if (status == OM_STATUS_OK) {
        *type_id = (Om_Type_Id){(uint32_t)(storage->nodes.length - 1)};
    }
    return status;
}

Om_Status om_type_store_init(Om_Type_Store *store)
{
    static const Om_Type_Kind primitives[OM_TYPE_PRIMITIVE_COUNT] = {
        OM_TYPE_INT, OM_TYPE_FLOAT, OM_TYPE_BOOL,
        OM_TYPE_STRING, OM_TYPE_NULL, OM_TYPE_UNIT,
        OM_TYPE_NODE_REF, OM_TYPE_ACTOR_ID, OM_TYPE_DOWN_REASON
    };
    if (store == NULL || store->owned_storage != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Store_Storage *storage = om_alloc(sizeof(*storage));
    if (storage == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *storage = (Om_Type_Store_Storage){0};
    Om_Status status = om_vector_init(&storage->nodes, sizeof(Om_Type_Node));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->children, sizeof(Om_Type_Id));
    }
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&storage->changes, sizeof(Om_Type_Change));
    }
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < OM_TYPE_PRIMITIVE_COUNT; ++index) {
        status = append_primitive(storage, primitives[index],
                                  &store->primitive_types[index]);
    }
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&storage->nodes);
        om_vector_destroy(&storage->children);
        om_vector_destroy(&storage->changes);
        om_free(storage);
        *store = (Om_Type_Store){0};
        return status;
    }
    store->owned_storage = storage;
    return OM_STATUS_OK;
}

void om_type_store_destroy(Om_Type_Store *store)
{
    if (store == NULL) {
        return;
    }
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage != NULL) {
        om_vector_destroy(&storage->nodes);
        om_vector_destroy(&storage->children);
        om_vector_destroy(&storage->changes);
        om_free(storage);
    }
    *store = (Om_Type_Store){0};
}

Om_Status om_type_store_primitive(const Om_Type_Store *store,
                                  Om_Type_Kind kind,
                                  Om_Type_Id *type_id)
{
    if (storage_for(store) == NULL || type_id == NULL
        || (unsigned)kind >= OM_TYPE_PRIMITIVE_COUNT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *type_id = store->primitive_types[kind];
    return OM_STATUS_OK;
}

Om_Status om_type_store_variable(Om_Type_Store *store,
                                 uint32_t level,
                                 Om_Type_Id *type_id)
{
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || type_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (storage->nodes.length >= UINT32_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    const Om_Type_Node node = {
        .kind = OM_TYPE_VARIABLE,
        .first_child = 0,
        .child_count = 0,
        .nominal_symbol = OM_SYMBOL_ID_INVALID,
        .instance = OM_TYPE_ID_INVALID,
        .level = level,
        .constraints = 0,
    };
    const Om_Status status = om_vector_push(&storage->nodes, &node);
    if (status == OM_STATUS_OK) {
        *type_id = (Om_Type_Id){(uint32_t)(storage->nodes.length - 1)};
    }
    return status;
}

Om_Status om_type_store_require(Om_Type_Store *store,
                                Om_Type_Id type_id,
                                uint32_t constraints,
                                bool *satisfied)
{
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || satisfied == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t mark = storage->changes.length;
    *satisfied = false;
    const Om_Status status = require_recursive(storage, type_id, constraints,
                                               0, satisfied);
    if (status != OM_STATUS_OK || !*satisfied) {
        rollback_changes(storage, mark);
        *satisfied = false;
        return status;
    }
    storage->changes.length = mark;
    return OM_STATUS_OK;
}

Om_Status om_type_store_list(Om_Type_Store *store,
                             Om_Type_Id element_type,
                             Om_Type_Id *type_id)
{
    return intern_type(store, OM_TYPE_LIST, OM_SYMBOL_ID_INVALID,
                       &element_type, 1, type_id);
}

Om_Status om_type_store_map(Om_Type_Store *store,
                            Om_Type_Id key_type,
                            Om_Type_Id value_type,
                            Om_Type_Id *type_id)
{
    const Om_Type_Id children[2] = {key_type, value_type};
    return intern_type(store, OM_TYPE_MAP, OM_SYMBOL_ID_INVALID,
                       children, 2, type_id);
}

Om_Status om_type_store_nominal(Om_Type_Store *store,
                                Om_Type_Kind kind,
                                Om_Symbol_Id symbol_id,
                                Om_Type_Id *type_id)
{
    if ((kind != OM_TYPE_RECORD && kind != OM_TYPE_MESSAGE
         && kind != OM_TYPE_ACTOR_PROTOCOL)
        || symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return intern_type(store, kind, symbol_id, NULL, 0, type_id);
}

Om_Status om_type_store_actor_ref(Om_Type_Store *store,
                                 Om_Symbol_Id actor_symbol,
                                 Om_Type_Id *type_id)
{
    Om_Type_Id protocol = OM_TYPE_ID_INVALID;
    Om_Status status = om_type_store_nominal(
        store, OM_TYPE_ACTOR_PROTOCOL, actor_symbol, &protocol);
    if (status == OM_STATUS_OK) {
        status = om_type_store_actor_ref_type(store, protocol, type_id);
    }
    return status;
}

Om_Status om_type_store_actor_ref_type(Om_Type_Store *store,
                                       Om_Type_Id protocol_type,
                                       Om_Type_Id *type_id)
{
    Om_Type_View protocol_view = {0};
    Om_Status status = om_type_store_get(store, protocol_type, &protocol_view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (protocol_view.kind != OM_TYPE_ACTOR_PROTOCOL
        && protocol_view.kind != OM_TYPE_VARIABLE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return intern_type(store, OM_TYPE_ACTOR_REF, OM_SYMBOL_ID_INVALID,
                       &protocol_type, 1, type_id);
}

Om_Status om_type_store_function(Om_Type_Store *store,
                                 Om_Type_Kind kind,
                                 const Om_Type_Id *parameter_types,
                                 size_t parameter_count,
                                 Om_Type_Id return_type,
                                 Om_Type_Id *type_id)
{
    if ((kind != OM_TYPE_FUNCTION && kind != OM_TYPE_ACTION)
        || (parameter_types == NULL && parameter_count != 0)
        || parameter_count == SIZE_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t child_count = parameter_count + 1;
    Om_Type_Id *children = om_alloc_array(child_count, sizeof(*children));
    if (children == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < parameter_count; ++index) {
        children[index] = parameter_types[index];
    }
    children[parameter_count] = return_type;
    const Om_Status status = intern_type(store, kind, OM_SYMBOL_ID_INVALID,
                                         children, child_count, type_id);
    om_free(children);
    return status;
}

Om_Status om_type_store_result(Om_Type_Store *store,
                               Om_Type_Id value_type,
                               Om_Type_Id error_type,
                               Om_Type_Id *type_id)
{
    const Om_Type_Id children[2] = {value_type, error_type};
    return intern_type(store, OM_TYPE_RESULT, OM_SYMBOL_ID_INVALID,
                       children, 2, type_id);
}

Om_Status om_type_store_resolve(const Om_Type_Store *store,
                                Om_Type_Id type_id,
                                Om_Type_Id *resolved_type)
{
    return resolve_internal(storage_for(store), type_id, resolved_type);
}

Om_Status om_type_store_get(const Om_Type_Store *store,
                            Om_Type_Id type_id,
                            Om_Type_View *view)
{
    const Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || view == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Type_Node *node = node_at_const(storage, resolved);
    if (node == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *view = (Om_Type_View){
        .kind = node->kind,
        .child_count = node->child_count,
        .nominal_symbol = node->nominal_symbol,
        .type_variable_level = node->level,
        .constraints = node->constraints,
        .type_variable_unbound = node->kind == OM_TYPE_VARIABLE,
    };
    return OM_STATUS_OK;
}

Om_Status om_type_store_child_at(const Om_Type_Store *store,
                                 Om_Type_Id type_id,
                                 size_t index,
                                 Om_Type_Id *child_type)
{
    const Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || child_type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    return child_at_internal(storage, node_at_const(storage, resolved),
                             index, child_type);
}

static Om_Status occurs_in_type(const Om_Type_Store_Storage *storage,
                                Om_Type_Id variable,
                                Om_Type_Id candidate,
                                size_t depth,
                                bool *occurs)
{
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, candidate, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (resolved.value == variable.value) {
        *occurs = true;
        return OM_STATUS_OK;
    }
    const Om_Type_Node *node = node_at_const(storage, resolved);
    *occurs = false;
    for (size_t index = 0; status == OM_STATUS_OK && !*occurs
                           && index < node->child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = child_at_internal(storage, node, index, &child);
        if (status == OM_STATUS_OK) {
            status = occurs_in_type(storage, variable, child, depth + 1, occurs);
        }
    }
    return status;
}

static Om_Status record_change(Om_Type_Store_Storage *storage,
                               Om_Type_Id type_id)
{
    Om_Type_Node *node = node_at(storage, type_id);
    if (node == NULL || node->kind != OM_TYPE_VARIABLE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Type_Change change = {
        .type_id = type_id,
        .old_instance = node->instance,
        .old_level = node->level,
        .old_constraints = node->constraints,
    };
    return om_vector_push(&storage->changes, &change);
}

static Om_Status lower_levels(Om_Type_Store_Storage *storage,
                              Om_Type_Id type_id,
                              uint32_t level,
                              size_t depth)
{
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Type_Node *node = node_at(storage, resolved);
    if (node == NULL) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (node->kind == OM_TYPE_VARIABLE) {
        if (node->level > level) {
            status = record_change(storage, resolved);
            if (status == OM_STATUS_OK) {
                node->level = level;
            }
        }
        return status;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = child_at_internal(storage, node, index, &child);
        if (status == OM_STATUS_OK) {
            status = lower_levels(storage, child, level, depth + 1);
        }
    }
    return status;
}

static Om_Status bind_variable(Om_Type_Store_Storage *storage,
                               Om_Type_Id variable,
                               Om_Type_Id candidate,
                               bool *unified)
{
    const Om_Type_Node *variable_node = node_at_const(storage, variable);
    if (variable_node == NULL || variable_node->kind != OM_TYPE_VARIABLE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    bool occurs = false;
    Om_Status status = occurs_in_type(storage, variable, candidate, 0, &occurs);
    if (status != OM_STATUS_OK || occurs) {
        *unified = false;
        return status;
    }
    bool constraints_satisfied = false;
    status = require_recursive(storage, candidate, variable_node->constraints,
                               0, &constraints_satisfied);
    if (status != OM_STATUS_OK || !constraints_satisfied) {
        *unified = false;
        return status;
    }
    const uint32_t variable_level = variable_node->level;
    status = lower_levels(storage, candidate, variable_level, 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = record_change(storage, variable);
    if (status == OM_STATUS_OK) {
        Om_Type_Node *mutable_variable = node_at(storage, variable);
        mutable_variable->instance = candidate;
        *unified = true;
    }
    return status;
}

static Om_Status unify_recursive(Om_Type_Store_Storage *storage,
                                 Om_Type_Id left,
                                 Om_Type_Id right,
                                 size_t depth,
                                 bool *unified)
{
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved_left = OM_TYPE_ID_INVALID;
    Om_Type_Id resolved_right = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, left, &resolved_left);
    if (status == OM_STATUS_OK) {
        status = resolve_internal(storage, right, &resolved_right);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (resolved_left.value == resolved_right.value) {
        *unified = true;
        return OM_STATUS_OK;
    }
    const Om_Type_Node *left_node = node_at_const(storage, resolved_left);
    const Om_Type_Node *right_node = node_at_const(storage, resolved_right);
    if (left_node->kind == OM_TYPE_VARIABLE) {
        return bind_variable(storage, resolved_left, resolved_right, unified);
    }
    if (right_node->kind == OM_TYPE_VARIABLE) {
        return bind_variable(storage, resolved_right, resolved_left, unified);
    }
    if (left_node->kind != right_node->kind
        || left_node->nominal_symbol.value != right_node->nominal_symbol.value
        || left_node->child_count != right_node->child_count) {
        *unified = false;
        return OM_STATUS_OK;
    }
    *unified = true;
    for (size_t index = 0; status == OM_STATUS_OK && *unified
                           && index < left_node->child_count; ++index) {
        Om_Type_Id left_child = OM_TYPE_ID_INVALID;
        Om_Type_Id right_child = OM_TYPE_ID_INVALID;
        status = child_at_internal(storage, left_node, index, &left_child);
        if (status == OM_STATUS_OK) {
            status = child_at_internal(storage, right_node, index, &right_child);
        }
        if (status == OM_STATUS_OK) {
            status = unify_recursive(storage, left_child, right_child,
                                     depth + 1, unified);
        }
    }
    return status;
}

static void rollback_changes(Om_Type_Store_Storage *storage, size_t mark)
{
    while (storage->changes.length > mark) {
        const size_t last = storage->changes.length - 1;
        const Om_Type_Change *change = om_vector_at_const(&storage->changes, last);
        Om_Type_Node *node = node_at(storage, change->type_id);
        if (node != NULL) {
            node->instance = change->old_instance;
            node->level = change->old_level;
            node->constraints = change->old_constraints;
        }
        storage->changes.length = last;
    }
}

Om_Status om_type_store_unify(Om_Type_Store *store,
                              Om_Type_Id left,
                              Om_Type_Id right,
                              bool *unified)
{
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || unified == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t mark = storage->changes.length;
    *unified = false;
    const Om_Status status = unify_recursive(storage, left, right, 0, unified);
    if (status != OM_STATUS_OK || !*unified) {
        rollback_changes(storage, mark);
        *unified = false;
        return status;
    }
    storage->changes.length = mark;
    return OM_STATUS_OK;
}

static bool vector_contains_type(const Om_Vector *items, Om_Type_Id type_id)
{
    for (size_t index = 0; index < items->length; ++index) {
        const Om_Type_Id *item = om_vector_at_const(items, index);
        if (item->value == type_id.value) {
            return true;
        }
    }
    return false;
}

static Om_Status collect_generalized_variables(
    const Om_Type_Store_Storage *storage,
    Om_Type_Id type_id,
    uint32_t environment_level,
    size_t depth,
    Om_Vector *visited,
    Om_Vector *variables
)
{
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK || vector_contains_type(visited, resolved)) {
        return status;
    }
    status = om_vector_push(visited, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Type_Node *node = node_at_const(storage, resolved);
    if (node->kind == OM_TYPE_VARIABLE) {
        if (node->level > environment_level) {
            return om_vector_push(variables, &resolved);
        }
        return OM_STATUS_OK;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < node->child_count; ++index) {
        Om_Type_Id child = OM_TYPE_ID_INVALID;
        status = child_at_internal(storage, node, index, &child);
        if (status == OM_STATUS_OK) {
            status = collect_generalized_variables(storage, child,
                                                   environment_level, depth + 1,
                                                   visited, variables);
        }
    }
    return status;
}

Om_Status om_type_store_generalize(const Om_Type_Store *store,
                                   Om_Type_Id type_id,
                                   uint32_t environment_level,
                                   Om_Type_Scheme *scheme)
{
    const Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || scheme == NULL || scheme->quantified_variables != NULL
        || scheme->quantified_constraints != NULL
        || scheme->owned_quantified_variables != NULL
        || scheme->owned_quantified_constraints != NULL
        || scheme->quantified_variable_count != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector visited = {0};
    Om_Vector variables = {0};
    Om_Status status = om_vector_init(&visited, sizeof(Om_Type_Id));
    if (status == OM_STATUS_OK) {
        status = om_vector_init(&variables, sizeof(Om_Type_Id));
    }
    if (status == OM_STATUS_OK) {
        status = collect_generalized_variables(storage, type_id,
                                               environment_level, 0,
                                               &visited, &variables);
    }
    om_vector_destroy(&visited);
    if (status != OM_STATUS_OK) {
        om_vector_destroy(&variables);
        return status;
    }
    uint32_t *constraints = om_alloc_array(variables.length,
                                           sizeof(*constraints));
    if (variables.length != 0 && constraints == NULL) {
        om_vector_destroy(&variables);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < variables.length; ++index) {
        const Om_Type_Id *variable = om_vector_at_const(&variables, index);
        const Om_Type_Node *node = node_at_const(storage, *variable);
        if (node == NULL || node->kind != OM_TYPE_VARIABLE) {
            om_free(constraints);
            om_vector_destroy(&variables);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        constraints[index] = node->constraints;
    }
    scheme->type_id = type_id;
    scheme->quantified_variables = variables.data;
    scheme->quantified_constraints = constraints;
    scheme->quantified_variable_count = variables.length;
    scheme->owned_quantified_variables = variables.data;
    scheme->owned_quantified_constraints = constraints;
    return OM_STATUS_OK;
}

void om_type_scheme_destroy(Om_Type_Scheme *scheme)
{
    if (scheme == NULL) {
        return;
    }
    om_free(scheme->owned_quantified_variables);
    om_free(scheme->owned_quantified_constraints);
    *scheme = (Om_Type_Scheme){.type_id = OM_TYPE_ID_INVALID};
}

static bool scheme_quantifies(const Om_Type_Scheme *scheme, Om_Type_Id type_id)
{
    for (size_t index = 0; index < scheme->quantified_variable_count; ++index) {
        if (scheme->quantified_variables[index].value == type_id.value) {
            return true;
        }
    }
    return false;
}

static uint32_t scheme_constraints_for(const Om_Type_Scheme *scheme,
                                       Om_Type_Id type_id)
{
    for (size_t index = 0; index < scheme->quantified_variable_count; ++index) {
        if (scheme->quantified_variables[index].value == type_id.value) {
            return scheme->quantified_constraints[index];
        }
    }
    return 0;
}

static Om_Status instantiate_recursive(Om_Type_Store *store,
                                       const Om_Type_Scheme *scheme,
                                       Om_Type_Id type_id,
                                       uint32_t level,
                                       size_t depth,
                                       Om_Vector *replacements,
                                       Om_Type_Id *instantiated)
{
    if (depth >= OM_TYPE_MAX_RECURSION_DEPTH) {
        return OM_STATUS_OVERFLOW;
    }
    Om_Type_Store_Storage *storage = storage_for(store);
    Om_Type_Id quantified_variable = type_id;
    if (!scheme_quantifies(scheme, quantified_variable)) {
        Om_Type_Id resolved = OM_TYPE_ID_INVALID;
        const Om_Status resolve_status = resolve_internal(storage, type_id,
                                                           &resolved);
        if (resolve_status != OM_STATUS_OK) {
            return resolve_status;
        }
        quantified_variable = resolved;
    }
    if (scheme_quantifies(scheme, quantified_variable)) {
        for (size_t index = 0; index < replacements->length; ++index) {
            const Om_Type_Replacement *replacement = om_vector_at_const(
                replacements, index);
            if (replacement->original.value == quantified_variable.value) {
                *instantiated = replacement->replacement;
                return OM_STATUS_OK;
            }
        }
        Om_Type_Id variable = OM_TYPE_ID_INVALID;
        Om_Status status = om_type_store_variable(store, level, &variable);
        if (status == OM_STATUS_OK) {
            bool constraints_satisfied = false;
            status = om_type_store_require(
                store, variable,
                scheme_constraints_for(scheme, quantified_variable),
                &constraints_satisfied);
            if (status == OM_STATUS_OK && !constraints_satisfied) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        if (status == OM_STATUS_OK) {
            const Om_Type_Replacement replacement = {quantified_variable, variable};
            status = om_vector_push(replacements, &replacement);
        }
        if (status == OM_STATUS_OK) {
            *instantiated = variable;
        }
        return status;
    }
    Om_Type_Id resolved = OM_TYPE_ID_INVALID;
    Om_Status status = resolve_internal(storage, type_id, &resolved);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Type_Node *node = node_at_const(storage, resolved);
    if (node->kind == OM_TYPE_VARIABLE) {
        *instantiated = resolved;
        return OM_STATUS_OK;
    }
    if (node->child_count == 0) {
        *instantiated = resolved;
        return OM_STATUS_OK;
    }
    Om_Type_Id *children = om_alloc_array(node->child_count, sizeof(*children));
    Om_Type_Id *source_children = om_alloc_array(node->child_count,
                                                 sizeof(*source_children));
    if (children == NULL || source_children == NULL) {
        om_free(children);
        om_free(source_children);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const size_t child_count = node->child_count;
    const Om_Type_Kind kind = node->kind;
    const Om_Symbol_Id nominal_symbol = node->nominal_symbol;
    for (size_t index = 0; index < child_count; ++index) {
        status = child_at_internal(storage, node, index, &source_children[index]);
        if (status != OM_STATUS_OK) {
            break;
        }
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < child_count; ++index) {
        status = instantiate_recursive(store, scheme, source_children[index], level,
                                       depth + 1, replacements,
                                       &children[index]);
    }
    if (status == OM_STATUS_OK) {
        status = intern_type(store, kind, nominal_symbol,
                             children, child_count, instantiated);
    }
    om_free(children);
    om_free(source_children);
    return status;
}

Om_Status om_type_store_instantiate(Om_Type_Store *store,
                                    const Om_Type_Scheme *scheme,
                                    uint32_t level,
                                    Om_Type_Id *type_id)
{
    Om_Type_Store_Storage *storage = storage_for(store);
    if (storage == NULL || scheme == NULL || type_id == NULL
        || node_at_const(storage, scheme->type_id) == NULL
        || (scheme->quantified_variables == NULL
            && scheme->quantified_variable_count != 0)
        || (scheme->quantified_constraints == NULL
            && scheme->quantified_variable_count != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector replacements = {0};
    Om_Status status = om_vector_init(&replacements,
                                      sizeof(Om_Type_Replacement));
    if (status == OM_STATUS_OK) {
        status = instantiate_recursive(store, scheme, scheme->type_id,
                                       level, 0, &replacements, type_id);
    }
    om_vector_destroy(&replacements);
    return status;
}

const char *om_type_kind_name(Om_Type_Kind kind)
{
    switch (kind) {
    case OM_TYPE_INT: return "Int";
    case OM_TYPE_FLOAT: return "Float";
    case OM_TYPE_BOOL: return "Bool";
    case OM_TYPE_STRING: return "String";
    case OM_TYPE_NULL: return "Null";
    case OM_TYPE_UNIT: return "Unit";
    case OM_TYPE_NODE_REF: return "Node_Ref";
    case OM_TYPE_ACTOR_ID: return "Actor_Id";
    case OM_TYPE_DOWN_REASON: return "Down_Reason";
    case OM_TYPE_LIST: return "List";
    case OM_TYPE_MAP: return "Map";
    case OM_TYPE_RECORD: return "Record";
    case OM_TYPE_MESSAGE: return "Message";
    case OM_TYPE_ACTOR_PROTOCOL: return "ActorProtocol";
    case OM_TYPE_ACTOR_REF: return "ActorRef";
    case OM_TYPE_FUNCTION: return "Function";
    case OM_TYPE_ACTION: return "Action";
    case OM_TYPE_RESULT: return "Result";
    case OM_TYPE_VARIABLE: return "TypeVar";
    default: return "InvalidType";
    }
}
