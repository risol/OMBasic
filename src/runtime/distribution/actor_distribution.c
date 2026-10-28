#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ombasic/runtime/distribution.h"

#include <limits.h>
#include <string.h>
#if defined(OM_BUILD_FUZZERS)
#include <stdio.h>
#endif

#include "ombasic/distribution/serializer.h"
#include "ombasic/distribution/transport.h"
#include "ombasic/foundation/memory.h"
#include "runtime_internal.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define OM_RUNTIME_DISTRIBUTION_REMOTE_BIT (UINT64_C(1) << 63)
#define OM_RUNTIME_DISTRIBUTION_PROXY_BIT  (UINT64_C(1) << 62)
#define OM_RUNTIME_DISTRIBUTION_ID_MASK \
    (OM_RUNTIME_DISTRIBUTION_PROXY_BIT - UINT64_C(1))
#define OM_RUNTIME_DISTRIBUTION_PENDING_LIMIT \
    (OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES * (size_t)4)

#if defined(_WIN32)
typedef CRITICAL_SECTION Om_Dist_Mutex;
typedef CONDITION_VARIABLE Om_Dist_Condition;
typedef HANDLE Om_Dist_Thread;
#define OM_DIST_THREAD(name) static DWORD WINAPI name(LPVOID argument)
#define OM_DIST_THREAD_RETURN return 0
#else
typedef pthread_mutex_t Om_Dist_Mutex;
typedef pthread_cond_t Om_Dist_Condition;
typedef pthread_t Om_Dist_Thread;
#define OM_DIST_THREAD(name) static void *name(void *argument)
#define OM_DIST_THREAD_RETURN return NULL
#endif

typedef struct Om_Runtime_Distribution_State Om_Runtime_Distribution_State;
typedef struct Om_Runtime_Distribution_Peer Om_Runtime_Distribution_Peer;

typedef struct Om_Runtime_Distribution_Queued_Frame {
    Om_Distribution_Frame_Type type;
    uint64_t sender_actor_id;
    uint64_t receiver_actor_id;
    uint64_t fingerprint;
    uint8_t *payload;
    size_t payload_length;
    struct Om_Runtime_Distribution_Queued_Frame *next;
} Om_Runtime_Distribution_Queued_Frame;

typedef struct Om_Runtime_Distribution_Proxy {
    uint64_t handle;
    uint64_t remote_actor_id;
    Om_Runtime_Distribution_Peer *peer;
    struct Om_Runtime_Distribution_Proxy *next;
} Om_Runtime_Distribution_Proxy;

typedef struct Om_Runtime_Distribution_Watch {
    uint64_t local_watcher_id;
    uint64_t remote_actor_id;
    Om_Runtime_Distribution_Peer *peer;
    bool notified;
    struct Om_Runtime_Distribution_Watch *next;
} Om_Runtime_Distribution_Watch;

struct Om_Runtime_Distribution_Peer {
    Om_Runtime_Distribution_State *distribution;
    Om_Distribution_Connection connection;
    Om_Distribution_Node_Id remote_node_id;
    uint64_t node_ref;
    Om_Execution_Context *reader_context;
    Om_Dist_Mutex queue_mutex;
    Om_Dist_Condition queue_condition;
    Om_Runtime_Distribution_Queued_Frame *queue_head;
    Om_Runtime_Distribution_Queued_Frame *queue_tail;
    size_t queued_bytes;
    bool closed;
    bool reader_started;
    bool writer_started;
    Om_Dist_Thread reader_thread;
    Om_Dist_Thread writer_thread;
    struct Om_Runtime_Distribution_Peer *next;
};

struct Om_Runtime_Distribution_State {
    Om_Execution_Context *root_context;
    const Om_Runtime_Program_Descriptor *program;
    Om_Distribution_Node_Id local_node_id;
    Om_Distribution_Listener listener;
    bool listener_open;
    Om_Dist_Mutex mutex;
    Om_Runtime_Distribution_Peer *peers;
    Om_Runtime_Distribution_Proxy *proxies;
    Om_Runtime_Distribution_Watch *watches;
    uint64_t next_node_ref;
    uint64_t next_remote_actor_id;
    uint64_t next_proxy_id;
    bool closing;
};

