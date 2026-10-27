#ifndef OMBASIC_RUNTIME_VM_INTERNAL_H
#define OMBASIC_RUNTIME_VM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "ombasic/runtime/vm.h"

typedef struct Om_VM_Distribution_Peer Om_VM_Distribution_Peer;

typedef struct Om_VM_Heap Om_VM_Heap;
typedef struct Om_VM_State Om_VM_State;
typedef struct Om_Blocking_Pool Om_Blocking_Pool;
typedef struct Om_Timer_Service Om_Timer_Service;

struct Om_VM_Object {
    Om_VM_Object_Kind kind;
    Om_VM_Heap *owner;
    struct Om_VM_Object *next;
    struct Om_VM_Object *mark_next;
    size_t allocation_size;
    bool marked;
};

typedef struct Om_VM_String_Object {
    Om_VM_Object base;
    size_t length;
    char bytes[];
} Om_VM_String_Object;

typedef struct Om_VM_List_Object {
    Om_VM_Object base;
    size_t length;
    Om_VM_Value items[];
} Om_VM_List_Object;

typedef struct Om_VM_Map_Entry {
    Om_VM_Value key;
    Om_VM_Value value;
} Om_VM_Map_Entry;

typedef struct Om_VM_Map_Object {
    Om_VM_Object base;
    size_t length;
    size_t capacity;
    Om_VM_Map_Entry entries[];
} Om_VM_Map_Object;

typedef struct Om_VM_Record_Object {
    Om_VM_Object base;
    uint32_t schema_index;
    size_t field_count;
    Om_VM_Value fields[];
} Om_VM_Record_Object;

typedef struct Om_VM_Result_Object {
    Om_VM_Object base;
    bool is_ok;
    Om_VM_Value payload;
} Om_VM_Result_Object;

typedef struct Om_VM_Function_Object {
    Om_VM_Object base;
    uint32_t function_index;
} Om_VM_Function_Object;

typedef enum Om_VM_Iterator_Kind {
    OM_VM_ITERATOR_LIST = 0,
    OM_VM_ITERATOR_MAP
} Om_VM_Iterator_Kind;

typedef struct Om_VM_Iterator_Object {
    Om_VM_Object base;
    Om_VM_Iterator_Kind kind;
    Om_VM_Object *collection;
    size_t next_index;
} Om_VM_Iterator_Object;

struct Om_VM_Heap {
    Om_VM_State *vm;
    Om_VM_State *execution_state;
    uint64_t actor_id;
    Om_VM_Object *objects;
    size_t bytes_allocated;
    size_t object_count;
    size_t max_bytes;
    size_t gc_threshold;
    bool gc_pending;
    Om_Status last_status;
};

typedef struct Om_VM_Call_Frame {
    uint32_t function_index;
    uint32_t pc;
    uint32_t return_pc;
    size_t stack_base;
    Om_VM_Value *locals;
    uint64_t actor_id;
    uint8_t return_mode;
} Om_VM_Call_Frame;

typedef enum Om_VM_Return_Mode {
    OM_VM_RETURN_NORMAL = 0,
    OM_VM_RETURN_ROOT,
    OM_VM_RETURN_SPAWN_INITIALIZER,
    OM_VM_RETURN_SCHEDULED_HANDLER,
    OM_VM_RETURN_REMOTE_INITIALIZER
} Om_VM_Return_Mode;

typedef struct Om_VM_Message_Node {
    Om_VM_Heap transit_heap;
    Om_VM_Value message;
    struct Om_VM_Message_Node *next;
} Om_VM_Message_Node;

typedef struct Om_VM_Remote_Actor_Ref {
    uint64_t value;
    uint64_t peer_handle;
    uint64_t remote_actor_id;
} Om_VM_Remote_Actor_Ref;

typedef struct Om_VM_Remote_Watch {
    uint64_t peer_handle;
    uint64_t remote_actor_id;
    uint64_t watcher_actor_id;
    bool notified;
} Om_VM_Remote_Watch;

