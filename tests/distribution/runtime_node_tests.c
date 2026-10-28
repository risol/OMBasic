#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ombasic/runtime/distribution.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct Test_Message {
    int64_t command;
    int64_t number;
    uint64_t actor_ref;
    void *text;
} Test_Message;

typedef struct Actor_Down_Message {
    uint64_t actor_id;
    int64_t reason;
} Actor_Down_Message;

typedef struct Test_State {
    uint64_t unused;
} Test_State;

enum {
    COMMAND_START = 1,
    COMMAND_REQUEST = 2,
    COMMAND_RESPONSE = 3,
    COMMAND_WATCH_ONLY = 4,
};

static const Om_Runtime_Type_Descriptor INT_TYPE = {
    .fingerprint = UINT64_C(0x810001),
    .kind = OM_RUNTIME_TYPE_INT,
    .size = sizeof(int64_t),
    .alignment = _Alignof(int64_t),
};

static const Om_Runtime_Type_Descriptor ACTOR_REF_TYPE = {
    .fingerprint = UINT64_C(0x810002),
    .kind = OM_RUNTIME_TYPE_ACTOR_REF,
    .size = sizeof(uint64_t),
    .alignment = _Alignof(uint64_t),
};

static const Om_Runtime_Type_Descriptor STRING_TYPE = {
    .fingerprint = UINT64_C(0x810003),
    .kind = OM_RUNTIME_TYPE_STRING,
    .size = sizeof(void *),
    .alignment = _Alignof(void *),
};

static const Om_Runtime_Field_Descriptor MESSAGE_FIELDS[] = {
    {.offset = offsetof(Test_Message, command), .type = &INT_TYPE},
    {.offset = offsetof(Test_Message, number), .type = &INT_TYPE},
    {.offset = offsetof(Test_Message, actor_ref), .type = &ACTOR_REF_TYPE},
    {.offset = offsetof(Test_Message, text), .type = &STRING_TYPE},
};

static const Om_Runtime_Type_Descriptor MESSAGE_TYPE = {
    .fingerprint = UINT64_C(0x820001),
    .kind = OM_RUNTIME_TYPE_MESSAGE,
    .size = sizeof(Test_Message),
    .alignment = _Alignof(Test_Message),
    .fields = MESSAGE_FIELDS,
    .field_count = sizeof(MESSAGE_FIELDS) / sizeof(MESSAGE_FIELDS[0]),
};

static const Om_Runtime_Field_Descriptor ACTOR_DOWN_FIELDS[] = {
    {.offset = offsetof(Actor_Down_Message, actor_id), .type = &INT_TYPE},
    {.offset = offsetof(Actor_Down_Message, reason), .type = &INT_TYPE},
};

static const Om_Runtime_Type_Descriptor ACTOR_DOWN_TYPE = {
    .fingerprint = UINT64_C(0x820002),
    .kind = OM_RUNTIME_TYPE_MESSAGE,
    .size = sizeof(Actor_Down_Message),
    .alignment = _Alignof(Actor_Down_Message),
    .fields = ACTOR_DOWN_FIELDS,
    .field_count = sizeof(ACTOR_DOWN_FIELDS) / sizeof(ACTOR_DOWN_FIELDS[0]),
};

static const Om_Runtime_Type_Descriptor STATE_TYPE = {
    .fingerprint = UINT64_C(0x830001),
    .kind = OM_RUNTIME_TYPE_RECORD,
    .size = sizeof(Test_State),
    .alignment = _Alignof(Test_State),
};

static atomic_int worker_finished;
static atomic_int client_response_seen;
static atomic_int client_down_seen;
static atomic_int client_failed;
static atomic_int expected_down_reason;

static bool require_status(Om_Status status, const char *operation)
{
    if (status == OM_STATUS_OK) return true;
    (void)fprintf(stderr, "%s failed: %s (%d)\n", operation,
                  om_status_name(status), (int)status);
    return false;
}

static Om_Status initialize_state(Om_Execution_Context *context,
                                  void *state_payload)
{
    (void)context;
    if (state_payload == NULL) return OM_STATUS_INVALID_ARGUMENT;
    ((Test_State *)state_payload)->unused = 0;
    return OM_STATUS_OK;
}

