#ifndef OMBASIC_SEMANTIC_ANALYSIS_INTERNAL_H
#define OMBASIC_SEMANTIC_ANALYSIS_INTERNAL_H

#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/foundation/vector.h"

typedef struct Om_Semantic_Analysis_Context {
    const Om_Module_Project *project;
    const Om_Parser_Result *parsed_modules;
    size_t module_count;
    const Om_Resolution_Result *resolution;
    const Om_Type_Inference_Result *inference;
    Om_Vector diagnostics;
    Om_Vector symbol_effects;
    Om_Vector node_effects;
    Om_Vector actor_protocol;
    Om_Vector serializable_types;
} Om_Semantic_Analysis_Context;

const Om_Ast *om_semantic_module_ast(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id
);
const Om_Ast_Node *om_semantic_node(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
Om_Ast_Node_Id om_semantic_child(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    const Om_Ast_Node *node,
    size_t index
);
const Om_Symbol *om_semantic_symbol(
    const Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id symbol_id
);
Om_Symbol_Id om_semantic_resolved_symbol(
    const Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
Om_Status om_semantic_add_diagnostic(
    Om_Semantic_Analysis_Context *context,
    Om_Semantic_Analysis_Diagnostic_Code code,
    Om_Module_Id module_id,
    Om_Source_Span span,
    Om_Symbol_Id symbol_id
);
Om_Status om_semantic_set_node_effect(
    Om_Semantic_Analysis_Context *context,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id,
    uint32_t effects
);
Om_Status om_semantic_set_symbol_effect(
    Om_Semantic_Analysis_Context *context,
    Om_Symbol_Id symbol_id,
    uint32_t effects
);

Om_Status om_effect_analyze_internal(Om_Semantic_Analysis_Context *context);
Om_Status om_actor_protocol_check_internal(
    Om_Semantic_Analysis_Context *context
);
Om_Status om_serializable_check_internal(
    Om_Semantic_Analysis_Context *context
);

#endif