typedef struct Om_VM_Remote_Watcher {
    uint64_t peer_handle;
    uint64_t watcher_actor_id;
} Om_VM_Remote_Watcher;

typedef struct Om_VM_Remote_Spawn_Request {
    uint64_t request_id;
    uint64_t spawned_actor_id;
    uint64_t requester_actor_id;
    Om_Status status;
    bool active;
    bool completed;
} Om_VM_Remote_Spawn_Request;

struct Om_VM_Distribution_Peer {
    Om_VM_State *vm;
    uint64_t handle;
    Om_Distribution_Node_Handshake remote_handshake;
    Om_Distribution_Connection connection;
    _Atomic bool connected;
    void *send_mutex;
    void *thread_state;
    bool thread_started;
};

typedef struct Om_VM_Actor_State {
    uint64_t id;
    uint32_t bytecode_actor_index;
    Om_VM_Heap *heap;
    Om_VM_Value *saved_stack;
    size_t saved_stack_length;
    size_t saved_stack_capacity;
    Om_VM_Call_Frame *saved_frames;
    size_t saved_frame_count;
    size_t saved_frame_capacity;
    Om_VM_Value saved_handler_result;
    Om_VM_Value *state_values;
    size_t state_count;
    Om_VM_Message_Node *mailbox_head;
    Om_VM_Message_Node *mailbox_tail;
    size_t mailbox_count;
    uint64_t *watchers;
    size_t watcher_count;
    size_t watcher_capacity;
    Om_VM_Remote_Watcher *remote_watchers;
    size_t remote_watcher_count;
    size_t remote_watcher_capacity;
    uint32_t down_reason;
    bool initialization_pending;
    bool stopped;
    bool failed;
    bool scheduled;
    bool running;
    bool context_saved;
    bool waiting_native_call;
    bool native_call_completion_ready;
    Om_Status native_call_status;
    const Om_Native_Call_Binding *native_call_binding;
    Om_Native_Call_Result native_call_result;
    uint32_t native_call_function;
    uint32_t native_call_instruction;
} Om_VM_Actor_State;

typedef struct Om_VM_Native_Callback_Target {
    uint32_t function_index;
    size_t parameter_index;
    Om_Symbol_Id message_symbol;
} Om_VM_Native_Callback_Target;

typedef struct Om_VM_Native_Binding_Group {
    Om_Native_Call_Binding *bindings;
    size_t binding_count;
} Om_VM_Native_Binding_Group;

struct Om_VM_State {
    const Om_Bytecode_Image *image;
    Om_Distribution_Node_Handshake distribution_handshake;
    Om_VM_Distribution_Peer **distribution_peers;
    size_t distribution_peer_count;
    size_t distribution_peer_capacity;
    Om_VM_Remote_Actor_Ref *remote_actor_refs;
    size_t remote_actor_ref_count;
    size_t remote_actor_ref_capacity;
    Om_VM_Remote_Watch *remote_watches;
    size_t remote_watch_count;
    size_t remote_watch_capacity;
    Om_VM_Remote_Spawn_Request *remote_spawn_requests;
    size_t remote_spawn_request_count;
    size_t remote_spawn_request_capacity;
    uint64_t next_distribution_request_id;
    uint64_t next_remote_actor_ref;
    const Om_Native_Call_Binding **native_bindings;
    size_t native_binding_count;
    Om_VM_Native_Binding_Group **native_binding_groups;
    size_t native_binding_group_count;
    Om_VM_Native_Callback_Target *native_callback_targets;
    size_t native_callback_target_count;
    size_t native_callback_target_capacity;
    Om_VM_Limits limits;
    Om_VM_State *shared;
    Om_VM_Heap heap;
    _Atomic size_t heap_bytes_allocated;
    _Atomic uint64_t gc_collection_count;
    Om_VM_Value *stack;
    size_t stack_length;
    size_t stack_capacity;
    Om_VM_Call_Frame *frames;
    size_t frame_count;
    size_t frame_capacity;
    _Atomic uint64_t instruction_count;
    Om_VM_Actor_State *actors;
    _Atomic size_t actor_count;
    size_t actor_capacity;
    uint64_t *ready_queue;
    size_t ready_head;
    size_t ready_length;
    size_t ready_capacity;
    size_t mailbox_message_count;
    size_t running_actor_count;
    size_t max_running_actor_count;
    uint32_t actor_down_schema_index;
    uint64_t handler_actor_id;
    Om_VM_Value handler_result;
    bool handler_completed;
    bool stop_unwind_requested;
    uint64_t stop_actor_id;
    bool suspend_current_actor_after_instruction;
    _Atomic uint64_t run_actors_spawned;
    _Atomic uint64_t run_messages_processed;
    _Atomic uint64_t run_actors_stopped;
    _Atomic bool scheduler_done;
    _Atomic bool scheduler_failed;
    _Atomic bool execution_active;
    size_t pending_external_tasks;
    Om_VM_Runtime_Error_Code async_error_code;
    uint32_t async_error_function;
    uint32_t async_error_instruction;
    Om_Blocking_Pool *blocking_pool;
    Om_Timer_Service *timer_service;
    void *scheduler_mutex;
};