static Om_Status worker_handler(Om_Execution_Context *context,
                                void *state_payload,
                                const void *message_payload)
{
    (void)state_payload;
    const Test_Message *request = message_payload;
    if (request == NULL || request->command != COMMAND_REQUEST) {
        atomic_store_explicit(&client_failed, 1, memory_order_release);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    Om_Runtime_Actor_Ref self = {0};
    Om_Status status = om_runtime_actor_self(context, &self);
    void *response = NULL;
    Om_Runtime_Root_Frame *root = NULL;
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(context, &MESSAGE_TYPE, &response);
    }
    if (status == OM_STATUS_OK) {
        root = om_runtime_roots_push(context, &response, 1);
        if (root == NULL) status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, response, 0,
            (Om_Runtime_Value){.integer = COMMAND_RESPONSE});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, response, 1,
            (Om_Runtime_Value){.integer = request->number + 1});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, response, 2,
            (Om_Runtime_Value){.integer = (int64_t)self.id});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, response, 3,
            (Om_Runtime_Value){.managed = request->text});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_actor_send(context,
            (Om_Runtime_Actor_Ref){request->actor_ref}, response);
    }
    if (root != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, root);
        if (status == OM_STATUS_OK) status = pop_status;
    }
    if (status == OM_STATUS_OK) status = om_runtime_actor_stop(context);
    if (status == OM_STATUS_OK) {
        atomic_store_explicit(&worker_finished, 1, memory_order_release);
    } else {
        atomic_store_explicit(&client_failed, 1, memory_order_release);
    }
    return status;
}

static bool text_equals(Om_Execution_Context *context,
                        const void *string,
                        const char *expected)
{
    Om_String_View view = {0};
    if (om_runtime_string_view(context, string, &view) != OM_STATUS_OK) {
        return false;
    }
    const size_t length = strlen(expected);
    return view.length == length
        && memcmp(view.data, expected, length) == 0;
}

static Om_Status client_handler(Om_Execution_Context *context,
                                void *state_payload,
                                const void *message_payload)
{
    (void)state_payload;
    const Test_Message *message = message_payload;
    if (message == NULL) return OM_STATUS_INVALID_ARGUMENT;
    if (message->command == COMMAND_WATCH_ONLY) {
        const Om_Status status = om_runtime_actor_watch(context,
            (Om_Runtime_Actor_Ref){message->actor_ref});
        if (status != OM_STATUS_OK) {
            atomic_store_explicit(&client_failed, 1, memory_order_release);
        }
        return status;
    }
    if (message->command == COMMAND_START) {
        Om_Runtime_Actor_Ref self = {0};
        Om_Status status = om_runtime_actor_self(context, &self);
        if (status == OM_STATUS_OK) {
            status = om_runtime_actor_watch(context,
                (Om_Runtime_Actor_Ref){message->actor_ref});
        }
        void *request = NULL;
        Om_Runtime_Root_Frame *root = NULL;
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_create(context, &MESSAGE_TYPE,
                                              &request);
        }
        if (status == OM_STATUS_OK) {
            root = om_runtime_roots_push(context, &request, 1);
            if (root == NULL) status = OM_STATUS_OUT_OF_MEMORY;
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(context, request, 0,
                (Om_Runtime_Value){.integer = COMMAND_REQUEST});
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(context, request, 1,
                (Om_Runtime_Value){.integer = message->number});
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(context, request, 2,
                (Om_Runtime_Value){.integer = (int64_t)self.id});
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_record_set(context, request, 3,
                (Om_Runtime_Value){.managed = message->text});
        }
        if (status == OM_STATUS_OK) {
            status = om_runtime_actor_send(context,
                (Om_Runtime_Actor_Ref){message->actor_ref}, request);
        }
        if (root != NULL) {
            const Om_Status pop_status = om_runtime_roots_pop(context, root);
            if (status == OM_STATUS_OK) status = pop_status;
        }
        if (status != OM_STATUS_OK) {
            atomic_store_explicit(&client_failed, 1, memory_order_release);
        }
        return status;
    }
    if (message->command == COMMAND_RESPONSE) {
        const bool valid = message->number == 43
            && text_equals(context, message->text, "hello from node A");
        if (!valid) {
            atomic_store_explicit(&client_failed, 1, memory_order_release);
            return OM_STATUS_INVALID_ARGUMENT;
        }
        atomic_store_explicit(&client_response_seen, 1, memory_order_release);
        return OM_STATUS_OK;
    }
    atomic_store_explicit(&client_failed, 1, memory_order_release);
    return OM_STATUS_INVALID_ARGUMENT;
}

