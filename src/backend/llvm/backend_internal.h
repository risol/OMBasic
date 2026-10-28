#ifndef OMBASIC_BACKEND_LLVM_INTERNAL_H
#define OMBASIC_BACKEND_LLVM_INTERNAL_H

#include "ombasic/backend/llvm_backend.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "llvm-c/Analysis.h"
#include "llvm-c/Core.h"
#include "llvm-c/DebugInfo.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"

#include "abi/native_abi.h"
#include "ombasic/foundation/fingerprint.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/runtime/abi/base.h"
#include "ombasic/semantic/system_actors.h"
#include "ombasic/target/target.h"

#ifndef OM_LLVM_TARGET_TRIPLE
#define OM_LLVM_TARGET_TRIPLE ""
#endif
#ifndef OM_LLVM_OPT_LEVEL
#define OM_LLVM_OPT_LEVEL 0
#endif
#if OM_LLVM_OPT_LEVEL != 0 && OM_LLVM_OPT_LEVEL != 2
#error "OM_LLVM_OPT_LEVEL must be 0 or 2"
#endif

typedef struct Om_Llvm_Type_Descriptor_Cache {
    Om_Type_Id type_id;
    LLVMValueRef global;
    uint64_t fingerprint;
} Om_Llvm_Type_Descriptor_Cache;

typedef struct Om_Llvm_Native_Abi_Type {
    LLVMTypeRef llvm_type;
    unsigned bit_width;
    bool is_integer;
    bool is_signed;
    bool is_float;
    bool is_boolean;
    bool is_pointer;
} Om_Llvm_Native_Abi_Type;

#define OM_LLVM_NATIVE_CALLBACK_TRAMPOLINE_COUNT 64u

typedef struct Om_Llvm_Native_Callback_Thunk_Set {
    Om_Native_Abi_Type *parameter_types;
    size_t parameter_count;
    LLVMTypeRef function_type;
    LLVMTypeRef function_pointer_type;
    LLVMValueRef candidates;
} Om_Llvm_Native_Callback_Thunk_Set;

typedef struct Om_Llvm_Native_Callback_Call {
    size_t callback_count;
    size_t source_parameter_count;
    size_t *binding_for_parameter;
    Om_Llvm_Native_Callback_Thunk_Set **thunk_sets;
    LLVMValueRef *message_descriptors;
} Om_Llvm_Native_Callback_Call;

