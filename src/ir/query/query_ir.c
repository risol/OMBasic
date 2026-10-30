#include "ombasic/ir/query_ir.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/vector.h"

typedef struct Om_Query_Text_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_Query_Text_Buffer;

static const Om_Hir_Node *query_node(const Om_Hir_Project *hir,
                                    Om_Hir_Node_Id id)
{
    return om_hir_get_node(hir, id);
}

static Om_Status append_operator(Om_Vector *operators,
                                 Om_Query_Operator_Kind kind,
                                 Om_Hir_Node_Id node_id,
                                 Om_Hir_Node_Id expression_id,
                                 bool descending)
{
    const Om_Query_Operator operation = {
        kind, node_id, expression_id, descending,
    };
    return om_vector_push(operators, &operation);
}

const char *om_query_operator_name(Om_Query_Operator_Kind kind)
{
    static const char *const names[] = {
        "scan", "filter", "order-key", "take", "project",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid-query-operator";
}

const char *om_query_physical_kind_name(Om_Query_Physical_Kind kind)
{
    static const char *const names[] = {
        "fused-scan", "stable-sort", "stable-top-k",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid-query-physical-plan";
}

Om_Status om_query_plan_build(const Om_Hir_Project *hir,
                              Om_Hir_Node_Id query_node_id,
                              Om_Query_Plan *result)
{
    if (hir == NULL || result == NULL || hir->owned_storage == NULL
        || result->owned_storage != NULL || result->operators != NULL
        || result->operator_count != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *query = query_node(hir, query_node_id);
    if (query == NULL || query->kind != OM_HIR_QUERY
        || query->child_count < 2
        || query->symbol_id.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Vector operators = {0};
    Om_Status status = om_vector_init(&operators, sizeof(Om_Query_Operator));
    const Om_Hir_Node *source = om_hir_child_at(hir, query, 0);
    if (status == OM_STATUS_OK && source == NULL) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        status = append_operator(&operators, OM_QUERY_OPERATOR_SCAN,
            source->id, source->id, false);
    }
    bool has_order = false;
    bool has_take = false;
    Om_Hir_Node_Id select_expression = OM_HIR_NODE_ID_INVALID;
    Om_Hir_Node_Id take_expression = OM_HIR_NODE_ID_INVALID;
    for (size_t index = 1; status == OM_STATUS_OK
         && index < query->child_count; ++index) {
        const Om_Hir_Node *clause = om_hir_child_at(hir, query, index);
        if (clause == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (clause->kind == OM_HIR_QUERY_WHERE
            && clause->child_count == 1) {
            const Om_Hir_Node *expression = om_hir_child_at(hir, clause, 0);
            status = expression == NULL ? OM_STATUS_INVALID_ARGUMENT
                : append_operator(&operators, OM_QUERY_OPERATOR_FILTER,
                    clause->id, expression->id, false);
        } else if (clause->kind == OM_HIR_QUERY_ORDER && !has_order
                   && clause->child_count != 0) {
            has_order = true;
            for (size_t key_index = 0; status == OM_STATUS_OK
                 && key_index < clause->child_count; ++key_index) {
                const Om_Hir_Node *key = om_hir_child_at(hir, clause, key_index);
                const Om_Hir_Node *expression = key != NULL
                    && key->child_count == 1
                    ? om_hir_child_at(hir, key, 0) : NULL;
                status = key == NULL || key->kind != OM_HIR_QUERY_ORDER_KEY
                    || expression == NULL ? OM_STATUS_INVALID_ARGUMENT
                    : append_operator(&operators,
                        OM_QUERY_OPERATOR_ORDER_KEY, key->id, expression->id,
                        (key->flags & OM_HIR_FLAG_QUERY_DESCENDING) != 0);
            }
        } else if (clause->kind == OM_HIR_QUERY_TAKE && !has_take
                   && clause->child_count == 1) {
            const Om_Hir_Node *expression = om_hir_child_at(hir, clause, 0);
            has_take = true;
            take_expression = expression != NULL
                ? expression->id : OM_HIR_NODE_ID_INVALID;
            status = expression == NULL ? OM_STATUS_INVALID_ARGUMENT
                : append_operator(&operators, OM_QUERY_OPERATOR_TAKE,
                    clause->id, expression->id, false);
        } else if (clause->kind == OM_HIR_QUERY_SELECT
                   && select_expression.value == UINT32_MAX
                   && clause->child_count == 1) {
            const Om_Hir_Node *expression = om_hir_child_at(hir, clause, 0);
            select_expression = expression != NULL
                ? expression->id : OM_HIR_NODE_ID_INVALID;
            status = expression == NULL ? OM_STATUS_INVALID_ARGUMENT
                : append_operator(&operators, OM_QUERY_OPERATOR_PROJECT,
                    clause->id, expression->id, false);
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == OM_STATUS_OK
        && select_expression.value == UINT32_MAX) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    if (status == OM_STATUS_OK) {
        result->query_node_id = query_node_id;
        result->source_node_id = source->id;
        result->range_symbol_id = query->symbol_id;
        result->select_expression_id = select_expression;
        result->take_expression_id = take_expression;
        result->has_take = has_take;
        result->physical_kind = !has_order
            ? OM_QUERY_PHYSICAL_FUSED_SCAN
            : has_take ? OM_QUERY_PHYSICAL_STABLE_TOP_K
                       : OM_QUERY_PHYSICAL_STABLE_SORT;
        result->operators = operators.data;
        result->operator_count = operators.length;
        result->owned_storage = operators.data;
        operators.data = NULL;
        operators.length = 0;
        operators.capacity = 0;
    }
    om_vector_destroy(&operators);
    if (status == OM_STATUS_OK) {
        status = om_query_plan_verify(hir, result);
    }
    if (status != OM_STATUS_OK) {
        om_query_plan_destroy(result);
    }
    return status;
}

Om_Status om_query_plan_verify(const Om_Hir_Project *hir,
                               const Om_Query_Plan *plan)
{
    if (hir == NULL || plan == NULL || hir->owned_storage == NULL
        || plan->owned_storage == NULL || plan->operators == NULL
        || plan->operator_count < 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Hir_Node *query = query_node(hir, plan->query_node_id);
    const Om_Hir_Node *source = query_node(hir, plan->source_node_id);
    if (query == NULL || query->kind != OM_HIR_QUERY || source == NULL
        || source->id.value != plan->source_node_id.value
        || plan->range_symbol_id.value != query->symbol_id.value
        || plan->operators[0].kind != OM_QUERY_OPERATOR_SCAN
        || plan->operators[0].node_id.value != source->id.value
        || plan->operators[0].expression_id.value != source->id.value) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    unsigned phase = 0;
    bool saw_project = false;
    bool saw_take = false;
    bool saw_order = false;
    for (size_t index = 1; index < plan->operator_count; ++index) {
        const Om_Query_Operator *operation = &plan->operators[index];
        const Om_Hir_Node *operation_node = query_node(hir, operation->node_id);
        const Om_Hir_Node *expression = query_node(hir,
                                                   operation->expression_id);
        if (operation_node == NULL || expression == NULL || saw_project) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        if (operation->kind == OM_QUERY_OPERATOR_FILTER) {
            const Om_Hir_Node *operand = operation_node->child_count == 1
                ? om_hir_child_at(hir, operation_node, 0) : NULL;
            if (phase != 0 || operation_node->kind != OM_HIR_QUERY_WHERE
                || operand == NULL
                || operand->id.value != operation->expression_id.value
                || operation->descending) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        } else if (operation->kind == OM_QUERY_OPERATOR_ORDER_KEY) {
            const Om_Hir_Node *operand = operation_node->child_count == 1
                ? om_hir_child_at(hir, operation_node, 0) : NULL;
            const bool descending = (operation_node->flags
                & OM_HIR_FLAG_QUERY_DESCENDING) != 0;
            if (phase > 1 || operation_node->kind != OM_HIR_QUERY_ORDER_KEY
                || operand == NULL
                || operand->id.value != operation->expression_id.value
                || operation->descending != descending) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            phase = 1;
            saw_order = true;
        } else if (operation->kind == OM_QUERY_OPERATOR_TAKE) {
            const Om_Hir_Node *operand = operation_node->child_count == 1
                ? om_hir_child_at(hir, operation_node, 0) : NULL;
            if (phase > 2 || saw_take
                || operation_node->kind != OM_HIR_QUERY_TAKE
                || operand == NULL
                || operand->id.value != operation->expression_id.value
                || operation->descending) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            phase = 2;
            saw_take = true;
        } else if (operation->kind == OM_QUERY_OPERATOR_PROJECT) {
            const Om_Hir_Node *operand = operation_node->child_count == 1
                ? om_hir_child_at(hir, operation_node, 0) : NULL;
            if (phase > 3 || operation_node->kind != OM_HIR_QUERY_SELECT
                || operand == NULL
                || operand->id.value != operation->expression_id.value
                || operation->expression_id.value
                    != plan->select_expression_id.value
                || operation->descending) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            phase = 3;
            saw_project = true;
        } else {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    const Om_Query_Physical_Kind expected = !saw_order
        ? OM_QUERY_PHYSICAL_FUSED_SCAN
        : saw_take ? OM_QUERY_PHYSICAL_STABLE_TOP_K
                   : OM_QUERY_PHYSICAL_STABLE_SORT;
    return saw_project && saw_take == plan->has_take
        && expected == plan->physical_kind
        && (saw_take == (plan->take_expression_id.value != UINT32_MAX))
        ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status append_text(Om_Query_Text_Buffer *buffer,
                             const char *format,
                             ...)
{
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    const int required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (required < 0) {
        va_end(arguments);
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t bytes = (size_t)required;
    if (bytes > SIZE_MAX - buffer->length - 1) {
        va_end(arguments);
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t needed = buffer->length + bytes + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 128 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        char *data = om_realloc_array(buffer->data, capacity, sizeof(*data));
        if (data == NULL) {
            va_end(arguments);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    (void)vsnprintf(buffer->data + buffer->length,
                    buffer->capacity - buffer->length, format, arguments);
    buffer->length += bytes;
    va_end(arguments);
    return OM_STATUS_OK;
}

Om_Status om_query_plan_dump(const Om_Hir_Project *hir,
                             const Om_Query_Plan *plan,
                             Om_Query_Text *result)
{
    if (result == NULL || result->data != NULL || result->length != 0
        || om_query_plan_verify(hir, plan) != OM_STATUS_OK) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Query_Text_Buffer buffer = {0};
    Om_Status status = append_text(&buffer,
        "query hir=%" PRIu32 " source=%" PRIu32 " range=%" PRIu32
        " physical=%s\n", plan->query_node_id.value,
        plan->source_node_id.value, plan->range_symbol_id.value,
        om_query_physical_kind_name(plan->physical_kind));
    for (size_t index = 0; status == OM_STATUS_OK
         && index < plan->operator_count; ++index) {
        const Om_Query_Operator *operation = &plan->operators[index];
        status = append_text(&buffer,
            "  op%zu %s node=%" PRIu32 " expression=%" PRIu32
            " descending=%u\n", index,
            om_query_operator_name(operation->kind), operation->node_id.value,
            operation->expression_id.value, operation->descending ? 1u : 0u);
    }
    if (status == OM_STATUS_OK) {
        result->data = buffer.data;
        result->length = buffer.length;
        result->owned_storage = buffer.data;
    } else {
        om_free(buffer.data);
    }
    return status;
}

void om_query_plan_destroy(Om_Query_Plan *plan)
{
    if (plan == NULL) return;
    om_free(plan->owned_storage);
    *plan = (Om_Query_Plan){0};
}

void om_query_text_destroy(Om_Query_Text *text)
{
    if (text == NULL) return;
    om_free(text->owned_storage);
    *text = (Om_Query_Text){0};
}
