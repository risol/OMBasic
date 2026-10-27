#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "../vm_internal.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "ombasic/distribution/serializer.h"
#include "ombasic/foundation/arena.h"
#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/system_actors.h"

typedef struct Om_VM_Distribution_Mutex {
#if defined(_WIN32)
    CRITICAL_SECTION native;
#else
    pthread_mutex_t native;
#endif
} Om_VM_Distribution_Mutex;

typedef struct Om_VM_Distribution_Thread {
#if defined(_WIN32)
    HANDLE native;
#else
    pthread_t native;
#endif
} Om_VM_Distribution_Thread;

typedef struct Om_VM_Wire_Conversion {
    Om_VM_State *vm;
    Om_Arena arena;
    size_t remaining_values;
} Om_VM_Wire_Conversion;

static Om_VM_State *distribution_shared_vm(Om_VM_State *state)
{
    return state->shared == NULL ? state : state->shared;
}

static bool distribution_mutex_init(Om_VM_Distribution_Mutex *mutex)
{
#if defined(_WIN32)
    return InitializeCriticalSectionEx(&mutex->native, 0, 0) != 0;
#else
    return pthread_mutex_init(&mutex->native, NULL) == 0;
#endif
}

static void distribution_mutex_destroy(Om_VM_Distribution_Mutex *mutex)
{
#if defined(_WIN32)
    DeleteCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_destroy(&mutex->native);
#endif
}

static void distribution_mutex_lock(Om_VM_Distribution_Mutex *mutex)
{
#if defined(_WIN32)
    EnterCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_lock(&mutex->native);
#endif
}

static void distribution_mutex_unlock(Om_VM_Distribution_Mutex *mutex)
{
#if defined(_WIN32)
    LeaveCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_unlock(&mutex->native);
#endif
}

static void notify_peer_watches_disconnected(
    Om_VM_Distribution_Peer *peer);
static Om_Status enqueue_actor_down(Om_VM_State *state,
                                    uint64_t actor_id,
                                    uint64_t watcher_actor_id,
                                    uint32_t down_reason);
static Om_Status send_actor_down_to_peer(Om_VM_Distribution_Peer *peer,
                                         uint64_t actor_id,
                                         uint64_t watcher_actor_id,
                                         uint32_t down_reason);
static Om_Status add_remote_watcher(Om_VM_Distribution_Peer *peer,
                                    uint64_t target_actor_id,
                                    uint64_t watcher_actor_id,
                                    uint32_t *stopped_reason,
                                    bool *is_stopped);

static void peer_mark_disconnected(Om_VM_Distribution_Peer *peer,
                                   bool notify_watchers)
{
    const bool was_connected = atomic_exchange_explicit(
        &peer->connected, false, memory_order_acq_rel);
    om_distribution_connection_interrupt(&peer->connection);
    if (was_connected && notify_watchers) {
        notify_peer_watches_disconnected(peer);
    }
    om_vm_scheduler_broadcast(peer->vm);
}

