#ifndef OMBASIC_RUNTIME_NATIVE_RUNTIME_H
#define OMBASIC_RUNTIME_NATIVE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/slice.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/foundation/system_actors.h"

typedef struct Om_Execution_Context Om_Execution_Context;
typedef struct Om_Runtime_Root_Frame Om_Runtime_Root_Frame;
typedef struct Om_Runtime_Type_Descriptor Om_Runtime_Type_Descriptor;
typedef struct Om_Runtime_Actor Om_Runtime_Actor;
typedef struct Om_Runtime_Actor_System Om_Runtime_Actor_System;
typedef void (*Om_Runtime_Native_Callback_Function)(void);
typedef void (*Om_Runtime_CString_Release)(void *pointer);
typedef void (*Om_Runtime_Native_Pointer_Release)(void *pointer);

typedef enum Om_Runtime_Fault_Code {
    OM_RUNTIME_FAULT_NONE = 0,
    OM_RUNTIME_FAULT_INTEGER_OVERFLOW = 1,
    OM_RUNTIME_FAULT_DIVISION_BY_ZERO,
    OM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS,
    OM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND,
    OM_RUNTIME_FAULT_ZERO_RANGE_STEP,
    OM_RUNTIME_FAULT_OPERATION_FAILED,
    OM_RUNTIME_FAULT_ACTOR_FAILED
} Om_Runtime_Fault_Code;

typedef struct Om_Runtime_Source_Span {
    const char *path;
    uint32_t line;
    uint32_t column;
} Om_Runtime_Source_Span;

typedef int (*Om_Runtime_Entry_Function)(Om_Execution_Context *context);

typedef struct Om_Runtime_Actor_Ref {
    uint64_t id;
} Om_Runtime_Actor_Ref;

typedef enum Om_Runtime_Type_Kind {
    OM_RUNTIME_TYPE_UNIT = 0,
    OM_RUNTIME_TYPE_NULL,
    OM_RUNTIME_TYPE_BOOL,
    OM_RUNTIME_TYPE_INT,
    OM_RUNTIME_TYPE_FLOAT,
    OM_RUNTIME_TYPE_STRING,
    OM_RUNTIME_TYPE_RECORD,
    OM_RUNTIME_TYPE_MESSAGE,
    OM_RUNTIME_TYPE_LIST,
    OM_RUNTIME_TYPE_MAP,
    OM_RUNTIME_TYPE_RESULT,
    OM_RUNTIME_TYPE_NATIVE_POINTER,
    OM_RUNTIME_TYPE_ACTOR_REF
} Om_Runtime_Type_Kind;

typedef struct Om_Runtime_Field_Descriptor {
    size_t offset;
    const Om_Runtime_Type_Descriptor *type;
} Om_Runtime_Field_Descriptor;

typedef Om_Status (*Om_Runtime_Trace_Visitor)(void *managed_value,
                                             void *visitor_context);
typedef Om_Status (*Om_Runtime_Trace_Function)(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);
typedef Om_Status (*Om_Runtime_Copy_Function)(Om_Execution_Context *context,
                                             void *destination,
                                             const void *source);
typedef Om_Status (*Om_Runtime_Equal_Function)(Om_Execution_Context *context,
                                              const void *left,
                                              const void *right,
                                              bool *equal);
typedef Om_Status (*Om_Runtime_Hash_Function)(Om_Execution_Context *context,
                                             const void *value,
                                             uint64_t *hash);
typedef Om_Status (*Om_Runtime_Byte_Sink)(const uint8_t *bytes,
                                         size_t length,
                                         void *sink_context);
typedef Om_Status (*Om_Runtime_Serialize_Function)(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Byte_Sink sink,
    void *sink_context);

struct Om_Runtime_Type_Descriptor {
    uint64_t fingerprint;
    Om_Runtime_Type_Kind kind;
    /* Record/Message payload layout, or scalar value layout. */
    size_t size;
    size_t alignment;
    const Om_Runtime_Field_Descriptor *fields;
    size_t field_count;
    const Om_Runtime_Type_Descriptor *element_type;
    const Om_Runtime_Type_Descriptor *key_type;
    const Om_Runtime_Type_Descriptor *value_type;
    /* Trace callbacks visit managed pointers contained in value bytes. For a
       managed object the collector passes its payload; for a field it passes
       the field's stored value. They must not allocate or mutate the heap. */
    Om_Runtime_Trace_Function trace;
    Om_Runtime_Copy_Function copy;
    Om_Runtime_Equal_Function equal;
    Om_Runtime_Hash_Function hash;
    Om_Runtime_Serialize_Function serialize;
    Om_System_Message_Id system_message_id;
};

