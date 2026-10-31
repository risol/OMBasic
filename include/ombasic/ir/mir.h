#ifndef OMBASIC_IR_MIR_H
#define OMBASIC_IR_MIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/system_actors.h"
#include "ombasic/ir/ids.h"
#include "ombasic/ir/hir.h"
#include "ombasic/ir/specialize.h"
#include "ombasic/semantic/type_store.h"

typedef enum Om_Mir_Function_Kind {
    OM_MIR_FUNCTION = 0,
    OM_MIR_ACTION,
    OM_MIR_RECEIVE,
    OM_MIR_ACTOR_STATE_INIT
} Om_Mir_Function_Kind;

typedef enum Om_Mir_Opcode {
    OM_MIR_CONST_INT = 0,
    OM_MIR_CONST_FLOAT,
    OM_MIR_CONST_BOOL,
    OM_MIR_CONST_NULL,
    OM_MIR_INT_ADD_CHECKED,
    OM_MIR_INT_SUB_CHECKED,
    OM_MIR_INT_MUL_CHECKED,
    OM_MIR_INT_DIV_CHECKED,
    OM_MIR_INT_REM_CHECKED,
    OM_MIR_INT_NEGATE_CHECKED,
    OM_MIR_FLOAT_ADD,
    OM_MIR_FLOAT_SUB,
    OM_MIR_FLOAT_MUL,
    OM_MIR_FLOAT_DIV,
    OM_MIR_FLOAT_NEGATE,
    OM_MIR_BOOL_AND,
    OM_MIR_BOOL_OR,
    OM_MIR_BOOL_NOT,
    OM_MIR_COMPARE,
    OM_MIR_CALL_DIRECT,
    OM_MIR_FUNCTION_REF,
    OM_MIR_CALL_INDIRECT,
    OM_MIR_ALLOC_OBJECT,
    OM_MIR_LOAD_FIELD,
    OM_MIR_STORE_INIT_FIELD,
    OM_MIR_COPY_MANAGED,
    OM_MIR_LIST_CREATE,
    OM_MIR_LIST_CONCAT,
    OM_MIR_LIST_APPEND,
    OM_MIR_LIST_BUILDER_CREATE,
    OM_MIR_LIST_BUILDER_RESERVE,
    OM_MIR_LIST_BUILDER_APPEND,
    OM_MIR_LIST_BUILDER_SEAL,
    OM_MIR_LIST_SET_COPY,
    OM_MIR_LIST_SET_CONSUME,
    OM_MIR_LIST_SET_RANGE_COPY,
    OM_MIR_LIST_FILL_RANGE_COPY,
    OM_MIR_LIST_REPLACE_RANGE_COPY,
    OM_MIR_LIST_REVERSE_COPY,
    OM_MIR_LIST_SLICE_COPY,
    OM_MIR_LIST_STABLE_ORDER,
    OM_MIR_LIST_LENGTH,
    OM_MIR_LIST_GET,
    OM_MIR_MAP_CREATE,
    OM_MIR_MAP_LENGTH,
    OM_MIR_MAP_KEY_AT,
    OM_MIR_MAP_GET,
    OM_MIR_RESULT_MAKE_OK,
    OM_MIR_RESULT_MAKE_ERROR,
    OM_MIR_RESULT_TAG,
    OM_MIR_RESULT_OK_PAYLOAD,
    OM_MIR_RESULT_ERROR_PAYLOAD,
    OM_MIR_RUNTIME_STRING_CREATE,
    OM_MIR_RUNTIME_STRING_FORMAT,
    OM_MIR_RUNTIME_STRING_CONCAT,
    OM_MIR_RUNTIME_STRING_CONCAT_MANY,
    OM_MIR_STRING_JOIN_LIST,
    OM_MIR_STRING_SPLIT_LIST,
    OM_MIR_STRING_REPLACE,
    OM_MIR_STRING_UTF8_BYTES,
    OM_MIR_LIST_COMPACT_BYTES,
    OM_MIR_STRING_HEX_DECODE_BYTES,
    OM_MIR_STRING_HEX_ENCODE_BYTES,
    OM_MIR_RUNTIME_ACTOR_SPAWN,
    OM_MIR_RUNTIME_ACTOR_SELF,
    OM_MIR_RUNTIME_ACTOR_SEND,
    OM_MIR_RUNTIME_ACTOR_WATCH,
    OM_MIR_RUNTIME_ACTOR_STOP,
    OM_MIR_RUNTIME_SYSTEM_ACTOR,
    OM_MIR_ACTOR_IDENTITY,
    OM_MIR_RUNTIME_FAULT,
    OM_MIR_NATIVE_CALL,
    OM_MIR_NATIVE_CALLBACK_ADDRESS,
    OM_MIR_CSTRING_CONVERT,
    OM_MIR_RUNTIME_PRINT
} Om_Mir_Opcode;

