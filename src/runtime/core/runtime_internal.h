#ifndef OMBASIC_RUNTIME_INTERNAL_H
#define OMBASIC_RUNTIME_INTERNAL_H

#include <setjmp.h>

#include "ombasic/runtime/abi.h"
#include "ombasic/runtime/embedding.h"

typedef struct Om_Runtime_Actor Om_Runtime_Actor;
typedef struct Om_Runtime_Actor_System Om_Runtime_Actor_System;

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

typedef union Om_Runtime_Object_Header {
    struct {
        const Om_Runtime_Type_Descriptor *type;
        struct Om_Execution_Context *owner;
        union Om_Runtime_Object_Header *next;
        size_t allocated_size;
        void *external_storage;
        bool marked;
    } fields;
    long double long_double_alignment;
    void *pointer_alignment;
    uint64_t integer_alignment;
} Om_Runtime_Object_Header;

typedef struct Om_Runtime_String_Body {
    size_t length;
    char bytes[];
} Om_Runtime_String_Body;

typedef struct Om_Runtime_Native_Pointer_Body {
    void *pointer;
    Om_Runtime_Native_Pointer_Release release;
    void *owner;
} Om_Runtime_Native_Pointer_Body;

typedef struct Om_Runtime_List_Body {
    size_t length;
    size_t capacity;
    Om_Runtime_Value *items;
} Om_Runtime_List_Body;

typedef struct Om_Runtime_Map_Entry {
    Om_Runtime_Value key;
    Om_Runtime_Value value;
} Om_Runtime_Map_Entry;

typedef struct Om_Runtime_Map_Body {
    size_t length;
    size_t capacity;
    Om_Runtime_Map_Entry *entries;
} Om_Runtime_Map_Body;

typedef struct Om_Runtime_Object_Copy_Pair {
    const Om_Runtime_Object_Header *source;
    void *destination;
} Om_Runtime_Object_Copy_Pair;

typedef struct Om_Runtime_Object_Copy_Context {
    Om_Execution_Context *source_context;
    Om_Execution_Context *destination_context;
    Om_Runtime_Object_Copy_Pair *pairs;
    size_t pair_count;
    size_t pair_capacity;
} Om_Runtime_Object_Copy_Context;

typedef Om_Status (*Om_Runtime_External_Root_Provider)(
    Om_Execution_Context *context,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);

extern const Om_Runtime_Type_Descriptor RUNTIME_STRING_TYPE;
extern const Om_Runtime_Type_Descriptor RUNTIME_NATIVE_POINTER_TYPE;
void finalize_native_pointer(Om_Runtime_Object_Header *object);
void destroy_actor_system(Om_Runtime_Actor_System *system);
Om_Status runtime_collect_internal(Om_Execution_Context *context);

bool runtime_is_power_of_two(size_t value);
bool type_is_managed(const Om_Runtime_Type_Descriptor *type);
bool result_descriptor_valid(const Om_Runtime_Type_Descriptor *type);
bool runtime_descriptor_compatible(
    const Om_Runtime_Type_Descriptor *expected,
    const Om_Runtime_Type_Descriptor *actual,
    size_t depth);
size_t stored_value_size(const Om_Runtime_Type_Descriptor *type);
bool descriptor_valid_for_record(const Om_Runtime_Type_Descriptor *type);
bool value_matches_type(const Om_Execution_Context *context,
                        const Om_Runtime_Type_Descriptor *type,
                        Om_Runtime_Value value);
void store_value(void *address,
                 const Om_Runtime_Type_Descriptor *type,
                 Om_Runtime_Value value);

Om_Runtime_Object_Header *find_object(const Om_Execution_Context *context,
                                      const void *pointer);
void *object_payload(Om_Runtime_Object_Header *object);
const void *object_payload_const(const Om_Runtime_Object_Header *object);
Om_Status allocate_object(Om_Execution_Context *context,
                          const Om_Runtime_Type_Descriptor *type,
                          size_t payload_size,
                          size_t external_size,
                          Om_Runtime_Object_Header **result);
