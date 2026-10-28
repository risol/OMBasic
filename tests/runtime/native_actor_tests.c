#include "runtime_internal.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Probe_State {
    atomic_int active;
    uint64_t message_count;
    uint64_t last_sequence;
    void *retained_label;
    void *owned_pointer;
} Probe_State;

typedef struct Probe_Message {
    int64_t sequence;
    void *label;
    void *labels;
    int64_t command;
    uint64_t target_actor_id;
} Probe_Message;

typedef struct Actor_Down_Message {
    uint64_t actor_id;
    int64_t reason;
} Actor_Down_Message;

typedef struct Test_State {
    atomic_int active;
    uint64_t count;
} Test_State;

enum {
    PROBE_COMMAND_NONE = 0,
    PROBE_COMMAND_SELF_SEND = 1,
    PROBE_COMMAND_STOP = 2,
    PROBE_COMMAND_FAIL = 3,
    PROBE_COMMAND_WATCH = 4,
    PROBE_COMMAND_PARALLEL = 5,
    PROBE_COMMAND_WATCH_TWICE = 6,
    PROBE_COMMAND_STRESS = 7,
    PROBE_COMMAND_POINTER_STOP = 8
};

static const Om_Runtime_Type_Descriptor INT_TYPE = {
    .fingerprint = UINT64_C(0x1001),
    .kind = OM_RUNTIME_TYPE_INT,
    .size = sizeof(int64_t),
    .alignment = _Alignof(int64_t),
};