typedef enum Om_Mir_Terminator_Kind {
    OM_MIR_TERMINATOR_NONE = 0,
    OM_MIR_BR,
    OM_MIR_COND_BR,
    OM_MIR_SWITCH,
    OM_MIR_RETURN,
    OM_MIR_UNREACHABLE
} Om_Mir_Terminator_Kind;

typedef enum Om_Mir_Instruction_Flag {
    OM_MIR_INSTRUCTION_FLAG_NONE = 0,
    OM_MIR_INSTRUCTION_FLAG_NATIVE_BLOCKING = 1u << 0,
    OM_MIR_INSTRUCTION_FLAG_OWNERSHIP_PROOF = 1u << 1
} Om_Mir_Instruction_Flag;

typedef enum Om_Mir_Runtime_Fault_Kind {
    OM_MIR_RUNTIME_FAULT_ZERO_RANGE_STEP = 1,
    OM_MIR_RUNTIME_FAULT_ACTOR_FAILED = 2
} Om_Mir_Runtime_Fault_Kind;

typedef struct Om_Mir_Instruction_Spec {
    Om_Mir_Opcode opcode;
    Om_Type_Id type_id;
    Om_Hir_Operator operator_kind;
    Om_Function_Instance_Id callee_instance_id;
    Om_Symbol_Id symbol_id;
    Om_Source_Span span;
    uint32_t effects;
    uint32_t flags;
    uint64_t integer_value;
    double float_value;
    Om_String_View string_value;
} Om_Mir_Instruction_Spec;

typedef struct Om_Mir_Edge_Input {
    Om_Mir_Block_Id target_block_id;
    const Om_Mir_Value_Id *arguments;
    size_t argument_count;
    bool has_case_value;
    bool is_default_case;
    int64_t case_value;
} Om_Mir_Edge_Input;

typedef struct Om_Mir_Function {
    Om_Mir_Function_Id id;
    Om_Function_Instance_Id instance_id;
    Om_Mir_Function_Kind kind;
    Om_Module_Id module_id;
    Om_Symbol_Id symbol_id;
    /* Borrowed from the owning resolution project for native debug names. */
    Om_String_View debug_name;
    Om_Type_Id signature_type_id;
    Om_Mir_Block_Id entry_block_id;
    uint32_t effects;
    Om_Source_Span span;
} Om_Mir_Function;

typedef struct Om_Mir_Value {
    Om_Mir_Value_Id id;
    Om_Type_Id type_id;
    Om_Mir_Block_Id defining_block_id;
    Om_Mir_Instruction_Id defining_instruction_id;
    bool is_block_parameter;
} Om_Mir_Value;

typedef struct Om_Mir_Instruction {
    Om_Mir_Instruction_Id id;
    Om_Mir_Block_Id block_id;
    Om_Mir_Value_Id result_id;
    Om_Mir_Opcode opcode;
    Om_Hir_Operator operator_kind;
    Om_Function_Instance_Id callee_instance_id;
    Om_Symbol_Id symbol_id;
    Om_Source_Span span;
    uint32_t operand_start;
    uint32_t operand_count;
    uint32_t effects;
    uint32_t flags;
    uint64_t integer_value;
    double float_value;
    uint32_t string_start;
    uint32_t string_length;
} Om_Mir_Instruction;

typedef struct Om_Mir_Edge {
    Om_Mir_Block_Id target_block_id;
    uint32_t argument_start;
    uint32_t argument_count;
    bool has_case_value;
    bool is_default_case;
    int64_t case_value;
} Om_Mir_Edge;