typedef struct Om_Llvm_Backend_Context {
    const Om_Mir_Project *project;
    Om_Target target;
    const char *target_triple;
    const char *requested_target_triple;
    char *owned_target_triple;
    Om_Llvm_Optimization_Level optimization_level;
    Om_String_View source_root;
    const Om_Backend_Source_Location *source_locations;
    size_t source_location_count;
    const Om_Mir_Instruction *current_instruction;
    const Om_Mir_Function *current_function;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTargetMachineRef target_machine;
    LLVMTargetDataRef target_data;
    LLVMDIBuilderRef debug_builder;
    LLVMMetadataRef debug_compile_unit;
    LLVMMetadataRef *debug_files;
    LLVMMetadataRef *debug_subprograms;
    bool debug_finalized;
    LLVMTypeRef i1_type;
    LLVMTypeRef i8_type;
    LLVMTypeRef i32_type;
    LLVMTypeRef i64_type;
    LLVMTypeRef double_type;
    LLVMTypeRef byte_pointer_type;
    LLVMTypeRef runtime_source_span_type;
    LLVMTypeRef runtime_type_descriptor_type;
    LLVMTypeRef runtime_field_descriptor_type;
    LLVMTypeRef runtime_actor_handler_descriptor_type;
    LLVMTypeRef runtime_actor_descriptor_type;
    LLVMTypeRef runtime_actor_ref_type;
    LLVMTypeRef runtime_program_descriptor_type;
    LLVMValueRef *functions;
    LLVMBasicBlockRef *blocks;
    LLVMBasicBlockRef *block_tails;
    LLVMValueRef *values;
    LLVMValueRef runtime_context_create;
    LLVMValueRef runtime_context_destroy;
    LLVMValueRef runtime_set_source_spans;
    LLVMValueRef runtime_validate_program;
    LLVMValueRef runtime_run;
    LLVMValueRef runtime_fault;
    LLVMValueRef runtime_roots_push;
    LLVMValueRef runtime_roots_pop;
    LLVMValueRef runtime_native_pointer_create;
    LLVMValueRef runtime_native_pointer_borrow;
    LLVMValueRef runtime_native_pointer_take;
    LLVMValueRef runtime_string_create;
    LLVMValueRef runtime_string_cstring_view;
    LLVMValueRef runtime_string_from_cstring;
    LLVMValueRef runtime_string_from_owned_cstring;
    LLVMValueRef runtime_string_format;
    LLVMValueRef runtime_string_concat;
    LLVMValueRef runtime_print;
    LLVMValueRef runtime_record_create;
    LLVMValueRef runtime_record_set;
    LLVMValueRef runtime_record_get;
    LLVMValueRef runtime_list_create;
    LLVMValueRef runtime_list_append;
    LLVMValueRef runtime_list_get;
    LLVMValueRef runtime_list_length;
    LLVMValueRef runtime_map_create;
    LLVMValueRef runtime_map_set;
    LLVMValueRef runtime_map_get;
    LLVMValueRef runtime_map_key_at;
    LLVMValueRef runtime_map_length;
    LLVMValueRef runtime_actor_system_start;
    LLVMValueRef runtime_actor_system_wait;
    LLVMValueRef runtime_actor_spawn_id;
    LLVMValueRef runtime_actor_spawn_at;
    LLVMValueRef runtime_distribution_create;
    LLVMValueRef runtime_distribution_destroy;
    LLVMValueRef runtime_actor_self_id;
    LLVMValueRef runtime_actor_state_object;
    LLVMValueRef runtime_actor_message_object;
    LLVMValueRef runtime_actor_send_id;
    LLVMValueRef runtime_actor_watch_id;
    LLVMValueRef runtime_actor_stop;
    LLVMValueRef runtime_native_callback_register_many;
    LLVMValueRef runtime_native_callback_unregister_many;
    LLVMValueRef runtime_native_callback_post;
    LLVMTypeRef native_callback_function_pointer_type;
    LLVMValueRef *root_frames;
    LLVMValueRef *root_arrays;
    uint32_t *root_slot_indices;
    uint32_t *root_slot_counts_by_value;
    size_t *root_counts;
    Om_Llvm_Type_Descriptor_Cache *descriptors;
    size_t descriptor_count;
    size_t descriptor_capacity;
    Om_Llvm_Native_Callback_Thunk_Set *callback_thunk_sets;
    size_t callback_thunk_set_count;
    size_t callback_thunk_set_capacity;
    LLVMValueRef *actor_descriptors;
    LLVMValueRef program_descriptor;
    uint32_t generated_block_count;
} Om_Llvm_Backend_Context;

enum Om_Llvm_Runtime_Fault_Code {
    OM_LLVM_RUNTIME_FAULT_INTEGER_OVERFLOW = 1,
    OM_LLVM_RUNTIME_FAULT_DIVISION_BY_ZERO = 2,
    OM_LLVM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS = 3,
    OM_LLVM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND = 4,
    OM_LLVM_RUNTIME_FAULT_ZERO_RANGE_STEP = 5,
    OM_LLVM_RUNTIME_FAULT_OPERATION_FAILED = 6,
    OM_LLVM_RUNTIME_FAULT_ACTOR_FAILED = 7
};

/* Private implementation entry points shared by LLVM backend modules. */
Om_Status emit_actor_instruction(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 Om_Mir_Block_Id block_id,
                                 const Om_Mir_Instruction *instruction,
                                 const Om_Mir_Value_Id *operands,
                                 LLVMValueRef *result,
                                 bool *handled);
Om_Status create_actor_state_initializer(
    Om_Llvm_Backend_Context *backend,
    size_t actor_index,
    LLVMValueRef *initializer_out);
Om_Status create_actor_handler_wrapper(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Actor_Handler *handler,
    LLVMValueRef *wrapper_out);
LLVMValueRef create_private_string(Om_Llvm_Backend_Context *backend,
                                          const char *name,
                                          const char *text);