void om_vm_scheduler_lock(Om_VM_State *state);
void om_vm_scheduler_unlock(Om_VM_State *state);
void om_vm_scheduler_wait(Om_VM_State *state);
void om_vm_scheduler_broadcast(Om_VM_State *state);
Om_VM_Message_Node *om_vm_distribution_message_node_create(
    Om_VM_State *state
);
void om_vm_distribution_message_node_destroy(Om_VM_Message_Node *node);
Om_Status om_vm_distribution_enqueue_message(
    Om_VM_State *state,
    uint64_t actor_id,
    Om_VM_Message_Node *node
);
void om_vm_distribution_shutdown(Om_VM_State *state);
Om_Status om_vm_distribution_send_message(
    Om_VM_State *state,
    uint64_t remote_ref_value,
    uint64_t sender_actor_id,
    Om_VM_Value message
);
Om_Status om_vm_distribution_watch_remote(
    Om_VM_State *state,
    uint64_t remote_ref_value,
    uint64_t watcher_actor_id
);
void om_vm_distribution_actor_stopped(
    Om_VM_State *state,
    uint64_t actor_id,
    uint32_t down_reason
);
Om_Status om_vm_distribution_spawn_actor(
    Om_VM_State *state,
    uint32_t actor_symbol,
    uint64_t *actor_id
);
Om_Status om_vm_distribution_spawn_remote(
    Om_VM_State *state,
    uint64_t peer_handle,
    uint32_t actor_symbol,
    uint64_t requester_actor_id,
    uint64_t *remote_actor_id
);
Om_Status om_vm_distribution_make_remote_actor_ref(
    Om_VM_State *state,
    uint64_t peer_handle,
    uint64_t remote_actor_id,
    Om_VM_Value *actor_ref
);

Om_VM_Object *om_vm_heap_allocate(
    Om_VM_Heap *heap,
    size_t size,
    Om_VM_Object_Kind kind
);
void om_vm_heap_destroy(Om_VM_Heap *heap);
Om_Status om_vm_heap_collect(Om_VM_Heap *heap);
Om_Status om_vm_heap_make_string(
    Om_VM_Heap *heap,
    Om_String_View view,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_list(
    Om_VM_Heap *heap,
    const Om_VM_Value *items,
    size_t item_count,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_map(
    Om_VM_Heap *heap,
    const Om_VM_Map_Entry *entries,
    size_t entry_count,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_record(
    Om_VM_Heap *heap,
    Om_VM_Object_Kind kind,
    uint32_t schema_index,
    const Om_VM_Value *fields,
    size_t field_count,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_result(
    Om_VM_Heap *heap,
    bool is_ok,
    Om_VM_Value payload,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_function(
    Om_VM_Heap *heap,
    uint32_t function_index,
    Om_VM_Value *value
);
Om_Status om_vm_heap_make_iterator(
    Om_VM_Heap *heap,
    Om_VM_Iterator_Kind kind,
    Om_VM_Object *collection,
    Om_VM_Value *value
);

#endif