static Om_Status peer_send_frame(Om_VM_Distribution_Peer *peer,
                                 Om_Distribution_Frame_Type type,
                                 uint64_t sender_actor_id,
                                 uint64_t receiver_actor_id,
                                 uint64_t message_fingerprint,
                                 Om_Const_Byte_Slice payload)
{
    if (peer == NULL
        || ((type == OM_DISTRIBUTION_FRAME_WATCH
             || type == OM_DISTRIBUTION_FRAME_ACTOR_DOWN)
            && (sender_actor_id == 0 || receiver_actor_id == 0))
        || ((type == OM_DISTRIBUTION_FRAME_SPAWN
             || type == OM_DISTRIBUTION_FRAME_SPAWN_ACK)
            && message_fingerprint == 0)
        || (type == OM_DISTRIBUTION_FRAME_SPAWN
            && receiver_actor_id != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Frame frame = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .type = type,
        .sender = {
            .node = peer->vm->distribution_handshake.node_id,
            .actor_id = sender_actor_id
        },
        .receiver = {
            .node = peer->remote_handshake.node_id,
            .actor_id = receiver_actor_id
        },
        .message_fingerprint = message_fingerprint,
        .payload = payload
    };
    Om_VM_Distribution_Mutex *send_mutex = peer->send_mutex;
    distribution_mutex_lock(send_mutex);
    Om_Status status = atomic_load_explicit(
        &peer->connected, memory_order_acquire)
        ? om_distribution_connection_send(&peer->connection, &frame, 0)
        : OM_STATUS_IO_ERROR;
    distribution_mutex_unlock(send_mutex);
    if (status != OM_STATUS_OK) {
        peer_mark_disconnected(peer, true);
    }
    return status;
}

static Om_VM_Distribution_Peer *peer_by_handle(const Om_VM_State *state,
                                                uint64_t handle)
{
    if (handle == 0 || handle > state->distribution_peer_count) {
        return NULL;
    }
    Om_VM_Distribution_Peer *peer =
        state->distribution_peers[(size_t)(handle - 1)];
    return peer != NULL && peer->handle == handle ? peer : NULL;
}

static Om_VM_Distribution_Peer *peer_by_node(const Om_VM_State *state,
                                              Om_Distribution_Node_Id node)
{
    for (size_t index = 0; index < state->distribution_peer_count; ++index) {
        Om_VM_Distribution_Peer *peer = state->distribution_peers[index];
        if (peer != NULL
            && om_distribution_node_id_equal(peer->remote_handshake.node_id,
                                             node)) {
            return peer;
        }
    }
    return NULL;
}

static Om_VM_Remote_Actor_Ref *remote_ref_by_value(Om_VM_State *state,
                                                    uint64_t value)
{
    for (size_t index = 0; index < state->remote_actor_ref_count; ++index) {
        if (state->remote_actor_refs[index].value == value) {
            return &state->remote_actor_refs[index];
        }
    }
    return NULL;
}

static Om_Status remote_ref_reserve(Om_VM_State *state, size_t required)
{
    if (required <= state->remote_actor_ref_capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = state->remote_actor_ref_capacity == 0
        ? 8 : state->remote_actor_ref_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            return OM_STATUS_OVERFLOW;
        }
        capacity *= 2;
    }
    Om_VM_Remote_Actor_Ref *refs = om_realloc_array(
        state->remote_actor_refs, capacity, sizeof(*refs));
    if (refs == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    state->remote_actor_refs = refs;
    state->remote_actor_ref_capacity = capacity;
    return OM_STATUS_OK;
}

static Om_Status remote_ref_get_or_create_locked(
    Om_VM_State *state,
    Om_VM_Distribution_Peer *peer,
    uint64_t actor_id,
    uint64_t *value)
{
    if (peer == NULL || value == NULL || actor_id == 0
        || !atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        return OM_STATUS_NOT_FOUND;
    }
    for (size_t index = 0; index < state->remote_actor_ref_count; ++index) {
        const Om_VM_Remote_Actor_Ref *ref = &state->remote_actor_refs[index];
        if (ref->peer_handle == peer->handle
            && ref->remote_actor_id == actor_id) {
            *value = ref->value;
            return OM_STATUS_OK;
        }
    }
    if (state->next_remote_actor_ref <= 4) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    const Om_Status reserve_status = remote_ref_reserve(
        state, state->remote_actor_ref_count + 1);
    if (reserve_status != OM_STATUS_OK) {
        return reserve_status;
    }
    const uint64_t new_value = state->next_remote_actor_ref--;
    state->remote_actor_refs[state->remote_actor_ref_count++] =
        (Om_VM_Remote_Actor_Ref){
            .value = new_value,
            .peer_handle = peer->handle,
            .remote_actor_id = actor_id
        };
    *value = new_value;
    return OM_STATUS_OK;
}

static bool local_actor_id_exists(const Om_VM_State *state, uint64_t actor_id)
{
    if (actor_id == OM_SYSTEM_ACTOR_REF_CONSOLE
        || actor_id == OM_SYSTEM_ACTOR_REF_TIMER
        || actor_id == OM_SYSTEM_ACTOR_REF_FILE_SYSTEM) {
        return true;
    }
    const size_t actor_count = atomic_load_explicit(
        &state->actor_count, memory_order_acquire);
    return actor_id != 0 && actor_id <= actor_count
        && state->actors[(size_t)(actor_id - 1)].id == actor_id;
}

static Om_Status actor_ref_to_wire(Om_VM_State *state,
                                   uint64_t value,
                                   Om_Distribution_Actor_Ref *wire_ref)
{
    if (wire_ref == NULL || value == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    om_vm_scheduler_lock(state);
    const bool is_local = local_actor_id_exists(state, value);
    if (is_local) {
        *wire_ref = (Om_Distribution_Actor_Ref){
            .node = state->distribution_handshake.node_id,
            .actor_id = value
        };
        om_vm_scheduler_unlock(state);
        return OM_STATUS_OK;
    }
    Om_Status status = OM_STATUS_NOT_FOUND;
    Om_VM_Remote_Actor_Ref *remote = remote_ref_by_value(state, value);
    if (remote != NULL) {
        Om_VM_Distribution_Peer *peer = peer_by_handle(
            state, remote->peer_handle);
        if (peer != NULL
            && atomic_load_explicit(&peer->connected, memory_order_acquire)) {
            *wire_ref = (Om_Distribution_Actor_Ref){
                .node = peer->remote_handshake.node_id,
                .actor_id = remote->remote_actor_id
            };
            status = OM_STATUS_OK;
        } else {
            status = OM_STATUS_IO_ERROR;
        }
    }
    om_vm_scheduler_unlock(state);
    return status;
}

static Om_Status wire_value_array(Om_VM_Wire_Conversion *conversion,
                                  size_t count,
                                  Om_Distribution_Value **values)
{
    if (values == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *values = NULL;
    if (count == 0) {
        return OM_STATUS_OK;
    }
    if (count > OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH
        || count > conversion->remaining_values) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    size_t bytes = 0;
    if (!om_checked_size_mul(count, sizeof(**values), &bytes)) {
        return OM_STATUS_OVERFLOW;
    }
    void *allocation = NULL;
    const Om_Status status = om_arena_alloc(
        &conversion->arena, bytes, alignof(Om_Distribution_Value),
        &allocation);
    if (status != OM_STATUS_OK) {
        return status;
    }
    (void)memset(allocation, 0, bytes);
    *values = allocation;
    return OM_STATUS_OK;
}

static Om_Status wire_map_array(Om_VM_Wire_Conversion *conversion,
                                size_t count,
                                Om_Distribution_Map_Entry **entries)
{
    if (entries == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *entries = NULL;
    if (count == 0) {
        return OM_STATUS_OK;
    }
    if (count > OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH
        || count > conversion->remaining_values / 2) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    size_t bytes = 0;
    if (!om_checked_size_mul(count, sizeof(**entries), &bytes)) {
        return OM_STATUS_OVERFLOW;
    }
    void *allocation = NULL;
    const Om_Status status = om_arena_alloc(
        &conversion->arena, bytes, alignof(Om_Distribution_Map_Entry),
        &allocation);
    if (status != OM_STATUS_OK) {
        return status;
    }
    (void)memset(allocation, 0, bytes);
    *entries = allocation;
    return OM_STATUS_OK;
}

static Om_Status vm_value_to_wire(Om_VM_Wire_Conversion *conversion,
                                  Om_VM_Value source,
                                  size_t depth,
                                  Om_Distribution_Value *destination)
{
    if (conversion == NULL || destination == NULL
        || depth > OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH
        || conversion->remaining_values == 0) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    conversion->remaining_values -= 1;
    switch (source.tag) {
    case OM_VM_VALUE_NULL:
        destination->kind = OM_DISTRIBUTION_VALUE_NULL;
        return OM_STATUS_OK;
    case OM_VM_VALUE_UNIT:
        destination->kind = OM_DISTRIBUTION_VALUE_UNIT;
        return OM_STATUS_OK;
    case OM_VM_VALUE_BOOL:
        destination->kind = OM_DISTRIBUTION_VALUE_BOOL;
        destination->as.boolean_value = source.as.boolean;
        return OM_STATUS_OK;
    case OM_VM_VALUE_INT:
        destination->kind = OM_DISTRIBUTION_VALUE_INT64;
        destination->as.integer_value = source.as.integer;
        return OM_STATUS_OK;
    case OM_VM_VALUE_FLOAT:
        destination->kind = OM_DISTRIBUTION_VALUE_FLOAT64;
        destination->as.float_value = source.as.floating;
        return OM_STATUS_OK;
    case OM_VM_VALUE_ACTOR_ID:
        destination->kind = OM_DISTRIBUTION_VALUE_ACTOR_ID;
        destination->as.actor_id = source.as.actor_id;
        return OM_STATUS_OK;
    case OM_VM_VALUE_NODE_REF:
        return OM_STATUS_INVALID_ARGUMENT;
    case OM_VM_VALUE_ACTOR_REF:
        destination->kind = OM_DISTRIBUTION_VALUE_ACTOR_REF;
        return actor_ref_to_wire(conversion->vm, source.as.actor_id,
                                 &destination->as.actor_ref);
    case OM_VM_VALUE_DOWN_REASON:
        destination->kind = OM_DISTRIBUTION_VALUE_DOWN_REASON;
        destination->as.down_reason = source.as.down_reason;
        return OM_STATUS_OK;
    case OM_VM_VALUE_OBJECT:
        break;
    case OM_VM_VALUE_UNINITIALIZED:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (source.as.object == NULL || source.as.object->owner == NULL
        || source.as.object->owner->vm != conversion->vm) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    switch (source.as.object->kind) {
    case OM_VM_OBJECT_STRING: {
        const Om_VM_String_Object *string =
            (const Om_VM_String_Object *)source.as.object;
        destination->kind = OM_DISTRIBUTION_VALUE_STRING;
        destination->as.string_value =
            (Om_String_View){string->bytes, string->length};
        return OM_STATUS_OK;
    }
    case OM_VM_OBJECT_LIST: {
        const Om_VM_List_Object *list =
            (const Om_VM_List_Object *)source.as.object;
        destination->kind = OM_DISTRIBUTION_VALUE_LIST;
        Om_Distribution_Value *items = NULL;
        Om_Status status = wire_value_array(
            conversion, list->length, &items);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < list->length; ++index) {
            status = vm_value_to_wire(conversion, list->items[index],
                                      depth + 1, &items[index]);
        }
        if (status == OM_STATUS_OK) {
            destination->as.list.items = items;
            destination->as.list.count = list->length;
        }
        return status;
    }
    case OM_VM_OBJECT_MAP: {
        const Om_VM_Map_Object *map =
            (const Om_VM_Map_Object *)source.as.object;
        destination->kind = OM_DISTRIBUTION_VALUE_MAP;
        Om_Distribution_Map_Entry *entries = NULL;
        Om_Status status = wire_map_array(
            conversion, map->length, &entries);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < map->length; ++index) {
            status = vm_value_to_wire(conversion, map->entries[index].key,
                                      depth + 1, &entries[index].key);
            if (status == OM_STATUS_OK) {
                status = vm_value_to_wire(
                    conversion, map->entries[index].value, depth + 1,
                    &entries[index].value);
            }
        }
        if (status == OM_STATUS_OK) {
            destination->as.map.entries = entries;
            destination->as.map.count = map->length;
        }
        return status;
    }
    case OM_VM_OBJECT_RECORD:
    case OM_VM_OBJECT_MESSAGE: {
        const Om_VM_Record_Object *record =
            (const Om_VM_Record_Object *)source.as.object;
        if (record->schema_index >= conversion->vm->image->schema_count) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        const Om_Bytecode_Schema *schema =
            &conversion->vm->image->schemas[record->schema_index];
        if (schema->field_count != record->field_count
            || (source.as.object->kind == OM_VM_OBJECT_MESSAGE
                && schema->kind != OM_BYTECODE_SCHEMA_MESSAGE)
            || (source.as.object->kind == OM_VM_OBJECT_RECORD
                && schema->kind != OM_BYTECODE_SCHEMA_RECORD)) {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        destination->kind = source.as.object->kind == OM_VM_OBJECT_MESSAGE
            ? OM_DISTRIBUTION_VALUE_MESSAGE
            : OM_DISTRIBUTION_VALUE_RECORD;
        destination->as.composite.schema_fingerprint = schema->fingerprint;
        Om_Distribution_Value *fields = NULL;
        Om_Status status = wire_value_array(
            conversion, record->field_count, &fields);
        for (size_t index = 0; status == OM_STATUS_OK
             && index < record->field_count; ++index) {
            status = vm_value_to_wire(conversion, record->fields[index],
                                      depth + 1, &fields[index]);
        }
        if (status == OM_STATUS_OK) {
            destination->as.composite.fields = fields;
            destination->as.composite.field_count = record->field_count;
        }
        return status;
    }
    case OM_VM_OBJECT_FUNCTION:
    case OM_VM_OBJECT_RESULT:
    case OM_VM_OBJECT_ITERATOR:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status serialize_vm_message(Om_VM_State *state,
                                      Om_VM_Value message,
                                      Om_Byte_Slice *payload,
                                      uint64_t *fingerprint)
{
    if (payload == NULL || fingerprint == NULL
        || message.tag != OM_VM_VALUE_OBJECT
        || message.as.object == NULL
        || message.as.object->kind != OM_VM_OBJECT_MESSAGE
        || message.as.object->owner == NULL
        || message.as.object->owner->vm != state) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_VM_Record_Object *record =
        (const Om_VM_Record_Object *)message.as.object;
    if (record->schema_index >= state->image->schema_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Bytecode_Schema *schema =
        &state->image->schemas[record->schema_index];
    if (schema->kind != OM_BYTECODE_SCHEMA_MESSAGE
        || schema->field_count != record->field_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Wire_Conversion conversion = {.vm = state};
    conversion.remaining_values = OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES;
    Om_Status status = om_arena_init(&conversion.arena, 4096);
    Om_Distribution_Value *fields = NULL;
    if (status == OM_STATUS_OK) {
        status = wire_value_array(&conversion, record->field_count, &fields);
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < record->field_count; ++index) {
        status = vm_value_to_wire(&conversion, record->fields[index], 1,
                                  &fields[index]);
    }
    if (status == OM_STATUS_OK) {
        status = om_distribution_message_serialize(
            fields, record->field_count,
            (Om_Distribution_Serializer_Limits){0}, payload);
    }
    if (status == OM_STATUS_OK) {
        *fingerprint = schema->fingerprint;
    }
    om_arena_destroy(&conversion.arena);
    return status;
}

static Om_Status schema_find_by_fingerprint(
    const Om_Bytecode_Image *image,
    uint64_t fingerprint,
    Om_Bytecode_Schema_Kind kind,
    uint32_t *schema_index)
{
    if (image == NULL || schema_index == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *schema_index = UINT32_MAX;
    for (size_t index = 0; index < image->schema_count; ++index) {
        const Om_Bytecode_Schema *schema = &image->schemas[index];
        if (schema->kind == kind && schema->fingerprint == fingerprint) {
            if (*schema_index != UINT32_MAX) {
                return OM_STATUS_INVALID_ARGUMENT;
            }
            *schema_index = (uint32_t)index;
        }
    }
    return *schema_index == UINT32_MAX
        ? OM_STATUS_NOT_FOUND : OM_STATUS_OK;
}

static Om_Status decode_actor_ref(Om_VM_State *state,
                                  Om_Distribution_Actor_Ref wire_ref,
                                  Om_VM_Value *value)
{
    if (wire_ref.actor_id == 0 || value == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    if (om_distribution_node_id_equal(
            wire_ref.node, state->distribution_handshake.node_id)) {
        *value = om_vm_value_actor_ref(wire_ref.actor_id);
        return OM_STATUS_OK;
    }
    Om_Status status = OM_STATUS_NOT_FOUND;
    om_vm_scheduler_lock(state);
    Om_VM_Distribution_Peer *peer = peer_by_node(state, wire_ref.node);
    uint64_t handle = 0;
    if (peer != NULL) {
        status = remote_ref_get_or_create_locked(
            state, peer, wire_ref.actor_id, &handle);
    }
    om_vm_scheduler_unlock(state);
    if (status == OM_STATUS_OK) {
        *value = om_vm_value_actor_ref(handle);
    }
    return status;
}

static Om_Status wire_value_to_vm(Om_VM_State *state,
                                  Om_VM_Heap *heap,
                                  const Om_Distribution_Value *source,
                                  size_t depth,
                                  size_t *remaining_values,
                                  Om_VM_Value *value)
{
    if (source == NULL || remaining_values == NULL || value == NULL
        || depth > OM_DISTRIBUTION_DEFAULT_MAX_NESTING_DEPTH
        || *remaining_values == 0) {
        return OM_STATUS_OUT_OF_RANGE;
    }
    *remaining_values -= 1;
    switch (source->kind) {
    case OM_DISTRIBUTION_VALUE_NULL:
        *value = om_vm_value_null();
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_UNIT:
        *value = om_vm_value_unit();
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_BOOL:
        *value = om_vm_value_bool(source->as.boolean_value);
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_INT64:
        *value = om_vm_value_int(source->as.integer_value);
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_FLOAT64:
        *value = om_vm_value_float(source->as.float_value);
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_STRING:
        return om_vm_heap_make_string(heap, source->as.string_value, value);
    case OM_DISTRIBUTION_VALUE_ACTOR_REF:
        return decode_actor_ref(state, source->as.actor_ref, value);
    case OM_DISTRIBUTION_VALUE_ACTOR_ID:
        *value = om_vm_value_actor_id(source->as.actor_id);
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_DOWN_REASON:
        *value = (Om_VM_Value){
            .tag = OM_VM_VALUE_DOWN_REASON,
            .as.down_reason = source->as.down_reason
        };
        return OM_STATUS_OK;
    case OM_DISTRIBUTION_VALUE_LIST: {
        const size_t count = source->as.list.count;
        if (count > OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH
            || count > *remaining_values) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_VM_Value *items = count == 0 ? NULL
            : om_alloc_array(count, sizeof(*items));
        if (count != 0 && items == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        Om_Status status = OM_STATUS_OK;
        for (size_t index = 0; status == OM_STATUS_OK && index < count;
             ++index) {
            status = wire_value_to_vm(state, heap,
                &source->as.list.items[index], depth + 1,
                remaining_values, &items[index]);
        }
        if (status == OM_STATUS_OK) {
            status = om_vm_heap_make_list(heap, items, count, value);
        }
        om_free(items);
        return status;
    }
    case OM_DISTRIBUTION_VALUE_MAP: {
        const size_t count = source->as.map.count;
        if (count > OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH
            || count > *remaining_values / 2) {
            return OM_STATUS_OUT_OF_RANGE;
        }
        Om_VM_Map_Entry *entries = count == 0 ? NULL
            : om_alloc_array(count, sizeof(*entries));
        if (count != 0 && entries == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        Om_Status status = OM_STATUS_OK;
        for (size_t index = 0; status == OM_STATUS_OK && index < count;
             ++index) {
            status = wire_value_to_vm(state, heap,
                &source->as.map.entries[index].key, depth + 1,
                remaining_values, &entries[index].key);
            if (status == OM_STATUS_OK) {
                status = wire_value_to_vm(state, heap,
                    &source->as.map.entries[index].value, depth + 1,
                    remaining_values, &entries[index].value);
            }
        }
        if (status == OM_STATUS_OK) {
            status = om_vm_heap_make_map(heap, entries, count, value);
        }
        om_free(entries);
        return status;
    }
    case OM_DISTRIBUTION_VALUE_RECORD:
    case OM_DISTRIBUTION_VALUE_MESSAGE: {
        const Om_Bytecode_Schema_Kind schema_kind =
            source->kind == OM_DISTRIBUTION_VALUE_MESSAGE
                ? OM_BYTECODE_SCHEMA_MESSAGE : OM_BYTECODE_SCHEMA_RECORD;
        uint32_t schema_index = UINT32_MAX;
        Om_Status status = schema_find_by_fingerprint(
            state->image, source->as.composite.schema_fingerprint,
            schema_kind, &schema_index);
        const size_t count = source->as.composite.field_count;
        if (status != OM_STATUS_OK
            || state->image->schemas[schema_index].field_count != count
            || count > OM_DISTRIBUTION_DEFAULT_MAX_COLLECTION_LENGTH
            || count > *remaining_values) {
            return status == OM_STATUS_OK
                ? OM_STATUS_INVALID_ARGUMENT : status;
        }
        Om_VM_Value *fields = count == 0 ? NULL
            : om_alloc_array(count, sizeof(*fields));
        if (count != 0 && fields == NULL) {
            return OM_STATUS_OUT_OF_MEMORY;
        }
        for (size_t index = 0; status == OM_STATUS_OK && index < count;
             ++index) {
            status = wire_value_to_vm(state, heap,
                &source->as.composite.fields[index], depth + 1,
                remaining_values, &fields[index]);
        }
        if (status == OM_STATUS_OK) {
            status = om_vm_heap_make_record(
                heap,
                schema_kind == OM_BYTECODE_SCHEMA_MESSAGE
                    ? OM_VM_OBJECT_MESSAGE : OM_VM_OBJECT_RECORD,
                schema_index, fields, count, value);
        }
        om_free(fields);
        return status;
    }
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status receive_message_frame(Om_VM_Distribution_Peer *peer,
                                       const Om_Distribution_Frame *frame)
{
    Om_VM_State *state = peer->vm;
    if (frame->type != OM_DISTRIBUTION_FRAME_MESSAGE
        || !om_distribution_node_id_equal(
            frame->sender.node, peer->remote_handshake.node_id)
        || !om_distribution_node_id_equal(
            frame->receiver.node, state->distribution_handshake.node_id)
        || frame->receiver.actor_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t schema_index = UINT32_MAX;
    Om_Status status = schema_find_by_fingerprint(
        state->image, frame->message_fingerprint,
        OM_BYTECODE_SCHEMA_MESSAGE, &schema_index);
    if (status != OM_STATUS_OK) {
        return status;
    }
    const Om_Bytecode_Schema *schema = &state->image->schemas[schema_index];
    Om_Distribution_Message decoded = {0};
    status = om_distribution_message_deserialize(
        frame->payload, frame->message_fingerprint, schema->fingerprint,
        schema->field_count, (Om_Distribution_Serializer_Limits){0},
        &decoded);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_VM_Message_Node *node =
        om_vm_distribution_message_node_create(state);
    if (node == NULL) {
        om_distribution_message_destroy(&decoded);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    Om_VM_Value *fields = decoded.field_count == 0 ? NULL
        : om_alloc_array(decoded.field_count, sizeof(*fields));
    if (decoded.field_count != 0 && fields == NULL) {
        om_vm_distribution_message_node_destroy(node);
        om_distribution_message_destroy(&decoded);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    size_t remaining_values = OM_DISTRIBUTION_DEFAULT_MAX_TOTAL_VALUES;
    for (size_t index = 0; status == OM_STATUS_OK
         && index < decoded.field_count; ++index) {
        status = wire_value_to_vm(
            state, &node->transit_heap, &decoded.fields[index], 1,
            &remaining_values, &fields[index]);
    }
    if (status == OM_STATUS_OK) {
        status = om_vm_heap_make_record(
            &node->transit_heap, OM_VM_OBJECT_MESSAGE, schema_index, fields,
            decoded.field_count, &node->message);
    }
    om_free(fields);
    om_distribution_message_destroy(&decoded);
    if (status == OM_STATUS_OK) {
        status = om_vm_distribution_enqueue_message(
            state, frame->receiver.actor_id, node);
        if (status == OM_STATUS_NOT_FOUND || status == OM_STATUS_CANCELLED) {
            status = OM_STATUS_OK;
        }
    }
    if (status != OM_STATUS_OK) {
        om_vm_distribution_message_node_destroy(node);
    }
    return status;
}

static bool frame_matches_peer_and_local_node(
    const Om_VM_Distribution_Peer *peer,
    const Om_Distribution_Frame *frame)
{
    return om_distribution_node_id_equal(
               frame->sender.node, peer->remote_handshake.node_id)
        && om_distribution_node_id_equal(
               frame->receiver.node,
               peer->vm->distribution_handshake.node_id);
}

static Om_Status receive_watch_frame(Om_VM_Distribution_Peer *peer,
                                     const Om_Distribution_Frame *frame)
{
    if (frame->type != OM_DISTRIBUTION_FRAME_WATCH
        || !frame_matches_peer_and_local_node(peer, frame)
        || frame->sender.actor_id == 0 || frame->receiver.actor_id == 0
        || frame->payload.length != 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint32_t stopped_reason = 1;
    bool is_stopped = false;
    const Om_Status status = add_remote_watcher(
        peer, frame->receiver.actor_id, frame->sender.actor_id,
        &stopped_reason, &is_stopped);
    if (status == OM_STATUS_NOT_FOUND) {
        return send_actor_down_to_peer(peer, frame->receiver.actor_id,
                                       frame->sender.actor_id, 1);
    }
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (is_stopped) {
        return send_actor_down_to_peer(peer, frame->receiver.actor_id,
                                       frame->sender.actor_id,
                                       stopped_reason);
    }
    return OM_STATUS_OK;
}

static uint32_t read_u32_be(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24)
        | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8)
        | (uint32_t)bytes[3];
}

static void write_u32_be(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t spawn_status_to_wire(Om_Status status)
{
    switch (status) {
    case OM_STATUS_OK: return 0;
    case OM_STATUS_NOT_FOUND: return 1;
    case OM_STATUS_OUT_OF_RANGE: return 2;
    case OM_STATUS_OUT_OF_MEMORY: return 3;
    case OM_STATUS_INVALID_ARGUMENT:
    case OM_STATUS_OVERFLOW:
    case OM_STATUS_INVALID_UTF8:
    case OM_STATUS_END_OF_INPUT:
    case OM_STATUS_CYCLE_DETECTED:
    case OM_STATUS_IO_ERROR:
    case OM_STATUS_PATH_ESCAPE:
    case OM_STATUS_NOT_A_DIRECTORY:
    case OM_STATUS_CANCELLED:
        return 4;
    }
    return 4;
}

static Om_Status spawn_status_from_wire(uint32_t status)
{
    switch (status) {
    case 0: return OM_STATUS_OK;
    case 1: return OM_STATUS_NOT_FOUND;
    case 2: return OM_STATUS_OUT_OF_RANGE;
    case 3: return OM_STATUS_OUT_OF_MEMORY;
    case 4: return OM_STATUS_INVALID_ARGUMENT;
    default: return OM_STATUS_INVALID_ARGUMENT;
    }
}

static Om_Status receive_spawn_frame(Om_VM_Distribution_Peer *peer,
                                     const Om_Distribution_Frame *frame)
{
    if (frame->type != OM_DISTRIBUTION_FRAME_SPAWN
        || !frame_matches_peer_and_local_node(peer, frame)
        || frame->receiver.actor_id != 0
        || frame->message_fingerprint == 0
        || frame->payload.length != 4 || frame->payload.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t actor_symbol = read_u32_be(frame->payload.data);
    uint64_t spawned_actor_id = 0;
    const Om_Status spawn_status = om_vm_distribution_spawn_actor(
        peer->vm, actor_symbol, &spawned_actor_id);
    uint8_t payload[4] = {0};
    write_u32_be(payload, spawn_status_to_wire(spawn_status));
    return peer_send_frame(peer, OM_DISTRIBUTION_FRAME_SPAWN_ACK,
                           spawned_actor_id, frame->sender.actor_id,
                           frame->message_fingerprint,
                           (Om_Const_Byte_Slice){payload, sizeof(payload)});
}

static Om_Status receive_spawn_ack_frame(
    Om_VM_Distribution_Peer *peer,
    const Om_Distribution_Frame *frame)
{
    if (frame->type != OM_DISTRIBUTION_FRAME_SPAWN_ACK
        || !frame_matches_peer_and_local_node(peer, frame)
        || frame->message_fingerprint == 0
        || frame->payload.length != 4 || frame->payload.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t spawn_status_code = read_u32_be(frame->payload.data);
    if (spawn_status_code > 4) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Status spawn_status =
        spawn_status_from_wire(spawn_status_code);
    const bool succeeded = spawn_status == OM_STATUS_OK;
    if (succeeded != (frame->sender.actor_id != 0)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = peer->vm;
    om_vm_scheduler_lock(state);
    Om_VM_Remote_Spawn_Request *request = NULL;
    for (size_t index = 0; index < state->remote_spawn_request_count;
         ++index) {
        Om_VM_Remote_Spawn_Request *candidate =
            &state->remote_spawn_requests[index];
        if (candidate->active
            && candidate->request_id == frame->message_fingerprint) {
            request = candidate;
            break;
        }
    }
    if (request != NULL) {
        if (request->requester_actor_id != frame->receiver.actor_id) {
            om_vm_scheduler_unlock(state);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        request->status = spawn_status;
        request->spawned_actor_id = frame->sender.actor_id;
        request->completed = true;
    }
    om_vm_scheduler_broadcast(state);
    om_vm_scheduler_unlock(state);
    return OM_STATUS_OK;
}

static Om_Status receive_actor_down_frame(
    Om_VM_Distribution_Peer *peer,
    const Om_Distribution_Frame *frame)
{
    if (frame->type != OM_DISTRIBUTION_FRAME_ACTOR_DOWN
        || !frame_matches_peer_and_local_node(peer, frame)
        || frame->sender.actor_id == 0 || frame->receiver.actor_id == 0
        || frame->payload.length != 4 || frame->payload.data == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const uint32_t down_reason = read_u32_be(frame->payload.data);
    if (down_reason > 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_State *state = peer->vm;
    for (size_t index = 0;; ++index) {
        Om_VM_Remote_Watch watch = {0};
        bool deliver = false;
        om_vm_scheduler_lock(state);
        if (index < state->remote_watch_count) {
            Om_VM_Remote_Watch *entry = &state->remote_watches[index];
            watch = *entry;
            if (entry->peer_handle == peer->handle
                && entry->remote_actor_id == frame->sender.actor_id
                && entry->watcher_actor_id == frame->receiver.actor_id
                && !entry->notified) {
                entry->notified = true;
                deliver = true;
            }
        } else {
            om_vm_scheduler_unlock(state);
            break;
        }
        om_vm_scheduler_unlock(state);
        if (deliver) {
            const Om_Status status = enqueue_actor_down(
                state, watch.remote_actor_id, watch.watcher_actor_id,
                down_reason);
            if (status != OM_STATUS_OK) {
                return status;
            }
        }
    }
    return OM_STATUS_OK;
}

static Om_Status receive_peer_frame(Om_VM_Distribution_Peer *peer,
                                    const Om_Distribution_Frame *frame)
{
    switch (frame->type) {
    case OM_DISTRIBUTION_FRAME_MESSAGE:
        return receive_message_frame(peer, frame);
    case OM_DISTRIBUTION_FRAME_WATCH:
        return receive_watch_frame(peer, frame);
    case OM_DISTRIBUTION_FRAME_ACTOR_DOWN:
        return receive_actor_down_frame(peer, frame);
    case OM_DISTRIBUTION_FRAME_SPAWN:
        return receive_spawn_frame(peer, frame);
    case OM_DISTRIBUTION_FRAME_SPAWN_ACK:
        return receive_spawn_ack_frame(peer, frame);
    case OM_DISTRIBUTION_FRAME_HELLO:
    case OM_DISTRIBUTION_FRAME_HELLO_ACK:
    case OM_DISTRIBUTION_FRAME_WATCH_ACK:
        return OM_STATUS_INVALID_ARGUMENT;
    }
    return OM_STATUS_INVALID_ARGUMENT;
}

static void receive_peer_frames(Om_VM_Distribution_Peer *peer)
{
    while (atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        Om_Distribution_Frame frame = {0};
        const Om_Status status = om_distribution_connection_receive(
            &peer->connection, 0, &frame);
        if (status != OM_STATUS_OK) {
            break;
        }
        if (receive_peer_frame(peer, &frame) != OM_STATUS_OK) {
            break;
        }
    }
    peer_mark_disconnected(peer, true);
}

#if defined(_WIN32)
static DWORD WINAPI peer_thread_entry(void *opaque)
{
    receive_peer_frames((Om_VM_Distribution_Peer *)opaque);
    return 0;
}
#else
static void *peer_thread_entry(void *opaque)
{
    receive_peer_frames((Om_VM_Distribution_Peer *)opaque);
    return NULL;
}
#endif

static bool peer_thread_start(Om_VM_Distribution_Peer *peer)
{
    Om_VM_Distribution_Thread *thread = om_alloc(sizeof(*thread));
    if (thread == NULL) {
        return false;
    }
#if defined(_WIN32)
    thread->native = CreateThread(
        NULL, 0, peer_thread_entry, peer, 0, NULL);
    if (thread->native == NULL) {
        om_free(thread);
        return false;
    }
#else
    if (pthread_create(&thread->native, NULL, peer_thread_entry, peer) != 0) {
        om_free(thread);
        return false;
    }
#endif
    peer->thread_state = thread;
    peer->thread_started = true;
    return true;
}

static void peer_thread_join(Om_VM_Distribution_Peer *peer)
{
    if (!peer->thread_started || peer->thread_state == NULL) {
        return;
    }
    Om_VM_Distribution_Thread *thread = peer->thread_state;
#if defined(_WIN32)
    (void)WaitForSingleObject(thread->native, INFINITE);
    (void)CloseHandle(thread->native);
#else
    (void)pthread_join(thread->native, NULL);
#endif
    om_free(thread);
    peer->thread_state = NULL;
    peer->thread_started = false;
}

static void peer_destroy(Om_VM_Distribution_Peer *peer)
{
    if (peer == NULL) {
        return;
    }
    peer_thread_join(peer);
    om_distribution_connection_close(&peer->connection);
    if (peer->send_mutex != NULL) {
        Om_VM_Distribution_Mutex *mutex = peer->send_mutex;
        distribution_mutex_destroy(mutex);
        om_free(mutex);
    }
    om_free(peer);
}

static Om_Status peer_table_reserve(Om_VM_State *state, size_t required)
{
    if (required <= state->distribution_peer_capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = state->distribution_peer_capacity == 0
        ? 4 : state->distribution_peer_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            return OM_STATUS_OVERFLOW;
        }
        capacity *= 2;
    }
    Om_VM_Distribution_Peer **peers = om_realloc_array(
        state->distribution_peers, capacity, sizeof(*peers));
    if (peers == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    state->distribution_peers = peers;
    state->distribution_peer_capacity = capacity;
    return OM_STATUS_OK;
}

static Om_Status remote_watch_reserve(Om_VM_State *state, size_t required)
{
    if (required <= state->remote_watch_capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = state->remote_watch_capacity == 0
        ? 8 : state->remote_watch_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            return OM_STATUS_OVERFLOW;
        }
        capacity *= 2;
    }
    Om_VM_Remote_Watch *watches = om_realloc_array(
        state->remote_watches, capacity, sizeof(*watches));
    if (watches == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    state->remote_watches = watches;
    state->remote_watch_capacity = capacity;
    return OM_STATUS_OK;
}

static Om_Status remote_spawn_request_reserve(Om_VM_State *state,
                                              size_t required)
{
    if (required <= state->remote_spawn_request_capacity) {
        return OM_STATUS_OK;
    }
    size_t capacity = state->remote_spawn_request_capacity == 0
        ? 4 : state->remote_spawn_request_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            return OM_STATUS_OVERFLOW;
        }
        capacity *= 2;
    }
    Om_VM_Remote_Spawn_Request *requests = om_realloc_array(
        state->remote_spawn_requests, capacity, sizeof(*requests));
    if (requests == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    state->remote_spawn_requests = requests;
    state->remote_spawn_request_capacity = capacity;
    return OM_STATUS_OK;
}

static Om_Status enqueue_actor_down(Om_VM_State *state,
                                    uint64_t actor_id,
                                    uint64_t watcher_actor_id,
                                    uint32_t down_reason)
{
    state = distribution_shared_vm(state);
    if (actor_id == 0 || watcher_actor_id == 0 || down_reason > 2
        || state->actor_down_schema_index >= state->image->schema_count) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_VM_Message_Node *node =
        om_vm_distribution_message_node_create(state);
    if (node == NULL) {
        return OM_STATUS_OUT_OF_MEMORY;
    }
    const Om_VM_Value fields[2] = {
        om_vm_value_actor_id(actor_id),
        {.tag = OM_VM_VALUE_DOWN_REASON, .as.down_reason = down_reason}
    };
    Om_Status status = om_vm_heap_make_record(
        &node->transit_heap, OM_VM_OBJECT_MESSAGE,
        state->actor_down_schema_index, fields, 2, &node->message);
    if (status == OM_STATUS_OK) {
        status = om_vm_distribution_enqueue_message(
            state, watcher_actor_id, node);
    }
    if (status == OM_STATUS_NOT_FOUND || status == OM_STATUS_CANCELLED) {
        status = OM_STATUS_OK;
    }
    if (status != OM_STATUS_OK) {
        om_vm_distribution_message_node_destroy(node);
    }
    return status;
}

static void notify_peer_watches_disconnected(
    Om_VM_Distribution_Peer *peer)
{
    Om_VM_State *state = peer->vm;
    for (size_t index = 0;; ++index) {
        Om_VM_Remote_Watch watch = {0};
        bool notify = false;
        om_vm_scheduler_lock(state);
        if (index < state->remote_watch_count) {
            watch = state->remote_watches[index];
            if (watch.peer_handle == peer->handle && !watch.notified) {
                state->remote_watches[index].notified = true;
                notify = true;
            }
        } else {
            om_vm_scheduler_unlock(state);
            break;
        }
        om_vm_scheduler_unlock(state);
        if (notify) {
            (void)enqueue_actor_down(state, watch.remote_actor_id,
                                     watch.watcher_actor_id, 2);
        }
    }
}

static Om_Status send_actor_down_to_peer(Om_VM_Distribution_Peer *peer,
                                         uint64_t actor_id,
                                         uint64_t watcher_actor_id,
                                         uint32_t down_reason)
{
    if (down_reason > 2) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    uint8_t payload[4] = {
        (uint8_t)(down_reason >> 24),
        (uint8_t)(down_reason >> 16),
        (uint8_t)(down_reason >> 8),
        (uint8_t)down_reason
    };
    return peer_send_frame(peer, OM_DISTRIBUTION_FRAME_ACTOR_DOWN,
                           actor_id, watcher_actor_id,
                           0,
                           (Om_Const_Byte_Slice){payload, sizeof(payload)});
}

static Om_Status add_remote_watcher(Om_VM_Distribution_Peer *peer,
                                    uint64_t target_actor_id,
                                    uint64_t watcher_actor_id,
                                    uint32_t *stopped_reason,
                                    bool *is_stopped)
{
    Om_VM_State *state = peer->vm;
    if (target_actor_id == 0 || watcher_actor_id == 0
        || stopped_reason == NULL || is_stopped == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *stopped_reason = 1;
    *is_stopped = false;
    Om_Status status = OM_STATUS_NOT_FOUND;
    om_vm_scheduler_lock(state);
    const size_t actor_count = atomic_load_explicit(
        &state->actor_count, memory_order_acquire);
    if (target_actor_id <= actor_count
        && state->actors[(size_t)(target_actor_id - 1)].id
            == target_actor_id) {
        Om_VM_Actor_State *actor =
            &state->actors[(size_t)(target_actor_id - 1)];
        status = OM_STATUS_OK;
        *is_stopped = actor->stopped;
        *stopped_reason = actor->down_reason;
        if (!actor->stopped) {
            for (size_t index = 0; index < actor->remote_watcher_count;
                 ++index) {
                const Om_VM_Remote_Watcher *existing =
                    &actor->remote_watchers[index];
                if (existing->peer_handle == peer->handle
                    && existing->watcher_actor_id == watcher_actor_id) {
                    om_vm_scheduler_unlock(state);
                    return OM_STATUS_OK;
                }
            }
            if (actor->remote_watcher_count
                == actor->remote_watcher_capacity) {
                size_t capacity = actor->remote_watcher_capacity == 0
                    ? 4 : actor->remote_watcher_capacity * 2;
                if (capacity < actor->remote_watcher_capacity) {
                    status = OM_STATUS_OVERFLOW;
                } else {
                    Om_VM_Remote_Watcher *watchers = om_realloc_array(
                        actor->remote_watchers, capacity,
                        sizeof(*watchers));
                    if (watchers == NULL) {
                        status = OM_STATUS_OUT_OF_MEMORY;
                    } else {
                        actor->remote_watchers = watchers;
                        actor->remote_watcher_capacity = capacity;
                    }
                }
            }
            if (status == OM_STATUS_OK) {
                actor->remote_watchers[actor->remote_watcher_count++] =
                    (Om_VM_Remote_Watcher){
                        .peer_handle = peer->handle,
                        .watcher_actor_id = watcher_actor_id
                    };
            }
        }
    }
    om_vm_scheduler_unlock(state);
    return status;
}

Om_Status om_vm_distribution_attach_peer(
    Om_VM *vm,
    Om_Distribution_Connection *connection,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref)
{
    if (vm == NULL || vm->owned_state == NULL || connection == NULL
        || connection->owned_state == NULL || node_ref == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *node_ref = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    Om_VM_State *state = vm->owned_state;
    Om_Distribution_Node_Handshake remote = {0};
    Om_Status status = om_distribution_connection_handshake_peer(
        connection, &state->distribution_handshake, expected_peer, 0,
        &remote);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_VM_Distribution_Peer *peer = om_alloc(sizeof(*peer));
    if (peer == NULL) {
        om_distribution_connection_close(connection);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    *peer = (Om_VM_Distribution_Peer){
        .vm = state,
        .remote_handshake = remote,
        .connection = *connection
    };
    *connection = (Om_Distribution_Connection){0};
    atomic_init(&peer->connected, true);
    Om_VM_Distribution_Mutex *send_mutex =
        om_alloc(sizeof(*send_mutex));
    if (send_mutex == NULL
        || !distribution_mutex_init(send_mutex)) {
        om_free(send_mutex);
        om_distribution_connection_close(&peer->connection);
        om_free(peer);
        return OM_STATUS_OUT_OF_MEMORY;
    }
    peer->send_mutex = send_mutex;

    om_vm_scheduler_lock(state);
    if (atomic_load_explicit(&state->execution_active,
                             memory_order_acquire)) {
        status = OM_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; status == OM_STATUS_OK
         && index < state->distribution_peer_count; ++index) {
        if (om_distribution_node_id_equal(
                state->distribution_peers[index]
                    ->remote_handshake.node_id,
                remote.node_id)) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == OM_STATUS_OK) {
        status = peer_table_reserve(
            state, state->distribution_peer_count + 1);
    }
    if (status == OM_STATUS_OK) {
        peer->handle = (uint64_t)state->distribution_peer_count + 1;
        state->distribution_peers[state->distribution_peer_count++] = peer;
        if (!peer_thread_start(peer)) {
            state->distribution_peer_count -= 1;
            state->distribution_peers[state->distribution_peer_count] = NULL;
            status = OM_STATUS_OUT_OF_MEMORY;
        }
    }
    om_vm_scheduler_unlock(state);
    if (status != OM_STATUS_OK) {
        peer_destroy(peer);
        return status;
    }
    *node_ref = om_vm_value_node_ref(peer->handle);
    return OM_STATUS_OK;
}

Om_Status om_vm_distribution_connect_peer(
    Om_VM *vm,
    Om_String_View host,
    uint16_t port,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref)
{
    if (vm == NULL || vm->owned_state == NULL || node_ref == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Connection connection = {0};
    Om_Status status = om_distribution_connection_connect(
        &connection, host, port);
    if (status == OM_STATUS_OK) {
        status = om_vm_distribution_attach_peer(
            vm, &connection, expected_peer, node_ref);
    }
    om_distribution_connection_close(&connection);
    return status;
}

Om_Status om_vm_distribution_accept_peer(
    Om_VM *vm,
    const Om_Distribution_Listener *listener,
    const Om_Distribution_Node_Id *expected_peer,
    Om_VM_Value *node_ref)
{
    if (vm == NULL || vm->owned_state == NULL || listener == NULL
        || listener->owned_state == NULL || node_ref == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Distribution_Connection connection = {0};
    Om_Status status = om_distribution_listener_accept(listener, &connection);
    if (status == OM_STATUS_OK) {
        status = om_vm_distribution_attach_peer(
            vm, &connection, expected_peer, node_ref);
    }
    om_distribution_connection_close(&connection);
    return status;
}

Om_Status om_vm_distribution_actor_ref(
    Om_VM *vm,
    Om_VM_Value node_ref,
    uint64_t remote_actor_id,
    Om_VM_Value *actor_ref)
{
    if (vm == NULL || vm->owned_state == NULL || actor_ref == NULL
        || node_ref.tag != OM_VM_VALUE_NODE_REF
        || remote_actor_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *actor_ref = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    Om_VM_State *state = vm->owned_state;
    om_vm_scheduler_lock(state);
    Om_VM_Distribution_Peer *peer = peer_by_handle(
        state, node_ref.as.node_ref);
    uint64_t value = 0;
    const Om_Status status = peer == NULL
        ? OM_STATUS_NOT_FOUND
        : remote_ref_get_or_create_locked(
            state, peer, remote_actor_id, &value);
    om_vm_scheduler_unlock(state);
    if (status == OM_STATUS_OK) {
        *actor_ref = om_vm_value_actor_ref(value);
    }
    return status;
}

Om_Status om_vm_distribution_make_remote_actor_ref(
    Om_VM_State *state,
    uint64_t peer_handle,
    uint64_t remote_actor_id,
    Om_VM_Value *actor_ref)
{
    if (state == NULL || actor_ref == NULL || remote_actor_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *actor_ref = (Om_VM_Value){.tag = OM_VM_VALUE_UNINITIALIZED};
    state = distribution_shared_vm(state);
    om_vm_scheduler_lock(state);
    Om_VM_Distribution_Peer *peer = peer_by_handle(state, peer_handle);
    uint64_t value = 0;
    const Om_Status status = peer == NULL
        ? OM_STATUS_NOT_FOUND
        : remote_ref_get_or_create_locked(
            state, peer, remote_actor_id, &value);
    om_vm_scheduler_unlock(state);
    if (status == OM_STATUS_OK) {
        *actor_ref = om_vm_value_actor_ref(value);
    }
    return status;
}

Om_Status om_vm_distribution_spawn_remote(
    Om_VM_State *state,
    uint64_t peer_handle,
    uint32_t actor_symbol,
    uint64_t requester_actor_id,
    uint64_t *remote_actor_id)
{
    if (state == NULL || peer_handle == 0 || actor_symbol == UINT32_MAX
        || remote_actor_id == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    *remote_actor_id = 0;
    state = distribution_shared_vm(state);
    uint64_t request_id = 0;
    Om_VM_Distribution_Peer *peer = NULL;
    size_t request_index = SIZE_MAX;
    om_vm_scheduler_lock(state);
    peer = peer_by_handle(state, peer_handle);
    if (peer == NULL) {
        om_vm_scheduler_unlock(state);
        return OM_STATUS_NOT_FOUND;
    }
    if (!atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        om_vm_scheduler_unlock(state);
        return OM_STATUS_IO_ERROR;
    }
    if (state->next_distribution_request_id == 0) {
        om_vm_scheduler_unlock(state);
        return OM_STATUS_OVERFLOW;
    }
    request_id = state->next_distribution_request_id++;
    for (size_t index = 0; index < state->remote_spawn_request_count; ++index) {
        if (!state->remote_spawn_requests[index].active) {
            request_index = index;
            break;
        }
    }
    Om_Status status = OM_STATUS_OK;
    if (request_index == SIZE_MAX) {
        status = remote_spawn_request_reserve(
            state, state->remote_spawn_request_count + 1);
        if (status == OM_STATUS_OK) {
            request_index = state->remote_spawn_request_count++;
        }
    }
    if (status == OM_STATUS_OK) {
        state->remote_spawn_requests[request_index] =
            (Om_VM_Remote_Spawn_Request){
                .request_id = request_id,
                .requester_actor_id = requester_actor_id,
                .status = OM_STATUS_CANCELLED,
                .active = true
            };
    }
    om_vm_scheduler_unlock(state);
    if (status != OM_STATUS_OK) {
        return status;
    }

    uint8_t payload[4] = {0};
    write_u32_be(payload, actor_symbol);
    status = peer_send_frame(peer, OM_DISTRIBUTION_FRAME_SPAWN,
                             requester_actor_id, 0, request_id,
                             (Om_Const_Byte_Slice){payload, sizeof(payload)});
    if (status != OM_STATUS_OK) {
        om_vm_scheduler_lock(state);
        for (size_t index = 0;
             index < state->remote_spawn_request_count; ++index) {
            Om_VM_Remote_Spawn_Request *request =
                &state->remote_spawn_requests[index];
            if (request->active && request->request_id == request_id) {
                request->active = false;
                break;
            }
        }
        om_vm_scheduler_unlock(state);
        return status;
    }

    om_vm_scheduler_lock(state);
    for (;;) {
        Om_VM_Remote_Spawn_Request *request = NULL;
        for (size_t index = 0;
             index < state->remote_spawn_request_count; ++index) {
            Om_VM_Remote_Spawn_Request *candidate =
                &state->remote_spawn_requests[index];
            if (candidate->active && candidate->request_id == request_id) {
                request = candidate;
                break;
            }
        }
        if (request == NULL) {
            status = OM_STATUS_CANCELLED;
            break;
        }
        if (request->completed) {
            status = request->status;
            *remote_actor_id = request->spawned_actor_id;
            request->active = false;
            break;
        }
        if (!atomic_load_explicit(&peer->connected, memory_order_acquire)) {
            request->active = false;
            status = OM_STATUS_IO_ERROR;
            break;
        }
        om_vm_scheduler_wait(state);
    }
    om_vm_scheduler_unlock(state);
    return status;
}

Om_Status om_vm_distribution_watch_remote(
    Om_VM_State *state,
    uint64_t remote_ref_value,
    uint64_t watcher_actor_id)
{
    if (state == NULL || remote_ref_value == 0 || watcher_actor_id == 0) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    state = distribution_shared_vm(state);
    Om_VM_Distribution_Peer *peer = NULL;
    uint64_t remote_actor_id = 0;
    bool connected = false;
    Om_Status status = OM_STATUS_OK;
    om_vm_scheduler_lock(state);
    Om_VM_Remote_Actor_Ref *remote =
        remote_ref_by_value(state, remote_ref_value);
    if (remote == NULL) {
        status = OM_STATUS_NOT_FOUND;
    } else {
        peer = peer_by_handle(state, remote->peer_handle);
        remote_actor_id = remote->remote_actor_id;
        if (peer == NULL || remote_actor_id == 0) {
            status = OM_STATUS_NOT_FOUND;
        }
    }
    if (status == OM_STATUS_OK) {
        for (size_t index = 0; index < state->remote_watch_count; ++index) {
            const Om_VM_Remote_Watch *watch = &state->remote_watches[index];
            if (watch->peer_handle == peer->handle
                && watch->remote_actor_id == remote_actor_id
                && watch->watcher_actor_id == watcher_actor_id) {
                om_vm_scheduler_unlock(state);
                return OM_STATUS_OK;
            }
        }
        status = remote_watch_reserve(state, state->remote_watch_count + 1);
        if (status == OM_STATUS_OK) {
            connected = atomic_load_explicit(
                &peer->connected, memory_order_acquire);
            state->remote_watches[state->remote_watch_count++] =
                (Om_VM_Remote_Watch){
                    .peer_handle = peer->handle,
                    .remote_actor_id = remote_actor_id,
                    .watcher_actor_id = watcher_actor_id,
                    .notified = !connected
                };
        }
    }
    om_vm_scheduler_unlock(state);
    if (status != OM_STATUS_OK) {
        return status;
    }
    if (!connected) {
        return enqueue_actor_down(state, remote_actor_id,
                                  watcher_actor_id, 2);
    }
    status = peer_send_frame(peer, OM_DISTRIBUTION_FRAME_WATCH,
                             watcher_actor_id, remote_actor_id,
                             0,
                             (Om_Const_Byte_Slice){0});
    return !atomic_load_explicit(&peer->connected, memory_order_acquire)
        ? OM_STATUS_OK : status;
}

void om_vm_distribution_actor_stopped(Om_VM_State *state,
                                      uint64_t actor_id,
                                      uint32_t down_reason)
{
    if (state == NULL || actor_id == 0 || down_reason > 2) {
        return;
    }
    state = distribution_shared_vm(state);
    for (size_t index = 0;; ++index) {
        Om_VM_Remote_Watcher watcher = {0};
        Om_VM_Distribution_Peer *peer = NULL;
        om_vm_scheduler_lock(state);
        const size_t actor_count = atomic_load_explicit(
            &state->actor_count, memory_order_acquire);
        if (actor_id > actor_count
            || state->actors[(size_t)(actor_id - 1)].id != actor_id
            || index >= state->actors[(size_t)(actor_id - 1)]
                            .remote_watcher_count) {
            om_vm_scheduler_unlock(state);
            break;
        }
        watcher = state->actors[(size_t)(actor_id - 1)]
                      .remote_watchers[index];
        peer = peer_by_handle(state, watcher.peer_handle);
        om_vm_scheduler_unlock(state);
        if (peer != NULL) {
            (void)send_actor_down_to_peer(
                peer, actor_id, watcher.watcher_actor_id, down_reason);
        }
    }
}

Om_Status om_vm_distribution_send_message(
    Om_VM_State *state,
    uint64_t remote_ref_value,
    uint64_t sender_actor_id,
    Om_VM_Value message)
{
    if (state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    state = distribution_shared_vm(state);
    Om_VM_Distribution_Peer *peer = NULL;
    uint64_t remote_actor_id = 0;
    bool found_remote = false;
    om_vm_scheduler_lock(state);
    Om_VM_Remote_Actor_Ref *remote =
        remote_ref_by_value(state, remote_ref_value);
    if (remote != NULL) {
        found_remote = true;
        peer = peer_by_handle(state, remote->peer_handle);
        remote_actor_id = remote->remote_actor_id;
    }
    om_vm_scheduler_unlock(state);
    if (!found_remote || peer == NULL || remote_actor_id == 0) {
        return OM_STATUS_NOT_FOUND;
    }
    if (!atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        return OM_STATUS_IO_ERROR;
    }
    Om_Byte_Slice payload = {0};
    uint64_t fingerprint = 0;
    Om_Status status = serialize_vm_message(
        state, message, &payload, &fingerprint);
    if (status != OM_STATUS_OK) {
        return status;
    }
    Om_Distribution_Frame frame = {
        .protocol_version = OM_DISTRIBUTION_PROTOCOL_VERSION,
        .type = OM_DISTRIBUTION_FRAME_MESSAGE,
        .sender = {
            .node = state->distribution_handshake.node_id,
            .actor_id = sender_actor_id
        },
        .receiver = {
            .node = peer->remote_handshake.node_id,
            .actor_id = remote_actor_id
        },
        .message_fingerprint = fingerprint,
        .payload = {payload.data, payload.length}
    };
    Om_VM_Distribution_Mutex *send_mutex = peer->send_mutex;
    distribution_mutex_lock(send_mutex);
    if (atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        status = om_distribution_connection_send(&peer->connection, &frame, 0);
    } else {
        status = OM_STATUS_IO_ERROR;
    }
    distribution_mutex_unlock(send_mutex);
    om_free(payload.data);
    if (status != OM_STATUS_OK) {
        peer_mark_disconnected(peer, true);
    }
    return status;
}

void om_vm_distribution_shutdown(Om_VM_State *state)
{
    if (state == NULL) {
        return;
    }
    state = distribution_shared_vm(state);
    for (size_t index = 0; index < state->distribution_peer_count; ++index) {
        Om_VM_Distribution_Peer *peer = state->distribution_peers[index];
        if (peer != NULL) {
            peer_mark_disconnected(peer, false);
        }
    }
    for (size_t index = 0; index < state->distribution_peer_count; ++index) {
        peer_destroy(state->distribution_peers[index]);
    }
    om_free(state->distribution_peers);
    om_free(state->remote_actor_refs);
    om_free(state->remote_watches);
    om_free(state->remote_spawn_requests);
    state->distribution_peers = NULL;
    state->distribution_peer_count = 0;
    state->distribution_peer_capacity = 0;
    state->remote_actor_refs = NULL;
    state->remote_actor_ref_count = 0;
    state->remote_actor_ref_capacity = 0;
    state->remote_watches = NULL;
    state->remote_watch_count = 0;
    state->remote_watch_capacity = 0;
    state->remote_spawn_requests = NULL;
    state->remote_spawn_request_count = 0;
    state->remote_spawn_request_capacity = 0;
}
