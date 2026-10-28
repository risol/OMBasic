#include "ombasic/runtime/native_runtime.h"

#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"
#include "blocking_pool.h"
#include "timer_service.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define OM_RUNTIME_DEFAULT_COLLECTION_THRESHOLD ((size_t)64 * 1024)
#define OM_RUNTIME_SYSTEM_QUEUE_CAPACITY ((size_t)1024)
#define OM_RUNTIME_REMOTE_ACTOR_ID_BIT (UINT64_C(1) << 63)

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

struct Om_Runtime_Root_Frame {
    struct Om_Runtime_Root_Frame *previous;
    void **slots;
    size_t count;
};

typedef struct Om_Runtime_Actor_Message {
    void *object;
    struct Om_Runtime_Actor_Message *next;
} Om_Runtime_Actor_Message;

typedef struct Om_Runtime_Actor_Watcher {
    Om_Runtime_Actor_Ref actor;
    bool remote;
    bool notified;
    struct Om_Runtime_Actor_Watcher *next;
} Om_Runtime_Actor_Watcher;

typedef struct Om_Runtime_Ready_Actor {
    Om_Runtime_Actor *actor;
    struct Om_Runtime_Ready_Actor *next;
} Om_Runtime_Ready_Actor;

typedef struct Om_Runtime_Actor_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_Runtime_Actor_Thread;

struct Om_Runtime_Actor {
    Om_Runtime_Actor_Ref reference;
    const Om_Runtime_Actor_Descriptor *descriptor;
    Om_Execution_Context *context;
    void *state_object;
    void *state_payload;
    Om_Runtime_Root_Frame *state_root;
    Om_Runtime_Actor_Message *mailbox_head;
    Om_Runtime_Actor_Message *mailbox_tail;
    Om_Runtime_Actor_Watcher *watchers;
    bool running;
    bool scheduled;
    bool stop_requested;
    bool terminated;
    bool failed;
    struct Om_Runtime_Actor *next;
};

struct Om_Runtime_Actor_System {
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    Om_Execution_Context *root_context;
    Om_Execution_Context *mailbox_context;
    const Om_Runtime_Program_Descriptor *program;
    const Om_Runtime_Distribution_Hooks *distribution_hooks;
    void *distribution_state;
    size_t distribution_hooks_active;
    Om_Blocking_Pool *blocking_pool;
    Om_Timer_Service *timer_service;
    Om_Runtime_Actor_Thread *threads;
    size_t worker_count;
    Om_Runtime_Actor *actors;
    Om_Runtime_Ready_Actor *ready_head;
    Om_Runtime_Ready_Actor *ready_tail;
    uint64_t next_actor_id;
    size_t outstanding_messages;
    size_t running_handlers;
    bool stopping;
    bool shutting_down;
    bool joined;
};

typedef struct Om_Runtime_System_Task {
    Om_Runtime_Actor_System *system;
    Om_System_Message_Id operation;
    uint64_t recipient_id;
    char *path;
    uint8_t *input_bytes;
    size_t input_length;
    uint8_t *result_bytes;
    size_t result_length;
    Om_Execution_Context *payload_context;
    void *payload_object;
} Om_Runtime_System_Task;

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
    Om_Runtime_Actor_State_Initializer pending_actor_initializer;
    void *pending_actor_initializer_state;
    void *pending_actor_state_object;
    Om_Status pending_actor_initializer_status;
    Om_Runtime_Actor_Handler pending_actor_handler;
    void *pending_actor_state;
    const void *pending_actor_message;
    Om_Status pending_actor_status;
};

typedef struct Om_Runtime_Native_Callback_Registration {
    Om_Runtime_Native_Callback_Function callback;
    Om_Runtime_Actor_System *system;
    uint64_t actor_id;
    const Om_Runtime_Type_Descriptor *message_type;
    size_t in_flight;
    Om_Status first_failure;
    bool active;
    struct Om_Runtime_Native_Callback_Registration *next;
} Om_Runtime_Native_Callback_Registration;

#if defined(_WIN32)
static SRWLOCK native_callback_mutex = SRWLOCK_INIT;
static CONDITION_VARIABLE native_callback_condition =
    CONDITION_VARIABLE_INIT;
#else
static pthread_mutex_t native_callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t native_callback_condition = PTHREAD_COND_INITIALIZER;
#endif
static Om_Runtime_Native_Callback_Registration *native_callback_registrations;

static void native_callback_lock(void)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&native_callback_mutex);
#else
    (void)pthread_mutex_lock(&native_callback_mutex);
#endif
}

static void native_callback_unlock(void)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&native_callback_mutex);
#else
    (void)pthread_mutex_unlock(&native_callback_mutex);
#endif
}

static void native_callback_wait(void)
{
#if defined(_WIN32)
    (void)SleepConditionVariableSRW(&native_callback_condition,
        &native_callback_mutex, INFINITE, 0);
#else
    (void)pthread_cond_wait(&native_callback_condition,
                            &native_callback_mutex);
#endif
}

static void native_callback_broadcast(void)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&native_callback_condition);
#else
    (void)pthread_cond_broadcast(&native_callback_condition);
#endif
}

static void finalize_native_pointer(Om_Runtime_Object_Header *object);

static void restore_roots(Om_Execution_Context *context,
                          Om_Runtime_Root_Frame *base)
{
    if (context == NULL) {
        return;
    }
    Om_Runtime_Root_Frame *frame = context->roots;
    while (frame != NULL && frame != base) {
        Om_Runtime_Root_Frame *previous = frame->previous;
        om_free(frame);
        frame = previous;
    }
    context->roots = base;
}

static const char *runtime_fault_message(Om_Runtime_Fault_Code code)
{
    switch (code) {
    case OM_RUNTIME_FAULT_INTEGER_OVERFLOW:
        return "integer overflow";
    case OM_RUNTIME_FAULT_DIVISION_BY_ZERO:
        return "division by zero";
    case OM_RUNTIME_FAULT_INDEX_OUT_OF_BOUNDS:
        return "collection index out of bounds";
    case OM_RUNTIME_FAULT_MAP_KEY_NOT_FOUND:
        return "map key not found";
    case OM_RUNTIME_FAULT_ZERO_RANGE_STEP:
        return "numeric range step cannot be zero";
    case OM_RUNTIME_FAULT_OPERATION_FAILED:
        return "runtime operation failed";
    case OM_RUNTIME_FAULT_ACTOR_FAILED:
        return "Actor handler failed";
    case OM_RUNTIME_FAULT_NONE:
        break;
    }
    return "unknown runtime fault";
}

static void report_runtime_fault(const Om_Execution_Context *context,
                                 Om_Runtime_Fault_Code code,
                                 uint32_t source_span_id)
{
    const char *message = runtime_fault_message(code);
    if (context != NULL && source_span_id < context->source_span_count) {
        const Om_Runtime_Source_Span *span =
            &context->source_spans[source_span_id];
        if (span->path != NULL && span->path[0] != '\0'
            && span->line != 0 && span->column != 0) {
            (void)fprintf(stderr, "%s:%" PRIu32 ":%" PRIu32
                ": runtime fault: %s\n", span->path, span->line,
                span->column, message);
            return;
        }
    }
    (void)fprintf(stderr, "ombasic runtime fault: %s\n", message);
}

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

