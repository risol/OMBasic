#ifndef OMBASIC_BYTECODE_BYTECODE_H
#define OMBASIC_BYTECODE_BYTECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/system_actors.h"
#include "ombasic/source/span.h"
#include "ombasic/syntax/parser.h"
#include "ombasic/semantic/semantic_analysis.h"

#define OM_BYTECODE_FORMAT_VERSION 3u
#define OM_BYTECODE_DEFAULT_MAX_BYTES ((size_t)64 * (size_t)1024 * (size_t)1024)

typedef enum Om_Bytecode_Constant_Kind {
    OM_BYTECODE_CONSTANT_NULL = 0,
    OM_BYTECODE_CONSTANT_UNIT,
    OM_BYTECODE_CONSTANT_BOOL,
    OM_BYTECODE_CONSTANT_INT,
    OM_BYTECODE_CONSTANT_FLOAT,
    OM_BYTECODE_CONSTANT_DOWN_REASON,
    OM_BYTECODE_CONSTANT_STRING
} Om_Bytecode_Constant_Kind;

typedef struct Om_Bytecode_Constant {
    Om_Bytecode_Constant_Kind kind;
    union {
        bool boolean_value;
        int64_t integer_value;
        double float_value;
        uint32_t down_reason_value;
        Om_String_View string_value;
    } as;
} Om_Bytecode_Constant;

typedef enum Om_Bytecode_Opcode {
    OM_BC_NOP = 0,
    OM_BC_LOAD_CONST,
    OM_BC_LOAD_UNIT,
    OM_BC_LOAD_LOCAL,
    OM_BC_STORE_LOCAL,
    OM_BC_POP,
    OM_BC_DUP,
    OM_BC_ADD_INT,
    OM_BC_SUB_INT,
    OM_BC_MUL_INT,
    OM_BC_DIV_INT,
    OM_BC_MOD_INT,
    OM_BC_NEG_INT,
    OM_BC_ADD_FLOAT,
    OM_BC_SUB_FLOAT,
    OM_BC_MUL_FLOAT,
    OM_BC_DIV_FLOAT,
    OM_BC_NEG_FLOAT,
    OM_BC_NOT_BOOL,
    OM_BC_EQ,
    OM_BC_NE,
    OM_BC_LT,
    OM_BC_LE,
    OM_BC_GT,
    OM_BC_GE,
    OM_BC_JUMP,
    OM_BC_JUMP_IF_FALSE,
    OM_BC_JUMP_IF_TRUE,
    OM_BC_CALL,
    OM_BC_CALL_INDIRECT,
    OM_BC_LOAD_FUNCTION,
    OM_BC_RETURN,
    OM_BC_MAKE_RECORD,
    OM_BC_MAKE_MESSAGE,
    OM_BC_MAKE_OK,
    OM_BC_MAKE_ERROR,
    OM_BC_IS_OK,
    OM_BC_UNWRAP_ERROR,
    OM_BC_GET_FIELD,
    OM_BC_WITH_FIELD,
    OM_BC_MAKE_LIST,
    OM_BC_MAKE_MAP,
    OM_BC_GET_INDEX,
    OM_BC_BUILD_STRING,
    OM_BC_TO_STRING,
    OM_BC_CHECK_RESULT,
    OM_BC_UNWRAP_OK,
    OM_BC_RETURN_ERROR,
    OM_BC_SPAWN,
    OM_BC_SEND,
    OM_BC_WATCH,
    OM_BC_STOP,
    OM_BC_SELF,
    OM_BC_FOR_RANGE_TEST,
    OM_BC_ITER_INIT,
    OM_BC_ITER_NEXT,
    OM_BC_LOAD_STATE,
    OM_BC_STORE_STATE,
    OM_BC_LOAD_SYSTEM_ACTOR,
    OM_BC_OPCODE_COUNT
} Om_Bytecode_Opcode;

typedef struct Om_Bytecode_Instruction {
    Om_Bytecode_Opcode opcode;
    uint32_t operand_a;
    uint32_t operand_b;
    int64_t immediate;
} Om_Bytecode_Instruction;

typedef enum Om_Bytecode_Function_Flag {
    OM_BYTECODE_FUNCTION_NONE = 0,
    OM_BYTECODE_FUNCTION_ACTION = 1u << 0,
    OM_BYTECODE_FUNCTION_RECEIVE = 1u << 1,
    OM_BYTECODE_FUNCTION_EXTERNAL = 1u << 2,
    OM_BYTECODE_FUNCTION_VARIADIC = 1u << 3
} Om_Bytecode_Function_Flag;