static const Om_Runtime_Type_Descriptor STRING_TYPE = {
    .fingerprint = UINT64_C(0x1002),
    .kind = OM_RUNTIME_TYPE_STRING,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

static const Om_Runtime_Type_Descriptor STRING_LIST_TYPE = {
    .fingerprint = UINT64_C(0x1003),
    .kind = OM_RUNTIME_TYPE_LIST,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
    .element_type = &STRING_TYPE,
};

static const Om_Runtime_Type_Descriptor NATIVE_POINTER_TYPE = {
    .fingerprint = UINT64_C(0x1004),
    .kind = OM_RUNTIME_TYPE_NATIVE_POINTER,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

static const Om_Runtime_Field_Descriptor PROBE_STATE_FIELDS[] = {
    {.offset = offsetof(Probe_State, retained_label), .type = &STRING_TYPE},
    {.offset = offsetof(Probe_State, owned_pointer),
     .type = &NATIVE_POINTER_TYPE},
};

static const Om_Runtime_Type_Descriptor PROBE_STATE_TYPE = {
    .fingerprint = UINT64_C(0x2001),
    .kind = OM_RUNTIME_TYPE_RECORD,
    .size = sizeof(Probe_State),
    .alignment = _Alignof(Probe_State),
    .fields = PROBE_STATE_FIELDS,
    .field_count = sizeof(PROBE_STATE_FIELDS) / sizeof(PROBE_STATE_FIELDS[0]),
};

static const Om_Runtime_Field_Descriptor TEST_STATE_FIELDS[] = {
    {.offset = offsetof(Test_State, count), .type = &INT_TYPE},
};

static const Om_Runtime_Type_Descriptor TEST_STATE_TYPE = {
    .fingerprint = UINT64_C(0x2002),
    .kind = OM_RUNTIME_TYPE_RECORD,
    .size = sizeof(Test_State),
    .alignment = _Alignof(Test_State),
    .fields = TEST_STATE_FIELDS,
    .field_count = sizeof(TEST_STATE_FIELDS) / sizeof(TEST_STATE_FIELDS[0]),
};

static const Om_Runtime_Field_Descriptor PROBE_MESSAGE_FIELDS[] = {
    {.offset = offsetof(Probe_Message, sequence), .type = &INT_TYPE},
    {.offset = offsetof(Probe_Message, label), .type = &STRING_TYPE},
    {.offset = offsetof(Probe_Message, labels), .type = &STRING_LIST_TYPE},
    {.offset = offsetof(Probe_Message, command), .type = &INT_TYPE},
    {.offset = offsetof(Probe_Message, target_actor_id), .type = &INT_TYPE},
};

static const Om_Runtime_Type_Descriptor PROBE_MESSAGE_TYPE = {
    .fingerprint = UINT64_C(0x3001),
    .kind = OM_RUNTIME_TYPE_MESSAGE,
    .size = sizeof(Probe_Message),
    .alignment = _Alignof(Probe_Message),
    .fields = PROBE_MESSAGE_FIELDS,
    .field_count = sizeof(PROBE_MESSAGE_FIELDS)
        / sizeof(PROBE_MESSAGE_FIELDS[0]),
};

static const Om_Runtime_Field_Descriptor ACTOR_DOWN_FIELDS[] = {
    {.offset = offsetof(Actor_Down_Message, actor_id), .type = &INT_TYPE},
    {.offset = offsetof(Actor_Down_Message, reason), .type = &INT_TYPE},
};

static const Om_Runtime_Type_Descriptor ACTOR_DOWN_TYPE = {
    .fingerprint = UINT64_C(0x3002),
    .kind = OM_RUNTIME_TYPE_MESSAGE,
    .size = sizeof(Actor_Down_Message),
    .alignment = _Alignof(Actor_Down_Message),
    .fields = ACTOR_DOWN_FIELDS,
    .field_count = sizeof(ACTOR_DOWN_FIELDS) / sizeof(ACTOR_DOWN_FIELDS[0]),
};

static atomic_size_t order_count;
static int64_t observed_order[8192];
static atomic_size_t transfer_ok;
static atomic_size_t handler_failures;
static atomic_int actor_down_reason;
static atomic_size_t actor_down_count;
static atomic_size_t parallel_entered;
static atomic_size_t parallel_active;
static atomic_size_t parallel_max_active;
static atomic_bool parallel_release;
static atomic_size_t stress_message_count;
static atomic_size_t actor_pointer_release_count;
static int actor_pointer_resource;

static void release_actor_pointer(void *pointer)
{
    if (pointer == &actor_pointer_resource) {
        (void)atomic_fetch_add_explicit(&actor_pointer_release_count, 1,
                                        memory_order_release);
    }
}

static Om_Status probe_handler(Om_Execution_Context *context,
                               void *state_payload,
                               const void *message_payload);
static Om_Status watcher_probe_handler(Om_Execution_Context *context,
                                       void *state_payload,
                                       const void *message_payload);
static Om_Status actor_down_handler(Om_Execution_Context *context,
                                    void *state_payload,
                                    const void *message_payload);

static Om_Status initialize_probe_state(Om_Execution_Context *context,
                                        void *state_payload)
{
    (void)context;
    Probe_State *state = state_payload;
    if (state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_init(&state->active, 0);
    state->message_count = 0;
    state->last_sequence = 0;
    state->retained_label = NULL;
    state->owned_pointer = NULL;
    return OM_STATUS_OK;
}

static Om_Status initialize_test_state(Om_Execution_Context *context,
                                       void *state_payload)
{
    (void)context;
    Test_State *state = state_payload;
    if (state == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_init(&state->active, 0);
    state->count = 0;
    return OM_STATUS_OK;
}

static Om_Status faulting_state_initializer(Om_Execution_Context *context,
                                           void *state_payload)
{
    (void)state_payload;
    om_runtime_fault(context, OM_RUNTIME_FAULT_OPERATION_FAILED, UINT32_MAX);
}

static const Om_Runtime_Actor_Handler_Descriptor PROBE_HANDLERS[] = {
    {&PROBE_MESSAGE_TYPE, probe_handler},
};

static const Om_Runtime_Actor_Handler_Descriptor WATCHER_HANDLERS[] = {
    {&PROBE_MESSAGE_TYPE, watcher_probe_handler},
    {&ACTOR_DOWN_TYPE, actor_down_handler},
};

static const Om_Runtime_Actor_Descriptor PROBE_ACTOR = {
    .fingerprint = UINT64_C(0x4001),
    .canonical_name = "Test.Probe",
    .state_type = &PROBE_STATE_TYPE,
    .initialize_state = initialize_probe_state,
    .handlers = PROBE_HANDLERS,
    .handler_count = sizeof(PROBE_HANDLERS) / sizeof(PROBE_HANDLERS[0]),
};

static const Om_Runtime_Actor_Descriptor WATCHER_ACTOR = {
    .fingerprint = UINT64_C(0x4002),
    .canonical_name = "Test.Watcher",
    .state_type = &TEST_STATE_TYPE,
    .initialize_state = initialize_test_state,
    .handlers = WATCHER_HANDLERS,
    .handler_count = sizeof(WATCHER_HANDLERS) / sizeof(WATCHER_HANDLERS[0]),
};

static const Om_Runtime_Actor_Descriptor FAULTING_INIT_ACTOR = {
    .fingerprint = UINT64_C(0x4003),
    .canonical_name = "Test.FaultingInit",
    .state_type = &TEST_STATE_TYPE,
    .initialize_state = faulting_state_initializer,
};

static const Om_Runtime_Actor_Descriptor *const ACTORS[] = {
    &PROBE_ACTOR,
    &WATCHER_ACTOR,
    &FAULTING_INIT_ACTOR,
};

static const Om_Runtime_Program_Descriptor PROGRAM = {
    .abi_version = OM_RUNTIME_ABI_VERSION,
    .actors = ACTORS,
    .actor_count = sizeof(ACTORS) / sizeof(ACTORS[0]),
    .actor_down_message_type = &ACTOR_DOWN_TYPE,
    .actor_down_actor_id_offset = offsetof(Actor_Down_Message, actor_id),
    .actor_down_reason_offset = offsetof(Actor_Down_Message, reason),
    .collection_threshold = 1024,
};

#define TEST_ASSERT(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "%s:%d: assertion failed: %s\n",          \
                          __FILE__, __LINE__, #condition);                       \
            goto cleanup;                                                       \
        }                                                                        \
    } while (false)

static Om_Status create_probe_message(Om_Execution_Context *context,
                                      int64_t sequence,
                                      int64_t command,
                                      uint64_t target_actor_id,
                                      void *label,
                                      void *labels,
                                      void **result)
{
    Om_Status status = om_runtime_record_create(context, &PROBE_MESSAGE_TYPE,
                                                result);
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, *result, 0,
            (Om_Runtime_Value){.integer = sequence});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, *result, 1,
            (Om_Runtime_Value){.managed = label});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, *result, 2,
            (Om_Runtime_Value){.managed = labels});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, *result, 3,
            (Om_Runtime_Value){.integer = command});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, *result, 4,
            (Om_Runtime_Value){.integer = (int64_t)target_actor_id});
    }
    return status;
}

