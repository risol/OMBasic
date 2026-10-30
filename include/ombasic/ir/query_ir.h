#ifndef OMBASIC_IR_QUERY_IR_H
#define OMBASIC_IR_QUERY_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/ir/hir.h"

typedef enum Om_Query_Operator_Kind {
    OM_QUERY_OPERATOR_SCAN = 0,
    OM_QUERY_OPERATOR_FILTER,
    OM_QUERY_OPERATOR_ORDER_KEY,
    OM_QUERY_OPERATOR_TAKE,
    OM_QUERY_OPERATOR_PROJECT
} Om_Query_Operator_Kind;

typedef enum Om_Query_Physical_Kind {
    OM_QUERY_PHYSICAL_FUSED_SCAN = 0,
    OM_QUERY_PHYSICAL_STABLE_SORT,
    OM_QUERY_PHYSICAL_STABLE_TOP_K
} Om_Query_Physical_Kind;

typedef struct Om_Query_Operator {
    Om_Query_Operator_Kind kind;
    Om_Hir_Node_Id node_id;
    Om_Hir_Node_Id expression_id;
    bool descending;
} Om_Query_Operator;

typedef struct Om_Query_Plan {
    Om_Hir_Node_Id query_node_id;
    Om_Hir_Node_Id source_node_id;
    Om_Symbol_Id range_symbol_id;
    Om_Hir_Node_Id select_expression_id;
    Om_Hir_Node_Id take_expression_id;
    bool has_take;
    Om_Query_Physical_Kind physical_kind;
    Om_Query_Operator *operators;
    size_t operator_count;
    void *owned_storage;
} Om_Query_Plan;

typedef struct Om_Query_Text {
    char *data;
    size_t length;
    void *owned_storage;
} Om_Query_Text;

Om_Status om_query_plan_build(const Om_Hir_Project *hir,
                              Om_Hir_Node_Id query_node_id,
                              Om_Query_Plan *result);
Om_Status om_query_plan_verify(const Om_Hir_Project *hir,
                               const Om_Query_Plan *plan);
Om_Status om_query_plan_dump(const Om_Hir_Project *hir,
                             const Om_Query_Plan *plan,
                             Om_Query_Text *result);
void om_query_plan_destroy(Om_Query_Plan *plan);
void om_query_text_destroy(Om_Query_Text *text);
const char *om_query_operator_name(Om_Query_Operator_Kind kind);
const char *om_query_physical_kind_name(Om_Query_Physical_Kind kind);

#endif