Om_Status allocation_safepoint(Om_Execution_Context *context,
                               size_t expected_bytes);

/* Runtime-private operations. These are intentionally absent from installed
   headers and may change without incrementing the generated-code ABI. */
Om_Status om_runtime_actor_system_context_create(
    Om_Execution_Context *context,
    Om_Execution_Context **result);
Om_Status om_runtime_actor_spawn(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_spawn_assigned(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_self(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref *result);
Om_Status om_runtime_actor_send(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref target,
                                const void *message_object);
Om_Status om_runtime_actor_watch(Om_Execution_Context *context,
                                 Om_Runtime_Actor_Ref target);
Om_Status om_runtime_actor_watch_remote(
    Om_Execution_Context *context,
    uint64_t target_local_actor_id,
    uint64_t remote_watcher_ref);
Om_Status om_runtime_actor_post_remote_down(
    Om_Execution_Context *context,
    uint64_t watcher_local_actor_id,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason);
Om_Status om_runtime_distribution_attach(
    Om_Execution_Context *context,
    const Om_Runtime_Distribution_Hooks *hooks,
    void *state);
Om_Status om_runtime_distribution_detach(Om_Execution_Context *context,
                                         void *state);
Om_Status om_runtime_collect(Om_Execution_Context *context);
size_t om_runtime_live_object_count(const Om_Execution_Context *context);
size_t om_runtime_collection_count(const Om_Execution_Context *context);
Om_Status om_runtime_object_clone(Om_Execution_Context *source_context,
                                  Om_Execution_Context *destination_context,
                                  const void *source_object,
                                  void **result);
Om_Status om_runtime_object_type(
    const Om_Execution_Context *context,
    const void *object,
    const Om_Runtime_Type_Descriptor **result);
Om_Status om_runtime_record_set(Om_Execution_Context *context,
                                void *record,
                                size_t field_index,
                                Om_Runtime_Value value);
Om_Status om_runtime_list_append(Om_Execution_Context *context,
                                 void *list,
                                 Om_Runtime_Value value);
Om_Status om_runtime_map_set(Om_Execution_Context *context,
                             void *map,
                             Om_Runtime_Value key,
                             Om_Runtime_Value value);
Om_Status om_runtime_map_get(const Om_Execution_Context *context,
                             const void *map,
                             Om_Runtime_Value key,
                             Om_Runtime_Value *result);

struct Om_Runtime_Root_Frame {
    struct Om_Runtime_Root_Frame *previous;
    void **slots;
    size_t count;
};

typedef struct Om_Runtime_Fault_Boundary {
    jmp_buf environment;
    struct Om_Runtime_Fault_Boundary *previous;
    Om_Runtime_Root_Frame *root_base;
    volatile Om_Runtime_Fault_Code code;
    volatile uint32_t source_span_id;
} Om_Runtime_Fault_Boundary;

struct Om_Execution_Context {
    Om_Runtime_Object_Header *objects;
    Om_Runtime_Root_Frame *roots;
    Om_Runtime_Fault_Boundary *fault_boundary;
    const Om_Runtime_Source_Span *source_spans;
    size_t source_span_count;
    size_t allocation_bytes;
    size_t collection_threshold;
    size_t minimum_collection_threshold;
    size_t live_object_count;
    size_t collection_count;
    bool collecting;
    Om_Runtime_Actor_System *actor_system;
    Om_Runtime_Actor *current_actor;
    void *current_actor_message_object;
    bool owns_actor_system;
    bool is_actor_mailbox;
    Om_Runtime_External_Root_Provider external_root_provider;
    Om_Runtime_Actor_State_Initializer pending_actor_initializer;
    void *pending_actor_initializer_state;
    void *pending_actor_state_object;
    Om_Status pending_actor_initializer_status;
    Om_Runtime_Actor_Handler pending_actor_handler;
    void *pending_actor_state;
    const void *pending_actor_message;
    Om_Status pending_actor_status;
};

#endif