Om_Status actor_canonical_name_text(
    const Om_Mir_Actor_Descriptor *actor,
    char **text);
Om_Status callback_status_check(Om_Llvm_Backend_Context *backend,
                                       LLVMValueRef function,
                                       LLVMValueRef result_status);
bool native_metadata_name_equal(Om_String_View left,
                                       Om_String_View right);
Om_Status emit_native_callback_registration(
    Om_Llvm_Backend_Context *backend,
    const Om_Mir_Function *function,
    Om_Mir_Block_Id block_id,
    const Om_Mir_Instruction *instruction,
    size_t callback_count,
    Om_Llvm_Native_Callback_Thunk_Set *const *thunk_sets,
    LLVMValueRef *message_descriptors,
    LLVMValueRef *registered_slot_out,
    LLVMTypeRef *registered_array_type_out);
void native_callback_call_destroy(Om_Llvm_Native_Callback_Call *call);
Om_Status native_callback_call_prepare(
    Om_Llvm_Backend_Context *backend,
    const Om_Omi_Module *module,
    const Om_Omi_Native_Function *native_function,
    Om_Llvm_Native_Callback_Call *call);
Om_Status create_basic_blocks(Om_Llvm_Backend_Context *backend);
Om_Status create_block_parameters(Om_Llvm_Backend_Context *backend);
Om_Status add_checked_continuation(Om_Llvm_Backend_Context *backend,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef failure_condition,
                                          uint32_t fault_code,
                                          uint32_t source_span_id);
Om_Status emit_instruction(Om_Llvm_Backend_Context *backend,
                                  const Om_Mir_Function *function,
                                  const Om_Mir_Block *block,
                                  const Om_Mir_Instruction *instruction);
Om_Status emit_terminator(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 const Om_Mir_Block *block);
Om_Status initialize_debug_info(Om_Llvm_Backend_Context *backend,
                                       const char *target_triple);
void set_instruction_debug_location(Om_Llvm_Backend_Context *backend,
                                           const Om_Mir_Function *function,
                                           const Om_Mir_Instruction *instruction);
Om_Status create_source_span_table(Om_Llvm_Backend_Context *backend,
                                          LLVMValueRef *table);
Om_Status runtime_value_kind(Om_Type_Kind kind, uint32_t *runtime_kind);
const Om_Mir_Nominal_Descriptor *nominal_for_type(
    const Om_Mir_Project *project,
    const Om_Type_View *type
);
Om_Status runtime_descriptor(Om_Llvm_Backend_Context *backend,
                                    Om_Type_Id type_id,
                                    unsigned depth,
                                    LLVMValueRef *descriptor);
Om_Status create_actor_runtime_descriptors(Om_Llvm_Backend_Context *backend);
Om_Status create_program_descriptor(
    Om_Llvm_Backend_Context *backend,
    LLVMValueRef source_span_table,
    LLVMValueRef entry_thunk);
Om_Status run_optimization_pipeline(Om_Llvm_Backend_Context *backend);
Om_Status om_llvm_backend_emit_file(Om_Llvm_Backend_Context *backend,
                                   Om_Llvm_Artifact artifact,
                                   const char *output_path);
LLVMValueRef value_at(const Om_Llvm_Backend_Context *backend,
                             Om_Mir_Value_Id value_id);
LLVMValueRef entry_alloca(Om_Llvm_Backend_Context *backend,
                                 const Om_Mir_Function *function,
                                 LLVMTypeRef type,
                                 const char *name);
LLVMValueRef function_for_instance(const Om_Llvm_Backend_Context *backend,
                                          Om_Function_Instance_Id instance_id);
Om_Status create_function_declarations(Om_Llvm_Backend_Context *backend);
LLVMValueRef create_string_constant(Om_Llvm_Backend_Context *backend,
                                           const Om_Mir_Instruction *instruction);
LLVMValueRef context_for_function(const Om_Llvm_Backend_Context *backend,
                                         const Om_Mir_Function *function);
const Om_Mir_Function *mir_function_for_instance(
    const Om_Mir_Project *project,
    Om_Function_Instance_Id instance_id);
Om_Status create_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                               LLVMValueRef value,
                                               LLVMTypeRef value_type,
                                               Om_Type_Kind kind,
                                               LLVMValueRef *storage);