typedef struct Om_Mir_Terminator {
    Om_Mir_Terminator_Kind kind;
    Om_Mir_Value_Id value_id;
    uint32_t edge_start;
    uint32_t edge_count;
} Om_Mir_Terminator;

typedef struct Om_Mir_Block {
    Om_Mir_Block_Id id;
    Om_Mir_Function_Id function_id;
    uint32_t parameter_start;
    uint32_t parameter_count;
    uint32_t instruction_start;
    uint32_t instruction_count;
    Om_Mir_Terminator terminator;
    bool has_terminator;
} Om_Mir_Block;

typedef struct Om_Mir_Field_Descriptor {
    Om_Symbol_Id symbol_id;
    Om_Type_Id type_id;
} Om_Mir_Field_Descriptor;

typedef struct Om_Mir_Nominal_Descriptor {
    Om_Symbol_Id symbol_id;
    Om_Type_Kind type_kind;
    uint32_t field_start;
    uint32_t field_count;
    Om_System_Message_Id system_message_id;
    Om_String_View canonical_module_path;
    Om_String_View name;
} Om_Mir_Nominal_Descriptor;

typedef struct Om_Mir_Actor_State_Initializer {
    Om_Symbol_Id field_symbol_id;
    Om_Function_Instance_Id function_instance_id;
} Om_Mir_Actor_State_Initializer;

typedef struct Om_Mir_Actor_Handler {
    Om_Symbol_Id message_symbol_id;
    Om_Type_Id message_type_id;
    Om_Function_Instance_Id function_instance_id;
} Om_Mir_Actor_Handler;

typedef struct Om_Mir_Actor_Descriptor {
    Om_Symbol_Id symbol_id;
    Om_Type_Id state_type_id;
    uint32_t state_initializer_start;
    uint32_t state_initializer_count;
    uint32_t handler_start;
    uint32_t handler_count;
    Om_String_View canonical_module_path;
    Om_String_View name;
} Om_Mir_Actor_Descriptor;

typedef struct Om_Mir_Project {
    const Om_Type_Store *type_store;
    /* Borrowed from the source HIR; used to resolve Native call metadata. */
    const Om_Resolution_Result *resolution;
    const Om_Mir_Function *functions;
    size_t function_count;
    const Om_Mir_Block *blocks;
    size_t block_count;
    const Om_Mir_Value *values;
    size_t value_count;
    const Om_Mir_Instruction *instructions;
    size_t instruction_count;
    const Om_Mir_Value_Id *operands;
    size_t operand_count;
    const Om_Mir_Edge *edges;
    size_t edge_count;
    const Om_Mir_Value_Id *edge_arguments;
    size_t edge_argument_count;
    const Om_Mir_Nominal_Descriptor *nominals;
    size_t nominal_count;
    const Om_Mir_Field_Descriptor *fields;
    size_t field_count;
    const Om_Mir_Actor_Descriptor *actors;
    size_t actor_count;
    const Om_Mir_Actor_State_Initializer *actor_state_initializers;
    size_t actor_state_initializer_count;
    const Om_Mir_Actor_Handler *actor_handlers;
    size_t actor_handler_count;
    Om_Symbol_Id actor_down_message_symbol_id;
    Om_Type_Id actor_down_message_type_id;
    const char *string_data;
    size_t string_data_length;
    void *owned_storage;
} Om_Mir_Project;

typedef enum Om_Mir_Diagnostic_Code {
    OM_MIR_DIAGNOSTIC_E1101 = 1101,
    OM_MIR_DIAGNOSTIC_E1102,
    OM_MIR_DIAGNOSTIC_E1103,
    OM_MIR_DIAGNOSTIC_E1104,
    OM_MIR_DIAGNOSTIC_E1105,
    OM_MIR_DIAGNOSTIC_E1106
} Om_Mir_Diagnostic_Code;

typedef struct Om_Mir_Diagnostic {
    Om_Mir_Diagnostic_Code code;
    Om_Mir_Function_Id function_id;
    Om_Mir_Block_Id block_id;
    Om_Mir_Value_Id value_id;
    Om_Source_Span span;
} Om_Mir_Diagnostic;

