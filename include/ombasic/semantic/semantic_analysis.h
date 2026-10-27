#ifndef OMBASIC_SEMANTIC_ANALYSIS_H
#define OMBASIC_SEMANTIC_ANALYSIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/semantic/type_inference.h"

typedef enum Om_Semantic_Analysis_Diagnostic_Code {
    OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0501 = 501,
    OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0601 = 601,
    OM_SEMANTIC_ANALYSIS_DIAGNOSTIC_E0701 = 701
} Om_Semantic_Analysis_Diagnostic_Code;

typedef enum Om_Effect_Flag {
    OM_EFFECT_NONE = 0,
    OM_EFFECT_MUTATES_LOCAL = 1u << 0,
    OM_EFFECT_MUTATES_ACTOR_STATE = 1u << 1,
    OM_EFFECT_SEND_MESSAGE = 1u << 2,
    OM_EFFECT_SPAWN_ACTOR = 1u << 3,
    OM_EFFECT_NATIVE = 1u << 4,
    OM_EFFECT_FILE_IO = 1u << 5,
    OM_EFFECT_NETWORK_IO = 1u << 6,
    OM_EFFECT_CLOCK = 1u << 7,
    OM_EFFECT_WATCH = 1u << 8,
    OM_EFFECT_STOP = 1u << 9,
    OM_EFFECT_CALL_ACTION = 1u << 10,
    OM_EFFECT_READS_ACTOR_STATE = 1u << 11
} Om_Effect_Flag;

typedef struct Om_Symbol_Effect {
    Om_Symbol_Id symbol_id;
    uint32_t effects;
} Om_Symbol_Effect;

typedef struct Om_Node_Effect {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    uint32_t effects;
} Om_Node_Effect;

typedef struct Om_Actor_Protocol_Entry {
    Om_Symbol_Id actor_symbol;
    Om_Symbol_Id message_symbol;
    Om_Ast_Node_Id receive_node;
} Om_Actor_Protocol_Entry;

typedef struct Om_Type_Serializability {
    Om_Type_Id type_id;
    bool serializable;
} Om_Type_Serializability;

typedef struct Om_Semantic_Analysis_Diagnostic {
    Om_Semantic_Analysis_Diagnostic_Code code;
    Om_Module_Id module_id;
    Om_Source_Span span;
    Om_Symbol_Id symbol_id;
} Om_Semantic_Analysis_Diagnostic;

typedef struct Om_Semantic_Analysis_Result {
    const Om_Semantic_Analysis_Diagnostic *diagnostics;
    size_t diagnostic_count;
    const Om_Symbol_Effect *symbol_effects;
    size_t symbol_effect_count;
    const Om_Node_Effect *node_effects;
    size_t node_effect_count;
    const Om_Actor_Protocol_Entry *actor_protocol;
    size_t actor_protocol_count;
    const Om_Type_Serializability *serializable_types;
    size_t serializable_type_count;
    void *owned_diagnostics;
    void *owned_symbol_effects;
    void *owned_node_effects;
    void *owned_actor_protocol;
    void *owned_serializable_types;
} Om_Semantic_Analysis_Result;

/* Requires parse, resolution, and type-inference results without diagnostics.
 * Pass a zero-initialized result and destroy it before reuse. */
Om_Status om_semantic_analyze_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    Om_Semantic_Analysis_Result *result
);
void om_semantic_analysis_result_destroy(Om_Semantic_Analysis_Result *result);
uint32_t om_semantic_effect_for_symbol(
    const Om_Semantic_Analysis_Result *result,
    Om_Symbol_Id symbol_id
);
uint32_t om_semantic_effect_for_node(
    const Om_Semantic_Analysis_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
bool om_semantic_actor_accepts(
    const Om_Semantic_Analysis_Result *result,
    Om_Symbol_Id actor_symbol,
    Om_Symbol_Id message_symbol
);
bool om_semantic_type_is_serializable(
    const Om_Semantic_Analysis_Result *result,
    Om_Type_Id type_id,
    bool *serializable
);
const char *om_semantic_analysis_diagnostic_code_name(
    Om_Semantic_Analysis_Diagnostic_Code code
);

#endif