static Om_Status actor_down_handler(Om_Execution_Context *context,
                                    void *state_payload,
                                    const void *message_payload)
{
    (void)context;
    (void)state_payload;
    const Actor_Down_Message *message = message_payload;
    if (message == NULL
        || message->reason != atomic_load_explicit(&expected_down_reason,
                                                    memory_order_acquire)) {
        atomic_store_explicit(&client_failed, 1, memory_order_release);
        return OM_STATUS_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&client_down_seen, 1, memory_order_release);
    return OM_STATUS_OK;
}

static const Om_Runtime_Actor_Handler_Descriptor WORKER_HANDLERS[] = {
    {.message_type = &MESSAGE_TYPE, .handler = worker_handler},
};
static const Om_Runtime_Actor_Handler_Descriptor CLIENT_HANDLERS[] = {
    {.message_type = &MESSAGE_TYPE, .handler = client_handler},
    {.message_type = &ACTOR_DOWN_TYPE, .handler = actor_down_handler},
};

static const Om_Runtime_Actor_Descriptor WORKER_ACTOR = {
    .fingerprint = UINT64_C(0x840001),
    .canonical_name = "Distribution.Worker",
    .state_type = &STATE_TYPE,
    .initialize_state = initialize_state,
    .handlers = WORKER_HANDLERS,
    .handler_count = sizeof(WORKER_HANDLERS) / sizeof(WORKER_HANDLERS[0]),
};
static const Om_Runtime_Actor_Descriptor CLIENT_ACTOR = {
    .fingerprint = UINT64_C(0x840002),
    .canonical_name = "Distribution.Client",
    .state_type = &STATE_TYPE,
    .initialize_state = initialize_state,
    .handlers = CLIENT_HANDLERS,
    .handler_count = sizeof(CLIENT_HANDLERS) / sizeof(CLIENT_HANDLERS[0]),
};
static const Om_Runtime_Type_Descriptor *const TYPES[] = {
    &INT_TYPE, &ACTOR_REF_TYPE, &STRING_TYPE, &MESSAGE_TYPE,
    &ACTOR_DOWN_TYPE, &STATE_TYPE,
};
static const Om_Runtime_Actor_Descriptor *const ACTORS[] = {
    &WORKER_ACTOR, &CLIENT_ACTOR,
};
static const Om_Runtime_Program_Descriptor PROGRAM = {
    .fingerprint = UINT64_C(0x850001),
    .interface_fingerprint = UINT64_C(0x850002),
    .types = TYPES,
    .type_count = sizeof(TYPES) / sizeof(TYPES[0]),
    .actors = ACTORS,
    .actor_count = sizeof(ACTORS) / sizeof(ACTORS[0]),
    .actor_down_message_type = &ACTOR_DOWN_TYPE,
    .actor_down_actor_id_offset = offsetof(Actor_Down_Message, actor_id),
    .actor_down_reason_offset = offsetof(Actor_Down_Message, reason),
    .collection_threshold = 1024,
};

static void sleep_milliseconds(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    const struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    (void)nanosleep(&duration, NULL);
#endif
}

static bool wait_for_flags(const atomic_int *first,
                           const atomic_int *second,
                           unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        if (atomic_load_explicit(&client_failed, memory_order_acquire) != 0) {
            return false;
        }
        const bool first_set = first == NULL
            || atomic_load_explicit(first, memory_order_acquire) != 0;
        const bool second_set = second == NULL
            || atomic_load_explicit(second, memory_order_acquire) != 0;
        if (first_set && second_set) return true;
        sleep_milliseconds(10);
    }
    return false;
}

static int run_server(void)
{
    int result = 1;
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Distribution distribution = {0};
    if (context == NULL) goto cleanup;
    if (!require_status(om_runtime_actor_system_start(context, &PROGRAM, 2),
                        "actor system start")) {
        goto cleanup;
    }
    if (!require_status(om_runtime_distribution_create(context, &PROGRAM,
            &distribution), "distribution create")) goto cleanup;
    uint16_t port = 0;
    if (!require_status(om_runtime_distribution_listen(&distribution,
        (Om_String_View){"127.0.0.1", sizeof("127.0.0.1") - 1}, 0, 4,
        &port), "distribution listen")) goto cleanup;
    (void)printf("PORT %u\n", (unsigned)port);
    (void)fflush(stdout);
    uint64_t node_ref = 0;
    if (!require_status(om_runtime_distribution_accept(&distribution,
            &node_ref), "distribution accept")) goto cleanup;
    if (!wait_for_flags(&worker_finished, NULL, 15000)) {
        (void)fprintf(stderr, "server timed out or actor failed: finished=%d failed=%d\n",
            atomic_load_explicit(&worker_finished, memory_order_acquire),
            atomic_load_explicit(&client_failed, memory_order_acquire));
        goto cleanup;
    }
    if (!require_status(om_runtime_actor_system_wait(context),
                        "server actor wait")) goto cleanup;
    sleep_milliseconds(500);
    result = 0;
cleanup:
    om_runtime_distribution_destroy(&distribution);
    om_runtime_context_destroy(context);
    return result;
}