static const Om_Runtime_Type_Descriptor RUNTIME_STRING_TYPE = {
    .fingerprint = UINT64_C(0x6f6d737472696e67),
    .kind = OM_RUNTIME_TYPE_STRING,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

static Om_Status trace_native_pointer(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);

static const Om_Runtime_Type_Descriptor RUNTIME_NATIVE_POINTER_TYPE = {
    .fingerprint = UINT64_C(0x6f6d6e6174697665),
    .kind = OM_RUNTIME_TYPE_NATIVE_POINTER,
    .size = sizeof(Om_Runtime_Native_Pointer_Body),
    .alignment = _Alignof(Om_Runtime_Native_Pointer_Body),
    .trace = trace_native_pointer,
};

static size_t stored_value_size(const Om_Runtime_Type_Descriptor *type);

static bool type_is_managed(const Om_Runtime_Type_Descriptor *type)
{
    return type != NULL && (type->kind == OM_RUNTIME_TYPE_STRING
        || type->kind == OM_RUNTIME_TYPE_RECORD
        || type->kind == OM_RUNTIME_TYPE_MESSAGE
        || type->kind == OM_RUNTIME_TYPE_LIST
        || type->kind == OM_RUNTIME_TYPE_MAP
        || type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER);
}

static bool result_descriptor_valid(const Om_Runtime_Type_Descriptor *type)
{
    if (type == NULL || type->kind != OM_RUNTIME_TYPE_RESULT
        || type->fields == NULL || type->field_count != 3
        || type->fields[0].type == NULL
        || type->fields[0].type->kind != OM_RUNTIME_TYPE_BOOL) {
        return false;
    }
    for (size_t index = 0; index < type->field_count; ++index) {
        const Om_Runtime_Field_Descriptor *field = &type->fields[index];
        const size_t field_size = stored_value_size(field->type);
        if (field->type == NULL || field_size > type->size
            || field->offset > type->size - field_size) {
            return false;
        }
    }
    return true;
}

static bool runtime_descriptor_compatible(
    const Om_Runtime_Type_Descriptor *expected,
    const Om_Runtime_Type_Descriptor *actual,
    size_t depth)
{
    if (expected == actual) return true;
    if (expected == NULL || actual == NULL || depth > 64
        || expected->kind != actual->kind
        || expected->size != actual->size
        || expected->alignment != actual->alignment) {
        return false;
    }
    switch (expected->kind) {
    case OM_RUNTIME_TYPE_STRING:
        return true;
    case OM_RUNTIME_TYPE_LIST:
        return expected->element_type != NULL && actual->element_type != NULL
            && runtime_descriptor_compatible(expected->element_type,
                                             actual->element_type, depth + 1);
    case OM_RUNTIME_TYPE_MAP:
        return expected->key_type != NULL && actual->key_type != NULL
            && expected->value_type != NULL && actual->value_type != NULL
            && runtime_descriptor_compatible(expected->key_type,
                                             actual->key_type, depth + 1)
            && runtime_descriptor_compatible(expected->value_type,
                                             actual->value_type, depth + 1);
    case OM_RUNTIME_TYPE_RESULT:
        return result_descriptor_valid(expected)
            && result_descriptor_valid(actual)
            && expected->fields[0].offset == actual->fields[0].offset
            && expected->fields[1].offset == actual->fields[1].offset
            && expected->fields[2].offset == actual->fields[2].offset
            && runtime_descriptor_compatible(expected->fields[1].type,
                                             actual->fields[1].type, depth + 1)
            && runtime_descriptor_compatible(expected->fields[2].type,
                                             actual->fields[2].type, depth + 1);
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
        return expected->fingerprint == actual->fingerprint
            && expected->field_count == actual->field_count;
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return expected->fingerprint == actual->fingerprint;
    case OM_RUNTIME_TYPE_UNIT:
    case OM_RUNTIME_TYPE_NULL:
    case OM_RUNTIME_TYPE_BOOL:
    case OM_RUNTIME_TYPE_INT:
    case OM_RUNTIME_TYPE_FLOAT:
    case OM_RUNTIME_TYPE_ACTOR_REF:
        return true;
    }
    return false;
}

static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static size_t stored_value_size(const Om_Runtime_Type_Descriptor *type)
{
    if (type_is_managed(type)) {
        return sizeof(void *);
    }
    return type == NULL ? 0 : type->size;
}

static Om_Runtime_Object_Header *find_object(
    const Om_Execution_Context *context,
    const void *pointer
)
{
    if (context == NULL || pointer == NULL) {
        return NULL;
    }
    if (context->is_actor_mailbox) {
        /* The mailbox heap is Runtime-private. All object handles reaching it
           come from queued transit objects or their validated typed fields,
           so the owner word is a constant-time membership check. */
        Om_Runtime_Object_Header *object = (Om_Runtime_Object_Header *)pointer;
        return object->fields.owner == context && object->fields.type != NULL
            ? object : NULL;
    }
    for (Om_Runtime_Object_Header *object = context->objects;
         object != NULL; object = object->fields.next) {
        if ((const void *)object == pointer) {
            return object;
        }
    }
    return NULL;
}

static void *object_payload(Om_Runtime_Object_Header *object)
{
    return object == NULL ? NULL : (void *)(object + 1);
}

static const void *object_payload_const(const Om_Runtime_Object_Header *object)
{
    return object == NULL ? NULL : (const void *)(object + 1);
}

static Om_Status collect_internal(Om_Execution_Context *context);
static void destroy_actor_system(Om_Runtime_Actor_System *system);

static Om_Status allocation_safepoint(Om_Execution_Context *context,
                                      size_t expected_bytes)
{
    if (context == NULL || context->collecting) {
        return context == NULL ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    }
    if (expected_bytes > SIZE_MAX - context->allocation_bytes) {
        Om_Status status = collect_internal(context);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    if (expected_bytes > SIZE_MAX - context->allocation_bytes) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (context->allocation_bytes + expected_bytes
        > context->collection_threshold) {
        return collect_internal(context);
    }
    return OM_STATUS_OK;
}

static Om_Status allocate_object(Om_Execution_Context *context,
                                 const Om_Runtime_Type_Descriptor *type,
                                 size_t payload_size,
                                 size_t external_size,
                                 Om_Runtime_Object_Header **result)
{
    if (context == NULL || type == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    if (type->alignment != 0
        && (!is_power_of_two(type->alignment)
            || type->alignment > _Alignof(Om_Runtime_Object_Header))) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(sizeof(Om_Runtime_Object_Header), payload_size,
                             &allocation_size)
        || !om_checked_size_add(allocation_size, external_size,
                                &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = allocation_safepoint(context, allocation_size);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Object_Header *object = om_alloc(allocation_size);
    if (object == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(object, 0, allocation_size);
    object->fields.type = type;
    object->fields.owner = context;
    object->fields.next = context->objects;
    object->fields.allocated_size = allocation_size;
    context->objects = object;
    ++context->live_object_count;
    context->allocation_bytes += allocation_size;
    *result = object;
    return OM_STATUS_OK;
}

static bool descriptor_valid_for_record(
    const Om_Runtime_Type_Descriptor *type
)
{
    if (type == NULL || (type->kind != OM_RUNTIME_TYPE_RECORD
                         && type->kind != OM_RUNTIME_TYPE_MESSAGE)
        || (type->field_count != 0 && type->fields == NULL)
        || (type->alignment != 0
            && (!is_power_of_two(type->alignment)
                || type->alignment > _Alignof(Om_Runtime_Object_Header)))) {
        return false;
    }
    for (size_t index = 0; index < type->field_count; ++index) {
        const Om_Runtime_Field_Descriptor *field = &type->fields[index];
        const size_t field_size = stored_value_size(field->type);
        if (field->type == NULL || field_size > type->size
            || field->offset > type->size - field_size) {
            return false;
        }
    }
    return true;
}

static bool value_matches_type(const Om_Execution_Context *context,
                               const Om_Runtime_Type_Descriptor *type,
                               Om_Runtime_Value value)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
    case OM_RUNTIME_TYPE_UNIT:
        return true;
    case OM_RUNTIME_TYPE_NULL:
        return value.managed == NULL;
    case OM_RUNTIME_TYPE_BOOL:
        return type->size == sizeof(value.boolean);
    case OM_RUNTIME_TYPE_INT:
        return type->size == sizeof(value.integer);
    case OM_RUNTIME_TYPE_ACTOR_REF:
        return type->size == sizeof(value.integer);
    case OM_RUNTIME_TYPE_FLOAT:
        return type->size == sizeof(value.floating);
    case OM_RUNTIME_TYPE_STRING:
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
    case OM_RUNTIME_TYPE_LIST:
    case OM_RUNTIME_TYPE_MAP:
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return value.managed == NULL
            || find_object(context, value.managed) != NULL;
    case OM_RUNTIME_TYPE_RESULT:
        return false;
    }
    return false;
}

static void store_value(void *address,
                        const Om_Runtime_Type_Descriptor *type,
                        Om_Runtime_Value value)
{
    if (type->kind == OM_RUNTIME_TYPE_BOOL) {
        (void)memcpy(address, &value.boolean, sizeof(value.boolean));
    } else if (type->kind == OM_RUNTIME_TYPE_INT) {
        (void)memcpy(address, &value.integer, sizeof(value.integer));
    } else if (type->kind == OM_RUNTIME_TYPE_ACTOR_REF) {
        (void)memcpy(address, &value.integer, sizeof(value.integer));
    } else if (type->kind == OM_RUNTIME_TYPE_FLOAT) {
        (void)memcpy(address, &value.floating, sizeof(value.floating));
    } else if (type_is_managed(type)) {
        (void)memcpy(address, &value.managed, sizeof(value.managed));
    }
}

Om_Execution_Context *om_runtime_context_create(size_t collection_threshold)
{
    Om_Execution_Context *context = om_alloc(sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    const size_t threshold = collection_threshold == 0
        ? OM_RUNTIME_DEFAULT_COLLECTION_THRESHOLD : collection_threshold;
    *context = (Om_Execution_Context){
        .collection_threshold = threshold,
        .minimum_collection_threshold = threshold,
    };
    return context;
}

Om_Status om_runtime_actor_system_context_create(
    Om_Execution_Context *context,
    Om_Execution_Context **result)
{
    if (context == NULL || context->actor_system == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Execution_Context *worker = om_runtime_context_create(
        context->collection_threshold);
    if (worker == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    worker->actor_system = context->actor_system;
    worker->source_spans = context->source_spans;
    worker->source_span_count = context->source_span_count;
    *result = worker;
    return OM_STATUS_OK;
}

Om_Status om_runtime_set_source_spans(
    Om_Execution_Context *context,
    const Om_Runtime_Source_Span *spans,
    size_t span_count
)
{
    if (context == NULL || (span_count != 0 && spans == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < span_count; ++index) {
        if (spans[index].path == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    context->source_spans = spans;
    context->source_span_count = span_count;
    return OM_STATUS_OK;
}

int om_runtime_run(Om_Execution_Context *context,
                   Om_Runtime_Entry_Function entry)
{
    if (context == NULL || entry == NULL) {
        return 70;
    }
    Om_Runtime_Fault_Boundary boundary = {
        .previous = context->fault_boundary,
        .root_base = context->roots,
        .source_span_id = UINT32_MAX,
    };
    context->fault_boundary = &boundary;
    const int jump_status = setjmp(boundary.environment);
    int result = 70;
    if (jump_status == 0) {
        result = entry(context);
        if (context->roots != boundary.root_base) {
            restore_roots(context, boundary.root_base);
            report_runtime_fault(context, OM_RUNTIME_FAULT_OPERATION_FAILED,
                                 UINT32_MAX);
            result = 70;
        }
    } else {
        restore_roots(context, boundary.root_base);
        if (boundary.code != OM_RUNTIME_FAULT_ACTOR_FAILED) {
            report_runtime_fault(context, boundary.code,
                                 boundary.source_span_id);
        }
        result = 70;
    }
    context->fault_boundary = boundary.previous;
    return result;
}

_Noreturn void om_runtime_fault(Om_Execution_Context *context,
                                Om_Runtime_Fault_Code code,
                                uint32_t source_span_id)
{
    if (context != NULL && context->fault_boundary != NULL) {
        Om_Runtime_Fault_Boundary *boundary = context->fault_boundary;
        boundary->code = code;
        boundary->source_span_id = source_span_id;
        longjmp(boundary->environment, 1);
    }
    report_runtime_fault(context, code, source_span_id);
    abort();
}

void om_runtime_context_destroy(Om_Execution_Context *context)
{
    if (context == NULL) {
        return;
    }
    if (context->owns_actor_system && context->actor_system != NULL) {
        destroy_actor_system(context->actor_system);
        context->actor_system = NULL;
    }
    Om_Runtime_Object_Header *object = context->objects;
    while (object != NULL) {
        Om_Runtime_Object_Header *next = object->fields.next;
        finalize_native_pointer(object);
        om_free(object->fields.external_storage);
        om_free(object);
        object = next;
    }
    Om_Runtime_Root_Frame *frame = context->roots;
    while (frame != NULL) {
        Om_Runtime_Root_Frame *previous = frame->previous;
        om_free(frame);
        frame = previous;
    }
    om_free(context);
}

Om_Runtime_Root_Frame *om_runtime_roots_push(Om_Execution_Context *context,
                                             void **root_slots,
                                             size_t root_count)
{
    if (context == NULL || (root_count != 0 && root_slots == NULL)) {
        return NULL;
    }
    Om_Runtime_Root_Frame *frame = om_alloc(sizeof(*frame));
    if (frame == NULL) {
        return NULL;
    }
    *frame = (Om_Runtime_Root_Frame){
        .previous = context->roots,
        .slots = root_slots,
        .count = root_count,
    };
    context->roots = frame;
    return frame;
}

Om_Status om_runtime_roots_pop(Om_Execution_Context *context,
                               Om_Runtime_Root_Frame *frame)
{
    if (context == NULL || frame == NULL || context->roots != frame) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    context->roots = frame->previous;
    om_free(frame);
    return OM_STATUS_OK;
}

static Om_Status mark_pointer(Om_Execution_Context *context,
                              const void *pointer,
                              Om_Runtime_Object_Header ***worklist,
                              size_t *worklist_count,
                              size_t *worklist_capacity)
{
    Om_Runtime_Object_Header *object = find_object(context, pointer);
    if (object == NULL) {
        return OM_STATUS_OK;
    }
    if (object->fields.marked) {
        return OM_STATUS_OK;
    }
    if (*worklist_count == *worklist_capacity) {
        const size_t capacity = *worklist_capacity == 0 ? 32
            : *worklist_capacity * 2;
        if (capacity < *worklist_capacity
            || capacity > SIZE_MAX / sizeof(**worklist)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_Runtime_Object_Header **items = om_realloc_array(
            *worklist, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        *worklist = items;
        *worklist_capacity = capacity;
    }
    object->fields.marked = true;
    (*worklist)[(*worklist_count)++] = object;
    return OM_STATUS_OK;
}

static Om_Status mark_mailbox_object(
    Om_Execution_Context *context,
    Om_Runtime_Object_Header *object,
    Om_Runtime_Object_Header ***worklist,
    size_t *worklist_count,
    size_t *worklist_capacity)
{
    if (context == NULL || object == NULL
        || object->fields.owner != context) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (object->fields.marked) {
        return OM_STATUS_OK;
    }
    if (*worklist_count == *worklist_capacity) {
        const size_t capacity = *worklist_capacity == 0 ? 32
            : *worklist_capacity * 2;
        if (capacity < *worklist_capacity
            || capacity > SIZE_MAX / sizeof(**worklist)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_Runtime_Object_Header **items = om_realloc_array(
            *worklist, capacity, sizeof(*items));
        if (items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        *worklist = items;
        *worklist_capacity = capacity;
    }
    object->fields.marked = true;
    (*worklist)[(*worklist_count)++] = object;
    return OM_STATUS_OK;
}

typedef struct Om_Runtime_Trace_State {
    Om_Execution_Context *context;
    Om_Runtime_Object_Header ***worklist;
    size_t *worklist_count;
    size_t *worklist_capacity;
} Om_Runtime_Trace_State;

static Om_Status trace_visit_mark(void *managed_value, void *visitor_context)
{
    Om_Runtime_Trace_State *state = visitor_context;
    if (state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return mark_pointer(state->context, managed_value, state->worklist,
                        state->worklist_count, state->worklist_capacity);
}

static Om_Status trace_native_pointer(
    Om_Execution_Context *context,
    const void *value,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context)
{
    if (context == NULL || value == NULL || visitor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Native_Pointer_Body *body = value;
    return body->owner == NULL ? OM_STATUS_OK
        : visitor(body->owner, visitor_context);
}

static void finalize_native_pointer(Om_Runtime_Object_Header *object)
{
    if (object == NULL || object->fields.type == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_NATIVE_POINTER) {
        return;
    }
    Om_Runtime_Native_Pointer_Body *body = object_payload(object);
    Om_Runtime_Native_Pointer_Release release = body->release;
    void *pointer = body->pointer;
    body->release = NULL;
    body->pointer = NULL;
    body->owner = NULL;
    if (release != NULL && pointer != NULL) {
        release(pointer);
    }
}

static Om_Status mark_managed_field(Om_Execution_Context *context,
                                    const void *address,
                                    const Om_Runtime_Type_Descriptor *type,
                                    Om_Runtime_Object_Header ***worklist,
                                    size_t *worklist_count,
                                    size_t *worklist_capacity)
{
    if (type == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->trace != NULL) {
        Om_Runtime_Trace_State state = {
            context, worklist, worklist_count, worklist_capacity,
        };
        const Om_Status status = type->trace(context, address,
            trace_visit_mark, &state);
        if (status != OM_STATUS_OK) {
            return status;
        }
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)address + type->fields[0].offset,
            sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return mark_managed_field(context,
            (const unsigned char *)address + payload->offset,
            payload->type, worklist, worklist_count, worklist_capacity);
    }
    if (!type_is_managed(type)) {
        return OM_STATUS_OK;
    }
    void *pointer = NULL;
    (void)memcpy(&pointer, address, sizeof(pointer));
    return mark_pointer(context, pointer, worklist, worklist_count,
                        worklist_capacity);
}

static Om_Status collect_internal(Om_Execution_Context *context)
{
    if (context == NULL || context->collecting) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    context->collecting = true;
    Om_Runtime_Object_Header **worklist = NULL;
    size_t worklist_count = 0;
    size_t worklist_capacity = 0;
    Om_Status status = OM_STATUS_OK;
    for (Om_Runtime_Root_Frame *frame = context->roots;
         status == OM_STATUS_OK && frame != NULL; frame = frame->previous) {
        for (size_t index = 0; status == OM_STATUS_OK && index < frame->count;
             ++index) {
            status = mark_pointer(context, frame->slots[index], &worklist,
                                  &worklist_count, &worklist_capacity);
        }
    }
    if (status == OM_STATUS_OK && context->is_actor_mailbox
        && context->actor_system != NULL) {
        /* Mailbox heaps are collected while the Actor-system mutex is held;
           queued transit Messages are roots owned by the mailbox itself. */
        for (Om_Runtime_Actor *actor = context->actor_system->actors;
             status == OM_STATUS_OK && actor != NULL; actor = actor->next) {
            for (Om_Runtime_Actor_Message *message = actor->mailbox_head;
                 status == OM_STATUS_OK && message != NULL;
                 message = message->next) {
                /* Queued transit objects are allocated in this heap. Their
                   queue entries already hold the object header address, so
                   avoid a linear heap membership search for every root. */
                status = mark_mailbox_object(context,
                    (Om_Runtime_Object_Header *)message->object, &worklist,
                    &worklist_count, &worklist_capacity);
            }
        }
    }
    size_t cursor = 0;
    while (status == OM_STATUS_OK && cursor < worklist_count) {
        const Om_Runtime_Object_Header *object = worklist[cursor++];
        const Om_Runtime_Type_Descriptor *type = object->fields.type;
        const unsigned char *payload = object_payload_const(object);
        if (type->trace != NULL) {
            Om_Runtime_Trace_State trace_state = {
                context, &worklist, &worklist_count, &worklist_capacity,
            };
            status = type->trace(context, payload, trace_visit_mark,
                                 &trace_state);
            if (status != OM_STATUS_OK) {
                break;
            }
        }
        if (type->kind == OM_RUNTIME_TYPE_RECORD
            || type->kind == OM_RUNTIME_TYPE_MESSAGE) {
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < type->field_count; ++index) {
                const Om_Runtime_Field_Descriptor *field = &type->fields[index];
                status = mark_managed_field(context, payload + field->offset,
                    field->type, &worklist, &worklist_count,
                    &worklist_capacity);
            }
        } else if (type->kind == OM_RUNTIME_TYPE_LIST) {
            const Om_Runtime_List_Body *list = (const Om_Runtime_List_Body *)payload;
            if (type_is_managed(type->element_type)) {
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < list->length; ++index) {
                    status = mark_managed_field(context, &list->items[index],
                        type->element_type, &worklist, &worklist_count,
                        &worklist_capacity);
                }
            }
        } else if (type->kind == OM_RUNTIME_TYPE_MAP) {
            const Om_Runtime_Map_Body *map = (const Om_Runtime_Map_Body *)payload;
            for (size_t index = 0; status == OM_STATUS_OK
                 && index < map->length; ++index) {
                status = mark_managed_field(context, &map->entries[index].key,
                    type->key_type, &worklist, &worklist_count,
                    &worklist_capacity);
                if (status == OM_STATUS_OK) {
                    status = mark_managed_field(context,
                        &map->entries[index].value, type->value_type,
                        &worklist, &worklist_count, &worklist_capacity);
                }
            }
        }
    }
    om_free(worklist);
    if (status != OM_STATUS_OK) {
        for (Om_Runtime_Object_Header *object = context->objects;
             object != NULL; object = object->fields.next) {
            object->fields.marked = false;
        }
        context->collecting = false;
        return status;
    }
    size_t live_bytes = 0;
    Om_Runtime_Object_Header **link = &context->objects;
    while (*link != NULL) {
        Om_Runtime_Object_Header *object = *link;
        if (object->fields.marked) {
            object->fields.marked = false;
            if (object->fields.allocated_size > SIZE_MAX - live_bytes) {
                live_bytes = SIZE_MAX;
            } else {
                live_bytes += object->fields.allocated_size;
            }
            link = &object->fields.next;
        } else {
            *link = object->fields.next;
            finalize_native_pointer(object);
            om_free(object->fields.external_storage);
            om_free(object);
            --context->live_object_count;
        }
    }
    context->allocation_bytes = 0;
    context->collection_threshold = context->minimum_collection_threshold;
    if (live_bytes > SIZE_MAX / 2) {
        context->collection_threshold = SIZE_MAX;
    } else if (live_bytes * 2 > context->collection_threshold) {
        context->collection_threshold = live_bytes * 2;
    }
    ++context->collection_count;
    context->collecting = false;
    return OM_STATUS_OK;
}

Om_Status om_runtime_collect(Om_Execution_Context *context)
{
    return collect_internal(context);
}

size_t om_runtime_live_object_count(const Om_Execution_Context *context)
{
    return context == NULL ? 0 : context->live_object_count;
}

size_t om_runtime_collection_count(const Om_Execution_Context *context)
{
    return context == NULL ? 0 : context->collection_count;
}

Om_Status om_runtime_string_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   Om_Const_Byte_Slice utf8,
                                   void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_STRING
        || (utf8.data == NULL && utf8.length != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Status status = om_utf8_validate(utf8);
    if (status != OM_STATUS_OK) {
        return status;
    }
    size_t payload_size = 0;
    if (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                             utf8.length, &payload_size)
        || !om_checked_size_add(payload_size, 1, &payload_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    status = allocate_object(context, type, payload_size, 0, &object);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_String_Body *string = object_payload(object);
    string->length = utf8.length;
    if (utf8.length != 0) {
        (void)memcpy(string->bytes, utf8.data, utf8.length);
    }
    string->bytes[utf8.length] = '\0';
    *result = object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_create_utf8(Om_Execution_Context *context,
                                        const Om_Runtime_Type_Descriptor *type,
                                        const uint8_t *bytes,
                                        size_t length,
                                        void **result)
{
    return om_runtime_string_create(context, type,
        (Om_Const_Byte_Slice){bytes, length}, result);
}

Om_Status om_runtime_string_create_utf8_default(Om_Execution_Context *context,
                                                const uint8_t *bytes,
                                                size_t length,
                                                void **result)
{
    return om_runtime_string_create_utf8(context, &RUNTIME_STRING_TYPE,
                                         bytes, length, result);
}

Om_Status om_runtime_string_concat(Om_Execution_Context *context,
                                   const void *left,
                                   const void *right,
                                   void **result)
{
    if (context == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_String_View left_view = {0};
    Om_String_View right_view = {0};
    Om_Status status = om_runtime_string_view(context, left, &left_view);
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(context, right, &right_view);
    }
    size_t length = 0;
    size_t payload_size = 0;
    if (status == OM_STATUS_OK
        && (!om_checked_size_add(left_view.length, right_view.length, &length)
            || !om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                   length, &payload_size)
            || !om_checked_size_add(payload_size, 1, &payload_size))) {
        status = OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Object_Header *object = NULL;
    if (status == OM_STATUS_OK) {
        status = allocate_object(context, &RUNTIME_STRING_TYPE, payload_size,
                                 0, &object);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_String_Body *string = object_payload(object);
    string->length = length;
    if (left_view.length != 0) {
        (void)memcpy(string->bytes, left_view.data, left_view.length);
    }
    if (right_view.length != 0) {
        (void)memcpy(string->bytes + left_view.length, right_view.data,
                     right_view.length);
    }
    string->bytes[length] = '\0';
    *result = object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_format_value(Om_Execution_Context *context,
                                         Om_Runtime_Type_Kind kind,
                                         const Om_Runtime_Value *value,
                                         void **result)
{
    if (context == NULL || value == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    static const uint8_t NULL_TEXT[] = "null";
    static const uint8_t UNIT_TEXT[] = "()";
    static const uint8_t TRUE_TEXT[] = "true";
    static const uint8_t FALSE_TEXT[] = "false";
    const uint8_t *bytes = NULL;
    size_t length = 0;
    char buffer[64];
    int written = 0;
    switch (kind) {
    case OM_RUNTIME_TYPE_UNIT:
        bytes = UNIT_TEXT;
        length = sizeof(UNIT_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_NULL:
        bytes = NULL_TEXT;
        length = sizeof(NULL_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_BOOL:
        bytes = value->boolean ? TRUE_TEXT : FALSE_TEXT;
        length = value->boolean ? sizeof(TRUE_TEXT) - 1
                                : sizeof(FALSE_TEXT) - 1;
        break;
    case OM_RUNTIME_TYPE_INT:
        written = snprintf(buffer, sizeof(buffer), "%lld",
                           (long long)value->integer);
        break;
    case OM_RUNTIME_TYPE_FLOAT:
        written = snprintf(buffer, sizeof(buffer), "%.17g", value->floating);
        break;
    case OM_RUNTIME_TYPE_STRING: {
        Om_String_View view = {0};
        const Om_Status status = om_runtime_string_view(context,
            value->managed, &view);
        if (status != OM_STATUS_OK) {
            return status;
        }
        bytes = (const uint8_t *)view.data;
        length = view.length;
        break;
    }
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (kind == OM_RUNTIME_TYPE_INT || kind == OM_RUNTIME_TYPE_FLOAT) {
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        bytes = (const uint8_t *)buffer;
        length = (size_t)written;
    }
    return om_runtime_string_create_utf8_default(context, bytes, length,
                                                  result);
}

Om_Status om_runtime_string_view(const Om_Execution_Context *context,
                                 const void *string,
                                 Om_String_View *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, string);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_STRING) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_String_Body *body = object_payload_const(object);
    *result = (Om_String_View){body->bytes, body->length};
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_cstring_view(const Om_Execution_Context *context,
                                         const void *string,
                                         const uint8_t **bytes,
                                         size_t *length)
{
    if (bytes == NULL || length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = NULL;
    *length = 0;
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (view.length != 0 && memchr(view.data, '\0', view.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *bytes = (const uint8_t *)view.data;
    *length = view.length;
    return OM_STATUS_OK;
}

Om_Status om_runtime_string_create_from_cstring(
    Om_Execution_Context *context,
    const char *cstring,
    void **result)
{
    if (context == NULL || cstring == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t length = strlen(cstring);
    return om_runtime_string_create_utf8_default(context,
        (const uint8_t *)cstring, length, result);
}

Om_Status om_runtime_string_create_from_owned_cstring(
    Om_Execution_Context *context,
    char *cstring,
    Om_Runtime_CString_Release release,
    void **result)
{
    if (release == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (cstring == NULL) {
        if (result != NULL) {
            *result = NULL;
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (result == NULL) {
        release(cstring);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Status status = om_runtime_string_create_from_cstring(
        context, cstring, result);
    release(cstring);
    return status;
}

Om_Status om_runtime_print(Om_Execution_Context *context,
                           const void *string)
{
    if (context == NULL || find_object(context, string) == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string, &view);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if ((view.length != 0 && fwrite(view.data, 1, view.length, stdout) != view.length)
        || fputc('\n', stdout) == EOF) {
        return OM_STATUS_IO_ERROR;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_create(Om_Execution_Context *context,
                                   const Om_Runtime_Type_Descriptor *type,
                                   void **result)
{
    if (context == NULL || result == NULL || !descriptor_valid_for_record(type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type, type->size, 0,
                                             &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

Om_Status om_runtime_record_set(Om_Execution_Context *context,
                                void *record,
                                size_t field_index,
                                Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (!value_matches_type(context, field->type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    store_value((unsigned char *)object_payload(object) + field->offset,
                field->type, value);
    return OM_STATUS_OK;
}

static Om_Status validate_inline_value_data(
    const Om_Execution_Context *context,
    const Om_Runtime_Type_Descriptor *type,
    const void *value,
    size_t depth)
{
    if (context == NULL || type == NULL || value == NULL || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)value + type->fields[0].offset,
            sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return validate_inline_value_data(context, payload->type,
            (const unsigned char *)value + payload->offset, depth + 1);
    }
    if (type_is_managed(type)) {
        void *managed = NULL;
        (void)memcpy(&managed, value, sizeof(managed));
        if (managed == NULL) return OM_STATUS_OK;
        const Om_Runtime_Object_Header *object = find_object(context, managed);
        if (object == NULL || object->fields.type->kind != type->kind
            || (type->kind != OM_RUNTIME_TYPE_STRING
                && type->system_message_id != OM_SYSTEM_MESSAGE_ID_ANY
                && !runtime_descriptor_compatible(type,
                    object->fields.type, 0))) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        return OM_STATUS_OK;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_set_value(Om_Execution_Context *context,
                                      void *record,
                                      size_t field_index,
                                      const void *value)
{
    Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || value == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type->kind != OM_RUNTIME_TYPE_RESULT) {
        Om_Runtime_Value scalar = {0};
        const size_t size = stored_value_size(field->type);
        if (size > sizeof(scalar)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        (void)memcpy(&scalar, value, size);
        return om_runtime_record_set(context, record, field_index, scalar);
    }
    if (!result_descriptor_valid(field->type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status value_status = validate_inline_value_data(context,
        field->type, value, 0);
    if (value_status != OM_STATUS_OK) return value_status;
    (void)memcpy((unsigned char *)object_payload(object) + field->offset,
                 value, field->type->size);
    return OM_STATUS_OK;
}

Om_Status om_runtime_record_get(const Om_Execution_Context *context,
                                const void *record,
                                size_t field_index,
                                void *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, record);
    if (object == NULL || result == NULL
        || !descriptor_valid_for_record(object->fields.type)
        || field_index >= object->fields.type->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Field_Descriptor *field =
        &object->fields.type->fields[field_index];
    if (field->type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(field->type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        (void)memcpy(result,
            (const unsigned char *)object_payload_const(object) + field->offset,
            field->type->size);
        return OM_STATUS_OK;
    }
    const size_t result_size = stored_value_size(field->type);
    if (result_size != 0) {
        (void)memcpy(result,
            (const unsigned char *)object_payload_const(object) + field->offset,
            result_size);
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_create(Om_Execution_Context *context,
                                 const Om_Runtime_Type_Descriptor *type,
                                 void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_LIST || type->element_type == NULL
        || type->element_type->kind > OM_RUNTIME_TYPE_ACTOR_REF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type,
        sizeof(Om_Runtime_List_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

static Om_Status list_reserve(Om_Execution_Context *context,
                              Om_Runtime_Object_Header *object,
                              size_t requested_capacity)
{
    Om_Runtime_List_Body *list = object_payload(object);
    if (requested_capacity <= list->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = list->capacity == 0 ? 4 : list->capacity;
    while (capacity < requested_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = requested_capacity;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*list->items)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t new_bytes = capacity * sizeof(*list->items);
    const size_t old_bytes = list->capacity * sizeof(*list->items);
    Om_Status status = allocation_safepoint(context,
        new_bytes > old_bytes ? new_bytes - old_bytes : 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Value *items = om_realloc_array(list->items, capacity,
                                               sizeof(*items));
    if (items == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    list->items = items;
    list->capacity = capacity;
    object->fields.external_storage = items;
    object->fields.allocated_size += new_bytes - old_bytes;
    context->allocation_bytes += new_bytes - old_bytes;
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_append(Om_Execution_Context *context,
                                 void *list_pointer,
                                 Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->element_type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_List_Body *list = object_payload(object);
    if (list->length == SIZE_MAX) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Status status = list_reserve(context, object, list->length + 1);
    if (status == OM_STATUS_OK) {
        list->items[list->length++] = value;
    }
    return status;
}

Om_Status om_runtime_list_append_value(Om_Execution_Context *context,
                                      void *list,
                                      const Om_Runtime_Value *value)
{
    return value == NULL ? OM_STATUS_INVALID_ARGUMENT
        : om_runtime_list_append(context, list, *value);
}

Om_Status om_runtime_list_get(const Om_Execution_Context *context,
                              const void *list_pointer,
                              size_t index,
                              Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *list = object_payload_const(object);
    if (index >= list->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *result = list->items[index];
    return OM_STATUS_OK;
}

Om_Status om_runtime_list_length(const Om_Execution_Context *context,
                                 const void *list_pointer,
                                 size_t *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, list_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_LIST) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_List_Body *list = object_payload_const(object);
    *result = list->length;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_create(Om_Execution_Context *context,
                                const Om_Runtime_Type_Descriptor *type,
                                void **result)
{
    if (context == NULL || type == NULL || result == NULL
        || type->kind != OM_RUNTIME_TYPE_MAP || type->key_type == NULL
        || type->value_type == NULL
        || (type->key_type->kind != OM_RUNTIME_TYPE_INT
            && type->key_type->kind != OM_RUNTIME_TYPE_BOOL
            && type->key_type->kind != OM_RUNTIME_TYPE_STRING
            && type->key_type->kind != OM_RUNTIME_TYPE_ACTOR_REF)
        || type->value_type->kind > OM_RUNTIME_TYPE_ACTOR_REF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    Om_Runtime_Object_Header *object = NULL;
    const Om_Status status = allocate_object(context, type,
        sizeof(Om_Runtime_Map_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

static Om_Status map_key_equal(const Om_Execution_Context *context,
                               const Om_Runtime_Type_Descriptor *type,
                               Om_Runtime_Value left,
                               Om_Runtime_Value right,
                               bool *equal)
{
    if (type->kind == OM_RUNTIME_TYPE_INT
        || type->kind == OM_RUNTIME_TYPE_ACTOR_REF) {
        *equal = left.integer == right.integer;
    } else if (type->kind == OM_RUNTIME_TYPE_BOOL) {
        *equal = left.boolean == right.boolean;
    } else if (type->kind == OM_RUNTIME_TYPE_STRING) {
        if (left.managed == right.managed) {
            *equal = true;
        } else if (left.managed == NULL || right.managed == NULL) {
            *equal = false;
        } else if (find_object(context, left.managed) == NULL
                   || find_object(context, right.managed) == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        } else {
            Om_String_View left_view = {0};
            Om_String_View right_view = {0};
            Om_Status status = om_runtime_string_view(context, left.managed,
                                                       &left_view);
            if (status == OM_STATUS_OK) {
                status = om_runtime_string_view(context, right.managed,
                                                 &right_view);
            }
            if (status != OM_STATUS_OK) {
                return status;
            }
            *equal = left_view.length == right_view.length
                && (left_view.length == 0
                    || memcmp(left_view.data, right_view.data,
                              left_view.length) == 0);
        }
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_OK;
}

static Om_Status map_find_key(const Om_Execution_Context *context,
                              const Om_Runtime_Object_Header *object,
                              Om_Runtime_Value key,
                              size_t *index,
                              bool *found)
{
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *found = false;
    for (size_t offset = 0; offset < map->length; ++offset) {
        bool equal = false;
        const Om_Status status = map_key_equal(context, type->key_type, key,
            map->entries[offset].key, &equal);
        if (status != OM_STATUS_OK) {
            return status;
        }
        if (equal) {
            *index = offset;
            *found = true;
            return OM_STATUS_OK;
        }
    }
    return OM_STATUS_OK;
}

static Om_Status map_reserve(Om_Execution_Context *context,
                             Om_Runtime_Object_Header *object,
                             size_t requested_capacity)
{
    Om_Runtime_Map_Body *map = object_payload(object);
    if (requested_capacity <= map->capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = map->capacity == 0 ? 4 : map->capacity;
    while (capacity < requested_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = requested_capacity;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*map->entries)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const size_t new_bytes = capacity * sizeof(*map->entries);
    const size_t old_bytes = map->capacity * sizeof(*map->entries);
    Om_Status status = allocation_safepoint(context,
        new_bytes > old_bytes ? new_bytes - old_bytes : 0);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Runtime_Map_Entry *entries = om_realloc_array(map->entries, capacity,
                                                     sizeof(*entries));
    if (entries == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (capacity > map->capacity) {
        (void)memset(entries + map->capacity, 0,
            (capacity - map->capacity) * sizeof(*entries));
    }
    map->entries = entries;
    map->capacity = capacity;
    object->fields.external_storage = entries;
    object->fields.allocated_size += new_bytes - old_bytes;
    context->allocation_bytes += new_bytes - old_bytes;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_set(Om_Execution_Context *context,
                             void *map_pointer,
                             Om_Runtime_Value key,
                             Om_Runtime_Value value)
{
    Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->key_type, key)
        || !value_matches_type(context, type->value_type, value)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t index = 0;
    bool found = false;
    Om_Status status = map_find_key(context, object, key, &index, &found);
    Om_Runtime_Map_Body *map = object_payload(object);
    if (status == OM_STATUS_OK && !found) {
        if (map->length == SIZE_MAX) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        status = map_reserve(context, object, map->length + 1);
        if (status == OM_STATUS_OK) {
            index = map->length++;
        }
    }
    if (status == OM_STATUS_OK) {
        map->entries[index] = (Om_Runtime_Map_Entry){key, value};
    }
    return status;
}

Om_Status om_runtime_map_set_values(Om_Execution_Context *context,
                                    void *map,
                                    const Om_Runtime_Value *key,
                                    const Om_Runtime_Value *value)
{
    if (key == NULL || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return om_runtime_map_set(context, map, *key, *value);
}

Om_Status om_runtime_map_get(const Om_Execution_Context *context,
                             const void *map_pointer,
                             Om_Runtime_Value key,
                             Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *type = object->fields.type;
    if (!value_matches_type(context, type->key_type, key)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t index = 0;
    bool found = false;
    const Om_Status status = map_find_key(context, object, key, &index,
                                           &found);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (!found) {
        return OM_STATUS_NOT_FOUND;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *result = map->entries[index].value;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_get_value(const Om_Execution_Context *context,
                                   const void *map,
                                   const Om_Runtime_Value *key,
                                   Om_Runtime_Value *result)
{
    return key == NULL ? OM_STATUS_INVALID_ARGUMENT
        : om_runtime_map_get(context, map, *key, result);
}

Om_Status om_runtime_map_key_at(const Om_Execution_Context *context,
                                const void *map_pointer,
                                size_t index,
                                Om_Runtime_Value *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    if (index >= map->length) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *result = map->entries[index].key;
    return OM_STATUS_OK;
}

Om_Status om_runtime_map_length(const Om_Execution_Context *context,
                                const void *map_pointer,
                                size_t *result)
{
    const Om_Runtime_Object_Header *object = find_object(context, map_pointer);
    if (object == NULL || result == NULL
        || object->fields.type->kind != OM_RUNTIME_TYPE_MAP) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Map_Body *map = object_payload_const(object);
    *result = map->length;
    return OM_STATUS_OK;
}

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

static Om_Status clone_managed_value(Om_Runtime_Object_Copy_Context *copy,
                                     const Om_Runtime_Type_Descriptor *type,
                                     const void *source,
                                     void *destination,
                                     size_t depth);

static Om_Status clone_object_value(Om_Runtime_Object_Copy_Context *copy,
                                    const Om_Runtime_Object_Header *source,
                                    void **destination,
                                    size_t depth)
{
    if (copy == NULL || source == NULL || destination == NULL || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < copy->pair_count; ++index) {
        if (copy->pairs[index].source == source) {
            *destination = copy->pairs[index].destination;
            return OM_STATUS_OK;
        }
    }
    const Om_Runtime_Type_Descriptor *type = source->fields.type;
    if (type == NULL || type->trace != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    Om_Runtime_Object_Header *object = NULL;
    Om_Status status = OM_STATUS_OK;
    if (type->kind == OM_RUNTIME_TYPE_STRING) {
        const Om_Runtime_String_Body *source_string =
            object_payload_const(source);
        size_t payload_size = 0;
        if (!om_checked_size_add(offsetof(Om_Runtime_String_Body, bytes),
                                 source_string->length + 1, &payload_size)) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        status = allocate_object(copy->destination_context, type,
            payload_size, 0, &object);
        if (status == OM_STATUS_OK) {
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                return OM_STATUS_OUT_OF_MEMORY;
            }
            Om_Runtime_String_Body *destination_string = object_payload(object);
            destination_string->length = source_string->length;
            (void)memcpy(destination_string->bytes, source_string->bytes,
                         source_string->length + 1);
            status = om_runtime_roots_pop(copy->destination_context, root);
        }
    } else if (type->kind == OM_RUNTIME_TYPE_RECORD
               || type->kind == OM_RUNTIME_TYPE_MESSAGE) {
        if (!descriptor_valid_for_record(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type, type->size,
                                 0, &object);
        if (status == OM_STATUS_OK) {
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const unsigned char *source_payload =
                    object_payload_const(source);
                unsigned char *destination_payload = object_payload(object);
                (void)memcpy(destination_payload, source_payload, type->size);
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < type->field_count; ++index) {
                    const Om_Runtime_Field_Descriptor *field =
                        &type->fields[index];
                    status = clone_managed_value(copy, field->type,
                        source_payload + field->offset,
                        destination_payload + field->offset, depth + 1);
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    status = pop_status;
                }
            }
        }
    } else if (type->kind == OM_RUNTIME_TYPE_LIST) {
        if (type->element_type == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type,
            sizeof(Om_Runtime_List_Body), 0, &object);
        if (status == OM_STATUS_OK) {
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Runtime_List_Body *source_list =
                    object_payload_const(source);
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < source_list->length; ++index) {
                    Om_Runtime_Value value = {0};
                    const Om_Runtime_Value source_value =
                        source_list->items[index];
                    if (type_is_managed(type->element_type)) {
                        status = clone_managed_value(copy, type->element_type,
                            &source_value.managed, &value.managed, depth + 1);
                    } else {
                        value = source_value;
                    }
                    void *value_root = value.managed;
                    Om_Runtime_Root_Frame *value_root_frame = NULL;
                    if (status == OM_STATUS_OK
                        && type_is_managed(type->element_type)) {
                        value_root_frame = om_runtime_roots_push(
                            copy->destination_context, &value_root, 1);
                        if (value_root_frame == NULL) {
                            status = OM_STATUS_OUT_OF_MEMORY;
                        }
                    }
                    if (status == OM_STATUS_OK) {
                        status = om_runtime_list_append(copy->destination_context,
                            object, value);
                    }
                    if (value_root_frame != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, value_root_frame);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    status = pop_status;
                }
            }
        }
    } else if (type->kind == OM_RUNTIME_TYPE_MAP) {
        if (type->key_type == NULL || type->value_type == NULL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        status = allocate_object(copy->destination_context, type,
            sizeof(Om_Runtime_Map_Body), 0, &object);
        if (status == OM_STATUS_OK) {
            if (copy->pair_count == copy->pair_capacity) {
                const size_t capacity = copy->pair_capacity == 0 ? 16
                    : copy->pair_capacity * 2;
                if (capacity < copy->pair_capacity
                    || capacity > SIZE_MAX / sizeof(*copy->pairs)) {
                    status = OM_STATUS_OUT_OF_RANGE;
                } else {
                    Om_Runtime_Object_Copy_Pair *pairs = om_realloc_array(
                        copy->pairs, capacity, sizeof(*pairs));
                    if (pairs == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        copy->pairs = pairs;
                        copy->pair_capacity = capacity;
                    }
                }
            }
        }
        if (status == OM_STATUS_OK) {
            copy->pairs[copy->pair_count++] = (Om_Runtime_Object_Copy_Pair){
                source, object,
            };
            void *object_root = object;
            Om_Runtime_Root_Frame *root = om_runtime_roots_push(
                copy->destination_context, &object_root, 1);
            if (root == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
            } else {
                const Om_Runtime_Map_Body *source_map =
                    object_payload_const(source);
                for (size_t index = 0; status == OM_STATUS_OK
                     && index < source_map->length; ++index) {
                    Om_Runtime_Value key = source_map->entries[index].key;
                    Om_Runtime_Value value = source_map->entries[index].value;
                    Om_Runtime_Root_Frame *key_root = NULL;
                    Om_Runtime_Root_Frame *value_root = NULL;
                    if (type_is_managed(type->key_type)) {
                        key.managed = NULL;
                        status = clone_managed_value(copy,
                            type->key_type,
                            &source_map->entries[index].key.managed,
                            &key.managed, depth + 1);
                        if (status == OM_STATUS_OK) {
                            key_root = om_runtime_roots_push(
                                copy->destination_context, &key.managed, 1);
                            if (key_root == NULL) {
                                status = OM_STATUS_OUT_OF_MEMORY;
                            }
                        }
                    }
                    if (status == OM_STATUS_OK
                        && type_is_managed(type->value_type)) {
                        value.managed = NULL;
                        status = clone_managed_value(copy,
                            type->value_type,
                            &source_map->entries[index].value.managed,
                            &value.managed, depth + 1);
                        if (status == OM_STATUS_OK) {
                            value_root = om_runtime_roots_push(
                                copy->destination_context, &value.managed, 1);
                            if (value_root == NULL) {
                                status = OM_STATUS_OUT_OF_MEMORY;
                            }
                        }
                    }
                    if (status == OM_STATUS_OK) {
                        status = om_runtime_map_set(copy->destination_context,
                            object, key, value);
                    }
                    if (value_root != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, value_root);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                    if (key_root != NULL) {
                        const Om_Status pop_status = om_runtime_roots_pop(
                            copy->destination_context, key_root);
                        if (status == OM_STATUS_OK) {
                            status = pop_status;
                        }
                    }
                }
                const Om_Status pop_status = om_runtime_roots_pop(
                    copy->destination_context, root);
                if (status == OM_STATUS_OK) {
                    status = pop_status;
                }
            }
        }
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    if (status == OM_STATUS_OK) {
        *destination = object;
    }
    return status;
}

static Om_Status clone_managed_value(Om_Runtime_Object_Copy_Context *copy,
                                     const Om_Runtime_Type_Descriptor *type,
                                     const void *source,
                                     void *destination,
                                     size_t depth)
{
    if (copy == NULL || type == NULL || source == NULL || destination == NULL
        || depth > 128) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type_is_managed(type)) {
        void *source_object = NULL;
        (void)memcpy(&source_object, source, sizeof(source_object));
        if (source_object == NULL) {
            void *null_object = NULL;
            (void)memcpy(destination, &null_object, sizeof(null_object));
            return OM_STATUS_OK;
        }
        const Om_Runtime_Object_Header *header = find_object(
            copy->source_context, source_object);
        if (header == NULL || !runtime_descriptor_compatible(type,
                header->fields.type, 0)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *cloned_object = NULL;
        Om_Status status = clone_object_value(copy, header, &cloned_object,
                                              depth + 1);
        if (status == OM_STATUS_OK) {
            (void)memcpy(destination, &cloned_object, sizeof(cloned_object));
        }
        return status;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (!result_descriptor_valid(type)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        bool is_ok = false;
        (void)memcpy(&is_ok,
            (const unsigned char *)source + type->fields[0].offset,
            sizeof(is_ok));
        (void)memset(destination, 0, type->size);
        (void)memcpy((unsigned char *)destination + type->fields[0].offset,
            &is_ok, sizeof(is_ok));
        const Om_Runtime_Field_Descriptor *payload =
            &type->fields[is_ok ? 1 : 2];
        return clone_managed_value(copy, payload->type,
            (const unsigned char *)source + payload->offset,
            (unsigned char *)destination + payload->offset, depth + 1);
    }
    if (type->kind == OM_RUNTIME_TYPE_UNIT) {
        return OM_STATUS_OK;
    }
    if (type->kind != OM_RUNTIME_TYPE_NULL
        && type->kind != OM_RUNTIME_TYPE_BOOL
        && type->kind != OM_RUNTIME_TYPE_INT
        && type->kind != OM_RUNTIME_TYPE_ACTOR_REF
        && type->kind != OM_RUNTIME_TYPE_FLOAT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->size != 0) {
        (void)memcpy(destination, source, type->size);
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_object_clone(Om_Execution_Context *source_context,
                                  Om_Execution_Context *destination_context,
                                  const void *source_object,
                                  void **result)
{
    if (source_context == NULL || destination_context == NULL
        || source_object == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    const Om_Runtime_Object_Header *source = find_object(source_context,
                                                          source_object);
    if (source == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Object_Copy_Context copy = {
        .source_context = source_context,
        .destination_context = destination_context,
    };
    const Om_Status status = clone_object_value(&copy, source, result, 0);
    om_free(copy.pairs);
    return status;
}

Om_Status om_runtime_native_pointer_create(
    Om_Execution_Context *context,
    void *pointer,
    Om_Runtime_Native_Pointer_Release release,
    void *owner,
    void **result)
{
    if (context == NULL || result == NULL
        || (owner != NULL && find_object(context, owner) == NULL)) {
        if (release != NULL && pointer != NULL) {
            release(pointer);
        }
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    void *owner_root = owner;
    Om_Runtime_Root_Frame *owner_frame = owner == NULL ? NULL
        : om_runtime_roots_push(context, &owner_root, 1);
    if (owner != NULL && owner_frame == NULL) {
        if (release != NULL && pointer != NULL) {
            release(pointer);
        }
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Runtime_Object_Header *object = NULL;
    Om_Status status = allocate_object(context, &RUNTIME_NATIVE_POINTER_TYPE,
        sizeof(Om_Runtime_Native_Pointer_Body), 0, &object);
    if (status == OM_STATUS_OK) {
        Om_Runtime_Native_Pointer_Body *body = object_payload(object);
        *body = (Om_Runtime_Native_Pointer_Body){
            .pointer = pointer,
            .release = release,
            .owner = owner_root,
        };
    } else if (release != NULL && pointer != NULL) {
        release(pointer);
    }
    if (owner_frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context,
                                                           owner_frame);
        if (status == OM_STATUS_OK && pop_status != OM_STATUS_OK) {
            finalize_native_pointer(object);
            status = pop_status;
        }
    }
    if (status == OM_STATUS_OK) {
        *result = object;
    }
    return status;
}

Om_Status om_runtime_native_pointer_borrow(
    const Om_Execution_Context *context,
    const void *native_pointer,
    void **result)
{
    if (context == NULL || native_pointer == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Object_Header *object = find_object(context,
                                                          native_pointer);
    if (object == NULL || object->fields.type
            != &RUNTIME_NATIVE_POINTER_TYPE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Native_Pointer_Body *body = object_payload_const(object);
    *result = body->pointer;
    return OM_STATUS_OK;
}

Om_Status om_runtime_native_pointer_take(
    Om_Execution_Context *context,
    void *native_pointer,
    void **result)
{
    if (context == NULL || native_pointer == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Object_Header *object = find_object(context, native_pointer);
    if (object == NULL || object->fields.type
            != &RUNTIME_NATIVE_POINTER_TYPE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Native_Pointer_Body *body = object_payload(object);
    if (body->release == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = body->pointer;
    body->pointer = NULL;
    body->release = NULL;
    body->owner = NULL;
    return OM_STATUS_OK;
}

Om_Status om_runtime_object_type(
    const Om_Execution_Context *context,
    const void *object,
    const Om_Runtime_Type_Descriptor **result)
{
    if (context == NULL || object == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Object_Header *header = find_object(context, object);
    if (header == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = header->fields.type;
    return OM_STATUS_OK;
}

static void actor_mutex_lock(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    EnterCriticalSection(&system->mutex);
#else
    (void)pthread_mutex_lock(&system->mutex);
#endif
}

static void actor_mutex_unlock(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    LeaveCriticalSection(&system->mutex);
#else
    (void)pthread_mutex_unlock(&system->mutex);
#endif
}

static void actor_condition_wait(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(&system->condition, &system->mutex,
                                   INFINITE);
#else
    (void)pthread_cond_wait(&system->condition, &system->mutex);
#endif
}

static void actor_condition_signal(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    WakeConditionVariable(&system->condition);
#else
    (void)pthread_cond_signal(&system->condition);
#endif
}

static void actor_condition_broadcast(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&system->condition);
#else
    (void)pthread_cond_broadcast(&system->condition);
#endif
}

static bool actor_sync_init(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    if (!InitializeCriticalSectionEx(&system->mutex, 0, 0)) {
        return false;
    }
    InitializeConditionVariable(&system->condition);
#else
    if (pthread_mutex_init(&system->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&system->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&system->mutex);
        return false;
    }
#endif
    return true;
}

static void actor_sync_destroy(Om_Runtime_Actor_System *system)
{
#if defined(_WIN32)
    DeleteCriticalSection(&system->mutex);
#else
    (void)pthread_cond_destroy(&system->condition);
    (void)pthread_mutex_destroy(&system->mutex);
#endif
}

static Om_Runtime_Actor *actor_find_locked(Om_Runtime_Actor_System *system,
                                           Om_Runtime_Actor_Ref reference)
{
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (actor->reference.id == reference.id) {
            return actor;
        }
    }
    return NULL;
}

static const Om_Runtime_Actor_Handler_Descriptor *actor_handler_for(
    const Om_Runtime_Actor_Descriptor *actor,
    const Om_Runtime_Type_Descriptor *message_type)
{
    for (size_t index = 0; index < actor->handler_count; ++index) {
        const Om_Runtime_Actor_Handler_Descriptor *handler =
            &actor->handlers[index];
        if (handler->message_type->fingerprint == message_type->fingerprint) {
            return handler;
        }
    }
    return NULL;
}

static Om_Status actor_enqueue_object_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *actor,
    void *message_object);
static Om_Status system_task_run(void *context);
static void system_file_complete(void *context, Om_Status task_status);
static void system_task_destroy(void *context);

static const Om_Runtime_Type_Descriptor *actor_system_message_type(
    const Om_Runtime_Actor_System *system,
    Om_System_Message_Id message_id)
{
    if (system == NULL || system->program == NULL) return NULL;
    for (size_t index = 0; index < system->program->type_count; ++index) {
        const Om_Runtime_Type_Descriptor *type = system->program->types[index];
        if (type != NULL && type->system_message_id == message_id) {
            return type;
        }
    }
    return NULL;
}

static Om_Status actor_deliver_result_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_System_Message_Id completion_id,
    bool succeeded,
    const uint8_t *success_bytes,
    size_t success_length,
    const char *error_text)
{

    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Type_Descriptor *completion_type =
        actor_system_message_type(system, completion_id);
    if (completion_type == NULL || completion_type->kind != OM_RUNTIME_TYPE_MESSAGE
        || completion_type->field_count != 1
        || completion_type->fields[0].type == NULL
        || !result_descriptor_valid(completion_type->fields[0].type)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Type_Descriptor *result_type =
        completion_type->fields[0].type;
    const Om_Runtime_Field_Descriptor *payload_field =
        &result_type->fields[succeeded ? 1 : 2];
    if ((succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_UNIT
         && payload_field->type->kind != OM_RUNTIME_TYPE_STRING)
        || (!succeeded && payload_field->type->kind != OM_RUNTIME_TYPE_STRING)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }

    void *completion_object = NULL;
    Om_Status status = om_runtime_record_create(system->mailbox_context,
        completion_type, &completion_object);

    if (status != OM_STATUS_OK) return status;
    void *roots[] = {completion_object, NULL};
    Om_Runtime_Root_Frame *root = om_runtime_roots_push(
        system->mailbox_context, roots, 2);

    if (root == NULL) return OM_STATUS_OUT_OF_MEMORY;

    unsigned char *result_value = om_alloc(result_type->size);
    if (result_value == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    } else {
        (void)memset(result_value, 0, result_type->size);
        (void)memcpy(result_value + result_type->fields[0].offset,
                     &succeeded, sizeof(succeeded));
        if (payload_field->type->kind == OM_RUNTIME_TYPE_STRING) {
            const uint8_t *text = success_bytes;
            size_t length = success_length;
            static const uint8_t fallback_error[] = "system operation failed";
            if (!succeeded) {
                text = error_text == NULL ? fallback_error
                    : (const uint8_t *)error_text;
                length = strlen((const char *)text);
            }
            status = om_runtime_string_create_utf8_default(
                system->mailbox_context, text, length, &roots[1]);

            if (status == OM_STATUS_OK) {
                (void)memcpy(result_value + payload_field->offset, &roots[1],
                             sizeof(roots[1]));
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set_value(system->mailbox_context,
                completion_object, 0, result_value);

        }
    }
    om_free(result_value);
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient,
                                              completion_object);

    }
    const Om_Status pop_status = om_runtime_roots_pop(
        system->mailbox_context, root);

    if (status == OM_STATUS_OK) status = pop_status;
    return status;
}

static Om_Status actor_deliver_message_locked(
    Om_Runtime_Actor_System *system,
    uint64_t recipient_id,
    Om_Execution_Context *source_context,
    const void *message_object)
{
    Om_Runtime_Actor *recipient = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){recipient_id});
    if (recipient == NULL || recipient->terminated
        || recipient->stop_requested) {
        return OM_STATUS_OK;
    }
    const Om_Runtime_Object_Header *source = find_object(source_context,
                                                          message_object);
    if (source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *mailbox_object = NULL;
    Om_Status status = om_runtime_object_clone(source_context,
        system->mailbox_context, message_object, &mailbox_object);
    if (status == OM_STATUS_OK
        && actor_handler_for(recipient->descriptor, source->fields.type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, recipient, mailbox_object);
    }
    return status;
}

static Om_Status actor_send_console_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id != OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
        || message_type->field_count != 2) {
        return OM_STATUS_NOT_FOUND;
    }

    void *text_object = NULL;
    uint64_t callback_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                             &text_object);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &callback_id);
    }
    Om_String_View text = {0};
    if (status == OM_STATUS_OK) {
        status = om_runtime_string_view(source_context, text_object, &text);
    }
    if (status != OM_STATUS_OK) return status;

    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE,
        .recipient_id = callback_id,
        .input_length = text.length,
    };
    if (text.length != 0) {
        task->input_bytes = om_alloc(text.length);
        if (task->input_bytes == NULL) {
            system_task_destroy(task);
            return OM_STATUS_OUT_OF_MEMORY;
        }
        (void)memcpy(task->input_bytes, text.data, text.length);
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

static Om_Status actor_schedule_locked(Om_Runtime_Actor_System *system,
                                       Om_Runtime_Actor *actor)
{
    if (actor->terminated || actor->scheduled || actor->running) {
        return OM_STATUS_OK;
    }
    Om_Runtime_Ready_Actor *ready = om_alloc(sizeof(*ready));
    if (ready == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    ready->actor = actor;
    ready->next = NULL;
    actor->scheduled = true;
    if (system->ready_tail == NULL) {
        system->ready_head = ready;
    } else {
        system->ready_tail->next = ready;
    }
    system->ready_tail = ready;
    actor_condition_signal(system);
    return OM_STATUS_OK;
}

static Om_Status actor_enqueue_object_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *actor,
    void *message_object)
{
    if (actor == NULL || actor->terminated || actor->stop_requested
        || message_object == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_Message *message = om_alloc(sizeof(*message));
    if (message == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *message = (Om_Runtime_Actor_Message){.object = message_object};
    Om_Status status = actor_schedule_locked(system, actor);
    if (status != OM_STATUS_OK) {
        om_free(message);
        return status;
    }
    if (actor->mailbox_tail == NULL) {
        actor->mailbox_head = message;
    } else {
        actor->mailbox_tail->next = message;
    }
    actor->mailbox_tail = message;
    ++system->outstanding_messages;
    /* actor_schedule_locked signals a worker when this actor becomes ready.
       An already scheduled or running actor needs no second wakeup; the
       completion path broadcasts after it drains or reschedules its mailbox. */
    return OM_STATUS_OK;
}

static bool native_callback_message_supported(
    const Om_Runtime_Type_Descriptor *message_type)
{
    if (!descriptor_valid_for_record(message_type)
        || message_type->kind != OM_RUNTIME_TYPE_MESSAGE
        || (message_type->field_count != 0 && message_type->fields == NULL)) {
        return false;
    }
    for (size_t index = 0; index < message_type->field_count; ++index) {
        const Om_Runtime_Type_Descriptor *field_type =
            message_type->fields[index].type;
        if (field_type == NULL
            || (field_type->kind != OM_RUNTIME_TYPE_BOOL
                && field_type->kind != OM_RUNTIME_TYPE_INT
                && field_type->kind != OM_RUNTIME_TYPE_FLOAT)) {
            return false;
        }
    }
    return true;
}

static bool native_callback_address_in_list_locked(
    Om_Runtime_Native_Callback_Function callback,
    const Om_Runtime_Native_Callback_Registration *staged)
{
    for (const Om_Runtime_Native_Callback_Registration *item =
             native_callback_registrations;
         item != NULL; item = item->next) {
        if (item->callback == callback) return true;
    }
    for (const Om_Runtime_Native_Callback_Registration *item = staged;
         item != NULL; item = item->next) {
        if (item->callback == callback) return true;
    }
    return false;
}

Om_Status om_runtime_native_callback_register_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *const *candidate_sets,
    const uint64_t *candidate_counts,
    const Om_Runtime_Type_Descriptor *const *message_types,
    Om_Runtime_Native_Callback_Function *registered_functions)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL || callback_count == 0
        || callback_count > SIZE_MAX
        || candidate_sets == NULL || candidate_counts == NULL
        || message_types == NULL || registered_functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t count = (size_t)callback_count;
    if (count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Registration *)
        || count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Function)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *actor = actor_find_locked(system,
        context->current_actor->reference);
    Om_Status status = system->shutting_down || actor == NULL
            || actor != context->current_actor || actor->terminated
            || actor->stop_requested
        ? OM_STATUS_CANCELLED : OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        const Om_Runtime_Type_Descriptor *message_type = message_types[index];
        if (candidate_sets[index] == NULL || candidate_counts[index] == 0
            || candidate_counts[index] > 4096
            || !native_callback_message_supported(message_type)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else if (actor_handler_for(actor->descriptor, message_type) == NULL) {
            status = OM_STATUS_NOT_FOUND;
        }
    }
    actor_mutex_unlock(system);
    if (status != OM_STATUS_OK) return status;

    Om_Runtime_Native_Callback_Registration **entries = om_alloc_array(
        count, sizeof(*entries));
    Om_Runtime_Native_Callback_Function *selected = om_alloc_array(
        count, sizeof(*selected));
    if (entries == NULL || selected == NULL) {
        om_free(entries);
        om_free(selected);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    (void)memset(entries, 0, count * sizeof(*entries));
    for (size_t index = 0; index < count; ++index) {
        entries[index] = om_alloc(sizeof(*entries[index]));
        if (entries[index] == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        *entries[index] = (Om_Runtime_Native_Callback_Registration){
            .system = system,
            .actor_id = context->current_actor->reference.id,
            .message_type = message_types[index],
            .first_failure = OM_STATUS_OK,
            .active = true,
        };
    }

    native_callback_lock();
    Om_Runtime_Native_Callback_Registration *staged = NULL;
    Om_Runtime_Native_Callback_Registration *staged_tail = NULL;
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        Om_Runtime_Native_Callback_Function callback = NULL;
        for (uint64_t candidate_index = 0;
             candidate_index < candidate_counts[index]; ++candidate_index) {
            Om_Runtime_Native_Callback_Function candidate =
                candidate_sets[index][candidate_index];
            if (candidate != NULL
                && !native_callback_address_in_list_locked(candidate, staged)) {
                callback = candidate;
                break;
            }
        }
        if (callback == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
            break;
        }
        entries[index]->callback = callback;
        selected[index] = callback;
        if (staged_tail == NULL) staged = entries[index];
        else staged_tail->next = entries[index];
        staged_tail = entries[index];
    }
    if (status == OM_STATUS_OK) {
        staged_tail->next = native_callback_registrations;
        native_callback_registrations = staged;
        (void)memcpy(registered_functions, selected,
                     count * sizeof(*registered_functions));
    }
    native_callback_unlock();

    if (status != OM_STATUS_OK) {
        for (size_t index = 0; index < count; ++index) {
            om_free(entries[index]);
        }
    }
    om_free(entries);
    om_free(selected);
    return status;
}

Om_Status om_runtime_native_callback_unregister_many(
    Om_Execution_Context *context,
    uint64_t callback_count,
    const Om_Runtime_Native_Callback_Function *registered_functions)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL || callback_count == 0
        || callback_count > SIZE_MAX || registered_functions == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const size_t count = (size_t)callback_count;
    if (count > SIZE_MAX / sizeof(Om_Runtime_Native_Callback_Registration *)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    Om_Runtime_Native_Callback_Registration **entries = om_alloc_array(
        count, sizeof(*entries));
    if (entries == NULL) return OM_STATUS_OUT_OF_MEMORY;
    (void)memset(entries, 0, count * sizeof(*entries));

    Om_Status status = OM_STATUS_OK;
    bool registrations_removed = false;
    native_callback_lock();
    for (size_t index = 0; status == OM_STATUS_OK && index < count; ++index) {
        for (Om_Runtime_Native_Callback_Registration *item =
                 native_callback_registrations;
             item != NULL; item = item->next) {
            if (item->active && item->callback == registered_functions[index]
                && item->system == context->actor_system
                && item->actor_id == context->current_actor->reference.id) {
                entries[index] = item;
                break;
            }
        }
        if (entries[index] == NULL) status = OM_STATUS_NOT_FOUND;
        for (size_t previous = 0; status == OM_STATUS_OK && previous < index;
             ++previous) {
            if (entries[previous] == entries[index]) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < count; ++index) {
            entries[index]->active = false;
        }
        bool in_flight = false;
        do {
            in_flight = false;
            for (size_t index = 0; index < count; ++index) {
                in_flight = in_flight || entries[index]->in_flight != 0;
            }
            if (in_flight) native_callback_wait();
        } while (in_flight);

        for (size_t index = 0; index < count; ++index) {
            if (status == OM_STATUS_OK
                && entries[index]->first_failure != OM_STATUS_OK) {
                status = entries[index]->first_failure;
            }
            Om_Runtime_Native_Callback_Registration **link =
                &native_callback_registrations;
            while (*link != NULL && *link != entries[index]) {
                link = &(*link)->next;
            }
            if (*link == entries[index]) *link = entries[index]->next;
        }
        registrations_removed = true;
    }
    native_callback_unlock();
    if (registrations_removed) {
        for (size_t index = 0; index < count; ++index) {
            om_free(entries[index]);
        }
    }
    om_free(entries);
    return status;
}

Om_Status om_runtime_native_callback_post(
    Om_Runtime_Native_Callback_Function callback,
    const uint64_t *value_bits,
    uint64_t value_count)
{
    if (callback == NULL || value_count > SIZE_MAX
        || (value_count != 0 && value_bits == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    native_callback_lock();
    Om_Runtime_Native_Callback_Registration *registration =
        native_callback_registrations;
    while (registration != NULL
        && (!registration->active || registration->callback != callback)) {
        registration = registration->next;
    }
    if (registration == NULL) {
        native_callback_unlock();
        return OM_STATUS_NOT_FOUND;
    }
    if (registration->in_flight == SIZE_MAX) {
        native_callback_unlock();
        return OM_STATUS_OUT_OF_RANGE;
    }
    ++registration->in_flight;
    Om_Runtime_Actor_System *system = registration->system;
    const uint64_t actor_id = registration->actor_id;
    const Om_Runtime_Type_Descriptor *message_type =
        registration->message_type;
    native_callback_unlock();

    Om_Status status = OM_STATUS_OK;
    actor_mutex_lock(system);
    Om_Runtime_Actor *actor = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){actor_id});
    if (system->shutting_down || actor == NULL || actor->terminated
        || actor->stop_requested) {
        status = OM_STATUS_CANCELLED;
    } else if (!native_callback_message_supported(message_type)
        || message_type->field_count != (size_t)value_count
        || actor_handler_for(actor->descriptor, message_type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    void *message_object = NULL;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(system->mailbox_context,
            message_type, &message_object);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < (size_t)value_count; ++index) {
        Om_Runtime_Value value = {0};
        switch (message_type->fields[index].type->kind) {
        case OM_RUNTIME_TYPE_BOOL:
            value.boolean = value_bits[index] != 0;
            break;
        case OM_RUNTIME_TYPE_INT:
            (void)memcpy(&value.integer, &value_bits[index],
                         sizeof(value.integer));
            break;
        case OM_RUNTIME_TYPE_FLOAT:
            (void)memcpy(&value.floating, &value_bits[index],
                         sizeof(value.floating));
            break;
        default:
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(system->mailbox_context,
                message_object, index, value);
        }
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, actor, message_object);
    }
    actor_mutex_unlock(system);

    native_callback_lock();
    if (status != OM_STATUS_OK && registration->first_failure == OM_STATUS_OK) {
        registration->first_failure = status;
    }
    --registration->in_flight;
    native_callback_broadcast();
    native_callback_unlock();
    return status;
}

static Om_Status system_task_copy_string(Om_Execution_Context *context,
                                        const void *string_object,
                                        char **result,
                                        size_t *result_length)
{
    if (context == NULL || string_object == NULL || result == NULL
        || result_length == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    *result_length = 0;
    Om_String_View view = {0};
    Om_Status status = om_runtime_string_view(context, string_object, &view);
    if (status != OM_STATUS_OK) return status;
    if (view.length != 0 && memchr(view.data, '\0', view.length) != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (!om_checked_size_add(view.length, 1, &allocation_size)) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    char *copy = om_alloc(allocation_size);
    if (copy == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (view.length != 0) (void)memcpy(copy, view.data, view.length);
    copy[view.length] = '\0';
    *result = copy;
    *result_length = view.length;
    return OM_STATUS_OK;
}

static FILE *system_file_open(const char *path,
                              bool write,
                              Om_Status *status)
{
    if (path == NULL || status == NULL) return NULL;
#if defined(_WIN32)
    const size_t path_length = strlen(path);
    if (path_length >= (size_t)INT_MAX) {
        *status = OM_STATUS_OUT_OF_RANGE;
        return NULL;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, (int)path_length + 1, NULL, 0);
    if (wide_length <= 0) {
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    wchar_t *wide_path = om_alloc_array((size_t)wide_length,
                                         sizeof(*wide_path));
    if (wide_path == NULL) {
        *status = OM_STATUS_OUT_OF_MEMORY;
        return NULL;
    }
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, (int)path_length + 1, wide_path, wide_length);
    if (converted != wide_length) {
        om_free(wide_path);
        *status = OM_STATUS_IO_ERROR;
        return NULL;
    }
    FILE *file = NULL;
    const errno_t open_status = _wfopen_s(&file, wide_path,
                                          write ? L"wb" : L"rb");
    om_free(wide_path);
    *status = open_status != 0 || file == NULL
        ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#else
    FILE *file = fopen(path, write ? "wb" : "rb");
    *status = file == NULL ? OM_STATUS_IO_ERROR : OM_STATUS_OK;
    return file;
#endif
}

static Om_Status system_file_read(Om_Runtime_System_Task *task)
{

    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task->path, false, &status);

    if (file == NULL) return status;
    size_t capacity = 0;
    for (;;) {
        if (task->result_length == capacity) {
            size_t next_capacity = capacity == 0 ? 8192 : capacity;
            if (capacity != 0) {
                if (capacity > SIZE_MAX / 2) {
                    status = OM_STATUS_OUT_OF_RANGE;
                    break;
                }
                next_capacity = capacity * 2;
            }
            uint8_t *next = om_realloc_array(task->result_bytes,
                next_capacity, sizeof(*next));
            if (next == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            task->result_bytes = next;
            capacity = next_capacity;
        }
        const size_t available = capacity - task->result_length;
        const size_t received = fread(task->result_bytes + task->result_length,
            1, available, file);
        task->result_length += received;
        if (ferror(file)) {
            status = OM_STATUS_IO_ERROR;
            break;
        }
        if (feof(file)) break;
        if (received == 0) {
            status = OM_STATUS_IO_ERROR;
            break;
        }
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        status = OM_STATUS_IO_ERROR;
    }

    return status;
}

static Om_Status system_file_write(Om_Runtime_System_Task *task)
{
    Om_Status status = OM_STATUS_OK;
    FILE *file = system_file_open(task->path, true, &status);
    if (file == NULL) return status;
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, file)
            != task->input_length) {
        status = OM_STATUS_IO_ERROR;
    }
    if (status == OM_STATUS_OK && fflush(file) != 0) {
        status = OM_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0 && status == OM_STATUS_OK) {
        status = OM_STATUS_IO_ERROR;
    }
    return status;
}

static Om_Status system_console_write(Om_Runtime_System_Task *task)
{
    if (task->input_length != 0
        && fwrite(task->input_bytes, 1, task->input_length, stdout)
            != task->input_length) {
        return OM_STATUS_IO_ERROR;
    }
    return fflush(stdout) == 0 ? OM_STATUS_OK : OM_STATUS_IO_ERROR;
}

static Om_Status system_task_run(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE) {
        return system_console_write(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ) {
        return system_file_read(task);
    }
    if (task->operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        return system_file_write(task);
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void actor_fail_locked(Om_Runtime_Actor_System *system,
                              uint64_t actor_id)
{
    Om_Runtime_Actor *actor = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){actor_id});
    if (actor == NULL || actor->terminated) return;
    actor->failed = true;
    actor->stop_requested = true;
    if (!actor->running) {
        if (actor_schedule_locked(system, actor) != OM_STATUS_OK) {
            actor->terminated = true;
        }
    }
    actor_condition_broadcast(system);
}

static void system_file_complete(void *context, Om_Status task_status)
{

    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);

    if (!system->shutting_down) {
        if (task_status == OM_STATUS_OK
            && task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
            && om_utf8_validate((Om_Const_Byte_Slice){task->result_bytes,
                task->result_length}) != OM_STATUS_OK) {
            task_status = OM_STATUS_IO_ERROR;
        }
        const Om_System_Message_Id completion_id =
            task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                ? OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE_COMPLETED
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? OM_SYSTEM_MESSAGE_ID_FILE_READ_COMPLETED
                : OM_SYSTEM_MESSAGE_ID_FILE_WRITE_COMPLETED;
        const char *error_text =
            task->operation == OM_SYSTEM_MESSAGE_ID_CONSOLE_WRITE
                ? "console write failed"
            : task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? "file read failed or contents are not valid UTF-8"
                : "file write failed";
        const Om_Status delivery_status = actor_deliver_result_locked(system,
            task->recipient_id, completion_id, task_status == OM_STATUS_OK,
            task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? task->result_bytes : NULL,
            task->operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
                ? task->result_length : 0,
            error_text);

        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

}

static void timer_task_complete(void *context, Om_Status task_status)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL || task->system == NULL) return;
    Om_Runtime_Actor_System *system = task->system;
    actor_mutex_lock(system);
    if (task_status == OM_STATUS_OK && !system->shutting_down) {
        const Om_Status delivery_status = actor_deliver_message_locked(system,
            task->recipient_id, task->payload_context, task->payload_object);
        if (delivery_status != OM_STATUS_OK) {
            actor_fail_locked(system, task->recipient_id);
        }
    }
    if (system->outstanding_messages != 0) {
        --system->outstanding_messages;
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);
}

static void system_task_destroy(void *context)
{
    Om_Runtime_System_Task *task = context;
    if (task == NULL) return;

    om_free(task->path);

    om_free(task->input_bytes);

    om_free(task->result_bytes);

    if (task->payload_context != NULL) {
        om_runtime_context_destroy(task->payload_context);
    }
    om_free(task);

}

static Om_Status actor_send_file_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL) return OM_STATUS_NOT_FOUND;
    const Om_System_Message_Id operation = message_type->system_message_id;
    const size_t expected_fields = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        ? 2 : operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE ? 3 : 0;
    if (expected_fields == 0 || message_type->field_count != expected_fields) {
        return OM_STATUS_NOT_FOUND;
    }
    void *path_object = NULL;
    void *contents_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &path_object);
    if (status == OM_STATUS_OK && operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &contents_object);
    }
    const size_t recipient_field = operation == OM_SYSTEM_MESSAGE_ID_FILE_READ
        ? 1 : 2;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object,
            recipient_field, &recipient_id);
    }
    if (status != OM_STATUS_OK) return status;

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = operation,
        .recipient_id = recipient_id,
    };
    size_t path_length = 0;
    status = system_task_copy_string(source_context, path_object, &task->path,
                                     &path_length);
    if (status == OM_STATUS_OK
        && operation == OM_SYSTEM_MESSAGE_ID_FILE_WRITE) {
        char *contents = NULL;
        status = system_task_copy_string(source_context, contents_object,
                                         &contents, &task->input_length);
        task->input_bytes = (uint8_t *)contents;
    }
    (void)path_length;
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    if (system->blocking_pool == NULL
        || system->outstanding_messages == SIZE_MAX) {
        system_task_destroy(task);
        return system->blocking_pool == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }
    ++system->outstanding_messages;
    status = om_blocking_pool_submit(system->blocking_pool,
        (Om_Blocking_Task){
            .run = system_task_run,
            .complete = system_file_complete,
            .destroy = system_task_destroy,
            .context = task,
        });
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

static Om_Status actor_send_timer_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object)
{
    if (message_type == NULL
        || message_type->system_message_id != OM_SYSTEM_MESSAGE_ID_TIMER_AFTER
        || message_type->field_count != 3) {
        return OM_STATUS_NOT_FOUND;
    }
    int64_t delay = 0;
    void *payload_object = NULL;
    uint64_t recipient_id = 0;
    Om_Status status = om_runtime_record_get(source_context, message_object, 0,
                                              &delay);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 1,
                                       &payload_object);
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_get(source_context, message_object, 2,
                                       &recipient_id);
    }
    if (status != OM_STATUS_OK || delay < 0) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    if (system->timer_service == NULL
        || system->outstanding_messages == SIZE_MAX) {
        return system->timer_service == NULL ? OM_STATUS_CANCELLED
                                             : OM_STATUS_OUT_OF_RANGE;
    }

    Om_Runtime_System_Task *task = om_alloc(sizeof(*task));
    if (task == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *task = (Om_Runtime_System_Task){
        .system = system,
        .operation = OM_SYSTEM_MESSAGE_ID_TIMER_AFTER,
        .recipient_id = recipient_id,
    };
    task->payload_context = om_runtime_context_create(
        system->program->collection_threshold);
    if (task->payload_context == NULL) {
        system_task_destroy(task);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_Runtime_Object_Header *payload = find_object(source_context,
                                                           payload_object);
    if (payload == NULL || payload->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        system_task_destroy(task);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    status = om_runtime_object_clone(source_context, task->payload_context,
                                     payload_object, &task->payload_object);
    if (status != OM_STATUS_OK) {
        system_task_destroy(task);
        return status;
    }
    uint64_t timer_id = 0;
    ++system->outstanding_messages;
    status = om_timer_service_schedule(system->timer_service,
        (Om_Timer_Task){
            .delay_milliseconds = (uint64_t)delay,
            .complete = timer_task_complete,
            .destroy = system_task_destroy,
            .context = task,
        }, &timer_id);
    (void)timer_id;
    if (status != OM_STATUS_OK) {
        --system->outstanding_messages;
        system_task_destroy(task);
        actor_condition_broadcast(system);
    }
    return status;
}

static Om_Status actor_discard_mailbox_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *actor)
{
    Om_Status status = OM_STATUS_OK;
    Om_Runtime_Actor_Message *message = actor->mailbox_head;
    actor->mailbox_head = NULL;
    actor->mailbox_tail = NULL;
    while (message != NULL) {
        Om_Runtime_Actor_Message *next = message->next;
        if (system->outstanding_messages != 0) {
            --system->outstanding_messages;
        } else {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        om_free(message);
        message = next;
    }
    return status;
}

static Om_Status actor_make_down_message_for_id_locked(
    Om_Runtime_Actor_System *system,
    uint64_t actor_id,
    Om_Runtime_Actor_Down_Reason reason,
    void **message_object)
{
    const Om_Runtime_Type_Descriptor *type =
        system->program->actor_down_message_type;
    if (type == NULL || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    void *object = NULL;
    Om_Status status = om_runtime_record_create(system->mailbox_context, type,
                                                &object);
    if (status != OM_STATUS_OK) {
        return status;
    }
    unsigned char *payload = object_payload(find_object(system->mailbox_context,
                                                        object));
    const int64_t reason_value = (int64_t)reason;
    (void)memcpy(payload + system->program->actor_down_actor_id_offset,
                 &actor_id, sizeof(actor_id));
    (void)memcpy(payload + system->program->actor_down_reason_offset,
                 &reason_value, sizeof(reason_value));
    *message_object = object;
    return OM_STATUS_OK;
}

static Om_Status actor_make_down_message_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *target,
    Om_Runtime_Actor_Down_Reason reason,
    void **message_object)
{
    if (target == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return actor_make_down_message_for_id_locked(system,
        target->reference.id, reason, message_object);
}

static Om_Status actor_notify_one_watcher_locked(
    Om_Runtime_Actor_System *system,
    Om_Runtime_Actor *target,
    Om_Runtime_Actor_Ref watcher_reference,
    bool remote_watcher)
{
    if (remote_watcher) {
        if (system->distribution_hooks == NULL
            || system->distribution_hooks->notify_watcher == NULL) {
            return OM_STATUS_NOT_FOUND;
        }
        return system->distribution_hooks->notify_watcher(
            system->distribution_state, system->root_context,
            watcher_reference.id, target->reference.id,
            target->failed ? OM_RUNTIME_ACTOR_DOWN_FAILED
                           : OM_RUNTIME_ACTOR_DOWN_NORMAL);
    }
    Om_Runtime_Actor *watcher = actor_find_locked(system, watcher_reference);
    if (watcher == NULL || watcher->terminated || watcher->stop_requested) {
        return OM_STATUS_OK;
    }
    void *message_object = NULL;
    Om_Status status = actor_make_down_message_locked(system, target,
        target->failed ? OM_RUNTIME_ACTOR_DOWN_FAILED
                       : OM_RUNTIME_ACTOR_DOWN_NORMAL,
        &message_object);
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, watcher, message_object);
    }
    return status;
}

static Om_Status actor_notify_watchers_locked(Om_Runtime_Actor_System *system,
                                              Om_Runtime_Actor *actor)
{
    Om_Status status = OM_STATUS_OK;
    for (Om_Runtime_Actor_Watcher *watcher = actor->watchers;
         watcher != NULL; watcher = watcher->next) {
        if (!watcher->notified) {
            const Om_Status notify_status = actor_notify_one_watcher_locked(
                system, actor, watcher->actor, watcher->remote);
            watcher->notified = true;
            if (status == OM_STATUS_OK) {
                status = notify_status;
            }
        }
    }
    return status;
}

static bool actor_program_contains(const Om_Runtime_Program_Descriptor *program,
                                   const Om_Runtime_Actor_Descriptor *actor)
{
    if (program == NULL || actor == NULL) {
        return false;
    }
    for (size_t index = 0; index < program->actor_count; ++index) {
        if (program->actors[index] == actor) {
            return true;
        }
    }
    return false;
}

static Om_Status actor_program_validate(
    const Om_Runtime_Program_Descriptor *program)
{
    if (program == NULL
        || (program->type_count != 0 && program->types == NULL)
        || (program->actor_count != 0 && program->actors == NULL)
        || (program->source_span_count != 0 && program->source_spans == NULL)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (program->actor_down_message_type != NULL) {
        const Om_Runtime_Type_Descriptor *down =
            program->actor_down_message_type;
        if (down->kind != OM_RUNTIME_TYPE_MESSAGE
            || !descriptor_valid_for_record(down)
            || program->actor_down_actor_id_offset > down->size
            || sizeof(uint64_t) > down->size
                - program->actor_down_actor_id_offset
            || program->actor_down_reason_offset > down->size
            || sizeof(int64_t) > down->size
                - program->actor_down_reason_offset) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
    }
    for (size_t actor_index = 0; actor_index < program->actor_count;
         ++actor_index) {
        const Om_Runtime_Actor_Descriptor *actor = program->actors[actor_index];
        if (actor == NULL || actor->canonical_name == NULL
            || actor->state_type == NULL
            || actor->state_type->kind != OM_RUNTIME_TYPE_RECORD
            || !descriptor_valid_for_record(actor->state_type)
            || (actor->handler_count != 0 && actor->handlers == NULL)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        for (size_t handler_index = 0; handler_index < actor->handler_count;
             ++handler_index) {
            const Om_Runtime_Actor_Handler_Descriptor *handler =
                &actor->handlers[handler_index];
            if (handler->message_type == NULL || handler->handler == NULL
                || handler->message_type->kind != OM_RUNTIME_TYPE_MESSAGE
                || !descriptor_valid_for_record(handler->message_type)) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            for (size_t prior = 0; prior < handler_index; ++prior) {
                if (actor->handlers[prior].message_type->fingerprint
                    == handler->message_type->fingerprint) {
                    return OM_STATUS_INVALID_ARGUMENT;
                }
            }
        }
        for (size_t prior = 0; prior < actor_index; ++prior) {
            if (program->actors[prior]->fingerprint == actor->fingerprint) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    return OM_STATUS_OK;
}

static int actor_state_initializer_entry(Om_Execution_Context *context);

static Om_Status actor_context_initialize_state(Om_Runtime_Actor *actor)
{
    Om_Execution_Context *context = actor->context;
    const Om_Runtime_Actor_Descriptor *descriptor = actor->descriptor;
    Om_Status status = om_runtime_set_source_spans(context,
        actor->context->actor_system->program->source_spans,
        actor->context->actor_system->program->source_span_count);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(context, descriptor->state_type,
                                         &actor->state_object);
    }
    if (status == OM_STATUS_OK) {
        actor->state_root = om_runtime_roots_push(context,
            &actor->state_object, 1);
        if (actor->state_root == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    if (status == OM_STATUS_OK) {
        actor->state_payload = object_payload(find_object(context,
                                                          actor->state_object));
    }
    if (status == OM_STATUS_OK && descriptor->initialize_state != NULL) {
        context->pending_actor_initializer = descriptor->initialize_state;
        context->pending_actor_initializer_state = actor->state_payload;
        context->pending_actor_state_object = actor->state_object;
        context->pending_actor_initializer_status = OM_STATUS_OK;
        const int initializer_result = om_runtime_run(context,
            actor_state_initializer_entry);
        status = context->pending_actor_initializer_status;
        if (initializer_result != 0 && status == OM_STATUS_OK) {
            status = OM_STATUS_CANCELLED;
        }
        context->pending_actor_initializer = NULL;
        context->pending_actor_initializer_state = NULL;
        context->pending_actor_state_object = NULL;
    }
    return status;
}

static int actor_state_initializer_entry(Om_Execution_Context *context)
{
    if (context == NULL || context->pending_actor_initializer == NULL) {
        return 1;
    }
    context->pending_actor_initializer_status =
        context->pending_actor_initializer(context,
            context->pending_actor_initializer_state);
    return context->pending_actor_initializer_status == OM_STATUS_OK ? 0 : 1;
}

static int actor_handler_entry(Om_Execution_Context *context)
{
    Om_Runtime_Actor *actor = context->current_actor;
    if (actor == NULL || context->pending_actor_handler == NULL) {
        context->pending_actor_status = OM_STATUS_INVALID_ARGUMENT;
        return 1;
    }
    context->pending_actor_status = context->pending_actor_handler(context,
        context->pending_actor_state, context->pending_actor_message);
    return context->pending_actor_status == OM_STATUS_OK ? 0 : 1;
}

static Om_Runtime_Actor_Message *actor_pop_message_locked(
    Om_Runtime_Actor *actor)
{
    Om_Runtime_Actor_Message *message = actor->mailbox_head;
    if (message != NULL) {
        actor->mailbox_head = message->next;
        if (actor->mailbox_head == NULL) {
            actor->mailbox_tail = NULL;
        }
        message->next = NULL;
    }
    return message;
}

static void actor_worker_loop(Om_Runtime_Actor_System *system)
{
    for (;;) {
        Om_Execution_Context *reclaim_context = NULL;
        bool reclaim_actor_context = false;
        actor_mutex_lock(system);
        while (system->ready_head == NULL && !system->stopping) {
            actor_condition_wait(system);
        }
        if (system->ready_head == NULL && system->stopping) {
            actor_mutex_unlock(system);
            return;
        }
        Om_Runtime_Ready_Actor *ready = system->ready_head;
        system->ready_head = ready->next;
        if (system->ready_head == NULL) {
            system->ready_tail = NULL;
        }
        Om_Runtime_Actor *actor = ready->actor;

        actor->scheduled = false;
        om_free(ready);
        if (actor->terminated) {
            actor_mutex_unlock(system);
            continue;
        }
        Om_Runtime_Actor_Message *message = actor_pop_message_locked(actor);
        if (message == NULL && !actor->stop_requested) {
            actor_mutex_unlock(system);
            continue;
        }
        void *message_object = NULL;
        const Om_Runtime_Actor_Handler_Descriptor *handler = NULL;
        Om_Runtime_Root_Frame *message_root = NULL;
        bool handler_succeeded = true;
        if (message != NULL) {
            Om_Status status = om_runtime_object_clone(system->mailbox_context,
                actor->context, message->object, &message_object);
            if (status == OM_STATUS_OK) {
                message_root = om_runtime_roots_push(actor->context,
                    &message_object, 1);
                if (message_root == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                }
            }
            const Om_Runtime_Object_Header *object = status == OM_STATUS_OK
                ? find_object(actor->context, message_object) : NULL;
            if (status == OM_STATUS_OK && object == NULL) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                handler = actor_handler_for(actor->descriptor,
                                             object->fields.type);
                if (handler == NULL) {
                    status = OM_STATUS_NOT_FOUND;
                }
            }
            om_free(message);
            if (system->outstanding_messages != 0) {
                --system->outstanding_messages;
            } else {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
            if (status == OM_STATUS_OK) {
                actor->context->current_actor = actor;
                actor->context->current_actor_message_object = message_object;
                actor->context->pending_actor_handler = handler->handler;
                actor->context->pending_actor_state = actor->state_payload;
                actor->context->pending_actor_message =
                    object_payload_const(find_object(actor->context,
                                                     message_object));
                actor->context->pending_actor_status = OM_STATUS_OK;
            } else {
                handler_succeeded = false;
            }
        }
        actor->running = true;
        ++system->running_handlers;
        actor_mutex_unlock(system);

        if (message != NULL && handler_succeeded) {

            const int handler_result = om_runtime_run(actor->context,
                                                       actor_handler_entry);

            handler_succeeded = handler_result == 0
                && actor->context->pending_actor_status == OM_STATUS_OK;
        }
        actor->context->current_actor = NULL;
        actor->context->current_actor_message_object = NULL;
        actor->context->pending_actor_handler = NULL;
        actor->context->pending_actor_state = NULL;
        actor->context->pending_actor_message = NULL;
        if (message_root != NULL) {
            (void)om_runtime_roots_pop(actor->context, message_root);
        }

        actor_mutex_lock(system);
        if (!handler_succeeded) {
            actor->failed = true;
            actor->stop_requested = true;
        }
        Om_Status finish_status = OM_STATUS_OK;
        if (actor->stop_requested || system->shutting_down) {
            (void)actor_discard_mailbox_locked(system, actor);
            reclaim_actor_context = true;
        } else {
            actor->running = false;
            if (actor->mailbox_head != NULL) {
                finish_status = actor_schedule_locked(system, actor);
                if (finish_status != OM_STATUS_OK) {
                    actor->running = true;
                    actor->failed = true;
                    actor->stop_requested = true;
                    (void)actor_discard_mailbox_locked(system, actor);
                    reclaim_actor_context = true;
                }
            }
        }
        if (finish_status != OM_STATUS_OK) {
            actor->failed = true;
        }
        if (reclaim_actor_context) {
            reclaim_context = actor->context;
            actor->context = NULL;
            actor->state_object = NULL;
            actor->state_payload = NULL;
            actor->state_root = NULL;
        } else {
            if (system->running_handlers != 0) {
                --system->running_handlers;
            }
        }
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        if (reclaim_actor_context) {
            /* Native release functions may run here; do not hold the Actor
               system mutex while reclaiming the stopped Actor heap. */

            om_runtime_context_destroy(reclaim_context);

            actor_mutex_lock(system);
            if (system->running_handlers != 0) {
                --system->running_handlers;
            }
            actor->running = false;
            actor->terminated = true;
            if (!system->shutting_down) {
                finish_status = actor_notify_watchers_locked(system, actor);
                if (finish_status != OM_STATUS_OK) {
                    actor->failed = true;
                }
            }
            actor_condition_broadcast(system);
            actor_mutex_unlock(system);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI actor_worker_entry(LPVOID argument)
{
    actor_worker_loop((Om_Runtime_Actor_System *)argument);
    return 0;
}
#else
static void *actor_worker_entry(void *argument)
{
    actor_worker_loop((Om_Runtime_Actor_System *)argument);
    return NULL;
}
#endif

static bool actor_all_terminated_locked(Om_Runtime_Actor_System *system)
{
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (!actor->terminated) {
            return false;
        }
    }
    return system->ready_head == NULL && system->running_handlers == 0
        && system->outstanding_messages == 0;
}

static void destroy_actor_system(Om_Runtime_Actor_System *system)
{
    if (system == NULL) {
        return;
    }
    actor_mutex_lock(system);
    system->shutting_down = true;
    for (Om_Runtime_Actor *actor = system->actors; actor != NULL;
         actor = actor->next) {
        if (actor->terminated) {
            continue;
        }
        actor->stop_requested = true;
        (void)actor_discard_mailbox_locked(system, actor);
        if (!actor->running) {
            const Om_Status status = actor_schedule_locked(system, actor);
            if (status != OM_STATUS_OK) {
                actor->terminated = true;
            }
        }
    }
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

    /* Stop asynchronous producers before waiting for their outstanding work.
       Their cancellation callbacks release the operation counts and do not
       deliver new messages once shutting_down has been set. */
    om_timer_service_destroy(system->timer_service);
    system->timer_service = NULL;
    om_blocking_pool_destroy(system->blocking_pool);
    system->blocking_pool = NULL;

    actor_mutex_lock(system);
    while (!actor_all_terminated_locked(system)) {
        actor_condition_wait(system);
    }
    system->stopping = true;
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);

    if (!system->joined) {
        for (size_t index = 0; index < system->worker_count; ++index) {
#if defined(_WIN32)
            (void)WaitForSingleObject(system->threads[index].native, INFINITE);
            (void)CloseHandle(system->threads[index].native);
#else
            (void)pthread_join(system->threads[index].native, NULL);
#endif
        }
        system->joined = true;
    }
    Om_Runtime_Actor *actor = system->actors;
    while (actor != NULL) {
        Om_Runtime_Actor *next = actor->next;
        Om_Runtime_Actor_Watcher *watcher = actor->watchers;
        while (watcher != NULL) {
            Om_Runtime_Actor_Watcher *next_watcher = watcher->next;
            om_free(watcher);
            watcher = next_watcher;
        }
        om_runtime_context_destroy(actor->context);
        om_free(actor);
        actor = next;
    }
    Om_Runtime_Ready_Actor *ready = system->ready_head;
    while (ready != NULL) {
        Om_Runtime_Ready_Actor *next = ready->next;
        om_free(ready);
        ready = next;
    }
    om_runtime_context_destroy(system->mailbox_context);
    om_free(system->threads);
    actor_sync_destroy(system);
    om_free(system);
}

Om_Status om_runtime_actor_system_start(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    size_t worker_count)
{
    if (context == NULL || context->actor_system != NULL
        || context->current_actor != NULL || worker_count > 64) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Status status = actor_program_validate(program);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (worker_count == 0) {
        worker_count = 4;
    }
    Om_Runtime_Actor_System *system = om_alloc(sizeof(*system));
    if (system == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *system = (Om_Runtime_Actor_System){
        .root_context = context,
        .program = program,
        .worker_count = worker_count,
        .next_actor_id = 1,
    };
    if (!actor_sync_init(system)) {
        om_free(system);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    system->mailbox_context = om_runtime_context_create(
        program->collection_threshold);
    if (system->mailbox_context == NULL) {
        actor_sync_destroy(system);
        om_free(system);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    system->mailbox_context->actor_system = system;
    system->mailbox_context->is_actor_mailbox = true;
    status = om_runtime_set_source_spans(system->mailbox_context,
        program->source_spans, program->source_span_count);
    if (status == OM_STATUS_OK) {
        status = om_blocking_pool_create(&system->blocking_pool,
            worker_count, OM_RUNTIME_SYSTEM_QUEUE_CAPACITY);
    }
    if (status == OM_STATUS_OK) {
        status = om_timer_service_create(&system->timer_service,
            OM_RUNTIME_SYSTEM_QUEUE_CAPACITY);
    }
    system->threads = om_alloc_array(worker_count, sizeof(*system->threads));
    if (status == OM_STATUS_OK && system->threads == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status != OM_STATUS_OK) {
        om_timer_service_destroy(system->timer_service);
        om_blocking_pool_destroy(system->blocking_pool);
        om_runtime_context_destroy(system->mailbox_context);
        actor_sync_destroy(system);
        om_free(system->threads);
        om_free(system);
        return status;
    }
    context->actor_system = system;
    context->owns_actor_system = true;
    if (program->source_spans != NULL || program->source_span_count != 0) {
        status = om_runtime_set_source_spans(context, program->source_spans,
                                             program->source_span_count);
    }
    size_t started = 0;
    for (; status == OM_STATUS_OK && started < worker_count; ++started) {
#if defined(_WIN32)
        system->threads[started].native = CreateThread(NULL, 0,
            actor_worker_entry, system, 0, NULL);
        if (system->threads[started].native == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
#else
        if (pthread_create(&system->threads[started].native, NULL,
                           actor_worker_entry, system) != 0) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
#endif
    }
    if (status != OM_STATUS_OK) {
        actor_mutex_lock(system);
        system->stopping = true;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        for (size_t index = 0; index < started; ++index) {
#if defined(_WIN32)
            (void)WaitForSingleObject(system->threads[index].native, INFINITE);
            (void)CloseHandle(system->threads[index].native);
#else
            (void)pthread_join(system->threads[index].native, NULL);
#endif
        }
        context->actor_system = NULL;
        context->owns_actor_system = false;
        om_timer_service_destroy(system->timer_service);
        om_blocking_pool_destroy(system->blocking_pool);
        om_runtime_context_destroy(system->mailbox_context);
        actor_sync_destroy(system);
        om_free(system->threads);
        om_free(system);
        return status;
    }
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_system_wait(Om_Execution_Context *context)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    while (system->outstanding_messages != 0
           || system->running_handlers != 0) {
        actor_condition_wait(system);
    }
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}

Om_Status om_runtime_distribution_attach(
    Om_Execution_Context *context,
    const Om_Runtime_Distribution_Hooks *hooks,
    void *state)
{
    if (context == NULL || context->actor_system == NULL || hooks == NULL
        || hooks->spawn == NULL || hooks->send == NULL
        || hooks->watch == NULL || hooks->notify_watcher == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    if (system->distribution_hooks != NULL) {
        actor_mutex_unlock(system);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    system->distribution_hooks = hooks;
    system->distribution_state = state;
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}

Om_Status om_runtime_distribution_detach(
    Om_Execution_Context *context,
    void *state)
{
    if (context == NULL || context->actor_system == NULL || state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    const Om_Status status = system->distribution_state != state
        || system->distribution_hooks == NULL
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
    if (status == OM_STATUS_OK) {
        system->distribution_hooks = NULL;
        system->distribution_state = NULL;
        while (system->distribution_hooks_active != 0) {
            actor_condition_wait(system);
        }
    }
    actor_mutex_unlock(system);
    return status;
}

static Om_Status actor_spawn_with_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    bool has_assigned_id,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->actor_system == NULL
        || descriptor == NULL || result == NULL
        || !actor_program_contains(context->actor_system->program, descriptor)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    Om_Runtime_Actor *actor = om_alloc(sizeof(*actor));
    if (actor == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *actor = (Om_Runtime_Actor){.descriptor = descriptor};
    actor->context = om_runtime_context_create(
        system->program->collection_threshold);
    if (actor->context == NULL) {
        om_free(actor);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    actor->context->actor_system = system;
    Om_Status status = actor_context_initialize_state(actor);
    if (status != OM_STATUS_OK) {
        om_runtime_context_destroy(actor->context);
        om_free(actor);
        return status;
    }
    actor_mutex_lock(system);
    if (system->shutting_down) {
        status = OM_STATUS_CANCELLED;
    } else if (has_assigned_id
               && (assigned_id < OM_RUNTIME_REMOTE_ACTOR_ID_BIT
                   || actor_find_locked(system,
                       (Om_Runtime_Actor_Ref){assigned_id}) != NULL)) {
        status = OM_STATUS_INVALID_ARGUMENT;
    } else if (!has_assigned_id
               && (system->next_actor_id == 0
                   || system->next_actor_id
                       >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT)) {
        status = OM_STATUS_OUT_OF_RANGE;
    } else if (has_assigned_id) {
        actor->reference.id = assigned_id;
        actor->next = system->actors;
        system->actors = actor;
        *result = actor->reference;
    } else {
        actor->reference.id = system->next_actor_id++;
        actor->next = system->actors;
        system->actors = actor;
        *result = actor->reference;
    }
    actor_mutex_unlock(system);
    if (status != OM_STATUS_OK) {
        om_runtime_context_destroy(actor->context);
        om_free(actor);
    }
    return status;
}

Om_Status om_runtime_actor_spawn(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    Om_Runtime_Actor_Ref *result)
{
    return actor_spawn_with_id(context, descriptor, false, 0, result);
}

Om_Status om_runtime_actor_spawn_assigned(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t assigned_id,
    Om_Runtime_Actor_Ref *result)
{
    if (assigned_id < OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return actor_spawn_with_id(context, descriptor, true, assigned_id, result);
}

Om_Status om_runtime_actor_spawn_at(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->actor_system == NULL
        || descriptor == NULL || result == NULL
        || !actor_program_contains(context->actor_system->program, descriptor)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    const Om_Runtime_Distribution_Hooks *hooks = system->distribution_hooks;
    void *state = system->distribution_state;
    if (hooks == NULL || hooks->spawn == NULL) {
        actor_mutex_unlock(system);
        return OM_STATUS_NOT_FOUND;
    }
    ++system->distribution_hooks_active;
    actor_mutex_unlock(system);
    const Om_Status status = hooks->spawn(state, context, descriptor,
                                          node_ref, result);
    actor_mutex_lock(system);
    --system->distribution_hooks_active;
    actor_condition_broadcast(system);
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_spawn_id(
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t *result_id)
{
    if (result_id == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Actor_Ref reference = {0};
    const Om_Status status = om_runtime_actor_spawn(context, descriptor,
                                                     &reference);
    if (status == OM_STATUS_OK) *result_id = reference.id;
    return status;
}

Om_Status om_runtime_actor_self(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref *result)
{
    if (context == NULL || context->current_actor == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = context->current_actor->reference;
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_self_id(Om_Execution_Context *context,
                                   uint64_t *result_id)
{
    if (result_id == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Actor_Ref reference = {0};
    const Om_Status status = om_runtime_actor_self(context, &reference);
    if (status == OM_STATUS_OK) *result_id = reference.id;
    return status;
}

Om_Status om_runtime_actor_state_object(Om_Execution_Context *context,
                                        void **result_object)
{
    if (context == NULL || result_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (context->current_actor != NULL) {
        *result_object = context->current_actor->state_object;
    } else if (context->pending_actor_initializer != NULL) {
        *result_object = context->pending_actor_state_object;
    } else {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return *result_object == NULL ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
}

Om_Status om_runtime_actor_message_object(Om_Execution_Context *context,
                                          void **result_object)
{
    if (context == NULL || result_object == NULL
        || context->current_actor == NULL
        || context->current_actor_message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result_object = context->current_actor_message_object;
    return OM_STATUS_OK;
}

Om_Status om_runtime_actor_send(Om_Execution_Context *context,
                                Om_Runtime_Actor_Ref target,
                                const void *message_object)
{
    if (context == NULL || context->actor_system == NULL
        || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    const Om_Runtime_Object_Header *source = find_object(context,
                                                          message_object);
    if (source == NULL || source->fields.type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    actor_mutex_lock(system);
    if (system->shutting_down
        || (context->current_actor != NULL
            && context->current_actor->stop_requested)) {
        actor_mutex_unlock(system);
        return OM_STATUS_CANCELLED;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_CONSOLE) {
        Om_Status status = OM_STATUS_OK;
        status = actor_send_console_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_FILE_SYSTEM) {
        const Om_Status status = actor_send_file_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    if (target.id == OM_SYSTEM_ACTOR_REF_TIMER) {
        const Om_Status status = actor_send_timer_locked(system, context,
            source->fields.type, message_object);
        actor_mutex_unlock(system);
        return status;
    }
    Om_Runtime_Actor *actor = actor_find_locked(system, target);
    if (actor == NULL && target.id >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        const Om_Runtime_Distribution_Hooks *hooks =
            system->distribution_hooks;
        void *distribution_state = system->distribution_state;
        if (hooks != NULL && hooks->send != NULL) {
            ++system->distribution_hooks_active;
        }
        actor_mutex_unlock(system);
        if (hooks == NULL || hooks->send == NULL) return OM_STATUS_NOT_FOUND;
        const Om_Status hook_status = hooks->send(distribution_state, context,
            target.id, message_object);
        actor_mutex_lock(system);
        --system->distribution_hooks_active;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        return hook_status;
    }
    Om_Status status = actor == NULL || actor->terminated || actor->stop_requested
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
    if (status == OM_STATUS_OK && context->current_actor != NULL
        && context->current_actor->stop_requested) {
        status = OM_STATUS_CANCELLED;
    }
    if (status == OM_STATUS_OK
        && actor_handler_for(actor->descriptor, source->fields.type) == NULL) {
        status = OM_STATUS_NOT_FOUND;
    }
    if (status == OM_STATUS_OK) {
        void *mailbox_object = NULL;
        status = om_runtime_object_clone(context, system->mailbox_context,
                                         message_object, &mailbox_object);
        if (status == OM_STATUS_OK) {
            status = actor_enqueue_object_locked(system, actor,
                                                  mailbox_object);
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_send_id(Om_Execution_Context *context,
                                   uint64_t target_id,
                                   const void *message_object)
{
    return om_runtime_actor_send(context,
        (Om_Runtime_Actor_Ref){target_id}, message_object);
}

Om_Status om_runtime_actor_watch(Om_Execution_Context *context,
                                 Om_Runtime_Actor_Ref target)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *target_actor = actor_find_locked(system, target);
    if (target_actor == NULL
        && target.id >= OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        const Om_Runtime_Distribution_Hooks *hooks =
            system->distribution_hooks;
        void *distribution_state = system->distribution_state;
        if (hooks != NULL && hooks->watch != NULL) {
            ++system->distribution_hooks_active;
        }
        actor_mutex_unlock(system);
        if (hooks == NULL || hooks->watch == NULL) return OM_STATUS_NOT_FOUND;
        const Om_Status hook_status = hooks->watch(distribution_state,
            context, target.id);
        actor_mutex_lock(system);
        --system->distribution_hooks_active;
        actor_condition_broadcast(system);
        actor_mutex_unlock(system);
        return hook_status;
    }
    Om_Status status = target_actor == NULL
        ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    Om_Runtime_Actor *watcher = context->current_actor;
    bool already_watching = false;
    if (status == OM_STATUS_OK) {
        for (Om_Runtime_Actor_Watcher *item = target_actor->watchers;
             item != NULL; item = item->next) {
            if (!item->remote
                && item->actor.id == watcher->reference.id) {
                already_watching = true;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK && !already_watching) {
        Om_Runtime_Actor_Watcher *item = om_alloc(sizeof(*item));
        if (item == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            item->actor = watcher->reference;
            item->remote = false;
            item->notified = false;
            item->next = target_actor->watchers;
            target_actor->watchers = item;
            if (target_actor->terminated) {
                status = actor_notify_one_watcher_locked(
                    system, target_actor, watcher->reference, false);
                item->notified = true;
            }
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_watch_remote(
    Om_Execution_Context *context,
    uint64_t target_local_actor_id,
    uint64_t remote_watcher_ref)
{
    if (context == NULL || context->actor_system == NULL
        || remote_watcher_ref < OM_RUNTIME_REMOTE_ACTOR_ID_BIT) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *target = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){target_local_actor_id});
    Om_Status status = target == NULL || target->stop_requested
        ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    bool already_watching = false;
    if (status == OM_STATUS_OK) {
        for (Om_Runtime_Actor_Watcher *item = target->watchers;
             item != NULL; item = item->next) {
            if (item->remote && item->actor.id == remote_watcher_ref) {
                already_watching = true;
                break;
            }
        }
    }
    if (status == OM_STATUS_OK && !already_watching) {
        Om_Runtime_Actor_Watcher *item = om_alloc(sizeof(*item));
        if (item == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        } else {
            *item = (Om_Runtime_Actor_Watcher){
                .actor = {remote_watcher_ref},
                .remote = true,
                .next = target->watchers,
            };
            target->watchers = item;
            if (target->terminated) {
                status = actor_notify_one_watcher_locked(system, target,
                    item->actor, true);
                item->notified = true;
            }
        }
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_post_remote_down(
    Om_Execution_Context *context,
    uint64_t watcher_local_actor_id,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason)
{
    if (context == NULL || context->actor_system == NULL
        || (reason != OM_RUNTIME_ACTOR_DOWN_NORMAL
            && reason != OM_RUNTIME_ACTOR_DOWN_FAILED
            && reason != OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    Om_Runtime_Actor *watcher = actor_find_locked(system,
        (Om_Runtime_Actor_Ref){watcher_local_actor_id});
    Om_Status status = watcher == NULL || watcher->terminated
        || watcher->stop_requested ? OM_STATUS_NOT_FOUND
        : system->program->actor_down_message_type == NULL
            ? OM_STATUS_INVALID_ARGUMENT : OM_STATUS_OK;
    void *message_object = NULL;
    if (status == OM_STATUS_OK) {
        status = actor_make_down_message_for_id_locked(system,
            terminated_actor_id, reason, &message_object);
    }
    if (status == OM_STATUS_OK) {
        status = actor_enqueue_object_locked(system, watcher, message_object);
    }
    actor_mutex_unlock(system);
    return status;
}

Om_Status om_runtime_actor_watch_id(Om_Execution_Context *context,
                                    uint64_t target_id)
{
    return om_runtime_actor_watch(context,
        (Om_Runtime_Actor_Ref){target_id});
}

Om_Status om_runtime_actor_stop(Om_Execution_Context *context)
{
    if (context == NULL || context->actor_system == NULL
        || context->current_actor == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_System *system = context->actor_system;
    actor_mutex_lock(system);
    context->current_actor->stop_requested = true;
    actor_mutex_unlock(system);
    return OM_STATUS_OK;
}
