#ifndef OMBASIC_IR_HIR_H
#define OMBASIC_IR_HIR_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/ir/ids.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/semantic/name_resolver.h"
#include "ombasic/semantic/semantic_analysis.h"
#include "ombasic/semantic/type_inference.h"
#include "ombasic/syntax/parser.h"

typedef enum Om_Hir_Kind {
    OM_HIR_PROGRAM = 0,
    OM_HIR_IMPORT,
    OM_HIR_RECORD,
    OM_HIR_MESSAGE,
    OM_HIR_FUNCTION,
    OM_HIR_ACTION,
    OM_HIR_ACTOR,
    OM_HIR_RECEIVE,
    OM_HIR_PARAMETER,
    OM_HIR_BLOCK,
    OM_HIR_BIND,
    OM_HIR_DECLARE_VAR,
    OM_HIR_ASSIGN_VAR,
    OM_HIR_ACTOR_STATE_INIT,
    OM_HIR_ACTOR_STATE_ASSIGN,
    OM_HIR_RETURN,
    OM_HIR_BREAK,
    OM_HIR_CONTINUE,
    OM_HIR_SEND,
    OM_HIR_WATCH,
    OM_HIR_STOP,
    OM_HIR_CALL_STATEMENT,
    OM_HIR_IF,
    OM_HIR_IF_ARM,
    OM_HIR_WHILE,
    OM_HIR_FOR_RANGE,
    OM_HIR_FOR_EACH,
    OM_HIR_SELECT,
    OM_HIR_CASE_ARM,
    OM_HIR_LOCAL,
    OM_HIR_INTEGER,
    OM_HIR_FLOAT,
    OM_HIR_STRING,
    OM_HIR_STRING_TEXT,
    OM_HIR_INTERPOLATED_STRING,
    OM_HIR_BOOLEAN,
    OM_HIR_NULL,
    OM_HIR_SELF,
    OM_HIR_DOWN_REASON,
    OM_HIR_UNARY,
    OM_HIR_TRY,
    OM_HIR_BINARY,
    OM_HIR_CALL_DIRECT,
    OM_HIR_CALL_INDIRECT,
    OM_HIR_RESULT_OK,
    OM_HIR_RESULT_ERROR,
    OM_HIR_RECORD_CONSTRUCT,
    OM_HIR_MESSAGE_CONSTRUCT,
    OM_HIR_FIELD_READ,
    OM_HIR_SYMBOL_REF,
    OM_HIR_SYSTEM_ACTOR_REF,
    OM_HIR_INDEX,
    OM_HIR_RECORD_WITH,
    OM_HIR_FIELD_UPDATE,
    OM_HIR_SPAWN,
    OM_HIR_GROUP,
    OM_HIR_LIST,
    OM_HIR_MAP,
    OM_HIR_MAP_ENTRY
} Om_Hir_Kind;

typedef enum Om_Hir_Operator {
    OM_HIR_OPERATOR_NONE = 0,
    OM_HIR_OPERATOR_ADD,
    OM_HIR_OPERATOR_SUBTRACT,
    OM_HIR_OPERATOR_MULTIPLY,
    OM_HIR_OPERATOR_DIVIDE,
    OM_HIR_OPERATOR_REMAINDER,
    OM_HIR_OPERATOR_EQUAL,
    OM_HIR_OPERATOR_NOT_EQUAL,
    OM_HIR_OPERATOR_LESS,
    OM_HIR_OPERATOR_GREATER,
    OM_HIR_OPERATOR_LESS_EQUAL,
    OM_HIR_OPERATOR_GREATER_EQUAL,
    OM_HIR_OPERATOR_AND,
    OM_HIR_OPERATOR_OR,
    OM_HIR_OPERATOR_NOT,
    OM_HIR_OPERATOR_NEGATE
} Om_Hir_Operator;

typedef enum Om_Hir_Entity_Kind {
    OM_HIR_ENTITY_FUNCTION = 0,
    OM_HIR_ENTITY_ACTION,
    OM_HIR_ENTITY_RECORD,
    OM_HIR_ENTITY_MESSAGE,
    OM_HIR_ENTITY_ACTOR,
    OM_HIR_ENTITY_RECEIVE
} Om_Hir_Entity_Kind;

