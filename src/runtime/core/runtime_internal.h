#ifndef OMBASIC_RUNTIME_INTERNAL_H
#define OMBASIC_RUNTIME_INTERNAL_H

#include <setjmp.h>

#include "ombasic/runtime/native_runtime.h"

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
