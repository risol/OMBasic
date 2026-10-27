#ifndef OMBASIC_RUNTIME_VM_H
#define OMBASIC_RUNTIME_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/bytecode/bytecode.h"
#include "ombasic/distribution/handshake.h"
#include "ombasic/distribution/transport.h"
#include "ombasic/ffi/native_call.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"

typedef struct Om_VM_Object Om_VM_Object;

typedef enum Om_VM_Object_Kind {
    OM_VM_OBJECT_STRING = 0,
    OM_VM_OBJECT_LIST,
    OM_VM_OBJECT_MAP,
    OM_VM_OBJECT_RECORD,
    OM_VM_OBJECT_MESSAGE,
    OM_VM_OBJECT_FUNCTION,
    OM_VM_OBJECT_RESULT,
    OM_VM_OBJECT_ITERATOR
} Om_VM_Object_Kind;

typedef enum Om_VM_Value_Tag {
    OM_VM_VALUE_UNINITIALIZED = 0,
    OM_VM_VALUE_NULL,
    OM_VM_VALUE_UNIT,
    OM_VM_VALUE_BOOL,
    OM_VM_VALUE_INT,
    OM_VM_VALUE_FLOAT,
    OM_VM_VALUE_ACTOR_ID,
    OM_VM_VALUE_NODE_REF,
    OM_VM_VALUE_ACTOR_REF,
    OM_VM_VALUE_DOWN_REASON,
    OM_VM_VALUE_OBJECT
} Om_VM_Value_Tag;

typedef struct Om_VM_Value {
    Om_VM_Value_Tag tag;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        uint64_t actor_id;
        uint64_t node_ref;
        uint32_t down_reason;
        Om_VM_Object *object;
    } as;
} Om_VM_Value;

typedef struct Om_VM_Limits {
    size_t max_stack_values;
    size_t max_call_frames;
    size_t max_heap_bytes;
    uint64_t max_instructions;
    size_t max_actors;
    size_t max_mailbox_messages;
    /* Zero selects a bounded default threshold for Actor-local collection. */
    size_t gc_threshold_bytes;
    /* Zero uses a small default pool and bytecode quantum. */
    size_t worker_count;
    size_t instruction_quantum;
} Om_VM_Limits;

typedef struct Om_VM {
    void *owned_state;
} Om_VM;

typedef struct Om_VM_Native_Callback_Message {
    size_t native_function_index;
    size_t parameter_index;
    Om_Symbol_Id message_symbol;
} Om_VM_Native_Callback_Message;

typedef enum Om_VM_Runtime_Error_Code {
    OM_VM_RUNTIME_ERROR_NONE = 0,
    OM_VM_RUNTIME_ERROR_INVALID_FUNCTION,
    OM_VM_RUNTIME_ERROR_ARITY,
    OM_VM_RUNTIME_ERROR_TYPE,
    OM_VM_RUNTIME_ERROR_UNINITIALIZED_LOCAL,
    OM_VM_RUNTIME_ERROR_INTEGER_OVERFLOW,
    OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO,
    OM_VM_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
    OM_VM_RUNTIME_ERROR_MISSING_FIELD,
    OM_VM_RUNTIME_ERROR_MISSING_KEY,
    OM_VM_RUNTIME_ERROR_BAD_RESULT_ACCESS,
    OM_VM_RUNTIME_ERROR_UNSUPPORTED_OPCODE,
    OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
    OM_VM_RUNTIME_ERROR_STEP_LIMIT,
    OM_VM_RUNTIME_ERROR_STACK_LIMIT,
    OM_VM_RUNTIME_ERROR_CALL_DEPTH_LIMIT,
    OM_VM_RUNTIME_ERROR_HEAP_LIMIT,
    OM_VM_RUNTIME_ERROR_INVALID_RANGE_STEP,
    OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
    OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
    OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
    OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
    OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE,
    OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
    OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT,
    OM_VM_RUNTIME_ERROR_DISTRIBUTION
} Om_VM_Runtime_Error_Code;

typedef struct Om_VM_Runtime_Error {
    Om_VM_Runtime_Error_Code code;
    uint32_t function_index;
    uint32_t instruction_index;
    bool has_source_span;
    Om_Source_Span source_span;
} Om_VM_Runtime_Error;

typedef struct Om_VM_Execution_Result {
    bool succeeded;
    Om_VM_Value value;
    Om_VM_Runtime_Error error;
    uint64_t actors_spawned;
    uint64_t messages_processed;
    uint64_t actors_stopped;
} Om_VM_Execution_Result;

