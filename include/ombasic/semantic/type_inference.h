#ifndef OMBASIC_SEMANTIC_TYPE_INFERENCE_H
#define OMBASIC_SEMANTIC_TYPE_INFERENCE_H

#include <stddef.h>

#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/type_store.h"

typedef enum Om_Type_Inference_Diagnostic_Code {
    OM_TYPE_INFERENCE_DIAGNOSTIC_E0401 = 401,
    OM_TYPE_INFERENCE_DIAGNOSTIC_E0402 = 402,
    OM_TYPE_INFERENCE_DIAGNOSTIC_E0403 = 403,
    OM_TYPE_INFERENCE_DIAGNOSTIC_E0404 = 404,
    OM_TYPE_INFERENCE_DIAGNOSTIC_QUERY_SOURCE_NOT_LIST = 1201,
    OM_TYPE_INFERENCE_DIAGNOSTIC_QUERY_WHERE_NOT_BOOL = 1202,
    OM_TYPE_INFERENCE_DIAGNOSTIC_QUERY_TAKE_NOT_INT = 1203,
    OM_TYPE_INFERENCE_DIAGNOSTIC_QUERY_ORDER_TYPE_UNSUPPORTED = 1204
} Om_Type_Inference_Diagnostic_Code;

typedef struct Om_Inferred_Node {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Type_Id type_id;
} Om_Inferred_Node;

typedef struct Om_Type_Inference_Diagnostic {
    Om_Type_Inference_Diagnostic_Code code;
    Om_Module_Id module_id;
    Om_Source_Span span;
    Om_Type_Id expected_type;
    Om_Type_Id actual_type;
} Om_Type_Inference_Diagnostic;

typedef struct Om_Type_Inference_Result {
    Om_Type_Store type_store;
    const Om_Inferred_Node *inferred_nodes;
    size_t inferred_node_count;
    /* Indexed by Symbol_Id; entries without a value have type_id INVALID. */
    const Om_Type_Scheme *symbol_schemes;
    size_t symbol_scheme_count;
    const Om_Type_Inference_Diagnostic *diagnostics;
    size_t diagnostic_count;
    void *owned_inferred_nodes;
    void *owned_symbol_schemes;
    void *owned_diagnostics;
} Om_Type_Inference_Result;

/* Requires syntax and name-resolution results without prior diagnostics.
 * Pass a zero-initialized result and destroy it before reusing the storage. */
Om_Status om_type_infer_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    Om_Type_Inference_Result *result
);
void om_type_inference_result_destroy(Om_Type_Inference_Result *result);
Om_Type_Id om_type_inference_type_for_node(
    const Om_Type_Inference_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
const Om_Type_Scheme *om_type_inference_scheme_for_symbol(
    const Om_Type_Inference_Result *result,
    Om_Symbol_Id symbol_id
);
const char *om_type_inference_diagnostic_code_name(
    Om_Type_Inference_Diagnostic_Code code
);

#endif