Om_Status allocate_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                                Om_Type_Id type_id,
                                                LLVMValueRef *storage);
Om_Status load_runtime_value_storage(Om_Llvm_Backend_Context *backend,
                                            LLVMValueRef storage,
                                            Om_Type_Id type_id,
                                            LLVMValueRef *result);
Om_Status get_mir_value_storage(Om_Llvm_Backend_Context *backend,
                                       Om_Mir_Value_Id value_id,
                                       LLVMValueRef *storage,
                                       Om_Type_Kind *kind_out);
Om_Status emit_function_bodies(Om_Llvm_Backend_Context *backend);
Om_Status add_entry_wrapper(Om_Llvm_Backend_Context *backend,
                                   Om_Symbol_Id entry_symbol);
Om_Status store_value_root(Om_Llvm_Backend_Context *backend,
                                  Om_Mir_Function_Id function_id,
                                  Om_Mir_Value_Id value_id,
                                  LLVMValueRef value);
Om_Status initialize_function_roots(Om_Llvm_Backend_Context *backend);
Om_Status om_llvm_backend_initialize_module(Om_Llvm_Backend_Context *backend);
Om_Status om_llvm_backend_verify_module(Om_Llvm_Backend_Context *backend);
Om_Status native_abi_type(Om_Llvm_Backend_Context *backend,
                                 Om_Native_Abi_Type abi_type,
                                 bool is_result,
                                 Om_Llvm_Native_Abi_Type *result);
Om_Status add_native_enum_attribute(Om_Llvm_Backend_Context *backend,
                                           LLVMValueRef function,
                                           LLVMValueRef call,
                                           LLVMAttributeIndex index,
                                           const char *name,
                                           uint64_t value);
Om_Status add_native_integer_extension_attribute(
    Om_Llvm_Backend_Context *backend,
    LLVMValueRef function,
    LLVMValueRef call,
    LLVMAttributeIndex index,
    const Om_Llvm_Native_Abi_Type *abi_type);
Om_Status emit_native_call(Om_Llvm_Backend_Context *backend,
                                  const Om_Mir_Function *function,
                                  Om_Mir_Block_Id block_id,
                                  const Om_Mir_Instruction *instruction,
                                  const Om_Mir_Value_Id *operands,
                                  LLVMValueRef *result);
Om_Status initialize_types(Om_Llvm_Backend_Context *backend);
Om_Status emit_runtime_status_check(Om_Llvm_Backend_Context *backend,
                                           Om_Mir_Block_Id block_id,
                                           LLVMValueRef status_value);
Om_Status emit_runtime_status_check_as(
    Om_Llvm_Backend_Context *backend,
    Om_Mir_Block_Id block_id,
    LLVMValueRef status_value,
    uint32_t fault_code
);
Om_Status emit_runtime_object_call(Om_Llvm_Backend_Context *backend,
                                          const Om_Mir_Function *function,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef callee,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count,
                                          LLVMValueRef *result);
Om_Status emit_runtime_status_call(Om_Llvm_Backend_Context *backend,
                                          Om_Mir_Block_Id block_id,
                                          LLVMValueRef function,
                                          LLVMValueRef *arguments,
                                          unsigned argument_count);
Om_Status resolved_type(const Om_Type_Store *store,
                               Om_Type_Id type_id,
                               Om_Type_Id *resolved_id,
                               Om_Type_View *view);
Om_Status language_function_type(Om_Llvm_Backend_Context *backend,
                                        Om_Type_Id type_id,
                                        unsigned depth,
                                        LLVMTypeRef *function_type);
Om_Status language_type(Om_Llvm_Backend_Context *backend,
                               Om_Type_Id type_id,
                               unsigned depth,
                               LLVMTypeRef *llvm_type);
Om_Status type_kind(Om_Llvm_Backend_Context *backend,
                           Om_Type_Id type_id,
                           Om_Type_Kind *kind);
Om_Status native_record_llvm_type(Om_Llvm_Backend_Context *backend,
                                         const Om_Omi_Module *module,
                                         size_t record_index,
                                         LLVMTypeRef *llvm_type);

#endif