typedef struct Om_Bytecode_Function {
    uint32_t name_constant;
    uint32_t entry_pc;
    uint32_t end_pc;
    uint32_t parameter_count;
    uint32_t local_count;
    uint32_t max_stack;
    uint32_t flags;
} Om_Bytecode_Function;

typedef struct Om_Bytecode_Source_Map_Entry {
    uint32_t start_pc;
    uint32_t end_pc;
    Om_Source_Span span;
} Om_Bytecode_Source_Map_Entry;

typedef struct Om_Bytecode_Actor_Handler {
    Om_Symbol_Id message_symbol;
    uint32_t function_index;
} Om_Bytecode_Actor_Handler;

typedef struct Om_Bytecode_Actor {
    Om_Symbol_Id actor_symbol;
    uint32_t initializer_function;
    uint32_t state_count;
    uint32_t first_handler;
    uint32_t handler_count;
} Om_Bytecode_Actor;

typedef enum Om_Bytecode_Schema_Kind {
    OM_BYTECODE_SCHEMA_RECORD = 0,
    OM_BYTECODE_SCHEMA_MESSAGE
} Om_Bytecode_Schema_Kind;

typedef struct Om_Bytecode_Schema_Field {
    uint32_t name_constant;
} Om_Bytecode_Schema_Field;

typedef struct Om_Bytecode_Schema {
    Om_Symbol_Id symbol_id;
    Om_Bytecode_Schema_Kind kind;
    uint32_t first_field;
    uint32_t field_count;
    uint64_t fingerprint;
} Om_Bytecode_Schema;

/*
 * In-memory arrays are owned by owned_storage. Serialized images use the
 * field-wise little-endian OMBC format; no C structure layout is persisted.
 */
typedef struct Om_Bytecode_Image {
    const Om_Bytecode_Constant *constants;
    size_t constant_count;
    const Om_Bytecode_Function *functions;
    size_t function_count;
    const Om_Bytecode_Instruction *instructions;
    size_t instruction_count;
    const Om_Bytecode_Source_Map_Entry *source_map;
    size_t source_map_count;
    const Om_Bytecode_Actor *actors;
    size_t actor_count;
    const Om_Bytecode_Actor_Handler *handlers;
    size_t handler_count;
    const Om_Bytecode_Schema *schemas;
    size_t schema_count;
    const Om_Bytecode_Schema_Field *schema_fields;
    size_t schema_field_count;
    uint64_t program_fingerprint;
    uint64_t interface_fingerprint;
    void *owned_storage;
} Om_Bytecode_Image;

typedef struct Om_Bytecode_Builder {
    void *owned_storage;
} Om_Bytecode_Builder;

typedef enum Om_Bytecode_Verify_Code {
    OM_BYTECODE_VERIFY_OK = 0,
    OM_BYTECODE_VERIFY_INVALID_IMAGE,
    OM_BYTECODE_VERIFY_INVALID_CONSTANT,
    OM_BYTECODE_VERIFY_INVALID_FUNCTION,
    OM_BYTECODE_VERIFY_INVALID_OPCODE,
    OM_BYTECODE_VERIFY_INVALID_OPERAND,
    OM_BYTECODE_VERIFY_INVALID_JUMP,
    OM_BYTECODE_VERIFY_STACK_UNDERFLOW,
    OM_BYTECODE_VERIFY_STACK_MISMATCH,
    OM_BYTECODE_VERIFY_STACK_LIMIT,
    OM_BYTECODE_VERIFY_INVALID_SOURCE_MAP,
    OM_BYTECODE_VERIFY_INVALID_ACTOR
} Om_Bytecode_Verify_Code;

typedef struct Om_Bytecode_Verify_Result {
    bool valid;
    Om_Bytecode_Verify_Code code;
    uint32_t function_index;
    uint32_t instruction_index;
} Om_Bytecode_Verify_Result;

typedef enum Om_Bytecode_Emit_Code {
    OM_BYTECODE_EMIT_UNSUPPORTED_NODE = 0,
    OM_BYTECODE_EMIT_UNRESOLVED_SYMBOL,
    OM_BYTECODE_EMIT_INVALID_INPUT
} Om_Bytecode_Emit_Code;