typedef Om_Status (*Om_Runtime_Actor_State_Initializer)(
    Om_Execution_Context *context,
    void *state_payload);
typedef Om_Status (*Om_Runtime_Actor_Handler)(
    Om_Execution_Context *context,
    void *state_payload,
    const void *message_payload);

typedef struct Om_Runtime_Actor_Handler_Descriptor {
    const Om_Runtime_Type_Descriptor *message_type;
    Om_Runtime_Actor_Handler handler;
} Om_Runtime_Actor_Handler_Descriptor;

typedef struct Om_Runtime_Actor_Descriptor {
    uint64_t fingerprint;
    const char *canonical_name;
    const Om_Runtime_Type_Descriptor *state_type;
    Om_Runtime_Actor_State_Initializer initialize_state;
    const Om_Runtime_Actor_Handler_Descriptor *handlers;
    size_t handler_count;
} Om_Runtime_Actor_Descriptor;

typedef enum Om_Runtime_Actor_Down_Reason {
    OM_RUNTIME_ACTOR_DOWN_NORMAL = 0,
    OM_RUNTIME_ACTOR_DOWN_FAILED = 1,
    OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE = 2
} Om_Runtime_Actor_Down_Reason;

typedef struct Om_Runtime_Program_Descriptor {
    uint64_t fingerprint;
    uint64_t interface_fingerprint;
    const Om_Runtime_Type_Descriptor *const *types;
    size_t type_count;
    const Om_Runtime_Actor_Descriptor *const *actors;
    size_t actor_count;
    const Om_Runtime_Type_Descriptor *actor_down_message_type;
    size_t actor_down_actor_id_offset;
    size_t actor_down_reason_offset;
    const Om_Runtime_Source_Span *source_spans;
    size_t source_span_count;
    Om_Runtime_Entry_Function entry_action;
    size_t collection_threshold;
} Om_Runtime_Program_Descriptor;

typedef struct Om_Runtime_Distribution_Hooks {
    Om_Status (*spawn)(void *state,
                      Om_Execution_Context *context,
                      const Om_Runtime_Actor_Descriptor *descriptor,
                      uint64_t node_ref,
                      Om_Runtime_Actor_Ref *result);
    Om_Status (*send)(void *state,
                      Om_Execution_Context *context,
                      uint64_t remote_actor_ref,
                      const void *message_object);
    Om_Status (*watch)(void *state,
                       Om_Execution_Context *context,
                       uint64_t remote_actor_ref);
    Om_Status (*notify_watcher)(void *state,
                                Om_Execution_Context *context,
                                uint64_t remote_watcher_ref,
                                uint64_t terminated_actor_id,
                                Om_Runtime_Actor_Down_Reason reason);
} Om_Runtime_Distribution_Hooks;

typedef union Om_Runtime_Value {
    bool boolean;
    int64_t integer;
    double floating;
    void *managed;
} Om_Runtime_Value;

/* Callback values are normalized by compiler-generated C ABI trampolines to
   64-bit scalar bit patterns. Integers preserve their two's-complement bits,
   booleans are zero or one, and floating-point values use binary64 bits. */
Om_Status om_runtime_native_callback_register_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *const *candidate_sets,
    const uint64_t *candidate_counts,
    const Om_Runtime_Type_Descriptor *const *message_types,
    Om_Runtime_Native_Callback_Function *registered_functions
);
Om_Status om_runtime_native_callback_unregister_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *registered_functions
);
Om_Status om_runtime_native_callback_post(
    Om_Runtime_Native_Callback_Function callback,
    const uint64_t *value_bits,
    uint64_t value_count
);

/* A zero threshold selects the runtime default collection threshold. */
Om_Execution_Context *om_runtime_context_create(size_t collection_threshold);
void om_runtime_context_destroy(Om_Execution_Context *context);

/* Creates a thread-confined context that can post messages to the actor
   system started on `context`. The actor system must outlive the new context. */
Om_Status om_runtime_actor_system_context_create(
    Om_Execution_Context *context,
    Om_Execution_Context **result
);

