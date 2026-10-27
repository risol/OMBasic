#include "ombasic/runtime/vm.h"

#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "../vm_internal.h"
#include "../system_actor/blocking_pool.h"
#include "../system_actor/timer_service.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "ombasic/ffi/abi_type.h"
#include "ombasic/ffi/c_string.h"

static Om_Status reserve_external_task(Om_VM_State *shared,
                                       bool timer,
                                       void **service_out);
static void finish_external_task(Om_VM_State *shared);
static Om_Status run_native_call_task(void *opaque);
static void complete_native_call_task(void *opaque, Om_Status status);
static void destroy_native_call_task(void *opaque);
static Om_VM_Actor_State *find_actor(Om_VM_State *state, uint64_t actor_id);

#define OM_VM_DEFAULT_MAX_STACK ((size_t)65536)
#define OM_VM_DEFAULT_MAX_FRAMES ((size_t)1024)
#define OM_VM_DEFAULT_MAX_HEAP ((size_t)64 * (size_t)1024 * (size_t)1024)
#define OM_VM_DEFAULT_MAX_INSTRUCTIONS UINT64_C(10000000)
#define OM_VM_DEFAULT_MAX_ACTORS ((size_t)4096)
#define OM_VM_DEFAULT_MAX_MAILBOX_MESSAGES ((size_t)65536)
#define OM_VM_MAX_FORMAT_DEPTH ((size_t)128)
#define OM_VM_DEFAULT_GC_THRESHOLD ((size_t)64 * (size_t)1024)
#define OM_VM_DEFAULT_WORKER_COUNT ((size_t)4)
#define OM_VM_DEFAULT_INSTRUCTION_QUANTUM ((size_t)256)
#define OM_VM_MAX_WORKER_COUNT ((size_t)64)

typedef struct Om_VM_Scheduler_Mutex {
#if defined(_WIN32)
    CRITICAL_SECTION native;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t native;
    pthread_cond_t condition;
#endif
} Om_VM_Scheduler_Mutex;

static Om_VM_State *shared_vm(Om_VM_State *state)
{
    return state->shared == NULL ? state : state->shared;
}

static const Om_VM_State *shared_vm_const(const Om_VM_State *state)
{
    return state->shared == NULL ? state : state->shared;
}

static bool scheduler_mutex_init(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex = om_alloc(sizeof(*mutex));
    if (mutex == NULL) {
        return false;
    }
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&mutex->native, 0, 0)) {
        om_free(mutex);
        return false;
    }
    InitializeConditionVariable(&mutex->condition);
#else
    if (pthread_mutex_init(&mutex->native, NULL) != 0) {
        om_free(mutex);
        return false;
    }
    if (pthread_cond_init(&mutex->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&mutex->native);
        om_free(mutex);
        return false;
    }
#endif
    state->scheduler_mutex = mutex;
    return true;
}

static void scheduler_mutex_lock(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex =
        (Om_VM_Scheduler_Mutex *)shared_vm(state)->scheduler_mutex;
#if defined(_WIN32)
    EnterCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_lock(&mutex->native);
#endif
}

static void scheduler_mutex_unlock(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex =
        (Om_VM_Scheduler_Mutex *)shared_vm(state)->scheduler_mutex;
#if defined(_WIN32)
    LeaveCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_unlock(&mutex->native);
#endif
}

void om_vm_scheduler_lock(Om_VM_State *state)
{
    scheduler_mutex_lock(state);
}

void om_vm_scheduler_unlock(Om_VM_State *state)
{
    scheduler_mutex_unlock(state);
}

static void scheduler_condition_wait(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex =
        (Om_VM_Scheduler_Mutex *)shared_vm(state)->scheduler_mutex;
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&mutex->condition, &mutex->native,
                                   INFINITE);
#else
    (void)pthread_cond_wait(&mutex->condition, &mutex->native);
#endif
}

static void scheduler_condition_broadcast(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex =
        (Om_VM_Scheduler_Mutex *)shared_vm(state)->scheduler_mutex;
#if defined(_WIN32)
    WakeAllConditionVariable(&mutex->condition);
#else
    (void)pthread_cond_broadcast(&mutex->condition);
#endif
}

void om_vm_scheduler_wait(Om_VM_State *state)
{
    scheduler_condition_wait(state);
}

void om_vm_scheduler_broadcast(Om_VM_State *state)
{
    scheduler_condition_broadcast(state);
}

static void scheduler_mutex_destroy(Om_VM_State *state)
{
    Om_VM_Scheduler_Mutex *mutex =
        (Om_VM_Scheduler_Mutex *)state->scheduler_mutex;
    if (mutex == NULL) {
        return;
    }
#if defined(_WIN32)
    DeleteCriticalSection(&mutex->native);
#else
    (void)pthread_cond_destroy(&mutex->condition);
    (void)pthread_mutex_destroy(&mutex->native);
#endif
    om_free(mutex);
    state->scheduler_mutex = NULL;
}

typedef struct Om_VM_Byte_Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Om_VM_Byte_Buffer;

static bool object_belongs_to_vm(const Om_VM_State *state,
                                 const Om_VM_Object *object)
{
    return object != NULL && object->owner != NULL
        && object->owner->vm == shared_vm_const(state);
}

static Om_VM_Heap *active_heap(Om_VM_State *state)
{
    Om_VM_State *shared = shared_vm(state);
    const uint64_t actor_id = state->frame_count == 0
        ? state->handler_actor_id
        : state->frames[state->frame_count - 1].actor_id;
    if (actor_id == 0
        || actor_id > atomic_load_explicit(&shared->actor_count,
                                           memory_order_acquire)) {
        return &shared->heap;
    }
    Om_VM_Actor_State *actor = &shared->actors[(size_t)(actor_id - 1)];
    return actor->id == actor_id ? actor->heap : NULL;
}

static bool value_is_valid_for_vm(const Om_VM_State *state,
                                  Om_VM_Value value)
{
    if (value.tag < OM_VM_VALUE_NULL || value.tag > OM_VM_VALUE_OBJECT) {
        return false;
    }
    return value.tag != OM_VM_VALUE_OBJECT
        || object_belongs_to_vm(state, value.as.object);
}

static void set_runtime_error(Om_VM_State *state,
                              Om_VM_Execution_Result *result,
                              Om_VM_Runtime_Error_Code code,
                              uint32_t function_index,
                              uint32_t instruction_index)
{
    Om_VM_State *shared = shared_vm(state);
    result->succeeded = false;
    result->value = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    result->error = (Om_VM_Runtime_Error){
        .code = code,
        .function_index = function_index,
        .instruction_index = instruction_index
    };
    Om_Source_Span span = {0};
    if (om_bytecode_source_map_lookup(state->image, instruction_index, &span)
        == OM_STATUS_OK) {
        result->error.source_span = span;
        result->error.has_source_span = true;
    }
    atomic_store_explicit(&shared->scheduler_failed, true,
                          memory_order_release);
    scheduler_condition_broadcast(shared);
}

static Om_VM_Runtime_Error_Code error_for_allocation_status(Om_Status status)
{
    if (status == OM_STATUS_OUT_OF_MEMORY) {
        return OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OUT_OF_RANGE || status == OM_STATUS_OVERFLOW) {
        return OM_VM_RUNTIME_ERROR_HEAP_LIMIT;
    }
    return OM_VM_RUNTIME_ERROR_INVALID_BYTECODE;
}

static bool reserve_stack(Om_VM_State *state, size_t required,
                          Om_VM_Execution_Result *result,
                          uint32_t function_index, uint32_t pc)
{
    if (required > state->limits.max_stack_values) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_STACK_LIMIT,
                          function_index, pc);
        return false;
    }
    if (required <= state->stack_capacity) {
        return true;
    }
    size_t capacity = state->stack_capacity == 0
        ? (state->limits.max_stack_values < 16
               ? state->limits.max_stack_values : 16)
        : state->stack_capacity;
    while (capacity < required) {
        if (capacity > state->limits.max_stack_values / 2) {
            capacity = state->limits.max_stack_values;
        } else {
            capacity *= 2;
        }
        if (capacity < required && capacity == state->limits.max_stack_values) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_STACK_LIMIT,
                              function_index, pc);
            return false;
        }
    }
    Om_VM_Value *stack = om_realloc_array(
        state->stack, capacity, sizeof(*state->stack));
    if (stack == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        return false;
    }
    state->stack = stack;
    state->stack_capacity = capacity;
    return true;
}

static bool push_value(Om_VM_State *state, Om_VM_Value value,
                       Om_VM_Execution_Result *result,
                       uint32_t function_index, uint32_t pc)
{
    if (!value_is_valid_for_vm(state, value)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    if (state->stack_length == SIZE_MAX
        || !reserve_stack(state, state->stack_length + 1, result,
                          function_index, pc)) {
        return false;
    }
    state->stack[state->stack_length++] = value;
    return true;
}

static bool pop_value(Om_VM_State *state, Om_VM_Value *value,
                      Om_VM_Execution_Result *result,
                      uint32_t function_index, uint32_t pc)
{
    if (state->stack_length == 0 || value == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    *value = state->stack[--state->stack_length];
    return true;
}

static Om_VM_Value peek_value(const Om_VM_State *state, size_t from_top)
{
    return state->stack[state->stack_length - from_top - 1];
}

static Om_Status grow_frame_array(Om_VM_State *state)
{
    size_t capacity = state->frame_capacity == 0 ? 8 : state->frame_capacity;
    if (capacity > state->limits.max_call_frames / 2) {
        capacity = state->limits.max_call_frames;
    } else {
        capacity *= 2;
    }
    if (capacity <= state->frame_capacity) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_VM_Call_Frame *frames = om_realloc_array(
        state->frames, capacity, sizeof(*state->frames));
    if (frames == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    state->frames = frames;
    state->frame_capacity = capacity;
    return OM_STATUS_OK;
}

static bool push_frame(Om_VM_State *state, uint32_t function_index,
                       const Om_VM_Value *arguments, size_t argument_count,
                       size_t stack_base, uint32_t return_pc,
                       uint64_t actor_id, Om_VM_Return_Mode return_mode,
                       Om_VM_Execution_Result *result,
                       uint32_t caller_function, uint32_t caller_pc)
{
    if (function_index >= state->image->function_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_FUNCTION,
                          caller_function, caller_pc);
        return false;
    }
    const Om_Bytecode_Function *function =
        &state->image->functions[function_index];
    if ((function->flags & OM_BYTECODE_FUNCTION_EXTERNAL) != 0) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                          caller_function, caller_pc);
        return false;
    }
    if (argument_count != function->parameter_count
        || function->local_count < function->parameter_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_ARITY,
                          caller_function, caller_pc);
        return false;
    }
    if (state->frame_count >= state->limits.max_call_frames) {
        set_runtime_error(state, result,
                          OM_VM_RUNTIME_ERROR_CALL_DEPTH_LIMIT,
                          caller_function, caller_pc);
        return false;
    }
    if (state->frame_count == state->frame_capacity) {
        const Om_Status status = grow_frame_array(state);
        if (status != OM_STATUS_OK) {
            set_runtime_error(state, result,
                status == OM_STATUS_OUT_OF_MEMORY
                    ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                    : OM_VM_RUNTIME_ERROR_CALL_DEPTH_LIMIT,
                caller_function, caller_pc);
            return false;
        }
    }
    Om_VM_Value *locals = NULL;
    if (function->local_count != 0) {
        locals = om_alloc_array(function->local_count, sizeof(*locals));
        if (locals == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              caller_function, caller_pc);
            return false;
        }
        for (size_t index = 0; index < function->local_count; ++index) {
            locals[index] = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
        }
        for (size_t index = 0; index < argument_count; ++index) {
            if (!value_is_valid_for_vm(state, arguments[index])) {
                om_free(locals);
                set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                                  caller_function, caller_pc);
                return false;
            }
            locals[index] = arguments[index];
        }
    }
    state->frames[state->frame_count++] = (Om_VM_Call_Frame){
        .function_index = function_index,
        .pc = function->entry_pc,
        .return_pc = return_pc,
        .stack_base = stack_base,
        .locals = locals,
        .actor_id = actor_id,
        .return_mode = (uint8_t)return_mode
    };
    return true;
}

static void clear_frames(Om_VM_State *state)
{
    for (size_t index = 0; index < state->frame_count; ++index) {
        om_free(state->frames[index].locals);
    }
    state->frame_count = 0;
    state->stack_length = 0;
}

static bool buffer_append(Om_VM_State *state, Om_VM_Byte_Buffer *buffer,
                          const char *bytes, size_t length,
                          Om_VM_Execution_Result *result,
                          uint32_t function_index, uint32_t pc)
{
    if (length == 0) {
        return true;
    }
    if (bytes == NULL || length > state->limits.max_heap_bytes
        || buffer->length > state->limits.max_heap_bytes - length) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_HEAP_LIMIT,
                          function_index, pc);
        return false;
    }
    size_t required = 0;
    if (!om_checked_size_add(buffer->length, length, &required)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_HEAP_LIMIT,
                          function_index, pc);
        return false;
    }
    if (buffer->data == NULL || required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0
            ? (state->limits.max_heap_bytes < 64
                   ? state->limits.max_heap_bytes : 64)
            : buffer->capacity;
        while (capacity < required) {
            if (capacity > state->limits.max_heap_bytes / 2) {
                capacity = state->limits.max_heap_bytes;
            } else {
                capacity *= 2;
            }
            if (capacity < required && capacity == state->limits.max_heap_bytes) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_HEAP_LIMIT,
                                  function_index, pc);
                return false;
            }
        }
        char *data = om_realloc_array(buffer->data, capacity, sizeof(char));
        if (data == NULL) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, pc);
            return false;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    (void)memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length = required;
    return true;
}

static bool buffer_append_cstr(Om_VM_State *state, Om_VM_Byte_Buffer *buffer,
                               const char *text,
                               Om_VM_Execution_Result *result,
                               uint32_t function_index, uint32_t pc)
{
    return buffer_append(state, buffer, text, strlen(text), result,
                         function_index, pc);
}

static bool value_equal(Om_VM_Value left, Om_VM_Value right, size_t depth);

static bool string_object_equal(const Om_VM_String_Object *left,
                                const Om_VM_String_Object *right)
{
    return left->length == right->length
        && (left->length == 0
            || memcmp(left->bytes, right->bytes, left->length) == 0);
}

static bool map_find(const Om_VM_Map_Object *map, Om_VM_Value key,
                     size_t depth, Om_VM_Value *value)
{
    for (size_t reverse = map->length; reverse > 0; --reverse) {
        const Om_VM_Map_Entry *entry = &map->entries[reverse - 1];
        if (value_equal(entry->key, key, depth + 1)) {
            if (value != NULL) {
                *value = entry->value;
            }
            return true;
        }
    }
    return false;
}