typedef struct Om_Mir_Verification_Result {
    const Om_Mir_Diagnostic *diagnostics;
    size_t diagnostic_count;
    void *owned_diagnostics;
} Om_Mir_Verification_Result;

typedef struct Om_Mir_Lowering_Result {
    const Om_Mir_Diagnostic *diagnostics;
    size_t diagnostic_count;
    void *owned_diagnostics;
} Om_Mir_Lowering_Result;

typedef struct Om_Mir_Text {
    char *data;
    size_t length;
} Om_Mir_Text;

Om_Status om_mir_project_init(Om_Mir_Project *project,
                              const Om_Type_Store *type_store);
void om_mir_project_destroy(Om_Mir_Project *project);
Om_Status om_mir_add_nominal_descriptor(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_Type_Kind type_kind,
    const Om_Mir_Field_Descriptor *fields,
    size_t field_count
);
Om_Status om_mir_set_system_message_id(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_System_Message_Id system_message_id
);
Om_Status om_mir_set_nominal_identity(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_String_View canonical_module_path,
    Om_String_View name
);
Om_Status om_mir_add_actor_descriptor(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_Type_Id state_type_id,
    const Om_Mir_Actor_State_Initializer *initializers,
    size_t initializer_count,
    const Om_Mir_Actor_Handler *handlers,
    size_t handler_count
);
Om_Status om_mir_set_actor_identity(
    Om_Mir_Project *project,
    Om_Symbol_Id symbol_id,
    Om_String_View canonical_module_path,
    Om_String_View name
);
Om_Status om_mir_lower_project(const Om_Hir_Project *hir,
                               const Om_Hir_Specialization *specialization,
                               Om_Mir_Project *result,
                               Om_Mir_Lowering_Result *lowering_result);
void om_mir_lowering_result_destroy(Om_Mir_Lowering_Result *result);
Om_Status om_mir_add_function(Om_Mir_Project *project,
                              Om_Function_Instance_Id instance_id,
                              Om_Mir_Function_Kind kind,
                              Om_Module_Id module_id,
                              Om_Symbol_Id symbol_id,
                              Om_Type_Id signature_type_id,
                              uint32_t effects,
                              Om_Source_Span span,
                              Om_Mir_Function_Id *function_id);
Om_Status om_mir_set_function_debug_name(Om_Mir_Project *project,
                                         Om_Mir_Function_Id function_id,
                                         Om_String_View name);
Om_Status om_mir_add_block(Om_Mir_Project *project,
                           Om_Mir_Function_Id function_id,
                           Om_Mir_Block_Id *block_id);
Om_Status om_mir_add_block_parameter(Om_Mir_Project *project,
                                     Om_Mir_Block_Id block_id,
                                     Om_Type_Id type_id,
                                     Om_Mir_Value_Id *value_id);
Om_Status om_mir_add_instruction(Om_Mir_Project *project,
                                 Om_Mir_Block_Id block_id,
                                 const Om_Mir_Instruction_Spec *spec,
                                 const Om_Mir_Value_Id *operands,
                                 size_t operand_count,
                                 Om_Mir_Value_Id *result_id);
Om_Status om_mir_set_terminator(Om_Mir_Project *project,
                                Om_Mir_Block_Id block_id,
                                Om_Mir_Terminator_Kind kind,
                                Om_Mir_Value_Id value_id,
                                const Om_Mir_Edge_Input *edges,
                                size_t edge_count);
Om_Status om_mir_verify_project(const Om_Mir_Project *project,
                                Om_Mir_Verification_Result *result);
void om_mir_verification_result_destroy(Om_Mir_Verification_Result *result);
Om_Status om_mir_dump(const Om_Mir_Project *project, Om_Mir_Text *result);
void om_mir_text_destroy(Om_Mir_Text *text);
const char *om_mir_opcode_name(Om_Mir_Opcode opcode);
const char *om_mir_terminator_name(Om_Mir_Terminator_Kind kind);
const char *om_mir_diagnostic_code_name(Om_Mir_Diagnostic_Code code);

#endif