/* Source span data is borrowed and must outlive the execution context. */
Om_Status om_runtime_set_source_spans(
    Om_Execution_Context *context,
    const Om_Runtime_Source_Span *spans,
    size_t span_count
);

/* Runs an entry through a recoverable runtime-fault boundary. A fault returns
   a non-zero process result after restoring the root chain to its saved base. */
int om_runtime_run(Om_Execution_Context *context,
                   Om_Runtime_Entry_Function entry);

/* A Program descriptor is borrowed until context destruction. Each Actor
   processes one native handler at a time on the bounded worker pool. */
Om_Status om_runtime_actor_system_start(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    size_t worker_count);
Om_Status om_runtime_actor_system_wait(Om_Execution_Context *context);
Om_Status om_runtime_actor_spawn(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    Om_Runtime_Actor_Ref *result);
/* Remote IDs use the high bit; local actor IDs are allocated below it. */
Om_Status om_runtime_actor_spawn_assigned(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_spawn_at(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_spawn_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t *result_id);
Om_Status om_runtime_actor_self(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_self_id(Om_Execution_Context *context,
                                   uint64_t *result_id);
Om_Status om_runtime_actor_state_object(Om_Execution_Context *context,
                                        void **result_object);
Om_Status om_runtime_actor_message_object(Om_Execution_Context *context,
                                          void **result_object);
Om_Status om_runtime_actor_send(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref target,
                                const void *message_object);
Om_Status om_runtime_actor_send_id(Om_Execution_Context *context,
                                   uint64_t target_id,
                                   const void *message_object);
Om_Status om_runtime_actor_watch(Om_Execution_Context *context,
                                 Om_Runtime_Actor_Ref target);
Om_Status om_runtime_actor_watch_id(Om_Execution_Context *context,
                                    uint64_t target_id);
Om_Status om_runtime_actor_watch_remote(
    Om_Execution_Context *context,
    uint64_t target_local_actor_id,
    uint64_t remote_watcher_ref
);
Om_Status om_runtime_actor_post_remote_down(
    Om_Execution_Context *context,
    uint64_t watcher_local_actor_id,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason
);
Om_Status om_runtime_actor_stop(Om_Execution_Context *context);

/* A Native distribution adapter attaches after actor_system_start. The
   adapter state and hooks remain borrowed until the actor system stops. */
Om_Status om_runtime_distribution_attach(
    Om_Execution_Context *context,
    const Om_Runtime_Distribution_Hooks *hooks,
    void *state
);
Om_Status om_runtime_distribution_detach(
    Om_Execution_Context *context,
    void *state
);

/* Runtime faults are non-local exits to the nearest om_runtime_run boundary.
   A fault without a boundary is printed and terminates the process. */
_Noreturn void om_runtime_fault(Om_Execution_Context *context,
                                Om_Runtime_Fault_Code code,
                                uint32_t source_span_id);

/* Root slots are native stack addresses whose contents are managed pointers.
   Frames must be popped in last-in-first-out order. */
Om_Runtime_Root_Frame *om_runtime_roots_push(Om_Execution_Context *context,
                                             void **root_slots,
                                             size_t root_count);
Om_Status om_runtime_roots_pop(Om_Execution_Context *context,
                               Om_Runtime_Root_Frame *frame);

/* Collection is non-moving. Unrooted objects can be reclaimed at any call to
   this function or at an allocation safepoint. */
Om_Status om_runtime_collect(Om_Execution_Context *context);
size_t om_runtime_live_object_count(const Om_Execution_Context *context);
size_t om_runtime_collection_count(const Om_Execution_Context *context);
Om_Status om_runtime_object_clone(Om_Execution_Context *source_context,
                                  Om_Execution_Context *destination_context,
                                  const void *source_object,
                                  void **result);

/* CPointer is a managed opaque resource wrapper. Borrow exposes its raw
   address for one call; take transfers an owned address and disarms the
   wrapper. The owner of a borrowed result is traced until that wrapper dies. */
Om_Status om_runtime_native_pointer_create(
    Om_Execution_Context *context,
    void *pointer,
    Om_Runtime_Native_Pointer_Release release,
    void *owner,
    void **result
);
Om_Status om_runtime_native_pointer_borrow(
    const Om_Execution_Context *context,
    const void *native_pointer,
    void **result
);
Om_Status om_runtime_native_pointer_take(
    Om_Execution_Context *context,
    void *native_pointer,
    void **result
);

/* Returns the descriptor for an object owned by this context. */
Om_Status om_runtime_object_type(
    const Om_Execution_Context *context,
    const void *object,
    const Om_Runtime_Type_Descriptor **result
);

Om_Status om_runtime_string_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   Om_Const_Byte_Slice utf8,
                                   void **result);
Om_Status om_runtime_string_create_utf8(Om_Execution_Context *context,
                                        const Om_Runtime_Type_Descriptor *type,
                                        const uint8_t *bytes,
                                        size_t length,
                                        void **result);
Om_Status om_runtime_string_create_utf8_default(Om_Execution_Context *context,
                                                const uint8_t *bytes,
                                                size_t length,
                                                void **result);
Om_Status om_runtime_string_concat(Om_Execution_Context *context,
                                   const void *left,
                                   const void *right,
                                   void **result);
Om_Status om_runtime_string_format_value(Om_Execution_Context *context,
                                         Om_Runtime_Type_Kind kind,
                                         const Om_Runtime_Value *value,
                                         void **result);
Om_Status om_runtime_string_view(const Om_Execution_Context *context,
                                 const void *string,
                                 Om_String_View *result);
/* Returns a borrowed UTF-8 view unless its bytes contain an embedded NUL. */
Om_Status om_runtime_string_cstring_view(const Om_Execution_Context *context,
                                         const void *string,
                                         const uint8_t **bytes,
                                         size_t *length);
Om_Status om_runtime_string_create_from_cstring(
    Om_Execution_Context *context,
    const char *cstring,
    void **result
);
/* Takes ownership and releases after copying or after an output validation error. */
Om_Status om_runtime_string_create_from_owned_cstring(
    Om_Execution_Context *context,
    char *cstring,
    Om_Runtime_CString_Release release,
    void **result
);
Om_Status om_runtime_print(Om_Execution_Context *context,
                           const void *string);

Om_Status om_runtime_record_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   void **result);
Om_Status om_runtime_record_set(Om_Execution_Context *context,
                                void *record,
                                size_t field_index,
                                Om_Runtime_Value value);