static int run_unreachable_server(void)
{
    int result = 1;
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Distribution distribution = {0};
    if (context == NULL) goto cleanup;
    if (!require_status(om_runtime_actor_system_start(context, &PROGRAM, 2),
                        "actor system start")) {
        goto cleanup;
    }
    if (!require_status(om_runtime_distribution_create(context, &PROGRAM,
            &distribution), "distribution create")) goto cleanup;
    uint16_t port = 0;
    if (!require_status(om_runtime_distribution_listen(&distribution,
        (Om_String_View){"127.0.0.1", sizeof("127.0.0.1") - 1}, 0, 4,
        &port), "distribution listen")) goto cleanup;
    (void)printf("PORT %u\n", (unsigned)port);
    (void)fflush(stdout);
    uint64_t node_ref = 0;
    if (!require_status(om_runtime_distribution_accept(&distribution,
            &node_ref), "distribution accept")) goto cleanup;
    (void)node_ref;
    sleep_milliseconds(500);
    result = 0;
cleanup:
    om_runtime_distribution_destroy(&distribution);
    om_runtime_context_destroy(context);
    return result;
}

static int run_unreachable_client(uint16_t port)
{
    int result = 1;
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Distribution distribution = {0};
    if (context == NULL) goto cleanup;
    atomic_store_explicit(&client_down_seen, 0, memory_order_release);
    atomic_store_explicit(&client_failed, 0, memory_order_release);
    atomic_store_explicit(&expected_down_reason,
        OM_RUNTIME_ACTOR_DOWN_NODE_UNREACHABLE, memory_order_release);
    if (!require_status(om_runtime_actor_system_start(context, &PROGRAM, 2),
                        "actor system start")) {
        goto cleanup;
    }
    if (!require_status(om_runtime_distribution_create(context, &PROGRAM,
            &distribution), "distribution create")) goto cleanup;
    uint64_t node_ref = 0;
    if (!require_status(om_runtime_distribution_connect(&distribution,
        (Om_String_View){"127.0.0.1", sizeof("127.0.0.1") - 1}, port,
        &node_ref), "distribution connect")) goto cleanup;
    Om_Runtime_Actor_Ref remote_worker = {0};
    if (!require_status(om_runtime_actor_spawn_at(context, &WORKER_ACTOR,
            node_ref, &remote_worker), "remote spawn")) goto cleanup;
    Om_Runtime_Actor_Ref watcher = {0};
    if (!require_status(om_runtime_actor_spawn(context, &CLIENT_ACTOR,
            &watcher), "local watcher spawn")) goto cleanup;
    void *message = NULL;
    Om_Status status = om_runtime_record_create(context, &MESSAGE_TYPE,
                                                &message);
    Om_Runtime_Root_Frame *root = NULL;
    if (status == OM_STATUS_OK) {
        root = om_runtime_roots_push(context, &message, 1);
        if (root == NULL) status = OM_STATUS_OUT_OF_MEMORY;
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, message, 0,
            (Om_Runtime_Value){.integer = COMMAND_WATCH_ONLY});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, message, 2,
            (Om_Runtime_Value){.integer = (int64_t)remote_worker.id});
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_actor_send(context, watcher, message);
    }
    if (root != NULL) {
        const Om_Status pop_status = om_runtime_roots_pop(context, root);
        if (status == OM_STATUS_OK) status = pop_status;
    }
    if (!require_status(status, "send remote watch request")) goto cleanup;
    if (!wait_for_flags(&client_down_seen, NULL, 15000)) {
        (void)fprintf(stderr,
            "node unreachable wait timed out or Actor failed: down=%d failed=%d\n",
            atomic_load_explicit(&client_down_seen, memory_order_acquire),
            atomic_load_explicit(&client_failed, memory_order_acquire));
        goto cleanup;
    }
    if (!require_status(om_runtime_actor_system_wait(context),
                        "client actor wait")) goto cleanup;
    result = 0;