static void dist_mutex_lock(Om_Dist_Mutex *mutex)
{
#if defined(_WIN32)
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

static void dist_mutex_unlock(Om_Dist_Mutex *mutex)
{
#if defined(_WIN32)
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

static bool dist_mutex_init(Om_Dist_Mutex *mutex)
{
#if defined(_WIN32)
    return InitializeCriticalSectionEx(mutex, 0, 0) != 0;
#else
    return pthread_mutex_init(mutex, NULL) == 0;
#endif
}

static void dist_mutex_destroy(Om_Dist_Mutex *mutex)
{
#if defined(_WIN32)
    DeleteCriticalSection(mutex);
#else
    (void)pthread_mutex_destroy(mutex);
#endif
}

static void dist_condition_signal(Om_Dist_Condition *condition)
{
#if defined(_WIN32)
    WakeConditionVariable(condition);
#else
    (void)pthread_cond_signal(condition);
#endif
}

static void dist_condition_broadcast(Om_Dist_Condition *condition)
{
#if defined(_WIN32)
    WakeAllConditionVariable(condition);
#else
    (void)pthread_cond_broadcast(condition);
#endif
}

static void dist_condition_wait(Om_Dist_Condition *condition,
                                Om_Dist_Mutex *mutex)
{
#if defined(_WIN32)
    (void)SleepConditionVariableCS(condition, mutex, INFINITE);
#else
    (void)pthread_cond_wait(condition, mutex);
#endif
}

static bool dist_condition_init(Om_Dist_Condition *condition)
{
#if defined(_WIN32)
    InitializeConditionVariable(condition);
    return true;
#else
    return pthread_cond_init(condition, NULL) == 0;
#endif
}

static void dist_condition_destroy(Om_Dist_Condition *condition)
{
#if !defined(_WIN32)
    (void)pthread_cond_destroy(condition);
#else
    (void)condition;
#endif
}

#if defined(_WIN32)
typedef DWORD (WINAPI *Om_Dist_Win_Thread_Entry)(LPVOID);
static bool dist_win_thread_start(Om_Dist_Thread *thread,
                                  Om_Dist_Win_Thread_Entry entry,
                                  void *argument)
{
    *thread = CreateThread(NULL, 0, entry, argument, 0, NULL);
    return *thread != NULL;
}
#define OM_DIST_START_THREAD(thread, entry, argument) \
    dist_win_thread_start((thread), (Om_Dist_Win_Thread_Entry)(entry), \
                          (argument))
static void dist_thread_join(Om_Dist_Thread thread)
{
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
}
#else
static bool dist_thread_start(Om_Dist_Thread *thread,
                              void *(*entry)(void *),
                              void *argument)
{
    return pthread_create(thread, NULL, entry, argument) == 0;
}
#define OM_DIST_START_THREAD(thread, entry, argument) \
    dist_thread_start((thread), (entry), (argument))
static void dist_thread_join(Om_Dist_Thread thread)
{
    (void)pthread_join(thread, NULL);
}
#endif

static void queued_frame_destroy(Om_Runtime_Distribution_Queued_Frame *frame)
{
    if (frame == NULL) return;
    om_free(frame->payload);
    om_free(frame);
}

static void peer_signal_closed(Om_Runtime_Distribution_Peer *peer)
{
    dist_mutex_lock(&peer->queue_mutex);
    peer->closed = true;
    dist_condition_broadcast(&peer->queue_condition);
    dist_mutex_unlock(&peer->queue_mutex);
    om_distribution_connection_interrupt(&peer->connection);
}

static Om_Runtime_Distribution_Peer *peer_by_node_ref_locked(
    Om_Runtime_Distribution_State *distribution,
    uint64_t node_ref)
{
    for (Om_Runtime_Distribution_Peer *peer = distribution->peers;
         peer != NULL; peer = peer->next) {
        if (peer->node_ref == node_ref) return peer;
    }
    return NULL;
}

static Om_Runtime_Distribution_Peer *peer_by_node_id_locked(
    Om_Runtime_Distribution_State *distribution,
    Om_Distribution_Node_Id node_id)
{
    for (Om_Runtime_Distribution_Peer *peer = distribution->peers;
         peer != NULL; peer = peer->next) {
        if (om_distribution_node_id_equal(peer->remote_node_id, node_id)) {
            return peer;
        }
    }
    return NULL;
}

static Om_Runtime_Distribution_Proxy *proxy_by_handle_locked(
    Om_Runtime_Distribution_State *distribution,
    uint64_t handle)
{
    for (Om_Runtime_Distribution_Proxy *proxy = distribution->proxies;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->handle == handle) return proxy;
    }
    return NULL;
}

static Om_Runtime_Distribution_Proxy *proxy_by_remote_locked(
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    uint64_t remote_actor_id)
{
    for (Om_Runtime_Distribution_Proxy *proxy = distribution->proxies;
         proxy != NULL; proxy = proxy->next) {
        if (proxy->peer == peer
            && proxy->remote_actor_id == remote_actor_id) return proxy;
    }
    return NULL;
}

static Om_Status proxy_intern(Om_Runtime_Distribution_State *distribution,
                              Om_Runtime_Distribution_Peer *peer,
                              uint64_t remote_actor_id,
                              uint64_t *handle)
{
    if (handle == NULL || remote_actor_id == 0
        || ((remote_actor_id & OM_RUNTIME_DISTRIBUTION_REMOTE_BIT) != 0
            && (remote_actor_id & OM_RUNTIME_DISTRIBUTION_PROXY_BIT) != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Proxy *proxy = proxy_by_remote_locked(
        distribution, peer, remote_actor_id);
    if (proxy != NULL) {
        *handle = proxy->handle;
        dist_mutex_unlock(&distribution->mutex);
        return OM_STATUS_OK;
    }
    if (distribution->next_proxy_id == 0
        || distribution->next_proxy_id > OM_RUNTIME_DISTRIBUTION_ID_MASK) {
        dist_mutex_unlock(&distribution->mutex);
        return OM_STATUS_OUT_OF_RANGE;
    }
    proxy = om_alloc(sizeof(*proxy));
    if (proxy == NULL) {
        dist_mutex_unlock(&distribution->mutex);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    proxy->handle = OM_RUNTIME_DISTRIBUTION_REMOTE_BIT
        | OM_RUNTIME_DISTRIBUTION_PROXY_BIT | distribution->next_proxy_id++;
    proxy->remote_actor_id = remote_actor_id;
    proxy->peer = peer;
    proxy->next = distribution->proxies;
    distribution->proxies = proxy;
    *handle = proxy->handle;
    dist_mutex_unlock(&distribution->mutex);
    return OM_STATUS_OK;
}

static Om_Status queue_frame(Om_Runtime_Distribution_Peer *peer,
                             Om_Distribution_Frame_Type type,
                             uint64_t sender_actor_id,
                             uint64_t receiver_actor_id,
                             uint64_t fingerprint,
                             uint8_t *payload,
                             size_t payload_length)
{
    if (payload_length > OM_DISTRIBUTION_DEFAULT_MAX_PAYLOAD_BYTES
        || (payload_length != 0 && payload == NULL)) {
        om_free(payload);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Distribution_Queued_Frame *frame = om_alloc(sizeof(*frame));
    if (frame == NULL) {
        om_free(payload);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *frame = (Om_Runtime_Distribution_Queued_Frame){
        .type = type,
        .sender_actor_id = sender_actor_id,
        .receiver_actor_id = receiver_actor_id,
        .fingerprint = fingerprint,
        .payload = payload,
        .payload_length = payload_length,
    };
    dist_mutex_lock(&peer->queue_mutex);
    if (peer->closed) {
        dist_mutex_unlock(&peer->queue_mutex);
        queued_frame_destroy(frame);
        return OM_STATUS_IO_ERROR;
    }
    if (payload_length > OM_RUNTIME_DISTRIBUTION_PENDING_LIMIT
        || peer->queued_bytes > OM_RUNTIME_DISTRIBUTION_PENDING_LIMIT
            - payload_length) {
        dist_mutex_unlock(&peer->queue_mutex);
        queued_frame_destroy(frame);
        return OM_STATUS_OUT_OF_RANGE;
    }
    if (peer->queue_tail == NULL) peer->queue_head = frame;
    else peer->queue_tail->next = frame;
    peer->queue_tail = frame;
    peer->queued_bytes += payload_length;
    dist_condition_signal(&peer->queue_condition);
    dist_mutex_unlock(&peer->queue_mutex);
    return OM_STATUS_OK;
}

static Om_Status current_actor_id(Om_Execution_Context *context,
                                  uint64_t *actor_id)
{
    const Om_Status status = om_runtime_actor_self_id(context, actor_id);
    if (status == OM_STATUS_INVALID_ARGUMENT) {
        *actor_id = 0;
        return OM_STATUS_OK;
    }
    return status;
}

static bool runtime_type_is_managed(const Om_Runtime_Type_Descriptor *type)
{
    return type != NULL
        && (type->kind == OM_RUNTIME_TYPE_STRING
            || type->kind == OM_RUNTIME_TYPE_RECORD
            || type->kind == OM_RUNTIME_TYPE_MESSAGE
            || type->kind == OM_RUNTIME_TYPE_LIST
            || type->kind == OM_RUNTIME_TYPE_MAP
            || type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER);
}

static Om_Status runtime_actor_ref_encode(
    Om_Runtime_Distribution_State *distribution,
    uint64_t actor_id,
    Om_Distribution_Actor_Ref *wire_ref)
{
    if (wire_ref == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if ((actor_id & (OM_RUNTIME_DISTRIBUTION_REMOTE_BIT
                     | OM_RUNTIME_DISTRIBUTION_PROXY_BIT))
        == (OM_RUNTIME_DISTRIBUTION_REMOTE_BIT
            | OM_RUNTIME_DISTRIBUTION_PROXY_BIT)) {
        dist_mutex_lock(&distribution->mutex);
        Om_Runtime_Distribution_Proxy *proxy = proxy_by_handle_locked(
            distribution, actor_id);
        if (proxy == NULL) {
            dist_mutex_unlock(&distribution->mutex);
            return OM_STATUS_NOT_FOUND;
        }
        wire_ref->node = proxy->peer->remote_node_id;
        wire_ref->actor_id = proxy->remote_actor_id;
        dist_mutex_unlock(&distribution->mutex);
        return OM_STATUS_OK;
    }
    wire_ref->node = distribution->local_node_id;
    wire_ref->actor_id = actor_id;
    return OM_STATUS_OK;
}

static Om_Status encode_runtime_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    Om_Runtime_Value value,
    Om_Distribution_Value *wire_value);

static Om_Status encode_inline_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    const void *bytes,
    Om_Distribution_Value *wire_value);

static void wire_value_destroy(Om_Distribution_Value *value)
{
    if (value == NULL) return;
    if (value->kind == OM_DISTRIBUTION_VALUE_LIST) {
        Om_Distribution_Value *items =
            (Om_Distribution_Value *)value->as.list.items;
        for (size_t index = 0; index < value->as.list.count; ++index) {
            wire_value_destroy(&items[index]);
        }
        om_free(items);
    } else if (value->kind == OM_DISTRIBUTION_VALUE_MAP) {
        Om_Distribution_Map_Entry *entries =
            (Om_Distribution_Map_Entry *)value->as.map.entries;
        for (size_t index = 0; index < value->as.map.count; ++index) {
            wire_value_destroy(&entries[index].key);
            wire_value_destroy(&entries[index].value);
        }
        om_free(entries);
    } else if (value->kind == OM_DISTRIBUTION_VALUE_RECORD
               || value->kind == OM_DISTRIBUTION_VALUE_MESSAGE) {
        Om_Distribution_Value *fields =
            (Om_Distribution_Value *)value->as.composite.fields;
        for (size_t index = 0; index < value->as.composite.field_count;
             ++index) {
            wire_value_destroy(&fields[index]);
        }
        om_free(fields);
    }
    *value = (Om_Distribution_Value){0};
}

static Om_Status encode_composite_fields(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    const unsigned char *payload,
    Om_Distribution_Value *wire_value,
    bool is_message)
{
    const size_t field_count = type->kind == OM_RUNTIME_TYPE_RESULT
        ? 2 : type->field_count;
    Om_Distribution_Value *fields = field_count == 0 ? NULL
        : om_alloc_array(field_count, sizeof(*fields));
    if (field_count != 0 && fields == NULL) return OM_STATUS_OUT_OF_MEMORY;
    if (field_count != 0) {
        (void)memset(fields, 0, field_count * sizeof(*fields));
    }
    wire_value->kind = is_message ? OM_DISTRIBUTION_VALUE_MESSAGE
                                  : OM_DISTRIBUTION_VALUE_RECORD;
    wire_value->as.composite.schema_fingerprint = type->fingerprint;
    wire_value->as.composite.fields = fields;
    wire_value->as.composite.field_count = field_count;
    Om_Status status = OM_STATUS_OK;
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        if (type->field_count != 3 || type->fields == NULL
            || type->fields[0].type == NULL
            || type->fields[0].type->kind != OM_RUNTIME_TYPE_BOOL) {
            status = OM_STATUS_INVALID_ARGUMENT;
        } else {
            bool is_ok = false;
            (void)memcpy(&is_ok, payload + type->fields[0].offset,
                         sizeof(is_ok));
            fields[0].kind = OM_DISTRIBUTION_VALUE_BOOL;
            fields[0].as.boolean_value = is_ok;
            const Om_Runtime_Field_Descriptor *active =
                &type->fields[is_ok ? 1 : 2];
            status = encode_inline_value(context, distribution, active->type,
                payload + active->offset, &fields[1]);
        }
        if (status != OM_STATUS_OK) wire_value_destroy(wire_value);
        return status;
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
         ++index) {
        const Om_Runtime_Field_Descriptor *field = &type->fields[index];
        if (field->type->kind == OM_RUNTIME_TYPE_RESULT) {
            status = encode_composite_fields(context, distribution,
                field->type, payload + field->offset, &fields[index], false);
        } else {
            Om_Runtime_Value value = {0};
            size_t value_size = runtime_type_is_managed(field->type)
                ? sizeof(void *) : field->type->size;
            if (field->type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (value_size > sizeof(value)) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                (void)memcpy(&value, payload + field->offset, value_size);
                status = encode_runtime_value(context, distribution,
                    field->type, value, &fields[index]);
            }
        }
    }
    if (status != OM_STATUS_OK) wire_value_destroy(wire_value);
    return status;
}

static Om_Status encode_runtime_object(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    const void *object,
    Om_Distribution_Value *wire_value)
{
    if (object == NULL) {
        wire_value->kind = OM_DISTRIBUTION_VALUE_NULL;
        return OM_STATUS_OK;
    }
    switch (type->kind) {
    case OM_RUNTIME_TYPE_STRING: {
        Om_String_View view = {0};
        const Om_Status status = om_runtime_string_view(context, object,
                                                        &view);
        if (status == OM_STATUS_OK) {
            wire_value->kind = OM_DISTRIBUTION_VALUE_STRING;
            wire_value->as.string_value = view;
        }
        return status;
    }
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE: {
        Om_Runtime_Value unused = {0};
        (void)unused;
        const size_t field_count = type->field_count;
        Om_Distribution_Value *fields = field_count == 0 ? NULL
            : om_alloc_array(field_count, sizeof(*fields));
        if (field_count != 0 && fields == NULL) return OM_STATUS_OUT_OF_MEMORY;
        if (field_count != 0) {
            (void)memset(fields, 0, field_count * sizeof(*fields));
        }
        wire_value->kind = type->kind == OM_RUNTIME_TYPE_MESSAGE
            ? OM_DISTRIBUTION_VALUE_MESSAGE : OM_DISTRIBUTION_VALUE_RECORD;
        wire_value->as.composite.schema_fingerprint = type->fingerprint;
        wire_value->as.composite.fields = fields;
        wire_value->as.composite.field_count = field_count;
        Om_Status status = OM_STATUS_OK;
        for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
             ++index) {
            const Om_Runtime_Type_Descriptor *field_type =
                type->fields[index].type;
            if (field_type->kind == OM_RUNTIME_TYPE_RESULT) {
                void *inline_value = om_alloc(field_type->size);
                if (inline_value == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                    break;
                }
                status = om_runtime_record_get(context, object, index,
                                               inline_value);
                if (status == OM_STATUS_OK) {
                    status = encode_composite_fields(context, distribution,
                        field_type, inline_value, &fields[index], false);
                }
                om_free(inline_value);
            } else if (field_type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else {
                Om_Runtime_Value value = {0};
                status = om_runtime_record_get(context, object, index,
                                               &value);
                if (status != OM_STATUS_OK) break;
                status = encode_runtime_value(context, distribution,
                    field_type, value, &fields[index]);
            }
        }
        if (status != OM_STATUS_OK) wire_value_destroy(wire_value);
        return status;
    }
    case OM_RUNTIME_TYPE_LIST: {
        size_t length = 0;
        Om_Status status = om_runtime_list_length(context, object, &length);
        Om_Distribution_Value *items = length == 0 ? NULL
            : om_alloc_array(length, sizeof(*items));
        if (status == OM_STATUS_OK && length != 0 && items == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
        if (status != OM_STATUS_OK) {
            om_free(items);
            return status;
        }
        if (length != 0) (void)memset(items, 0, length * sizeof(*items));
        wire_value->kind = OM_DISTRIBUTION_VALUE_LIST;
        wire_value->as.list.items = items;
        wire_value->as.list.count = length;
        for (size_t index = 0; status == OM_STATUS_OK && index < length;
             ++index) {
            Om_Runtime_Value value = {0};
            status = om_runtime_list_get(context, object, index, &value);
            if (status == OM_STATUS_OK) {
                status = encode_runtime_value(context, distribution,
                    type->element_type, value, &items[index]);
            }
        }
        if (status != OM_STATUS_OK) wire_value_destroy(wire_value);
        return status;
    }
    case OM_RUNTIME_TYPE_MAP: {
        size_t length = 0;
        Om_Status status = om_runtime_map_length(context, object, &length);
        Om_Distribution_Map_Entry *entries = length == 0 ? NULL
            : om_alloc_array(length, sizeof(*entries));
        if (status == OM_STATUS_OK && length != 0 && entries == NULL) {
            status = OM_STATUS_OUT_OF_MEMORY;
        }
        if (status != OM_STATUS_OK) {
            om_free(entries);
            return status;
        }
        if (length != 0) (void)memset(entries, 0, length * sizeof(*entries));
        wire_value->kind = OM_DISTRIBUTION_VALUE_MAP;
        wire_value->as.map.entries = entries;
        wire_value->as.map.count = length;
        for (size_t index = 0; status == OM_STATUS_OK && index < length;
             ++index) {
            Om_Runtime_Value key = {0};
            Om_Runtime_Value value = {0};
            status = om_runtime_map_key_at(context, object, index, &key);
            if (status == OM_STATUS_OK) {
                status = om_runtime_map_get_value(context, object, &key,
                                                   &value);
            }
            if (status == OM_STATUS_OK) {
                status = encode_runtime_value(context, distribution,
                    type->key_type, key, &entries[index].key);
            }
            if (status == OM_STATUS_OK) {
                status = encode_runtime_value(context, distribution,
                    type->value_type, value, &entries[index].value);
            }
        }
        if (status != OM_STATUS_OK) wire_value_destroy(wire_value);
        return status;
    }
    default:
        return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status encode_inline_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    const void *bytes,
    Om_Distribution_Value *wire_value)
{
    if (type == NULL || bytes == NULL || wire_value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        return encode_composite_fields(context, distribution, type, bytes,
                                       wire_value, false);
    }
    if (type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (runtime_type_is_managed(type)) {
        void *object = NULL;
        (void)memcpy(&object, bytes, sizeof(object));
        return encode_runtime_object(context, distribution, type, object,
                                     wire_value);
    }
    Om_Runtime_Value value = {0};
    if (type->size > sizeof(value)) return OM_STATUS_INVALID_ARGUMENT;
    (void)memcpy(&value, bytes, type->size);
    return encode_runtime_value(context, distribution, type, value,
                                wire_value);
}

static Om_Status encode_runtime_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const Om_Runtime_Type_Descriptor *type,
    Om_Runtime_Value value,
    Om_Distribution_Value *wire_value)
{
    if (type == NULL || wire_value == NULL) return OM_STATUS_INVALID_ARGUMENT;
    *wire_value = (Om_Distribution_Value){0};
    switch (type->kind) {
    case OM_RUNTIME_TYPE_UNIT:
        wire_value->kind = OM_DISTRIBUTION_VALUE_UNIT;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_NULL:
        wire_value->kind = OM_DISTRIBUTION_VALUE_NULL;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_BOOL:
        wire_value->kind = OM_DISTRIBUTION_VALUE_BOOL;
        wire_value->as.boolean_value = value.boolean;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_INT:
        wire_value->kind = OM_DISTRIBUTION_VALUE_INT64;
        wire_value->as.integer_value = value.integer;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_FLOAT:
        wire_value->kind = OM_DISTRIBUTION_VALUE_FLOAT64;
        wire_value->as.float_value = value.floating;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_ACTOR_REF:
        wire_value->kind = OM_DISTRIBUTION_VALUE_ACTOR_REF;
        return runtime_actor_ref_encode(distribution, (uint64_t)value.integer,
                                        &wire_value->as.actor_ref);
    case OM_RUNTIME_TYPE_STRING:
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
    case OM_RUNTIME_TYPE_LIST:
    case OM_RUNTIME_TYPE_MAP:
        return encode_runtime_object(context, distribution, type,
                                     value.managed, wire_value);
    case OM_RUNTIME_TYPE_RESULT:
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status encode_message_payload(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    const void *message_object,
    const Om_Runtime_Type_Descriptor *message_type,
    uint8_t **payload,
    size_t *payload_length)
{
    Om_Distribution_Value *fields = message_type->field_count == 0 ? NULL
        : om_alloc_array(message_type->field_count, sizeof(*fields));
    if (message_type->field_count != 0 && fields == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (message_type->field_count != 0) {
        (void)memset(fields, 0,
            message_type->field_count * sizeof(*fields));
    }
    Om_Status status = OM_STATUS_OK;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < message_type->field_count; ++index) {
        const Om_Runtime_Type_Descriptor *field_type =
            message_type->fields[index].type;
        if (field_type->kind == OM_RUNTIME_TYPE_RESULT) {
            void *inline_value = om_alloc(field_type->size);
            if (inline_value == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            status = om_runtime_record_get(context, message_object, index,
                                           inline_value);
            if (status == OM_STATUS_OK) {
                status = encode_inline_value(context, distribution, field_type,
                    inline_value, &fields[index]);
            }
            om_free(inline_value);
        } else {
            Om_Runtime_Value value = {0};
            status = om_runtime_record_get(context, message_object, index,
                                           &value);
            if (status == OM_STATUS_OK) {
                status = encode_runtime_value(context, distribution,
                    field_type, value, &fields[index]);
            }
        }
    }
    Om_Byte_Slice serialized = {0};
    if (status == OM_STATUS_OK) {
        status = om_distribution_message_serialize(fields,
            message_type->field_count,
            (Om_Distribution_Serializer_Limits){
                .maximum_payload_bytes = OM_DISTRIBUTION_DEFAULT_MAX_PAYLOAD_BYTES,
                .maximum_collection_length =
                    OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH,
                .maximum_nesting_depth =
                    OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH,
                .maximum_total_values =
                    OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES,
            }, &serialized);
    }
    for (size_t index = 0; index < message_type->field_count; ++index) {
        wire_value_destroy(&fields[index]);
    }
    om_free(fields);
    if (status != OM_STATUS_OK) {
        om_free(serialized.data);
        return status;
    }
    *payload = serialized.data;
    *payload_length = serialized.length;
    return OM_STATUS_OK;
}

static Om_Status wire_value_count(const Om_Distribution_Value *value,
                                 size_t *count)
{
    if (value == NULL || count == NULL || *count == SIZE_MAX) {
        return OM_STATUS_OVERFLOW;
    }
    ++*count;
    Om_Status status = OM_STATUS_OK;
    if (value->kind == OM_DISTRIBUTION_VALUE_LIST) {
        for (size_t index = 0; status == OM_STATUS_OK
             && index < value->as.list.count; ++index) {
            status = wire_value_count(&value->as.list.items[index], count);
        }
    } else if (value->kind == OM_DISTRIBUTION_VALUE_MAP) {
        for (size_t index = 0; status == OM_STATUS_OK
             && index < value->as.map.count; ++index) {
            status = wire_value_count(&value->as.map.entries[index].key,
                                      count);
            if (status == OM_STATUS_OK) {
                status = wire_value_count(&value->as.map.entries[index].value,
                                          count);
            }
        }
    } else if (value->kind == OM_DISTRIBUTION_VALUE_RECORD
               || value->kind == OM_DISTRIBUTION_VALUE_MESSAGE) {
        for (size_t index = 0; status == OM_STATUS_OK
             && index < value->as.composite.field_count; ++index) {
            status = wire_value_count(&value->as.composite.fields[index],
                                      count);
        }
    }
    return status;
}

static Om_Status decode_object(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    void **result);

static Om_Status decode_runtime_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    Om_Runtime_Value *result);

static Om_Status decode_inline_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    void *result);

static Om_Status decode_result_inline(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    void *result)
{
    if (type == NULL || type->kind != OM_RUNTIME_TYPE_RESULT
        || type->field_count != 3 || type->fields == NULL
        || wire_value == NULL
        || wire_value->kind != OM_DISTRIBUTION_VALUE_RECORD
        || wire_value->as.composite.schema_fingerprint != type->fingerprint
        || wire_value->as.composite.field_count != 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Distribution_Value *wire_tag =
        &wire_value->as.composite.fields[0];
    if (wire_tag->kind != OM_DISTRIBUTION_VALUE_BOOL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const bool is_ok = wire_tag->as.boolean_value;
    const Om_Runtime_Field_Descriptor *tag = &type->fields[0];
    if (tag->type == NULL || tag->type->kind != OM_RUNTIME_TYPE_BOOL
        || tag->offset > type->size
        || sizeof(is_ok) > type->size - tag->offset) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(result, 0, type->size);
    (void)memcpy((unsigned char *)result + tag->offset, &is_ok,
                 sizeof(is_ok));
    const Om_Runtime_Field_Descriptor *payload =
        &type->fields[is_ok ? 1 : 2];
    if (payload->type == NULL || payload->offset > type->size
        || payload->type->size > type->size - payload->offset) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return decode_inline_value(context, distribution, peer, payload->type,
        &wire_value->as.composite.fields[1], roots, root_capacity, root_count,
        (unsigned char *)result + payload->offset);
}

static Om_Status decode_actor_ref(
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Distribution_Value *wire_value,
    uint64_t *actor_ref)
{
    (void)peer;
    if (wire_value == NULL || actor_ref == NULL
        || wire_value->kind != OM_DISTRIBUTION_VALUE_ACTOR_REF) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Distribution_Actor_Ref reference = wire_value->as.actor_ref;
    if (reference.actor_id == 0
        || om_distribution_node_id_equal(reference.node,
                                          distribution->local_node_id)) {
        *actor_ref = reference.actor_id;
        return OM_STATUS_OK;
    }
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Peer *owner = peer_by_node_id_locked(
        distribution, reference.node);
    dist_mutex_unlock(&distribution->mutex);
    if (owner == NULL) return OM_STATUS_NOT_FOUND;
    return proxy_intern(distribution, owner, reference.actor_id, actor_ref);
}

static Om_Status decode_runtime_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    Om_Runtime_Value *result)
{
    if (type == NULL || wire_value == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = (Om_Runtime_Value){0};
    switch (type->kind) {
    case OM_RUNTIME_TYPE_UNIT:
        return wire_value->kind == OM_DISTRIBUTION_VALUE_UNIT
            ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
    case OM_RUNTIME_TYPE_NULL:
        return wire_value->kind == OM_DISTRIBUTION_VALUE_NULL
            ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
    case OM_RUNTIME_TYPE_BOOL:
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_BOOL) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        result->boolean = wire_value->as.boolean_value;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_INT:
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_INT64) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        result->integer = wire_value->as.integer_value;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_FLOAT:
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_FLOAT64) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        result->floating = wire_value->as.float_value;
        return OM_STATUS_OK;
    case OM_RUNTIME_TYPE_ACTOR_REF: {
        uint64_t actor_ref = 0;
        const Om_Status status = decode_actor_ref(distribution, peer,
            wire_value, &actor_ref);
        if (status == OM_STATUS_OK) result->integer = (int64_t)actor_ref;
        return status;
    }
    case OM_RUNTIME_TYPE_STRING:
    case OM_RUNTIME_TYPE_RECORD:
    case OM_RUNTIME_TYPE_MESSAGE:
    case OM_RUNTIME_TYPE_LIST:
    case OM_RUNTIME_TYPE_MAP: {
        void *object = NULL;
        const Om_Status status = decode_object(context, distribution, peer,
            type, wire_value, roots, root_capacity, root_count, &object);
        if (status == OM_STATUS_OK) result->managed = object;
        return status;
    }
    case OM_RUNTIME_TYPE_RESULT:
    case OM_RUNTIME_TYPE_NATIVE_POINTER:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status decode_inline_value(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    void *result)
{
    if (type == NULL || wire_value == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->kind == OM_RUNTIME_TYPE_RESULT) {
        return decode_result_inline(context, distribution, peer, type,
            wire_value, roots, root_capacity, root_count, result);
    }
    if (type->kind == OM_RUNTIME_TYPE_NATIVE_POINTER) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Value value = {0};
    const Om_Status status = decode_runtime_value(context, distribution, peer,
        type, wire_value, roots, root_capacity, root_count, &value);
    if (status != OM_STATUS_OK) return status;
    const size_t value_size = runtime_type_is_managed(type)
        ? sizeof(void *) : type->size;
    if (value_size > sizeof(value)) return OM_STATUS_INVALID_ARGUMENT;
    (void)memcpy(result, &value, value_size);
    return OM_STATUS_OK;
}

static Om_Status root_managed_value(void **roots,
                                    size_t root_capacity,
                                    size_t *root_count,
                                    void *object)
{
    if (object == NULL) return OM_STATUS_OK;
    if (*root_count >= root_capacity) return OM_STATUS_OUT_OF_RANGE;
    roots[(*root_count)++] = object;
    return OM_STATUS_OK;
}

static Om_Status decode_object(
    Om_Execution_Context *context,
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer,
    const Om_Runtime_Type_Descriptor *type,
    const Om_Distribution_Value *wire_value,
    void **roots,
    size_t root_capacity,
    size_t *root_count,
    void **result)
{
    if (type == NULL || wire_value == NULL || result == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    if (wire_value->kind == OM_DISTRIBUTION_VALUE_NULL) {
        return runtime_type_is_managed(type) ? OM_STATUS_OK
                                             : OM_STATUS_INVALID_ARGUMENT;
    }
    if (type->kind == OM_RUNTIME_TYPE_STRING) {
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_STRING) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *string = NULL;
        Om_Status status = om_runtime_string_create_utf8(context, type,
            (const uint8_t *)wire_value->as.string_value.data,
            wire_value->as.string_value.length, &string);
        if (status == OM_STATUS_OK) {
            status = root_managed_value(roots, root_capacity, root_count,
                                        string);
        }
        if (status == OM_STATUS_OK) *result = string;
        return status;
    }
    if (type->kind == OM_RUNTIME_TYPE_RECORD
        || type->kind == OM_RUNTIME_TYPE_MESSAGE) {
        if ((type->kind == OM_RUNTIME_TYPE_MESSAGE
                && wire_value->kind != OM_DISTRIBUTION_VALUE_MESSAGE)
            || (type->kind == OM_RUNTIME_TYPE_RECORD
                && wire_value->kind != OM_DISTRIBUTION_VALUE_RECORD)
            || wire_value->as.composite.schema_fingerprint != type->fingerprint
            || wire_value->as.composite.field_count != type->field_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *record = NULL;
        Om_Status status = om_runtime_record_create(context, type, &record);
        if (status == OM_STATUS_OK) {
            status = root_managed_value(roots, root_capacity, root_count,
                                        record);
        }
        if (status != OM_STATUS_OK) return status;
        const Om_Distribution_Value *fields =
            wire_value->as.composite.fields;
        for (size_t index = 0; status == OM_STATUS_OK
             && index < type->field_count; ++index) {
            const Om_Runtime_Type_Descriptor *field_type =
                type->fields[index].type;
            if (field_type->kind == OM_RUNTIME_TYPE_RESULT) {
                void *inline_value = om_alloc(field_type->size);
                if (inline_value == NULL) {
                    status = OM_STATUS_OUT_OF_MEMORY;
                    break;
                }
                status = decode_result_inline(context, distribution, peer,
                    field_type, &fields[index], roots, root_capacity,
                    root_count, inline_value);
                if (status == OM_STATUS_OK) {
                    status = om_runtime_record_set_value(context, record,
                        index, inline_value);
                }
                om_free(inline_value);
            } else {
                Om_Runtime_Value value = {0};
                status = decode_runtime_value(context, distribution, peer,
                    field_type, &fields[index], roots, root_capacity,
                    root_count, &value);
                if (status == OM_STATUS_OK) {
                    status = om_runtime_record_set(context, record, index,
                                                   value);
                }
            }
        }
        if (status == OM_STATUS_OK) *result = record;
        return status;
    }
    if (type->kind == OM_RUNTIME_TYPE_LIST) {
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_LIST) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *list = NULL;
        Om_Status status = om_runtime_list_create(context, type, &list);
        if (status == OM_STATUS_OK) {
            status = root_managed_value(roots, root_capacity, root_count, list);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < wire_value->as.list.count; ++index) {
            Om_Runtime_Value value = {0};
            status = decode_runtime_value(context, distribution, peer,
                type->element_type, &wire_value->as.list.items[index], roots,
                root_capacity, root_count, &value);
            if (status == OM_STATUS_OK) {
                status = om_runtime_list_append_value(context, list, &value);
            }
        }
        if (status == OM_STATUS_OK) *result = list;
        return status;
    }
    if (type->kind == OM_RUNTIME_TYPE_MAP) {
        if (wire_value->kind != OM_DISTRIBUTION_VALUE_MAP) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        void *map = NULL;
        Om_Status status = om_runtime_map_create(context, type, &map);
        if (status == OM_STATUS_OK) {
            status = root_managed_value(roots, root_capacity, root_count, map);
        }
        for (size_t index = 0; status == OM_STATUS_OK
             && index < wire_value->as.map.count; ++index) {
            Om_Runtime_Value key = {0};
            Om_Runtime_Value value = {0};
            status = decode_runtime_value(context, distribution, peer,
                type->key_type, &wire_value->as.map.entries[index].key, roots,
                root_capacity, root_count, &key);
            if (status == OM_STATUS_OK) {
                status = decode_runtime_value(context, distribution, peer,
                    type->value_type,
                    &wire_value->as.map.entries[index].value, roots,
                    root_capacity, root_count, &value);
            }
            if (status == OM_STATUS_OK) {
                status = om_runtime_map_set_values(context, map, &key, &value);
            }
        }
        if (status == OM_STATUS_OK) *result = map;
        return status;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static const Om_Runtime_Type_Descriptor *message_type_by_fingerprint(
    const Om_Runtime_Program_Descriptor *program,
    uint64_t fingerprint)
{
    for (size_t index = 0; index < program->type_count; ++index) {
        const Om_Runtime_Type_Descriptor *type = program->types[index];
        if (type != NULL && type->kind == OM_RUNTIME_TYPE_MESSAGE
            && type->fingerprint == fingerprint) return type;
    }
    return NULL;
}

static const Om_Runtime_Actor_Descriptor *actor_by_fingerprint(
    const Om_Runtime_Program_Descriptor *program,
    uint64_t fingerprint)
{
    for (size_t index = 0; index < program->actor_count; ++index) {
        const Om_Runtime_Actor_Descriptor *actor = program->actors[index];
        if (actor != NULL && actor->fingerprint == fingerprint) return actor;
    }
    return NULL;
}

static Om_Status dispatch_message(
    Om_Runtime_Distribution_Peer *peer,
    const Om_Distribution_Frame *frame)
{
    Om_Runtime_Distribution_State *distribution = peer->distribution;
    const Om_Runtime_Type_Descriptor *message_type =
        message_type_by_fingerprint(distribution->program,
                                    frame->message_fingerprint);
    if (message_type == NULL) return OM_STATUS_NOT_FOUND;
    Om_Distribution_Message message = {0};
    Om_Status status = om_distribution_message_deserialize(frame->payload,
        frame->message_fingerprint, message_type->fingerprint,
        message_type->field_count,
        (Om_Distribution_Serializer_Limits){
            .maximum_payload_bytes = OM_DISTRIBUTION_DEFAULT_MAX_PAYLOAD_BYTES,
            .maximum_collection_length =
                OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH,
            .maximum_nesting_depth =
                OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH,
            .maximum_total_values =
                OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES,
        }, &message);
    size_t root_capacity = 1;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < message.field_count; ++index) {
        status = wire_value_count(&message.fields[index], &root_capacity);
    }
    void **roots = status == OM_STATUS_OK
        ? om_alloc_array(root_capacity, sizeof(*roots)) : NULL;
    if (status == OM_STATUS_OK && roots == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        (void)memset(roots, 0, root_capacity * sizeof(*roots));
    }
    Om_Runtime_Root_Frame *root_frame = status == OM_STATUS_OK
        ? om_runtime_roots_push(peer->reader_context, roots, root_capacity)
        : NULL;
    if (status == OM_STATUS_OK && root_frame == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    void *runtime_message = NULL;
    size_t root_count = 0;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(peer->reader_context, message_type,
                                          &runtime_message);
    }
    if (status == OM_STATUS_OK) {
        status = root_managed_value(roots, root_capacity, &root_count,
                                    runtime_message);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < message.field_count; ++index) {
        const Om_Runtime_Type_Descriptor *field_type =
            message_type->fields[index].type;
        if (field_type->kind == OM_RUNTIME_TYPE_RESULT) {
            void *inline_value = om_alloc(field_type->size);
            if (inline_value == NULL) {
                status = OM_STATUS_OUT_OF_MEMORY;
                break;
            }
            status = decode_result_inline(peer->reader_context, distribution,
                peer, field_type, &message.fields[index], roots, root_capacity,
                &root_count, inline_value);
            if (status == OM_STATUS_OK) {
                status = om_runtime_record_set_value(peer->reader_context,
                    runtime_message, index, inline_value);
            }
            om_free(inline_value);
        } else {
            Om_Runtime_Value value = {0};
            status = decode_runtime_value(peer->reader_context, distribution,
                peer, field_type, &message.fields[index], roots, root_capacity,
                &root_count, &value);
            if (status == OM_STATUS_OK) {
                status = om_runtime_record_set(peer->reader_context,
                    runtime_message, index, value);
            }
        }
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_actor_send_id(peer->reader_context,
            frame->receiver.actor_id, runtime_message);
    }
    if (root_frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(peer->reader_context,
                                                          root_frame);
        if (status == OM_STATUS_OK) status = pop_status;
    }
    om_free(roots);
    om_distribution_message_destroy(&message);
    return status;
}

static Om_Status send_one_byte_frame(Om_Runtime_Distribution_Peer *peer,
                                     Om_Distribution_Frame_Type type,
                                     uint64_t sender_actor_id,
                                     uint64_t receiver_actor_id,
                                     uint64_t fingerprint,
                                     uint8_t byte)
{
    uint8_t *payload = om_alloc(sizeof(*payload));
    if (payload == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *payload = byte;
    return queue_frame(peer, type, sender_actor_id, receiver_actor_id,
                       fingerprint, payload, sizeof(*payload));
}

static Om_Status dispatch_frame(Om_Runtime_Distribution_Peer *peer,
                                const Om_Distribution_Frame *frame)
{
    Om_Runtime_Distribution_State *distribution = peer->distribution;
    if (!om_distribution_node_id_equal(frame->sender.node,
                                        peer->remote_node_id)
        || !om_distribution_node_id_equal(frame->receiver.node,
                                           distribution->local_node_id)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    switch (frame->type) {
    case OM_DISTRIBUTION_FRAME_MESSAGE:
        return dispatch_message(peer, frame);
    case OM_DISTRIBUTION_FRAME_SPAWN: {
        const Om_Runtime_Actor_Descriptor *actor = actor_by_fingerprint(
            distribution->program, frame->message_fingerprint);
        Om_Runtime_Actor_Ref spawned = {0};
        Om_Status status = actor == NULL
            || frame->receiver.actor_id < OM_RUNTIME_DISTRIBUTION_REMOTE_BIT
            || (frame->receiver.actor_id & OM_RUNTIME_DISTRIBUTION_PROXY_BIT)
                != 0
            ? OM_STATUS_NOT_FOUND
            : om_runtime_actor_spawn_assigned(peer->reader_context, actor,
                frame->receiver.actor_id, &spawned);
        return send_one_byte_frame(peer,
            OM_DISTRIBUTION_FRAME_SPAWN_ACK, frame->receiver.actor_id,
            frame->sender.actor_id, frame->message_fingerprint,
            (uint8_t)status);
    }
    case OM_DISTRIBUTION_FRAME_SPAWN_ACK:
        return frame->payload.length == 1 ? OM_STATUS_OK
                                          : OM_STATUS_INVALID_ARGUMENT;
    case OM_DISTRIBUTION_FRAME_WATCH: {
        uint64_t remote_watcher = 0;
        Om_Status status = proxy_intern(distribution, peer,
            frame->sender.actor_id, &remote_watcher);
        if (status == OM_STATUS_OK) {
            status = om_runtime_actor_watch_remote(peer->reader_context,
                frame->receiver.actor_id, remote_watcher);
        }
        return send_one_byte_frame(peer, OM_DISTRIBUTION_FRAME_WATCH_ACK,
            frame->receiver.actor_id, frame->sender.actor_id, 0,
            (uint8_t)status);
    }
    case OM_DISTRIBUTION_FRAME_WATCH_ACK: {
        if (frame->payload.length != 1) return OM_STATUS_INVALID_ARGUMENT;
        const uint8_t peer_status = frame->payload.data[0];
        if (peer_status == OM_STATUS_OK) return OM_STATUS_OK;
        Om_Runtime_Distribution_State *state = distribution;
        dist_mutex_lock(&state->mutex);
        for (Om_Runtime_Distribution_Watch *watch = state->watches;
             watch != NULL; watch = watch->next) {
            if (watch->peer == peer
                && watch->local_watcher_id == frame->receiver.actor_id
                && watch->remote_actor_id == frame->sender.actor_id) {
                watch->notified = true;
                break;
            }
        }
        dist_mutex_unlock(&state->mutex);
        const Om_Status post_status = om_runtime_actor_post_remote_down(
            peer->reader_context,
            frame->receiver.actor_id, frame->sender.actor_id,
            OM_RUNTIME_ACTOR_DOWN_FAILED);
        return post_status == OM_STATUS_NOT_FOUND ? OM_STATUS_OK : post_status;
    }
    case OM_DISTRIBUTION_FRAME_ACTOR_DOWN: {
        if (frame->payload.length != 1
            || frame->payload.data[0] > OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        dist_mutex_lock(&distribution->mutex);
        for (Om_Runtime_Distribution_Watch *watch = distribution->watches;
             watch != NULL; watch = watch->next) {
            if (watch->peer == peer
                && watch->local_watcher_id == frame->receiver.actor_id
                && watch->remote_actor_id == frame->sender.actor_id) {
                watch->notified = true;
                break;
            }
        }
        dist_mutex_unlock(&distribution->mutex);
        const Om_Status post_status = om_runtime_actor_post_remote_down(
            peer->reader_context,
            frame->receiver.actor_id, frame->sender.actor_id,
            (Om_Runtime_Actor_Down_Reason)frame->payload.data[0]);
        return post_status == OM_STATUS_NOT_FOUND ? OM_STATUS_OK : post_status;
    }
    case OM_DISTRIBUTION_FRAME_HELLO:
    case OM_DISTRIBUTION_FRAME_HELLO_ACK:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void peer_notify_node_unreachable(Om_Runtime_Distribution_Peer *peer)
{
    Om_Runtime_Distribution_State *distribution = peer->distribution;
    for (;;) {
        uint64_t watcher_id = 0;
        uint64_t remote_actor_id = 0;
        bool found = false;
        dist_mutex_lock(&distribution->mutex);
        if (!distribution->closing) {
            for (Om_Runtime_Distribution_Watch *watch = distribution->watches;
                 watch != NULL; watch = watch->next) {
                if (watch->peer == peer && !watch->notified) {
                    watch->notified = true;
                    watcher_id = watch->local_watcher_id;
                    remote_actor_id = watch->remote_actor_id;
                    found = true;
                    break;
                }
            }
        }
        dist_mutex_unlock(&distribution->mutex);
        if (!found) break;
        (void)om_runtime_actor_post_remote_down(peer->reader_context,
            watcher_id, remote_actor_id,
            OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE);
    }
}

OM_DIST_THREAD(distribution_reader_thread)
{
    Om_Runtime_Distribution_Peer *peer = argument;
    for (;;) {
        Om_Distribution_Frame frame = {0};
        const Om_Status status = om_distribution_connection_receive(
            &peer->connection, OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES,
            &frame);
        if (status != OM_STATUS_OK) break;
        if (dispatch_frame(peer, &frame) != OM_STATUS_OK) break;
    }
    peer_signal_closed(peer);
    peer_notify_node_unreachable(peer);
    om_runtime_context_destroy(peer->reader_context);
    peer->reader_context = NULL;
    OM_DIST_THREAD_RETURN;
}

OM_DIST_THREAD(distribution_writer_thread)
{
    Om_Runtime_Distribution_Peer *peer = argument;
    for (;;) {
        dist_mutex_lock(&peer->queue_mutex);
        while (peer->queue_head == NULL && !peer->closed) {
            dist_condition_wait(&peer->queue_condition, &peer->queue_mutex);
        }
        if (peer->closed && peer->queue_head == NULL) {
            dist_mutex_unlock(&peer->queue_mutex);
            break;
        }
        Om_Runtime_Distribution_Queued_Frame *queued = peer->queue_head;
        peer->queue_head = queued->next;
        if (peer->queue_head == NULL) peer->queue_tail = NULL;
        peer->queued_bytes -= queued->payload_length;
        dist_mutex_unlock(&peer->queue_mutex);

        Om_Distribution_Frame frame = {
            .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
            .type = queued->type,
            .sender = {
                .node = peer->distribution->local_node_id,
                .actor_id = queued->sender_actor_id,
            },
            .receiver = {
                .node = peer->remote_node_id,
                .actor_id = queued->receiver_actor_id,
            },
            .message_fingerprint = queued->fingerprint,
            .payload = {queued->payload, queued->payload_length},
        };
        const Om_Status status = om_distribution_connection_send(
            &peer->connection, &frame,
            OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES);
        queued_frame_destroy(queued);
        if (status != OM_STATUS_OK) {
            peer_signal_closed(peer);
            break;
        }
    }
    OM_DIST_THREAD_RETURN;
}

static Om_Status distribution_spawn_hook(
    void *state,
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result);
static Om_Status distribution_send_hook(
    void *state,
    Om_Execution_Context *context,
    uint64_t remote_actor_ref,
    const void *message_object);
static Om_Status distribution_watch_hook(void *state,
                                         Om_Execution_Context *context,
                                         uint64_t remote_actor_ref);
static Om_Status distribution_notify_watcher_hook(
    void *state,
    Om_Execution_Context *context,
    uint64_t remote_watcher_ref,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason);

static const Om_Runtime_Distribution_Hooks DISTRIBUTION_HOOKS = {
    .spawn = distribution_spawn_hook,
    .send = distribution_send_hook,
    .watch = distribution_watch_hook,
    .notify_watcher = distribution_notify_watcher_hook,
};

static Om_Status distribution_spawn_hook(
    void *state,
    Om_Execution_Context *context,
    const Om_Runtime_Actor_Descriptor *descriptor,
    uint64_t node_ref,
    Om_Runtime_Actor_Ref *result)
{
    Om_Runtime_Distribution_State *distribution = state;
    if (distribution == NULL || context == NULL || descriptor == NULL
        || result == NULL) return OM_STATUS_INVALID_ARGUMENT;
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Peer *peer = peer_by_node_ref_locked(
        distribution, node_ref);
    const uint64_t counter = distribution->next_remote_actor_id;
    if (peer == NULL || counter == 0
        || counter > OM_RUNTIME_DISTRIBUTION_ID_MASK) {
        dist_mutex_unlock(&distribution->mutex);
        return peer == NULL ? OM_STATUS_NOT_FOUND : OM_STATUS_OUT_OF_RANGE;
    }
    distribution->next_remote_actor_id++;
    const uint64_t remote_actor_id =
        OM_RUNTIME_DISTRIBUTION_REMOTE_BIT | counter;
    dist_mutex_unlock(&distribution->mutex);

    uint64_t proxy_handle = 0;
    Om_Status status = proxy_intern(distribution, peer, remote_actor_id,
                                    &proxy_handle);
    uint64_t sender_actor_id = 0;
    if (status == OM_STATUS_OK) {
        status = current_actor_id(context, &sender_actor_id);
    }
    if (status == OM_STATUS_OK) {
        status = queue_frame(peer, OM_DISTRIBUTION_FRAME_SPAWN,
            sender_actor_id, remote_actor_id, descriptor->fingerprint,
            NULL, 0);
    }
    if (status != OM_STATUS_OK) {
        dist_mutex_lock(&distribution->mutex);
        Om_Runtime_Distribution_Proxy **link = &distribution->proxies;
        while (*link != NULL && (*link)->handle != proxy_handle) {
            link = &(*link)->next;
        }
        if (*link != NULL) {
            Om_Runtime_Distribution_Proxy *removed = *link;
            *link = removed->next;
            om_free(removed);
        }
        dist_mutex_unlock(&distribution->mutex);
        return status;
    }
    result->id = proxy_handle;
    return OM_STATUS_OK;
}

static Om_Status distribution_send_hook(
    void *state,
    Om_Execution_Context *context,
    uint64_t remote_actor_ref,
    const void *message_object)
{
    Om_Runtime_Distribution_State *distribution = state;
    if (distribution == NULL || context == NULL || message_object == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Proxy *proxy = proxy_by_handle_locked(
        distribution, remote_actor_ref);
    Om_Runtime_Distribution_Peer *peer = proxy == NULL ? NULL : proxy->peer;
    const uint64_t receiver_actor_id = proxy == NULL
        ? 0 : proxy->remote_actor_id;
    dist_mutex_unlock(&distribution->mutex);
    if (proxy == NULL) return OM_STATUS_NOT_FOUND;

    const Om_Runtime_Type_Descriptor *message_type = NULL;
    Om_Status status = om_runtime_object_type(context, message_object,
                                              &message_type);
    if (status != OM_STATUS_OK || message_type->kind != OM_RUNTIME_TYPE_MESSAGE) {
        return status == OM_STATUS_OK ? OM_STATUS_INVALID_ARGUMENT : status;
    }
    uint8_t *payload = NULL;
    size_t payload_length = 0;
    status = encode_message_payload(context, distribution, message_object,
        message_type, &payload, &payload_length);
    uint64_t sender_actor_id = 0;
    if (status == OM_STATUS_OK) {
        status = current_actor_id(context, &sender_actor_id);
    }
    if (status == OM_STATUS_OK) {
        return queue_frame(peer, OM_DISTRIBUTION_FRAME_MESSAGE,
            sender_actor_id, receiver_actor_id, message_type->fingerprint,
            payload, payload_length);
    }
    om_free(payload);
    return status;
}

static void remove_watch(Om_Runtime_Distribution_State *distribution,
                         Om_Runtime_Distribution_Peer *peer,
                         uint64_t watcher_id,
                         uint64_t remote_actor_id)
{
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Watch **link = &distribution->watches;
    while (*link != NULL) {
        Om_Runtime_Distribution_Watch *watch = *link;
        if (watch->peer == peer && watch->local_watcher_id == watcher_id
            && watch->remote_actor_id == remote_actor_id) {
            *link = watch->next;
            om_free(watch);
            break;
        }
        link = &watch->next;
    }
    dist_mutex_unlock(&distribution->mutex);
}

static Om_Status distribution_watch_hook(void *state,
                                         Om_Execution_Context *context,
                                         uint64_t remote_actor_ref)
{
    Om_Runtime_Distribution_State *distribution = state;
    if (distribution == NULL || context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Proxy *proxy = proxy_by_handle_locked(
        distribution, remote_actor_ref);
    Om_Runtime_Distribution_Peer *peer = proxy == NULL ? NULL : proxy->peer;
    const uint64_t remote_actor_id = proxy == NULL
        ? 0 : proxy->remote_actor_id;
    dist_mutex_unlock(&distribution->mutex);
    if (proxy == NULL) return OM_STATUS_NOT_FOUND;
    uint64_t local_watcher_id = 0;
    Om_Status status = om_runtime_actor_self_id(context, &local_watcher_id);
    if (status != OM_STATUS_OK) return status;
    Om_Runtime_Distribution_Watch *created = om_alloc(sizeof(*created));
    if (created == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *created = (Om_Runtime_Distribution_Watch){
        .local_watcher_id = local_watcher_id,
        .remote_actor_id = remote_actor_id,
        .peer = peer,
    };
    dist_mutex_lock(&distribution->mutex);
    for (Om_Runtime_Distribution_Watch *watch = distribution->watches;
         watch != NULL; watch = watch->next) {
        if (watch->peer == peer && watch->local_watcher_id == local_watcher_id
            && watch->remote_actor_id == remote_actor_id) {
            dist_mutex_unlock(&distribution->mutex);
            om_free(created);
            return OM_STATUS_OK;
        }
    }
    created->next = distribution->watches;
    distribution->watches = created;
    dist_mutex_unlock(&distribution->mutex);

    status = queue_frame(peer, OM_DISTRIBUTION_FRAME_WATCH, local_watcher_id,
        remote_actor_id, 0, NULL, 0);
    if (status != OM_STATUS_OK) {
        remove_watch(distribution, peer, local_watcher_id, remote_actor_id);
    }
    return status;
}

static Om_Status distribution_notify_watcher_hook(
    void *state,
    Om_Execution_Context *context,
    uint64_t remote_watcher_ref,
    uint64_t terminated_actor_id,
    Om_Runtime_Actor_Down_Reason reason)
{
    Om_Runtime_Distribution_State *distribution = state;
    if (distribution == NULL || context == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Proxy *proxy = proxy_by_handle_locked(
        distribution, remote_watcher_ref);
    Om_Runtime_Distribution_Peer *peer = proxy == NULL ? NULL : proxy->peer;
    const uint64_t remote_watcher_id = proxy == NULL
        ? 0 : proxy->remote_actor_id;
    dist_mutex_unlock(&distribution->mutex);
    if (proxy == NULL) return OM_STATUS_NOT_FOUND;
    return send_one_byte_frame(peer, OM_DISTRIBUTION_FRAME_ACTOR_DOWN,
        terminated_actor_id, remote_watcher_id, 0, (uint8_t)reason);
}

static void peer_release(Om_Runtime_Distribution_Peer *peer)
{
    if (peer == NULL) return;
    if (peer->reader_started) dist_thread_join(peer->reader_thread);
    else if (peer->reader_context != NULL) {
        om_runtime_context_destroy(peer->reader_context);
    }
    if (peer->writer_started) dist_thread_join(peer->writer_thread);
    om_distribution_connection_close(&peer->connection);
    Om_Runtime_Distribution_Queued_Frame *frame = peer->queue_head;
    while (frame != NULL) {
        Om_Runtime_Distribution_Queued_Frame *next = frame->next;
        queued_frame_destroy(frame);
        frame = next;
    }
    dist_condition_destroy(&peer->queue_condition);
    dist_mutex_destroy(&peer->queue_mutex);
    om_free(peer);
}

static void remove_peer_from_distribution(
    Om_Runtime_Distribution_State *distribution,
    Om_Runtime_Distribution_Peer *peer)
{
    dist_mutex_lock(&distribution->mutex);
    Om_Runtime_Distribution_Peer **link = &distribution->peers;
    while (*link != NULL && *link != peer) link = &(*link)->next;
    if (*link == peer) *link = peer->next;
    dist_mutex_unlock(&distribution->mutex);
}

static Om_Status add_peer(Om_Runtime_Distribution_State *distribution,
                          Om_Distribution_Connection *connection,
                          const Om_Distribution_Node_Handshake *remote,
                          uint64_t *node_ref)
{
    Om_Runtime_Distribution_Peer *peer = om_alloc(sizeof(*peer));
    if (peer == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *peer = (Om_Runtime_Distribution_Peer){
        .distribution = distribution,
        .connection = *connection,
        .remote_node_id = remote->node_id,
    };
    *connection = (Om_Distribution_Connection){0};
    if (!dist_mutex_init(&peer->queue_mutex)) {
        om_distribution_connection_close(&peer->connection);
        om_free(peer);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    if (!dist_condition_init(&peer->queue_condition)) {
        dist_mutex_destroy(&peer->queue_mutex);
        om_distribution_connection_close(&peer->connection);
        om_free(peer);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_Status status = om_runtime_actor_system_context_create(
        distribution->root_context, &peer->reader_context);
    if (status != OM_STATUS_OK) {
        dist_condition_destroy(&peer->queue_condition);
        dist_mutex_destroy(&peer->queue_mutex);
        om_distribution_connection_close(&peer->connection);
        om_free(peer);
        return status;
    }
    dist_mutex_lock(&distribution->mutex);
    if (distribution->closing
        || peer_by_node_id_locked(distribution, remote->node_id) != NULL
        || distribution->next_node_ref == 0
        || distribution->next_node_ref >= OM_RUNTIME_DISTRIBUTION_REMOTE_BIT) {
        status = OM_STATUS_INVALID_ARGUMENT;
    } else {
        peer->node_ref = distribution->next_node_ref++;
        peer->next = distribution->peers;
        distribution->peers = peer;
    }
    dist_mutex_unlock(&distribution->mutex);
    if (status != OM_STATUS_OK) {
        peer_release(peer);
        return status;
    }
    if (!OM_DIST_START_THREAD(&peer->reader_thread,
                              distribution_reader_thread, peer)) {
        remove_peer_from_distribution(distribution, peer);
        peer_release(peer);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    peer->reader_started = true;
    if (!OM_DIST_START_THREAD(&peer->writer_thread,
                              distribution_writer_thread, peer)) {
        peer_signal_closed(peer);
        remove_peer_from_distribution(distribution, peer);
        peer_release(peer);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    peer->writer_started = true;
    *node_ref = peer->node_ref;
    return OM_STATUS_OK;
}

static Om_Status handshake_connection(
    Om_Runtime_Distribution_State *distribution,
    Om_Distribution_Connection *connection,
    Om_Distribution_Node_Handshake *remote)
{
    const Om_Distribution_Node_Handshake local = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .node_id = distribution->local_node_id,
        .program_fingerprint = distribution->program->fingerprint,
        .interface_fingerprint = distribution->program->interface_fingerprint,
    };
    return om_distribution_connection_handshake_peer(connection, &local, NULL,
        OM_DISTRIBUTION_DEFAULT_MAX_FRAME_BYTES, remote);
}

Om_Status om_runtime_distribution_create(
    Om_Execution_Context *context,
    const Om_Runtime_Program_Descriptor *program,
    Om_Runtime_Distribution *distribution)
{
    if (context == NULL || program == NULL || distribution == NULL
        || distribution->owned_state != NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Execution_Context *probe = NULL;
    Om_Status status = om_runtime_actor_system_context_create(context, &probe);
    if (status != OM_STATUS_OK) return status;
    om_runtime_context_destroy(probe);
    Om_Runtime_Distribution_State *state = om_alloc(sizeof(*state));
    if (state == NULL) return OM_STATUS_OUT_OF_MEMORY;
    *state = (Om_Runtime_Distribution_State){
        .root_context = context,
        .program = program,
        .next_node_ref = 1,
        .next_remote_actor_id = 1,
        .next_proxy_id = 1,
    };
    if (!dist_mutex_init(&state->mutex)) {
        om_free(state);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    status = om_distribution_node_id_generate(&state->local_node_id);
    if (status == OM_STATUS_OK) {
        status = om_runtime_distribution_attach(context, &DISTRIBUTION_HOOKS,
                                                state);
    }
    if (status != OM_STATUS_OK) {
        dist_mutex_destroy(&state->mutex);
        om_free(state);
        return status;
    }
    distribution->owned_state = state;
    return OM_STATUS_OK;
}

Om_Status om_runtime_distribution_listen(
    Om_Runtime_Distribution *distribution,
    Om_String_View bind_address,
    uint16_t port,
    int backlog,
    uint16_t *bound_port)
{
    if (distribution == NULL || distribution->owned_state == NULL
        || bound_port == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Distribution_State *state = distribution->owned_state;
    if (state->listener_open) return OM_STATUS_INVALID_ARGUMENT;
    Om_Status status = om_distribution_listener_open(&state->listener,
        bind_address, port, backlog);
    if (status == OM_STATUS_OK) {
        state->listener_open = true;
        *bound_port = om_distribution_listener_port(&state->listener);
    }
    return status;
}

Om_Status om_runtime_distribution_accept(
    Om_Runtime_Distribution *distribution,
    uint64_t *node_ref)
{
    if (distribution == NULL || distribution->owned_state == NULL
        || node_ref == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Distribution_State *state = distribution->owned_state;
    if (!state->listener_open) return OM_STATUS_INVALID_ARGUMENT;
    Om_Distribution_Connection connection = {0};
    Om_Status status = om_distribution_listener_accept(&state->listener,
                                                       &connection);
    Om_Distribution_Node_Handshake remote = {0};
    if (status == OM_STATUS_OK) {
        status = handshake_connection(state, &connection, &remote);
    }
    if (status == OM_STATUS_OK) {
        status = add_peer(state, &connection, &remote, node_ref);
    }
    om_distribution_connection_close(&connection);
    return status;
}

Om_Status om_runtime_distribution_connect(
    Om_Runtime_Distribution *distribution,
    Om_String_View host,
    uint16_t port,
    uint64_t *node_ref)
{
    if (distribution == NULL || distribution->owned_state == NULL
        || node_ref == NULL) return OM_STATUS_INVALID_ARGUMENT;
    Om_Runtime_Distribution_State *state = distribution->owned_state;
    Om_Distribution_Connection connection = {0};
    Om_Status status = om_distribution_connection_connect(&connection, host,
                                                          port);
    Om_Distribution_Node_Handshake remote = {0};
    if (status == OM_STATUS_OK) {
        status = handshake_connection(state, &connection, &remote);
    }
    if (status == OM_STATUS_OK) {
        status = add_peer(state, &connection, &remote, node_ref);
    }
    om_distribution_connection_close(&connection);
    return status;
}

void om_runtime_distribution_destroy(
    Om_Runtime_Distribution *distribution)
{
    if (distribution == NULL || distribution->owned_state == NULL) return;
    Om_Runtime_Distribution_State *state = distribution->owned_state;
    (void)om_runtime_distribution_detach(state->root_context, state);
    dist_mutex_lock(&state->mutex);
    state->closing = true;
    Om_Runtime_Distribution_Peer *peers = state->peers;
    dist_mutex_unlock(&state->mutex);
    if (state->listener_open) {
        om_distribution_listener_close(&state->listener);
        state->listener_open = false;
    }
    for (Om_Runtime_Distribution_Peer *peer = peers; peer != NULL;
         peer = peer->next) {
        peer_signal_closed(peer);
    }
    Om_Runtime_Distribution_Peer *peer = peers;
    while (peer != NULL) {
        Om_Runtime_Distribution_Peer *next = peer->next;
        peer_release(peer);
        peer = next;
    }
    Om_Runtime_Distribution_Proxy *proxy = state->proxies;
    while (proxy != NULL) {
        Om_Runtime_Distribution_Proxy *next = proxy->next;
        om_free(proxy);
        proxy = next;
    }
    Om_Runtime_Distribution_Watch *watch = state->watches;
    while (watch != NULL) {
        Om_Runtime_Distribution_Watch *next = watch->next;
        om_free(watch);
        watch = next;
    }
    dist_mutex_destroy(&state->mutex);
    om_free(state);
    distribution->owned_state = NULL;
}

#if defined(OM_BUILD_FUZZERS)
/* White-box fuzz hook; excluded from normal Runtime builds. */
Om_Status om_runtime_distribution_fuzz_descriptor_codec(
    const uint8_t *data,
    size_t size)
{
    enum { MAX_FIELDS = 8, MAX_ROOTS = MAX_FIELDS * 2 + 2, MAX_TEXT = 32 };
    static const Om_Runtime_Type_Descriptor scalar_types[] = {
        {.fingerprint = UINT64_C(0x7101),
         .kind = OM_RUNTIME_TYPE_BOOL,
         .size = sizeof(bool),
         .alignment = _Alignof(bool)},
        {.fingerprint = UINT64_C(0x7102),
         .kind = OM_RUNTIME_TYPE_INT,
         .size = sizeof(int64_t),
         .alignment = _Alignof(int64_t)},
        {.fingerprint = UINT64_C(0x7103),
         .kind = OM_RUNTIME_TYPE_FLOAT,
         .size = sizeof(double),
         .alignment = _Alignof(double)},
        {.fingerprint = UINT64_C(0x7104),
         .kind = OM_RUNTIME_TYPE_STRING,
         .size = sizeof(void *),
         .alignment = _Alignof(void *)},
        {.fingerprint = UINT64_C(0x7105),
         .kind = OM_RUNTIME_TYPE_ACTOR_REF,
         .size = sizeof(uint64_t),
         .alignment = _Alignof(uint64_t)},
    };
    const Om_Runtime_Field_Descriptor record_field = {
        .offset = 0,
        .type = &scalar_types[1],
    };
    const Om_Runtime_Type_Descriptor record_type = {
        .fingerprint = UINT64_C(0x7201),
        .kind = OM_RUNTIME_TYPE_RECORD,
        .size = sizeof(int64_t),
        .alignment = _Alignof(int64_t),
        .fields = &record_field,
        .field_count = 1,
    };
    const Om_Runtime_Type_Descriptor list_type = {
        .fingerprint = UINT64_C(0x7202),
        .kind = OM_RUNTIME_TYPE_LIST,
        .size = sizeof(void *),
        .alignment = _Alignof(void *),
        .element_type = &scalar_types[1],
    };
    const Om_Runtime_Type_Descriptor map_type = {
        .fingerprint = UINT64_C(0x7203),
        .kind = OM_RUNTIME_TYPE_MAP,
        .size = sizeof(void *),
        .alignment = _Alignof(void *),
        .key_type = &scalar_types[1],
        .value_type = &scalar_types[0],
    };
    const Om_Runtime_Type_Descriptor *const field_types[] = {
        &scalar_types[0], &scalar_types[1], &scalar_types[2],
        &scalar_types[3], &scalar_types[4], &record_type, &list_type,
        &map_type,
    };
    const size_t field_count = size == 0
        ? 1 : 1u + (size_t)(data[0] % MAX_FIELDS);
    Om_Runtime_Field_Descriptor fields[MAX_FIELDS] = {0};
    const Om_Runtime_Type_Descriptor message_type = {
        .fingerprint = UINT64_C(0xD35C000000000001)
            | (size == 0 ? 0 : (uint64_t)data[0] << 8),
        .kind = OM_RUNTIME_TYPE_MESSAGE,
        .size = MAX_FIELDS * sizeof(Om_Runtime_Value),
        .alignment = _Alignof(Om_Runtime_Value),
        .fields = fields,
        .field_count = field_count,
    };
    const size_t type_count = sizeof(field_types) / sizeof(field_types[0]);
    size_t cursor = 1;
    for (size_t index = 0; index < field_count; ++index) {
        const size_t type_index = size == 0 ? index % type_count
            : (size_t)data[(1 + index) % size] % type_count;
        fields[index] = (Om_Runtime_Field_Descriptor){
            .offset = index * sizeof(Om_Runtime_Value),
            .type = field_types[type_index],
        };
    }

    Om_Execution_Context *context = om_runtime_context_create(1024 * 1024);
    if (context == NULL) return OM_STATUS_OUT_OF_MEMORY;
    Om_Runtime_Distribution_State distribution = {0};
    Om_Runtime_Distribution_Peer peer = {
        .distribution = &distribution,
        .reader_context = context,
    };
    void *roots[MAX_ROOTS] = {0};
    Om_Runtime_Root_Frame *root_frame =
        om_runtime_roots_push(context, roots, MAX_ROOTS);
    Om_Status status = root_frame == NULL
        ? OM_STATUS_OUT_OF_MEMORY : OM_STATUS_OK;
    size_t root_count = 0;
    void *message_object = NULL;
    void *decoded_object = NULL;
    uint8_t *payload = NULL;
    size_t payload_length = 0;
    Om_Distribution_Message wire_message = {0};
    Om_Runtime_Value original_values[MAX_FIELDS] = {0};
    size_t stage = 0;

    if (status == OM_STATUS_OK) {
        stage = 1;
        status = om_runtime_record_create(context, &message_type,
                                          &message_object);
    }
    if (status == OM_STATUS_OK) {
        stage = 2;
        status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                    message_object);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
         ++index) {
        const Om_Runtime_Type_Descriptor *field_type = fields[index].type;
        Om_Runtime_Value value = {0};
        const uint8_t raw = size == 0 ? 0 : data[cursor % size];
        ++cursor;
        switch (field_type->kind) {
        case OM_RUNTIME_TYPE_BOOL:
            value.boolean = (raw & 1u) != 0;
            break;
        case OM_RUNTIME_TYPE_INT:
            value.integer = (int64_t)(int8_t)raw;
            break;
        case OM_RUNTIME_TYPE_FLOAT:
            value.floating = (double)(int8_t)raw;
            break;
        case OM_RUNTIME_TYPE_STRING: {
            uint8_t text[MAX_TEXT] = {0};
            const size_t start = cursor < size ? cursor : size;
            const size_t available = size - start;
            const size_t text_length = available < MAX_TEXT
                ? available : MAX_TEXT;
            for (size_t byte = 0; byte < text_length; ++byte) {
                text[byte] = data[start + byte] & 0x7fu;
            }
            cursor = start + text_length;
            stage = 3;
            status = om_runtime_string_create_utf8_default(context, text,
                text_length, &value.managed);
            if (status == OM_STATUS_OK) {
                status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                           value.managed);
            }
            break;
        }
        case OM_RUNTIME_TYPE_RECORD: {
            void *record = NULL;
            status = om_runtime_record_create(context, field_type, &record);
            if (status == OM_STATUS_OK) {
                status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                            record);
            }
            if (status == OM_STATUS_OK) {
                status = om_runtime_record_set(context, record, 0,
                    (Om_Runtime_Value){.integer = (int64_t)raw});
            }
            value.managed = record;
            break;
        }
        case OM_RUNTIME_TYPE_LIST: {
            void *list = NULL;
            status = om_runtime_list_create(context, field_type, &list);
            if (status == OM_STATUS_OK) {
                status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                            list);
            }
            const size_t item_count = 1u + (size_t)(raw % 3u);
            for (size_t item = 0; status == OM_STATUS_OK
                 && item < item_count; ++item) {
                status = om_runtime_list_append(context, list,
                    (Om_Runtime_Value){
                        .integer = (int64_t)raw + (int64_t)item,
                    });
            }
            value.managed = list;
            break;
        }
        case OM_RUNTIME_TYPE_MAP: {
            void *map = NULL;
            status = om_runtime_map_create(context, field_type, &map);
            if (status == OM_STATUS_OK) {
                status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                            map);
            }
            const size_t item_count = 1u + (size_t)(raw % 2u);
            for (size_t item = 0; status == OM_STATUS_OK
                 && item < item_count; ++item) {
                status = om_runtime_map_set(context, map,
                    (Om_Runtime_Value){.integer = (int64_t)item + 1},
                    (Om_Runtime_Value){
                        .boolean = ((raw >> item) & 1u) != 0,
                    });
            }
            value.managed = map;
            break;
        }
        case OM_RUNTIME_TYPE_ACTOR_REF:
            value.integer = (int64_t)raw + 1;
            break;
        default:
            status = OM_STATUS_INVALID_ARGUMENT;
            break;
        }
        original_values[index] = value;
        if (status == OM_STATUS_OK) {
            stage = 4;
            status = om_runtime_record_set(context, message_object, index,
                                           value);
        }
    }
    if (status == OM_STATUS_OK) {
        stage = 5;
        status = encode_message_payload(context, &distribution, message_object,
            &message_type, &payload, &payload_length);
    }
    if (status == OM_STATUS_OK) {
        stage = 6;
        status = om_distribution_message_deserialize(
            (Om_Const_Byte_Slice){payload, payload_length},
            message_type.fingerprint, message_type.fingerprint, field_count,
            (Om_Distribution_Serializer_Limits){
                .maximum_payload_bytes = 4096,
                .maximum_collection_length = MAX_FIELDS,
                .maximum_nesting_depth = 4,
                .maximum_total_values = MAX_FIELDS * 6,
            }, &wire_message);
    }
    if (status == OM_STATUS_OK) {
        stage = 7;
        status = om_runtime_record_create(context, &message_type,
                                          &decoded_object);
    }
    if (status == OM_STATUS_OK) {
        stage = 8;
        status = root_managed_value(roots, MAX_ROOTS, &root_count,
                                    decoded_object);
    }
    for (size_t index = 0; status == OM_STATUS_OK && index < field_count;
         ++index) {
        Om_Runtime_Value value = {0};
        stage = 9;
        status = decode_runtime_value(context, &distribution, &peer,
            fields[index].type, &wire_message.fields[index], roots, MAX_ROOTS,
            &root_count, &value);
        if (status == OM_STATUS_OK) {
            stage = 10;
            status = om_runtime_record_set(context, decoded_object, index,
                                           value);
        }
        if (status == OM_STATUS_OK) {
            Om_Runtime_Value roundtrip = {0};
            stage = 11;
            status = om_runtime_record_get(context, decoded_object, index,
                                           &roundtrip);
            if (status == OM_STATUS_OK
                && fields[index].type->kind == OM_RUNTIME_TYPE_BOOL
                && roundtrip.boolean != original_values[index].boolean) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (status == OM_STATUS_OK
                && (fields[index].type->kind == OM_RUNTIME_TYPE_INT
                    || fields[index].type->kind == OM_RUNTIME_TYPE_ACTOR_REF)
                && roundtrip.integer != original_values[index].integer) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (status == OM_STATUS_OK
                && fields[index].type->kind == OM_RUNTIME_TYPE_FLOAT
                && roundtrip.floating != original_values[index].floating) {
                status = OM_STATUS_INVALID_ARGUMENT;
            } else if (status == OM_STATUS_OK
                && fields[index].type->kind == OM_RUNTIME_TYPE_STRING) {
                Om_String_View original_view = {0};
                Om_String_View roundtrip_view = {0};
                stage = 12;
                status = om_runtime_string_view(context,
                    original_values[index].managed, &original_view);
                if (status == OM_STATUS_OK) {
                    stage = 13;
                    status = om_runtime_string_view(context, roundtrip.managed,
                                                    &roundtrip_view);
                }
                if (status == OM_STATUS_OK
                    && (original_view.length != roundtrip_view.length
                        || memcmp(original_view.data, roundtrip_view.data,
                                  original_view.length) != 0)) {
                    status = OM_STATUS_INVALID_ARGUMENT;
                }
            } else if (status == OM_STATUS_OK
                && (fields[index].type->kind == OM_RUNTIME_TYPE_RECORD
                    || fields[index].type->kind == OM_RUNTIME_TYPE_LIST
                    || fields[index].type->kind == OM_RUNTIME_TYPE_MAP)
                && (original_values[index].managed == NULL
                    || roundtrip.managed == NULL)) {
                status = OM_STATUS_INVALID_ARGUMENT;
            }
        }
    }

    if (status != OM_STATUS_OK && status != OM_STATUS_OUT_OF_MEMORY) {
        (void)fprintf(stderr, "descriptor codec fuzz stage %zu: %s\n", stage,
                      om_status_name(status));
    }

    om_distribution_message_destroy(&wire_message);
    om_free(payload);
    if (root_frame != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, root_frame);
        if (status == OM_STATUS_OK) status = pop_status;
    }
    om_runtime_context_destroy(context);
    return status;
}
#endif