Om_Status om_runtime_record_set_value(Om_Execution_Context *context,
                                      void *record,
                                      size_t field_index,
                                      const void *value);
Om_Status om_runtime_record_get(const Om_Execution_Context *context,
                                const void *record,
                                size_t field_index,
                                void *result);

Om_Status om_runtime_list_create(Om_Execution_Context *context,
                                 const Om_Runtime_Type_Descriptor *type,
                                 void **result);
Om_Status om_runtime_list_append(Om_Execution_Context *context,
                                 void *list,
                                 Om_Runtime_Value value);
Om_Status om_runtime_list_append_value(Om_Execution_Context *context,
                                      void *list,
                                      const Om_Runtime_Value *value);
Om_Status om_runtime_list_get(const Om_Execution_Context *context,
                              const void *list,
                              size_t index,
                              Om_Runtime_Value *result);
Om_Status om_runtime_list_length(const Om_Execution_Context *context,
                                 const void *list,
                                 size_t *result);

Om_Status om_runtime_map_create(Om_Execution_Context *context,
                                const Om_Runtime_Type_Descriptor *type,
                                void **result);
Om_Status om_runtime_map_set(Om_Execution_Context *context,
                             void *map,
                             Om_Runtime_Value key,
                             Om_Runtime_Value value);
Om_Status om_runtime_map_set_values(Om_Execution_Context *context,
                                    void *map,
                                    const Om_Runtime_Value *key,
                                    const Om_Runtime_Value *value);
Om_Status om_runtime_map_get(const Om_Execution_Context *context,
                             const void *map,
                             Om_Runtime_Value key,
                             Om_Runtime_Value *result);
Om_Status om_runtime_map_get_value(const Om_Execution_Context *context,
                                   const void *map,
                                   const Om_Runtime_Value *key,
                                   Om_Runtime_Value *result);
Om_Status om_runtime_map_key_at(const Om_Execution_Context *context,
                                const void *map,
                                size_t index,
                                Om_Runtime_Value *result);
Om_Status om_runtime_map_length(const Om_Execution_Context *context,
                                const void *map,
                                size_t *result);

#endif