Om_VM_Value om_vm_value_null(void);
Om_VM_Value om_vm_value_unit(void);
Om_VM_Value om_vm_value_bool(bool value);
Om_VM_Value om_vm_value_int(int64_t value);
Om_VM_Value om_vm_value_float(double value);
Om_VM_Value om_vm_value_actor_id(uint64_t value);
Om_VM_Value om_vm_value_node_ref(uint64_t value);
Om_VM_Value om_vm_value_actor_ref(uint64_t value);
bool om_vm_value_as_bool(Om_VM_Value value, bool *out);
bool om_vm_value_as_int(Om_VM_Value value, int64_t *out);
bool om_vm_value_as_float(Om_VM_Value value, double *out);
bool om_vm_value_as_actor_id(Om_VM_Value value, uint64_t *out);
bool om_vm_value_as_node_ref(Om_VM_Value value, uint64_t *out);
bool om_vm_value_as_actor_ref(Om_VM_Value value, uint64_t *out);
bool om_vm_value_as_string(Om_VM_Value value, Om_String_View *out);
bool om_vm_value_as_result(
    Om_VM_Value value,
    bool *is_ok,
    Om_VM_Value *payload
);
bool om_vm_value_as_list(
    Om_VM_Value value,
    const Om_VM_Value **items,
    size_t *item_count
);
bool om_vm_value_object_kind(Om_VM_Value value, Om_VM_Object_Kind *out);

/* The image is borrowed and must outlive the VM. Zero limits use safe defaults. */
Om_Status om_vm_init(
    Om_VM *vm,
    const Om_Bytecode_Image *image,
    Om_VM_Limits limits
);
/* Returns the VM's stable-for-its-lifetime identity and image fingerprints. */
Om_Status om_vm_distribution_handshake_info(
    const Om_VM *vm,
    Om_Distribution_Node_Handshake *handshake
);
/*
 * Completes the peer handshake, starts its mailbox receiver, and transfers
 * connection ownership to the VM. Call before execution. The connection is
 * closed if the attach attempt fails.
 */
Om_Status om_vm_distribution_attach_peer(
    Om_VM *vm,
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref
);
Om_Status om_vm_distribution_connect_peer(
    Om_VM *vm,
    Om_String_View host,
    uint16_t port,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref
);
Om_Status om_vm_distribution_accept_peer(
    Om_VM *vm,
    const Om_Distribution_Listener *listener,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref
);
/* Creates a VM-local ActorRef for an Actor ID on an attached peer. */
Om_Status om_vm_distribution_actor_ref(
    Om_VM *vm,
    Om_VM_Value node_ref,
    uint64_t remote_actor_id,
    Om_VM_Value *actor_ref
);
void om_vm_destroy(Om_VM *vm);
Om_Status om_vm_find_function(
    const Om_Bytecode_Image *image,
    Om_String_View name,
    uint32_t *function_index
);
/*
 * Binds a verified external bytecode function to an `.omi` call binding.
 * The binding, its metadata, and its dynamic library must outlive the VM.
 * Bind before execution; a binding cannot be changed while the VM is active.
 */
Om_Status om_vm_bind_native_function(
    Om_VM *vm,
    uint32_t function_index,
    const Om_Native_Call_Binding *binding
);
/*
 * Binds supported declarations in an `.omi` module to matching external
 * bytecode functions. Callback Message mappings must already have been
 * checked against compiler semantics. The VM owns the call bindings; the
 * library and metadata module must outlive the VM.
 */
Om_Status om_vm_bind_native_module(
    Om_VM *vm,
    const Om_Native_Library *library,
    const Om_Omi_Module *module,
    const Om_VM_Native_Callback_Message *callback_messages,
    size_t callback_message_count,
    size_t maximum_cstring_bytes
);
/* Maps an `.omi` callback parameter to a declared bytecode Message schema. */
Om_Status om_vm_bind_native_callback_message(
    Om_VM *vm,
    uint32_t function_index,
    size_t parameter_index,
    Om_Symbol_Id message_symbol
);
/* Runtime failures are returned in result; the Om_Status covers API failures. */
Om_Status om_vm_execute(
    Om_VM *vm,
    uint32_t function_index,
    const Om_VM_Value *arguments,
    size_t argument_count,
    Om_VM_Execution_Result *result
);
/*
 * Thread-safe Native callback ingress. The caller must keep the VM alive and
 * invoke this only while om_vm_execute is active. Values are copied into the
 * target Actor's ordinary mailbox; only primitive values are accepted.
 */
Om_Status om_vm_native_callback_post(
    Om_VM *vm,
    uint64_t recipient_actor_id,
    Om_Symbol_Id message_symbol,
    const Om_VM_Value *fields,
    size_t field_count
);
const char *om_vm_runtime_error_name(Om_VM_Runtime_Error_Code code);

#endif