cleanup:
    om_runtime_distribution_destroy(&distribution);
    om_runtime_context_destroy(context);
    return result;
}

static int run_client(uint16_t port)
{
    int result = 1;
    Om_Execution_Context *context = om_runtime_context_create(0);
    Om_Runtime_Distribution distribution = {0};
    if (context == NULL) goto cleanup;
    if (!require_status(om_runtime_actor_system_start(context, &PROGRAM, 2),
                        "actor system start")) {
        goto cleanup;
    }
    if (!require_status(om_runtime_distribution_create(context, &PROGRAM,
            &distribution), "distribution create")) goto cleanup;
    uint64_t node_ref = 0;
    if (!require_status(om_runtime_distribution_connect(&distribution,
        (Om_String_View){"127.0.0.1", sizeof("127.0.0.1") - 1}, port,
        &node_ref), "distribution connect")) goto cleanup;
    Om_Runtime_Actor_Ref remote_worker = {0};
    if (!require_status(om_runtime_actor_spawn_at(context, &WORKER_ACTOR,
            node_ref, &remote_worker), "remote spawn")) goto cleanup;
    Om_Runtime_Actor_Ref client = {0};
    if (!require_status(om_runtime_actor_spawn(context, &CLIENT_ACTOR, &client),
                        "local spawn")) goto cleanup;

    void *roots[2] = {0};
    Om_Runtime_Root_Frame *root = om_runtime_roots_push(context, roots, 2);
    if (root == NULL) goto cleanup;
    Om_Status status = om_runtime_string_create_utf8_default(context,
        (const uint8_t *)"hello from node A", sizeof("hello from node A") - 1,
        &roots[0]);
    if (status != OM_STATUS_OK) (void)fprintf(stderr, "create string: %s\n", om_status_name(status));
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_create(context, &MESSAGE_TYPE, &roots[1]);
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "create start record: %s\n", om_status_name(status));
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, roots[1], 0,
            (Om_Runtime_Value){.integer = COMMAND_START});
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "set start kind: %s\n", om_status_name(status));
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, roots[1], 1,
            (Om_Runtime_Value){.integer = 42});
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "set start number: %s\n", om_status_name(status));
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, roots[1], 2,
            (Om_Runtime_Value){.integer = (int64_t)remote_worker.id});
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "set start ActorRef: %s\n", om_status_name(status));
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_record_set(context, roots[1], 3,
            (Om_Runtime_Value){.managed = roots[0]});
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "set start text: %s\n", om_status_name(status));
    }
    if (status == OM_STATUS_OK) {
        status = om_runtime_actor_send(context, client, roots[1]);
        if (status != OM_STATUS_OK) (void)fprintf(stderr, "send start: %s\n", om_status_name(status));
    }
    const Om_Status pop_status = om_runtime_roots_pop(context, root);
    if (status == OM_STATUS_OK) status = pop_status;
    if (!require_status(status, "create and send start message")) goto cleanup;
    if (!wait_for_flags(&client_response_seen, &client_down_seen, 15000)) {
        (void)fprintf(stderr, "client wait timed out or actor failed: response=%d down=%d failed=%d\n",
            atomic_load_explicit(&client_response_seen, memory_order_acquire),
            atomic_load_explicit(&client_down_seen, memory_order_acquire),
            atomic_load_explicit(&client_failed, memory_order_acquire));
        goto cleanup;
    }
    if (!require_status(om_runtime_actor_system_wait(context),
                        "client actor wait")) goto cleanup;
    result = 0;
cleanup:
    om_runtime_distribution_destroy(&distribution);
    om_runtime_context_destroy(context);
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "server") == 0) return run_server();
    if (argc == 2 && strcmp(argv[1], "unreachable-server") == 0) {
        return run_unreachable_server();
    }
    if (argc == 3 && strcmp(argv[1], "client") == 0) {
        char *end = NULL;
        const unsigned long parsed = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed == 0 || parsed > 65535) {
            return 2;
        }
        return run_client((uint16_t)parsed);
    }
    if (argc == 3 && strcmp(argv[1], "unreachable-client") == 0) {
        char *end = NULL;
        const unsigned long parsed = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed == 0 || parsed > 65535) {
            return 2;
        }
        return run_unreachable_client((uint16_t)parsed);
    }
    (void)fprintf(stderr,
        "usage: %s server | client <port> | unreachable-server | "
        "unreachable-client <port>\n", argv[0]);
    return 2;
}