static void note_parallel_active(void)
{
    const size_t active = atomic_fetch_add_explicit(&parallel_active, 1,
        memory_order_acq_rel) + 1;
    size_t previous = atomic_load_explicit(&parallel_max_active,
                                            memory_order_acquire);
    while (previous < active
           && !atomic_compare_exchange_weak_explicit(&parallel_max_active,
                &previous, active, memory_order_acq_rel,
                memory_order_acquire)) {
    }
}

static Om_Status probe_handler(Om_Execution_Context *context,
                               void *state_payload,
                               const void *message_payload)
{
    Probe_State *state = state_payload;
    const Probe_Message *message = message_payload;
    if (state == NULL || message == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const int active = atomic_fetch_add_explicit(&state->active, 1,
                                                 memory_order_acq_rel);
    if (active != 0) {
        (void)atomic_fetch_add_explicit(&handler_failures, 1,
                                        memory_order_relaxed);
    }
    Om_Status status = OM_STATUS_OK;
    ++state->message_count;
    state->last_sequence = (uint64_t)message->sequence;
    if (message->sequence > 0) {
        const size_t index = atomic_fetch_add_explicit(&order_count, 1,
            memory_order_acq_rel);
        if (index < sizeof(observed_order) / sizeof(observed_order[0])) {
            observed_order[index] = message->sequence;
        } else {
            status = OM_STATUS_OUT_OF_RANGE;
        }
    }
    if (message->command == PROBE_COMMAND_STRESS) {
        (void)atomic_fetch_add_explicit(&stress_message_count, 1,
                                        memory_order_relaxed);
    }
    if (status == OM_STATUS_OK && message->label != NULL) {
        Om_String_View view = {0};
        Om_Runtime_Value first = {0};
        Om_String_View list_view = {0};
        size_t list_length = 0;
        status = om_runtime_string_view(context, message->label, &view);
        if (status == OM_STATUS_OK) {
            status = om_runtime_list_length(context, message->labels,
                                            &list_length);
        }
        if (status == OM_STATUS_OK && list_length != 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_list_get(context, message->labels, 0, &first);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_string_view(context, first.managed,
                                            &list_view);
        }
        if (status == OM_STATUS_OK
            && view.length == sizeof("cross-heap message") - 1
            && list_view.length == sizeof("list element") - 1
            && memcmp(view.data, "cross-heap message", view.length) == 0
            && memcmp(list_view.data, "list element", list_view.length) == 0) {
            state->retained_label = message->label;
            (void)atomic_fetch_add_explicit(&transfer_ok, 1,
                                            memory_order_release);
        } else if (status == OM_STATUS_OK) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == OM_STATUS_OK && state->retained_label != NULL) {
        status = om_runtime_collect(context);
        Om_String_View retained = {0};
        if (status == OM_STATUS_OK) {
            status = om_runtime_string_view(context, state->retained_label,
                                            &retained);
        }
        if (status == OM_STATUS_OK
            && retained.length != sizeof("cross-heap message") - 1) {
            status = OM_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == OM_STATUS_OK
        && message->command == PROBE_COMMAND_SELF_SEND) {
        Om_Runtime_Actor_Ref self = {0};
        void *next = NULL;
        status = om_runtime_actor_self(context, &self);
        if (status == OM_STATUS_OK) {
            status = create_probe_message(context, message->sequence + 1,
                PROBE_COMMAND_NONE, 0, NULL, NULL, &next);
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_actor_send(context, self, next);
        }
    } else if (status == OM_STATUS_OK
               && message->command == PROBE_COMMAND_STOP) {
        status = om_runtime_actor_stop(context);
    } else if (status == OM_STATUS_OK
               && message->command == PROBE_COMMAND_FAIL) {
        status = OM_STATUS_CANCELLED;
    } else if (status == OM_STATUS_OK
               && message->command == PROBE_COMMAND_PARALLEL) {
        note_parallel_active();
        (void)atomic_fetch_add_explicit(&parallel_entered, 1,
                                        memory_order_release);
        while (!atomic_load_explicit(&parallel_release, memory_order_acquire)) {
        }
        (void)atomic_fetch_sub_explicit(&parallel_active, 1,
                                        memory_order_acq_rel);
    } else if (status == OM_STATUS_OK
               && message->command == PROBE_COMMAND_POINTER_STOP) {
        status = om_runtime_native_pointer_create(context,
            &actor_pointer_resource, release_actor_pointer, NULL,
            &state->owned_pointer);
        if (status == OM_STATUS_OK) {
            status = om_runtime_actor_stop(context);
        }
    }
    (void)atomic_fetch_sub_explicit(&state->active, 1, memory_order_acq_rel);
    return status;
}

static Om_Status watcher_probe_handler(Om_Execution_Context *context,
                                       void *state_payload,
                                       const void *message_payload)
{
    (void)state_payload;
    const Probe_Message *message = message_payload;
    if (message == NULL
        || (message->command != PROBE_COMMAND_WATCH
            && message->command != PROBE_COMMAND_WATCH_TWICE)) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    const Om_Runtime_Actor_Ref target = {.id = message->target_actor_id};
    Om_Status status = om_runtime_actor_watch(context, target);
    if (status == OM_STATUS_OK
        && message->command == PROBE_COMMAND_WATCH_TWICE) {
        status = om_runtime_actor_watch(context, target);
    }
    return status;
}

static Om_Status actor_down_handler(Om_Execution_Context *context,
                                    void *state_payload,
                                    const void *message_payload)
{
    (void)context;
    (void)state_payload;
    const Actor_Down_Message *message = message_payload;
    if (message == NULL) {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&actor_down_reason, (int)message->reason,
                          memory_order_release);
    (void)atomic_fetch_add_explicit(&actor_down_count, 1,
                                    memory_order_release);
    return OM_STATUS_OK;
}

static bool send_command(Om_Execution_Context *context,
                         Om_Runtime_Actor_Ref target,
                         int64_t sequence,
                         int64_t command,
                         uint64_t target_actor_id)
{
    void *message = NULL;
    Om_Status status = create_probe_message(context, sequence, command,
        target_actor_id, NULL, NULL, &message);
    Om_Runtime_Root_Frame *root = status == OM_STATUS_OK
        ? om_runtime_roots_push(context, &message, 1) : NULL;
    if (status == OM_STATUS_OK && root == NULL) {
        status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_actor_send(context, target, message);
    }
    if (root != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, root);
        if (status == OM_STATUS_OK) {
            status = pop_status;
        }
    }
    return status == OM_STATUS_OK;
}

static bool wait_for_parallel_handlers(size_t expected)
{
    for (size_t attempt = 0; attempt < 100000000; ++attempt) {
        if (atomic_load_explicit(&parallel_entered, memory_order_acquire)
            >= expected) {
            return true;
        }
    }
    return false;
}

static int run_message_stress(size_t message_count)
{
    Om_Execution_Context *context = om_runtime_context_create(4096);
    if (context == NULL) {
        return 1;
    }
    if (om_runtime_actor_system_start(context, &PROGRAM, 2) != OM_STATUS_OK) {
        om_runtime_context_destroy(context);
        return 1;
    }
    Om_Runtime_Actor_Ref actor = {0};
    if (om_runtime_actor_spawn(context, &PROBE_ACTOR, &actor) != OM_STATUS_OK) {
        om_runtime_context_destroy(context);
        return 1;
    }
    atomic_store_explicit(&stress_message_count, 0, memory_order_release);
    for (size_t index = 0; index < message_count; ++index) {
        if (!send_command(context, actor, 0, PROBE_COMMAND_STRESS, 0)) {
            om_runtime_context_destroy(context);
            return 1;
        }
    }
    const Om_Status wait_status = om_runtime_actor_system_wait(context);
    const size_t processed = atomic_load_explicit(&stress_message_count,
                                                   memory_order_acquire);
    om_runtime_context_destroy(context);
    if (wait_status != OM_STATUS_OK || processed != message_count) {
        (void)fprintf(stderr, "actor stress processed %zu of %zu messages\n",
                      processed, message_count);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--stress") == 0) {
        return run_message_stress(2000000);
    }
    if (argc != 1) {
        return 2;
    }
    bool passed = false;
    Om_Execution_Context *context = om_runtime_context_create(2048);
    TEST_ASSERT(context != NULL);
    TEST_ASSERT(om_runtime_actor_system_start(context, &PROGRAM, 2)
                == OM_STATUS_OK);

    Om_Runtime_Actor_Ref rejected_actor = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &FAULTING_INIT_ACTOR,
        &rejected_actor) == OM_STATUS_CANCELLED);

    atomic_store_explicit(&actor_pointer_release_count, 0,
                          memory_order_release);
    Om_Runtime_Actor_Ref releasing_actor = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR,
        &releasing_actor) == OM_STATUS_OK);
    TEST_ASSERT(send_command(context, releasing_actor, 0,
        PROBE_COMMAND_POINTER_STOP, 0));
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&actor_pointer_release_count,
        memory_order_acquire) == 1);

    Om_Runtime_Actor_Ref probe = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &probe)
                == OM_STATUS_OK);
    void *label = NULL;
    void *list = NULL;
    void *list_roots[2] = {NULL, NULL};
    Om_Runtime_Root_Frame *list_root = NULL;
    TEST_ASSERT(om_runtime_string_create_utf8_default(context,
        (const uint8_t *)"cross-heap message",
        sizeof("cross-heap message") - 1, &label) == OM_STATUS_OK);
    list_roots[0] = label;
    list_root = om_runtime_roots_push(context, list_roots, 2);
    TEST_ASSERT(list_root != NULL);
    TEST_ASSERT(om_runtime_list_create(context, &STRING_LIST_TYPE, &list)
                == OM_STATUS_OK);
    list_roots[1] = list;
    void *element = NULL;
    TEST_ASSERT(om_runtime_string_create_utf8_default(context,
        (const uint8_t *)"list element", sizeof("list element") - 1,
        &element) == OM_STATUS_OK);
    list_roots[0] = element;
    TEST_ASSERT(om_runtime_list_append(context, list,
        (Om_Runtime_Value){.managed = element}) == OM_STATUS_OK);
    void *message = NULL;
    TEST_ASSERT(create_probe_message(context, 1, PROBE_COMMAND_SELF_SEND,
        0, label, list, &message) == OM_STATUS_OK);
    void *message_root_slot = message;
    Om_Runtime_Root_Frame *message_root = om_runtime_roots_push(context,
        &message_root_slot, 1);
    TEST_ASSERT(message_root != NULL);
    TEST_ASSERT(om_runtime_actor_send(context, probe, message_root_slot)
                == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_roots_pop(context, message_root) == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_roots_pop(context, list_root) == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&order_count, memory_order_acquire) == 2);
    TEST_ASSERT(observed_order[0] == 1 && observed_order[1] == 2);
    TEST_ASSERT(atomic_load_explicit(&transfer_ok, memory_order_acquire) == 1);

    atomic_store_explicit(&order_count, 0, memory_order_release);
    Om_Runtime_Actor_Ref fifo = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &fifo)
                == OM_STATUS_OK);
    for (int64_t index = 0; index < 2048; ++index) {
        TEST_ASSERT(send_command(context, fifo, 100 + index,
                                 PROBE_COMMAND_NONE, 0));
    }
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&order_count, memory_order_acquire)
                == 2048);
    for (size_t index = 0; index < 2048; ++index) {
        TEST_ASSERT(observed_order[index] == (int64_t)(100 + index));
    }

    Om_Runtime_Actor_Ref target = {0};
    Om_Runtime_Actor_Ref watcher = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &target)
                == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_actor_spawn(context, &WATCHER_ACTOR, &watcher)
                == OM_STATUS_OK);
    atomic_store_explicit(&actor_down_reason, -1, memory_order_release);
    atomic_store_explicit(&actor_down_count, 0, memory_order_release);
    TEST_ASSERT(send_command(context, watcher, 0, PROBE_COMMAND_WATCH,
                             target.id));
    TEST_ASSERT(send_command(context, target, 0, PROBE_COMMAND_STOP, 0));
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&actor_down_reason, memory_order_acquire)
                == OM_RUNTIME_ACTOR_DOWN_NORMAL);
    TEST_ASSERT(atomic_load_explicit(&actor_down_count, memory_order_acquire)
                == 1);

    target = (Om_Runtime_Actor_Ref){0};
    watcher = (Om_Runtime_Actor_Ref){0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &target)
                == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_actor_spawn(context, &WATCHER_ACTOR, &watcher)
                == OM_STATUS_OK);
    atomic_store_explicit(&actor_down_reason, -1, memory_order_release);
    atomic_store_explicit(&actor_down_count, 0, memory_order_release);
    TEST_ASSERT(send_command(context, watcher, 0, PROBE_COMMAND_WATCH,
                             target.id));
    TEST_ASSERT(send_command(context, target, 0, PROBE_COMMAND_FAIL, 0));
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&actor_down_reason, memory_order_acquire)
                == OM_RUNTIME_ACTOR_DOWN_FAILED);
    TEST_ASSERT(atomic_load_explicit(&actor_down_count, memory_order_acquire)
                == 1);

    target = (Om_Runtime_Actor_Ref){0};
    watcher = (Om_Runtime_Actor_Ref){0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &target)
                == OM_STATUS_OK);
    TEST_ASSERT(send_command(context, target, 0, PROBE_COMMAND_STOP, 0));
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_actor_spawn(context, &WATCHER_ACTOR, &watcher)
                == OM_STATUS_OK);
    atomic_store_explicit(&actor_down_reason, -1, memory_order_release);
    atomic_store_explicit(&actor_down_count, 0, memory_order_release);
    TEST_ASSERT(send_command(context, watcher, 0,
                             PROBE_COMMAND_WATCH_TWICE, target.id));
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&actor_down_reason, memory_order_acquire)
                == OM_RUNTIME_ACTOR_DOWN_NORMAL);
    TEST_ASSERT(atomic_load_explicit(&actor_down_count, memory_order_acquire)
                == 1);

    atomic_store_explicit(&order_count, 0, memory_order_release);
    atomic_store_explicit(&parallel_entered, 0, memory_order_release);
    atomic_store_explicit(&parallel_release, false, memory_order_release);
    Om_Runtime_Actor_Ref stopper = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &stopper)
                == OM_STATUS_OK);
    TEST_ASSERT(send_command(context, stopper, 0,
                             PROBE_COMMAND_PARALLEL, 0));
    TEST_ASSERT(wait_for_parallel_handlers(1));
    TEST_ASSERT(send_command(context, stopper, 0, PROBE_COMMAND_STOP, 0));
    for (int64_t index = 0; index < 128; ++index) {
        TEST_ASSERT(send_command(context, stopper, 9000 + index,
                                 PROBE_COMMAND_NONE, 0));
    }
    atomic_store_explicit(&parallel_release, true, memory_order_release);
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(atomic_load_explicit(&order_count, memory_order_acquire) == 0);

    atomic_store_explicit(&parallel_entered, 0, memory_order_release);
    atomic_store_explicit(&parallel_active, 0, memory_order_release);
    atomic_store_explicit(&parallel_max_active, 0, memory_order_release);
    atomic_store_explicit(&parallel_release, false, memory_order_release);
    Om_Runtime_Actor_Ref serial = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &serial)
                == OM_STATUS_OK);
    TEST_ASSERT(send_command(context, serial, 0, PROBE_COMMAND_PARALLEL, 0));
    TEST_ASSERT(send_command(context, serial, 0, PROBE_COMMAND_PARALLEL, 0));
    const bool same_actor_ran_in_parallel = wait_for_parallel_handlers(2);
    atomic_store_explicit(&parallel_release, true, memory_order_release);
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(!same_actor_ran_in_parallel);
    TEST_ASSERT(atomic_load_explicit(&parallel_max_active,
                                     memory_order_acquire) == 1);

    atomic_store_explicit(&parallel_entered, 0, memory_order_release);
    atomic_store_explicit(&parallel_active, 0, memory_order_release);
    atomic_store_explicit(&parallel_max_active, 0, memory_order_release);
    atomic_store_explicit(&parallel_release, false, memory_order_release);
    Om_Runtime_Actor_Ref first = {0};
    Om_Runtime_Actor_Ref second = {0};
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &first)
                == OM_STATUS_OK);
    TEST_ASSERT(om_runtime_actor_spawn(context, &PROBE_ACTOR, &second)
                == OM_STATUS_OK);
    TEST_ASSERT(send_command(context, first, 0, PROBE_COMMAND_PARALLEL, 0));
    TEST_ASSERT(send_command(context, second, 0, PROBE_COMMAND_PARALLEL, 0));
    const bool ran_in_parallel = wait_for_parallel_handlers(2);
    atomic_store_explicit(&parallel_release, true, memory_order_release);
    TEST_ASSERT(om_runtime_actor_system_wait(context) == OM_STATUS_OK);
    TEST_ASSERT(ran_in_parallel);
    TEST_ASSERT(atomic_load_explicit(&parallel_max_active,
                                     memory_order_acquire) >= 2);
    TEST_ASSERT(atomic_load_explicit(&handler_failures,
                                     memory_order_acquire) == 0);
    passed = true;

cleanup:
    if (context != NULL) {
        om_runtime_context_destroy(context);
    }
    if (!passed) {
        return 1;
    }
    (void)puts("native actor runtime tests passed");
    return 0;
}