typedef enum Om_Hir_Flag {
    OM_HIR_FLAG_NONE = 0,
    OM_HIR_FLAG_EXPORTED = 1u << 0,
    OM_HIR_FLAG_HAS_ELSE = 1u << 1,
    OM_HIR_FLAG_CASE_ELSE = 1u << 2,
    OM_HIR_FLAG_MUTABLE = 1u << 3,
    OM_HIR_FLAG_MIN_INTEGER = 1u << 4
} Om_Hir_Flag;

typedef enum Om_Hir_Diagnostic_Code {
    OM_HIR_DIAGNOSTIC_E0901 = 901,
    OM_HIR_DIAGNOSTIC_E0902,
    OM_HIR_DIAGNOSTIC_E0903,
    OM_HIR_DIAGNOSTIC_E0904,
    OM_HIR_DIAGNOSTIC_E0905,
    OM_HIR_DIAGNOSTIC_E0906,
    OM_HIR_DIAGNOSTIC_E0907,
    OM_HIR_DIAGNOSTIC_E0908
} Om_Hir_Diagnostic_Code;

typedef struct Om_Hir_Node {
    Om_Hir_Node_Id id;
    Om_Hir_Kind kind;
    Om_Hir_Operator operator_kind;
    Om_Module_Id module_id;
    Om_Ast_Node_Id source_node;
    Om_Source_Span span;
    Om_Type_Id type_id;
    Om_Symbol_Id symbol_id;
    Om_Symbol_Id related_symbol_id;
    uint32_t effects;
    uint32_t flags;
    uint32_t child_start;
    uint32_t child_count;
    Om_String_View name;
    uint64_t integer_magnitude;
    double float_value;
} Om_Hir_Node;

typedef struct Om_Hir_Entity {
    Om_Hir_Entity_Id id;
    Om_Hir_Entity_Kind kind;
    Om_Module_Id module_id;
    Om_Hir_Node_Id node_id;
    Om_Symbol_Id symbol_id;
    Om_Symbol_Id related_symbol_id;
    Om_String_View name;
    Om_Source_Span span;
} Om_Hir_Entity;

typedef struct Om_Hir_Module {
    Om_Module_Id id;
    Om_Source_Id source_id;
    Om_String_View canonical_path;
    uint32_t source_length;
    Om_Hir_Node_Id root;
    uint32_t entity_start;
    uint32_t entity_count;
} Om_Hir_Module;

typedef struct Om_Hir_Project {
    const Om_Hir_Module *modules;
    size_t module_count;
    const Om_Hir_Entity *entities;
    size_t entity_count;
    const Om_Hir_Node *nodes;
    size_t node_count;
    const Om_Hir_Node_Id *children;
    size_t child_count;
    /* Borrowed from the type-inference result; keep it alive with this HIR. */
    const Om_Type_Store *type_store;
    const Om_Type_Inference_Result *inference;
    /* Borrowed semantic indexes; keep them alive with this HIR as well. */
    const Om_Resolution_Result *resolution;
    const Om_Semantic_Analysis_Result *semantic;
    void *owned_storage;
} Om_Hir_Project;

typedef struct Om_Hir_Diagnostic {
    Om_Hir_Diagnostic_Code code;
    Om_Module_Id module_id;
    Om_Hir_Node_Id node_id;
    Om_Source_Span span;
} Om_Hir_Diagnostic;

typedef struct Om_Hir_Verification_Result {
    const Om_Hir_Diagnostic *diagnostics;
    size_t diagnostic_count;
    void *owned_diagnostics;
} Om_Hir_Verification_Result;

typedef struct Om_Hir_Text {
    char *data;
    size_t length;
} Om_Hir_Text;

Om_Status om_hir_lower_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Hir_Project *result
);
void om_hir_project_destroy(Om_Hir_Project *project);
const Om_Hir_Node *om_hir_get_node(
    const Om_Hir_Project *project,
    Om_Hir_Node_Id id
);
const Om_Hir_Node *om_hir_child_at(
    const Om_Hir_Project *project,
    const Om_Hir_Node *node,
    size_t index
);
Om_Status om_hir_verify_project(
    const Om_Hir_Project *project,
    Om_Hir_Verification_Result *result
);
void om_hir_verification_result_destroy(Om_Hir_Verification_Result *result);
Om_Status om_hir_dump(const Om_Hir_Project *project, Om_Hir_Text *result);
void om_hir_text_destroy(Om_Hir_Text *text);
const char *om_hir_kind_name(Om_Hir_Kind kind);
const char *om_hir_diagnostic_code_name(Om_Hir_Diagnostic_Code code);

#endif