static bool object_equal(Om_VM_Object *left, Om_VM_Object *right,
                         size_t depth)
{
    if (left == right) {
        return true;
    }
    if (left == NULL || right == NULL || left->kind != right->kind
        || depth > OM_VM_MAX_FORMAT_DEPTH) {
        return false;
    }
    switch (left->kind) {
    case OM_VM_OBJECT_STRING:
        return string_object_equal((Om_VM_String_Object *)left,
                                   (Om_VM_String_Object *)right);
    case OM_VM_OBJECT_LIST: {
        const Om_VM_List_Object *a = (const Om_VM_List_Object *)left;
        const Om_VM_List_Object *b = (const Om_VM_List_Object *)right;
        if (a->length != b->length) {
            return false;
        }
        for (size_t index = 0; index < a->length; ++index) {
            if (!value_equal(a->items[index], b->items[index], depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case OM_VM_OBJECT_MAP: {
        const Om_VM_Map_Object *a = (const Om_VM_Map_Object *)left;
        const Om_VM_Map_Object *b = (const Om_VM_Map_Object *)right;
        if (a->length != b->length) {
            return false;
        }
        for (size_t index = 0; index < a->length; ++index) {
            Om_VM_Value other = {0};
            if (!map_find(b, a->entries[index].key, depth + 1, &other)
                || !value_equal(a->entries[index].value, other, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case OM_VM_OBJECT_RECORD:
    case OM_VM_OBJECT_MESSAGE: {
        const Om_VM_Record_Object *a = (const Om_VM_Record_Object *)left;
        const Om_VM_Record_Object *b = (const Om_VM_Record_Object *)right;
        if (a->schema_index != b->schema_index
            || a->field_count != b->field_count) {
            return false;
        }
        for (size_t index = 0; index < a->field_count; ++index) {
            if (!value_equal(a->fields[index], b->fields[index], depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case OM_VM_OBJECT_RESULT: {
        const Om_VM_Result_Object *a = (Om_VM_Result_Object *)left;
        const Om_VM_Result_Object *b = (Om_VM_Result_Object *)right;
        return a->is_ok == b->is_ok
            && value_equal(a->payload, b->payload, depth + 1);
    }
    case OM_VM_OBJECT_FUNCTION:
        return ((Om_VM_Function_Object *)left)->function_index
            == ((Om_VM_Function_Object *)right)->function_index;
    case OM_VM_OBJECT_ITERATOR:
        return false;
    }
    return false;
}

static bool value_equal(Om_VM_Value left, Om_VM_Value right, size_t depth)
{
    if (left.tag != right.tag || depth > OM_VM_MAX_FORMAT_DEPTH) {
        return false;
    }
    switch (left.tag) {
    case OM_VM_VALUE_NULL:
    case OM_VM_VALUE_UNIT:
        return true;
    case OM_VM_VALUE_BOOL:
        return left.as.boolean == right.as.boolean;
    case OM_VM_VALUE_INT:
        return left.as.integer == right.as.integer;
    case OM_VM_VALUE_FLOAT:
        return left.as.floating == right.as.floating;
    case OM_VM_VALUE_ACTOR_ID:
    case OM_VM_VALUE_ACTOR_REF:
        return left.as.actor_id == right.as.actor_id;
    case OM_VM_VALUE_NODE_REF:
        return left.as.node_ref == right.as.node_ref;
    case OM_VM_VALUE_DOWN_REASON:
        return left.as.down_reason == right.as.down_reason;
    case OM_VM_VALUE_OBJECT:
        return object_equal(left.as.object, right.as.object, depth + 1);
    case OM_VM_VALUE_UNINITIALIZED:
        return false;
    }
    return false;
}

static bool value_format(Om_VM_State *state, Om_VM_Value value,
                         Om_VM_Byte_Buffer *buffer, size_t depth,
                         Om_VM_Execution_Result *result,
                         uint32_t function_index, uint32_t pc);

static bool format_record(Om_VM_State *state,
                          const Om_VM_Record_Object *record,
                          Om_VM_Byte_Buffer *buffer, size_t depth,
                          Om_VM_Execution_Result *result,
                          uint32_t function_index, uint32_t pc)
{
    if (record->schema_index >= state->image->schema_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_Bytecode_Schema *schema =
        &state->image->schemas[record->schema_index];
    const char *open = record->base.kind == OM_VM_OBJECT_MESSAGE
        ? "Message{" : "Record{";
    if (!buffer_append_cstr(state, buffer, open, result, function_index, pc)) {
        return false;
    }
    for (size_t index = 0; index < record->field_count; ++index) {
        const Om_Bytecode_Constant *name_constant =
            &state->image->constants[
                state->image->schema_fields[schema->first_field + index]
                    .name_constant];
        if (index != 0
            && !buffer_append_cstr(state, buffer, ", ", result,
                                   function_index, pc)) {
            return false;
        }
        if (!buffer_append(state, buffer, name_constant->as.string_value.data,
                           name_constant->as.string_value.length, result,
                           function_index, pc)
            || !buffer_append_cstr(state, buffer, ": ", result,
                                   function_index, pc)
            || !value_format(state, record->fields[index], buffer, depth + 1,
                             result, function_index, pc)) {
            return false;
        }
    }
    return buffer_append_cstr(state, buffer, "}", result,
                              function_index, pc);
}

static bool value_format(Om_VM_State *state, Om_VM_Value value,
                         Om_VM_Byte_Buffer *buffer, size_t depth,
                         Om_VM_Execution_Result *result,
                         uint32_t function_index, uint32_t pc)
{
    if (depth > OM_VM_MAX_FORMAT_DEPTH) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_HEAP_LIMIT,
                          function_index, pc);
        return false;
    }
    char number[128] = {0};
    int written = 0;
    switch (value.tag) {
    case OM_VM_VALUE_UNINITIALIZED:
        return buffer_append_cstr(state, buffer, "<uninitialized>", result,
                                  function_index, pc);
    case OM_VM_VALUE_NULL:
        return buffer_append_cstr(state, buffer, "NULL", result,
                                  function_index, pc);
    case OM_VM_VALUE_UNIT:
        return buffer_append_cstr(state, buffer, "UNIT", result,
                                  function_index, pc);
    case OM_VM_VALUE_BOOL:
        return buffer_append_cstr(state, buffer,
                                  value.as.boolean ? "TRUE" : "FALSE",
                                  result, function_index, pc);
    case OM_VM_VALUE_INT:
        written = snprintf(number, sizeof(number), "%" PRId64,
                           value.as.integer);
        break;
    case OM_VM_VALUE_FLOAT:
        written = snprintf(number, sizeof(number), "%.17g", value.as.floating);
        break;
    case OM_VM_VALUE_ACTOR_ID:
        written = snprintf(number, sizeof(number), "Actor_Id(%" PRIu64 ")",
                           value.as.actor_id);
        break;
    case OM_VM_VALUE_ACTOR_REF:
        written = snprintf(number, sizeof(number), "ActorRef(%" PRIu64 ")",
                           value.as.actor_id);
        break;
    case OM_VM_VALUE_NODE_REF:
        written = snprintf(number, sizeof(number), "Node_Ref(%" PRIu64 ")",
                           value.as.node_ref);
        break;
    case OM_VM_VALUE_DOWN_REASON:
        written = snprintf(number, sizeof(number), "Actor_Down(%" PRIu32 ")",
                           value.as.down_reason);
        break;
    case OM_VM_VALUE_OBJECT:
        if (!object_belongs_to_vm(state, value.as.object)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        switch (value.as.object->kind) {
        case OM_VM_OBJECT_STRING: {
            const Om_VM_String_Object *string =
                (const Om_VM_String_Object *)value.as.object;
            return buffer_append(state, buffer, string->bytes, string->length,
                                 result, function_index, pc);
        }
        case OM_VM_OBJECT_LIST: {
            const Om_VM_List_Object *list =
                (const Om_VM_List_Object *)value.as.object;
            if (!buffer_append_cstr(state, buffer, "[", result,
                                    function_index, pc)) {
                return false;
            }
            for (size_t index = 0; index < list->length; ++index) {
                if ((index != 0
                     && !buffer_append_cstr(state, buffer, ", ", result,
                                            function_index, pc))
                    || !value_format(state, list->items[index], buffer,
                                     depth + 1, result, function_index, pc)) {
                    return false;
                }
            }
            return buffer_append_cstr(state, buffer, "]", result,
                                      function_index, pc);
        }
        case OM_VM_OBJECT_MAP: {
            const Om_VM_Map_Object *map =
                (const Om_VM_Map_Object *)value.as.object;
            if (!buffer_append_cstr(state, buffer, "{", result,
                                    function_index, pc)) {
                return false;
            }
            for (size_t index = 0; index < map->length; ++index) {
                if ((index != 0
                     && !buffer_append_cstr(state, buffer, ", ", result,
                                            function_index, pc))
                    || !value_format(state, map->entries[index].key,
                                     buffer, depth + 1, result,
                                     function_index, pc)
                    || !buffer_append_cstr(state, buffer, ": ", result,
                                           function_index, pc)
                    || !value_format(state, map->entries[index].value,
                                     buffer, depth + 1, result,
                                     function_index, pc)) {
                    return false;
                }
            }
            return buffer_append_cstr(state, buffer, "}", result,
                                      function_index, pc);
        }
        case OM_VM_OBJECT_RECORD:
        case OM_VM_OBJECT_MESSAGE:
            return format_record(state,
                (const Om_VM_Record_Object *)value.as.object, buffer,
                depth, result, function_index, pc);
        case OM_VM_OBJECT_RESULT: {
            const Om_VM_Result_Object *result_object =
                (const Om_VM_Result_Object *)value.as.object;
            if (!buffer_append_cstr(state, buffer,
                                    result_object->is_ok ? "Ok(" : "Error(",
                                    result, function_index, pc)
                || !value_format(state, result_object->payload, buffer,
                                 depth + 1, result, function_index, pc)) {
                return false;
            }
            return buffer_append_cstr(state, buffer, ")", result,
                                      function_index, pc);
        }
        case OM_VM_OBJECT_FUNCTION:
            written = snprintf(number, sizeof(number), "<function #%" PRIu32 ">",
                ((Om_VM_Function_Object *)value.as.object)->function_index);
            break;
        case OM_VM_OBJECT_ITERATOR:
            return buffer_append_cstr(state, buffer, "<iterator>", result,
                                      function_index, pc);
        }
        break;
    }
    if (written < 0 || (size_t)written >= sizeof(number)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    return buffer_append(state, buffer, number, (size_t)written, result,
                         function_index, pc);
}

static bool vm_to_string(Om_VM_State *state, Om_VM_Value input,
                         Om_VM_Value *output,
                         Om_VM_Execution_Result *result,
                         uint32_t function_index, uint32_t pc)
{
    if (input.tag == OM_VM_VALUE_OBJECT
        && object_belongs_to_vm(state, input.as.object)
        && input.as.object->kind == OM_VM_OBJECT_STRING) {
        *output = input;
        return true;
    }
    Om_VM_Byte_Buffer buffer = {0};
    const bool formatted = value_format(state, input, &buffer, 0, result,
                                        function_index, pc);
    if (!formatted) {
        om_free(buffer.data);
        return false;
    }
    const Om_Status status = om_vm_heap_make_string(
        active_heap(state), (Om_String_View){buffer.data, buffer.length}, output);
    om_free(buffer.data);
    if (status != OM_STATUS_OK) {
        set_runtime_error(state, result, error_for_allocation_status(status),
                          function_index, pc);
        return false;
    }
    return true;
}

static bool compare_order(Om_VM_Value left, Om_VM_Value right,
                          int *ordering)
{
    if (ordering == NULL || left.tag != right.tag) {
        return false;
    }
    switch (left.tag) {
    case OM_VM_VALUE_INT:
        *ordering = (left.as.integer > right.as.integer)
            - (left.as.integer < right.as.integer);
        return true;
    case OM_VM_VALUE_FLOAT:
        if (isnan(left.as.floating) || isnan(right.as.floating)) {
            return false;
        }
        *ordering = (left.as.floating > right.as.floating)
            - (left.as.floating < right.as.floating);
        return true;
    case OM_VM_VALUE_OBJECT:
        if (left.as.object == NULL || right.as.object == NULL
            || left.as.object->kind != OM_VM_OBJECT_STRING
            || right.as.object->kind != OM_VM_OBJECT_STRING) {
            return false;
        }
        {
            const Om_VM_String_Object *a =
                (const Om_VM_String_Object *)left.as.object;
            const Om_VM_String_Object *b =
                (const Om_VM_String_Object *)right.as.object;
            const size_t common = a->length < b->length ? a->length : b->length;
            const int order = common == 0
                ? 0 : memcmp(a->bytes, b->bytes, common);
            *ordering = order != 0 ? (order > 0 ? 1 : -1)
                : (a->length > b->length) - (a->length < b->length);
            return true;
        }
    default:
        return false;
    }
}

static bool get_string_constant(const Om_VM_State *state, uint32_t index,
                                Om_String_View *view)
{
    if (index >= state->image->constant_count || view == NULL) {
        return false;
    }
    const Om_Bytecode_Constant *constant = &state->image->constants[index];
    if (constant->kind != OM_BYTECODE_CONSTANT_STRING) {
        return false;
    }
    *view = constant->as.string_value;
    return true;
}

static bool find_schema(const Om_VM_State *state, uint32_t symbol_id,
                        Om_Bytecode_Schema_Kind kind, uint32_t *index)
{
    for (size_t cursor = 0; cursor < state->image->schema_count; ++cursor) {
        const Om_Bytecode_Schema *schema = &state->image->schemas[cursor];
        if (schema->symbol_id.value == symbol_id && schema->kind == kind) {
            if (index != NULL) {
                *index = (uint32_t)cursor;
            }
            return true;
        }
    }
    return false;
}

static bool find_record_field(const Om_VM_State *state,
                              const Om_VM_Record_Object *record,
                              Om_String_View name, size_t *field_index)
{
    if (record->schema_index >= state->image->schema_count) {
        return false;
    }
    const Om_Bytecode_Schema *schema =
        &state->image->schemas[record->schema_index];
    if (schema->field_count != record->field_count) {
        return false;
    }
    for (size_t cursor = 0; cursor < schema->field_count; ++cursor) {
        const size_t schema_field_index = schema->first_field + cursor;
        if (schema_field_index >= state->image->schema_field_count) {
            return false;
        }
        Om_String_View candidate = {0};
        const uint32_t name_constant =
            state->image->schema_fields[schema_field_index].name_constant;
        if (!get_string_constant(state, name_constant, &candidate)) {
            return false;
        }
        if (om_string_view_equal(candidate, name)) {
            *field_index = cursor;
            return true;
        }
    }
    return false;
}

static bool finish_current_frame(Om_VM_State *state, Om_VM_Value value,
                                 Om_VM_Execution_Result *result, bool *finished,
                                 uint32_t function_index, uint32_t pc)
{
    if (state->frame_count == 0 || !value_is_valid_for_vm(state, value)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_Call_Frame frame = state->frames[state->frame_count - 1];
    om_free(frame.locals);
    state->frame_count -= 1;
    state->stack_length = frame.stack_base;
    const Om_VM_Return_Mode return_mode =
        (Om_VM_Return_Mode)frame.return_mode;
    if (return_mode == OM_VM_RETURN_ROOT) {
        if (state->frame_count != 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        result->succeeded = true;
        result->value = value;
        result->error = (Om_VM_Runtime_Error){0};
        *finished = true;
        return true;
    }
    if (return_mode == OM_VM_RETURN_SCHEDULED_HANDLER) {
        if (state->frame_count != 0
            || frame.actor_id == 0
            || frame.actor_id != state->handler_actor_id) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        state->handler_result = value;
        state->handler_completed = true;
        *finished = true;
        return true;
    }
    if (return_mode == OM_VM_RETURN_REMOTE_INITIALIZER) {
        if (state->frame_count != 0 || frame.actor_id == 0
            || frame.actor_id != state->handler_actor_id) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        state->handler_result = value;
        state->handler_completed = true;
        *finished = true;
        return true;
    }
    if (state->frame_count == 0) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    state->frames[state->frame_count - 1].pc = frame.return_pc;
    if (return_mode == OM_VM_RETURN_SPAWN_INITIALIZER) {
        return push_value(state, om_vm_value_actor_ref(frame.actor_id), result,
                          state->frames[state->frame_count - 1].function_index,
                          frame.return_pc == 0 ? 0 : frame.return_pc - 1);
    }
    return push_value(state, value, result,
                      state->frames[state->frame_count - 1].function_index,
                      state->frames[state->frame_count - 1].pc == 0
                          ? 0 : state->frames[state->frame_count - 1].pc - 1);
}

static bool return_error_for_heap(Om_VM_State *state,
                                  Om_VM_Execution_Result *result,
                                  Om_Status status, uint32_t function_index,
                                  uint32_t pc)
{
    set_runtime_error(state, result, error_for_allocation_status(status),
                      function_index, pc);
    return false;
}

static bool execute_int_binary(Om_VM_State *state,
                               Om_Bytecode_Opcode opcode,
                               Om_VM_Execution_Result *result,
                               uint32_t function_index, uint32_t pc)
{
    if (state->stack_length < 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value left = peek_value(state, 1);
    const Om_VM_Value right = peek_value(state, 0);
    if (left.tag != OM_VM_VALUE_INT || right.tag != OM_VM_VALUE_INT) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    int64_t value = 0;
    bool overflow = false;
    switch (opcode) {
    case OM_BC_ADD_INT:
        overflow = ckd_add(&value, left.as.integer, right.as.integer);
        break;
    case OM_BC_SUB_INT:
        overflow = ckd_sub(&value, left.as.integer, right.as.integer);
        break;
    case OM_BC_MUL_INT:
        overflow = ckd_mul(&value, left.as.integer, right.as.integer);
        break;
    case OM_BC_DIV_INT:
        if (right.as.integer == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO,
                              function_index, pc);
            return false;
        }
        if (left.as.integer == INT64_MIN && right.as.integer == -1) {
            overflow = true;
        } else {
            value = left.as.integer / right.as.integer;
        }
        break;
    case OM_BC_MOD_INT:
        if (right.as.integer == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO,
                              function_index, pc);
            return false;
        }
        if (left.as.integer == INT64_MIN && right.as.integer == -1) {
            overflow = true;
        } else {
            value = left.as.integer % right.as.integer;
        }
        break;
    default:
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    if (overflow) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INTEGER_OVERFLOW,
                          function_index, pc);
        return false;
    }
    state->stack_length -= 2;
    return push_value(state, om_vm_value_int(value), result, function_index, pc);
}

static bool execute_float_binary(Om_VM_State *state,
                                 Om_Bytecode_Opcode opcode,
                                 Om_VM_Execution_Result *result,
                                 uint32_t function_index, uint32_t pc)
{
    if (state->stack_length < 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value left = peek_value(state, 1);
    const Om_VM_Value right = peek_value(state, 0);
    if (left.tag != OM_VM_VALUE_FLOAT || right.tag != OM_VM_VALUE_FLOAT) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    if (opcode == OM_BC_DIV_FLOAT && right.as.floating == 0.0) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO,
                          function_index, pc);
        return false;
    }
    double value = 0.0;
    switch (opcode) {
    case OM_BC_ADD_FLOAT:
        value = left.as.floating + right.as.floating;
        break;
    case OM_BC_SUB_FLOAT:
        value = left.as.floating - right.as.floating;
        break;
    case OM_BC_MUL_FLOAT:
        value = left.as.floating * right.as.floating;
        break;
    case OM_BC_DIV_FLOAT:
        value = left.as.floating / right.as.floating;
        break;
    default:
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    state->stack_length -= 2;
    return push_value(state, om_vm_value_float(value), result,
                      function_index, pc);
}

static bool execute_comparison(Om_VM_State *state,
                               Om_Bytecode_Opcode opcode,
                               Om_VM_Execution_Result *result,
                               uint32_t function_index, uint32_t pc)
{
    if (state->stack_length < 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value left = peek_value(state, 1);
    const Om_VM_Value right = peek_value(state, 0);
    bool answer = false;
    if (opcode == OM_BC_EQ || opcode == OM_BC_NE) {
        answer = value_equal(left, right, 0);
        if (opcode == OM_BC_NE) {
            answer = !answer;
        }
    } else {
        int ordering = 0;
        if (!compare_order(left, right, &ordering)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        switch (opcode) {
        case OM_BC_LT: answer = ordering < 0; break;
        case OM_BC_LE: answer = ordering <= 0; break;
        case OM_BC_GT: answer = ordering > 0; break;
        case OM_BC_GE: answer = ordering >= 0; break;
        default:
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
    }
    state->stack_length -= 2;
    return push_value(state, om_vm_value_bool(answer), result,
                      function_index, pc);
}

static bool vm_value_to_native(Om_VM_State *state, Om_VM_Value value,
                               Om_Native_Abi_Type abi_type,
                               Om_Native_Value *native_value)
{
    if (native_value == NULL) {
        return false;
    }
    *native_value = (Om_Native_Value){.abi_type = abi_type};
    if (abi_type == OM_NATIVE_ABI_CBOOL) {
        if (value.tag != OM_VM_VALUE_BOOL) {
            return false;
        }
        native_value->kind = OM_NATIVE_VALUE_SCALAR;
        native_value->as.boolean = value.as.boolean;
        return true;
    }
    if (abi_type == OM_NATIVE_ABI_CFLOAT
        || abi_type == OM_NATIVE_ABI_CDOUBLE) {
        if (value.tag != OM_VM_VALUE_FLOAT) {
            return false;
        }
        native_value->kind = OM_NATIVE_VALUE_SCALAR;
        native_value->as.floating_point = value.as.floating;
        return true;
    }
    if (abi_type == OM_NATIVE_ABI_CSTRING) {
        Om_String_View view = {0};
        if (!om_vm_value_as_string(value, &view)
            || !object_belongs_to_vm(state, value.as.object)) {
            return false;
        }
        native_value->kind = OM_NATIVE_VALUE_UTF8_STRING;
        native_value->as.utf8_string = view;
        return true;
    }
    Om_Native_Abi_Type_Info info = {0};
    if (value.tag != OM_VM_VALUE_INT
        || om_native_abi_type_info(abi_type, &info) != OM_STATUS_OK
        || info.is_floating_point || info.is_pointer) {
        return false;
    }
    native_value->kind = OM_NATIVE_VALUE_SCALAR;
    if (info.is_signed_integer) {
        native_value->as.signed_integer = value.as.integer;
        return true;
    }
    if (value.as.integer < 0) {
        return false;
    }
    native_value->as.unsigned_integer = (uint64_t)value.as.integer;
    return true;
}

static Om_Status native_value_to_vm(Om_VM_State *state,
                                   const Om_Native_Call_Binding *binding,
                                   Om_Native_Call_Result *native_result,
                                   Om_VM_Value *value)
{
    const Om_Native_Abi_Type type = native_result->value.abi_type;
    if (type == OM_NATIVE_ABI_CVOID) {
        *value = om_vm_value_unit();
        return OM_STATUS_OK;
    }
    if (type == OM_NATIVE_ABI_CBOOL) {
        if (native_result->value.kind != OM_NATIVE_VALUE_SCALAR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *value = om_vm_value_bool(native_result->value.as.boolean);
        return OM_STATUS_OK;
    }
    if (type == OM_NATIVE_ABI_CFLOAT || type == OM_NATIVE_ABI_CDOUBLE) {
        if (native_result->value.kind != OM_NATIVE_VALUE_SCALAR) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        *value = om_vm_value_float(
            native_result->value.as.floating_point);
        return OM_STATUS_OK;
    }
    if (type == OM_NATIVE_ABI_CSTRING) {
        Om_String_View view = {0};
        Om_Native_String temporary_copy = {0};
        Om_Status status = OM_STATUS_OK;
        if (native_result->value.kind == OM_NATIVE_VALUE_UTF8_STRING) {
            view = native_result->value.as.utf8_string;
        } else if (native_result->value.kind == OM_NATIVE_VALUE_POINTER
                   && native_result->value.as.pointer != NULL) {
            status = om_native_utf8_from_cstring(
                native_result->value.as.pointer,
                OM_NATIVE_DEFAULT_MAX_CSTRING_BYTES, &temporary_copy);
            if (status == OM_STATUS_OK) {
                view = (Om_String_View){temporary_copy.data,
                                        temporary_copy.length};
            }
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (native_result->ownership == OM_NATIVE_OWNERSHIP_OWNED) {
            const Om_Status release_status =
                om_native_call_result_release_bound(binding, native_result);
            if (release_status != OM_STATUS_OK) {
                status = release_status;
            }
        }
        Om_VM_Heap *heap = active_heap(state);
        if (status == OM_STATUS_OK && heap == NULL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = om_vm_heap_make_string(heap, view, value);
        }
        om_native_string_destroy(&temporary_copy);
        return status;
    }
    if (native_result->value.kind != OM_NATIVE_VALUE_SCALAR) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Native_Abi_Type_Info info = {0};
    if (om_native_abi_type_info(type, &info) != OM_STATUS_OK
        || info.is_floating_point || info.is_pointer) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (info.is_signed_integer) {
        *value = om_vm_value_int(
            native_result->value.as.signed_integer);
        return OM_STATUS_OK;
    }
    if (native_result->value.as.unsigned_integer > (uint64_t)INT64_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *value = om_vm_value_int(
        (int64_t)native_result->value.as.unsigned_integer);
    return OM_STATUS_OK;
}

typedef struct Om_VM_Native_Callback_Context {
    Om_VM vm;
    uint64_t actor_id;
    Om_Symbol_Id message_symbol;
    size_t field_count;
    atomic_int status;
    bool initialized;
} Om_VM_Native_Callback_Context;

typedef struct Om_VM_Native_Call_Task {
    Om_VM_State *shared;
    const Om_Native_Call_Binding *binding;
    uint64_t actor_id;
    uint32_t caller_function;
    uint32_t caller_instruction;
    Om_Native_Value *arguments;
    Om_Native_Callback_Closure *closures;
    Om_VM_Native_Callback_Context *callback_contexts;
    size_t parameter_count;
    Om_Native_Call_Result result;
} Om_VM_Native_Call_Task;

static bool find_native_callback_target(const Om_VM_State *shared,
                                       uint32_t function_index,
                                       size_t parameter_index,
                                       Om_Symbol_Id *message_symbol)
{
    for (size_t index = 0; index < shared->native_callback_target_count;
         ++index) {
        const Om_VM_Native_Callback_Target *target =
            &shared->native_callback_targets[index];
        if (target->function_index == function_index
            && target->parameter_index == parameter_index) {
            if (message_symbol != NULL) {
                *message_symbol = target->message_symbol;
            }
            return true;
        }
    }
    return false;
}

static Om_Status vm_native_callback_entry(
    void *opaque, const Om_Native_Value *arguments, size_t argument_count)
{
    Om_VM_Native_Callback_Context *context = opaque;
    Om_Status status = OM_STATUS_OK;
    if (context == NULL
        || argument_count != context->field_count
        || (argument_count != 0 && arguments == NULL)
        || argument_count > OM_NATIVE_MAX_CALLBACK_PARAMETERS) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Value fields[OM_NATIVE_MAX_CALLBACK_PARAMETERS] = {{0}};
    for (size_t index = 0; index < argument_count; ++index) {
        const Om_Native_Value *argument = &arguments[index];
        if (argument->kind != OM_NATIVE_VALUE_SCALAR) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (argument->abi_type == OM_NATIVE_ABI_CBOOL) {
            fields[index] = om_vm_value_bool(argument->as.boolean);
        } else if (argument->abi_type == OM_NATIVE_ABI_CFLOAT
                   || argument->abi_type == OM_NATIVE_ABI_CDOUBLE) {
            fields[index] = om_vm_value_float(argument->as.floating_point);
        } else {
            Om_Native_Abi_Type_Info info = {0};
            if (om_native_abi_type_info(argument->abi_type, &info)
                != OM_STATUS_OK || info.is_floating_point
                || info.is_pointer) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (info.is_signed_integer) {
                fields[index] = om_vm_value_int(argument->as.signed_integer);
            } else if (argument->as.unsigned_integer <= (uint64_t)INT64_MAX) {
                fields[index] = om_vm_value_int(
                    (int64_t)argument->as.unsigned_integer);
            } else {
                status = OM_STATUS_OUT_OF_RANGE;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_vm_native_callback_post(
            &context->vm, context->actor_id, context->message_symbol,
            fields, argument_count);
    }
    if (status != OM_STATUS_OK) {
        int expected = (int)OM_STATUS_OK;
        (void)atomic_compare_exchange_strong_explicit(
            &context->status, &expected, (int)status,
            memory_order_release, memory_order_relaxed);
    }
    return status;
}

static bool execute_native_call(Om_VM_State *state, uint32_t target_function,
                                size_t argument_count,
                                size_t arguments_start, size_t stack_base,
                                Om_VM_Execution_Result *result,
                                uint32_t caller_function, uint32_t pc)
{
    Om_VM_State *shared = shared_vm(state);
    if (target_function >= shared->native_binding_count
        || shared->native_bindings == NULL
        || shared->native_bindings[target_function] == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                          caller_function, pc);
        return false;
    }
    const Om_Native_Call_Binding *binding =
        shared->native_bindings[target_function];
    Om_Native_Call_Signature signature = {0};
    if (om_native_call_binding_signature(binding, &signature) != OM_STATUS_OK
        || (signature.parameter_count != 0 && signature.parameters == NULL)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                          caller_function, pc);
        return false;
    }
    size_t visible_parameter_count = 0;
    size_t callback_parameter_count = 0;
    for (size_t index = 0; index < signature.parameter_count; ++index) {
        if (signature.parameters[index].is_callback) {
            callback_parameter_count += 1;
        } else {
            visible_parameter_count += 1;
        }
    }
    if (visible_parameter_count != argument_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_ARITY,
                          caller_function, pc);
        return false;
    }
    Om_Native_Value *native_arguments = signature.parameter_count == 0 ? NULL
        : om_alloc_array(signature.parameter_count,
                         sizeof(*native_arguments));
    Om_Native_Callback_Closure *closures = callback_parameter_count == 0
        ? NULL : om_alloc_array(signature.parameter_count, sizeof(*closures));
    Om_VM_Native_Callback_Context *callback_contexts =
        callback_parameter_count == 0 ? NULL
        : om_alloc_array(signature.parameter_count,
                         sizeof(*callback_contexts));
    if ((signature.parameter_count != 0 && native_arguments == NULL)
        || (callback_parameter_count != 0
            && (closures == NULL || callback_contexts == NULL))) {
        om_free(native_arguments);
        om_free(closures);
        om_free(callback_contexts);
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          caller_function, pc);
        return false;
    }
    if (signature.parameter_count != 0) {
        (void)memset(native_arguments, 0,
                     signature.parameter_count * sizeof(*native_arguments));
    }
    if (closures != NULL) {
        (void)memset(closures, 0,
                     signature.parameter_count * sizeof(*closures));
        (void)memset(callback_contexts, 0,
                     signature.parameter_count * sizeof(*callback_contexts));
    }
    bool valid_arguments = true;
    size_t visible_index = 0;
    uint64_t actor_id = 0;
    if (state->frame_count != 0) {
        actor_id = state->frames[state->frame_count - 1].actor_id;
    }
    const Om_VM callback_vm = {.owned_state = shared};
    Om_Status callback_status = OM_STATUS_OK;
    for (size_t index = 0; index < signature.parameter_count; ++index) {
        const Om_Omi_Parameter *parameter = &signature.parameters[index];
        if (parameter->is_callback) {
            Om_Symbol_Id message_symbol = OM_SYMBOL_ID_INVALID;
            if (actor_id == 0
                || !find_native_callback_target(shared, target_function,
                                                index, &message_symbol)) {
                valid_arguments = false;
                break;
            }
            Om_VM_Native_Callback_Context *context = &callback_contexts[index];
            context->vm = callback_vm;
            context->actor_id = actor_id;
            context->message_symbol = message_symbol;
            context->field_count = parameter->callback_parameter_count;
            atomic_init(&context->status, (int)OM_STATUS_OK);
            context->initialized = true;
            callback_status = om_native_callback_closure_init(
                binding, index, vm_native_callback_entry, context,
                &closures[index]);
            if (callback_status != OM_STATUS_OK) {
                valid_arguments = false;
                break;
            }
            native_arguments[index] = (Om_Native_Value){
                .kind = OM_NATIVE_VALUE_POINTER,
                .abi_type = OM_NATIVE_ABI_CPOINTER,
                .as.pointer = om_native_callback_closure_code(
                    &closures[index])
            };
            if (native_arguments[index].as.pointer == NULL) {
                callback_status = OM_STATUS_INVALID_ARGUMENT;
                valid_arguments = false;
                break;
            }
        } else {
            if (!vm_value_to_native(
                    state, state->stack[arguments_start + visible_index],
                    signature.parameter_types[index],
                    &native_arguments[index])) {
                valid_arguments = false;
                break;
            }
            visible_index += 1;
        }
    }
    if (!valid_arguments) {
        for (size_t index = 0; index < signature.parameter_count; ++index) {
            if (closures != NULL) {
                om_native_callback_closure_destroy(&closures[index]);
            }
        }
        om_free(native_arguments);
        om_free(closures);
        om_free(callback_contexts);
        set_runtime_error(state, result,
            callback_status == OM_STATUS_OUT_OF_MEMORY
                ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                : OM_VM_RUNTIME_ERROR_TYPE,
            caller_function, pc);
        return false;
    }

    if (signature.blocking == OM_NATIVE_BLOCKING_YES && actor_id != 0) {
        Om_VM_Native_Call_Task *task = om_alloc(sizeof(*task));
        if (task == NULL) {
            for (size_t index = 0; index < signature.parameter_count; ++index) {
                if (closures != NULL) {
                    om_native_callback_closure_destroy(&closures[index]);
                }
            }
            om_free(native_arguments);
            om_free(closures);
            om_free(callback_contexts);
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              caller_function, pc);
            return false;
        }
        *task = (Om_VM_Native_Call_Task){
            .shared = shared,
            .binding = binding,
            .actor_id = actor_id,
            .caller_function = caller_function,
            .caller_instruction = pc,
            .arguments = native_arguments,
            .closures = closures,
            .callback_contexts = callback_contexts,
            .parameter_count = signature.parameter_count
        };
        void *pool = NULL;
        Om_Status submit_status = reserve_external_task(shared, false, &pool);
        bool actor_marked_waiting = false;
        if (submit_status == OM_STATUS_OK) {
            scheduler_mutex_lock(shared);
            Om_VM_Actor_State *actor = find_actor(shared, actor_id);
            if (actor == NULL || !actor->running
                || actor->waiting_native_call) {
                submit_status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                actor->waiting_native_call = true;
                actor_marked_waiting = true;
            }
            scheduler_mutex_unlock(shared);
        }
        if (submit_status == OM_STATUS_OK) {
            submit_status = om_blocking_pool_submit(
                (Om_Blocking_Pool *)pool,
                (Om_Blocking_Task){
                    .run = run_native_call_task,
                    .complete = complete_native_call_task,
                    .destroy = destroy_native_call_task,
                    .context = task
                });
        }
        if (submit_status != OM_STATUS_OK) {
            if (actor_marked_waiting) {
                scheduler_mutex_lock(shared);
                Om_VM_Actor_State *actor = find_actor(shared, actor_id);
                if (actor != NULL) {
                    actor->waiting_native_call = false;
                }
                scheduler_mutex_unlock(shared);
            }
            if (pool != NULL) {
                finish_external_task(shared);
            }
            destroy_native_call_task(task);
            set_runtime_error(state, result,
                submit_status == OM_STATUS_OUT_OF_MEMORY
                    ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                    : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                caller_function, pc);
            return false;
        }
        state->stack_length = stack_base;
        state->suspend_current_actor_after_instruction = true;
        return true;
    }

    Om_Native_Call_Result native_result = {0};
    Om_Status status = om_native_call_invoke(
        binding, native_arguments, signature.parameter_count,
        &native_result);
    for (size_t index = 0; index < signature.parameter_count; ++index) {
        if (closures != NULL) {
            om_native_callback_closure_destroy(&closures[index]);
        }
    }
    for (size_t index = 0; index < signature.parameter_count; ++index) {
        if (callback_contexts != NULL
            && signature.parameters[index].is_callback) {
            const Om_Status entry_status = (Om_Status)atomic_load_explicit(
                &callback_contexts[index].status, memory_order_acquire);
            if (entry_status != OM_STATUS_OK && callback_status == OM_STATUS_OK) {
                callback_status = entry_status;
            }
        }
    }
    om_free(native_arguments);
    om_free(closures);
    om_free(callback_contexts);
    if (status == OM_STATUS_OK && callback_status != OM_STATUS_OK) {
        status = callback_status;
    }
    if (status != OM_STATUS_OK) {
        om_native_call_result_destroy(&native_result);
        set_runtime_error(state, result,
            status == OM_STATUS_OUT_OF_MEMORY
                ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
            caller_function, pc);
        return false;
    }
    Om_VM_Value return_value = {.tag = OM_VM_VALUE_UNINITIALIZED};
    status = native_value_to_vm(state, binding, &native_result,
                                &return_value);
    om_native_call_result_destroy(&native_result);
    if (status != OM_STATUS_OK) {
        set_runtime_error(state, result,
            status == OM_STATUS_OUT_OF_MEMORY
                ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
            caller_function, pc);
        return false;
    }
    state->stack_length = stack_base;
    return push_value(state, return_value, result, caller_function, pc);
}

static bool execute_call(Om_VM_State *state, uint32_t target_function,
                         size_t argument_count, size_t arguments_start,
                         size_t stack_base, Om_VM_Execution_Result *result,
                         uint32_t caller_function, uint32_t pc)
{
    if (arguments_start > state->stack_length
        || argument_count > state->stack_length - arguments_start) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          caller_function, pc);
        return false;
    }
    if (target_function < state->image->function_count
        && (state->image->functions[target_function].flags
            & OM_BYTECODE_FUNCTION_EXTERNAL) != 0) {
        return execute_native_call(state, target_function, argument_count,
                                   arguments_start, stack_base, result,
                                   caller_function, pc);
    }
    if (!push_frame(state, target_function,
                    argument_count == 0 ? NULL : &state->stack[arguments_start],
                    argument_count, stack_base,
                    state->frames[state->frame_count - 1].pc,
                    state->frames[state->frame_count - 1].actor_id,
                    OM_VM_RETURN_NORMAL, result, caller_function, pc)) {
        return false;
    }
    state->stack_length = stack_base;
    return true;
}

static bool execute_make_string(Om_VM_State *state, size_t item_count,
                                Om_VM_Execution_Result *result,
                                uint32_t function_index, uint32_t pc)
{
    if (item_count > state->stack_length) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_Byte_Buffer buffer = {0};
    const size_t start = state->stack_length - item_count;
    for (size_t index = start; index < state->stack_length; ++index) {
        const Om_VM_Value value = state->stack[index];
        Om_String_View view = {0};
        if (!om_vm_value_as_string(value, &view)
            || !object_belongs_to_vm(state, value.as.object)) {
            om_free(buffer.data);
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if (!buffer_append(state, &buffer, view.data, view.length, result,
                           function_index, pc)) {
            om_free(buffer.data);
            return false;
        }
    }
    Om_VM_Value output = {0};
    const Om_Status status = om_vm_heap_make_string(
        active_heap(state), (Om_String_View){buffer.data, buffer.length}, &output);
    om_free(buffer.data);
    if (status != OM_STATUS_OK) {
        return return_error_for_heap(state, result, status,
                                     function_index, pc);
    }
    state->stack_length = start;
    return push_value(state, output, result, function_index, pc);
}

static bool execute_make_collection(Om_VM_State *state,
                                    const Om_Bytecode_Instruction *instruction,
                                    Om_VM_Execution_Result *result,
                                    uint32_t function_index, uint32_t pc)
{
    if (instruction->opcode == OM_BC_MAKE_LIST) {
        const size_t item_count = instruction->operand_a;
        if (item_count > state->stack_length) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const size_t start = state->stack_length - item_count;
        Om_VM_Value value = {0};
        const Om_Status status = om_vm_heap_make_list(
            active_heap(state),
            item_count == 0 ? NULL : &state->stack[start], item_count, &value);
        if (status != OM_STATUS_OK) {
            return return_error_for_heap(state, result, status,
                                         function_index, pc);
        }
        state->stack_length = start;
        return push_value(state, value, result, function_index, pc);
    }
    const size_t pair_count = instruction->operand_a;
    if (pair_count > state->stack_length / 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_Map_Entry *entries = pair_count == 0 ? NULL
        : om_alloc_array(pair_count, sizeof(*entries));
    if (pair_count != 0 && entries == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        return false;
    }
    const size_t start = state->stack_length - pair_count * 2;
    for (size_t index = 0; index < pair_count; ++index) {
        entries[index] = (Om_VM_Map_Entry){
            .key = state->stack[start + index * 2],
            .value = state->stack[start + index * 2 + 1]
        };
    }
    Om_VM_Value value = {0};
    const Om_Status status = om_vm_heap_make_map(
        active_heap(state), entries, pair_count, &value);
    om_free(entries);
    if (status != OM_STATUS_OK) {
        return return_error_for_heap(state, result, status,
                                     function_index, pc);
    }
    state->stack_length = start;
    return push_value(state, value, result, function_index, pc);
}

static bool execute_make_record(Om_VM_State *state,
                                const Om_Bytecode_Instruction *instruction,
                                Om_VM_Execution_Result *result,
                                uint32_t function_index, uint32_t pc)
{
    const Om_Bytecode_Schema_Kind schema_kind =
        instruction->opcode == OM_BC_MAKE_RECORD
            ? OM_BYTECODE_SCHEMA_RECORD : OM_BYTECODE_SCHEMA_MESSAGE;
    uint32_t schema_index = 0;
    if (!find_schema(state, instruction->operand_a, schema_kind,
                     &schema_index)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_Bytecode_Schema *schema =
        &state->image->schemas[schema_index];
    const size_t field_count = instruction->operand_b;
    if (field_count != schema->field_count
        || field_count > state->stack_length) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const size_t start = state->stack_length - field_count;
    Om_VM_Value value = {0};
    const Om_Status status = om_vm_heap_make_record(
        active_heap(state),
        schema_kind == OM_BYTECODE_SCHEMA_RECORD
            ? OM_VM_OBJECT_RECORD : OM_VM_OBJECT_MESSAGE,
        schema_index,
        field_count == 0 ? NULL : &state->stack[start], field_count, &value);
    if (status != OM_STATUS_OK) {
        return return_error_for_heap(state, result, status,
                                     function_index, pc);
    }
    state->stack_length = start;
    return push_value(state, value, result, function_index, pc);
}

static bool execute_get_field(Om_VM_State *state,
                              const Om_Bytecode_Instruction *instruction,
                              Om_VM_Execution_Result *result,
                              uint32_t function_index, uint32_t pc)
{
    if (state->stack_length == 0) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value receiver = peek_value(state, 0);
    Om_String_View name = {0};
    if (!get_string_constant(state, instruction->operand_a, &name)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    if (receiver.tag != OM_VM_VALUE_OBJECT
        || !object_belongs_to_vm(state, receiver.as.object)
        || (receiver.as.object->kind != OM_VM_OBJECT_RECORD
            && receiver.as.object->kind != OM_VM_OBJECT_MESSAGE)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Record_Object *record =
        (const Om_VM_Record_Object *)receiver.as.object;
    size_t field_index = 0;
    if (!find_record_field(state, record, name, &field_index)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_MISSING_FIELD,
                          function_index, pc);
        return false;
    }
    state->stack_length -= 1;
    return push_value(state, record->fields[field_index], result,
                      function_index, pc);
}

static bool execute_with_field(Om_VM_State *state,
                               const Om_Bytecode_Instruction *instruction,
                               Om_VM_Execution_Result *result,
                               uint32_t function_index, uint32_t pc)
{
    if (state->stack_length < 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value receiver = peek_value(state, 1);
    const Om_VM_Value replacement = peek_value(state, 0);
    Om_String_View name = {0};
    if (!get_string_constant(state, instruction->operand_a, &name)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    if (receiver.tag != OM_VM_VALUE_OBJECT
        || !object_belongs_to_vm(state, receiver.as.object)
        || (receiver.as.object->kind != OM_VM_OBJECT_RECORD
            && receiver.as.object->kind != OM_VM_OBJECT_MESSAGE)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Record_Object *record =
        (const Om_VM_Record_Object *)receiver.as.object;
    size_t field_index = 0;
    if (!find_record_field(state, record, name, &field_index)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_MISSING_FIELD,
                          function_index, pc);
        return false;
    }
    if (record->field_count == 0 || field_index >= record->field_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_MISSING_FIELD,
                          function_index, pc);
        return false;
    }
    Om_VM_Value *fields = om_alloc_array(record->field_count, sizeof(*fields));
    if (fields == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        return false;
    }
    (void)memcpy(fields, record->fields,
                 record->field_count * sizeof(*fields));
    fields[field_index] = replacement;
    Om_VM_Value updated = {0};
    const Om_Status status = om_vm_heap_make_record(
        active_heap(state), receiver.as.object->kind, record->schema_index,
        fields, record->field_count, &updated);
    om_free(fields);
    if (status != OM_STATUS_OK) {
        return return_error_for_heap(state, result, status,
                                     function_index, pc);
    }
    state->stack_length -= 2;
    return push_value(state, updated, result, function_index, pc);
}

static bool execute_get_index(Om_VM_State *state,
                              Om_VM_Execution_Result *result,
                              uint32_t function_index, uint32_t pc)
{
    if (state->stack_length < 2) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value collection = peek_value(state, 1);
    const Om_VM_Value index = peek_value(state, 0);
    Om_VM_Value item = {0};
    if (collection.tag != OM_VM_VALUE_OBJECT
        || !object_belongs_to_vm(state, collection.as.object)) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    if (collection.as.object->kind == OM_VM_OBJECT_LIST) {
        const Om_VM_List_Object *list =
            (const Om_VM_List_Object *)collection.as.object;
        if (index.tag != OM_VM_VALUE_INT) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if (index.as.integer < 0
            || (uint64_t)index.as.integer >= (uint64_t)list->length) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                              function_index, pc);
            return false;
        }
        item = list->items[(size_t)index.as.integer];
    } else if (collection.as.object->kind == OM_VM_OBJECT_MAP) {
        const Om_VM_Map_Object *map =
            (const Om_VM_Map_Object *)collection.as.object;
        if (!map_find(map, index, 0, &item)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_MISSING_KEY,
                              function_index, pc);
            return false;
        }
    } else {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    state->stack_length -= 2;
    return push_value(state, item, result, function_index, pc);
}

static bool execute_iteration(Om_VM_State *state,
                              const Om_Bytecode_Instruction *instruction,
                              Om_VM_Execution_Result *result,
                              uint32_t function_index, uint32_t pc)
{
    Om_VM_Call_Frame *frame = &state->frames[state->frame_count - 1];
    const Om_Bytecode_Function *function =
        &state->image->functions[frame->function_index];
    if (instruction->opcode == OM_BC_ITER_INIT) {
        if (state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const Om_VM_Value collection = peek_value(state, 0);
        if (collection.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, collection.as.object)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        Om_VM_Iterator_Kind kind;
        if (collection.as.object->kind == OM_VM_OBJECT_LIST) {
            kind = OM_VM_ITERATOR_LIST;
        } else if (collection.as.object->kind == OM_VM_OBJECT_MAP) {
            kind = OM_VM_ITERATOR_MAP;
        } else {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        Om_VM_Value iterator = {0};
        const Om_Status status = om_vm_heap_make_iterator(
            active_heap(state), kind, collection.as.object, &iterator);
        if (status != OM_STATUS_OK) {
            return return_error_for_heap(state, result, status,
                                         function_index, pc);
        }
        state->stack_length -= 1;
        return push_value(state, iterator, result, function_index, pc);
    }
    if (instruction->operand_a >= function->local_count
        || instruction->operand_b >= function->local_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_VM_Value iterator_value = frame->locals[instruction->operand_a];
    if (iterator_value.tag != OM_VM_VALUE_OBJECT
        || !object_belongs_to_vm(state, iterator_value.as.object)
        || iterator_value.as.object->kind != OM_VM_OBJECT_ITERATOR) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                          function_index, pc);
        return false;
    }
    Om_VM_Iterator_Object *iterator =
        (Om_VM_Iterator_Object *)iterator_value.as.object;
    bool has_next = false;
    Om_VM_Value next_value = {0};
    if (iterator->kind == OM_VM_ITERATOR_LIST) {
        const Om_VM_List_Object *list =
            (const Om_VM_List_Object *)iterator->collection;
        if (iterator->next_index < list->length) {
            next_value = list->items[iterator->next_index++];
            has_next = true;
        }
    } else {
        const Om_VM_Map_Object *map =
            (const Om_VM_Map_Object *)iterator->collection;
        if (iterator->next_index < map->length) {
            next_value = map->entries[iterator->next_index++].key;
            has_next = true;
        }
    }
    if (has_next) {
        frame->locals[instruction->operand_b] = next_value;
    }
    return push_value(state, om_vm_value_bool(has_next), result,
                      function_index, pc);
}

static bool execute_constant(Om_VM_State *state,
                             const Om_Bytecode_Instruction *instruction,
                             Om_VM_Execution_Result *result,
                             uint32_t function_index, uint32_t pc)
{
    if (instruction->operand_a >= state->image->constant_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    const Om_Bytecode_Constant *constant =
        &state->image->constants[instruction->operand_a];
    Om_VM_Value value = {0};
    switch (constant->kind) {
    case OM_BYTECODE_CONSTANT_NULL:
        value = om_vm_value_null();
        break;
    case OM_BYTECODE_CONSTANT_UNIT:
        value = om_vm_value_unit();
        break;
    case OM_BYTECODE_CONSTANT_BOOL:
        value = om_vm_value_bool(constant->as.boolean_value);
        break;
    case OM_BYTECODE_CONSTANT_INT:
        value = om_vm_value_int(constant->as.integer_value);
        break;
    case OM_BYTECODE_CONSTANT_FLOAT:
        value = om_vm_value_float(constant->as.float_value);
        break;
    case OM_BYTECODE_CONSTANT_DOWN_REASON:
        value = (Om_VM_Value){.tag = OM_VM_VALUE_DOWN_REASON,
                              .as.down_reason = constant->as.down_reason_value};
        break;
    case OM_BYTECODE_CONSTANT_STRING: {
        const Om_Status status = om_vm_heap_make_string(
            active_heap(state), constant->as.string_value, &value);
        if (status != OM_STATUS_OK) {
            return return_error_for_heap(state, result, status,
                                         function_index, pc);
        }
        break;
    }
    default:
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    return push_value(state, value, result, function_index, pc);
}

static Om_VM_Actor_State *find_actor(Om_VM_State *state, uint64_t actor_id)
{
    Om_VM_State *shared = shared_vm(state);
    if (actor_id == 0
        || actor_id > atomic_load_explicit(&shared->actor_count,
                                           memory_order_acquire)) {
        return NULL;
    }
    Om_VM_Actor_State *actor = &shared->actors[(size_t)(actor_id - 1)];
    return actor->id == actor_id ? actor : NULL;
}

static Om_Status reserve_actors(Om_VM_State *state, size_t required)
{
    Om_VM_State *shared = shared_vm(state);
    if (required > shared->limits.max_actors) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    return required <= shared->actor_capacity
        ? OM_STATUS_OK : OM_STATUS_OUT_OF_RANGE;
}

static Om_Status reserve_ready_queue(Om_VM_State *state, size_t required)
{
    state = shared_vm(state);
    if (required <= state->ready_capacity) {
        return OM_STATUS_OK;
    }
    if (state->ready_length != 0 && state->ready_capacity == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t capacity = state->ready_capacity == 0 ? 8 : state->ready_capacity;
    while (capacity < required) {
        if (capacity > state->limits.max_actors / 2) {
            capacity = state->limits.max_actors;
        } else {
            capacity *= 2;
        }
        if (capacity < required && capacity == state->limits.max_actors) {
            return OM_STATUS_OUT_OF_RANGE;
        }
    }
    uint64_t *queue = om_alloc_array(capacity, sizeof(*queue));
    if (queue == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < state->ready_length; ++index) {
        queue[index] = state->ready_queue[
            (state->ready_head + index) % state->ready_capacity];
    }
    om_free(state->ready_queue);
    state->ready_queue = queue;
    state->ready_head = 0;
    state->ready_capacity = capacity;
    return OM_STATUS_OK;
}

static bool schedule_actor_locked(Om_VM_State *state,
                                  Om_VM_Actor_State *actor,
                                  Om_VM_Execution_Result *result,
                                  uint32_t function_index, uint32_t pc)
{
    state = shared_vm(state);
    if (actor->scheduled || actor->running || actor->stopped
        || actor->waiting_native_call
        || (actor->mailbox_head == NULL && !actor->context_saved)) {
        return true;
    }
    if (state->ready_length == SIZE_MAX) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
                          function_index, pc);
        return false;
    }
    const Om_Status status = reserve_ready_queue(
        state, state->ready_length + 1);
    if (status != OM_STATUS_OK) {
        set_runtime_error(state, result,
            status == OM_STATUS_OUT_OF_MEMORY
                ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                : OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
            function_index, pc);
        return false;
    }
    const size_t tail = (state->ready_head + state->ready_length)
        % state->ready_capacity;
    state->ready_queue[tail] = actor->id;
    state->ready_length += 1;
    actor->scheduled = true;
    return true;
}

static bool finish_actor_quantum_locked(Om_VM_State *state,
                                        Om_VM_Actor_State *actor,
                                        Om_VM_Execution_Result *result,
                                        uint32_t function_index, uint32_t pc)
{
    Om_VM_State *shared = shared_vm(state);
    actor->running = false;
    if (shared->running_actor_count != 0) {
        shared->running_actor_count -= 1;
    }
    const bool scheduled = actor->stopped
        || atomic_load_explicit(&shared->scheduler_failed,
                                memory_order_acquire)
        || schedule_actor_locked(shared, actor, result, function_index, pc);
    scheduler_condition_broadcast(shared);
    return scheduled;
}

static void clear_actor_mailbox(Om_VM_State *state,
                                Om_VM_Actor_State *actor)
{
    state = shared_vm(state);
    Om_VM_Message_Node *node = actor->mailbox_head;
    while (node != NULL) {
        Om_VM_Message_Node *next = node->next;
        om_vm_heap_destroy(&node->transit_heap);
        om_free(node);
        if (state->mailbox_message_count != 0) {
            state->mailbox_message_count -= 1;
        }
        node = next;
    }
    actor->mailbox_head = NULL;
    actor->mailbox_tail = NULL;
    actor->mailbox_count = 0;
}

typedef struct Om_VM_Clone_Context {
    Om_VM_State *vm;
    Om_VM_Heap *destination;
    Om_VM_Object **sources;
    Om_VM_Object **copies;
    size_t count;
    size_t capacity;
    Om_Status status;
} Om_VM_Clone_Context;

static bool clone_context_reserve(Om_VM_Clone_Context *context)
{
    if (context->count < context->capacity) {
        return true;
    }
    const size_t capacity = context->capacity == 0 ? 8
        : context->capacity > SIZE_MAX / 2 ? SIZE_MAX
                                            : context->capacity * 2;
    if (capacity <= context->capacity) {
        context->status = OM_STATUS_OVERFLOW;
        return false;
    }
    Om_VM_Object **sources = om_realloc_array(
        context->sources, capacity, sizeof(*context->sources));
    if (sources == NULL) {
        context->status = OM_STATUS_OUT_OF_MEMORY;
        return false;
    }
    context->sources = sources;
    Om_VM_Object **copies = om_realloc_array(
        context->copies, capacity, sizeof(*context->copies));
    if (copies == NULL) {
        context->status = OM_STATUS_OUT_OF_MEMORY;
        return false;
    }
    context->copies = copies;
    context->capacity = capacity;
    return true;
}

static Om_VM_Object *clone_object_shell(Om_VM_Clone_Context *context,
                                        Om_VM_Object *source)
{
    size_t size = sizeof(Om_VM_Object);
    switch (source->kind) {
    case OM_VM_OBJECT_STRING: {
        const Om_VM_String_Object *string = (const Om_VM_String_Object *)source;
        if (!om_checked_size_add(offsetof(Om_VM_String_Object, bytes),
                                 string->length, &size)
            || !om_checked_size_add(size, 1, &size)) {
            context->status = OM_STATUS_OVERFLOW;
            return NULL;
        }
        Om_VM_String_Object *copy = (Om_VM_String_Object *)
            om_vm_heap_allocate(context->destination, size,
                                OM_VM_OBJECT_STRING);
        if (copy != NULL) {
            copy->length = string->length;
            (void)memcpy(copy->bytes, string->bytes, string->length + 1);
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_LIST: {
        const Om_VM_List_Object *list = (const Om_VM_List_Object *)source;
        size_t item_bytes = 0;
        if (!om_checked_size_mul(list->length, sizeof(Om_VM_Value),
                                 &item_bytes)
            || !om_checked_size_add(offsetof(Om_VM_List_Object, items),
                                   item_bytes, &size)) {
            context->status = OM_STATUS_OVERFLOW;
            return NULL;
        }
        Om_VM_List_Object *copy = (Om_VM_List_Object *)
            om_vm_heap_allocate(context->destination, size,
                                OM_VM_OBJECT_LIST);
        if (copy != NULL) {
            copy->length = list->length;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_MAP: {
        const Om_VM_Map_Object *map = (const Om_VM_Map_Object *)source;
        size_t entry_bytes = 0;
        if (!om_checked_size_mul(map->length, sizeof(Om_VM_Map_Entry),
                                 &entry_bytes)
            || !om_checked_size_add(offsetof(Om_VM_Map_Object, entries),
                                   entry_bytes, &size)) {
            context->status = OM_STATUS_OVERFLOW;
            return NULL;
        }
        Om_VM_Map_Object *copy = (Om_VM_Map_Object *)
            om_vm_heap_allocate(context->destination, size,
                                OM_VM_OBJECT_MAP);
        if (copy != NULL) {
            copy->length = map->length;
            copy->capacity = map->length;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_RECORD:
    case OM_VM_OBJECT_MESSAGE: {
        const Om_VM_Record_Object *record =
            (const Om_VM_Record_Object *)source;
        size_t field_bytes = 0;
        if (!om_checked_size_mul(record->field_count, sizeof(Om_VM_Value),
                                 &field_bytes)
            || !om_checked_size_add(offsetof(Om_VM_Record_Object, fields),
                                   field_bytes, &size)) {
            context->status = OM_STATUS_OVERFLOW;
            return NULL;
        }
        Om_VM_Record_Object *copy = (Om_VM_Record_Object *)
            om_vm_heap_allocate(context->destination, size, source->kind);
        if (copy != NULL) {
            copy->schema_index = record->schema_index;
            copy->field_count = record->field_count;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_RESULT: {
        Om_VM_Result_Object *copy = (Om_VM_Result_Object *)
            om_vm_heap_allocate(context->destination,
                                sizeof(Om_VM_Result_Object),
                                OM_VM_OBJECT_RESULT);
        if (copy != NULL) {
            copy->is_ok = ((Om_VM_Result_Object *)source)->is_ok;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_FUNCTION: {
        const uint32_t function_index =
            ((Om_VM_Function_Object *)source)->function_index;
        Om_VM_Function_Object *copy = (Om_VM_Function_Object *)
            om_vm_heap_allocate(context->destination,
                                sizeof(Om_VM_Function_Object),
                                OM_VM_OBJECT_FUNCTION);
        if (copy != NULL) {
            copy->function_index = function_index;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    case OM_VM_OBJECT_ITERATOR: {
        const Om_VM_Iterator_Object *iterator =
            (const Om_VM_Iterator_Object *)source;
        Om_VM_Iterator_Object *copy = (Om_VM_Iterator_Object *)
            om_vm_heap_allocate(context->destination,
                                sizeof(Om_VM_Iterator_Object),
                                OM_VM_OBJECT_ITERATOR);
        if (copy != NULL) {
            copy->kind = iterator->kind;
            copy->next_index = iterator->next_index;
        }
        return copy == NULL ? NULL : &copy->base;
    }
    }
    context->status = OM_STATUS_INVALID_ARGUMENT;
    return NULL;
}

static bool clone_value_shallow(Om_VM_Clone_Context *context,
                                Om_VM_Value source, Om_VM_Value *copy_value)
{
    if (source.tag != OM_VM_VALUE_OBJECT) {
        *copy_value = source;
        return true;
    }
    if (!object_belongs_to_vm(context->vm, source.as.object)) {
        context->status = OM_STATUS_INVALID_ARGUMENT;
        return false;
    }
    for (size_t index = 0; index < context->count; ++index) {
        if (context->sources[index] == source.as.object) {
            *copy_value = (Om_VM_Value){
                .tag = OM_VM_VALUE_OBJECT,
                .as.object = context->copies[index]
            };
            return true;
        }
    }
    if (!clone_context_reserve(context)) {
        return false;
    }
    Om_VM_Object *object = clone_object_shell(context, source.as.object);
    if (object == NULL) {
        if (context->status == OM_STATUS_OK) {
            context->status = context->destination->last_status;
        }
        return false;
    }
    context->sources[context->count] = source.as.object;
    context->copies[context->count] = object;
    context->count += 1;
    *copy_value = (Om_VM_Value){.tag = OM_VM_VALUE_OBJECT, .as.object = object};
    return true;
}

static Om_Status clone_message_into_heap(Om_VM_State *state,
                                        Om_VM_Heap *destination_heap,
                                        Om_VM_Value message,
                                        Om_VM_Value *copy)
{
    if (destination_heap == NULL || message.tag != OM_VM_VALUE_OBJECT
        || message.as.object == NULL
        || message.as.object->kind != OM_VM_OBJECT_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Clone_Context context = {
        .vm = state,
        .destination = destination_heap,
        .status = OM_STATUS_OK
    };
    if (!clone_value_shallow(&context, message, copy)) {
        om_free(context.sources);
        om_free(context.copies);
        return context.status;
    }
    for (size_t cursor = 0; cursor < context.count; ++cursor) {
        Om_VM_Object *source = context.sources[cursor];
        Om_VM_Object *destination = context.copies[cursor];
        switch (source->kind) {
        case OM_VM_OBJECT_LIST: {
            Om_VM_List_Object *source_list = (Om_VM_List_Object *)source;
            Om_VM_List_Object *copy_list = (Om_VM_List_Object *)destination;
            for (size_t index = 0; index < source_list->length; ++index) {
                if (!clone_value_shallow(&context, source_list->items[index],
                                         &copy_list->items[index])) {
                    goto clone_failed;
                }
            }
            break;
        }
        case OM_VM_OBJECT_MAP: {
            Om_VM_Map_Object *source_map = (Om_VM_Map_Object *)source;
            Om_VM_Map_Object *copy_map = (Om_VM_Map_Object *)destination;
            for (size_t index = 0; index < source_map->length; ++index) {
                if (!clone_value_shallow(&context,
                                         source_map->entries[index].key,
                                         &copy_map->entries[index].key)
                    || !clone_value_shallow(&context,
                                            source_map->entries[index].value,
                                            &copy_map->entries[index].value)) {
                    goto clone_failed;
                }
            }
            break;
        }
        case OM_VM_OBJECT_RECORD:
        case OM_VM_OBJECT_MESSAGE: {
            Om_VM_Record_Object *source_record =
                (Om_VM_Record_Object *)source;
            Om_VM_Record_Object *copy_record =
                (Om_VM_Record_Object *)destination;
            for (size_t index = 0; index < source_record->field_count; ++index) {
                if (!clone_value_shallow(&context, source_record->fields[index],
                                         &copy_record->fields[index])) {
                    goto clone_failed;
                }
            }
            break;
        }
        case OM_VM_OBJECT_RESULT: {
            Om_VM_Result_Object *source_result =
                (Om_VM_Result_Object *)source;
            Om_VM_Result_Object *copy_result =
                (Om_VM_Result_Object *)destination;
            if (!clone_value_shallow(&context, source_result->payload,
                                     &copy_result->payload)) {
                goto clone_failed;
            }
            break;
        }
        case OM_VM_OBJECT_ITERATOR: {
            Om_VM_Iterator_Object *source_iterator =
                (Om_VM_Iterator_Object *)source;
            Om_VM_Iterator_Object *copy_iterator =
                (Om_VM_Iterator_Object *)destination;
            Om_VM_Value collection = {
                .tag = OM_VM_VALUE_OBJECT,
                .as.object = source_iterator->collection
            };
            Om_VM_Value copied_collection = {0};
            if (!clone_value_shallow(&context, collection,
                                     &copied_collection)) {
                goto clone_failed;
            }
            copy_iterator->collection = copied_collection.as.object;
            break;
        }
        case OM_VM_OBJECT_STRING:
        case OM_VM_OBJECT_FUNCTION:
            break;
        }
    }
    om_free(context.sources);
    om_free(context.copies);
    return OM_STATUS_OK;

clone_failed:
    om_free(context.sources);
    om_free(context.copies);
    return context.status;
}

static bool enqueue_actor_message(Om_VM_State *state,
                                 Om_VM_Actor_State *actor,
                                 Om_VM_Value message,
                                 Om_VM_Execution_Result *result,
                                 uint32_t function_index, uint32_t pc)
{
    Om_VM_Message_Node *node = om_alloc(sizeof(*node));
    if (node == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        return false;
    }
    Om_VM_State *shared = shared_vm(state);
    *node = (Om_VM_Message_Node){
        .transit_heap = {
            .vm = shared,
            .max_bytes = shared->limits.max_heap_bytes,
            .gc_threshold = SIZE_MAX,
            .last_status = OM_STATUS_OK
        }
    };
    Om_VM_Value copied_message = {0};
    const Om_Status clone_status = clone_message_into_heap(
        state, &node->transit_heap, message, &copied_message);
    if (clone_status != OM_STATUS_OK) {
        om_vm_heap_destroy(&node->transit_heap);
        om_free(node);
        set_runtime_error(state, result,
                          error_for_allocation_status(clone_status),
                          function_index, pc);
        return false;
    }
    node->message = copied_message;

    scheduler_mutex_lock(state);
    if (actor->stopped) {
        scheduler_mutex_unlock(state);
        om_vm_heap_destroy(&node->transit_heap);
        om_free(node);
        return true;
    }
    if (shared->mailbox_message_count
        >= shared->limits.max_mailbox_messages) {
        scheduler_mutex_unlock(state);
        om_vm_heap_destroy(&node->transit_heap);
        om_free(node);
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT,
                          function_index, pc);
        return false;
    }
    if (!actor->scheduled && !actor->running) {
        const Om_Status status = reserve_ready_queue(
            shared, shared->ready_length + 1);
        if (status != OM_STATUS_OK) {
            scheduler_mutex_unlock(state);
            om_vm_heap_destroy(&node->transit_heap);
            om_free(node);
            set_runtime_error(state, result,
                status == OM_STATUS_OUT_OF_MEMORY
                    ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                    : OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
                function_index, pc);
            return false;
        }
    }
    if (actor->mailbox_tail == NULL) {
        actor->mailbox_head = node;
    } else {
        actor->mailbox_tail->next = node;
    }
    actor->mailbox_tail = node;
    actor->mailbox_count += 1;
    shared->mailbox_message_count += 1;
    if (!actor->scheduled && !actor->running
        && !schedule_actor_locked(shared, actor, result,
                                  function_index, pc)) {
        scheduler_mutex_unlock(state);
        return false;
    }
    scheduler_mutex_unlock(state);
    return true;
}

static void record_async_failure(Om_VM_State *shared,
                                 Om_VM_Runtime_Error_Code code,
                                 uint32_t function_index,
                                 uint32_t instruction_index)
{
    scheduler_mutex_lock(shared);
    if (shared->async_error_code == OM_VM_RUNTIME_ERROR_NONE) {
        shared->async_error_code = code;
        shared->async_error_function = function_index;
        shared->async_error_instruction = instruction_index;
    }
    atomic_store_explicit(&shared->scheduler_failed, true,
                          memory_order_release);
    scheduler_condition_broadcast(shared);
    scheduler_mutex_unlock(shared);
}

static Om_Status run_native_call_task(void *opaque)
{
    Om_VM_Native_Call_Task *task = opaque;
    if (task == NULL || task->binding == NULL
        || (task->parameter_count != 0 && task->arguments == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = om_native_call_invoke(
        task->binding, task->arguments, task->parameter_count,
        &task->result);
    Om_Status callback_status = OM_STATUS_OK;
    for (size_t index = 0; index < task->parameter_count; ++index) {
        if (task->closures != NULL) {
            om_native_callback_closure_destroy(&task->closures[index]);
        }
        if (task->callback_contexts != NULL
            && task->callback_contexts[index].initialized) {
            const Om_Status entry_status = (Om_Status)atomic_load_explicit(
                &task->callback_contexts[index].status,
                memory_order_acquire);
            if (entry_status != OM_STATUS_OK
                && callback_status == OM_STATUS_OK) {
                callback_status = entry_status;
            }
        }
    }
    if (status == OM_STATUS_OK && callback_status != OM_STATUS_OK) {
        status = callback_status;
    }
    return status;
}

static void complete_native_call_task(void *opaque, Om_Status status)
{
    Om_VM_Native_Call_Task *task = opaque;
    if (task == NULL || task->shared == NULL) {
        return;
    }
    Om_VM_State *shared = task->shared;
    Om_VM_Runtime_Error_Code scheduling_error = OM_VM_RUNTIME_ERROR_NONE;
    scheduler_mutex_lock(shared);
    Om_VM_Actor_State *actor = find_actor(shared, task->actor_id);
    if (actor == NULL || !actor->waiting_native_call
        || actor->native_call_completion_ready) {
        scheduling_error = OM_VM_RUNTIME_ERROR_INVALID_BYTECODE;
    } else if (!atomic_load_explicit(&shared->scheduler_failed,
                                    memory_order_acquire)) {
        actor->native_call_status = status;
        actor->native_call_binding = task->binding;
        actor->native_call_function = task->caller_function;
        actor->native_call_instruction = task->caller_instruction;
        actor->native_call_result = task->result;
        task->result = (Om_Native_Call_Result){0};
        actor->native_call_completion_ready = true;
        actor->waiting_native_call = false;
        Om_VM_Execution_Result scheduling_result = {.succeeded = true};
        if (!schedule_actor_locked(shared, actor, &scheduling_result,
                                   task->caller_function,
                                   task->caller_instruction)) {
            scheduling_error = scheduling_result.error.code
                == OM_VM_RUNTIME_ERROR_NONE
                ? OM_VM_RUNTIME_ERROR_ACTOR_LIMIT
                : scheduling_result.error.code;
        }
    } else {
        actor->waiting_native_call = false;
    }
    if (shared->pending_external_tasks != 0) {
        shared->pending_external_tasks -= 1;
    }
    scheduler_condition_broadcast(shared);
    scheduler_mutex_unlock(shared);
    if (scheduling_error != OM_VM_RUNTIME_ERROR_NONE) {
        record_async_failure(shared, scheduling_error,
                             task->caller_function,
                             task->caller_instruction);
    }
}

static void destroy_native_call_task(void *opaque)
{
    Om_VM_Native_Call_Task *task = opaque;
    if (task == NULL) {
        return;
    }
    for (size_t index = 0; index < task->parameter_count; ++index) {
        if (task->closures != NULL) {
            om_native_callback_closure_destroy(&task->closures[index]);
        }
    }
    if (task->result.ownership == OM_NATIVE_OWNERSHIP_OWNED) {
        (void)om_native_call_result_release_bound(task->binding,
                                                  &task->result);
    }
    om_native_call_result_destroy(&task->result);
    om_free(task->arguments);
    om_free(task->closures);
    om_free(task->callback_contexts);
    om_free(task);
}

static void publish_async_failure(Om_VM_State *state,
                                  Om_VM_Execution_Result *result,
                                  uint32_t fallback_function)
{
    Om_VM_State *shared = shared_vm(state);
    scheduler_mutex_lock(shared);
    const Om_VM_Runtime_Error_Code code = shared->async_error_code;
    const uint32_t function_index = shared->async_error_function;
    const uint32_t instruction_index = shared->async_error_instruction;
    scheduler_mutex_unlock(shared);
    if (code != OM_VM_RUNTIME_ERROR_NONE
        && result->error.code == OM_VM_RUNTIME_ERROR_NONE) {
        set_runtime_error(state, result, code,
                          function_index == UINT32_MAX
                              ? fallback_function : function_index,
                          instruction_index);
    }
}

static Om_Status enqueue_owned_message(Om_VM_State *shared,
                                       uint64_t actor_id,
                                       Om_VM_Message_Node *node,
                                       uint32_t function_index,
                                       uint32_t instruction_index,
                                       bool require_active_execution)
{
    if (shared == NULL || node == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    scheduler_mutex_lock(shared);
    if (require_active_execution
        && (!atomic_load_explicit(&shared->execution_active,
                                  memory_order_acquire)
            || atomic_load_explicit(&shared->scheduler_done,
                                    memory_order_acquire)
            || atomic_load_explicit(&shared->scheduler_failed,
                                    memory_order_acquire))) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_CANCELLED;
    }
    Om_VM_Actor_State *actor = find_actor(shared, actor_id);
    if (actor == NULL) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_NOT_FOUND;
    }
    if (actor->stopped) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_CANCELLED;
    }
    if (shared->mailbox_message_count >= shared->limits.max_mailbox_messages) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_OUT_OF_RANGE;
    }
    const bool schedule = !actor->scheduled && !actor->running;
    if (schedule) {
        const Om_Status queue_status = reserve_ready_queue(
            shared, shared->ready_length + 1);
        if (queue_status != OM_STATUS_OK) {
            scheduler_mutex_unlock(shared);
            return queue_status;
        }
    }
    node->next = NULL;
    if (actor->mailbox_tail == NULL) {
        actor->mailbox_head = node;
    } else {
        actor->mailbox_tail->next = node;
    }
    actor->mailbox_tail = node;
    actor->mailbox_count += 1;
    shared->mailbox_message_count += 1;
    if (schedule) {
        const size_t tail = (shared->ready_head + shared->ready_length)
            % shared->ready_capacity;
        shared->ready_queue[tail] = actor->id;
        shared->ready_length += 1;
        actor->scheduled = true;
    }
    scheduler_condition_broadcast(shared);
    scheduler_mutex_unlock(shared);
    (void)function_index;
    (void)instruction_index;
    return OM_STATUS_OK;
}

Om_Status om_vm_distribution_enqueue_message(
    Om_VM_State *state,
    uint64_t actor_id,
    Om_VM_Message_Node *node)
{
    return enqueue_owned_message(state, actor_id, node, UINT32_MAX, 0, false);
}

static void finish_external_task(Om_VM_State *shared)
{
    scheduler_mutex_lock(shared);
    if (shared->pending_external_tasks != 0) {
        shared->pending_external_tasks -= 1;
    }
    scheduler_condition_broadcast(shared);
    scheduler_mutex_unlock(shared);
}

typedef enum Om_VM_System_Task_Kind {
    OM_VM_SYSTEM_TASK_CONSOLE_WRITE = 0,
    OM_VM_SYSTEM_TASK_FILE_READ,
    OM_VM_SYSTEM_TASK_FILE_WRITE
} Om_VM_System_Task_Kind;

typedef struct Om_VM_System_Task {
    Om_VM_State *shared;
    Om_VM_System_Task_Kind kind;
    uint64_t reply_to;
    uint32_t completion_schema;
    uint32_t function_index;
    uint32_t instruction_index;
    char *path;
    char *input;
    size_t input_length;
    char *output;
    size_t output_length;
    size_t max_file_bytes;
} Om_VM_System_Task;

typedef struct Om_VM_Timer_Context {
    Om_VM_State *shared;
    uint64_t recipient;
    uint32_t function_index;
    uint32_t instruction_index;
    Om_VM_Message_Node *message_node;
} Om_VM_Timer_Context;

static FILE *open_native_file(const char *path, const char *mode)
{
#if defined(_WIN32)
    const size_t path_bytes = strlen(path);
    if (path_bytes >= (size_t)INT_MAX) {
        return NULL;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (required <= 0) {
        return NULL;
    }
    wchar_t *wide_path = om_alloc_array((size_t)required,
                                        sizeof(*wide_path));
    if (wide_path == NULL) {
        return NULL;
    }
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, required);
    if (converted != required) {
        om_free(wide_path);
        return NULL;
    }
    const wchar_t *wide_mode = strcmp(mode, "rb") == 0 ? L"rb"
        : strcmp(mode, "wb") == 0 ? L"wb" : NULL;
    FILE *file = NULL;
    const errno_t status = wide_mode != NULL
        ? _wfopen_s(&file, wide_path, wide_mode) : EINVAL;
    om_free(wide_path);
    return status == 0 ? file : NULL;
#else
    return fopen(path, mode);
#endif
}

static char *copy_native_text(Om_String_View text, bool reject_nul)
{
    if ((text.length != 0 && text.data == NULL)
        || text.length == SIZE_MAX
        || (reject_nul && text.length != 0
            && memchr(text.data, '\0', text.length) != NULL)) {
        return NULL;
    }
    char *copy = om_alloc(text.length + 1);
    if (copy == NULL) {
        return NULL;
    }
    if (text.length != 0) {
        (void)memcpy(copy, text.data, text.length);
    }
    copy[text.length] = '\0';
    return copy;
}

static Om_Status run_system_task(void *opaque)
{
    Om_VM_System_Task *task = opaque;
    if (task->kind == OM_VM_SYSTEM_TASK_CONSOLE_WRITE) {
        const size_t written = fwrite(task->input, 1, task->input_length,
                                      stdout);
        return written == task->input_length && fflush(stdout) == 0
            ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
    }
    if (task->kind == OM_VM_SYSTEM_TASK_FILE_WRITE) {
        FILE *file = open_native_file(task->path, "wb");
        if (file == NULL) {
            return OM_STATUS_IO_ERROR;
        }
        Om_Status status = OM_STATUS_OK;
        if (fwrite(task->input, 1, task->input_length, file)
                != task->input_length
            || fflush(file) != 0) {
            status = OM_STATUS_IO_ERROR;
        }
        if (fclose(file) != 0) {
            status = OM_STATUS_IO_ERROR;
        }
        return status;
    }

    FILE *file = open_native_file(task->path, "rb");
    if (file == NULL) {
        return OM_STATUS_IO_ERROR;
    }
    Om_Status status = OM_STATUS_OK;
    if (fseek(file, 0, SEEK_END) != 0) {
        status = OM_STATUS_IO_ERROR;
    }
    long file_length = status == OM_STATUS_OK ? ftell(file) : -1;
    if (file_length < 0) {
        status = OM_STATUS_IO_ERROR;
    }
    if (status == OM_STATUS_OK
        && (uintmax_t)file_length > (uintmax_t)task->max_file_bytes) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    if (status == OM_STATUS_OK && fseek(file, 0, SEEK_SET) != 0) {
        status = OM_STATUS_IO_ERROR;
    }
    if (status == OM_STATUS_OK) {
        task->output_length = (size_t)file_length;
        task->output = om_alloc(task->output_length + 1);
        if (task->output == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else if (fread(task->output, 1, task->output_length, file)
                   != task->output_length) {
            status = OM_STATUS_IO_ERROR;
        } else {
            task->output[task->output_length] = '\0';
            status = om_utf8_validate((Om_Const_Byte_Slice){
                (const uint8_t *)task->output, task->output_length
            });
        }
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        status = OM_STATUS_IO_ERROR;
    }
    return status;
}

static void destroy_system_task(void *opaque)
{
    Om_VM_System_Task *task = opaque;
    if (task == NULL) {
        return;
    }
    om_free(task->path);
    om_free(task->input);
    om_free(task->output);
    om_free(task);
}

static Om_VM_Message_Node *create_message_node(Om_VM_State *shared)
{
    Om_VM_Message_Node *node = om_alloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    *node = (Om_VM_Message_Node){
        .transit_heap = {
            .vm = shared,
            .max_bytes = shared->limits.max_heap_bytes,
            .gc_threshold = SIZE_MAX,
            .last_status = OM_STATUS_OK
        }
    };
    return node;
}

Om_VM_Message_Node *om_vm_distribution_message_node_create(
    Om_VM_State *state)
{
    return create_message_node(shared_vm(state));
}

static void destroy_message_node(Om_VM_Message_Node *node)
{
    if (node == NULL) {
        return;
    }
    om_vm_heap_destroy(&node->transit_heap);
    om_free(node);
}

void om_vm_distribution_message_node_destroy(Om_VM_Message_Node *node)
{
    destroy_message_node(node);
}

Om_Status om_vm_native_callback_post(Om_VM *vm,
                                     uint64_t recipient_actor_id,
                                     Om_Symbol_Id message_symbol,
                                     const Om_VM_Value *fields,
                                     size_t field_count)
{
    if (vm == NULL || vm->owned_state == NULL || recipient_actor_id == 0
        || message_symbol.value == UINT32_MAX
        || (field_count != 0 && fields == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *shared = vm->owned_state;
    if (!atomic_load_explicit(&shared->execution_active,
                              memory_order_acquire)) {
        return OM_STATUS_CANCELLED;
    }

    uint32_t schema_index = UINT32_MAX;
    for (size_t index = 0; index < shared->image->schema_count; ++index) {
        const Om_Bytecode_Schema *schema = &shared->image->schemas[index];
        if (schema->symbol_id.value == message_symbol.value
            && schema->kind == OM_BYTECODE_SCHEMA_MESSAGE) {
            if (schema->field_count != field_count
                || index > UINT32_MAX) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            schema_index = (uint32_t)index;
            break;
        }
    }
    if (schema_index == UINT32_MAX) {
        return OM_STATUS_NOT_FOUND;
    }
    for (size_t index = 0; index < field_count; ++index) {
        switch (fields[index].tag) {
        case OM_VM_VALUE_BOOL:
        case OM_VM_VALUE_INT:
        case OM_VM_VALUE_FLOAT:
            break;
        default:
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }

    Om_VM_Message_Node *node = create_message_node(shared);
    if (node == NULL) {
        record_async_failure(shared, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                             UINT32_MAX, 0);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Status build_status = om_vm_heap_make_record(
        &node->transit_heap, OM_VM_OBJECT_MESSAGE, schema_index, fields,
        field_count, &node->message);
    if (build_status != OM_STATUS_OK) {
        destroy_message_node(node);
        record_async_failure(shared, error_for_allocation_status(build_status),
                             UINT32_MAX, 0);
        return build_status;
    }

    const Om_Status enqueue_status = enqueue_owned_message(
        shared, recipient_actor_id, node, UINT32_MAX, 0, true);
    if (enqueue_status != OM_STATUS_OK) {
        destroy_message_node(node);
        if (enqueue_status == OM_STATUS_OUT_OF_RANGE
            || enqueue_status == OM_STATUS_OUT_OF_MEMORY) {
            record_async_failure(shared,
                enqueue_status == OM_STATUS_OUT_OF_RANGE
                    ? OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT
                    : OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                UINT32_MAX, 0);
        }
    }
    return enqueue_status;
}

static uint32_t find_system_schema(const Om_VM_State *state,
                                   uint32_t symbol_id,
                                   uint32_t field_count)
{
    for (size_t index = 0; index < state->image->schema_count; ++index) {
        const Om_Bytecode_Schema *schema = &state->image->schemas[index];
        if (schema->symbol_id.value == symbol_id
            && schema->kind == OM_BYTECODE_SCHEMA_MESSAGE
            && schema->field_count == field_count) {
            return (uint32_t)index;
        }
    }
    return UINT32_MAX;
}

static Om_VM_Runtime_Error_Code async_error_for_status(Om_Status status)
{
    if (status == OM_STATUS_OUT_OF_MEMORY) {
        return OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OUT_OF_RANGE) {
        return OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT;
    }
    if (status == OM_STATUS_NOT_FOUND || status == OM_STATUS_CANCELLED) {
        return OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF;
    }
    return OM_VM_RUNTIME_ERROR_INVALID_BYTECODE;
}

static bool enqueue_async_message(Om_VM_State *shared,
                                 uint64_t recipient,
                                 Om_VM_Message_Node *node,
                                 uint32_t function_index,
                                 uint32_t instruction_index)
{
    const Om_Status status = enqueue_owned_message(
        shared, recipient, node, function_index, instruction_index, false);
    if (status == OM_STATUS_OK) {
        return true;
    }
    if (status != OM_STATUS_CANCELLED) {
        record_async_failure(shared, async_error_for_status(status),
                             function_index, instruction_index);
    }
    return false;
}

static void complete_system_task(void *opaque, Om_Status status)
{
    Om_VM_System_Task *task = opaque;
    Om_VM_State *shared = task->shared;
    if (!atomic_load_explicit(&shared->scheduler_failed,
                              memory_order_acquire)) {
        Om_VM_Message_Node *node = create_message_node(shared);
        if (node == NULL) {
            record_async_failure(shared, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                                 task->function_index,
                                 task->instruction_index);
        } else {
            Om_VM_Value payload = om_vm_value_unit();
            Om_Status build_status = OM_STATUS_OK;
            if (status == OM_STATUS_OK
                && task->kind == OM_VM_SYSTEM_TASK_FILE_READ) {
                build_status = om_vm_heap_make_string(
                    &node->transit_heap,
                    (Om_String_View){task->output, task->output_length},
                    &payload);
            } else if (status != OM_STATUS_OK) {
                const char *description = om_status_name(status);
                build_status = om_vm_heap_make_string(
                    &node->transit_heap,
                    (Om_String_View){description, strlen(description)},
                    &payload);
            }
            Om_VM_Value result_value = {0};
            if (build_status == OM_STATUS_OK) {
                build_status = om_vm_heap_make_result(
                    &node->transit_heap, status == OM_STATUS_OK, payload,
                    &result_value);
            }
            if (build_status == OM_STATUS_OK) {
                build_status = om_vm_heap_make_record(
                    &node->transit_heap, OM_VM_OBJECT_MESSAGE,
                    task->completion_schema, &result_value, 1,
                    &node->message);
            }
            if (build_status != OM_STATUS_OK) {
                destroy_message_node(node);
                record_async_failure(shared,
                    async_error_for_status(build_status),
                    task->function_index, task->instruction_index);
            } else if (!enqueue_async_message(
                           shared, task->reply_to, node,
                           task->function_index, task->instruction_index)) {
                destroy_message_node(node);
            }
        }
    }
    finish_external_task(shared);
}

static Om_Status reserve_external_task(Om_VM_State *shared,
                                       bool timer,
                                       void **service_out)
{
    scheduler_mutex_lock(shared);
    void **service_slot = timer
        ? (void **)&shared->timer_service
        : (void **)&shared->blocking_pool;
    if (*service_slot == NULL) {
        Om_Status status = timer
            ? om_timer_service_create((Om_Timer_Service **)service_slot,
                                     shared->limits.max_mailbox_messages)
            : om_blocking_pool_create((Om_Blocking_Pool **)service_slot,
                                      shared->limits.worker_count,
                                      shared->limits.max_mailbox_messages);
        if (status != OM_STATUS_OK) {
            scheduler_mutex_unlock(shared);
            return status;
        }
    }
    if (shared->pending_external_tasks == SIZE_MAX) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_OUT_OF_RANGE;
    }
    shared->pending_external_tasks += 1;
    *service_out = *service_slot;
    scheduler_mutex_unlock(shared);
    return OM_STATUS_OK;
}

static Om_Status submit_system_task(Om_VM_State *shared,
                                    Om_VM_System_Task *task)
{
    void *pool = NULL;
    Om_Status status = reserve_external_task(shared, false, &pool);
    if (status != OM_STATUS_OK) {
        return status;
    }
    status = om_blocking_pool_submit((Om_Blocking_Pool *)pool,
        (Om_Blocking_Task){
            .run = run_system_task,
            .complete = complete_system_task,
            .destroy = destroy_system_task,
            .context = task
        });
    if (status != OM_STATUS_OK) {
        finish_external_task(shared);
    }
    return status;
}

static void complete_timer(void *opaque, Om_Status status)
{
    Om_VM_Timer_Context *context = opaque;
    Om_VM_State *shared = context->shared;
    if (status == OM_STATUS_OK
        && !atomic_load_explicit(&shared->scheduler_failed,
                                 memory_order_acquire)) {
        const bool transferred = enqueue_async_message(
            shared, context->recipient, context->message_node,
            context->function_index, context->instruction_index);
        if (transferred) {
            context->message_node = NULL;
        }
    }
    finish_external_task(shared);
}

static void destroy_timer_context(void *opaque)
{
    Om_VM_Timer_Context *context = opaque;
    if (context == NULL) {
        return;
    }
    destroy_message_node(context->message_node);
    om_free(context);
}

static Om_Status submit_timer(Om_VM_State *shared,
                              Om_VM_Timer_Context *context,
                              uint64_t delay_milliseconds)
{
    void *service = NULL;
    Om_Status status = reserve_external_task(shared, true, &service);
    if (status != OM_STATUS_OK) {
        return status;
    }
    uint64_t timer_id = 0;
    status = om_timer_service_schedule((Om_Timer_Service *)service,
        (Om_Timer_Task){
            .delay_milliseconds = delay_milliseconds,
            .complete = complete_timer,
            .destroy = destroy_timer_context,
            .context = context
        }, &timer_id);
    if (status != OM_STATUS_OK) {
        finish_external_task(shared);
    }
    return status;
}

static bool system_message_record(Om_VM_State *state,
                                  Om_VM_Value message_value,
                                  uint32_t symbol_id,
                                  size_t expected_fields,
                                  Om_VM_Record_Object **record_out)
{
    if (message_value.tag != OM_VM_VALUE_OBJECT
        || !object_belongs_to_vm(state, message_value.as.object)
        || message_value.as.object->kind != OM_VM_OBJECT_MESSAGE) {
        return false;
    }
    Om_VM_Record_Object *record =
        (Om_VM_Record_Object *)message_value.as.object;
    if (record->schema_index >= state->image->schema_count
        || state->image->schemas[record->schema_index].symbol_id.value
            != symbol_id
        || record->field_count != expected_fields) {
        return false;
    }
    *record_out = record;
    return true;
}

static bool actor_ref_field(Om_VM_State *state,
                            Om_VM_Value value,
                            uint64_t *actor_id)
{
    return om_vm_value_as_actor_ref(value, actor_id)
        && find_actor(state, *actor_id) != NULL;
}

static bool submit_native_request(Om_VM_State *state,
                                  Om_VM_System_Task *task,
                                  Om_VM_Execution_Result *result,
                                  uint32_t function_index,
                                  uint32_t instruction_index)
{
    Om_Status status = submit_system_task(shared_vm(state), task);
    if (status == OM_STATUS_OK) {
        return true;
    }
    destroy_system_task(task);
    set_runtime_error(state, result,
        status == OM_STATUS_OUT_OF_MEMORY
            ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
            : status == OM_STATUS_OUT_OF_RANGE
                ? OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT
                : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
        function_index, instruction_index);
    return false;
}

static bool submit_system_actor_message(Om_VM_State *state,
                                        uint64_t system_actor_id,
                                        Om_VM_Value message,
                                        Om_VM_Execution_Result *result,
                                        uint32_t function_index,
                                        uint32_t instruction_index)
{
    Om_VM_State *shared = shared_vm(state);
    Om_VM_Record_Object *request = NULL;
    if (system_actor_id == OM_SYSTEM_ACTOR_REF_CONSOLE) {
        if (!system_message_record(state, message,
                OM_SYSTEM_MESSAGE_CONSOLE_WRITE, 2, &request)) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE,
                              function_index, instruction_index);
            return false;
        }
        Om_String_View text_view = {0};
        uint64_t reply_to = 0;
        if (!om_vm_value_as_string(request->fields[0], &text_view)
            || !actor_ref_field(state, request->fields[1], &reply_to)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, instruction_index);
            return false;
        }
        const uint32_t completion_schema = find_system_schema(
            state, OM_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED, 1);
        if (completion_schema == UINT32_MAX) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, instruction_index);
            return false;
        }
        Om_VM_System_Task *task = om_alloc(sizeof(*task));
        if (task == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        *task = (Om_VM_System_Task){
            .shared = shared,
            .kind = OM_VM_SYSTEM_TASK_CONSOLE_WRITE,
            .reply_to = reply_to,
            .completion_schema = completion_schema,
            .function_index = function_index,
            .instruction_index = instruction_index,
            .input = copy_native_text(text_view, false),
            .input_length = text_view.length
        };
        if (task->input == NULL) {
            destroy_system_task(task);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        return submit_native_request(state, task, result, function_index,
                                     instruction_index);
    }

    if (system_actor_id == OM_SYSTEM_ACTOR_REF_FILE_SYSTEM) {
        Om_String_View path_view = {0};
        Om_String_View contents_view = {0};
        uint64_t reply_to = 0;
        Om_VM_System_Task_Kind kind = OM_VM_SYSTEM_TASK_FILE_READ;
        uint32_t completion_schema = UINT32_MAX;
        if (system_message_record(state, message,
                                  OM_SYSTEM_MESSAGE_FILE_READ, 2, &request)) {
            if (!om_vm_value_as_string(request->fields[0], &path_view)
                || !actor_ref_field(state, request->fields[1], &reply_to)) {
                set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                                  function_index, instruction_index);
                return false;
            }
            completion_schema = find_system_schema(
                state, OM_SYSTEM_MESSAGE_FILE_READ_COMPLETED, 1);
        } else if (system_message_record(state, message,
                                         OM_SYSTEM_MESSAGE_FILE_WRITE, 3,
                                         &request)) {
            kind = OM_VM_SYSTEM_TASK_FILE_WRITE;
            if (!om_vm_value_as_string(request->fields[0], &path_view)
                || !om_vm_value_as_string(request->fields[1], &contents_view)
                || !actor_ref_field(state, request->fields[2], &reply_to)) {
                set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                                  function_index, instruction_index);
                return false;
            }
            completion_schema = find_system_schema(
                state, OM_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED, 1);
        } else {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE,
                              function_index, instruction_index);
            return false;
        }
        if ((path_view.length != 0 && path_view.data == NULL)
            || (path_view.length != 0
                && memchr(path_view.data, '\0', path_view.length) != NULL)) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, instruction_index);
            return false;
        }
        if (completion_schema == UINT32_MAX) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, instruction_index);
            return false;
        }
        Om_VM_System_Task *task = om_alloc(sizeof(*task));
        if (task == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        *task = (Om_VM_System_Task){
            .shared = shared,
            .kind = kind,
            .reply_to = reply_to,
            .completion_schema = completion_schema,
            .function_index = function_index,
            .instruction_index = instruction_index,
            .path = copy_native_text(path_view, true),
                    .max_file_bytes = shared->limits.max_heap_bytes / 2
        };
        if (kind == OM_VM_SYSTEM_TASK_FILE_WRITE) {
            task->input = copy_native_text(contents_view, false);
            task->input_length = contents_view.length;
        }
        if (task->path == NULL
            || (kind == OM_VM_SYSTEM_TASK_FILE_WRITE && task->input == NULL)) {
            destroy_system_task(task);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        return submit_native_request(state, task, result, function_index,
                                     instruction_index);
    }

    if (system_actor_id == OM_SYSTEM_ACTOR_REF_TIMER) {
        if (!system_message_record(state, message,
                OM_SYSTEM_MESSAGE_TIMER_AFTER, 3, &request)) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE,
                              function_index, instruction_index);
            return false;
        }
        int64_t delay = 0;
        uint64_t recipient = 0;
        if (!om_vm_value_as_int(request->fields[0], &delay)
            || delay < 0
            || !actor_ref_field(state, request->fields[2], &recipient)
            || request->fields[1].tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, request->fields[1].as.object)
            || request->fields[1].as.object->kind != OM_VM_OBJECT_MESSAGE) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, instruction_index);
            return false;
        }
        Om_VM_Message_Node *node = create_message_node(shared);
        if (node == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        const Om_Status clone_status = clone_message_into_heap(
            state, &node->transit_heap, request->fields[1], &node->message);
        if (clone_status != OM_STATUS_OK) {
            destroy_message_node(node);
            set_runtime_error(state, result,
                              error_for_allocation_status(clone_status),
                              function_index, instruction_index);
            return false;
        }
        Om_VM_Timer_Context *context = om_alloc(sizeof(*context));
        if (context == NULL) {
            destroy_message_node(node);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, instruction_index);
            return false;
        }
        *context = (Om_VM_Timer_Context){
            .shared = shared,
            .recipient = recipient,
            .function_index = function_index,
            .instruction_index = instruction_index,
            .message_node = node
        };
        const Om_Status timer_status = submit_timer(
            shared, context, (uint64_t)delay);
        if (timer_status != OM_STATUS_OK) {
            destroy_timer_context(context);
            set_runtime_error(state, result,
                timer_status == OM_STATUS_OUT_OF_MEMORY
                    ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                    : timer_status == OM_STATUS_OUT_OF_RANGE
                        ? OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT
                        : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                function_index, instruction_index);
            return false;
        }
        return true;
    }

    set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
                      function_index, instruction_index);
    return false;
}

static bool schema_is_actor_down(const Om_VM_State *state,
                                 uint32_t schema_index)
{
    return state->actor_down_schema_index != UINT32_MAX
        && schema_index == state->actor_down_schema_index;
}

static bool make_actor_down_message(Om_VM_State *state, uint64_t actor_id,
                                    uint32_t reason, Om_VM_Value *message,
                                    Om_VM_Execution_Result *result,
                                    uint32_t function_index, uint32_t pc)
{
    if (state->actor_down_schema_index >= state->image->schema_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_Value fields[2] = {
        om_vm_value_actor_id(actor_id),
        {.tag = OM_VM_VALUE_DOWN_REASON, .as.down_reason = reason}
    };
    const Om_Status status = om_vm_heap_make_record(
        active_heap(state), OM_VM_OBJECT_MESSAGE,
        state->actor_down_schema_index, fields, 2, message);
    if (status != OM_STATUS_OK) {
        set_runtime_error(state, result, error_for_allocation_status(status),
                          function_index, pc);
        return false;
    }
    return true;
}

static bool notify_actor_watchers(Om_VM_State *state,
                                  Om_VM_Actor_State *actor,
                                  const uint64_t *watcher_ids,
                                  size_t watcher_count,
                                  Om_VM_Execution_Result *result,
                                  uint32_t function_index, uint32_t pc)
{
    if (watcher_count == 0) {
        return true;
    }
    Om_VM_Value message = {0};
    if (!make_actor_down_message(state, actor->id, actor->down_reason,
                                 &message, result, function_index, pc)) {
        return false;
    }
    for (size_t index = 0; index < watcher_count; ++index) {
        Om_VM_Actor_State *watcher = find_actor(state, watcher_ids[index]);
        if (watcher != NULL && !watcher->stopped
            && !enqueue_actor_message(state, watcher, message, result,
                                      function_index, pc)) {
            return false;
        }
    }
    return true;
}

static bool stop_actor(Om_VM_State *state, uint64_t actor_id,
                       uint32_t down_reason, bool failed,
                       Om_VM_Execution_Result *result,
                       uint32_t function_index, uint32_t pc)
{
    Om_VM_State *shared = shared_vm(state);
    Om_VM_Actor_State *actor = find_actor(state, actor_id);
    if (actor == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
                          function_index, pc);
        return false;
    }
    scheduler_mutex_lock(shared);
    if (actor->stopped) {
        scheduler_mutex_unlock(shared);
        return true;
    }
    actor->stopped = true;
    actor->failed = failed;
    actor->down_reason = down_reason;
    clear_actor_mailbox(state, actor);
    (void)atomic_fetch_add_explicit(&shared->run_actors_stopped, 1,
                                    memory_order_relaxed);
    const size_t watcher_count = actor->watcher_count;
    uint64_t *watcher_ids = watcher_count == 0 ? NULL
        : om_alloc_array(watcher_count, sizeof(*watcher_ids));
    if (watcher_count != 0 && watcher_ids != NULL) {
        (void)memcpy(watcher_ids, actor->watchers,
                     watcher_count * sizeof(*watcher_ids));
    }
    scheduler_mutex_unlock(shared);

    bool notified = true;
    if (watcher_count != 0 && watcher_ids == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        notified = false;
    } else if (!notify_actor_watchers(state, actor, watcher_ids,
                                      watcher_count, result,
                                      function_index, pc)) {
        notified = false;
    }
    om_vm_distribution_actor_stopped(shared, actor_id, down_reason);
    om_free(watcher_ids);
    om_free(actor->state_values);
    actor->state_values = NULL;
    actor->state_count = 0;
    if (actor->heap != NULL) {
        om_vm_heap_destroy(actor->heap);
        om_free(actor->heap);
        actor->heap = NULL;
    }
    return notified;
}

static bool add_actor_watcher(Om_VM_State *state, Om_VM_Actor_State *target,
                             uint64_t watcher_id,
                             bool *was_added,
                             bool *target_stopped,
                             Om_VM_Execution_Result *result,
                             uint32_t function_index, uint32_t pc)
{
    scheduler_mutex_lock(state);
    for (size_t index = 0; index < target->watcher_count; ++index) {
        if (target->watchers[index] == watcher_id) {
            if (was_added != NULL) {
                *was_added = false;
            }
            if (target_stopped != NULL) {
                *target_stopped = target->stopped;
            }
            scheduler_mutex_unlock(state);
            return true;
        }
    }
    if (target->watcher_count == target->watcher_capacity) {
        size_t capacity = target->watcher_capacity == 0
            ? 4 : target->watcher_capacity * 2;
        if (capacity < target->watcher_capacity) {
            scheduler_mutex_unlock(state);
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
                              function_index, pc);
            return false;
        }
        uint64_t *watchers = om_realloc_array(
            target->watchers, capacity, sizeof(*target->watchers));
        if (watchers == NULL) {
            scheduler_mutex_unlock(state);
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, pc);
            return false;
        }
        target->watchers = watchers;
        target->watcher_capacity = capacity;
    }
    target->watchers[target->watcher_count++] = watcher_id;
    if (was_added != NULL) {
        *was_added = true;
    }
    if (target_stopped != NULL) {
        *target_stopped = target->stopped;
    }
    scheduler_mutex_unlock(state);
    return true;
}

static bool spawn_actor(Om_VM_State *state, uint32_t actor_symbol,
                        bool has_placement,
                        Om_VM_Execution_Result *result,
                        uint32_t function_index, uint32_t pc)
{
    if (has_placement) {
        Om_VM_Value placement = {.tag = OM_VM_VALUE_UNINITIALIZED};
        if (!pop_value(state, &placement, result, function_index, pc)) {
            return false;
        }
        if (placement.tag != OM_VM_VALUE_NODE_REF) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        const uint64_t requester_actor_id = state->frame_count == 0
            ? 0 : state->frames[state->frame_count - 1].actor_id;
        uint64_t remote_actor_id = 0;
        Om_Status status = om_vm_distribution_spawn_remote(
            state, placement.as.node_ref, actor_symbol,
            requester_actor_id, &remote_actor_id);
        if (status != OM_STATUS_OK) {
            set_runtime_error(state, result,
                status == OM_STATUS_NOT_FOUND
                    ? OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF
                    : status == OM_STATUS_OUT_OF_MEMORY
                        ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                        : status == OM_STATUS_OUT_OF_RANGE
                            ? OM_VM_RUNTIME_ERROR_ACTOR_LIMIT
                            : OM_VM_RUNTIME_ERROR_DISTRIBUTION,
                function_index, pc);
            return false;
        }
        Om_VM_Value remote_actor_ref = {
            .tag = OM_VM_VALUE_UNINITIALIZED
        };
        status = om_vm_distribution_make_remote_actor_ref(
            state, placement.as.node_ref, remote_actor_id,
            &remote_actor_ref);
        if (status != OM_STATUS_OK) {
            set_runtime_error(state, result,
                status == OM_STATUS_OUT_OF_MEMORY
                    ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                    : OM_VM_RUNTIME_ERROR_DISTRIBUTION,
                function_index, pc);
            return false;
        }
        return push_value(state, remote_actor_ref, result,
                          function_index, pc);
    }
    size_t actor_index = state->image->actor_count;
    for (size_t index = 0; index < state->image->actor_count; ++index) {
        if (state->image->actors[index].actor_symbol.value == actor_symbol) {
            actor_index = index;
            break;
        }
    }
    if (actor_index == state->image->actor_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_State *shared = shared_vm(state);
    scheduler_mutex_lock(shared);
    const size_t actor_count = atomic_load_explicit(
        &shared->actor_count, memory_order_relaxed);
    if (actor_count >= shared->limits.max_actors) {
        scheduler_mutex_unlock(shared);
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
                          function_index, pc);
        return false;
    }
    const Om_Status reserve_status = reserve_actors(shared, actor_count + 1);
    if (reserve_status != OM_STATUS_OK) {
        scheduler_mutex_unlock(shared);
        set_runtime_error(state, result,
            reserve_status == OM_STATUS_OUT_OF_MEMORY
                ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                : OM_VM_RUNTIME_ERROR_ACTOR_LIMIT,
            function_index, pc);
        return false;
    }
    const Om_Bytecode_Actor *descriptor = &state->image->actors[actor_index];
    Om_VM_Value *state_values = NULL;
    if (descriptor->state_count != 0) {
        state_values = om_alloc_array(descriptor->state_count,
                                      sizeof(*state_values));
        if (state_values == NULL) {
            scheduler_mutex_unlock(shared);
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                              function_index, pc);
            return false;
        }
        for (size_t index = 0; index < descriptor->state_count; ++index) {
            state_values[index] = (Om_VM_Value){
                .tag = OM_VM_VALUE_UNINITIALIZED
            };
        }
    }
    const uint64_t actor_id = (uint64_t)actor_count + 1;
    Om_VM_Heap *actor_heap = om_alloc(sizeof(*actor_heap));
    if (actor_heap == NULL) {
        om_free(state_values);
        scheduler_mutex_unlock(shared);
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY,
                          function_index, pc);
        return false;
    }
    *actor_heap = (Om_VM_Heap){
        .vm = shared,
        .actor_id = actor_id,
        .max_bytes = state->limits.max_heap_bytes,
        .gc_threshold = state->limits.gc_threshold_bytes,
        .last_status = OM_STATUS_OK
    };
    shared->actors[actor_count] = (Om_VM_Actor_State){
        .id = actor_id,
        .bytecode_actor_index = (uint32_t)actor_index,
        .heap = actor_heap,
        .state_values = state_values,
        .state_count = descriptor->state_count
    };
    atomic_store_explicit(&shared->actor_count, actor_count + 1,
                          memory_order_release);
    (void)atomic_fetch_add_explicit(&shared->run_actors_spawned, 1,
                                    memory_order_relaxed);
    scheduler_mutex_unlock(shared);
    if (!push_frame(state, descriptor->initializer_function, NULL, 0,
                    state->stack_length, state->frames[state->frame_count - 1].pc,
                    actor_id, OM_VM_RETURN_SPAWN_INITIALIZER, result,
                    function_index, pc)) {
        return false;
    }
    return true;
}

Om_Status om_vm_distribution_spawn_actor(Om_VM_State *state,
                                          uint32_t actor_symbol,
                                          uint64_t *actor_id)
{
    if (state == NULL || actor_id == NULL
        || actor_symbol == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *actor_id = 0;
    Om_VM_State *shared = shared_vm(state);
    size_t actor_index = shared->image->actor_count;
    for (size_t index = 0; index < shared->image->actor_count; ++index) {
        if (shared->image->actors[index].actor_symbol.value == actor_symbol) {
            actor_index = index;
            break;
        }
    }
    if (actor_index == shared->image->actor_count) {
        return OM_STATUS_NOT_FOUND;
    }

    scheduler_mutex_lock(shared);
    const size_t actor_count = atomic_load_explicit(
        &shared->actor_count, memory_order_relaxed);
    if (actor_count >= shared->limits.max_actors) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = reserve_actors(shared, actor_count + 1);
    if (status == OM_STATUS_OK) {
        status = reserve_ready_queue(shared, shared->ready_length + 1);
    }
    if (status != OM_STATUS_OK) {
        scheduler_mutex_unlock(shared);
        return status;
    }
    const Om_Bytecode_Actor *descriptor =
        &shared->image->actors[actor_index];
    Om_VM_Value *state_values = descriptor->state_count == 0 ? NULL
        : om_alloc_array(descriptor->state_count, sizeof(*state_values));
    if (descriptor->state_count != 0 && state_values == NULL) {
        scheduler_mutex_unlock(shared);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < descriptor->state_count; ++index) {
        state_values[index] = (Om_VM_Value){
            .tag = OM_VM_VALUE_UNINITIALIZED
        };
    }
    const uint64_t new_actor_id = (uint64_t)actor_count + 1;
    Om_VM_Heap *actor_heap = om_alloc(sizeof(*actor_heap));
    if (actor_heap == NULL) {
        om_free(state_values);
        scheduler_mutex_unlock(shared);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *actor_heap = (Om_VM_Heap){
        .vm = shared,
        .actor_id = new_actor_id,
        .max_bytes = shared->limits.max_heap_bytes,
        .gc_threshold = shared->limits.gc_threshold_bytes,
        .last_status = OM_STATUS_OK
    };
    shared->actors[actor_count] = (Om_VM_Actor_State){
        .id = new_actor_id,
        .bytecode_actor_index = (uint32_t)actor_index,
        .heap = actor_heap,
        .state_values = state_values,
        .state_count = descriptor->state_count,
        .initialization_pending = true,
        .scheduled = true
    };
    const size_t tail = (shared->ready_head + shared->ready_length)
        % shared->ready_capacity;
    shared->ready_queue[tail] = new_actor_id;
    shared->ready_length += 1;
    atomic_store_explicit(&shared->actor_count, actor_count + 1,
                          memory_order_release);
    (void)atomic_fetch_add_explicit(&shared->run_actors_spawned, 1,
                                    memory_order_relaxed);
    scheduler_condition_broadcast(shared);
    scheduler_mutex_unlock(shared);
    *actor_id = new_actor_id;
    return OM_STATUS_OK;
}

static bool actor_message_handler(const Om_VM_State *state,
                                  const Om_VM_Actor_State *actor,
                                  uint32_t schema_index,
                                  uint32_t *function_index)
{
    if (actor->bytecode_actor_index >= state->image->actor_count
        || schema_index >= state->image->schema_count) {
        return false;
    }
    const Om_Bytecode_Actor *descriptor =
        &state->image->actors[actor->bytecode_actor_index];
    const Om_Symbol_Id message_symbol =
        state->image->schemas[schema_index].symbol_id;
    for (size_t index = 0; index < descriptor->handler_count; ++index) {
        const size_t handler_index = descriptor->first_handler + index;
        if (handler_index >= state->image->handler_count) {
            return false;
        }
        const Om_Bytecode_Actor_Handler *handler =
            &state->image->handlers[handler_index];
        if (handler->message_symbol.value == message_symbol.value) {
            *function_index = handler->function_index;
            return true;
        }
    }
    return false;
}

static bool start_next_actor_handler(Om_VM_State *state,
                                     Om_VM_Execution_Result *result,
                                     uint32_t fallback_function)
{
    Om_VM_State *shared = shared_vm(state);
    for (;;) {
        scheduler_mutex_lock(shared);
        while (shared->ready_length == 0
               && (shared->running_actor_count != 0
                   || shared->pending_external_tasks != 0)
               && !atomic_load_explicit(&shared->scheduler_failed,
                                        memory_order_acquire)) {
            scheduler_condition_wait(shared);
        }
        const bool failed = atomic_load_explicit(
            &shared->scheduler_failed, memory_order_acquire);
        if (failed || atomic_load_explicit(&shared->scheduler_done,
                                           memory_order_acquire)) {
            scheduler_mutex_unlock(shared);
            if (failed) {
                publish_async_failure(state, result, fallback_function);
            }
            return false;
        }
        if (shared->ready_length == 0) {
            atomic_store_explicit(&shared->scheduler_done, true,
                                  memory_order_release);
            scheduler_condition_broadcast(shared);
            scheduler_mutex_unlock(shared);
            return false;
        }
        const uint64_t actor_id = shared->ready_queue[shared->ready_head];
        shared->ready_head = (shared->ready_head + 1)
            % shared->ready_capacity;
        shared->ready_length -= 1;
        if (shared->ready_length == 0) {
            shared->ready_head = 0;
        }
        Om_VM_Actor_State *actor = find_actor(shared, actor_id);
        if (actor == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        actor->scheduled = false;
        if (actor->stopped || actor->running) {
            scheduler_mutex_unlock(shared);
            continue;
        }
        actor->running = true;
        shared->running_actor_count += 1;
        if (shared->running_actor_count > shared->max_running_actor_count) {
            shared->max_running_actor_count = shared->running_actor_count;
        }
        if (actor->initialization_pending) {
            actor->initialization_pending = false;
            if (actor->bytecode_actor_index >= state->image->actor_count
                || state->frame_count != 0) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                                  fallback_function, 0);
                (void)finish_actor_quantum_locked(
                    shared, actor, result, fallback_function, 0);
                scheduler_mutex_unlock(shared);
                return false;
            }
            const uint32_t initializer = state->image->actors[
                actor->bytecode_actor_index].initializer_function;
            state->handler_actor_id = actor_id;
            state->handler_result = (Om_VM_Value){
                .tag = OM_VM_VALUE_UNINITIALIZED
            };
            state->handler_completed = false;
            scheduler_mutex_unlock(shared);
            if (!push_frame(state, initializer, NULL, 0, 0, 0, actor_id,
                            OM_VM_RETURN_REMOTE_INITIALIZER, result,
                            fallback_function, 0)) {
                scheduler_mutex_lock(shared);
                actor = find_actor(shared, actor_id);
                if (actor != NULL) {
                    (void)finish_actor_quantum_locked(
                        shared, actor, result, fallback_function, 0);
                }
                scheduler_mutex_unlock(shared);
                return false;
            }
            return true;
        }
        if (actor->context_saved) {
            if (state->frame_count != 0) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                                  fallback_function, 0);
                (void)finish_actor_quantum_locked(
                    shared, actor, result, fallback_function, 0);
                scheduler_mutex_unlock(shared);
                return false;
            }
            om_free(state->stack);
            om_free(state->frames);
            state->stack = actor->saved_stack;
            state->stack_length = actor->saved_stack_length;
            state->stack_capacity = actor->saved_stack_capacity;
            state->frames = actor->saved_frames;
            state->frame_count = actor->saved_frame_count;
            state->frame_capacity = actor->saved_frame_capacity;
            state->handler_actor_id = actor_id;
            state->handler_result = actor->saved_handler_result;
            state->handler_completed = false;
            actor->saved_stack = NULL;
            actor->saved_stack_length = 0;
            actor->saved_stack_capacity = 0;
            actor->saved_frames = NULL;
            actor->saved_frame_count = 0;
            actor->saved_frame_capacity = 0;
            actor->saved_handler_result =
                (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
            actor->context_saved = false;
            const bool has_native_completion =
                actor->native_call_completion_ready;
            const Om_Status native_status = actor->native_call_status;
            const Om_Native_Call_Binding *native_binding =
                actor->native_call_binding;
            const uint32_t native_function = actor->native_call_function;
            const uint32_t native_instruction =
                actor->native_call_instruction;
            Om_Native_Call_Result native_result = actor->native_call_result;
            actor->native_call_completion_ready = false;
            actor->native_call_status = OM_STATUS_OK;
            actor->native_call_binding = NULL;
            actor->native_call_result = (Om_Native_Call_Result){0};
            scheduler_mutex_unlock(shared);
            if (has_native_completion) {
                if (native_status != OM_STATUS_OK) {
                    if (native_result.ownership == OM_NATIVE_OWNERSHIP_OWNED
                        && native_binding != NULL) {
                        (void)om_native_call_result_release_bound(
                            native_binding, &native_result);
                    }
                    om_native_call_result_destroy(&native_result);
                    set_runtime_error(state, result,
                        native_status == OM_STATUS_OUT_OF_MEMORY
                            ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                            : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                        native_function, native_instruction);
                    return false;
                }
                Om_VM_Value return_value = {
                    .tag = OM_VM_VALUE_UNINITIALIZED
                };
                const Om_Status conversion_status = native_value_to_vm(
                    state, native_binding, &native_result, &return_value);
                if (conversion_status != OM_STATUS_OK
                    && native_result.ownership == OM_NATIVE_OWNERSHIP_OWNED
                    && native_binding != NULL) {
                    (void)om_native_call_result_release_bound(
                        native_binding, &native_result);
                }
                om_native_call_result_destroy(&native_result);
                if (conversion_status != OM_STATUS_OK) {
                    set_runtime_error(state, result,
                        conversion_status == OM_STATUS_OUT_OF_MEMORY
                            ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                            : OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION,
                        native_function, native_instruction);
                    return false;
                }
                if (!push_value(state, return_value, result,
                                native_function, native_instruction)) {
                    return false;
                }
            }
            return true;
        }
        if (actor->mailbox_head == NULL) {
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            continue;
        }
        Om_VM_Message_Node *node = actor->mailbox_head;
        actor->mailbox_head = node->next;
        if (actor->mailbox_head == NULL) {
            actor->mailbox_tail = NULL;
        }
        actor->mailbox_count -= 1;
        shared->mailbox_message_count -= 1;
        (void)atomic_fetch_add_explicit(&shared->run_messages_processed, 1,
                                        memory_order_relaxed);
        scheduler_mutex_unlock(shared);

        const Om_VM_Value transit_message = node->message;
        if (transit_message.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, transit_message.as.object)
            || transit_message.as.object->kind != OM_VM_OBJECT_MESSAGE) {
            om_vm_heap_destroy(&node->transit_heap);
            om_free(node);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              fallback_function, 0);
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        const Om_VM_Record_Object *message =
            (const Om_VM_Record_Object *)transit_message.as.object;
        if (message->schema_index >= state->image->schema_count
            || state->image->schemas[message->schema_index].kind
                != OM_BYTECODE_SCHEMA_MESSAGE) {
            om_vm_heap_destroy(&node->transit_heap);
            om_free(node);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              fallback_function, 0);
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        uint32_t receive_function = UINT32_MAX;
        if (!actor_message_handler(state, actor, message->schema_index,
                                   &receive_function)) {
            const bool ignored = schema_is_actor_down(
                state, message->schema_index);
            om_vm_heap_destroy(&node->transit_heap);
            om_free(node);
            if (!ignored) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE,
                                  fallback_function, 0);
            }
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            if (!ignored) {
                return false;
            }
            continue;
        }
        if (receive_function >= state->image->function_count
            || message->field_count
                != state->image->functions[receive_function].parameter_count) {
            om_vm_heap_destroy(&node->transit_heap);
            om_free(node);
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              fallback_function, 0);
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        Om_VM_Value local_message = {0};
        const Om_Status clone_status = clone_message_into_heap(
            state, actor->heap, transit_message, &local_message);
        om_vm_heap_destroy(&node->transit_heap);
        om_free(node);
        if (clone_status != OM_STATUS_OK) {
            set_runtime_error(state, result,
                              error_for_allocation_status(clone_status),
                              fallback_function, 0);
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        message = (const Om_VM_Record_Object *)local_message.as.object;
        state->handler_actor_id = actor_id;
        state->handler_completed = false;
        state->handler_result = (Om_VM_Value){
            .tag = OM_VM_VALUE_UNINITIALIZED
        };
        if (!push_frame(state, receive_function, message->fields,
                        message->field_count, 0, 0, actor_id,
                        OM_VM_RETURN_SCHEDULED_HANDLER, result,
                        receive_function,
                        state->image->functions[receive_function].entry_pc)) {
            state->handler_actor_id = 0;
            scheduler_mutex_lock(shared);
            (void)finish_actor_quantum_locked(
                shared, actor, result, fallback_function, 0);
            scheduler_mutex_unlock(shared);
            return false;
        }
        return true;
    }
}

static void unwind_stopped_actor(Om_VM_State *state)
{
    const uint64_t actor_id = state->stop_actor_id;
    while (state->frame_count != 0) {
        Om_VM_Call_Frame *frame = &state->frames[state->frame_count - 1];
        if (frame->actor_id != actor_id) {
            break;
        }
        const bool scheduled = frame->return_mode
            == (uint8_t)OM_VM_RETURN_SCHEDULED_HANDLER;
        om_free(frame->locals);
        state->stack_length = frame->stack_base;
        state->frame_count -= 1;
        if (scheduled) {
            state->handler_result = om_vm_value_unit();
            state->handler_completed = true;
            break;
        }
    }
    state->stop_unwind_requested = false;
    state->stop_actor_id = 0;
}

static uint32_t find_actor_down_schema(const Om_Bytecode_Image *image)
{
    const Om_String_View actor_id_name = {"actor_id", 8};
    const Om_String_View reason_name = {"reason", 6};
    for (size_t schema_index = 0; schema_index < image->schema_count;
         ++schema_index) {
        const Om_Bytecode_Schema *schema = &image->schemas[schema_index];
        if (schema->kind != OM_BYTECODE_SCHEMA_MESSAGE
            || schema->field_count != 2
            || schema->first_field > image->schema_field_count
            || image->schema_field_count - schema->first_field < 2) {
            continue;
        }
        const uint32_t first_index = image->schema_fields[
            schema->first_field].name_constant;
        const uint32_t second_index = image->schema_fields[
            schema->first_field + 1].name_constant;
        if (first_index >= image->constant_count
            || second_index >= image->constant_count) {
            continue;
        }
        const Om_Bytecode_Constant *first = &image->constants[first_index];
        const Om_Bytecode_Constant *second = &image->constants[second_index];
        if (first->kind == OM_BYTECODE_CONSTANT_STRING
            && second->kind == OM_BYTECODE_CONSTANT_STRING
            && om_string_view_equal(first->as.string_value, actor_id_name)
            && om_string_view_equal(second->as.string_value, reason_name)) {
            return (uint32_t)schema_index;
        }
    }
    return UINT32_MAX;
}

static bool execute_instruction(Om_VM_State *state,
                               const Om_Bytecode_Instruction *instruction,
                               Om_VM_Execution_Result *result,
                               uint32_t function_index, uint32_t pc,
                               bool *finished)
{
    Om_VM_Call_Frame *frame = &state->frames[state->frame_count - 1];
    const Om_Bytecode_Function *function =
        &state->image->functions[function_index];
    Om_VM_Value value = {0};
    switch (instruction->opcode) {
    case OM_BC_NOP:
        return true;
    case OM_BC_LOAD_CONST:
        return execute_constant(state, instruction, result,
                                function_index, pc);
    case OM_BC_LOAD_UNIT:
        return push_value(state, om_vm_value_unit(), result,
                          function_index, pc);
    case OM_BC_LOAD_LOCAL:
        if (instruction->operand_a >= function->local_count) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        value = frame->locals[instruction->operand_a];
        if (value.tag == OM_VM_VALUE_UNINITIALIZED) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_UNINITIALIZED_LOCAL,
                              function_index, pc);
            return false;
        }
        return push_value(state, value, result, function_index, pc);
    case OM_BC_STORE_LOCAL:
        if (instruction->operand_a >= function->local_count
            || state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        frame->locals[instruction->operand_a] =
            state->stack[--state->stack_length];
        return true;
    case OM_BC_POP:
        return pop_value(state, &value, result, function_index, pc);
    case OM_BC_DUP:
        if (state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        return push_value(state, peek_value(state, 0), result,
                          function_index, pc);
    case OM_BC_ADD_INT:
    case OM_BC_SUB_INT:
    case OM_BC_MUL_INT:
    case OM_BC_DIV_INT:
    case OM_BC_MOD_INT:
        return execute_int_binary(state, instruction->opcode, result,
                                  function_index, pc);
    case OM_BC_NEG_INT:
        if (state->stack_length == 0
            || peek_value(state, 0).tag != OM_VM_VALUE_INT) {
            set_runtime_error(state, result,
                              state->stack_length == 0
                                  ? OM_VM_RUNTIME_ERROR_INVALID_BYTECODE
                                  : OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if (peek_value(state, 0).as.integer == INT64_MIN) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INTEGER_OVERFLOW,
                              function_index, pc);
            return false;
        }
        state->stack[state->stack_length - 1].as.integer =
            -state->stack[state->stack_length - 1].as.integer;
        return true;
    case OM_BC_ADD_FLOAT:
    case OM_BC_SUB_FLOAT:
    case OM_BC_MUL_FLOAT:
    case OM_BC_DIV_FLOAT:
        return execute_float_binary(state, instruction->opcode, result,
                                    function_index, pc);
    case OM_BC_NEG_FLOAT:
        if (state->stack_length == 0
            || peek_value(state, 0).tag != OM_VM_VALUE_FLOAT) {
            set_runtime_error(state, result,
                              state->stack_length == 0
                                  ? OM_VM_RUNTIME_ERROR_INVALID_BYTECODE
                                  : OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        state->stack[state->stack_length - 1].as.floating =
            -state->stack[state->stack_length - 1].as.floating;
        return true;
    case OM_BC_NOT_BOOL:
        if (state->stack_length == 0
            || peek_value(state, 0).tag != OM_VM_VALUE_BOOL) {
            set_runtime_error(state, result,
                              state->stack_length == 0
                                  ? OM_VM_RUNTIME_ERROR_INVALID_BYTECODE
                                  : OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        state->stack[state->stack_length - 1].as.boolean =
            !state->stack[state->stack_length - 1].as.boolean;
        return true;
    case OM_BC_EQ:
    case OM_BC_NE:
    case OM_BC_LT:
    case OM_BC_LE:
    case OM_BC_GT:
    case OM_BC_GE:
        return execute_comparison(state, instruction->opcode, result,
                                  function_index, pc);
    case OM_BC_JUMP:
        frame->pc = instruction->operand_a;
        return true;
    case OM_BC_JUMP_IF_FALSE:
    case OM_BC_JUMP_IF_TRUE: {
        if (!pop_value(state, &value, result, function_index, pc)) {
            return false;
        }
        if (value.tag != OM_VM_VALUE_BOOL) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if ((instruction->opcode == OM_BC_JUMP_IF_TRUE && value.as.boolean)
            || (instruction->opcode == OM_BC_JUMP_IF_FALSE
                && !value.as.boolean)) {
            frame->pc = instruction->operand_a;
        }
        return true;
    }
    case OM_BC_CALL: {
        const size_t argument_count = instruction->operand_b;
        if (argument_count > state->stack_length) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const size_t arguments_start = state->stack_length - argument_count;
        return execute_call(state, instruction->operand_a, argument_count,
                            arguments_start, arguments_start, result,
                            function_index, pc);
    }
    case OM_BC_CALL_INDIRECT: {
        const size_t argument_count = instruction->operand_b;
        if (state->stack_length == 0
            || argument_count > state->stack_length - 1) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const size_t callee_index = state->stack_length - argument_count - 1;
        const Om_VM_Value callee = state->stack[callee_index];
        if (callee.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, callee.as.object)
            || callee.as.object->kind != OM_VM_OBJECT_FUNCTION) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        const uint32_t target_function =
            ((Om_VM_Function_Object *)callee.as.object)->function_index;
        return execute_call(state, target_function, argument_count,
                            callee_index + 1, callee_index, result,
                            function_index, pc);
    }
    case OM_BC_LOAD_FUNCTION: {
        Om_VM_Value function_value = {0};
        const Om_Status status = om_vm_heap_make_function(
            active_heap(state), instruction->operand_a, &function_value);
        if (status != OM_STATUS_OK) {
            return return_error_for_heap(state, result, status,
                                         function_index, pc);
        }
        return push_value(state, function_value, result, function_index, pc);
    }
    case OM_BC_RETURN:
        if (!pop_value(state, &value, result, function_index, pc)) {
            return false;
        }
        return finish_current_frame(state, value, result, finished,
                                    function_index, pc);
    case OM_BC_MAKE_RECORD:
    case OM_BC_MAKE_MESSAGE:
        return execute_make_record(state, instruction, result,
                                   function_index, pc);
    case OM_BC_MAKE_OK:
    case OM_BC_MAKE_ERROR: {
        if (!pop_value(state, &value, result, function_index, pc)) {
            return false;
        }
        Om_VM_Value wrapped = {0};
        const Om_Status status = om_vm_heap_make_result(
            active_heap(state), instruction->opcode == OM_BC_MAKE_OK,
            value, &wrapped);
        if (status != OM_STATUS_OK) {
            return return_error_for_heap(state, result, status,
                                         function_index, pc);
        }
        return push_value(state, wrapped, result, function_index, pc);
    }
    case OM_BC_IS_OK:
    case OM_BC_UNWRAP_OK:
    case OM_BC_UNWRAP_ERROR: {
        if (state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        value = peek_value(state, 0);
        if (value.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, value.as.object)
            || value.as.object->kind != OM_VM_OBJECT_RESULT) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        {
            const Om_VM_Result_Object *result_object =
                (const Om_VM_Result_Object *)value.as.object;
            if (instruction->opcode == OM_BC_IS_OK) {
                state->stack_length -= 1;
                return push_value(state,
                    om_vm_value_bool(result_object->is_ok), result,
                    function_index, pc);
            }
            const bool wants_ok = instruction->opcode == OM_BC_UNWRAP_OK;
            if (result_object->is_ok != wants_ok) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_BAD_RESULT_ACCESS,
                                  function_index, pc);
                return false;
            }
            state->stack[state->stack_length - 1] = result_object->payload;
            return true;
        }
    }
    case OM_BC_GET_FIELD:
        return execute_get_field(state, instruction, result,
                                 function_index, pc);
    case OM_BC_WITH_FIELD:
        return execute_with_field(state, instruction, result,
                                  function_index, pc);
    case OM_BC_MAKE_LIST:
    case OM_BC_MAKE_MAP:
        return execute_make_collection(state, instruction, result,
                                       function_index, pc);
    case OM_BC_GET_INDEX:
        return execute_get_index(state, result, function_index, pc);
    case OM_BC_BUILD_STRING:
        return execute_make_string(state, instruction->operand_a, result,
                                   function_index, pc);
    case OM_BC_TO_STRING:
        if (state->stack_length == 0
            || !vm_to_string(state, peek_value(state, 0), &value, result,
                             function_index, pc)) {
            if (result->error.code == OM_VM_RUNTIME_ERROR_NONE) {
                set_runtime_error(state, result,
                                  OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                                  function_index, pc);
            }
            return false;
        }
        state->stack[state->stack_length - 1] = value;
        return true;
    case OM_BC_CHECK_RESULT:
        if (state->stack_length == 0
            || peek_value(state, 0).tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, peek_value(state, 0).as.object)
            || peek_value(state, 0).as.object->kind != OM_VM_OBJECT_RESULT) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        value = peek_value(state, 0);
        return push_value(state, om_vm_value_bool(
            ((Om_VM_Result_Object *)value.as.object)->is_ok), result,
            function_index, pc);
    case OM_BC_RETURN_ERROR: {
        if (!pop_value(state, &value, result, function_index, pc)) {
            return false;
        }
        if (value.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, value.as.object)
            || value.as.object->kind != OM_VM_OBJECT_RESULT
            || ((Om_VM_Result_Object *)value.as.object)->is_ok) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_BAD_RESULT_ACCESS,
                              function_index, pc);
            return false;
        }
        return finish_current_frame(state, value, result, finished,
                                    function_index, pc);
    }
    case OM_BC_FOR_RANGE_TEST: {
        if (state->stack_length < 3) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const Om_VM_Value current = peek_value(state, 2);
        const Om_VM_Value limit = peek_value(state, 1);
        const Om_VM_Value step = peek_value(state, 0);
        if (current.tag != OM_VM_VALUE_INT || limit.tag != OM_VM_VALUE_INT
            || step.tag != OM_VM_VALUE_INT) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if (step.as.integer == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_RANGE_STEP,
                              function_index, pc);
            return false;
        }
        const bool has_next = step.as.integer > 0
            ? current.as.integer <= limit.as.integer
            : current.as.integer >= limit.as.integer;
        state->stack_length -= 3;
        return push_value(state, om_vm_value_bool(has_next), result,
                          function_index, pc);
    }
    case OM_BC_ITER_INIT:
    case OM_BC_ITER_NEXT:
        return execute_iteration(state, instruction, result,
                                 function_index, pc);
    case OM_BC_SPAWN:
        if (instruction->operand_b > 1) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        return spawn_actor(state, instruction->operand_a,
                           instruction->operand_b != 0, result,
                           function_index, pc);
    case OM_BC_SEND: {
        if (state->stack_length < 2) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const Om_VM_Value target_value = peek_value(state, 1);
        const Om_VM_Value message = peek_value(state, 0);
        if (target_value.tag != OM_VM_VALUE_ACTOR_REF) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
                              function_index, pc);
            return false;
        }
        if (message.tag != OM_VM_VALUE_OBJECT
            || !object_belongs_to_vm(state, message.as.object)
            || message.as.object->kind != OM_VM_OBJECT_MESSAGE) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_TYPE,
                              function_index, pc);
            return false;
        }
        if (target_value.as.actor_id == OM_SYSTEM_ACTOR_REF_CONSOLE
            || target_value.as.actor_id == OM_SYSTEM_ACTOR_REF_TIMER
            || target_value.as.actor_id == OM_SYSTEM_ACTOR_REF_FILE_SYSTEM) {
            if (!submit_system_actor_message(
                    state, target_value.as.actor_id, message, result,
                    function_index, pc)) {
                return false;
            }
            state->stack_length -= 2;
            return true;
        }
        Om_VM_Actor_State *target = find_actor(
            state, target_value.as.actor_id);
        if (target == NULL) {
            const Om_Status status = om_vm_distribution_send_message(
                state, target_value.as.actor_id, frame->actor_id, message);
            if (status != OM_STATUS_OK) {
                set_runtime_error(state, result,
                    status == OM_STATUS_NOT_FOUND
                        ? OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF
                        : status == OM_STATUS_OUT_OF_MEMORY
                            ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                            : OM_VM_RUNTIME_ERROR_DISTRIBUTION,
                    function_index, pc);
                return false;
            }
            state->stack_length -= 2;
            return true;
        }
        if (!enqueue_actor_message(state, target, message, result,
                                   function_index, pc)) {
            return false;
        }
        state->stack_length -= 2;
        return true;
    }
    case OM_BC_WATCH: {
        if (frame->actor_id == 0
            || state->handler_actor_id != frame->actor_id) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        if (state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const Om_VM_Value target_value = peek_value(state, 0);
        if (target_value.tag != OM_VM_VALUE_ACTOR_REF) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
                              function_index, pc);
            return false;
        }
        Om_VM_Actor_State *target = find_actor(
            state, target_value.as.actor_id);
        if (target == NULL) {
            const Om_Status status = om_vm_distribution_watch_remote(
                state, target_value.as.actor_id, frame->actor_id);
            if (status != OM_STATUS_OK) {
                set_runtime_error(state, result,
                    status == OM_STATUS_NOT_FOUND
                        ? OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF
                        : status == OM_STATUS_OUT_OF_MEMORY
                            ? OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY
                            : OM_VM_RUNTIME_ERROR_DISTRIBUTION,
                    function_index, pc);
                return false;
            }
            state->stack_length -= 1;
            return true;
        }
        bool was_added = false;
        bool target_stopped = false;
        if (!add_actor_watcher(state, target, frame->actor_id, &was_added,
                               &target_stopped, result,
                               function_index, pc)) {
            return false;
        }
        state->stack_length -= 1;
        if (target_stopped && was_added) {
            Om_VM_Value message = {0};
            if (!make_actor_down_message(state, target->id,
                                         target->down_reason, &message,
                                         result, function_index, pc)) {
                return false;
            }
            Om_VM_Actor_State *watcher = find_actor(state, frame->actor_id);
            if (watcher == NULL
                || !enqueue_actor_message(state, watcher, message, result,
                                          function_index, pc)) {
                if (watcher == NULL) {
                    set_runtime_error(state, result,
                                      OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF,
                                      function_index, pc);
                }
                return false;
            }
        }
        return true;
    }
    case OM_BC_STOP: {
        if (frame->actor_id == 0
            || state->handler_actor_id != frame->actor_id) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        if (state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        const Om_VM_Value self = peek_value(state, 0);
        if (state->handler_actor_id != frame->actor_id
            || self.tag != OM_VM_VALUE_ACTOR_REF
            || self.as.actor_id != frame->actor_id) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        if (!stop_actor(state, frame->actor_id, 0, false, result,
                        function_index, pc)) {
            return false;
        }
        state->stack_length -= 1;
        state->stop_unwind_requested = true;
        state->stop_actor_id = frame->actor_id;
        return true;
    }
    case OM_BC_SELF:
        if (frame->actor_id == 0
            || state->handler_actor_id != frame->actor_id
            || find_actor(state, frame->actor_id) == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        return push_value(state, om_vm_value_actor_ref(frame->actor_id),
                          result, function_index, pc);
    case OM_BC_LOAD_SYSTEM_ACTOR: {
        static const uint64_t system_actor_ids[] = {
            OM_SYSTEM_ACTOR_REF_CONSOLE,
            OM_SYSTEM_ACTOR_REF_TIMER,
            OM_SYSTEM_ACTOR_REF_FILE_SYSTEM
        };
        if (instruction->operand_a >= OM_BYTECODE_SYSTEM_ACTOR_COUNT) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        return push_value(
            state,
            om_vm_value_actor_ref(system_actor_ids[instruction->operand_a]),
            result, function_index, pc);
    }
    case OM_BC_LOAD_STATE: {
        Om_VM_Actor_State *actor = find_actor(state, frame->actor_id);
        if (actor == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        if (instruction->operand_a >= actor->state_count) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        value = actor->state_values[instruction->operand_a];
        if (value.tag == OM_VM_VALUE_UNINITIALIZED) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_UNINITIALIZED_LOCAL,
                              function_index, pc);
            return false;
        }
        return push_value(state, value, result, function_index, pc);
    }
    case OM_BC_STORE_STATE: {
        Om_VM_Actor_State *actor = find_actor(state, frame->actor_id);
        if (actor == NULL) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT,
                              function_index, pc);
            return false;
        }
        if (instruction->operand_a >= actor->state_count
            || state->stack_length == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, pc);
            return false;
        }
        actor->state_values[instruction->operand_a] =
            state->stack[--state->stack_length];
        return true;
    }
    case OM_BC_OPCODE_COUNT:
        break;
    }
    set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                      function_index, pc);
    return false;
}

Om_Status om_vm_init(Om_VM *vm, const Om_Bytecode_Image *image,
                     Om_VM_Limits limits)
{
    if (vm == NULL || vm->owned_state != NULL || image == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Bytecode_Verify_Result verify = {0};
    if (om_bytecode_verify(image, &verify) != OM_STATUS_OK || !verify.valid) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (limits.max_stack_values == 0) {
        limits.max_stack_values = OM_VM_DEFAULT_MAX_STACK;
    }
    if (limits.max_call_frames == 0) {
        limits.max_call_frames = OM_VM_DEFAULT_MAX_FRAMES;
    }
    if (limits.max_heap_bytes == 0) {
        limits.max_heap_bytes = OM_VM_DEFAULT_MAX_HEAP;
    }
    if (limits.gc_threshold_bytes == 0) {
        limits.gc_threshold_bytes = OM_VM_DEFAULT_GC_THRESHOLD;
    }
    size_t safe_gc_limit = limits.max_heap_bytes / 2;
    if (safe_gc_limit == 0) {
        safe_gc_limit = 1;
    }
    if (limits.gc_threshold_bytes > safe_gc_limit) {
        limits.gc_threshold_bytes = safe_gc_limit;
    }
    if (limits.max_instructions == 0) {
        limits.max_instructions = OM_VM_DEFAULT_MAX_INSTRUCTIONS;
    }
    if (limits.max_actors == 0) {
        limits.max_actors = OM_VM_DEFAULT_MAX_ACTORS;
    }
    if (limits.max_mailbox_messages == 0) {
        limits.max_mailbox_messages = OM_VM_DEFAULT_MAX_MAILBOX_MESSAGES;
    }
    if (limits.worker_count == 0) {
        limits.worker_count = OM_VM_DEFAULT_WORKER_COUNT;
    }
    if (limits.worker_count > OM_VM_MAX_WORKER_COUNT) {
        limits.worker_count = OM_VM_MAX_WORKER_COUNT;
    }
    if (limits.instruction_quantum == 0) {
        limits.instruction_quantum = OM_VM_DEFAULT_INSTRUCTION_QUANTUM;
    }
    Om_Distribution_Node_Handshake distribution_handshake = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .program_fingerprint = image->program_fingerprint,
        .interface_fingerprint = image->interface_fingerprint
    };
    Om_Status status = om_distribution_node_id_generate(
        &distribution_handshake.node_id);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_VM_State *state = om_alloc(sizeof(*state));
    if (state == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(state, 0, sizeof(*state));
    state->image = image;
    state->distribution_handshake = distribution_handshake;
    state->next_remote_actor_ref = UINT64_MAX;
    state->next_distribution_request_id = 1;
    state->native_binding_count = image->function_count;
    if (state->native_binding_count != 0) {
        state->native_bindings = om_alloc_array(
            state->native_binding_count, sizeof(*state->native_bindings));
        if (state->native_bindings == NULL) {
            om_free(state);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memset(state->native_bindings, 0,
                     state->native_binding_count
                         * sizeof(*state->native_bindings));
    }
    state->limits = limits;
    state->heap = (Om_VM_Heap){
        .max_bytes = limits.max_heap_bytes,
        .gc_threshold = limits.gc_threshold_bytes,
        .last_status = OM_STATUS_OK
    };
    state->actor_down_schema_index = find_actor_down_schema(image);
    state->actor_capacity = limits.max_actors;
    state->actors = om_alloc_array(state->actor_capacity,
                                   sizeof(*state->actors));
    if (state->actors == NULL) {
        om_free(state->native_bindings);
        om_free(state);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(state->actors, 0,
                 state->actor_capacity * sizeof(*state->actors));
    atomic_init(&state->heap_bytes_allocated, 0);
    atomic_init(&state->gc_collection_count, 0);
    atomic_init(&state->instruction_count, 0);
    atomic_init(&state->actor_count, 0);
    atomic_init(&state->run_actors_spawned, 0);
    atomic_init(&state->run_messages_processed, 0);
    atomic_init(&state->run_actors_stopped, 0);
    atomic_init(&state->scheduler_done, false);
    atomic_init(&state->scheduler_failed, false);
    atomic_init(&state->execution_active, false);
    state->heap.vm = state;
    if (!scheduler_mutex_init(state)) {
        om_free(state->actors);
        om_free(state->native_bindings);
        om_free(state);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    vm->owned_state = state;
    return OM_STATUS_OK;
}

Om_Status om_vm_distribution_handshake_info(
    const Om_VM *vm,
    Om_Distribution_Node_Handshake *handshake)
{
    if (vm == NULL || vm->owned_state == NULL || handshake == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_VM_State *state = vm->owned_state;
    *handshake = state->distribution_handshake;
    return OM_STATUS_OK;
}

void om_vm_destroy(Om_VM *vm)
{
    if (vm == NULL || vm->owned_state == NULL) {
        return;
    }
    Om_VM_State *state = vm->owned_state;
    scheduler_mutex_lock(state);
    const bool execution_active = atomic_load_explicit(
        &state->execution_active, memory_order_acquire);
    scheduler_mutex_unlock(state);
    if (execution_active) {
        return;
    }
    om_vm_distribution_shutdown(state);
    clear_frames(state);
    const size_t actor_count = atomic_load_explicit(
        &state->actor_count, memory_order_acquire);
    for (size_t index = 0; index < actor_count; ++index) {
        Om_VM_Actor_State *actor = &state->actors[index];
        clear_actor_mailbox(state, actor);
        if (actor->native_call_completion_ready) {
            if (actor->native_call_result.ownership
                    == OM_NATIVE_OWNERSHIP_OWNED
                && actor->native_call_binding != NULL) {
                (void)om_native_call_result_release_bound(
                    actor->native_call_binding, &actor->native_call_result);
            }
            om_native_call_result_destroy(&actor->native_call_result);
            actor->native_call_completion_ready = false;
        }
        if (actor->heap != NULL) {
            om_vm_heap_destroy(actor->heap);
            om_free(actor->heap);
        }
        for (size_t frame_index = 0;
             frame_index < actor->saved_frame_count; ++frame_index) {
            om_free(actor->saved_frames[frame_index].locals);
        }
        om_free(actor->saved_frames);
        om_free(actor->saved_stack);
        om_free(actor->state_values);
        om_free(actor->watchers);
        om_free(actor->remote_watchers);
    }
    om_free(state->ready_queue);
    om_vm_heap_destroy(&state->heap);
    om_free(state->stack);
    om_free(state->frames);
    scheduler_mutex_destroy(state);
    om_free(state->actors);
    om_free(state->native_bindings);
    for (size_t group_index = 0;
         group_index < state->native_binding_group_count; ++group_index) {
        Om_VM_Native_Binding_Group *group =
            state->native_binding_groups[group_index];
        if (group == NULL) {
            continue;
        }
        for (size_t binding_index = 0;
             binding_index < group->binding_count; ++binding_index) {
            om_native_call_binding_destroy(&group->bindings[binding_index]);
        }
        om_free(group->bindings);
        om_free(group);
    }
    om_free(state->native_binding_groups);
    om_free(state->native_callback_targets);
    om_free(state);
    vm->owned_state = NULL;
}

Om_Status om_vm_find_function(const Om_Bytecode_Image *image,
                              Om_String_View name,
                              uint32_t *function_index)
{
    if (image == NULL || function_index == NULL
        || (name.length != 0 && name.data == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *function_index = UINT32_MAX;
    for (size_t index = 0; index < image->function_count; ++index) {
        const Om_Bytecode_Function *function = &image->functions[index];
        if (function->name_constant >= image->constant_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Bytecode_Constant *constant =
            &image->constants[function->name_constant];
        if (constant->kind == OM_BYTECODE_CONSTANT_STRING
            && om_string_view_equal(constant->as.string_value, name)) {
            *function_index = (uint32_t)index;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_NOT_FOUND;
}

static bool native_abi_supported_by_vm(Om_Native_Abi_Type type,
                                       bool is_result)
{
    if (type == OM_NATIVE_ABI_CVOID) {
        return is_result;
    }
    if (type == OM_NATIVE_ABI_CBOOL || type == OM_NATIVE_ABI_CFLOAT
        || type == OM_NATIVE_ABI_CDOUBLE || type == OM_NATIVE_ABI_CSTRING) {
        return true;
    }
    Om_Native_Abi_Type_Info info = {0};
    return om_native_abi_type_info(type, &info) == OM_STATUS_OK
        && !info.is_floating_point && !info.is_pointer;
}

static bool native_function_supported_by_vm(const Om_Omi_Module *module,
                                           size_t function_index)
{
    if (module == NULL || function_index >= module->function_count
        || module->functions == NULL) {
        return false;
    }
    const Om_Omi_Native_Function *function =
        &module->functions[function_index];
    if (function->parameter_start > module->parameter_count
        || function->parameter_count
            > module->parameter_count - function->parameter_start
        || (function->parameter_count != 0 && module->parameters == NULL)
        || !native_abi_supported_by_vm(function->result_type, true)) {
        return false;
    }
    for (size_t index = 0; index < function->parameter_count; ++index) {
        const Om_Omi_Parameter *parameter =
            &module->parameters[function->parameter_start + index];
        if (!parameter->is_callback) {
            if (!native_abi_supported_by_vm(parameter->abi_type, false)) {
                return false;
            }
            continue;
        }
        if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
            || parameter->callback_parameter_count
                > OM_NATIVE_MAX_CALLBACK_PARAMETERS
            || parameter->callback_parameter_start
                > module->callback_parameter_count
            || parameter->callback_parameter_count
                > module->callback_parameter_count
                    - parameter->callback_parameter_start
            || (parameter->callback_parameter_count != 0
                && module->callback_parameters == NULL)) {
            return false;
        }
        for (size_t callback_index = 0;
             callback_index < parameter->callback_parameter_count;
             ++callback_index) {
            const Om_Native_Abi_Type callback_type =
                module->callback_parameters[
                    parameter->callback_parameter_start + callback_index]
                    .abi_type;
            if (!native_abi_supported_by_vm(callback_type, false)
                || callback_type == OM_NATIVE_ABI_CSTRING) {
                return false;
            }
        }
    }
    return true;
}

Om_Status om_vm_bind_native_function(
    Om_VM *vm, uint32_t function_index,
    const Om_Native_Call_Binding *binding)
{
    if (vm == NULL || vm->owned_state == NULL || binding == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = vm->owned_state;
    if (function_index >= state->image->function_count
        || function_index >= state->native_binding_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Bytecode_Function *function =
        &state->image->functions[function_index];
    if ((function->flags & OM_BYTECODE_FUNCTION_EXTERNAL) == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Native_Call_Signature signature = {0};
    Om_Status status = om_native_call_binding_signature(binding, &signature);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const bool variadic_external =
        (function->flags & (OM_BYTECODE_FUNCTION_EXTERNAL
                            | OM_BYTECODE_FUNCTION_VARIADIC))
        == (OM_BYTECODE_FUNCTION_EXTERNAL | OM_BYTECODE_FUNCTION_VARIADIC);
    if (!native_abi_supported_by_vm(signature.result_type, true)
        || (signature.parameter_count != 0
            && signature.parameters == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t visible_parameter_count = 0;
    for (size_t index = 0; index < signature.parameter_count; ++index) {
        if (signature.parameters[index].is_callback) {
            if (signature.parameters[index].callback_result_type
                != OM_NATIVE_ABI_CVOID) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            continue;
        }
        if (!native_abi_supported_by_vm(signature.parameter_types[index],
                                        false)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        visible_parameter_count += 1;
    }
    if (!variadic_external
        && visible_parameter_count != function->parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    scheduler_mutex_lock(state);
    if (atomic_load_explicit(&state->execution_active,
                             memory_order_acquire)) {
        scheduler_mutex_unlock(state);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    state->native_bindings[function_index] = binding;
    scheduler_mutex_unlock(state);
    return OM_STATUS_OK;
}

Om_Status om_vm_bind_native_module(
    Om_VM *vm, const Om_Native_Library *library,
    const Om_Omi_Module *module,
    const Om_VM_Native_Callback_Message *callback_messages,
    size_t callback_message_count,
    size_t maximum_cstring_bytes)
{
    if (vm == NULL || vm->owned_state == NULL || library == NULL
        || library->handle == NULL || module == NULL
        || module->diagnostic_count != 0
        || (module->function_count != 0 && module->functions == NULL)
        || (module->parameter_count != 0 && module->parameters == NULL)
        || (module->callback_parameter_count != 0
            && module->callback_parameters == NULL)
        || (module->callback_binding_count != 0
            && module->callback_bindings == NULL)
        || (callback_message_count != 0 && callback_messages == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (module->function_count == 0) {
        return callback_message_count == 0
            ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = vm->owned_state;
    Om_VM_Native_Binding_Group *group = om_alloc(sizeof(*group));
    uint32_t *function_indices = om_alloc_array(
        module->function_count, sizeof(*function_indices));
    if (group == NULL || function_indices == NULL) {
        om_free(group);
        om_free(function_indices);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *group = (Om_VM_Native_Binding_Group){0};
    group->bindings = om_alloc_array(module->function_count,
                                     sizeof(*group->bindings));
    group->binding_count = module->function_count;
    if (group->bindings == NULL) {
        om_free(function_indices);
        om_free(group);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(group->bindings, 0,
                 module->function_count * sizeof(*group->bindings));

    size_t callback_target_count = 0;
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < module->function_count; ++index) {
        const Om_Omi_Native_Function *function = &module->functions[index];
        if (function->callback_binding_start > module->callback_binding_count
            || function->callback_binding_count
                > module->callback_binding_count
                    - function->callback_binding_start) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (!native_function_supported_by_vm(module, index)) {
            continue;
        }
        if (function->callback_binding_count > SIZE_MAX - callback_target_count) {
            status = OM_STATUS_OVERFLOW;
            break;
        }
        callback_target_count += function->callback_binding_count;
    }
    Om_VM_Native_Callback_Target *callback_targets =
        callback_target_count == 0 ? NULL
        : om_alloc_array(callback_target_count, sizeof(*callback_targets));
    if (status == OM_STATUS_OK && callback_target_count != 0
        && callback_targets == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }

    size_t target_index = 0;
    for (size_t index = 0; status == OM_STATUS_OK
                           && index < module->function_count; ++index) {
        const Om_Omi_Native_Function *function = &module->functions[index];
        if (!native_function_supported_by_vm(module, index)) {
            function_indices[index] = UINT32_MAX;
            continue;
        }
        status = om_vm_find_function(
            state->image, function->name, &function_indices[index]);
        if (status != OM_STATUS_OK) {
            break;
        }
        const uint32_t function_index = function_indices[index];
        if (function_index >= state->image->function_count
            || function_index >= state->native_binding_count
            || (state->image->functions[function_index].flags
                & OM_BYTECODE_FUNCTION_EXTERNAL) == 0) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        status = om_native_call_binding_init(
            &group->bindings[index], library, module, index,
            maximum_cstring_bytes);
        if (status != OM_STATUS_OK) {
            break;
        }
        Om_Native_Call_Signature signature = {0};
        status = om_native_call_binding_signature(
            &group->bindings[index], &signature);
        if (status != OM_STATUS_OK
            || !native_abi_supported_by_vm(signature.result_type, true)
            || (signature.parameter_count != 0
                && (signature.parameters == NULL
                    || signature.parameter_types == NULL))) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        const Om_Bytecode_Function *bytecode_function =
            &state->image->functions[function_index];
        const bool variadic_external =
            (bytecode_function->flags
             & (OM_BYTECODE_FUNCTION_EXTERNAL
                | OM_BYTECODE_FUNCTION_VARIADIC))
            == (OM_BYTECODE_FUNCTION_EXTERNAL
                | OM_BYTECODE_FUNCTION_VARIADIC);
        size_t visible_parameter_count = 0;
        for (size_t parameter_index = 0;
             status == OM_STATUS_OK
                 && parameter_index < signature.parameter_count;
             ++parameter_index) {
            const Om_Omi_Parameter *parameter =
                &signature.parameters[parameter_index];
            if (parameter->is_callback) {
                if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
                    || parameter->callback_parameter_count
                        > OM_NATIVE_MAX_CALLBACK_PARAMETERS
                    || parameter->callback_parameter_start
                        > signature.callback_parameter_count
                    || parameter->callback_parameter_count
                        > signature.callback_parameter_count
                            - parameter->callback_parameter_start
                    || (parameter->callback_parameter_count != 0
                        && signature.callback_parameters == NULL)) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                }
                continue;
            }
            if (!native_abi_supported_by_vm(
                    signature.parameter_types[parameter_index], false)) {
                status = OM_STATUS_INVALID_ARGUMENT;
                break;
            }
            visible_parameter_count += 1;
        }
        if (status != OM_STATUS_OK) {
            break;
        }
        if (!variadic_external
            && visible_parameter_count != bytecode_function->parameter_count) {
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }

        for (size_t binding_index = 0;
             status == OM_STATUS_OK
                 && binding_index < function->callback_binding_count;
             ++binding_index) {
            const Om_Omi_Callback_Binding *callback_binding =
                &module->callback_bindings[
                    function->callback_binding_start + binding_index];
            const Om_VM_Native_Callback_Message *message_mapping = NULL;
            for (size_t message_index = 0;
                 message_index < callback_message_count; ++message_index) {
                const Om_VM_Native_Callback_Message *candidate =
                    &callback_messages[message_index];
                if (candidate->native_function_index == index
                    && candidate->parameter_index
                        == callback_binding->parameter_index) {
                    if (message_mapping != NULL) {
                        status = OM_STATUS_INVALID_ARGUMENT;
                        break;
                    }
                    message_mapping = candidate;
                }
            }
            if (status == OM_STATUS_OK && message_mapping == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK
                && message_mapping->message_symbol.value == UINT32_MAX) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            uint32_t schema_index = UINT32_MAX;
            if (status == OM_STATUS_OK
                && !find_schema(state, message_mapping->message_symbol.value,
                                OM_BYTECODE_SCHEMA_MESSAGE, &schema_index)) {
                status = OM_STATUS_NOT_FOUND;
            }
            const Om_Omi_Parameter *callback_parameter = NULL;
            if (status == OM_STATUS_OK) {
                if (callback_binding->parameter_index
                    >= signature.parameter_count) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                } else {
                    callback_parameter = &signature.parameters[
                        callback_binding->parameter_index];
                }
            }
            if (status == OM_STATUS_OK
                && (!callback_parameter->is_callback
                    || state->image->schemas[schema_index].field_count
                        != callback_parameter->callback_parameter_count)) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                callback_targets[target_index++] =
                    (Om_VM_Native_Callback_Target){
                        .function_index = function_index,
                        .parameter_index = callback_binding->parameter_index,
                        .message_symbol = message_mapping->message_symbol
                    };
            }
        }
    }
    if (status == OM_STATUS_OK && target_index != callback_message_count) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }

    Om_VM_Native_Binding_Group **new_groups = NULL;
    Om_VM_Native_Callback_Target *new_callback_targets = NULL;
    size_t new_callback_target_count = 0;
    if (status == OM_STATUS_OK) {
        scheduler_mutex_lock(state);
        if (atomic_load_explicit(&state->execution_active,
                                 memory_order_acquire)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < module->function_count; ++index) {
            if (function_indices[index] != UINT32_MAX
                && state->native_bindings[function_indices[index]] != NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
        for (size_t index = 0; status == OM_STATUS_OK
                               && index < callback_target_count; ++index) {
            const Om_VM_Native_Callback_Target *candidate =
                &callback_targets[index];
            for (size_t old_index = 0;
                 old_index < state->native_callback_target_count; ++old_index) {
                const Om_VM_Native_Callback_Target *existing =
                    &state->native_callback_targets[old_index];
                if (existing->function_index == candidate->function_index
                    && existing->parameter_index == candidate->parameter_index) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                    break;
                }
            }
            for (size_t prior = 0; status == OM_STATUS_OK && prior < index;
                 ++prior) {
                if (callback_targets[prior].function_index
                        == candidate->function_index
                    && callback_targets[prior].parameter_index
                        == candidate->parameter_index) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                }
            }
        }
        size_t group_count = 0;
        if (status == OM_STATUS_OK
            && !om_checked_size_add(state->native_binding_group_count, 1,
                                    &group_count)) {
            status = OM_STATUS_OVERFLOW;
        }
        if (status == OM_STATUS_OK) {
            new_groups = om_alloc_array(group_count, sizeof(*new_groups));
            if (new_groups == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            }
        }
        if (status == OM_STATUS_OK && callback_target_count != 0) {
            if (!om_checked_size_add(state->native_callback_target_count,
                                     callback_target_count,
                                     &new_callback_target_count)) {
                status = OM_STATUS_OVERFLOW;
            } else {
                new_callback_targets = om_alloc_array(
                    new_callback_target_count,
                    sizeof(*new_callback_targets));
                if (new_callback_targets == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
        }
        if (status == OM_STATUS_OK) {
            if (state->native_binding_group_count != 0) {
                (void)memcpy(new_groups, state->native_binding_groups,
                    state->native_binding_group_count * sizeof(*new_groups));
            }
            new_groups[state->native_binding_group_count] = group;
            if (callback_target_count != 0) {
                if (state->native_callback_target_count != 0) {
                    (void)memcpy(new_callback_targets,
                        state->native_callback_targets,
                        state->native_callback_target_count
                            * sizeof(*new_callback_targets));
                }
                (void)memcpy(
                    new_callback_targets + state->native_callback_target_count,
                    callback_targets,
                    callback_target_count * sizeof(*new_callback_targets));
            }
            for (size_t index = 0; index < module->function_count; ++index) {
                if (function_indices[index] != UINT32_MAX) {
                    state->native_bindings[function_indices[index]] =
                        &group->bindings[index];
                }
            }
            Om_VM_Native_Binding_Group **old_groups =
                state->native_binding_groups;
            state->native_binding_groups = new_groups;
            state->native_binding_group_count = group_count;
            new_groups = NULL;
            om_free(old_groups);
            if (callback_target_count != 0) {
                Om_VM_Native_Callback_Target *old_targets =
                    state->native_callback_targets;
                state->native_callback_targets = new_callback_targets;
                state->native_callback_target_count =
                    new_callback_target_count;
                state->native_callback_target_capacity =
                    new_callback_target_count;
                new_callback_targets = NULL;
                om_free(old_targets);
            }
            group = NULL;
        }
        scheduler_mutex_unlock(state);
    }
    om_free(new_groups);
    om_free(new_callback_targets);
    om_free(callback_targets);
    om_free(function_indices);
    if (group != NULL) {
        for (size_t index = 0; index < group->binding_count; ++index) {
            om_native_call_binding_destroy(&group->bindings[index]);
        }
        om_free(group->bindings);
        om_free(group);
    }
    return status;
}

Om_Status om_vm_bind_native_callback_message(
    Om_VM *vm, uint32_t function_index, size_t parameter_index,
    Om_Symbol_Id message_symbol)
{
    if (vm == NULL || vm->owned_state == NULL
        || message_symbol.value == UINT32_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = vm->owned_state;
    if (function_index >= state->image->function_count
        || function_index >= state->native_binding_count
        || state->native_bindings == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Native_Call_Binding *binding =
        state->native_bindings[function_index];
    if (binding == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Native_Call_Signature signature = {0};
    Om_Status status = om_native_call_binding_signature(binding, &signature);
    if (status != OM_STATUS_OK || parameter_index >= signature.parameter_count
        || signature.parameters == NULL
        || !signature.parameters[parameter_index].is_callback) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Omi_Parameter *parameter = &signature.parameters[parameter_index];
    if (parameter->callback_result_type != OM_NATIVE_ABI_CVOID
        || parameter->callback_parameter_count
            > OM_NATIVE_MAX_CALLBACK_PARAMETERS
        || parameter->callback_parameter_start
            > signature.callback_parameter_count
        || parameter->callback_parameter_count
            > signature.callback_parameter_count
                - parameter->callback_parameter_start
        || (parameter->callback_parameter_count != 0
            && signature.callback_parameters == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < parameter->callback_parameter_count;
         ++index) {
        const Om_Omi_Callback_Parameter *callback_parameter =
            &signature.callback_parameters[
                parameter->callback_parameter_start + index];
        if (signature.callback_parameters == NULL
            || !native_abi_supported_by_vm(callback_parameter->abi_type,
                                           false)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    uint32_t schema_index = UINT32_MAX;
    if (!find_schema(state, message_symbol.value,
                     OM_BYTECODE_SCHEMA_MESSAGE, &schema_index)) {
        return OM_STATUS_NOT_FOUND;
    }
    if (state->image->schemas[schema_index].field_count
        != parameter->callback_parameter_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    scheduler_mutex_lock(state);
    if (atomic_load_explicit(&state->execution_active,
                             memory_order_acquire)
        || state->native_bindings[function_index] != binding) {
        scheduler_mutex_unlock(state);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < state->native_callback_target_count;
         ++index) {
        Om_VM_Native_Callback_Target *target =
            &state->native_callback_targets[index];
        if (target->function_index == function_index
            && target->parameter_index == parameter_index) {
            target->message_symbol = message_symbol;
            scheduler_mutex_unlock(state);
            return OM_STATUS_OK;
        }
    }
    if (state->native_callback_target_count
        == state->native_callback_target_capacity) {
        size_t capacity = state->native_callback_target_capacity == 0 ? 4
            : state->native_callback_target_capacity;
        if (capacity > SIZE_MAX / 2) {
            scheduler_mutex_unlock(state);
            return OM_STATUS_OVERFLOW;
        }
        if (state->native_callback_target_capacity != 0) {
            capacity *= 2;
        }
        Om_VM_Native_Callback_Target *targets = om_realloc_array(
            state->native_callback_targets, capacity, sizeof(*targets));
        if (targets == NULL) {
            scheduler_mutex_unlock(state);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        state->native_callback_targets = targets;
        state->native_callback_target_capacity = capacity;
    }
    state->native_callback_targets[state->native_callback_target_count++] =
        (Om_VM_Native_Callback_Target){
            .function_index = function_index,
            .parameter_index = parameter_index,
            .message_symbol = message_symbol
        };
    scheduler_mutex_unlock(state);
    return OM_STATUS_OK;
}

static bool execute_one_instruction(Om_VM_State *state,
                                    Om_VM_Execution_Result *result,
                                    bool *segment_finished,
                                    uint32_t fallback_function)
{
    if (state->frame_count == 0) {
        return false;
    }
    Om_VM_Call_Frame *frame = &state->frames[state->frame_count - 1];
    if (frame->function_index >= state->image->function_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          frame->function_index, frame->pc);
        return false;
    }
    const Om_Bytecode_Function *function =
        &state->image->functions[frame->function_index];
    const uint32_t function_index = frame->function_index;
    const uint32_t pc = frame->pc;
    if (pc < function->entry_pc || pc >= function->end_pc
        || pc >= state->image->instruction_count) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, pc);
        return false;
    }
    Om_VM_State *shared = shared_vm(state);
    uint64_t instruction_count = atomic_load_explicit(
        &shared->instruction_count, memory_order_relaxed);
    for (;;) {
        if (instruction_count >= state->limits.max_instructions) {
            set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_STEP_LIMIT,
                              function_index, pc);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &shared->instruction_count, &instruction_count,
                instruction_count + 1, memory_order_acq_rel,
                memory_order_relaxed)) {
            break;
        }
    }
    Om_VM_Heap *heap = active_heap(state);
    if (heap != NULL && heap->actor_id != 0 && heap->gc_pending) {
        heap->execution_state = state;
        const Om_Status gc_status = om_vm_heap_collect(heap);
        if (gc_status != OM_STATUS_OK) {
            set_runtime_error(state, result,
                              error_for_allocation_status(gc_status),
                              function_index, pc);
            return false;
        }
    }
    const Om_Bytecode_Instruction instruction =
        state->image->instructions[pc];
    frame->pc += 1;
    if (!execute_instruction(state, &instruction, result,
                             function_index, pc, segment_finished)) {
        return false;
    }
    if (state->stop_unwind_requested) {
        unwind_stopped_actor(state);
        *segment_finished = true;
    }
    (void)fallback_function;
    return true;
}

typedef struct Om_VM_Worker_Context {
    Om_VM_State state;
    Om_VM_Execution_Result result;
    uint32_t fallback_function;
} Om_VM_Worker_Context;

typedef struct Om_VM_Worker_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_VM_Worker_Thread;

static void initialize_worker_context(Om_VM_Worker_Context *worker,
                                     Om_VM_State *shared,
                                     uint32_t fallback_function)
{
    (void)memset(worker, 0, sizeof(*worker));
    worker->state.image = shared->image;
    worker->state.native_bindings = shared->native_bindings;
    worker->state.native_binding_count = shared->native_binding_count;
    worker->state.limits = shared->limits;
    worker->state.shared = shared;
    worker->state.actors = shared->actors;
    worker->state.actor_capacity = shared->actor_capacity;
    worker->state.actor_down_schema_index =
        shared->actor_down_schema_index;
    worker->state.handler_result =
        (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    worker->result = (Om_VM_Execution_Result){
        .succeeded = true,
        .value = {.tag = OM_VM_VALUE_UNINITIALIZED}
    };
    worker->fallback_function = fallback_function;
}

static void destroy_worker_context(Om_VM_Worker_Context *worker)
{
    if (worker == NULL) {
        return;
    }
    clear_frames(&worker->state);
    om_free(worker->state.stack);
    om_free(worker->state.frames);
    worker->state.stack = NULL;
    worker->state.stack_capacity = 0;
    worker->state.frames = NULL;
    worker->state.frame_capacity = 0;
}

static bool complete_actor_handler(Om_VM_State *state,
                                   Om_VM_Execution_Result *result,
                                   uint32_t fallback_function)
{
    const uint64_t actor_id = state->handler_actor_id;
    Om_VM_Actor_State *actor = find_actor(state, actor_id);
    if (actor == NULL) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          fallback_function, 0);
        return false;
    }
    if (!actor->stopped && state->handler_result.tag == OM_VM_VALUE_OBJECT
        && object_belongs_to_vm(state, state->handler_result.as.object)
        && state->handler_result.as.object->kind == OM_VM_OBJECT_RESULT
        && !((Om_VM_Result_Object *)state->handler_result.as.object)->is_ok) {
        if (!stop_actor(state, actor_id, 1, true, result,
                        fallback_function, 0)) {
            /* The Actor heap is released even if a notification failed. */
        }
    }
    state->handler_actor_id = 0;
    state->handler_completed = false;
    state->handler_result = (Om_VM_Value){
        .tag = OM_VM_VALUE_UNINITIALIZED
    };
    scheduler_mutex_lock(state);
    const bool released = finish_actor_quantum_locked(
        state, actor, result, fallback_function, 0);
    scheduler_mutex_unlock(state);
    return released
        && result->error.code == OM_VM_RUNTIME_ERROR_NONE;
}

static bool save_actor_execution_context(Om_VM_State *state,
                                         Om_VM_Execution_Result *result,
                                         uint32_t fallback_function)
{
    const uint64_t actor_id = state->handler_actor_id;
    Om_VM_Actor_State *actor = find_actor(state, actor_id);
    if (actor == NULL || actor->context_saved || state->frame_count == 0) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          fallback_function, 0);
        return false;
    }
    actor->saved_stack = state->stack;
    actor->saved_stack_length = state->stack_length;
    actor->saved_stack_capacity = state->stack_capacity;
    actor->saved_frames = state->frames;
    actor->saved_frame_count = state->frame_count;
    actor->saved_frame_capacity = state->frame_capacity;
    actor->saved_handler_result = state->handler_result;
    actor->context_saved = true;
    state->stack = NULL;
    state->stack_length = 0;
    state->stack_capacity = 0;
    state->frames = NULL;
    state->frame_count = 0;
    state->frame_capacity = 0;
    state->handler_actor_id = 0;
    state->handler_result = (Om_VM_Value){
        .tag = OM_VM_VALUE_UNINITIALIZED
    };
    scheduler_mutex_lock(state);
    const bool released = finish_actor_quantum_locked(
        state, actor, result, fallback_function, 0);
    scheduler_mutex_unlock(state);
    return released;
}

static void run_actor_worker(Om_VM_Worker_Context *worker)
{
    Om_VM_State *state = &worker->state;
    Om_VM_Execution_Result *result = &worker->result;
    Om_VM_State *shared = shared_vm(state);
    const size_t quantum = state->limits.instruction_quantum;
    for (;;) {
        const bool failed = atomic_load_explicit(
            &shared->scheduler_failed, memory_order_acquire);
        if (failed || atomic_load_explicit(&shared->scheduler_done,
                                           memory_order_acquire)) {
            if (failed) {
                publish_async_failure(state, result,
                                      worker->fallback_function);
            }
            return;
        }
        if (state->frame_count == 0 && state->handler_completed) {
            if (!complete_actor_handler(state, result,
                                        worker->fallback_function)) {
                return;
            }
            continue;
        }
        if (state->frame_count == 0) {
            if (!start_next_actor_handler(state, result,
                                          worker->fallback_function)) {
                if (result->error.code != OM_VM_RUNTIME_ERROR_NONE) {
                    atomic_store_explicit(&shared->scheduler_failed, true,
                                          memory_order_release);
                }
                return;
            }
        }

        size_t executed = 0;
        bool suspended_for_native_call = false;
        while (executed < quantum && state->frame_count != 0
               && !atomic_load_explicit(&shared->scheduler_failed,
                                        memory_order_acquire)) {
            bool segment_finished = false;
            if (!execute_one_instruction(state, result, &segment_finished,
                                         worker->fallback_function)) {
                break;
            }
            executed += 1;
            if (state->suspend_current_actor_after_instruction) {
                state->suspend_current_actor_after_instruction = false;
                suspended_for_native_call = true;
                break;
            }
            if (segment_finished && state->handler_completed) {
                break;
            }
        }

        if (result->error.code == OM_VM_RUNTIME_ERROR_NONE
            && atomic_load_explicit(&shared->scheduler_failed,
                                    memory_order_acquire)) {
            publish_async_failure(state, result,
                                  worker->fallback_function);
            const uint64_t actor_id = state->handler_actor_id;
            clear_frames(state);
            state->handler_actor_id = 0;
            state->handler_completed = false;
            if (actor_id != 0) {
                Om_VM_Actor_State *actor = find_actor(state, actor_id);
                if (actor != NULL) {
                    scheduler_mutex_lock(state);
                    (void)finish_actor_quantum_locked(
                        state, actor, result, worker->fallback_function, 0);
                    scheduler_mutex_unlock(state);
                }
            }
            return;
        }
        if (result->error.code != OM_VM_RUNTIME_ERROR_NONE) {
            const uint64_t actor_id = state->handler_actor_id;
            clear_frames(state);
            state->handler_actor_id = 0;
            state->handler_completed = false;
            state->handler_result = (Om_VM_Value){
                .tag = OM_VM_VALUE_UNINITIALIZED
            };
            if (actor_id != 0) {
                Om_VM_Actor_State *actor = find_actor(state, actor_id);
                if (actor != NULL) {
                    scheduler_mutex_lock(state);
                    (void)finish_actor_quantum_locked(
                        state, actor, result, worker->fallback_function, 0);
                    scheduler_mutex_unlock(state);
                }
            }
            return;
        }
        if (suspended_for_native_call) {
            if (state->handler_actor_id == 0
                || !save_actor_execution_context(
                    state, result, worker->fallback_function)) {
                return;
            }
            continue;
        }
        if (state->frame_count == 0 && state->handler_completed) {
            if (!complete_actor_handler(state, result,
                                        worker->fallback_function)) {
                return;
            }
            continue;
        }
        if (state->frame_count != 0 && state->handler_actor_id != 0
            && executed >= quantum) {
            if (!save_actor_execution_context(state, result,
                                              worker->fallback_function)) {
                return;
            }
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI actor_worker_entry(LPVOID argument)
{
    run_actor_worker((Om_VM_Worker_Context *)argument);
    return 0;
}

static bool create_actor_worker(Om_VM_Worker_Thread *thread,
                                Om_VM_Worker_Context *worker)
{
    thread->native = CreateThread(NULL, 0, actor_worker_entry, worker, 0, NULL);
    return thread->native != NULL;
}

static void join_actor_worker(Om_VM_Worker_Thread *thread)
{
    (void)WaitForSingleObject(thread->native, INFINITE);
    (void)CloseHandle(thread->native);
}
#else
static void *actor_worker_entry(void *argument)
{
    run_actor_worker((Om_VM_Worker_Context *)argument);
    return NULL;
}

static bool create_actor_worker(Om_VM_Worker_Thread *thread,
                                Om_VM_Worker_Context *worker)
{
    return pthread_create(&thread->native, NULL, actor_worker_entry, worker)
        == 0;
}

static void join_actor_worker(Om_VM_Worker_Thread *thread)
{
    (void)pthread_join(thread->native, NULL);
}
#endif

static void shutdown_external_services(Om_VM_State *state)
{
    Om_VM_State *shared = shared_vm(state);
    scheduler_mutex_lock(shared);
    Om_Blocking_Pool *blocking_pool = shared->blocking_pool;
    Om_Timer_Service *timer_service = shared->timer_service;
    shared->blocking_pool = NULL;
    shared->timer_service = NULL;
    scheduler_mutex_unlock(shared);
    if (blocking_pool != NULL) {
        const Om_Blocking_Pool_Shutdown_Mode mode =
            atomic_load_explicit(&shared->scheduler_failed,
                                 memory_order_acquire)
                ? OM_BLOCKING_POOL_CANCEL_PENDING
                : OM_BLOCKING_POOL_DRAIN;
        om_blocking_pool_shutdown(blocking_pool, mode);
        om_blocking_pool_destroy(blocking_pool);
    }
    if (timer_service != NULL) {
        om_timer_service_shutdown(timer_service);
        om_timer_service_destroy(timer_service);
    }
}

static void run_actor_worker_pool(Om_VM_State *state,
                                  Om_VM_Execution_Result *result,
                                  uint32_t fallback_function)
{
    Om_VM_Worker_Context contexts[OM_VM_MAX_WORKER_COUNT];
    Om_VM_Worker_Thread threads[OM_VM_MAX_WORKER_COUNT];
    const size_t worker_count = state->limits.worker_count;
    size_t created = 0;
    for (size_t index = 0; index < worker_count; ++index) {
        initialize_worker_context(&contexts[index], state,
                                  fallback_function);
        if (!create_actor_worker(&threads[index], &contexts[index])) {
            break;
        }
        created += 1;
    }
    if (created == 0) {
        initialize_worker_context(&contexts[0], state, fallback_function);
        run_actor_worker(&contexts[0]);
        if (contexts[0].result.error.code != OM_VM_RUNTIME_ERROR_NONE) {
            result->succeeded = false;
            result->value = (Om_VM_Value){
                .tag = OM_VM_VALUE_UNINITIALIZED
            };
            result->error = contexts[0].result.error;
        }
        destroy_worker_context(&contexts[0]);
        return;
    }
    for (size_t index = 0; index < created; ++index) {
        join_actor_worker(&threads[index]);
    }
    for (size_t index = 0; index < created; ++index) {
        if (contexts[index].result.error.code
            != OM_VM_RUNTIME_ERROR_NONE) {
            result->succeeded = false;
            result->value = (Om_VM_Value){
                .tag = OM_VM_VALUE_UNINITIALIZED
            };
            result->error = contexts[index].result.error;
            break;
        }
    }
    for (size_t index = 0; index < created; ++index) {
        destroy_worker_context(&contexts[index]);
    }
}

Om_Status om_vm_execute(Om_VM *vm, uint32_t function_index,
                        const Om_VM_Value *arguments, size_t argument_count,
                        Om_VM_Execution_Result *result)
{
    if (vm == NULL || vm->owned_state == NULL || result == NULL
        || (argument_count != 0 && arguments == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = vm->owned_state;
    scheduler_mutex_lock(state);
    if (atomic_load_explicit(&state->execution_active,
                             memory_order_acquire)
        || state->frame_count != 0) {
        scheduler_mutex_unlock(state);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&state->execution_active, true,
                          memory_order_release);
    scheduler_mutex_unlock(state);
    state->stack_length = 0;
    atomic_store_explicit(&state->instruction_count, 0, memory_order_relaxed);
    atomic_store_explicit(&state->scheduler_failed, false,
                          memory_order_release);
    atomic_store_explicit(&state->scheduler_done, false,
                          memory_order_release);
    scheduler_mutex_lock(state);
    state->max_running_actor_count = 0;
    state->pending_external_tasks = 0;
    state->async_error_code = OM_VM_RUNTIME_ERROR_NONE;
    state->async_error_function = UINT32_MAX;
    state->async_error_instruction = 0;
    scheduler_mutex_unlock(state);
    state->handler_actor_id = 0;
    state->handler_completed = false;
    state->stop_unwind_requested = false;
    state->stop_actor_id = 0;
    atomic_store_explicit(&state->run_actors_spawned, 0, memory_order_relaxed);
    atomic_store_explicit(&state->run_messages_processed, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&state->run_actors_stopped, 0, memory_order_relaxed);
    *result = (Om_VM_Execution_Result){
        .value = {.tag = OM_VM_VALUE_UNINITIALIZED}
    };
    if (!push_frame(state, function_index, arguments, argument_count,
                    0, 0, 0, OM_VM_RETURN_ROOT, result,
                    function_index, 0)) {
        clear_frames(state);
        scheduler_mutex_lock(state);
        atomic_store_explicit(&state->scheduler_done, true,
                              memory_order_release);
        atomic_store_explicit(&state->execution_active, false,
                              memory_order_release);
        scheduler_condition_broadcast(state);
        scheduler_mutex_unlock(state);
        return OM_STATUS_OK;
    }
    bool root_completed = false;
    while (result->error.code == OM_VM_RUNTIME_ERROR_NONE
           && !root_completed) {
        if (state->frame_count == 0) {
            set_runtime_error(state, result,
                              OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                              function_index, 0);
            break;
        }
        bool segment_finished = false;
        if (!execute_one_instruction(state, result, &segment_finished,
                                     function_index)) {
            break;
        }
        if (segment_finished && !state->handler_completed) {
            root_completed = true;
        }
    }
    if (root_completed && result->error.code == OM_VM_RUNTIME_ERROR_NONE) {
        run_actor_worker_pool(state, result, function_index);
    }
    shutdown_external_services(state);
    if (result->error.code != OM_VM_RUNTIME_ERROR_NONE) {
        clear_frames(state);
        state->handler_actor_id = 0;
        state->handler_completed = false;
        state->stop_unwind_requested = false;
        state->stop_actor_id = 0;
    } else if (!root_completed) {
        set_runtime_error(state, result, OM_VM_RUNTIME_ERROR_INVALID_BYTECODE,
                          function_index, 0);
        clear_frames(state);
    }
    result->actors_spawned = atomic_load_explicit(
        &state->run_actors_spawned, memory_order_relaxed);
    result->messages_processed = atomic_load_explicit(
        &state->run_messages_processed, memory_order_relaxed);
    result->actors_stopped = atomic_load_explicit(
        &state->run_actors_stopped, memory_order_relaxed);
    scheduler_mutex_lock(state);
    atomic_store_explicit(&state->scheduler_done, true,
                          memory_order_release);
    atomic_store_explicit(&state->execution_active, false,
                          memory_order_release);
    scheduler_condition_broadcast(state);
    scheduler_mutex_unlock(state);
    return OM_STATUS_OK;
}

const char *om_vm_runtime_error_name(Om_VM_Runtime_Error_Code code)
{
    switch (code) {
    case OM_VM_RUNTIME_ERROR_NONE: return "none";
    case OM_VM_RUNTIME_ERROR_INVALID_FUNCTION: return "invalid_function";
    case OM_VM_RUNTIME_ERROR_ARITY: return "arity";
    case OM_VM_RUNTIME_ERROR_TYPE: return "type";
    case OM_VM_RUNTIME_ERROR_UNINITIALIZED_LOCAL: return "uninitialized_local";
    case OM_VM_RUNTIME_ERROR_INTEGER_OVERFLOW: return "integer_overflow";
    case OM_VM_RUNTIME_ERROR_DIVISION_BY_ZERO: return "division_by_zero";
    case OM_VM_RUNTIME_ERROR_INDEX_OUT_OF_RANGE: return "index_out_of_range";
    case OM_VM_RUNTIME_ERROR_MISSING_FIELD: return "missing_field";
    case OM_VM_RUNTIME_ERROR_MISSING_KEY: return "missing_key";
    case OM_VM_RUNTIME_ERROR_BAD_RESULT_ACCESS: return "bad_result_access";
    case OM_VM_RUNTIME_ERROR_UNSUPPORTED_OPCODE: return "unsupported_opcode";
    case OM_VM_RUNTIME_ERROR_NATIVE_FUNCTION: return "native_function";
    case OM_VM_RUNTIME_ERROR_STEP_LIMIT: return "step_limit";
    case OM_VM_RUNTIME_ERROR_STACK_LIMIT: return "stack_limit";
    case OM_VM_RUNTIME_ERROR_CALL_DEPTH_LIMIT: return "call_depth_limit";
    case OM_VM_RUNTIME_ERROR_HEAP_LIMIT: return "heap_limit";
    case OM_VM_RUNTIME_ERROR_INVALID_RANGE_STEP: return "invalid_range_step";
    case OM_VM_RUNTIME_ERROR_OUT_OF_MEMORY: return "out_of_memory";
    case OM_VM_RUNTIME_ERROR_INVALID_BYTECODE: return "invalid_bytecode";
    case OM_VM_RUNTIME_ERROR_INVALID_ACTOR_REF: return "invalid_actor_ref";
    case OM_VM_RUNTIME_ERROR_INVALID_ACTOR_CONTEXT:
        return "invalid_actor_context";
    case OM_VM_RUNTIME_ERROR_UNHANDLED_MESSAGE: return "unhandled_message";
    case OM_VM_RUNTIME_ERROR_ACTOR_LIMIT: return "actor_limit";
    case OM_VM_RUNTIME_ERROR_MAILBOX_LIMIT: return "mailbox_limit";
    case OM_VM_RUNTIME_ERROR_DISTRIBUTION: return "distribution";
    }
    return "unknown";
}