typedef struct Om_Bytecode_Emit_Diagnostic {
    Om_Bytecode_Emit_Code code;
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Source_Span span;
} Om_Bytecode_Emit_Diagnostic;

typedef struct Om_Bytecode_Emit_Result {
    Om_Bytecode_Image image;
    const Om_Bytecode_Emit_Diagnostic *diagnostics;
    size_t diagnostic_count;
    void *owned_diagnostics;
} Om_Bytecode_Emit_Result;

Om_Status om_bytecode_builder_init(Om_Bytecode_Builder *builder);
void om_bytecode_builder_destroy(Om_Bytecode_Builder *builder);
Om_Status om_bytecode_builder_add_constant(
    Om_Bytecode_Builder *builder,
    const Om_Bytecode_Constant *constant,
    uint32_t *constant_index
);
Om_Status om_bytecode_builder_begin_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
);
Om_Status om_bytecode_builder_reserve_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
);
Om_Status om_bytecode_builder_begin_function_body(
    Om_Bytecode_Builder *builder,
    uint32_t function_index
);
Om_Status om_bytecode_builder_add_external_function(
    Om_Bytecode_Builder *builder,
    uint32_t name_constant,
    uint32_t parameter_count,
    uint32_t flags,
    uint32_t *function_index
);
Om_Status om_bytecode_builder_set_function_layout(
    Om_Bytecode_Builder *builder,
    uint32_t function_index,
    uint32_t parameter_count,
    uint32_t local_count
);
Om_Status om_bytecode_builder_end_function(
    Om_Bytecode_Builder *builder,
    uint32_t function_index
);
Om_Status om_bytecode_builder_emit(
    Om_Bytecode_Builder *builder,
    Om_Bytecode_Opcode opcode,
    uint32_t operand_a,
    uint32_t operand_b,
    int64_t immediate,
    Om_Source_Span span,
    uint32_t *instruction_index
);
Om_Status om_bytecode_builder_patch_jump(
    Om_Bytecode_Builder *builder,
    uint32_t instruction_index,
    uint32_t target_pc
);
Om_Status om_bytecode_builder_add_actor(
    Om_Bytecode_Builder *builder,
    Om_Symbol_Id actor_symbol,
    uint32_t initializer_function,
    uint32_t state_count,
    uint32_t *actor_index
);
Om_Status om_bytecode_builder_add_actor_handler(
    Om_Bytecode_Builder *builder,
    uint32_t actor_index,
    Om_Symbol_Id message_symbol,
    uint32_t function_index
);
Om_Status om_bytecode_builder_add_schema(
    Om_Bytecode_Builder *builder,
    Om_Symbol_Id symbol_id,
    Om_Bytecode_Schema_Kind kind,
    const uint32_t *field_name_constants,
    size_t field_count,
    uint64_t fingerprint,
    uint32_t *schema_index
);
/* Adds Console, Timer, and File_System request/completion message schemas. */
Om_Status om_bytecode_builder_add_system_actor_schemas(
    Om_Bytecode_Builder *builder
);
Om_Status om_bytecode_builder_finish(
    Om_Bytecode_Builder *builder,
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *verify_result
);
void om_bytecode_image_destroy(Om_Bytecode_Image *image);

Om_Status om_bytecode_verify(
    const Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *result
);
Om_Status om_bytecode_encode(
    const Om_Bytecode_Image *image,
    Om_Byte_Slice *encoded
);
Om_Status om_bytecode_decode(
    Om_Const_Byte_Slice encoded,
    size_t maximum_byte_length,
    Om_Bytecode_Image *image,
    Om_Bytecode_Verify_Result *verify_result
);
void om_byte_slice_destroy(Om_Byte_Slice *slice);
Om_Status om_bytecode_source_map_lookup(
    const Om_Bytecode_Image *image,
    uint32_t instruction_index,
    Om_Source_Span *span
);
const char *om_bytecode_opcode_name(Om_Bytecode_Opcode opcode);
const char *om_bytecode_verify_code_name(Om_Bytecode_Verify_Code code);
void om_bytecode_emit_result_destroy(Om_Bytecode_Emit_Result *result);

Om_Status om_bytecode_emit_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolution_Result *resolution,
    const Om_Type_Inference_Result *inference,
    const Om_Semantic_Analysis_Result *semantic,
    Om_Bytecode_Emit_Result *result
);

#endif
