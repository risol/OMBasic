#ifndef OMBASIC_RUNTIME_ACTOR_INTERNAL_H
#define OMBASIC_RUNTIME_ACTOR_INTERNAL_H

#include "runtime_internal.h"

#include "blocking_pool.h"
#include "timer_service.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define OM_RUNTIME_SYSTEM_QUEUE_CAPACITY ((size_t)1024)
#define OM_RUNTIME_REMOTE_ACTOR_ID_BIT (UINT64_C(1) << 63)

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

struct Om_Runtime_Network_Resource;
struct Om_Runtime_Process_Resource;

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
    uint64_t next_console_read_sequence;
    uint64_t serving_console_read_sequence;
    uint64_t next_logger_sequence;
    uint64_t serving_logger_sequence;
    size_t logger_pending_bytes;
    struct Om_Runtime_Network_Resource *network_resources;
    struct Om_Runtime_Process_Resource *process_resources;
    uint64_t next_network_resource_id;
    uint64_t next_process_resource_id;
    size_t active_network_resources;
    size_t pending_network_resources;
    size_t active_process_resources;
    size_t pending_process_resources;
    bool network_initialized;
    size_t running_handlers;
    bool stopping;
    bool shutting_down;
    bool joined;
};

typedef struct Om_Runtime_System_Text_Item {
    char *bytes;
    size_t length;
} Om_Runtime_System_Text_Item;

typedef struct Om_Runtime_System_Task {
    Om_Runtime_Actor_System *system;
    Om_System_Message_Id operation;
    uint64_t recipient_id;
    uint64_t sequence;
    int64_t requested_length;
    char *path;
    char *working_directory;
    uint8_t *input_bytes;
    size_t input_length;
    bool input_oversized;
    uint8_t *result_bytes;
    size_t result_length;
    size_t result_capacity;
    uint8_t console_pending_bytes[4];
    size_t console_pending_count;
    size_t console_pending_index;
    uint16_t console_pending_high_surrogate;
    bool console_input;
    const char *error_code;
    bool result_boolean;
    int64_t result_integer;
    Om_Runtime_System_Text_Item *result_items;
    size_t result_item_count;
    size_t result_item_capacity;
    size_t result_item_bytes;
    Om_Execution_Context *payload_context;
    void *payload_object;
    struct Om_Runtime_Network_Resource *network_resource;
    struct Om_Runtime_Network_Resource *network_listener;
    struct Om_Runtime_Process_Resource *process_resource;
    char **process_arguments;
    size_t process_argument_count;
    char **process_environment;
    size_t process_environment_count;
    bool process_replace_environment;
    bool process_resource_is_new;
    bool process_resource_registered;
    bool network_resource_is_new;
    bool network_resource_registered;
    int network_backlog;
} Om_Runtime_System_Task;

void actor_mutex_lock(Om_Runtime_Actor_System *system);
void actor_mutex_unlock(Om_Runtime_Actor_System *system);
void actor_condition_wait(Om_Runtime_Actor_System *system);
void actor_condition_signal(Om_Runtime_Actor_System *system);
void actor_condition_broadcast(Om_Runtime_Actor_System *system);
bool actor_sync_init(Om_Runtime_Actor_System *system);
void actor_sync_destroy(Om_Runtime_Actor_System *system);
Om_Runtime_Actor *actor_find_locked(Om_Runtime_Actor_System *system,
                                   Om_Runtime_Actor_Ref reference);
const Om_Runtime_Actor_Handler_Descriptor *actor_handler_for(
    const Om_Runtime_Actor_Descriptor *actor,
    const Om_Runtime_Type_Descriptor *message_type);
Om_Status actor_enqueue_object_locked(Om_Runtime_Actor_System *system,
                                     Om_Runtime_Actor *actor,
                                     void *message_object);
Om_Status actor_schedule_locked(Om_Runtime_Actor_System *system,
                                Om_Runtime_Actor *actor);
Om_Status actor_discard_mailbox_locked(Om_Runtime_Actor_System *system,
                                       Om_Runtime_Actor *actor);
Om_Runtime_Actor_Message *actor_pop_message_locked(
    Om_Runtime_Actor *actor);
Om_Status actor_notify_watchers_locked(Om_Runtime_Actor_System *system,
                                      Om_Runtime_Actor *actor);
void actor_fail_locked(Om_Runtime_Actor_System *system, uint64_t actor_id);
Om_Status actor_mailbox_root_provider(
    Om_Execution_Context *context,
    Om_Runtime_Trace_Visitor visitor,
    void *visitor_context);
Om_Status actor_send_console_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_file_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_timer_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_clock_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_secure_random_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_logger_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_network_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
Om_Status actor_send_process_locked(
    Om_Runtime_Actor_System *system,
    Om_Execution_Context *source_context,
    const Om_Runtime_Type_Descriptor *message_type,
    const void *message_object);
void actor_network_shutdown_locked(Om_Runtime_Actor_System *system);
void actor_network_destroy(Om_Runtime_Actor_System *system);
void actor_process_shutdown_locked(Om_Runtime_Actor_System *system);
void actor_process_destroy(Om_Runtime_Actor_System *system);

#endif
